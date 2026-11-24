// vision.h - camera frame of the printed template -> Level.   (portable core: no Arduino, no I/O, no allocation, no globals)
// Expression-for-expression port of sim/src/vision.ts. Pipeline and parameters: docs/system-design.md section 5.
//
//   0 capture (done by the camera driver)  1 find the four corner marks  2 straighten (homography + warp)
//   3 convert  4 paper estimate  5 ink strength  6 classify  7 clean up  8 quality  9 place pins  10 pack
//
// All buffers are owned by the caller: the VGA frame in, a VisionWork (about 770 KB: put it in PSRAM, never on the
// stack), a Level and a VisionReport out. Everything per pixel is integer maths; the doubles (mark statistics, the
// 8 x 8 homography solve, the live check) only use + - * / sqrt, so the results are bit-identical to the TypeScript.
#pragma once
#include <cstddef>
#include <cstdint>
#include "config.h"
#include "level.h"

namespace scribble {

constexpr int VGA_W = CAM_W, VGA_H = CAM_H;               // capture: 640 x 480 RGB565
constexpr int QVGA_W = 320, QVGA_H = 240;                 // the gray image the mark finder works on
constexpr int PAGE_N = WORLD_W * WORLD_H;                 // the straightened page: 320 x 192 = 61,440 pixels

// quality statuses (the sim's Q_*; prefixed here because config.h already has a Q_TOO_DARK threshold)
enum VisionStatus { VQ_OK = 0, VQ_MARKS_MISSING = 1, VQ_ORIENT_GUESS = 2, VQ_TOO_DARK = 3, VQ_EMPTY = 4, VQ_TOO_MUCH_INK = 5, VQ_NOISY = 6 };

struct Pt { double x, y; };

struct VisionParams {
  int inkThresh, satMin, colorVMin, minBlob, closeR, thickenR, edgeMargin, bgMin;
  double markRatio;
  /** Optional yield hook (null = none; defaultParams() leaves it null). The pipeline calls yieldFn(yieldUser) between stages, once
   *  per colour class in the clean-up and every 64 rows of the per-pixel loops, so a job on a device task can let other tasks
   *  (IDLE, and with it the task watchdog) run: the device passes a vTaskDelay(1). It never changes a result. */
  void (*yieldFn)(void* user);
  void* yieldUser;
};
VisionParams defaultParams();                              // from config.h

struct VisionReport {
  int status; bool marksFound, orientGuess;
  double inkCover, bgMean; int blobs;
  int passes;                      // label + clean-up passes this call ran: 1, or 3 when visionProcess tried its silent retry (5.8)
};

/** Mark centres in VGA pixels, in PAGE order: top-left (the ring), top-right, bottom-right, bottom-left. */
struct Marks { Pt pts[4]; bool orientGuess; };

/** Scratch of stage 1 (and of the live check): the QVGA integral image doubles as the flood-fill stack. */
struct MarkScratch {
  uint8_t gray[QVGA_W * QVGA_H];                           // 2 x 2 averaged gray of the VGA frame
  uint8_t mask[QVGA_W * QVGA_H];                           // adaptive threshold, then 0 / 1 / 2 = free / pending / visited
  uint32_t integ[(QVGA_W + 1) * (QVGA_H + 1)];
};
struct AnalyseScratch { uint8_t gray[PAGE_N]; uint8_t bg[PAGE_N]; };                         // stages 3-5
struct MorphScratch { uint8_t a[PAGE_N]; uint8_t b[PAGE_N]; uint8_t t[PAGE_N]; uint16_t stack[PAGE_N]; };   // stage 7

/**
 * Everything the pipeline needs between and inside stages, so Review can re-threshold without the camera.
 * The first block is kept after processing; the union is scratch that every stage reuses (stage 1, 3-5 and 7 never
 * run at the same time). sizeof(VisionWork) is the whole vision memory budget; the VGA frame itself is the caller's.
 */
struct VisionWork {
  uint16_t straight565[PAGE_N];     // the straightened page, 320 x 192 RGB565 (drawn dimmed behind the Review screen)
  uint8_t ink[PAGE_N];              // stage 5: 0..255, independent of the threshold
  uint8_t cls[PAGE_N];              // stage 6: 0 dark/other (ground), 1 red (spike), 2 blue (bouncy)
  uint8_t labels[PAGE_N];           // final: 0 none, 1 ground, 2 spike, 3 bouncy (after clean-up)
  Marks marks; bool marksFound;
  double bgMean;
  union Scratch { MarkScratch mark; AnalyseScratch analyse; MorphScratch morph; } scratch;
};

// ---- stage 1 ----------------------------------------------------------------------------------------------
/** 640 x 480 RGB565 -> 320 x 240 gray by 2 x 2 averaging. */
void grayQvga(const uint16_t* vga, uint8_t* out);
/** Four marks in page order (uses work.scratch.mark). False = not found. */
bool findMarks(const uint16_t* vga, const VisionParams& p, VisionWork& work, Marks& out);

// ---- stage 2 ----------------------------------------------------------------------------------------------
/** Solve the 3 x 3 projective map (h8 = 1) taking each src point to its dst point; h gets 9 doubles. */
void solveHomography(const Pt src[4], const Pt dst[4], double h[9]);
/** World (level px) -> VGA map from the marks: hq[0..5] in 16.16, hq[6], hq[7] in 2.30. False if degenerate. */
bool worldToVga(const Marks& m, int64_t hq[8]);
/** Fallback map: the centre 640 x 384 of the frame shrunk to 320 x 192. */
void centreCropMap(int64_t hq[8]);
/** Integer warp of the VGA frame onto the 320 x 192 level grid (4 bilinear taps per pixel). */
void warp(const uint16_t* vga, const int64_t hq[8], uint16_t* out);

// ---- the whole pipeline -----------------------------------------------------------------------------------
/** The pipeline once with the given parameters. Fills work, level (fromPaper, pins placed) and report. */
void visionProcessOnce(const uint16_t* vga, const VisionParams& p, VisionWork& work, Level& level, VisionReport& report);
/** Review's thinner/thicker buttons: stages 6-10 again from work.ink / work.cls. No camera, no page finder. */
void visionRethreshold(VisionWork& work, const VisionParams& p, Level& level, VisionReport& report);
/** The full pipeline plus the single silent automatic retry (docs/system-design.md 5.8). `used` = the params of the result. */
void visionProcess(const uint16_t* vga, const VisionParams& p, VisionWork& work, Level& level, VisionReport& report, VisionParams* used = nullptr);

// ---- the live Camera check (calibration screen) -----------------------------------------------------------
enum { LIGHT_GREY = 0, LIGHT_RED = 1, LIGHT_YELLOW = 2, LIGHT_GREEN = 3 };
enum { CUE_NONE = 0, CUE_CLOSER = 1, CUE_BACK = 2, CUE_TILT = 3 };
struct LiveCheck {
  bool marks;
  Pt quad[4];                      // QVGA pixels, clockwise (page corner order unknown without the ring test); valid if marks
  double distCm, edgePx, paperMean, clipFrac, contrast;
  double ratioH, ratioV;           // folded >= 1: how different opposite sides are (perspective)
  int lights[4];                   // distance, sharp, light, level
  int cue;
  bool allGreen;
};
/** `gray` = 320 x 240 gray image (grayQvga of the preview); uses work.scratch.mark.mask / .integ only. */
void visionLiveCheck(const uint8_t* gray, bool accelUpright, VisionWork& work, LiveCheck& out);

/** Overlap (intersection / union) of two 1-bit layers; 1 if both are empty. For tests and the debug panel. */
double levelLayerIoU(const uint8_t* a, const uint8_t* b);

inline uint16_t pack565(int r, int g, int b) { return uint16_t(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3)); }

}  // namespace scribble
