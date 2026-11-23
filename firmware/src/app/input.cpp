// input.cpp - touch + tilt.   Port of sim/src/input.ts on M5.Touch and M5.Imu.
#include "input.h"
#include <M5Unified.h>
#include <cmath>
#include "tilt.h"

namespace scribble {

void Input::begin() {
  down_ = pressedEdge_ = releasedEdge_ = false;
  x_ = y_ = sx_ = sy_ = 0; downAt_ = 0;
  acc_[0] = acc_[1] = acc_[2] = 0;
  smoothed_ = 0; virtualDir_ = 0;
  upright_ = true; jumpEdge_ = backEdge_ = false; jumpHeld_ = backHeld_ = false;
}

/** Read the hardware. Idempotent: edges come from comparing with the previous sample, so calling it again in the same loop
 *  pass (main() calls it once per pass, poll() calls it per tick) never creates a second edge. */
void Input::sample(uint32_t nowMs) {
  // ---- touch: ONE finger (index 0); a second finger is ignored ----
  const auto t = M5.Touch.getDetail();
  const bool d = t.isPressed();
  if (d) { x_ = t.x < 0 ? 0 : t.x >= SCREEN_W ? SCREEN_W - 1 : t.x; y_ = t.y < 0 ? 0 : t.y >= SCREEN_H ? SCREEN_H - 1 : t.y; }
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
  if (M5.Imu.update()) {
    const auto a = M5.Imu.getImuData().accel;
    acc_[0] = a.x; acc_[1] = a.y; acc_[2] = a.z;
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
