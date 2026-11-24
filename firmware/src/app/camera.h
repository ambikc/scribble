// camera.h - the camera: frames for the paper flow.   (platform adapter: esp32-camera on the device, test photos in the emulator)
//
//   begin()      "I need the camera": a lease that must be renewed (every tick of a paper screen calls it). The first call powers
//                the sensor up in a task on core 0; when nobody renews the lease for IDLE_MS the camera switches itself off again,
//                whatever way the player left the paper flow (docs/system-design.md 8.2: the camera buffers live only in the camera
//                screens). end() switches it off at once.
//   live()       the newest frame for the preview as 640 x 480 RGB565 (valid until release()); liveGray() = the same as 320 x 240 gray
//                for visionLiveCheck. The frame is NOT mirrored (docs/screens.md 4.2). On the device the sensor runs at QVGA (320 x 240)
//                while the exposure is free (the Check screen): live() is then that picture pixel-doubled, liveGray() is exact.
//   lockExposure() freezes exposure / white balance for the countdown so the snap matches the check. lockExposure(true) also switches
//                the sensor to VGA for the snap (a driver re-init, a few hundred ms: docs/hardware-notes.md "Camera bring-up" row 15);
//                lockExposure(false) switches it back to QVGA.
//   grabStart()  the snap: throws away CAMERA_SNAP_SETTLE_FRAMES frames, then averages FRAME_AVG frames into the caller's 640 x 480 buffer
//                without blocking the game loop (poll grabState()). The whole snap gives up after CAMERA_GRAB_BUDGET_MS: grabState() is
//                then Failed and grabFail() says Timeout (a sensor that stopped delivering), so the countdown never freezes.
//
// Pixels are native RGB565 uint16_t (what vision.h and Renderer::frame565 expect), whatever byte order the sensor delivers.
// Device pin map, power sequence and every other number that needs the real hardware: docs/hardware-notes.md "Camera bring-up";
// camera.cpp tags each one CONFIRMED / CORRECTED (from the vendor sources) or `UNVERIFIED on hardware` (decided at the first power-on).
// The I2C bus: the camera task wraps every bus access (esp_camera_init / deinit, every sensor register call, the CAM_EN cycle) in
// scribble::I2cGuard (app/i2c_lock.h); the loop task takes the same guard around M5.update() and the IMU reads. update() no longer
// parks the game loop.
// A camera that starts but delivers no frames is Failed after CAMERA_NO_FRAMES_MS (timed, not counted in driver timeouts); the
// lease retries it every 3 s.
// Emulator (#ifdef EMU): serves test photos instead of a sensor (see camera.cpp and emu/README.md): the synthetic template shots of
// shared/golden/vision_frames.bin by case name (env EMU_PHOTO_CASE or the script command `photo <case>`) or a raw / PPM photo file
// (env EMU_PHOTO). The live view settles (out of focus for the first moment) and has frame-to-frame noise, so the lights can turn green over time.
// Failure paths on the virtual clock: EMU_CAMERA_FAIL=1 (no sensor), EMU_CAMERA_STALL=1 (every snap times out after CAMERA_GRAB_BUDGET_MS,
// the live view keeps going), EMU_CAMERA_STALL=2 (starts, then never a frame: Failed after CAMERA_NO_FRAMES_MS).
#pragma once
#include <cstddef>
#include <cstdint>
#include "config.h"

namespace scribble {

constexpr int CAMERA_PIXELS = CAM_W * CAM_H;               // 640 x 480
constexpr size_t CAMERA_FRAME_BYTES = size_t(CAMERA_PIXELS) * 2;
constexpr int CAMERA_IDLE_MS = 2000;                        // no begin() for this long: the camera switches itself off
constexpr int CAMERA_GRAB_BUDGET_MS = 3000;                 // a snap (size switch if needed + settle frames + FRAME_AVG frames) gives up after this
constexpr int CAMERA_NO_FRAMES_MS = 1500;                   // running but no frame for this long: the camera is Failed
constexpr int CAMERA_SNAP_SETTLE_FRAMES = 3;                // frames thrown away at the start of a snap (stale queue, a size switch settling)

class Camera {
 public:
  enum class State : uint8_t { Off, Starting, Running, Failed };
  enum class Grab : uint8_t { Idle, Busy, Done, Failed };
  /** Why the last snap ended Failed (kept until the next grabStart(), so it can be read after grabAck()). */
  enum class GrabFail : uint8_t { None, Camera, Timeout };   // Camera = off / wrong frame / re-init failed; Timeout = no frames within the budget

  Camera();
  ~Camera();
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;

  /** Call once per loop pass: ends the lease when nobody renewed it (cheap; the device task checks the lease too). Never blocks. */
  void update(uint32_t nowMs);

  bool begin();                                  // take / renew the lease; true unless the camera failed to start
  void end();                                    // camera off now
  State state() const;
  bool running() const { return state() == State::Running; }
  bool failed() const { return state() == State::Failed; }

  /** Newest frame (CAM_W x CAM_H RGB565) or nullptr; the pointer stays valid until release() (the camera task cannot swap buffers meanwhile). */
  const uint16_t* live();
  void release();
  uint32_t frameNo() const;                      // +1 for every new live frame
  /** Copy of the newest frame as 320 x 240 gray (2 x 2 average, vision.h grayQvga). False if nothing newer than `lastNo` exists; `lastNo` is updated. */
  bool liveGray(uint8_t* dst320x240, uint32_t& lastNo);

  /** The snap. grabStart() returns at once; the camera averages FRAME_AVG frames into `dst` (CAM_W x CAM_H) in its own task; poll grabState(). */
  bool grabStart(uint16_t* dst);
  Grab grabState() const;
  void grabAck();                                // Done / Failed -> Idle
  GrabFail grabFail() const;                     // why the last snap failed (None after a good one)
  bool grabTimedOut() const { return grabFail() == GrabFail::Timeout; }
  bool grab(uint16_t* dst);                      // blocking convenience for tests (waits up to the budget + 0.5 s)

  void lockExposure(bool lock);                  // true at the countdown start, false when it ends

  /** Bytes camera.cpp holds in its own buffers right now (not the driver's frame buffers): 0 whenever the camera is off. */
  static size_t bufferBytes();

#ifdef EMU
  /** Emulator test photo: name = a case of vision_golden.h ("clean", "tilt", "dim", "dark", "faded", "noisy", "harsh", "nomarks", "blank", ...) or "file" (EMU_PHOTO). */
  void setCase(const char* name);
  static const char* caseNames();                // for messages
#endif

 private:
  struct Impl;
  Impl* p_ = nullptr;
};

}  // namespace scribble
