// input.cpp - touch + tilt.   Port of sim/src/input.ts on M5.Touch and M5.Imu.
#include "input.h"
#include <M5Unified.h>
#include <cmath>
#include "tilt.h"
#include "i2c_lock.h"

namespace scribble {

void Input::begin() {
  down_ = pressedEdge_ = releasedEdge_ = false; blocked_ = false; touchId_ = -1;
  x_ = y_ = sx_ = sy_ = 0; downAt_ = 0;
  acc_[0] = acc_[1] = acc_[2] = 0;
  smoothed_ = 0; virtualDir_ = 0;
  upright_ = true; jumpEdge_ = backEdge_ = false; jumpHeld_ = backHeld_ = false;
}

/** Read the hardware. Idempotent: edges come from comparing with the previous sample, so calling it again in the same loop
 *  pass (main() calls it once per pass, poll() calls it per tick) never creates a second edge. */
void Input::sample(uint32_t nowMs) {
  // ---- touch: ONE finger. A second finger ends the touch: getDetail(0) is the detail of RAW slot 0, which becomes the second
  // finger (another id) when the first one lifts, so without this a second finger would continue the first one's stroke with no
  // release. So: remember the id that started the touch; if the id changes, or a second finger appears after the press, emit a
  // release and ignore the screen until every finger is up.
  const int count = M5.Touch.getCount();
  if (blocked_ && count == 0) blocked_ = false;          // every finger is up (a lifted finger still counts for one update)
  bool d = false;
  if (!blocked_) {
    const auto& t = M5.Touch.getDetail();
    d = t.isPressed();
    if (d && down_ && (t.id != touchId_ || count > 1)) { d = false; blocked_ = true; }   // a second finger: release here
    else if (d && !down_ && count > 1) {                 // two fingers landed in the same sample: not a touch at all
      const auto& t2 = M5.Touch.getDetail(1);
      if (t2.isPressed() && t2.id != t.id) { d = false; blocked_ = true; }
    }
    if (d) {
      x_ = t.x < 0 ? 0 : t.x >= SCREEN_W ? SCREEN_W - 1 : t.x; y_ = t.y < 0 ? 0 : t.y >= SCREEN_H ? SCREEN_H - 1 : t.y;
      if (!down_) touchId_ = int(t.id);
    }
  }
  if (d && !down_) { sx_ = x_; sy_ = y_; downAt_ = nowMs; pressedEdge_ = true; }
  if (!d && down_) releasedEdge_ = true;
  down_ = d;

#ifdef EMU
  // emulator keys: Space = jump, Escape = back (edges latched like the touch edges)
  const bool j = emu::keyHeld(emu::Key::Jump), b = emu::keyHeld(emu::Key::Back);
  if (j && !jumpHeld_) jumpEdge_ = true;
  if (b && !backHeld_) backEdge_ = true;
  jumpHeld_ = j; backHeld_ = b;
#endif
}

InputState Input::poll(uint32_t nowMs) {
  sample(nowMs);
  // accelerometer (once per tick)
  {
    I2cGuard bus;                                        // the IMU shares the internal I2C bus with the camera task (i2c_lock.h)
    if (M5.Imu.update()) {
      const auto a = M5.Imu.getImuData().accel;
      acc_[0] = a.x; acc_[1] = a.y; acc_[2] = a.z;
    }
  }

  // tilt: the on-screen/virtual direction wins, otherwise the accelerometer along the rolling axis
  const double raw = virtualDir_ != 0 ? virtualDir_ * TILT_FULL : double(TILT_SIGN) * acc_[TILT_AXIS];
  smoothed_ = tilt_.step(raw);

  // upright (see tilt.h): vertical screen axis = the in-plane axis that is not the rolling axis; normal = z
  const int vert = TILT_AXIS == 1 ? 0 : 1;
  upright_ = uprightFromAccel(acc_[TILT_AXIS], acc_[vert], acc_[2], upright_);

  InputState s;
  s.touch.down = down_; s.touch.pressed = pressedEdge_; s.touch.released = releasedEdge_;
  s.touch.x = x_; s.touch.y = y_; s.touch.startX = sx_; s.touch.startY = sy_;
  s.touch.heldMs = down_ ? nowMs - downAt_ : 0;
  s.tilt = smoothed_;
  s.upright = upright_;
  s.keyJump = jumpEdge_; s.keyBack = backEdge_;
  return s;
}

void Input::consumeEdges() { pressedEdge_ = releasedEdge_ = false; jumpEdge_ = backEdge_ = false; }

}  // namespace scribble
