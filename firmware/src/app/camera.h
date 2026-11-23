// camera.h - the camera: VGA frames for the paper flow.   (platform adapter: esp32-camera on the device, test photos in the emulator)
//
//   begin()      "I need the camera": a lease that must be renewed (every tick of a paper screen calls it). The first call powers
//                the sensor up in a task on core 0; when nobody renews the lease for IDLE_MS the camera switches itself off again,
//                whatever way the player left the paper flow (docs/system-design.md 8.2: the camera buffers live only in the camera
//                screens). end() switches it off at once.
//   live()       the newest 640 x 480 RGB565 frame for the preview (valid until release()); liveGray() = the same as 320 x 240 gray
//                for visionLiveCheck. The frame is NOT mirrored (docs/screens.md 4.2).
//   grabStart()  the snap: averages FRAME_AVG frames into the caller's 640 x 480 buffer without blocking the game loop (poll grabState()).
//   lockExposure() freezes exposure / white balance for the countdown so the snap matches the check.
//
// Pixels are native RGB565 uint16_t (what vision.h and Renderer::frame565 expect), whatever byte order the sensor delivers.
// Device pin map, power sequence and every other number that needs the real hardware: docs/hardware-notes.md "Camera bring-up";
// each is marked `UNVERIFIED on hardware` in camera.cpp.
// Emulator (#ifdef EMU): serves test photos instead of a sensor (see camera.cpp and emu/README.md): the synthetic template shots of
// shared/golden/vision_frames.bin by case name (env EMU_PHOTO_CASE or the script command `photo <case>`) or a raw / PPM photo file
// (env EMU_PHOTO). The live view settles (out of focus for the first moment) and has frame-to-frame noise, so the lights can turn green over time.
#pragma once
#include <cstddef>
#include <cstdint>
#include "config.h"

namespace scribble {

constexpr int CAMERA_PIXELS = CAM_W * CAM_H;               // 640 x 480
constexpr size_t CAMERA_FRAME_BYTES = size_t(CAMERA_PIXELS) * 2;
constexpr int CAMERA_IDLE_MS = 2000;                        // no begin() for this long: the camera switches itself off

class Camera {
 public:
  enum class State : uint8_t { Off, Starting, Running, Failed };
  enum class Grab : uint8_t { Idle, Busy, Done, Failed };

  Camera();
  ~Camera();
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;

  /** Call once per loop pass: the emulator's lease expiry and log lines; on the device it only wakes the task when the lease ran out. */
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
  bool grab(uint16_t* dst);                      // blocking convenience for tests (waits up to ~2 s)

  void lockExposure(bool lock);                  // true at the countdown start, false when it ends

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
