// main.cpp - the GAME entry point (the day-one hardware check lives in ../hwcheck/main.cpp, env:hwcheck).
//
// setup(): the I2C lock, M5.begin, the screen ("Getting ready..."), the file system (formatting only when the first mount fails,
//          with "First start: preparing storage" on the screen), then wire Renderer / UI / Audio / LevelStore / Input / Game.
// loop():  the fixed 60 Hz accumulator of docs/system-design.md 6.1 (same as sim/src/main.ts):
//            accumulate real time (clamped to 0.1 s), run at most 6 ticks, draw + present once per pass.
//          Physics always advances in 1/60 s steps whatever the display rate.
// Emulator (#ifdef EMU): the "file system" is a plain folder (env EMU_FS, default ./emu_fs) and, when a script runs,
//          time is virtual and ONE pass = ONE tick, so a run is deterministic (emu/README.md).
#include <M5Unified.h>
#include <cmath>
#ifndef EMU
#include <LittleFS.h>
#endif
#include "app/game.h"
#include "app/camera.h"
#include "app/i2c_lock.h"

#ifndef SCRIBBLE_PERF
#define SCRIBBLE_PERF 0                           // 1 (env:cores3_perf): print [perf tick X us draw Y us present Z us passes P ticks T] each second
#endif

using namespace scribble;

static Renderer renderer;
static UI ui;
static Audio audio;
static LevelStore store;
static Input input;
static Camera camera;                            // paper mode: off until a paper screen asks for it (app/camera.h)
static Game game(renderer, ui, audio, store, input, &camera);

#ifndef EMU
static const char* const FS_ROOT = "/littlefs";  // LittleFS mount point on the device
#endif

constexpr double DT = 1.0 / PHYSICS_HZ;
constexpr double MAX_FRAME_S = 0.1;              // clamp: no spiral of death after a stall
constexpr int MAX_TICKS_PER_PASS = 6;            // catch up, but never more than 6 ticks

static double acc = 0;
static uint32_t lastMs = 0;
static bool virtualClock = false;

/** A fatal start-up problem: say so on the screen (and serial) and stop, instead of crashing in the first draw. */
[[noreturn]] static void fatal(const char* what) {
  Serial.printf("FATAL: %s\n", what);
  M5.Display.fillScreen(PAL_RED);
  M5.Display.setTextColor(PAL_WHITE, PAL_RED);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 8);
  M5.Display.print(what);
  for (;;) { { I2cGuard bus; M5.update(); } delay(100); }
}

/** A start-up message on the paper, with the ball (drawn and pushed at once: nothing else is on the screen yet). */
static void bootScreen(const char* line1, const char* line2 = nullptr) {
  renderer.clearPaper();
  renderer.ball(160, 92, 0);
  renderer.text(line1, 160, 140, 22, PAL_INK);
  if (line2) renderer.text(line2, 160, 172, 14, PAL_INK);
  renderer.present();
}

#if SCRIBBLE_PERF
// [perf ...] once a second (env:cores3_perf, or -DSCRIBBLE_PERF=1): average microseconds per call of game.tick, game.draw and
// renderer.present, and how many loop passes and ticks ran in that second (docs/day-one-runbook.md, part 1).
static struct { uint64_t tickUs = 0, drawUs = 0, presentUs = 0; uint32_t passes = 0, ticks = 0, since = 0; } perf;
#define PERF_TIME(field, call) do { const uint32_t t0_ = micros(); call; perf.field += micros() - t0_; } while (0)
static void perfReport(uint32_t now) {
  ++perf.passes;
  if (now - perf.since < 1000) return;
  const uint32_t p = perf.passes ? perf.passes : 1, t = perf.ticks ? perf.ticks : 1;
  Serial.printf("[perf tick %u us draw %u us present %u us passes %u ticks %u]\n", unsigned(perf.tickUs / t),
                unsigned(perf.drawUs / p), unsigned(perf.presentUs / p), unsigned(perf.passes), unsigned(perf.ticks));
  perf.tickUs = perf.drawUs = perf.presentUs = 0; perf.passes = perf.ticks = 0; perf.since = now;
}
#else
#define PERF_TIME(field, call) call
#endif

void setup() {
  i2cLockInit();                                  // first: the I2C lock must exist before any other task (camera) can run
  auto cfg = M5.config();
  {
    I2cGuard bus;
    M5.begin(cfg);
  }
  Serial.begin(115200);
#ifndef EMU
  Serial.setTxTimeoutMs(0);                       // USB serial with no host reading: drop output instead of stalling the loop
#endif
  Serial.println("Scribble Run");
#if SCRIBBLE_PERF
  Serial.println("SCRIBBLE_PERF: one [perf ...] line per second");
#endif

#ifdef EMU
  const char* root = emu::fsRoot();
  virtualClock = emu::scriptActive();
#else
  const char* root = FS_ROOT;
  if (!psramFound()) fatal("No PSRAM found. Check the build flags (BOARD_HAS_PSRAM) and the partition table.");
  Serial.printf("PSRAM %u KB, free %u KB\n", unsigned(ESP.getPsramSize() / 1024), unsigned(ESP.getFreePsram() / 1024));
#endif

  // the screen first, so a parent never looks at a black screen while the storage is prepared
  if (!renderer.begin()) fatal("Could not allocate the screen buffers (PSRAM).");
  bootScreen("Getting ready...");

#ifndef EMU
  // Mount without formatting first. Only a failed mount (the very first start, or a damaged partition) formats the ~9.9 MB
  // partition, which can take tens of seconds: say so on the screen, so nobody switches the device off in the middle.
  if (!LittleFS.begin(false, FS_ROOT)) {
    Serial.println("LittleFS: no file system yet, formatting (first start)");
    bootScreen("Getting ready...", "First start: preparing storage");
    if (!LittleFS.begin(true, FS_ROOT)) Serial.println("LittleFS mount failed: levels will not be saved this session");
  }
#endif
  Serial.printf("storage root: %s\n", root);

  if (!store.begin(root)) Serial.println("level store: the storage folder is not usable, running without saving");
  audio.clipRoot = root;
  audio.volume = store.settings.sound;
  audio.begin();
  input.begin();
  game.begin();

  lastMs = millis();
  acc = 0;
}

void loop() {
  {
    I2cGuard bus;                                 // touch, IMU and power chip share the internal I2C bus with the camera task
    M5.update();                                  // once per pass: touch, buttons
  }
  const uint32_t now = millis();
  input.sample(now);                              // latch tap edges even on a pass that runs no tick

  int ticks = 0;
  if (virtualClock) {
    PERF_TIME(tickUs, game.tick(input.poll(now)));   // script mode: one pass = one tick, deterministic
    input.consumeEdges();
    ticks = 1;
  } else {
    acc += std::fmin(double(now - lastMs) / 1000.0, MAX_FRAME_S);
    lastMs = now;
    while (acc >= DT && ticks < MAX_TICKS_PER_PASS) {
      PERF_TIME(tickUs, game.tick(input.poll(now)));   // tilt filtered once per tick; touch edges latched
      input.consumeEdges();                       // each tap is seen by exactly one tick
      acc -= DT; ++ticks;
    }
  }
#if SCRIBBLE_PERF
  perf.ticks += uint32_t(ticks);
#endif

  audio.update(now);
  camera.update(now);                             // switches the camera off when no paper screen has asked for it lately
  PERF_TIME(drawUs, game.draw());
  PERF_TIME(presentUs, renderer.present());
#if SCRIBBLE_PERF
  perfReport(now);
#endif
  if (ticks == 0) delay(1);                       // faster than 60 Hz: let the idle task and USB run
}
