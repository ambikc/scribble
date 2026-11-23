// main.cpp - the GAME entry point (the day-one hardware check lives in ../hwcheck/main.cpp, env:hwcheck).
//
// setup(): M5.begin, the file system, then create and wire Renderer / UI / Audio / LevelStore / Input / Game.
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

using namespace scribble;

static Renderer renderer;
static UI ui;
static Audio audio;
static LevelStore store;
static Input input;
static Game game(renderer, ui, audio, store, input);

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
  for (;;) { M5.update(); delay(100); }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("Scribble Run");

#ifdef EMU
  const char* root = emu::fsRoot();
  virtualClock = emu::scriptActive();
#else
  const char* root = FS_ROOT;
  if (!psramFound()) fatal("No PSRAM found. Check the build flags (BOARD_HAS_PSRAM) and the partition table.");
  Serial.printf("PSRAM %u KB, free %u KB\n", unsigned(ESP.getPsramSize() / 1024), unsigned(ESP.getFreePsram() / 1024));
  // the first mount formats the partition (a few seconds, once); the levels and settings live under /littlefs
  if (!LittleFS.begin(true, FS_ROOT)) Serial.println("LittleFS mount failed: levels will not be saved this session");
#endif
  Serial.printf("storage root: %s\n", root);

  if (!renderer.begin()) fatal("Could not allocate the screen buffers (PSRAM).");
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
  M5.update();                                    // once per pass: touch, buttons
  const uint32_t now = millis();
  input.sample(now);                              // latch tap edges even on a pass that runs no tick

  int ticks = 0;
  if (virtualClock) {
    game.tick(input.poll(now));                   // script mode: one pass = one tick, deterministic
    input.consumeEdges();
    ticks = 1;
  } else {
    acc += std::fmin(double(now - lastMs) / 1000.0, MAX_FRAME_S);
    lastMs = now;
    while (acc >= DT && ticks < MAX_TICKS_PER_PASS) {
      game.tick(input.poll(now));                 // tilt filtered once per tick; touch edges latched
      input.consumeEdges();                       // each tap is seen by exactly one tick
      acc -= DT; ++ticks;
    }
  }

  audio.update(now);
  game.draw();
  renderer.present();
  if (ticks == 0) delay(1);                       // faster than 60 Hz: let the idle task and USB run
}
