// game_paper.cpp - the five paper screens: stand-up guide, camera check, countdown, processing, review.
// Port of tickStandup..acceptReview / drawStandup..drawReview of sim/src/modes.ts onto the device layout (docs/screens.md section 4 and 5).
// OWNER: Paper agent.
//
// Threads (docs/system-design.md 2.3): the game loop never waits for the camera or the vision code.
//   * camera task (camera.cpp, core 0)  grabs frames, publishes the newest one, averages three of them at the snap
//   * vision task (here, core 0)        runs visionLiveCheck (~8 Hz in Check), visionProcess (Processing) and visionRethreshold (Review)
//   The screens start a job, keep animating, and poll for its end. The emulator runs the jobs inline (one script tick = one loop pass, so
//   a run stays deterministic); the code paths and state machine are the same.
#include "game.h"
#include "camera.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef EMU
#include <chrono>
#else
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace scribble {

namespace {
constexpr float DT = 1.0f / PHYSICS_HZ;
constexpr int BAR_Y = SCREEN_H - BAR_H;                   // 192: the bottom bar
constexpr uint16_t BACKDROP = 0x39C9;                     // #3A3A48: behind the dimmed camera pictures (countdown, processing)
constexpr float PI_F = 3.14159265f;
constexpr float MIN_PROCESSING_S = 0.6f;                  // the sparkle is always seen for a moment, even when the vision code is quick
constexpr float STANDUP_GATE_S = 8.0f;                    // "stand me up" waits this long for the IMU, then Ready works anyway (never a dead end)
constexpr int LIVE_CHECK_MS = 125;                        // the live check runs about 8 times a second

uint64_t nowUs() {
#ifdef EMU
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
  return uint64_t(esp_timer_get_time());
#endif
}

void* bigAlloc(size_t n) {
#ifdef EMU
  return std::malloc(n);
#else
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

/** The three big buffers (770 KB + 614 KB + 77 KB), once. PSRAM on the device. */
bool ensureBuffers(PaperState& s) {
  if (s.work && s.snap && s.gray && s.res && s.tmp) return true;
  if (s.allocFailed) return false;
  s.work = static_cast<VisionWork*>(bigAlloc(sizeof(VisionWork)));
  s.snap = static_cast<uint16_t*>(bigAlloc(CAMERA_FRAME_BYTES));
  s.gray = static_cast<uint8_t*>(bigAlloc(size_t(QVGA_W) * QVGA_H));
  s.res = static_cast<Level*>(bigAlloc(sizeof(Level)));
  s.tmp = static_cast<Level*>(bigAlloc(sizeof(Level)));
  if (!s.work || !s.snap || !s.gray || !s.res || !s.tmp) {
    std::free(s.work); std::free(s.snap); std::free(s.gray); std::free(s.res); std::free(s.tmp);
    s.work = nullptr; s.snap = nullptr; s.gray = nullptr; s.res = s.tmp = nullptr; s.allocFailed = true;
    Serial.println("[paper] not enough PSRAM for the vision buffers");
    return false;
  }
  std::memset(s.work, 0, sizeof(VisionWork));
  std::memset(s.res, 0, sizeof(Level)); std::memset(s.tmp, 0, sizeof(Level));      // zero on purpose: the golden CRC covers the whole struct
  Serial.printf("[paper buffers: VisionWork %u bytes, photo %u bytes, preview gray %u bytes, all in PSRAM]\n",
                unsigned(sizeof(VisionWork)), unsigned(CAMERA_FRAME_BYTES), unsigned(QVGA_W * QVGA_H));
  return true;
}

// =====================================================================================================================
// the vision worker: one job at a time on core 0 (inline in the emulator)
// =====================================================================================================================
enum class Job : uint8_t { None, Live, Process, Rethreshold };
struct Worker {
  std::atomic<int> state{0};                              // 0 idle, 1 running, 2 finished (result waiting)
  Job job = Job::None;
  // inputs (set before start)
  const uint8_t* gray = nullptr; bool upright = true;
  const uint16_t* vga = nullptr;
  VisionParams params{}; VisionWork* work = nullptr; Level* level = nullptr; VisionReport* report = nullptr;
  // outputs
  LiveCheck live{}; VisionParams used{}; uint32_t ms = 0;
#ifndef EMU
  TaskHandle_t task = nullptr;
#endif

  void run() {
    const uint64_t t0 = nowUs();
    switch (job) {
      case Job::Live: visionLiveCheck(gray, upright, *work, live); break;
      case Job::Process: used = params; visionProcess(vga, params, *work, *level, *report, &used); break;
      case Job::Rethreshold: visionRethreshold(*work, params, *level, *report); break;
      case Job::None: break;
    }
    ms = uint32_t((nowUs() - t0 + 500) / 1000);
    state.store(2, std::memory_order_release);
  }
#ifndef EMU
  static void trampoline(void* self) {
    Worker* w = static_cast<Worker*>(self);
    for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); w->run(); }
  }
#endif
  /** True if a new job may be started (a finished result nobody wanted any more is dropped). */
  bool available() { if (state.load() == 2) state.store(0); return state.load() == 0; }
  void start(Job j) {
    job = j; state.store(1, std::memory_order_release);
#ifdef EMU
    run();
#else
    if (!task) xTaskCreatePinnedToCore(trampoline, "vision", 16384, this, 1, &task, 0);   // created on first use; priority 1 on core 0, the game loop is on core 1
    if (!task) { state.store(0); return; }
    xTaskNotifyGive(task);
#endif
  }
  /** True once, when the job of kind `j` has finished. */
  bool take(Job j) {
    if (state.load(std::memory_order_acquire) != 2 || job != j) return false;
    state.store(0); return true;
  }
};
Worker W;

#ifdef EMU
uint32_t crcOfLevel(const Level& l) {
  static uint8_t buf[MAX_FILE_BYTES];
  const size_t n = encodeLevel(l, false, buf, sizeof buf);
  return n ? crc32(buf, 0, n) : 0;
}
#endif

// ---- small drawing helpers ------------------------------------------------------------------------------------------
void dashedLine(Renderer& r, float x0, float y0, float x1, float y1, float w, uint16_t color, float on = 9, float off = 6) {
  const float len = std::hypot(x1 - x0, y1 - y0), ux = (x1 - x0) / len, uy = (y1 - y0) / len;
  for (float d = 0; d < len; d += on + off) { const float e = std::fmin(d + on, len); r.line(x0 + ux * d, y0 + uy * d, x0 + ux * e, y0 + uy * e, w, color); }
}
void dashedRect(Renderer& r, int x, int y, int w, int h, float lw, uint16_t color) {
  dashedLine(r, float(x), float(y), float(x + w), float(y), lw, color);
  dashedLine(r, float(x + w), float(y), float(x + w), float(y + h), lw, color);
  dashedLine(r, float(x + w), float(y + h), float(x), float(y + h), lw, color);
  dashedLine(r, float(x), float(y + h), float(x), float(y), lw, color);
}

/** One of the four status lights of the Check bottom bar (display only): icon above, a lamp below. `fill` carries the lamp colour. */
void drawLight(Renderer& r, const Btn& b, float, void*) {
  r.g().fillRect(b.x, b.y, 44, BAR_H, PAL_WHITE);
  r.g().fillRect(b.x, b.y, 44, 2, PAL_INK);
  r.g().fillRect(b.x + 42, b.y, 2, BAR_H, PAL_INK);
  drawIcon(r, b.icon, b.x + 22, b.y + 18, 26);
  r.roundRect(b.x + 12, b.y + 34, 20, 9, 4, uint16_t(b.fill), PAL_INK, 2);
}

/** The Standup bar while the device still lies flat: a dotted tab asking to stand it up. */
void drawStandMeUp(Renderer& r, const Btn& b, float, void*) {
  r.g().fillRect(b.x, b.y, b.w, b.h, PAL_PAPER_DOT);
  r.g().fillRect(b.x, b.y, b.w, 2, PAL_INK);
  drawIcon(r, Icon::Up, b.x + 100, b.y + 26, 26, PAL_GREY);
  r.text("STAND ME UP", b.x + 190, b.y + 26, 17, PAL_GREY);
}

const uint16_t LAMP[4] = {PAL_GREY, PAL_RED, PAL_YELLOW, PAL_GREEN};     // LIGHT_GREY, RED, YELLOW, GREEN
}  // namespace

// =====================================================================================================================
// 4.1 stand-up guide
// =====================================================================================================================
void Game::tickStandup(const InputState& in) {
  topBar(Icon::None);
  const bool waiting = !in.upright && screenT < STANDUP_GATE_S;       // lying flat: Ready waits for the device to be stood up (then lets go anyway)
  Btn ready; ready.id = "ready"; ready.x = 0; ready.y = BAR_Y; ready.w = 320; ready.h = BAR_H;
  if (waiting) { ready.kind = BtnKind::Custom; ready.disabled = true; ready.draw = drawStandMeUp; }
  else { ready.kind = BtnKind::Tab; ready.icon = Icon::Check; ready.fill = PAL_GREEN; ready.iconSize = 38; ready.pulse = in.upright; }
  ui.add(ready);
  const char* hit = ui.update(in);
  if (in.upright && !paper.wasUpright && screenT > 0.1f) audio.play(Sfx::Tick);   // it got stood up: a small acknowledgement
  paper.wasUpright = in.upright;
  if (!hit) return;
  if (!std::strcmp(hit, "back")) leaveCamera();
  else if (!std::strcmp(hit, "ready")) { store.settings.standupSeen = true; store.saveSettings(); go(Screen::Check); }
}

void Game::drawStandup() {
  r.clear(PAL_PAPER);
  const bool flat = !paper.wasUpright && screenT < STANDUP_GATE_S;
  auto panel = [&](int x, int n) {
    r.roundRect(x, 58, 98, 126, 10, PAL_WHITE, PAL_INK, 2);
    r.disc(float(x + 17), 73, 11, PAL_INK);
    char d[2] = {char('0' + n), 0}; r.text(d, x + 17, 74, 14, PAL_WHITE);
  };
  panel(8, 1); panel(111, 2); panel(214, 3);
  // 1: the device standing on its base (the yellow dot is the camera)
  if (flat) { const float k = 0.5f + 0.5f * std::sin(t * 5.0f); r.ring(55, 114, 42, 3, PAL_YELLOW, 0, 6.2831853f, uint8_t(60 + 140 * k)); }
  r.roundRect(33, 86, 44, 52, 6, PAL_INK, -1);
  r.g().fillRect(38, 91, 34, 34, PAL_PAPER);
  r.disc(55, 131, 3, PAL_YELLOW); r.ring(55, 131, 3, 1, PAL_WHITE);
  const float base[8] = {26, 142, 84, 142, 78, 154, 32, 154};
  r.polygon(base, 4, PAL_GREY); r.polyline(base, 4, 2.5f, PAL_INK, true);
  // 2: the paper faces it, 40 cm away
  r.roundRect(115, 100, 20, 26, 3, PAL_INK, -1); r.g().fillRect(118, 103, 14, 14, PAL_PAPER);
  r.roundRect(179, 90, 22, 48, 2, PAL_WHITE, PAL_INK, 2);
  r.g().fillRect(184, 112, 10, 3, PAL_INK);
  r.disc(184, 96, 2, PAL_INK); r.disc(196, 96, 2, PAL_INK); r.disc(196, 132, 2, PAL_INK); r.disc(184, 132, 2, PAL_INK);
  r.line(139, 160, 175, 160, 2, PAL_INK); r.line(139, 156, 139, 164, 2, PAL_INK); r.line(175, 156, 175, 164, 2, PAL_INK);
  r.text("40 cm", 157, 148, 14);
  // 3: tap GO, then 3 2 1
  r.text("3", 234, 108, 30, PAL_INK); r.text("2", 263, 108, 30, PAL_RED); r.text("1", 290, 108, 30, PAL_BLUE);
  r.roundRect(236, 140, 50, 28, 8, PAL_GREEN, PAL_INK, 2);
  drawIcon(r, Icon::Play, 261, 154, 18);
  ui.draw(r, nowMs);
}

// =====================================================================================================================
// 4.2 camera check
// =====================================================================================================================
void Game::tickCheck(const InputState& in) {
  if (!cam || !ensureBuffers(paper)) { say(cam ? "Not enough memory for the camera" : "No camera in this build"); leaveCamera(); return; }
  if (screenT < 0.02f) { paper.lcValid = false; paper.greenFor = 0; paper.lcFrame = 0; paper.lcAt = 0; paper.debugNums = false; }
  cam->begin();                                                       // the lease: every tick of a paper screen renews it

  chipBack();
  Btn help; help.longMs = 2000; help.longId = "numbers";
  chip("help", Icon::Help, 276, &help);
  static const Icon icons[4] = {Icon::Ruler, Icon::Focus, Icon::Bulb, Icon::Level};
  for (int i = 0; i < 4; i++) {
    static const char* ids[4] = {"light0", "light1", "light2", "light3"};
    Btn b; b.id = ids[i]; b.x = i * 44; b.y = BAR_Y; b.w = 44; b.h = BAR_H; b.kind = BtnKind::Custom; b.disabled = true;
    b.icon = icons[i]; b.fill = LAMP[paper.lcValid ? paper.lc.lights[i] & 3 : 0]; b.draw = drawLight;
    ui.add(b);
  }
  const bool ready = paper.greenFor >= float(GREEN_HOLD_S);
  Btn goBtn; goBtn.id = "go"; goBtn.x = 176; goBtn.y = BAR_Y; goBtn.w = 144; goBtn.h = BAR_H; goBtn.kind = BtnKind::Tab; goBtn.icon = Icon::Play;
  goBtn.fill = ready ? PAL_GREEN : PAL_YELLOW; goBtn.iconSize = 38; goBtn.pulse = ready;
  ui.add(goBtn);
  const char* hit = ui.update(in);

  // the live check: about 8 Hz, only when the camera has a new frame, on the vision task
  if (W.take(Job::Live)) {
    paper.lc = W.live; paper.lcValid = true;
#ifdef EMU
    static int lastCode = -1;                                         // a line whenever the four lights change, so scripts can check "the lights turn green over time"
    const int code = paper.lc.marks ? paper.lc.lights[0] * 1000 + paper.lc.lights[1] * 100 + paper.lc.lights[2] * 10 + paper.lc.lights[3] + (paper.lc.allGreen ? 10000 : 0) : -2;
    if (code != lastCode) { lastCode = code; Serial.printf("[paper lights %s %d %d %d %d allGreen=%d @ %u ms]\n", paper.lc.marks ? "marks" : "nomarks", paper.lc.lights[0], paper.lc.lights[1], paper.lc.lights[2], paper.lc.lights[3], paper.lc.allGreen ? 1 : 0, unsigned(nowMs)); }
#endif
  }
  if (!cam->running()) paper.lcValid = false;
  else if (nowMs - paper.lcAt >= uint32_t(LIVE_CHECK_MS) && W.available() && cam->liveGray(paper.gray, paper.lcFrame)) {
    W.gray = paper.gray; W.upright = in.upright; W.work = paper.work;
    paper.lcAt = nowMs;
    W.start(Job::Live);
  }
  paper.greenFor = paper.lcValid && paper.lc.allGreen ? paper.greenFor + DT : 0.0f;

  if (!hit) return;
  if (!std::strcmp(hit, "back")) { cam->end(); leaveCamera(); }
  else if (!std::strcmp(hit, "help")) go(Screen::Standup);
  else if (!std::strcmp(hit, "numbers")) paper.debugNums = !paper.debugNums;
  else if (!std::strcmp(hit, "go")) {
    if (!cam->running() || cam->frameNo() == 0) { say(cam->failed() ? "No camera found" : "The camera is still waking up"); return; }
    paper.cdT = 0; for (bool& s : paper.said) s = false;
    paper.snapAsked = paper.snapped = false; paper.haveSnap = false;
    cam->lockExposure(true);                                           // so the snap looks like the check
    go(Screen::Countdown);
  }
}

namespace {
/** The yellow "closer / back / tilt" cue on the preview. */
void drawCue(Renderer& r, int cue) {
  if (cue == CUE_TILT) {
    const float arc[10] = {142, 176, 151, 182, 160, 184, 169, 182, 178, 176};
    r.polyline(arc, 5, 3.5f, PAL_YELLOW);
    const float head[6] = {172, 171, 178, 176, 171, 181};
    r.polyline(head, 3, 3.5f, PAL_YELLOW);
  } else if (cue == CUE_CLOSER || cue == CUE_BACK) {
    for (int side = 0; side < 2; side++) {                              // "closer": arrows point outwards (the page must grow); "back": inwards
      const float x = side == 0 ? 80.0f : 240.0f;
      const float s = ((cue == CUE_CLOSER) == (side == 1)) ? 1.0f : -1.0f;
      const float head[6] = {x - 8 * s, 88, x, 96, x - 8 * s, 104};
      r.polyline(head, 3, 3.5f, PAL_YELLOW);
      r.line(x - 18 * s, 96, x, 96, 3.5f, PAL_YELLOW);
    }
  }
}
}  // namespace

void Game::drawCheck() {
  r.clear(PAL_PAPER_DOT);
  const uint16_t* frame = cam ? cam->live() : nullptr;
  if (frame) {
    r.frame565(frame, 32, 0, 256, 192);
    cam->release();
    // the target frame: where the four marks should land (about 40 cm)
    const float box[10] = {58, 35, 262, 35, 262, 157, 58, 157, 58, 35};
    r.polyline(box, 5, 4.5f, PAL_INK, false, 140);
    dashedRect(r, 58, 35, 204, 122, 2.5f, PAL_WHITE);
    const LiveCheck& lc = paper.lc;
    if (paper.lcValid && lc.marks) {
      float q[8];
      for (int i = 0; i < 4; i++) { q[2 * i] = 32 + float(lc.quad[i].x) * 0.8f; q[2 * i + 1] = float(lc.quad[i].y) * 0.8f; }
      r.polyline(q, 4, 2.5f, PAL_GREEN, true);
      for (int i = 0; i < 4; i++) r.ring(q[2 * i], q[2 * i + 1], 12, 2.5f, PAL_GREEN);
      if (lc.cue) drawCue(r, lc.cue);
    } else r.text("?", 160, 96, 40, PAL_WHITE, Align::Center, PAL_INK);
    if (paper.debugNums && paper.lcValid && lc.marks) {
      r.roundRectA(36, 52, 130, 74, 8, PAL_WHITE, 230, PAL_INK, 2);
      char line[48];
      snprintf(line, sizeof line, "dist %.1f cm", lc.distCm); r.text(line, 42, 63, 10, PAL_INK, Align::Left);
      snprintf(line, sizeof line, "edge %.1f px", lc.edgePx); r.text(line, 42, 76, 10, PAL_INK, Align::Left);
      snprintf(line, sizeof line, "paper %.0f  clip %.0f%%", lc.paperMean, lc.clipFrac * 100); r.text(line, 42, 89, 10, PAL_INK, Align::Left);
      snprintf(line, sizeof line, "contrast %.0f", lc.contrast); r.text(line, 42, 102, 10, PAL_INK, Align::Left);
      snprintf(line, sizeof line, "ratio %.2f / %.2f", lc.ratioH, lc.ratioV); r.text(line, 42, 115, 10, PAL_INK, Align::Left);
    }
  } else {
    r.roundRect(40, 40, 240, 112, 12, PAL_WHITE, PAL_INK, 2);
    if (cam && cam->failed()) {
      drawIcon(r, Icon::Camera, 160, 78, 44, PAL_GREY);
      r.text("No camera found", 160, 112, 17);
      r.text("Draw the level with a finger instead", 160, 132, 10, PAL_INK);
    } else {
      drawIcon(r, Icon::Camera, 160, 78, 44);
      r.text("Waking the camera", 160, 112, 17);
      const float a = t * 5.0f; r.ring(160, 136, 6, 2.5f, PAL_GREEN, a, 4.0f);
    }
  }
  r.g().fillRect(0, 0, 32, 192, PAL_PAPER);                           // the side columns
  r.g().fillRect(288, 0, 32, 192, PAL_PAPER);
  ui.draw(r, nowMs);
}

// =====================================================================================================================
// 4.3 countdown and snap
// =====================================================================================================================
void Game::tickCountdown(const InputState& in) {
  if (!cam || !ensureBuffers(paper)) { go(Screen::Check); return; }
  cam->begin();
  chipBack(nullptr, Icon::Stop);
  const char* hit = ui.update(in);
  if (hit && !std::strcmp(hit, "back")) { cam->lockExposure(false); go(Screen::Check); return; }

  paper.cdT += DT;
  static const Clip words[4] = {Clip::Three, Clip::Two, Clip::One, Clip::Snap};
  for (int i = 0; i < 4; i++) if (!paper.said[i] && paper.cdT >= float(i * COUNTDOWN_STEP_S)) {
    paper.said[i] = true;
    audio.say(words[i]); audio.play(Sfx::Beat);
    const uint16_t cols[2] = {PAL_YELLOW, PAL_GREEN};
    spawn(6, 160, 70, cols, 2, 60, 0.5f, 2);
  }
  if (paper.cdT >= float(SNAP_AT_S)) {
    if (!paper.snapAsked) {                                           // the camera averages three frames in its own task
      if (cam->grabStart(paper.snap)) paper.snapAsked = true;
      else if (!cam->running() || paper.cdT > float(SNAP_AT_S) + 2.0f) { cam->lockExposure(false); say("The camera missed the picture"); go(Screen::Check); return; }
    } else if (!paper.snapped) {
      const Camera::Grab g = cam->grabState();
      if (g == Camera::Grab::Done) {                                  // the frame is taken FIRST, then the click and the flash
        cam->grabAck(); paper.snapped = true; paper.haveSnap = true;
        audio.play(Sfx::Shutter); flash(PAL_WHITE, 0.08f);
        cam->lockExposure(false);
        go(Screen::Processing);
      } else if (g == Camera::Grab::Failed || g == Camera::Grab::Idle) {
        cam->grabAck(); cam->lockExposure(false); say("The camera missed the picture"); go(Screen::Check);
      }
    }
  }
}

void Game::drawCountdown() {
  r.clear(BACKDROP);
  const uint16_t* f = cam ? cam->live() : nullptr;
  if (f) { r.frame565(f, 32, 0, 256, 192, 115); cam->release(); }
  const float ct = paper.cdT;
  const int n = ct < 1 ? 3 : ct < 2 ? 2 : ct < 3 ? 1 : 0;
  const float frac = ct - std::floor(ct);
  const uint16_t col = n == 3 ? PAL_INK : n == 2 ? PAL_RED : n == 1 ? PAL_BLUE : PAL_GREEN;
  r.disc(160, 78, 54, PAL_PAPER);
  const float pop = std::fmax(0.0f, 1.0f - frac * 4.0f);                // a little bounce at the start of every second
  r.ring(160, 78, 54 + 3 * pop, 5, PAL_INK);
  r.ring(160, 78, 54, 7, PAL_GREEN, -PI_F / 2, (1 - frac) * 2 * PI_F);
  r.text(n ? (n == 3 ? "3" : n == 2 ? "2" : "1") : "!", 160, 80, 92, col);
  const float by = 150 - std::fabs(std::sin(frac * PI_F)) * 26;
  r.ball(160, by, t * 3);
  for (int i = 0; i < 4; i++) { r.disc(float(115 + i * 30), 214, 11, ct >= float(i) + 0.05f ? PAL_GREEN : PAL_PAPER); r.ring(float(115 + i * 30), 214, 11, 2.5f, PAL_INK); }
  drawParts(); ui.draw(r, nowMs);
}

// =====================================================================================================================
// processing
// =====================================================================================================================
void Game::tickProcessing(const InputState& in) {
  ui.update(in);
  if (screenT < 0.02f) { paper.procStarted = paper.procDone = false; paper.procT = 0; paper.haveResult = false; paper.busy = false; }
  if (!paper.haveSnap || !paper.work || !paper.snap) { go(Screen::Check); return; }
  paper.procT += DT;
  if (!paper.procStarted && W.available()) {
    paper.params = defaultParams();
    W.vga = paper.snap; W.params = paper.params; W.work = paper.work; W.level = paper.res; W.report = &paper.report;
    paper.procStarted = true;
    W.start(Job::Process);
  }
  if (paper.procStarted && !paper.procDone && W.take(Job::Process)) {
    paper.procDone = true; paper.haveResult = true; paper.params = W.used; paper.reveal = 0; paper.pinDrag = 0;
    const VisionReport& rep = paper.report;
    Serial.printf("[paper vision %u ms (this computer, not the device): status %d marks %s ink %.4f blobs %d]\n", unsigned(W.ms), rep.status, rep.marksFound ? "found" : "missing", rep.inkCover, rep.blobs);
#ifdef EMU
    Serial.printf("[paper level crc %08X]\n", unsigned(crcOfLevel(*paper.res)));
#endif
  }
  if (paper.procDone && paper.procT >= MIN_PROCESSING_S) {
    const bool ok = paper.report.status == VQ_OK || paper.report.status == VQ_ORIENT_GUESS;
    audio.play(ok ? Sfx::Tada : Sfx::Hmm);
    go(Screen::Review);
  }
}

void Game::drawProcessing() {
  r.clear(BACKDROP);
  if (paper.haveSnap && paper.snap) r.frame565(paper.snap, 32, 0, 256, 192, 153);
  const uint16_t cols[3] = {PAL_YELLOW, PAL_WHITE, PAL_GREEN};
  for (int i = 0; i < 14; i++) {
    const float a = t * 2 + i * 1.7f, rr = 30 + std::fmod(i * 37 + t * 60, 70.0f);
    const float x = 160 + std::cos(a) * rr, y = 96 + std::sin(a * 1.3f) * rr * 0.7f, c = std::cos(a), s = std::sin(a);
    r.line(x - 3 * c, y - 3 * s, x + 3 * c, y + 3 * s, 2, cols[i % 3]);
    r.line(x + 3 * s, y - 3 * c, x - 3 * s, y + 3 * c, 2, cols[i % 3]);
  }
  drawIcon(r, Icon::Pencil, 160, 96, 40, PAL_WHITE);
}

// =====================================================================================================================
// 5 review
// =====================================================================================================================
void Game::tickReview(const InputState& in) {
  if (!paper.haveResult || !paper.work) { go(Screen::Check); return; }
  if (screenT < 0.02f) { paper.pinDrag = 0; paper.busy = false; }
  paper.reveal += DT;
  const int st = paper.report.status;
  const bool bad = st != VQ_OK && st != VQ_ORIENT_GUESS;              // Retake pulses
  chipBack();
  Btn items[4];
  items[0].id = "retake"; items[0].icon = Icon::Restart; items[0].iconSize = 28; items[0].pulse = bad;
  if (bad) items[0].fill = PAL_YELLOW;                                // Retake is yellow and breathes when the photo looks wrong
  items[1].id = "less"; items[1].icon = Icon::InkLess; items[1].iconSize = 30;
  items[2].id = "more"; items[2].icon = Icon::InkMore; items[2].iconSize = 30;
  items[3].id = "good"; items[3].icon = Icon::Check; items[3].fill = PAL_GREEN; items[3].iconSize = 38;
  static const int widths[4] = {64, 64, 64, 128};
  bar(items, widths, 4);
  const char* hit = ui.update(in);

  if (W.take(Job::Rethreshold)) {                                      // the new level is swapped in whole
    *paper.res = *paper.tmp; paper.busy = false; paper.reveal = 1;
    audio.play(Sfx::Tick);
#ifdef EMU
    Serial.printf("[paper rethreshold %u ms: status %d ink %.4f ink threshold %d]\n[paper level crc %08X]\n", unsigned(W.ms), paper.report.status, paper.report.inkCover, paper.params.inkThresh, unsigned(crcOfLevel(*paper.res)));
#endif
  }

  // pins: grab one within PIN_GRAB_RADIUS and drag it (the Start / Flag were placed by the vision code)
  if (!paper.busy) {
    const TouchState& t = in.touch;
    Level& lv = *paper.res;
    if (t.pressed && !ui.touchOnButton && t.y < BAR_Y) {
      const float ds = std::hypot(float(t.x - lv.startX), float(t.y - lv.startY)), df = std::hypot(float(t.x - lv.flagX), float(t.y - (lv.flagY - FLAG_CENTER_DY)));
      if (std::fmin(ds, df) <= float(PIN_GRAB_RADIUS)) paper.pinDrag = ds <= df ? 1 : 2;
    }
    if (paper.pinDrag && t.down) {
      const int x = t.x < 8 ? 8 : t.x > WORLD_W - 8 ? WORLD_W - 8 : t.x, y = t.y < 8 ? 8 : t.y > WORLD_H - 2 ? WORLD_H - 2 : t.y;
      if (paper.pinDrag == 1) { lv.startX = x; lv.startY = y; } else { lv.flagX = x; lv.flagY = y + FLAG_CENTER_DY; }
    }
    if (paper.pinDrag && t.released) {                                 // drop: never bury a pin in ground
      if (paper.pinDrag == 1) clearAround(lv, lv.startX, lv.startY, START_SAFE_RADIUS); else clearAround(lv, lv.flagX, lv.flagY - FLAG_CENTER_DY, FLAG_SAFE_RADIUS);
      paper.pinDrag = 0; audio.play(Sfx::Tick);
    }
  }

  if (!hit) return;
  if (!std::strcmp(hit, "back") || !std::strcmp(hit, "retake")) { go(Screen::Check); return; }
  if ((!std::strcmp(hit, "less") || !std::strcmp(hit, "more")) && !paper.busy && W.available()) {
    const int step = !std::strcmp(hit, "less") ? INK_STEP : -INK_STEP;   // less ink = a higher threshold
    int th = paper.params.inkThresh + step; th = th < 38 ? 38 : th > 128 ? 128 : th;
    paper.params.inkThresh = th;
    *paper.tmp = *paper.res;
    W.params = paper.params; W.work = paper.work; W.level = paper.tmp; W.report = &paper.report;
    paper.busy = true; paper.pinDrag = 0;
    W.start(Job::Rethreshold);
  } else if (!std::strcmp(hit, "good") && !paper.busy) {
    // save into the first empty slot (the last-opened slot if all ten are full), name it, and play it
    int target = store.firstEmpty();
    if (target < 0) { target = store.settings.lastSlot; if (target < 0 || target >= SLOTS) target = 0; char m[40]; snprintf(m, sizeof m, "All slots full: replaced %d", target + 1); say(m); }
    *paper.tmp = *paper.res;                                           // (not on the stack: a Level is 23 KB and the loop task has 8 KB)
    Level& lv = *paper.tmp;
    snprintf(lv.name, sizeof lv.name, "Paper %d", target + 1);
    lv.fromPaper = true;
    store.save(target, lv);
    store.settings.firstRunDone = true;
    Serial.printf("[paper accept slot %d]\n", target);
    if (cam) cam->end();                                               // the camera is not needed any more: off
    openLevel(target, true);
  }
}

void Game::drawReview() {
  r.clear(PAL_PAPER);
  if (!paper.haveResult || !paper.work) { ui.draw(r, nowMs); return; }
  const Level& lv = *paper.res;
  r.straight565(paper.work->straight565, 90);                          // the straightened page at ~35 %
  const float t3 = paper.reveal / 0.13f;                               // the layers appear one by one: ground, spikes, bouncy
  LayerMask mask; mask.ground = t3 > 0; mask.spike = t3 > 1; mask.bouncy = t3 > 2;
  r.drawLayers(lv, mask);
  r.startPin(float(lv.startX), float(lv.startY), paper.pinDrag == 1);
  r.flagPin(float(lv.flagX), float(lv.flagY), paper.pinDrag == 2);
  // legend: black / red / blue
  r.roundRectA(112, 6, 96, 34, 12, PAL_WHITE, 230, PAL_INK, 2);
  r.g().fillRect(128, 15, 14, 14, PAL_INK);
  const float tri[6] = {152, 29, 159, 15, 166, 29};
  r.polygon(tri, 3, PAL_RED); r.polyline(tri, 3, 1.6f, PAL_INK, true);
  r.disc(185, 22, 7, PAL_BLUE); r.ring(185, 22, 7, 1.6f, PAL_INK);
  const char* why = nullptr;
  switch (paper.report.status) {
    case VQ_MARKS_MISSING: why = "paper not found: used the whole picture"; break;
    case VQ_EMPTY: why = "draw bigger and darker"; break;
    case VQ_TOO_DARK: why = "more light, please"; break;
    case VQ_TOO_MUCH_INK: why = "is the paper in view?"; break;
    default: break;
  }
  if (why) r.text(why, 160, 52, 11, PAL_INK, Align::Center, PAL_WHITE);
  if (paper.busy) r.text("...", 160, 96, 40, PAL_INK, Align::Center, PAL_WHITE);
  ui.draw(r, nowMs);
}

}  // namespace scribble
