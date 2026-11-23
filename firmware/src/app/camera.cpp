// camera.cpp - the camera.   Device: esp32-camera (GC0308) in a task on core 0.   Emulator: test photos.   See camera.h.
// OWNER: Paper agent.
#include "camera.h"
#include <M5Unified.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "vision.h"

namespace scribble {
namespace {
inline int r8(unsigned p) { return int(((p >> 11) << 3) | (p >> 13)); }
inline int g8(unsigned p) { return int((((p >> 5) & 63) << 2) | ((p >> 9) & 3)); }
inline int b8(unsigned p) { return int(((p & 31) << 3) | ((p >> 2) & 7)); }
/** The snap's average of three frames, per channel in 8 bits, rounded like the simulator's Math.round(sum / 3). */
inline uint16_t avg3(unsigned a, unsigned b, unsigned c) {
  return pack565((2 * (r8(a) + r8(b) + r8(c)) + 3) / 6, (2 * (g8(a) + g8(b) + g8(c)) + 3) / 6, (2 * (b8(a) + b8(b) + b8(c)) + 3) / 6);
}
}  // namespace
}  // namespace scribble

// =====================================================================================================================
#ifndef EMU
// =====================================================================================================================
#include <atomic>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace scribble {
namespace {

// ---- the CoreS3 camera: facts and guesses. EVERYTHING marked UNVERIFIED must be checked on a real device (docs/hardware-notes.md, "Camera bring-up").
// GC0308 on the CoreS3 board: parallel 8-bit DVP into the ESP32-S3 LCD_CAM peripheral, control (SCCB) on the INTERNAL I2C bus (GPIO 11/12,
// address 0x21, chip id register 0x00 = 0x9B: M5GFX's board detection probes exactly that), sensor rail = AXP2101 ALDO3 3.3 V, which
// M5.begin() (M5Unified Power_Class) already switches on. CAM_RST is AW9523 P1_0.
constexpr int PIN_XCLK = 2;                                          // UNVERIFIED on hardware (M5Stack CoreS3 camera example values)
constexpr int PIN_PCLK = 45, PIN_VSYNC = 46, PIN_HREF = 38;          // UNVERIFIED on hardware
constexpr int PIN_D0 = 39, PIN_D1 = 40, PIN_D2 = 41, PIN_D3 = 42, PIN_D4 = 15, PIN_D5 = 16, PIN_D6 = 48, PIN_D7 = 47;   // UNVERIFIED on hardware
constexpr int XCLK_HZ = 20000000;                                    // UNVERIFIED on hardware (GC0308: 10..27 MHz)
constexpr bool SCCB_ON_INTERNAL_I2C = true;                          // UNVERIFIED on hardware: share M5Unified's I2C port (pin_sccb_sda = -1) instead of opening a second one on GPIO 11/12
constexpr uint8_t AW9523_ADDR = 0x58, AW9523_OUT_P1 = 0x03, CAM_RST_BIT = 0x01;   // UNVERIFIED on hardware: CAM_RST = P1_0; M5GFX uses P1_1 = LCD_RST on the same register
// Pixel format quirks of the sensor + driver. All three are decided by looking at a live picture (hardware-notes.md):
constexpr bool CAM_SWAP_BYTES = true;                                // UNVERIFIED on hardware: esp32-camera hands RGB565 as big-endian byte pairs; false if the preview looks like noise
constexpr bool CAM_SWAP_RB = false;                                  // UNVERIFIED on hardware: true if a red crayon looks blue in the preview (red/blue would swap the spike/bouncy layers!)
constexpr bool CAM_HMIRROR = false, CAM_VFLIP = false;               // UNVERIFIED on hardware: the page must NOT be mirrored (a mirrored page cannot be straightened); a 180 degree turn is fine (the ring decides)
constexpr int FB_COUNT = 2;
constexpr uint32_t RETRY_FAILED_MS = 3000;

inline uint16_t fromSensor(uint16_t v) {
  if (CAM_SWAP_BYTES) v = uint16_t((v << 8) | (v >> 8));
  if (CAM_SWAP_RB) v = uint16_t(((v & 0x1F) << 11) | (v & 0x07E0) | (v >> 11));
  return v;
}

void* psram(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
}  // namespace

struct Camera::Impl {
  std::atomic<uint32_t> leaseAt{0};
  std::atomic<bool> want{false};
  std::atomic<uint8_t> state{uint8_t(Camera::State::Off)};
  std::atomic<uint32_t> frameNo{0};
  std::atomic<int> lockReq{-1};                  // -1 nothing, 0 unlock, 1 lock
  std::atomic<uint16_t*> grabDst{nullptr};
  std::atomic<uint8_t> grabState{uint8_t(Camera::Grab::Idle)};
  SemaphoreHandle_t mtx = nullptr;
  TaskHandle_t task = nullptr;
  bool held = false;                             // game thread: live() took the mutex, release() gives it back
  uint32_t failedAt = 0;
  // owned by the task; published under mtx
  uint16_t* buf[2] = {nullptr, nullptr};
  uint8_t* gray[2] = {nullptr, nullptr};
  int front = -1;
  bool inited = false;

  bool startup();
  void shutdown();
  void publish(const camera_fb_t* fb);
  void doGrab();
  void applyLock(bool lock);
  void run();
  static void trampoline(void* self) { static_cast<Impl*>(self)->run(); }
};

bool Camera::Impl::startup() {
  for (int i = 0; i < 2; i++) {
    if (!buf[i]) buf[i] = static_cast<uint16_t*>(psram(CAMERA_FRAME_BYTES));
    if (!gray[i]) gray[i] = static_cast<uint8_t*>(psram(size_t(QVGA_W) * QVGA_H));
    if (!buf[i] || !gray[i]) { Serial.println("[camera] out of PSRAM for the live buffers"); return false; }
  }
  camera_config_t c;
  std::memset(&c, 0, sizeof c);
  c.pin_pwdn = -1; c.pin_reset = -1;                                 // UNVERIFIED on hardware: no PWDN; CAM_RST is behind the AW9523 (pulsed below if the sensor does not answer)
  c.pin_xclk = PIN_XCLK;
  c.pin_sccb_sda = SCCB_ON_INTERNAL_I2C ? -1 : 12; c.pin_sccb_scl = SCCB_ON_INTERNAL_I2C ? -1 : 11;
  c.sccb_i2c_port = SCCB_ON_INTERNAL_I2C ? int(M5.In_I2C.getPort()) : 1;
  c.pin_d0 = PIN_D0; c.pin_d1 = PIN_D1; c.pin_d2 = PIN_D2; c.pin_d3 = PIN_D3; c.pin_d4 = PIN_D4; c.pin_d5 = PIN_D5; c.pin_d6 = PIN_D6; c.pin_d7 = PIN_D7;
  c.pin_vsync = PIN_VSYNC; c.pin_href = PIN_HREF; c.pin_pclk = PIN_PCLK;
  c.xclk_freq_hz = XCLK_HZ;
  c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;     // UNVERIFIED on hardware: nobody else in this firmware uses LEDC
  c.pixel_format = PIXFORMAT_RGB565;
  c.frame_size = FRAMESIZE_VGA;                                      // 640 x 480: the vision core's input (docs/system-design.md 5.2)
  c.jpeg_quality = 12;
  c.fb_count = FB_COUNT;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;                                  // always the newest frame: the preview never lags behind
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) {
    Serial.printf("[camera] init failed (0x%x); pulsing CAM_RST and trying once more\n", unsigned(e));
    M5.In_I2C.bitOff(AW9523_ADDR, AW9523_OUT_P1, CAM_RST_BIT, 400000);   // UNVERIFIED on hardware
    vTaskDelay(pdMS_TO_TICKS(20));
    M5.In_I2C.bitOn(AW9523_ADDR, AW9523_OUT_P1, CAM_RST_BIT, 400000);
    vTaskDelay(pdMS_TO_TICKS(50));
    e = esp_camera_init(&c);
  }
  if (e != ESP_OK) { Serial.printf("[camera] init failed (0x%x)\n", unsigned(e)); return false; }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    Serial.printf("[camera] sensor pid 0x%02x (GC0308 = 0x9b)\n", unsigned(s->id.PID));
    if (s->set_hmirror) s->set_hmirror(s, CAM_HMIRROR ? 1 : 0);
    if (s->set_vflip) s->set_vflip(s, CAM_VFLIP ? 1 : 0);
    if (s->set_ae_level) s->set_ae_level(s, AE_LEVEL);              // UNVERIFIED on hardware: white paper fools auto-exposure into grey; tune AE_LEVEL (system-design 5.5)
  }
  front = -1;
  inited = true;
  return true;
}

void Camera::Impl::shutdown() {
  if (!inited) return;
  esp_camera_deinit();                                               // stops XCLK and frees the driver's frame buffers
  inited = false;
  xSemaphoreTake(mtx, portMAX_DELAY);                                // wait for a reader that is still drawing the last frame
  front = -1;
  for (int i = 0; i < 2; i++) { heap_caps_free(buf[i]); buf[i] = nullptr; heap_caps_free(gray[i]); gray[i] = nullptr; }
  xSemaphoreGive(mtx);
  state = uint8_t(State::Off);
  Serial.println("[camera] off");
}

void Camera::Impl::publish(const camera_fb_t* fb) {
  if (fb->width != size_t(CAM_W) || fb->height != size_t(CAM_H) || fb->format != PIXFORMAT_RGB565) return;   // not what we asked for: drop it
  const int back = front < 0 ? 0 : 1 - front;                        // the reader only ever holds `front`
  const uint16_t* in = reinterpret_cast<const uint16_t*>(fb->buf);
  uint16_t* out = buf[back];
  for (int i = 0; i < CAMERA_PIXELS; i++) out[i] = fromSensor(in[i]);
  grayQvga(out, gray[back]);
  xSemaphoreTake(mtx, portMAX_DELAY);
  front = back;
  frameNo.fetch_add(1);
  xSemaphoreGive(mtx);
}

/** Three frames averaged into grabDst. Runs in the camera task, so the game loop keeps animating. */
void Camera::Impl::doGrab() {
  uint16_t* dst = grabDst.load();
  uint16_t* scratch = buf[front < 0 ? 0 : 1 - front];                // the back buffer: no reader looks at it
  bool ok = dst != nullptr;
  camera_fb_t* fb = nullptr;
  for (int k = 0; ok && k < FRAME_AVG; k++) {                        // FRAME_AVG is 3: frames 1 and 2 are stored, frame 3 is read straight from the driver
    fb = esp_camera_fb_get();
    if (!fb || fb->width != size_t(CAM_W) || fb->height != size_t(CAM_H) || fb->format != PIXFORMAT_RGB565) { if (fb) esp_camera_fb_return(fb); ok = false; break; }
    const uint16_t* in = reinterpret_cast<const uint16_t*>(fb->buf);
    if (k == 0) for (int i = 0; i < CAMERA_PIXELS; i++) scratch[i] = fromSensor(in[i]);
    else if (k == 1) for (int i = 0; i < CAMERA_PIXELS; i++) dst[i] = fromSensor(in[i]);
    else for (int i = 0; i < CAMERA_PIXELS; i++) dst[i] = avg3(scratch[i], dst[i], fromSensor(in[i]));
    esp_camera_fb_return(fb);
  }
  grabState = uint8_t(ok ? Grab::Done : Grab::Failed);
}

void Camera::Impl::applyLock(bool lock) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;
  const int on = lock ? 0 : 1;                                       // UNVERIFIED on hardware: whether the GC0308 driver implements all three (the null checks keep it safe)
  if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, on);
  if (s->set_whitebal) s->set_whitebal(s, on);
  if (s->set_gain_ctrl) s->set_gain_ctrl(s, on);
}

void Camera::Impl::run() {
  int misses = 0;
  for (;;) {
    if (!want.load()) {
      if (inited) shutdown();
      if (state.load() == uint8_t(State::Starting)) state = uint8_t(State::Off);
      if (grabState.load() == uint8_t(Grab::Busy)) grabState = uint8_t(Grab::Failed);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }
    if (!inited) {
      if (state.load() == uint8_t(State::Failed) && millis() - failedAt < RETRY_FAILED_MS) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); continue; }
      state = uint8_t(State::Starting);
      if (!startup()) { state = uint8_t(State::Failed); failedAt = millis(); if (inited) shutdown(); else { for (int i = 0; i < 2; i++) { heap_caps_free(buf[i]); buf[i] = nullptr; heap_caps_free(gray[i]); gray[i] = nullptr; } } continue; }
      state = uint8_t(State::Running);
      Serial.println("[camera] on");
      misses = 0;
    }
    const int lr = lockReq.exchange(-1);
    if (lr >= 0) applyLock(lr == 1);
    if (grabState.load() == uint8_t(Grab::Busy)) { doGrab(); continue; }
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {                                                       // the driver timed out: no sensor clock, wiring, ...
      if (++misses > 20) { Serial.println("[camera] no frames"); shutdown(); state = uint8_t(State::Failed); failedAt = millis(); }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    misses = 0;
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
    xTaskCreatePinnedToCore(Impl::trampoline, "camera", 6144, p_, 2, &p_->task, 0);   // UNVERIFIED on hardware: priority 2 on core 0 (the game loop runs on core 1)
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
  p_->grabState = uint8_t(Grab::Busy);
  xTaskNotifyGive(p_->task);
  return true;
}
Camera::Grab Camera::grabState() const { return p_ ? Grab(p_->grabState.load()) : Grab::Idle; }
void Camera::grabAck() { if (p_) p_->grabState = uint8_t(Grab::Idle); }
bool Camera::grab(uint16_t* dst) {
  if (!grabStart(dst)) return false;
  for (int i = 0; i < 400 && grabState() == Grab::Busy; i++) vTaskDelay(pdMS_TO_TICKS(5));
  const bool ok = grabState() == Grab::Done;
  grabAck();
  return ok;
}
void Camera::lockExposure(bool lock) { if (p_) p_->lockReq = lock ? 1 : 0; }

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
}  // namespace

struct Camera::Impl {
  bool leased = false, on = false, announcedOn = false; uint32_t leaseAt = 0, onAt = 0, lastGen = 0;
  std::string want, loaded; bool loadOk = false;
  std::vector<uint16_t> base, soft, live; uint32_t frameNo = 0, noiseSeed = 12345;
  Grab grab = Grab::Idle; uint32_t grabDoneAt = 0; bool grabOk = false;
  bool held = false;
  int startMs = envInt("EMU_START_MS", 150), settleMs = envInt("EMU_SETTLE_MS", 900), grabMs = envInt("EMU_GRAB_MS", 200), noiseAmp = envInt("EMU_NOISE", 2);
  bool failing = envInt("EMU_CAMERA_FAIL", 0) != 0;          // pretend the sensor does not answer (the "No camera found" panel)

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
    base.assign(CAMERA_PIXELS, 0); soft.assign(CAMERA_PIXELS, 0); live.assign(CAMERA_PIXELS, 0);
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
  Camera::State state() const {
    if (!on) return State::Off;
    if (failing) return State::Failed;
    return millis() - onAt < uint32_t(startMs) ? State::Starting : State::Running;
  }
  /** A new live frame at ~15 Hz: the soft picture for the first settleMs, then the sharp one; a little noise on every frame. */
  void pump() {
    if (state() != State::Running || !ensureCase()) return;
    const uint32_t now = millis();
    if (frameNo > 0 && now - lastGen < 66) return;
    lastGen = now; frameNo++;
    const bool sharp = now - onAt >= uint32_t(startMs + settleMs);
    const uint16_t* src = sharp ? base.data() : soft.data();
    if (noiseAmp <= 0) { std::memcpy(live.data(), src, CAMERA_FRAME_BYTES); return; }
    uint32_t s = noiseSeed ^ (frameNo * 2654435761u);
    for (int i = 0; i < CAMERA_PIXELS; i++) {
      s = s * 1664525u + 1013904223u;
      const int n = int((s >> 24) * uint32_t(2 * noiseAmp + 1) >> 8) - noiseAmp;
      const unsigned p = src[i];
      live[i] = pack565(c255(r8(p) + n), c255(g8(p) + n), c255(b8(p) + n));
    }
  }
};

Camera::Camera() = default;
Camera::~Camera() { delete p_; }
const char* Camera::caseNames() { return "clean tilt dim dark faded noisy harsh upside-down tilt-noisy nomarks blank file"; }
void Camera::setCase(const char* name) { if (!p_) p_ = new Impl(); p_->want = name ? name : ""; }

bool Camera::begin() {
  if (!p_) p_ = new Impl();
  Impl& m = *p_;
  m.leaseAt = millis(); m.leased = true;
  if (!m.on) { m.on = true; m.onAt = millis(); m.announcedOn = false; std::printf(m.failing ? "[camera on: failed]\n" : "[camera on]\n"); std::fflush(stdout); }
  return !m.failing;
}
void Camera::end() {
  if (p_ && p_->on) { p_->on = false; p_->leased = false; p_->grab = Grab::Idle; std::printf("[camera off]\n"); std::fflush(stdout); }
}
void Camera::update(uint32_t nowMs) {
  if (p_ && p_->on && nowMs - p_->leaseAt > uint32_t(CAMERA_IDLE_MS)) end();
}
Camera::State Camera::state() const { return p_ ? p_->state() : State::Off; }
uint32_t Camera::frameNo() const { return p_ ? p_->frameNo : 0; }

const uint16_t* Camera::live() {
  if (!p_) return nullptr;
  p_->pump();
  if (p_->frameNo == 0 || p_->live.empty() || p_->state() != State::Running) return nullptr;
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
  if (!p_ || !dst || state() != State::Running || !p_->ensureCase()) return false;
  std::memcpy(dst, p_->base.data(), CAMERA_FRAME_BYTES);             // the three frames of the real camera are identical here: the average is the photo itself
  p_->grab = Grab::Busy; p_->grabDoneAt = millis() + uint32_t(p_->grabMs);
  return true;
}
Camera::Grab Camera::grabState() const {
  if (!p_) return Grab::Idle;
  if (p_->grab == Grab::Busy && millis() >= p_->grabDoneAt) p_->grab = Grab::Done;
  return p_->grab;
}
void Camera::grabAck() { if (p_) p_->grab = Grab::Idle; }
bool Camera::grab(uint16_t* dst) { if (!grabStart(dst)) return false; if (p_) p_->grab = Grab::Done; grabAck(); return true; }
void Camera::lockExposure(bool lock) { std::printf("[camera exposure %s]\n", lock ? "locked" : "auto"); std::fflush(stdout); }

}  // namespace scribble
#endif
