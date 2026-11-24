// camera.cpp - the camera.   Device: esp32-camera (GC0308) in a task on core 0.   Emulator: test photos.   See camera.h.
// OWNER: Paper agent.
#include "camera.h"
#include <M5Unified.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "i2c_lock.h"
#include "vision.h"

namespace scribble {
namespace {
inline int r8(unsigned p) { return int(((p >> 11) << 3) | (p >> 13)); }
inline int g8(unsigned p) { return int((((p >> 5) & 63) << 2) | ((p >> 9) & 3)); }
inline int b8(unsigned p) { return int(((p & 31) << 3) | ((p >> 2) & 7)); }
inline int gray8(unsigned p) { return (77 * r8(p) + 150 * g8(p) + 29 * b8(p)) >> 8; }   // == vision.cpp grayOf(rOf, gOf, bOf)
/** The snap's average of three frames, per channel in 8 bits, rounded like the simulator's Math.round(sum / 3). */
inline uint16_t avg3(unsigned a, unsigned b, unsigned c) {
  return pack565((2 * (r8(a) + r8(b) + r8(c)) + 3) / 6, (2 * (g8(a) + g8(b) + g8(c)) + 3) / 6, (2 * (b8(a) + b8(b) + b8(c)) + 3) / 6);
}
/** Every buffer camera.cpp allocates goes through these two (Camera::bufferBytes(), the start/stop leak test in emu/camera_test.sh). */
std::atomic<int64_t> heldBytes{0};
}  // namespace
size_t Camera::bufferBytes() { const int64_t n = heldBytes.load(); return n > 0 ? size_t(n) : 0; }
}  // namespace scribble

// =====================================================================================================================
#ifndef EMU
// =====================================================================================================================
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace scribble {
namespace {

// ---- the CoreS3 camera: facts and guesses (docs/hardware-notes.md, "Camera bring-up", has the sources for every line).
// Each value is tagged: CONFIRMED (matches the vendor sources), CORRECTED (the first guess was wrong; fixed from the sources), or
// UNVERIFIED on hardware (no source settles it: decided at the first power-on).
// The build: platform espressif32@8.0.0 ships Arduino-ESP32 3.3.x on ESP-IDF 5.5 (framework-arduinoespressif32-esp32s3-libs 3.3.12),
// NOT 2.0.x: Wire and esp32-camera's SCCB both use the new i2c_master driver (sdkconfig CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW).
// GC0308 on the CoreS3 board: parallel 8-bit DVP into the ESP32-S3 LCD_CAM peripheral, control (SCCB) on the INTERNAL I2C bus (I2C_NUM_1,
// SDA 12 / SCL 11, address 0x21, chip id register 0x00 = 0x9B: M5GFX's board detection probes exactly that), sensor rail = AXP2101
// ALDO3 3.3 V and camera enable = AW9523 P1_0, both switched on by M5.begin() before anything here runs.
// CORRECTED: no XCLK from the ESP32 (the module has its own clock): M5CoreS3 src/utility/GC0308.cpp `.pin_xclk = -1`, esp-bsp
// m5stack_core_s3.h `BSP_CAMERA_GPIO_XCLK GPIO_NUM_NC`. GPIO 2 is Port A SDA (M5Unified pin table): driving 20 MHz on it broke Port A.
constexpr int PIN_XCLK = -1;
constexpr int PIN_PCLK = 45, PIN_VSYNC = 46, PIN_HREF = 38;          // CONFIRMED: M5CoreS3 GC0308.cpp, esp-bsp BSP_CAMERA_PCLK/VSYNC/HSYNC
constexpr int PIN_D0 = 39, PIN_D1 = 40, PIN_D2 = 41, PIN_D3 = 42, PIN_D4 = 15, PIN_D5 = 16, PIN_D6 = 48, PIN_D7 = 47;   // CONFIRMED: same two sources (D0..D7 = Y2..Y9)
constexpr int XCLK_HZ = 20000000;                                    // CONFIRMED: M5CoreS3 and esp-bsp (BSP_CAMERA_XCLK_CLOCK_MHZ 20). With no XCLK pin it only sets LCD_CAM's clock divider (ll_cam.c)
constexpr bool SCCB_ON_INTERNAL_I2C = true;                          // CONFIRMED: share M5Unified's port 1 (pin_sccb_sda = -1): SCCB_Use_Port + i2c_master_get_bus_handle finds the bus Wire1 opened for M5GFX
// CORRECTED (name): AW9523 P1_0 is the camera ENABLE (esp-bsp BSP_CAMERA_EN = IO_EXPANDER_PIN_NUM_8), not a reset; there is no reset or PWDN pin
// (esp-bsp BSP_CAMERA_RST NC, M5CoreS3 pin_pwdn/pin_reset -1). M5GFX's CoreS3 power-on (board_detect/m5/pmic_ops.hpp) makes P1_0 an output
// (AW9523 reg 0x05 = 0x0C) and drives it high (reg 0x03 |= 0x03, with P1_1 = LCD_RST) during M5.begin(), and reads the camera id after that.
constexpr uint8_t AW9523_ADDR = 0x58, AW9523_OUT_P1 = 0x03, CAM_EN_BIT = 0x01;
// Pixel format of sensor + driver:
// CONFIRMED: the GC0308 sends RGB565 high byte first (Linux gc0308.c: OUT_FORMAT 6 = MEDIA_BUS_FMT_RGB565_2X8_BE) and esp32-camera stores the
// bytes as received (cam_hal.c swap_data = 0 -> ll_cam.c cam_byte_order), so a little-endian uint16_t read needs the swap.
constexpr bool CAM_SWAP_BYTES = true;
constexpr bool CAM_SWAP_RB = false;                                  // CONFIRMED: RGB565 (not BGR) per the same Linux format table. Still do the red-crayon check
constexpr bool CAM_HMIRROR = false, CAM_VFLIP = false;               // UNVERIFIED on hardware: esp-bsp uses HFLIP 0 / VFLIP 0 for this module; how it is mounted decides. Must NOT be mirrored; 180 degrees is fine
constexpr int FB_COUNT = 2;
// Two frame sizes (hardware-notes row 15): QVGA while the exposure is free (Check: the preview and the live check only need 320 x 240),
// VGA from lockExposure(true) to lockExposure(false) (the snap). CONFIRMED from the driver: a size switch needs a driver re-init.
// gc0308.c set_framesize only rewrites the sensor's window / subsample registers (CONFIG_GC_SENSOR_SUBSAMPLE_MODE=y in this build:
// QVGA = 1/2 subsample of the full 640 x 480 window, same field of view), but cam_hal.c sizes its frame buffers once in cam_config()
// and drops every frame whose length differs ("cam_hal: FB-SIZE: %u != %u", in the shipped object too). esp32-camera's own
// esp_camera_reconfigure() is deinit + init. So the switch is esp_camera_deinit + esp_camera_init (UNVERIFIED on hardware: its time).
constexpr framesize_t PREVIEW_SIZE = FRAMESIZE_QVGA, SNAP_SIZE = FRAMESIZE_VGA;
// The exposure lock across that re-init: the init resets the sensor (reg 0xfe = 0xf0 + the default table), which loses the locked AEC / AWB
// state. So the lock is taken at QVGA, these page-0 registers are read, and after the VGA init they are written back with AEC / AWB off.
// Register map: Linux gc0308.c (GC0308_EXP 0x03/0x04, GLOBAL_GAIN 0x50, AWB_R/G/B_GAIN 0x5a/0x5b/0x5c; its manual white-balance presets
// write exactly 0x5a..0x5c with AWB off). UNVERIFIED on hardware: that these are all the AEC loop sets (row 9 / 15 checks the brightness).
constexpr uint8_t KEEP_REGS[6] = {0x03, 0x04, 0x50, 0x5a, 0x5b, 0x5c};
constexpr uint32_t RETRY_FAILED_MS = 3000;
constexpr uint32_t FRAME_POLL_MS = 4;                                // how often the task looks for a new frame (esp_camera_available_frames)
static_assert(AE_LEVEL >= -2 && AE_LEVEL <= 2, "the GC0308 driver's set_ae_level rejects levels outside -2..2");
static_assert(CAMERA_SNAP_SETTLE_FRAMES >= FB_COUNT, "the settle frames must flush every queued (stale) frame buffer");

inline uint16_t fromSensor(uint16_t v) {
  if (CAM_SWAP_BYTES) v = uint16_t((v << 8) | (v >> 8));
  if (CAM_SWAP_RB) v = uint16_t(((v & 0x1F) << 11) | (v & 0x07E0) | (v >> 11));
  return v;
}

void* psramAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) heldBytes.fetch_add(int64_t(n));
  return p;
}
template <typename T> void psramFree(T*& p, size_t n) { if (p) { heap_caps_free(p); heldBytes.fetch_sub(int64_t(n)); p = nullptr; } }
inline bool after(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
}  // namespace

struct Camera::Impl {
  std::atomic<uint32_t> leaseAt{0};
  std::atomic<bool> want{false};
  std::atomic<uint8_t> state{uint8_t(Camera::State::Off)};
  std::atomic<uint32_t> frameNo{0};
  std::atomic<int> lockReq{-1};                  // -1 nothing, 0 unlock, 1 lock
  std::atomic<uint16_t*> grabDst{nullptr};
  std::atomic<uint8_t> grabState{uint8_t(Camera::Grab::Idle)};
  std::atomic<uint8_t> grabFail{uint8_t(Camera::GrabFail::None)};
  SemaphoreHandle_t mtx = nullptr;
  TaskHandle_t task = nullptr;
  bool held = false;                             // game thread: live() took the mutex, release() gives it back
  uint32_t failedAt = 0;
  // owned by the task; published under mtx
  uint16_t* buf[2] = {nullptr, nullptr};
  uint8_t* gray[2] = {nullptr, nullptr};
  int front = -1;
  bool inited = false;
  framesize_t size = PREVIEW_SIZE;               // what the driver was initialised with
  bool locked = false;                           // exposure lock wanted (and applied when inited)
  bool haveKeep = false; uint8_t keep[sizeof KEEP_REGS] = {};
  uint32_t lastFrameAt = 0;                      // the no-frames watchdog (CAMERA_NO_FRAMES_MS)

  enum class Fb : uint8_t { Got, Timeout, Interrupted };
  bool allocBuffers();
  void freeBuffers();
  camera_config_t config(framesize_t fs) const;
  bool initDriver(framesize_t fs);
  void deinitDriver();
  bool startup();
  void shutdown();
  bool switchSize(framesize_t fs);
  void applyLock(bool lock);
  void writeLockRegs();
  Fb fbGet(camera_fb_t*& fb, uint32_t deadline, bool interruptible);
  void publish(const camera_fb_t* fb);
  void doGrab();
  void fail(const char* why);
  void run();
  static void trampoline(void* self) { static_cast<Impl*>(self)->run(); }
};

bool Camera::Impl::allocBuffers() {
  for (int i = 0; i < 2; i++) {
    if (!buf[i]) buf[i] = static_cast<uint16_t*>(psramAlloc(CAMERA_FRAME_BYTES));
    if (!gray[i]) gray[i] = static_cast<uint8_t*>(psramAlloc(size_t(QVGA_W) * QVGA_H));
    if (!buf[i] || !gray[i]) { Serial.println("[camera] out of PSRAM for the live buffers"); return false; }
  }
  return true;
}
/** Only with the driver stopped (nothing publishes) or under mtx. */
void Camera::Impl::freeBuffers() {
  for (int i = 0; i < 2; i++) { psramFree(buf[i], CAMERA_FRAME_BYTES); psramFree(gray[i], size_t(QVGA_W) * QVGA_H); }
}

camera_config_t Camera::Impl::config(framesize_t fs) const {
  camera_config_t c;
  std::memset(&c, 0, sizeof c);
  c.pin_pwdn = -1; c.pin_reset = -1;                                 // CONFIRMED: no PWDN / RESET line (M5CoreS3 GC0308.cpp, esp-bsp BSP_CAMERA_RST NC)
  c.pin_xclk = PIN_XCLK;
  c.pin_sccb_sda = SCCB_ON_INTERNAL_I2C ? -1 : 12; c.pin_sccb_scl = SCCB_ON_INTERNAL_I2C ? -1 : 11;
  c.sccb_i2c_port = SCCB_ON_INTERNAL_I2C ? int(M5.In_I2C.getPort()) : 1;   // I2C_NUM_1 (M5Unified.inl: in_port = I2C_NUM_1 on the S3)
  c.pin_d0 = PIN_D0; c.pin_d1 = PIN_D1; c.pin_d2 = PIN_D2; c.pin_d3 = PIN_D3; c.pin_d4 = PIN_D4; c.pin_d5 = PIN_D5; c.pin_d6 = PIN_D6; c.pin_d7 = PIN_D7;
  c.pin_vsync = PIN_VSYNC; c.pin_href = PIN_HREF; c.pin_pclk = PIN_PCLK;
  c.xclk_freq_hz = XCLK_HZ;
  c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;     // CONFIRMED unused on the S3: LCD_CAM makes the clock, esp_camera.c defines CAMERA_ENABLE_OUT_CLOCK empty
  c.pixel_format = PIXFORMAT_RGB565;                                 // CONFIRMED: gc0308.c set_pixformat supports RGB565, YUV422, GRAYSCALE (no JPEG)
  c.frame_size = fs;                                                 // CONFIRMED: VGA is the GC0308 maximum (gc0308.c set_framesize clamps above it); QVGA = its 1/2 subsample
  c.jpeg_quality = 12;
  c.fb_count = FB_COUNT;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  // CAMERA_GRAB_WHEN_EMPTY: the driver's cam_task copies a frame into a PSRAM buffer only while one is free, so frames nobody reads are
  // not copied (with GRAB_LATEST it copies every sensor frame and replaces the queued one). The price is that up to FB_COUNT queued frames
  // can be old: the task reads every frame as it comes (the preview lags by at most one frame) and the snap throws
  // CAMERA_SNAP_SETTLE_FRAMES >= FB_COUNT away first.
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  return c;
}

/** esp_camera_init (+ one CAM_EN cycle and a retry) and the per-init sensor settings. All bus traffic under I2cGuard, delays outside it. */
bool Camera::Impl::initDriver(framesize_t fs) {
  const camera_config_t c = config(fs);
  const uint32_t t0 = millis();
  esp_err_t e;
  { I2cGuard g; e = esp_camera_init(&c); }                           // SCCB probe, soft reset, the default table: a few hundred ms holding the bus
  if (e != ESP_OK) {
    // ESP_ERR_NOT_SUPPORTED (0x106) = nothing answered at 0x21 / wrong id; ESP_ERR_INVALID_STATE / NOT_FOUND = no bus on port 1.
    Serial.printf("[camera] init failed (0x%x); cycling CAM_EN (AW9523 P1_0) and trying once more\n", unsigned(e));
    { I2cGuard g; M5.In_I2C.bitOff(AW9523_ADDR, AW9523_OUT_P1, CAM_EN_BIT, 400000); }
    vTaskDelay(pdMS_TO_TICKS(20));
    { I2cGuard g; M5.In_I2C.bitOn(AW9523_ADDR, AW9523_OUT_P1, CAM_EN_BIT, 400000); }
    vTaskDelay(pdMS_TO_TICKS(50));
    { I2cGuard g; e = esp_camera_init(&c); }
  }
  if (e != ESP_OK) { Serial.printf("[camera] init failed (0x%x)\n", unsigned(e)); return false; }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    I2cGuard g;
    if (s->set_hmirror) s->set_hmirror(s, CAM_HMIRROR ? 1 : 0);      // reg 0x14 bit 0 (gc0308.c, Linux, Allwinner agree)
    if (s->set_vflip) s->set_vflip(s, CAM_VFLIP ? 1 : 0);            // reg 0x14 bit 1
    // CONFIRMED: implemented (AEC target 0xd3 = 0x48 + 8 * level; +1 = 0x50, the Allwinner driver's EV+1 value). How much +1 helps
    // white paper is a tuning question: AE_LEVEL in shared/constants.json.
    if (s->set_ae_level) s->set_ae_level(s, AE_LEVEL);
  }
  inited = true;
  size = fs;
  lastFrameAt = millis();
  Serial.printf("[camera] init %s ok in %u ms, sensor pid 0x%02x (GC0308 = 0x9b)\n", fs == SNAP_SIZE ? "VGA" : "QVGA", unsigned(millis() - t0),
                s ? unsigned(s->id.PID) : 0u);
  if (locked) writeLockRegs();                                       // a re-init reset the sensor: put the frozen exposure back
  return true;
}

void Camera::Impl::deinitDriver() {
  if (!inited) return;
  { I2cGuard g; esp_camera_deinit(); }                               // frees the driver's frame buffers; SCCB_Deinit leaves M5's bus alone (sccb_owns_i2c_port = false)
  inited = false;
}

bool Camera::Impl::startup() {
  if (!allocBuffers()) return false;
  front = -1;
  locked = false; haveKeep = false;
  return initDriver(PREVIEW_SIZE);                                   // a pending lock is applied right after (run())
}

void Camera::Impl::shutdown() {
  deinitDriver();
  xSemaphoreTake(mtx, portMAX_DELAY);                                // wait for a reader that is still drawing the last frame
  front = -1;
  freeBuffers();
  xSemaphoreGive(mtx);
  locked = false; haveKeep = false;
  state = uint8_t(State::Off);
  Serial.printf("[camera] off (camera buffers %u bytes, free PSRAM %u)\n", unsigned(Camera::bufferBytes()), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void Camera::Impl::fail(const char* why) {
  Serial.printf("[camera] %s\n", why);
  shutdown();
  state = uint8_t(State::Failed);
  failedAt = millis();
}

/** Re-init the driver at another frame size (see PREVIEW_SIZE). The live buffers stay; the last published frame stays visible meanwhile. */
bool Camera::Impl::switchSize(framesize_t fs) {
  if (inited && size == fs) return true;
  deinitDriver();
  return initDriver(fs);
}

/** AEC and AWB off, then the kept registers back (after a re-init). Page 0 is selected by set_whitebal. */
void Camera::Impl::writeLockRegs() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  I2cGuard g;
  if (s->set_whitebal) s->set_whitebal(s, 0);
  if (s->set_reg) {
    s->set_reg(s, 0xd2, 0x80, 0x00);
    if (haveKeep) for (size_t i = 0; i < sizeof KEEP_REGS; i++) s->set_reg(s, KEEP_REGS[i], 0xff, keep[i]);
  }
}

void Camera::Impl::applyLock(bool lock) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  // CORRECTED: esp32-camera's GC0308 set_exposure_ctrl / set_gain_ctrl flip bits 0 / 2 of reg 0x22, which no other GC0308 driver uses for
  // AEC / AGC (Linux gc0308.c touches only bit 1 of 0x22; the Allwinner sunxi-vfe gc0308.c: AWB = 0x22 bit 1, auto exposure = 0xd2 bit 7,
  // auto gain "not supported": the AEC loop sets the gain too). Default 0x22 = 0x57, so clearing bits 0 / 2 would switch off some other
  // ISP block instead of freezing exposure. So: AWB with set_whitebal (0x22 bit 1, correct) and AEC (exposure + gain) with 0xd2 bit 7.
  const uint32_t t0 = millis();
  if (lock) {
    {
      I2cGuard g;
      if (s->set_whitebal) s->set_whitebal(s, 0);                    // also selects register page 0, which 0xd2 and KEEP_REGS need
      if (s->set_reg) s->set_reg(s, 0xd2, 0x80, 0x00);
      haveKeep = s->get_reg != nullptr;
      for (size_t i = 0; haveKeep && i < sizeof KEEP_REGS; i++) { const int v = s->get_reg(s, KEEP_REGS[i], 0xff); if (v < 0) haveKeep = false; else keep[i] = uint8_t(v); }
    }
    locked = true;
    if (!switchSize(SNAP_SIZE)) { fail("re-init at VGA failed"); return; }   // writes the lock back after the re-init
  } else {
    locked = false; haveKeep = false;
    if (size != PREVIEW_SIZE) { if (!switchSize(PREVIEW_SIZE)) { fail("re-init at QVGA failed"); return; } }
    if ((s = esp_camera_sensor_get()) == nullptr) return;
    I2cGuard g;                                                      // explicit, whatever the default table says
    if (s->set_whitebal) s->set_whitebal(s, 1);
    if (s->set_reg) s->set_reg(s, 0xd2, 0x80, 0x80);
  }
  Serial.printf("[camera] exposure %s, %s, %u ms\n", lock ? "locked" : "auto", size == SNAP_SIZE ? "VGA" : "QVGA", unsigned(millis() - t0));
}

/** The next frame without esp_camera_fb_get()'s own 4 s wait: poll for a queued frame until `deadline`. Interruptible = give up early
 *  (no failure) when the game wants something (lease ended, lock or snap asked). */
Camera::Impl::Fb Camera::Impl::fbGet(camera_fb_t*& fb, uint32_t deadline, bool interruptible) {
  fb = nullptr;
  for (;;) {
    if (esp_camera_available_frames() && (fb = esp_camera_fb_get()) != nullptr) { lastFrameAt = millis(); return Fb::Got; }
    if (!want.load()) return Fb::Interrupted;
    if (interruptible && (lockReq.load() >= 0 || grabState.load() == uint8_t(Grab::Busy))) return Fb::Interrupted;
    if (after(millis(), deadline)) return Fb::Timeout;
    vTaskDelay(pdMS_TO_TICKS(FRAME_POLL_MS));
  }
}

/** One pass over the sensor frame: byte swap, the 640 x 480 preview frame and the 320 x 240 gray copy (== grayQvga of that frame). */
void Camera::Impl::publish(const camera_fb_t* fb) {
  if (fb->format != PIXFORMAT_RGB565 || fb->len < fb->width * fb->height * 2) return;
  const bool qvga = fb->width == size_t(QVGA_W) && fb->height == size_t(QVGA_H);
  if (!qvga && (fb->width != size_t(CAM_W) || fb->height != size_t(CAM_H))) return;   // not what we asked for: drop it
  const int back = front < 0 ? 0 : 1 - front;                        // the reader only ever holds `front`
  const uint16_t* in = reinterpret_cast<const uint16_t*>(fb->buf);
  uint16_t* out = buf[back];
  uint8_t* gr = gray[back];
  if (qvga) {                                                        // each sensor pixel becomes a 2 x 2 block (two 32-bit stores)
    for (int y = 0; y < QVGA_H; y++) {
      const uint16_t* row = in + y * QVGA_W;
      uint32_t* o0 = reinterpret_cast<uint32_t*>(out + (2 * y) * CAM_W);
      uint32_t* o1 = reinterpret_cast<uint32_t*>(out + (2 * y + 1) * CAM_W);
      uint8_t* g = gr + y * QVGA_W;
      for (int x = 0; x < QVGA_W; x++) {
        const uint16_t p = fromSensor(row[x]);
        const uint32_t d = uint32_t(p) | (uint32_t(p) << 16);
        o0[x] = d; o1[x] = d;
        g[x] = uint8_t(gray8(p));                                    // grayQvga of four equal pixels = the pixel's gray
      }
    }
  } else {
    for (int y = 0; y < QVGA_H; y++) {
      const uint16_t* i0 = in + (2 * y) * CAM_W; const uint16_t* i1 = i0 + CAM_W;
      uint16_t* o0 = out + (2 * y) * CAM_W; uint16_t* o1 = o0 + CAM_W;
      uint8_t* g = gr + y * QVGA_W;
      for (int x = 0; x < QVGA_W; x++) {
        const uint16_t a = fromSensor(i0[2 * x]), b = fromSensor(i0[2 * x + 1]), c = fromSensor(i1[2 * x]), d = fromSensor(i1[2 * x + 1]);
        o0[2 * x] = a; o0[2 * x + 1] = b; o1[2 * x] = c; o1[2 * x + 1] = d;
        g[x] = uint8_t((gray8(a) + gray8(b) + gray8(c) + gray8(d)) >> 2);   // == grayQvga (vision.cpp)
      }
    }
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  front = back;
  frameNo.fetch_add(1);
  xSemaphoreGive(mtx);
}

/** The snap: VGA (normally already switched by lockExposure), CAMERA_SNAP_SETTLE_FRAMES thrown away, FRAME_AVG frames averaged into
 *  grabDst. All within CAMERA_GRAB_BUDGET_MS. Runs in the camera task, so the game loop keeps animating. */
void Camera::Impl::doGrab() {
  const uint32_t t0 = millis(), deadline = t0 + uint32_t(CAMERA_GRAB_BUDGET_MS);
  uint16_t* dst = grabDst.load();
  auto finish = [&](Camera::GrabFail why, const char* msg) {
    grabFail = uint8_t(why);
    grabState = uint8_t(why == Camera::GrabFail::None ? Grab::Done : Grab::Failed);
    if (msg) Serial.printf("[camera] snap failed after %u ms: %s\n", unsigned(millis() - t0), msg);
  };
  if (!dst) { finish(Camera::GrabFail::Camera, "no buffer"); return; }
  if (!switchSize(SNAP_SIZE)) { finish(Camera::GrabFail::Camera, "re-init at VGA failed"); fail("re-init at VGA failed"); return; }
  uint16_t* scratch = buf[front < 0 ? 0 : 1 - front];                // the back buffer: no reader looks at it
  camera_fb_t* fb = nullptr;
  for (int k = 0; k < CAMERA_SNAP_SETTLE_FRAMES; k++) {
    if (fbGet(fb, deadline, false) != Fb::Got) { finish(want.load() ? Camera::GrabFail::Timeout : Camera::GrabFail::Camera, "no frames (settle)"); return; }
    esp_camera_fb_return(fb);
  }
  for (int k = 0; k < FRAME_AVG; k++) {                              // FRAME_AVG is 3: frames 1 and 2 are stored, frame 3 is read straight from the driver
    if (fbGet(fb, deadline, false) != Fb::Got) { finish(want.load() ? Camera::GrabFail::Timeout : Camera::GrabFail::Camera, "no frames"); return; }
    if (fb->width != size_t(CAM_W) || fb->height != size_t(CAM_H) || fb->format != PIXFORMAT_RGB565 || fb->len < CAMERA_FRAME_BYTES) {
      esp_camera_fb_return(fb); finish(Camera::GrabFail::Camera, "not a VGA RGB565 frame"); return;
    }
    const uint16_t* in = reinterpret_cast<const uint16_t*>(fb->buf);
    if (k == 0) for (int i = 0; i < CAMERA_PIXELS; i++) scratch[i] = fromSensor(in[i]);
    else if (k == 1) for (int i = 0; i < CAMERA_PIXELS; i++) dst[i] = fromSensor(in[i]);
    else for (int i = 0; i < CAMERA_PIXELS; i++) dst[i] = avg3(scratch[i], dst[i], fromSensor(in[i]));
    esp_camera_fb_return(fb);
  }
  finish(Camera::GrabFail::None, nullptr);
  Serial.printf("[camera] snap in %u ms\n", unsigned(millis() - t0));
}

void Camera::Impl::run() {
  for (;;) {
    if (want.load() && millis() - leaseAt.load() > uint32_t(CAMERA_IDLE_MS)) want = false;   // the lease ran out (also without update())
    if (!want.load()) {
      if (inited || buf[0]) shutdown();
      if (state.load() == uint8_t(State::Starting)) state = uint8_t(State::Off);
      if (grabState.load() == uint8_t(Grab::Busy)) { grabFail = uint8_t(GrabFail::Camera); grabState = uint8_t(Grab::Failed); }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }
    if (!inited) {
      if (grabState.load() == uint8_t(Grab::Busy)) { grabFail = uint8_t(GrabFail::Camera); grabState = uint8_t(Grab::Failed); }
      if (state.load() == uint8_t(State::Failed) && millis() - failedAt < RETRY_FAILED_MS) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); continue; }
      state = uint8_t(State::Starting);
      if (!startup()) { fail("start failed"); continue; }
      state = uint8_t(State::Running);
      Serial.println("[camera] on");
    }
    const int lr = lockReq.exchange(-1);
    if (lr >= 0) { applyLock(lr == 1); if (!inited) continue; }
    if (grabState.load() == uint8_t(Grab::Busy)) { doGrab(); continue; }
    camera_fb_t* fb = nullptr;
    const Fb r = fbGet(fb, lastFrameAt + uint32_t(CAMERA_NO_FRAMES_MS), true);
    if (r == Fb::Interrupted) continue;
    if (r == Fb::Timeout) {                                          // started, but no frame for CAMERA_NO_FRAMES_MS: no sensor clock, wiring, ...
      // CORRECTED: this used to wait for one esp_camera_fb_get() timeout (FB_GET_TIMEOUT = 4 s, esp_camera.c); now the task polls
      // esp_camera_available_frames() and times the gap itself. Failed shows the "No camera found" panel; the lease retries every 3 s.
      fail("no frames for 1.5 s");
      continue;
    }
    publish(fb);
    esp_camera_fb_return(fb);
    vTaskDelay(1);                                                   // let core 0's idle task (and the watchdog) breathe
  }
}

// ---- the API ----------------------------------------------------------------------------------------------------
Camera::Camera() = default;
Camera::~Camera() { end(); }          // the task lives for the whole run: the process is simply restarted

bool Camera::begin() {
  if (!p_) {
    p_ = new Impl();
    p_->mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(Impl::trampoline, "camera", 6144, p_, 2, &p_->task, 0);   // UNVERIFIED on hardware (performance only): priority 2 on core 0 with the driver's own cam_task (CONFIG_CAMERA_CORE0) and the vision task; the game loop runs on core 1
    if (!p_->task || !p_->mtx) { Serial.println("[camera] could not create the task"); return false; }
  }
  p_->leaseAt = millis();
  if (!p_->want.exchange(true)) xTaskNotifyGive(p_->task);
  return p_->state.load() != uint8_t(State::Failed);
}
void Camera::end() { if (p_ && p_->want.exchange(false)) xTaskNotifyGive(p_->task); }
void Camera::update(uint32_t nowMs) {
  if (p_ && p_->want.load() && nowMs - p_->leaseAt.load() > uint32_t(CAMERA_IDLE_MS)) end();
}
Camera::State Camera::state() const { return p_ ? State(p_->state.load()) : State::Off; }
uint32_t Camera::frameNo() const { return p_ ? p_->frameNo.load() : 0; }

const uint16_t* Camera::live() {
  if (!p_ || p_->held) return nullptr;
  xSemaphoreTake(p_->mtx, portMAX_DELAY);
  if (p_->front < 0) { xSemaphoreGive(p_->mtx); return nullptr; }
  p_->held = true;
  return p_->buf[p_->front];
}
void Camera::release() { if (p_ && p_->held) { p_->held = false; xSemaphoreGive(p_->mtx); } }

bool Camera::liveGray(uint8_t* dst, uint32_t& lastNo) {
  if (!p_) return false;
  bool ok = false;
  xSemaphoreTake(p_->mtx, portMAX_DELAY);
  if (p_->front >= 0 && p_->frameNo.load() != lastNo) { std::memcpy(dst, p_->gray[p_->front], size_t(QVGA_W) * QVGA_H); lastNo = p_->frameNo.load(); ok = true; }
  xSemaphoreGive(p_->mtx);
  return ok;
}

bool Camera::grabStart(uint16_t* dst) {
  if (!p_ || !dst || state() != State::Running || p_->grabState.load() == uint8_t(Grab::Busy)) return false;
  p_->grabDst = dst;
  p_->grabFail = uint8_t(GrabFail::None);
  p_->grabState = uint8_t(Grab::Busy);
  xTaskNotifyGive(p_->task);
  return true;
}
Camera::Grab Camera::grabState() const { return p_ ? Grab(p_->grabState.load()) : Grab::Idle; }
Camera::GrabFail Camera::grabFail() const { return p_ ? GrabFail(p_->grabFail.load()) : GrabFail::None; }
void Camera::grabAck() { if (p_) p_->grabState = uint8_t(Grab::Idle); }
bool Camera::grab(uint16_t* dst) {
  if (!grabStart(dst)) return false;
  for (int i = 0; i < (CAMERA_GRAB_BUDGET_MS + 500) / 5 && grabState() == Grab::Busy; i++) vTaskDelay(pdMS_TO_TICKS(5));
  const bool ok = grabState() == Grab::Done;
  grabAck();
  return ok;
}
void Camera::lockExposure(bool lock) { if (p_ && p_->task) { p_->lockReq = lock ? 1 : 0; xTaskNotifyGive(p_->task); } }

}  // namespace scribble


// =====================================================================================================================
#else   // EMU: test photos instead of a sensor
// =====================================================================================================================
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "vision_golden.h"          // shared/generated: the names and transforms of the golden cases

namespace scribble {
namespace {

// ---- the stored golden photos: decoder mirrors test/test_vision (and decodeFrame in sim/scripts/vision-golden-lib.ts) ----------------
constexpr int N_GOLDEN_FRAMES = 4;
std::vector<uint16_t> goldenFrames[N_GOLDEN_FRAMES];
bool goldenLoaded = false;

uint8_t* readAll(const char* name, size_t& n) {
  std::vector<std::string> dirs;
  if (const char* d = std::getenv("EMU_GOLDEN_DIR")) dirs.push_back(std::string(d) + "/");
  const std::string here = __FILE__;                                 // .../firmware/src/app/camera.cpp
  const size_t k = here.rfind("/firmware/");
  if (k != std::string::npos) dirs.push_back(here.substr(0, k) + "/shared/golden/");
  for (const char* pre : {"../shared/golden/", "shared/golden/", "../../shared/golden/"}) dirs.push_back(pre);
  for (const std::string& d : dirs) {
    FILE* f = std::fopen((d + name).c_str(), "rb");
    if (!f) continue;
    std::fseek(f, 0, SEEK_END); n = size_t(std::ftell(f)); std::fseek(f, 0, SEEK_SET);
    uint8_t* b = static_cast<uint8_t*>(std::malloc(n));
    if (std::fread(b, 1, n, f) != n) { std::fclose(f); std::free(b); return nullptr; }
    std::fclose(f);
    return b;
  }
  return nullptr;
}
inline uint16_t predict(const uint16_t* f, int i) { return i >= CAM_W ? f[i - CAM_W] : i > 0 ? f[i - 1] : 0; }
void decodeFrame(const uint8_t* b, size_t pos, size_t end, uint16_t* f) {
  const int total = CAMERA_PIXELS; int i = 0;
  auto vi = [&]() { uint32_t v = 0; int sh = 0; uint8_t c; do { c = b[pos++]; v |= uint32_t(c & 127) << sh; sh += 7; } while (c & 128); return v; };
  for (;;) {
    const uint32_t run = vi(); for (uint32_t k = 0; k < run && i < total; k++, i++) f[i] = predict(f, i);
    if (pos >= end) break;
    const uint32_t z = vi(); if (i < total) { const int d = (z & 1) ? -int((z + 1) >> 1) : int(z >> 1); f[i] = uint16_t((predict(f, i) + d) & 0xffff); i++; }
  }
  while (i < total) { f[i] = predict(f, i); i++; }
}
bool loadGolden() {
  if (goldenLoaded) return true;
  size_t n = 0; uint8_t* b = readAll("vision_frames.bin", n);
  if (!b || n < 8 || std::memcmp(b, "VGF1", 4) != 0) { std::fprintf(stderr, "[camera] cannot read shared/golden/vision_frames.bin (set EMU_GOLDEN_DIR)\n"); std::free(b); return false; }
  uint32_t count; std::memcpy(&count, b + 4, 4);
  size_t pos = 8;
  for (int k = 0; k < N_GOLDEN_FRAMES && k < int(count); k++) {
    uint32_t len; std::memcpy(&len, b + pos, 4);
    goldenFrames[k].assign(CAMERA_PIXELS, 0); decodeFrame(b, pos + 4, pos + 4 + len, goldenFrames[k].data()); pos += 4 + len;
  }
  std::free(b); goldenLoaded = true; return true;
}

inline int c255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/** The case's photo (mirror of makePhoto in test_vision.cpp: the same integer transforms of a stored frame). */
void makeCase(const vgolden::Case& c, uint16_t* photo) {
  const uint16_t* in = goldenFrames[c.frame].data(); const int total = CAMERA_PIXELS;
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

/** A photo file: a P6 PPM of any size (cover-fitted into 640 x 480) or a raw 640 x 480 RGB565 dump (little endian). */
bool loadFile(const char* path, uint16_t* out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) { std::fprintf(stderr, "[camera] cannot open EMU_PHOTO=%s\n", path); return false; }
  std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> b(size_t(n > 0 ? n : 0));
  const bool rd = n > 0 && std::fread(b.data(), 1, b.size(), f) == b.size();
  std::fclose(f);
  if (!rd) return false;
  if (b.size() == CAMERA_FRAME_BYTES) { std::memcpy(out, b.data(), CAMERA_FRAME_BYTES); return true; }
  if (b.size() < 10 || b[0] != 'P' || b[1] != '6') { std::fprintf(stderr, "[camera] EMU_PHOTO must be a P6 PPM or a raw 640x480 RGB565 file\n"); return false; }
  size_t pos = 2; int vals[3] = {0, 0, 0};
  for (int k = 0; k < 3; k++) {
    while (pos < b.size() && (std::isspace(b[pos]) || b[pos] == '#')) { if (b[pos] == '#') while (pos < b.size() && b[pos] != '\n') pos++; else pos++; }
    while (pos < b.size() && std::isdigit(b[pos])) vals[k] = vals[k] * 10 + (b[pos++] - '0');
  }
  pos++;                                                              // the single whitespace after maxval
  const int w = vals[0], h = vals[1];
  if (w <= 0 || h <= 0 || vals[2] != 255 || pos + size_t(w) * h * 3 > b.size()) { std::fprintf(stderr, "[camera] unsupported PPM\n"); return false; }
  const double sc = std::max(double(CAM_W) / w, double(CAM_H) / h), dw = w * sc, dh = h * sc;
  for (int y = 0; y < CAM_H; y++) for (int x = 0; x < CAM_W; x++) {
    const int sx = std::min(w - 1, std::max(0, int((x + 0.5 - (CAM_W - dw) / 2) / sc))), sy = std::min(h - 1, std::max(0, int((y + 0.5 - (CAM_H - dh) / 2) / sc)));
    const uint8_t* px = &b[pos + (size_t(sy) * w + sx) * 3];
    out[y * CAM_W + x] = pack565(px[0], px[1], px[2]);
  }
  return true;
}

/** Defocus for the first moment of the live view: a separable box blur of every channel. */
void blurred(const uint16_t* in, uint16_t* out, int R) {
  std::vector<uint8_t> ch[3];
  for (auto& c : ch) c.resize(CAMERA_PIXELS);
  for (int i = 0; i < CAMERA_PIXELS; i++) { ch[0][i] = uint8_t(r8(in[i])); ch[1][i] = uint8_t(g8(in[i])); ch[2][i] = uint8_t(b8(in[i])); }
  std::vector<int> tmp(CAMERA_PIXELS);
  for (auto& c : ch) {
    for (int y = 0; y < CAM_H; y++) for (int x = 0; x < CAM_W; x++) { int s = 0; for (int d = -R; d <= R; d++) s += c[y * CAM_W + std::min(CAM_W - 1, std::max(0, x + d))]; tmp[y * CAM_W + x] = s; }
    for (int y = 0; y < CAM_H; y++) for (int x = 0; x < CAM_W; x++) { int s = 0; for (int d = -R; d <= R; d++) s += tmp[std::min(CAM_H - 1, std::max(0, y + d)) * CAM_W + x]; c[y * CAM_W + x] = uint8_t(s / ((2 * R + 1) * (2 * R + 1))); }
  }
  for (int i = 0; i < CAMERA_PIXELS; i++) out[i] = pack565(ch[0][i], ch[1][i], ch[2][i]);
}

int envInt(const char* n, int def) { const char* v = std::getenv(n); return v && *v ? std::atoi(v) : def; }

/** A frame-sized buffer counted in Camera::bufferBytes() (the device's live buffers are counted the same way): freed at end(). */
struct FrameBuf {
  uint16_t* p = nullptr;
  void alloc() { if (!p) { p = static_cast<uint16_t*>(std::calloc(CAMERA_PIXELS, sizeof(uint16_t))); if (p) heldBytes.fetch_add(int64_t(CAMERA_FRAME_BYTES)); } }
  void free() { if (p) { std::free(p); p = nullptr; heldBytes.fetch_sub(int64_t(CAMERA_FRAME_BYTES)); } }
  uint16_t* data() const { return p; }
};
}  // namespace

struct Camera::Impl {
  bool leased = false, on = false, announcedOn = false; uint32_t leaseAt = 0, onAt = 0, lastGen = 0;
  std::string want, loaded; bool loadOk = false;
  FrameBuf base, soft, live; uint32_t frameNo = 0, noiseSeed = 12345;    // base / soft = the "sensor" (the photo), live = the published frame
  Grab grab = Grab::Idle; uint32_t grabDoneAt = 0, grabAt = 0; GrabFail fail = GrabFail::None; bool grabStalls = false;
  bool held = false, vga = false; mutable bool saidNoFrames = false;
  int startMs = envInt("EMU_START_MS", 150), settleMs = envInt("EMU_SETTLE_MS", 900), grabMs = envInt("EMU_GRAB_MS", 200), noiseAmp = envInt("EMU_NOISE", 2);
  bool failing = envInt("EMU_CAMERA_FAIL", 0) != 0;          // pretend the sensor does not answer (the "No camera found" panel)
  int stall = envInt("EMU_CAMERA_STALL", 0);                  // 1 = every snap times out (live view fine), 2 = starts but never a frame

  /** Which photo is wanted now: the script command `photo`, else EMU_PHOTO_CASE, else "file" if EMU_PHOTO is set, else "clean". */
  std::string desired() const {
    if (const char* s = emu::photoName(); s && *s) return s;
    if (const char* e = std::getenv("EMU_PHOTO_CASE")) if (*e) return e;
    if (const char* f = std::getenv("EMU_PHOTO")) if (*f) return "file";
    return "clean";
  }
  bool ensureCase() {
    const std::string d = want.empty() ? desired() : want;
    if (d == loaded && loadOk) return true;
    if (d == loaded && !loadOk) return false;
    loaded = d; loadOk = false;
    base.alloc(); soft.alloc(); live.alloc();
    if (!base.p || !soft.p || !live.p) { std::fprintf(stderr, "[camera] out of memory\n"); return false; }
    std::memset(base.p, 0, CAMERA_FRAME_BYTES);
    if (d == "file") { const char* f = std::getenv("EMU_PHOTO"); if (!f || !loadFile(f, base.data())) return false; }
    else {
      if (!loadGolden()) return false;
      const vgolden::Case* c = nullptr;
      for (int k = 0; k < vgolden::nCases; k++) if (d == vgolden::cases[k].name) c = &vgolden::cases[k];
      if (!c) { std::fprintf(stderr, "[camera] unknown photo case '%s' (%s)\n", d.c_str(), Camera::caseNames()); return false; }
      makeCase(*c, base.data());
    }
    blurred(base.data(), soft.data(), 3);
    loadOk = true; frameNo = 0; lastGen = 0;
    std::printf("[camera photo %s]\n", d.c_str()); std::fflush(stdout);
    return true;
  }
  /** Camera off: every buffer goes (the device frees its live buffers and the driver's in shutdown()). */
  void freeAll() { base.free(); soft.free(); live.free(); loaded.clear(); loadOk = false; frameNo = 0; }
  Camera::State state() const {
    if (!on) return State::Off;
    if (failing) return State::Failed;
    const uint32_t up = millis() - onAt;
    if (up < uint32_t(startMs)) return State::Starting;
    if (stall == 2 && up >= uint32_t(startMs + CAMERA_NO_FRAMES_MS)) {   // the device's no-frames watchdog
      if (!saidNoFrames) { saidNoFrames = true; std::printf("[camera no frames for %d ms: failed]\n", CAMERA_NO_FRAMES_MS); std::fflush(stdout); }
      return State::Failed;
    }
    return State::Running;
  }
  /** A new live frame at ~15 Hz: the soft picture for the first settleMs, then the sharp one; a little noise on every frame. */
  void pump() {
    if (state() != State::Running || stall == 2 || !ensureCase()) return;
    const uint32_t now = millis();
    if (frameNo > 0 && now - lastGen < 66) return;
    lastGen = now; frameNo++;
    if (frameNo == 1) { std::printf("[camera buffers %u bytes]\n", unsigned(Camera::bufferBytes())); std::fflush(stdout); }
    const bool sharp = now - onAt >= uint32_t(startMs + settleMs);
    const uint16_t* src = sharp ? base.data() : soft.data();
    if (noiseAmp <= 0) { std::memcpy(live.data(), src, CAMERA_FRAME_BYTES); return; }
    uint32_t s = noiseSeed ^ (frameNo * 2654435761u);
    for (int i = 0; i < CAMERA_PIXELS; i++) {
      s = s * 1664525u + 1013904223u;
      const int n = int((s >> 24) * uint32_t(2 * noiseAmp + 1) >> 8) - noiseAmp;
      const unsigned p = src[i];
      live.p[i] = pack565(c255(r8(p) + n), c255(g8(p) + n), c255(b8(p) + n));
    }
  }
  /** The device runs the sensor at QVGA in the check and at VGA from the exposure lock to the unlock (a driver re-init each way). */
  void setVga(bool v) { if (v != vga) { vga = v; std::printf("[camera size %s]\n", v ? "vga" : "qvga"); std::fflush(stdout); } }
};

Camera::Camera() = default;
Camera::~Camera() { if (p_) p_->freeAll(); delete p_; }
const char* Camera::caseNames() { return "clean tilt dim dark faded noisy harsh upside-down tilt-noisy nomarks blank file"; }
void Camera::setCase(const char* name) { if (!p_) p_ = new Impl(); p_->want = name ? name : ""; }

bool Camera::begin() {
  if (!p_) p_ = new Impl();
  Impl& m = *p_;
  m.leaseAt = millis(); m.leased = true;
  if (!m.on) { m.on = true; m.onAt = millis(); m.announcedOn = false; m.saidNoFrames = false; std::printf(m.failing ? "[camera on: failed]\n" : "[camera on]\n"); std::fflush(stdout); }
  return m.state() != State::Failed;
}
void Camera::end() {
  if (p_ && p_->on) {
    p_->on = false; p_->leased = false; p_->grab = Grab::Idle; p_->vga = false;
    p_->freeAll();
    std::printf("[camera off]\n[camera buffers after off %u bytes]\n", unsigned(Camera::bufferBytes())); std::fflush(stdout);
  }
}
void Camera::update(uint32_t nowMs) {
  if (p_ && p_->on && nowMs - p_->leaseAt > uint32_t(CAMERA_IDLE_MS)) end();
}
Camera::State Camera::state() const { return p_ ? p_->state() : State::Off; }
uint32_t Camera::frameNo() const { return p_ ? p_->frameNo : 0; }

const uint16_t* Camera::live() {
  if (!p_) return nullptr;
  p_->pump();
  if (p_->frameNo == 0 || !p_->live.p || p_->state() != State::Running) return nullptr;
  return p_->live.data();
}
void Camera::release() {}

bool Camera::liveGray(uint8_t* dst, uint32_t& lastNo) {
  if (!live() || p_->frameNo == lastNo) return false;
  lastNo = p_->frameNo;
  grayQvga(p_->live.data(), dst);
  return true;
}

bool Camera::grabStart(uint16_t* dst) {
  if (!p_ || !dst || state() != State::Running || p_->grab == Grab::Busy || !p_->ensureCase()) return false;
  p_->setVga(true);
  p_->fail = GrabFail::None; p_->grabAt = millis();
  p_->grabStalls = p_->stall == 1;
  if (p_->grabStalls) p_->grabDoneAt = p_->grabAt + uint32_t(CAMERA_GRAB_BUDGET_MS);   // no frame ever comes: the budget runs out
  else {
    std::memcpy(dst, p_->base.data(), CAMERA_FRAME_BYTES);           // the three frames of the real camera are identical here: the average is the photo itself
    p_->grabDoneAt = p_->grabAt + uint32_t(p_->grabMs);
  }
  p_->grab = Grab::Busy;
  return true;
}
Camera::Grab Camera::grabState() const {
  if (!p_) return Grab::Idle;
  if (p_->grab == Grab::Busy && int32_t(millis() - p_->grabDoneAt) >= 0) {
    if (p_->grabStalls) {
      p_->grab = Grab::Failed; p_->fail = GrabFail::Timeout;
      std::printf("[camera snap timed out after %u ms]\n", unsigned(millis() - p_->grabAt)); std::fflush(stdout);
    } else p_->grab = Grab::Done;
  }
  return p_->grab;
}
Camera::GrabFail Camera::grabFail() const { return p_ ? p_->fail : GrabFail::None; }
void Camera::grabAck() { if (p_) p_->grab = Grab::Idle; }
bool Camera::grab(uint16_t* dst) {
  if (!grabStart(dst)) return false;
  if (p_->grabStalls) { p_->grab = Grab::Idle; p_->fail = GrabFail::Timeout; return false; }
  p_->grab = Grab::Done; grabAck(); return true;
}
void Camera::lockExposure(bool lock) {
  std::printf("[camera exposure %s]\n", lock ? "locked" : "auto"); std::fflush(stdout);
  if (p_ && p_->on) p_->setVga(lock);
}

}  // namespace scribble
#endif
