// Native unit tests of the vision core, run on the PC:  pio test -e native -f test_vision
// The golden data comes from the browser simulator (sim/scripts/make-vision-golden.ts):
//   shared/golden/vision_frames.bin   a few synthetic template photos (VGA RGB565, compressed)
//   shared/generated/vision_golden.h  what vision.ts makes of each photo (after a portable integer transform: dim, noise, ...)
// These tests prove that vision.cpp gives the SAME marks (to 0.1 px), the SAME homography, the SAME straightened page and
// ink arrays, and byte-identical level layers and pins as the TypeScript the simulator runs. Not "99.5 %": exactly.
#include <unity.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "level.h"
#include "vision.h"
#include "vision_golden.h"

using namespace scribble;

static constexpr int N_FRAMES = 4;
static uint16_t frames[N_FRAMES][VGA_W * VGA_H];       // the decoded stored photos
static uint16_t photo[VGA_W * VGA_H];                  // a frame after the case's transform
static uint8_t gray[QVGA_W * QVGA_H];
static VisionWork W;                                   // ~770 KB: the whole vision workspace, owned by the test
static Level L;
static uint8_t scrbBuf[MAX_FILE_BYTES];
static bool loaded = false;

// ---- loading the photos (mirror of decodeFrame in sim/scripts/vision-golden-lib.ts) -----------------------------------------
static uint8_t* readAll(const char* name, size_t& n) {
  const char* prefixes[] = { "../shared/golden/", "shared/golden/", "../../shared/golden/", "../../../shared/golden/" };
  for (const char* pre : prefixes) {
    char path[256]; snprintf(path, sizeof path, "%s%s", pre, name);
    FILE* f = fopen(path, "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END); n = size_t(ftell(f)); fseek(f, 0, SEEK_SET);
    uint8_t* b = static_cast<uint8_t*>(malloc(n)); if (fread(b, 1, n, f) != n) { fclose(f); free(b); return nullptr; }
    fclose(f); return b;
  }
  return nullptr;
}
static inline uint16_t predict(const uint16_t* f, int i) { return i >= VGA_W ? f[i - VGA_W] : i > 0 ? f[i - 1] : 0; }
static void decodeFrame(const uint8_t* b, size_t pos, size_t end, uint16_t* f) {
  const int total = VGA_W * VGA_H; int i = 0;
  auto vi = [&]() { uint32_t v = 0; int sh = 0; uint8_t c; do { c = b[pos++]; v |= uint32_t(c & 127) << sh; sh += 7; } while (c & 128); return v; };
  for (;;) {
    const uint32_t run = vi(); for (uint32_t k = 0; k < run && i < total; k++, i++) f[i] = predict(f, i);
    if (pos >= end) break;
    const uint32_t z = vi(); if (i < total) { const int d = (z & 1) ? -int((z + 1) >> 1) : int(z >> 1); f[i] = uint16_t((predict(f, i) + d) & 0xffff); i++; }
  }
  while (i < total) { f[i] = predict(f, i); i++; }
}
static void loadFrames() {
  if (loaded) return;
  size_t n = 0; uint8_t* b = readAll("vision_frames.bin", n);
  TEST_ASSERT_NOT_NULL_MESSAGE(b, "cannot find shared/golden/vision_frames.bin (run the tests from firmware/)");
  TEST_ASSERT_TRUE(b[0] == 'V' && b[1] == 'G' && b[2] == 'F' && b[3] == '1');
  uint32_t count; memcpy(&count, b + 4, 4); TEST_ASSERT_EQUAL_UINT32(N_FRAMES, count);
  size_t pos = 8;
  for (int k = 0; k < N_FRAMES; k++) { uint32_t len; memcpy(&len, b + pos, 4); decodeFrame(b, pos + 4, pos + 4 + len, frames[k]); pos += 4 + len; }
  free(b); loaded = true;
}

// ---- the portable integer transforms (mirror of applyXform) -------------------------------------------------------------------
static inline int r8(unsigned p) { return int(((p >> 11) << 3) | (p >> 13)); }
static inline int g8(unsigned p) { return int((((p >> 5) & 63) << 2) | ((p >> 9) & 3)); }
static inline int b8(unsigned p) { return int(((p & 31) << 3) | ((p >> 2) & 7)); }
static inline int c255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void makePhoto(const vgolden::Case& c) {
  const uint16_t* in = frames[c.frame]; const int total = VGA_W * VGA_H;
  uint32_t s = uint32_t(c.xb);
  for (int i = 0; i < total; i++) {
    const unsigned p = in[i];
    switch (c.xform) {
      case 0: photo[i] = uint16_t(p); break;
      case 1: photo[i] = pack565((r8(p) * c.xa + 128) >> 8, (g8(p) * c.xa + 128) >> 8, (b8(p) * c.xa + 128) >> 8); break;
      case 2: { s = s * 1664525u + 1013904223u; const int n = int(((s >> 24) * uint32_t(2 * c.xa + 1)) >> 8) - c.xa; photo[i] = pack565(c255(r8(p) + n), c255(g8(p) + n), c255(b8(p) + n)); break; }
      case 3: photo[i] = in[total - 1 - i]; break;
      case 4: { auto q = [&](int v) { return 255 - (((255 - v) * c.xa + 128) >> 8); }; photo[i] = pack565(q(r8(p)), q(g8(p)), q(b8(p))); break; }
    }
  }
}

static const vgolden::Case& caseNamed(const char* name) {
  for (int k = 0; k < vgolden::nCases; k++) if (!strcmp(vgolden::cases[k].name, name)) return vgolden::cases[k];
  TEST_FAIL_MESSAGE(name);
  return vgolden::cases[0];
}
static VisionParams paramsOf(const vgolden::Params& q) {
  VisionParams p = defaultParams();
  if (q.inkThresh >= 0) p.inkThresh = q.inkThresh;
  if (q.closeR >= 0) p.closeR = q.closeR;
  if (q.thickenR >= 0) p.thickenR = q.thickenR;
  if (q.minBlob >= 0) p.minBlob = q.minBlob;
  return p;
}
static uint32_t crcOf(const void* p, size_t n) { return crc32(static_cast<const uint8_t*>(p), 0, n); }
static uint32_t scrbCrc(const Level& l) { const size_t n = encodeLevel(l, false, scrbBuf, sizeof scrbBuf); return n ? crc32(scrbBuf, 0, n) : 0; }

static char msg[160];
static const char* at(const vgolden::Case& c, const char* what) { snprintf(msg, sizeof msg, "case '%s': %s", c.name, what); return msg; }

void setUp() { loadFrames(); }
void tearDown() {}

// =====================================================================================================================
void test_the_photos_decode_and_transforms_are_deterministic() {
  // the stored photos must decode to the synthetic template photos: paper white near the middle, dark marks near the corners
  const uint16_t mid = frames[0][240 * VGA_W + 320];
  TEST_ASSERT_TRUE(r8(mid) > 180 && g8(mid) > 180);
  // an upside-down photo of the clean frame is the pixel reverse of it
  makePhoto(caseNamed("upside-down"));
  for (int i = 0; i < 1000; i++) TEST_ASSERT_EQUAL_HEX16(frames[0][VGA_W * VGA_H - 1 - i], photo[i]);
}

void test_the_four_marks_agree_with_the_simulator_to_a_tenth_of_a_pixel() {
  double worst = 0; int checked = 0;
  for (int k = 0; k < vgolden::nCases; k++) {
    const vgolden::Case& c = vgolden::cases[k]; makePhoto(c);
    Marks m; const bool found = findMarks(photo, defaultParams(), W, m);
    // the golden "marksFound" is the pipeline's; a degenerate quad would also count as not found, so compare through worldToVga
    int64_t hq[8]; const bool ok = found && worldToVga(m, hq);
    TEST_ASSERT_EQUAL_MESSAGE(c.marksFound, ok, at(c, "marks found or not"));
    if (!ok) continue;
    TEST_ASSERT_EQUAL_MESSAGE(c.orientGuess, m.orientGuess, at(c, "orientation guess"));
    for (int i = 0; i < 4; i++) {
      const double dx = std::fabs(m.pts[i].x - c.marks[2 * i]), dy = std::fabs(m.pts[i].y - c.marks[2 * i + 1]);
      worst = std::fmax(worst, std::fmax(dx, dy));
      snprintf(msg, sizeof msg, "case '%s' mark %d: (%.4f, %.4f) vs (%.4f, %.4f)", c.name, i, m.pts[i].x, m.pts[i].y, c.marks[2 * i], c.marks[2 * i + 1]);
      TEST_ASSERT_TRUE_MESSAGE(dx <= 0.1 && dy <= 0.1, msg);
    }
    for (int i = 0; i < 8; i++) { snprintf(msg, sizeof msg, "case '%s' homography coefficient %d: %lld vs %lld", c.name, i, (long long)hq[i], (long long)c.hq[i]); TEST_ASSERT_TRUE_MESSAGE(hq[i] == c.hq[i], msg); }
    checked++;
  }
  TEST_ASSERT_TRUE(checked >= 6);
  printf("vision marks: %d cases with marks, worst difference from the simulator = %.9f px\n", checked, worst);
}

void test_the_warp_is_byte_identical_to_the_simulator() {
  for (int k = 0; k < vgolden::nCases; k++) {
    const vgolden::Case& c = vgolden::cases[k]; makePhoto(c);
    Marks m; int64_t hq[8];
    if (!(findMarks(photo, defaultParams(), W, m) && worldToVga(m, hq))) centreCropMap(hq);   // the same fallback as the pipeline
    warp(photo, hq, W.straight565);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcStraight, crcOf(W.straight565, sizeof W.straight565), at(c, "straightened page"));
  }
}

static void checkPipeline(const vgolden::Case& c) {
  makePhoto(c);
  VisionReport r; VisionParams used;
  visionProcess(photo, defaultParams(), W, L, r, &used);
  TEST_ASSERT_EQUAL_MESSAGE(c.marksFound, r.marksFound, at(c, "marksFound"));
  TEST_ASSERT_EQUAL_MESSAGE(c.orientGuess, r.orientGuess, at(c, "orientGuess"));
  TEST_ASSERT_EQUAL_MESSAGE(c.status, r.status, at(c, "quality status"));
  TEST_ASSERT_EQUAL_MESSAGE(c.usedInkThresh, used.inkThresh, at(c, "ink threshold after the automatic retry"));
  TEST_ASSERT_EQUAL_MESSAGE(c.blobs, r.blobs, at(c, "blob count"));
  TEST_ASSERT_TRUE_MESSAGE(r.inkCover == c.inkCover, at(c, "ink coverage"));
  TEST_ASSERT_TRUE_MESSAGE(r.bgMean == c.bgMean, at(c, "paper brightness"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcStraight, crcOf(W.straight565, sizeof W.straight565), at(c, "straightened page"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcCls, crcOf(W.cls, sizeof W.cls), at(c, "colour classes"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcInk, crcOf(W.ink, sizeof W.ink), at(c, "ink strength"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcGround, crcOf(L.ground, LAYER_BYTES), at(c, "ground layer"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcSpike, crcOf(L.spike, LAYER_BYTES), at(c, "spike layer"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcBouncy, crcOf(L.bouncy, LAYER_BYTES), at(c, "bouncy layer"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(c.startX, L.startX, at(c, "start x")); TEST_ASSERT_EQUAL_INT_MESSAGE(c.startY, L.startY, at(c, "start y"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(c.flagX, L.flagX, at(c, "flag x")); TEST_ASSERT_EQUAL_INT_MESSAGE(c.flagY, L.flagY, at(c, "flag y"));
  TEST_ASSERT_TRUE_MESSAGE(L.fromPaper, at(c, "fromPaper flag"));
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcScrb, scrbCrc(L), at(c, "whole .scrb file"));
  // Review's thinner / thicker ink etc. from the kept ink array
  for (int k = 0; k < c.nRethresh; k++) {
    const vgolden::Rethresh& q = c.rethresh[k];
    VisionReport r2; visionRethreshold(W, paramsOf(q.params), L, r2);
    snprintf(msg, sizeof msg, "case '%s' re-threshold #%d", c.name, k);
    TEST_ASSERT_EQUAL_MESSAGE(q.status, r2.status, msg); TEST_ASSERT_EQUAL_MESSAGE(q.blobs, r2.blobs, msg); TEST_ASSERT_TRUE_MESSAGE(q.inkCover == r2.inkCover, msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(q.ground, crcOf(L.ground, LAYER_BYTES), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(q.spike, crcOf(L.spike, LAYER_BYTES), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(q.bouncy, crcOf(L.bouncy, LAYER_BYTES), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(q.scrb, scrbCrc(L), msg);
  }
}

void test_every_stage_and_the_level_are_byte_identical_to_the_simulator() {
  for (int k = 0; k < vgolden::nCases; k++) checkPipeline(vgolden::cases[k]);
}

void test_a_sheet_without_marks_falls_back_to_a_centre_crop() {
  const vgolden::Case* c = &caseNamed("nomarks");
  makePhoto(*c);
  VisionReport r; visionProcess(photo, defaultParams(), W, L, r);
  TEST_ASSERT_FALSE(r.marksFound); TEST_ASSERT_FALSE(W.marksFound);
  TEST_ASSERT_EQUAL(VQ_MARKS_MISSING, r.status);
  // the fallback is the centre 640 x 384 of the frame shrunk 2:1: page pixel (x, y) ~ the 2 x 2 block at (2x, 48 + 2y)
  for (int i = 0; i < 2000; i++) {
    const int x = 8 + (i * 37) % 300, y = 8 + (i * 91) % 170;
    int sr = 0, sg = 0, sb = 0;
    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { const unsigned p = photo[(48 + 2 * y + dy) * VGA_W + 2 * x + dx]; sr += r8(p); sg += g8(p); sb += b8(p); }
    const unsigned q = W.straight565[y * WORLD_W + x];
    TEST_ASSERT_TRUE(std::abs(r8(q) - sr / 4) <= 16 && std::abs(g8(q) - sg / 4) <= 16 && std::abs(b8(q) - sb / 4) <= 16);   // bilinear taps + RGB565 rounding
  }
  checkPipeline(*c);
}

void test_the_automatic_retry_lowers_the_threshold_like_the_simulator() {
  const vgolden::Case* c = &caseNamed("faded");
  makePhoto(*c);
  VisionReport r; VisionParams used; visionProcess(photo, defaultParams(), W, L, r, &used);
  TEST_ASSERT_EQUAL(58, used.inkThresh);                           // 77 * 0.75, rounded
  TEST_ASSERT_TRUE(r.status != VQ_EMPTY);
  checkPipeline(*c);
}

void test_a_blank_sheet_and_a_dark_room_are_reported_not_crashed_on() {
  for (int k = 0; k < vgolden::nCases; k++) {
    const vgolden::Case& c = vgolden::cases[k];
    if (strcmp(c.name, "blank") && strcmp(c.name, "dark")) continue;
    makePhoto(c); VisionReport r; visionProcess(photo, defaultParams(), W, L, r);
    TEST_ASSERT_EQUAL_MESSAGE(!strcmp(c.name, "blank") ? VQ_EMPTY : VQ_TOO_DARK, r.status, at(c, "status"));
    TEST_ASSERT_TRUE(isEmpty(L));
    TEST_ASSERT_EQUAL(32, L.startX); TEST_ASSERT_EQUAL(32, L.startY);   // the documented fallbacks
    TEST_ASSERT_EQUAL(288, L.flagX); TEST_ASSERT_EQUAL(100, L.flagY);
  }
}

void test_the_drawn_level_is_recovered_and_pins_are_on_the_ledges() {
  // not parity: the clean photo must give back what was drawn (overlap with the level the photo shows)
  Level truth; starterLevel(truth);
  paintLine(truth, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(truth, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(truth, MAT_GROUND, 200, 124, 248, 116, 4);
  fillRect(truth, MAT_SPIKE, 110, 160, 160, 170); fillRect(truth, MAT_BOUNCY, 170, 160, 215, 170);
  makePhoto(caseNamed("clean"));
  VisionReport r; visionProcess(photo, defaultParams(), W, L, r);
  TEST_ASSERT_EQUAL(VQ_OK, r.status);
  TEST_ASSERT_TRUE(levelLayerIoU(truth.ground, L.ground) > 0.9);
  TEST_ASSERT_TRUE(levelLayerIoU(truth.spike, L.spike) > 0.9);
  TEST_ASSERT_TRUE(levelLayerIoU(truth.bouncy, L.bouncy) > 0.9);
  TEST_ASSERT_TRUE(L.startX < 80 && L.startY < 120); TEST_ASSERT_TRUE(L.flagX > 240 && std::abs(L.flagY - 112) < 10);
}

void test_the_live_camera_check_matches_the_simulator() {
  double worst = 0; int n = 0;
  for (int k = 0; k < vgolden::nCases; k++) {
    const vgolden::Case& c = vgolden::cases[k]; makePhoto(c);
    grayQvga(photo, gray);
    for (int j = 0; j < c.nLive; j++) {
      const vgolden::Live& e = c.live[j];
      LiveCheck lc; visionLiveCheck(gray, e.upright, W, lc);
      TEST_ASSERT_EQUAL_MESSAGE(e.marks, lc.marks, at(c, "live: marks"));
      if (!e.marks) continue;
      for (int i = 0; i < 4; i++) { TEST_ASSERT_EQUAL_MESSAGE(e.lights[i], lc.lights[i], at(c, "live: light")); worst = std::fmax(worst, std::fmax(std::fabs(lc.quad[i].x - e.quad[2 * i]), std::fabs(lc.quad[i].y - e.quad[2 * i + 1]))); }
      TEST_ASSERT_EQUAL_MESSAGE(e.cue, lc.cue, at(c, "live: cue"));
      TEST_ASSERT_EQUAL_MESSAGE(e.allGreen, lc.allGreen, at(c, "live: allGreen"));
      const double got[7] = { lc.distCm, lc.edgePx, lc.paperMean, lc.clipFrac, lc.contrast, lc.ratioH, lc.ratioV };
      const double want[7] = { e.distCm, e.edgePx, e.paperMean, e.clipFrac, e.contrast, e.ratioH, e.ratioV };
      for (int i = 0; i < 7; i++) { worst = std::fmax(worst, std::fabs(got[i] - want[i])); snprintf(msg, sizeof msg, "case '%s' live value %d: %.12g vs %.12g", c.name, i, got[i], want[i]); TEST_ASSERT_TRUE_MESSAGE(std::fabs(got[i] - want[i]) <= 1e-9, msg); }
      n++;
    }
  }
  TEST_ASSERT_TRUE(n >= 6);
  printf("live check: %d runs, worst difference from the simulator = %.3g\n", n, worst);
}

void test_the_work_buffer_can_be_reused_for_the_next_photo() {
  // scratch memory is shared between stages and between photos: a second run of the same photo must give the same bytes
  const vgolden::Case& a = caseNamed("clean"); const vgolden::Case& b = caseNamed("tilt");
  checkPipeline(a); checkPipeline(b); checkPipeline(a);
}

// ---- the yield hook (device watchdog: the vision task must let IDLE0 run during a long job) ------------------------------------
struct YieldLog { int calls; };
static void countYield(void* u) { static_cast<YieldLog*>(u)->calls++; }

void test_the_yield_hook_is_called_often_and_changes_nothing() {
  static VisionWork W2; static Level L2;
  const char* names[3] = { "clean", "faded", "nomarks" };              // one pass, the silent retry, and the centre-crop fallback
  for (const char* name : names) {
    const vgolden::Case& c = caseNamed(name); makePhoto(c);
    VisionReport r1; VisionParams used1; visionProcess(photo, defaultParams(), W, L, r1, &used1);
    YieldLog log{ 0 };
    VisionParams p = defaultParams(); p.yieldFn = countYield; p.yieldUser = &log;
    VisionReport r2; VisionParams used2; visionProcess(photo, p, W2, L2, r2, &used2);
    snprintf(msg, sizeof msg, "case '%s': %d yields", name, log.calls);
    TEST_ASSERT_TRUE_MESSAGE(log.calls >= 15, msg);                      // stages, every 64 rows, every colour class
    TEST_ASSERT_EQUAL_MESSAGE(r1.status, r2.status, msg); TEST_ASSERT_EQUAL_MESSAGE(r1.blobs, r2.blobs, msg); TEST_ASSERT_EQUAL_MESSAGE(r1.passes, r2.passes, msg);
    TEST_ASSERT_TRUE_MESSAGE(r1.inkCover == r2.inkCover && r1.bgMean == r2.bgMean, msg);
    TEST_ASSERT_EQUAL_MESSAGE(used1.inkThresh, used2.inkThresh, msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, memcmp(&L, &L2, sizeof L), msg);           // the whole level, byte for byte
    TEST_ASSERT_EQUAL_MESSAGE(0, memcmp(W.straight565, W2.straight565, sizeof W.straight565), msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, memcmp(W.ink, W2.ink, sizeof W.ink), msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, memcmp(W.labels, W2.labels, sizeof W.labels), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.crcScrb, scrbCrc(L2), msg);          // and still the golden one
    // the re-threshold yields too (once per colour class at least)
    const int before = log.calls; VisionReport r3; visionRethreshold(W2, p, L2, r3);
    TEST_ASSERT_TRUE_MESSAGE(log.calls - before >= 3, msg);
    TEST_ASSERT_EQUAL_MESSAGE(1, r3.passes, msg);
    printf("yield hook, case '%s': %d calls in visionProcess (%d passes), %d in a re-threshold\n", name, before, r2.passes, log.calls - before);
  }
  TEST_ASSERT_EQUAL(1, defaultParams().yieldFn == nullptr ? 1 : 0);    // off unless the caller sets it: golden runs never yield
}

void test_timing_and_memory_report() {
  const vgolden::Case& c = caseNamed("clean"); makePhoto(c);
  using clk = std::chrono::steady_clock;
  auto ms = [](clk::time_point t0, clk::time_point t1) { return std::chrono::duration<double, std::milli>(t1 - t0).count(); };
  VisionReport r; Marks m; int64_t hq[8]; double tMarks = 1e9, tWarp = 1e9, tTotal = 1e9, tRe = 1e9, tLive = 1e9;
  for (int rep = 0; rep < 5; rep++) {
    auto t0 = clk::now(); const bool found = findMarks(photo, defaultParams(), W, m); auto t1 = clk::now(); worldToVga(m, hq); warp(photo, hq, W.straight565); auto t2 = clk::now();
    TEST_ASSERT_TRUE(found);
    visionProcess(photo, defaultParams(), W, L, r); auto t3 = clk::now();
    visionRethreshold(W, defaultParams(), L, r); auto t4 = clk::now();
    grayQvga(photo, gray); LiveCheck lc; visionLiveCheck(gray, true, W, lc); auto t5 = clk::now();
    tMarks = std::fmin(tMarks, ms(t0, t1)); tWarp = std::fmin(tWarp, ms(t1, t2)); tTotal = std::fmin(tTotal, ms(t2, t3)); tRe = std::fmin(tRe, ms(t3, t4)); tLive = std::fmin(tLive, ms(t4, t5));
  }
  printf("vision timing on this PC (best of 5): find marks %.2f ms, homography + warp %.2f ms, visionProcess (whole, 1 pass) %.2f ms, re-threshold %.2f ms, live check %.2f ms\n", tMarks, tWarp, tTotal, tRe, tLive);
  printf("vision memory: sizeof(VisionWork) = %zu bytes (kept %zu + scratch %zu: mark %zu, analyse %zu, morph %zu); VGA frame %zu; Level %zu\n",
         sizeof(VisionWork), sizeof(VisionWork) - sizeof(VisionWork::Scratch), sizeof(VisionWork::Scratch), sizeof(MarkScratch), sizeof(AnalyseScratch), sizeof(MorphScratch), size_t(VGA_W * VGA_H * 2), sizeof(Level));
  TEST_ASSERT_TRUE(sizeof(VisionWork) < 800000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_photos_decode_and_transforms_are_deterministic);
  RUN_TEST(test_the_four_marks_agree_with_the_simulator_to_a_tenth_of_a_pixel);
  RUN_TEST(test_the_warp_is_byte_identical_to_the_simulator);
  RUN_TEST(test_every_stage_and_the_level_are_byte_identical_to_the_simulator);
  RUN_TEST(test_a_sheet_without_marks_falls_back_to_a_centre_crop);
  RUN_TEST(test_the_automatic_retry_lowers_the_threshold_like_the_simulator);
  RUN_TEST(test_a_blank_sheet_and_a_dark_room_are_reported_not_crashed_on);
  RUN_TEST(test_the_drawn_level_is_recovered_and_pins_are_on_the_ledges);
  RUN_TEST(test_the_live_camera_check_matches_the_simulator);
  RUN_TEST(test_the_work_buffer_can_be_reused_for_the_next_photo);
  RUN_TEST(test_the_yield_hook_is_called_often_and_changes_nothing);
  RUN_TEST(test_timing_and_memory_report);
  return UNITY_END();
}
