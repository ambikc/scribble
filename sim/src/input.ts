// input.ts - touch + tilt for the simulator.   (platform adapter; the firmware version uses M5Unified)
//
//   touch : mouse or a finger on the 320x240 canvas -> ONE touch point (single touch, like the device)
//   tilt  : arrow keys / A D  (stand-in for the BMI270)  or the phone's orientation sensor
//   jump  : Space / Up / W    (on the device a tap anywhere on the playfield or the green Jump pad)
//   back  : Escape            (convenience only; the device has no hardware buttons)
//
// Output is ONE InputState per 60 Hz tick (poll() once per tick, then consumeEdges()). Edges are latched until consumed.
import * as C from './config';

export interface Touch {
  down: boolean; pressed: boolean; released: boolean;
  x: number; y: number;          // 320 x 240 screen pixels
  startX: number; startY: number; // where this touch began (for the grip-safe edge rule)
  heldMs: number;
}
export interface InputState {
  touch: Touch;
  tilt: number;                  // -1..1, dead-zone + smoothing applied (same maths as the firmware)
  keyJump: boolean;              // true for one tick when a jump key is pressed
  keyBack: boolean;
  upright: boolean;              // "the device is standing up" (always true in the browser unless a phone says otherwise)
}

export class Input {
  private canvas: HTMLCanvasElement;
  private down = false; private pressedEdge = false; private releasedEdge = false;
  private x = 0; private y = 0; private sx = 0; private sy = 0; private downAt = 0;
  private keys = new Set<string>();
  private jumpEdge = false; private backEdge = false;
  private rawAx = 0;                 // raw "accelerometer X" in g, before dead-zone and smoothing
  private smoothed = 0;
  private orientationOn = false; private orientAx = 0;
  private pointerId: number | null = null;
  upright = true;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas;
    canvas.style.touchAction = 'none';
    canvas.addEventListener('pointerdown', (e) => {
      if (this.pointerId !== null) return;                      // single touch only: a second finger is ignored
      this.pointerId = e.pointerId; canvas.setPointerCapture(e.pointerId);
      this.setPos(e); this.sx = this.x; this.sy = this.y; this.down = true; this.pressedEdge = true; this.downAt = performance.now();
      e.preventDefault();
    });
    canvas.addEventListener('pointermove', (e) => { if (e.pointerId === this.pointerId) this.setPos(e); });
    const up = (e: PointerEvent) => { if (e.pointerId === this.pointerId) { this.setPos(e); this.down = false; this.releasedEdge = true; this.pointerId = null; } };
    canvas.addEventListener('pointerup', up); canvas.addEventListener('pointercancel', up);
    window.addEventListener('keydown', (e) => {
      if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', ' '].includes(e.key)) e.preventDefault();
      if (!this.keys.has(e.key)) {
        if (e.key === ' ' || e.key === 'ArrowUp' || e.key === 'w' || e.key === 'W') this.jumpEdge = true;
        if (e.key === 'Escape') this.backEdge = true;
      }
      this.keys.add(e.key);
    });
    window.addEventListener('keyup', (e) => this.keys.delete(e.key));
    window.addEventListener('blur', () => this.keys.clear());
  }

  private setPos(e: PointerEvent) {
    const r = this.canvas.getBoundingClientRect();
    this.x = Math.max(0, Math.min(C.SCREEN_W - 1, ((e.clientX - r.left) / r.width) * C.SCREEN_W));
    this.y = Math.max(0, Math.min(C.SCREEN_H - 1, ((e.clientY - r.top) / r.height) * C.SCREEN_H));
  }

  /** Use the phone's orientation sensor as the tilt. iOS needs a user gesture + permission, hence a method. */
  async enableOrientation(): Promise<boolean> {
    const DOE = (window as unknown as { DeviceOrientationEvent?: { requestPermission?: () => Promise<string> } }).DeviceOrientationEvent;
    try {
      if (DOE?.requestPermission) { if ((await DOE.requestPermission()) !== 'granted') return false; }
      window.addEventListener('deviceorientation', (e) => {
        if (e.gamma === null && e.beta === null) return;
        // Held in landscape the sideways roll is `beta`; in portrait it is `gamma`. ax ~ sin(angle), like an accelerometer.
        const ang = (screen.orientation?.angle ?? 0) % 360;
        const deg = ang === 90 ? (e.beta ?? 0) : ang === 270 ? -(e.beta ?? 0) : ang === 180 ? -(e.gamma ?? 0) : (e.gamma ?? 0);
        this.orientAx = Math.sin((Math.max(-80, Math.min(80, deg)) * Math.PI) / 180);
        this.orientationOn = true;
      });
      return true;
    } catch { return false; }
  }
  get orientationActive() { return this.orientationOn; }

  /** Called once per 60 Hz tick. */
  poll(): InputState {
    // tilt: keyboard gives a full-scale ax, otherwise the phone sensor
    const k = (this.keys.has('ArrowRight') || this.keys.has('d') || this.keys.has('D') ? 1 : 0) - (this.keys.has('ArrowLeft') || this.keys.has('a') || this.keys.has('A') ? 1 : 0);
    this.rawAx = k !== 0 ? k * C.TILT_FULL : this.orientationOn ? this.orientAx : 0;
    // dead-zone, linear to TILT_FULL, then a light low-pass (same constants as the firmware)
    const mag = Math.max(0, Math.abs(this.rawAx) - C.TILT_DEADZONE) / (C.TILT_FULL - C.TILT_DEADZONE);
    const target = Math.sign(this.rawAx) * Math.min(1, mag);
    this.smoothed += (target - this.smoothed) * C.TILT_SMOOTH;
    if (Math.abs(this.smoothed) < 0.002) this.smoothed = 0;

    const s: InputState = {
      touch: { down: this.down, pressed: this.pressedEdge, released: this.releasedEdge, x: this.x, y: this.y, startX: this.sx, startY: this.sy, heldMs: this.down ? performance.now() - this.downAt : 0 },
      tilt: this.smoothed, keyJump: this.jumpEdge, keyBack: this.backEdge, upright: this.upright,
    };
    return s;
  }

  /** Call after a tick has used the edges. Edges are LATCHED until then, so a tap is never lost on a frame
   *  that happens to run no tick (a 60 Hz display often lands just under the 1/60 s threshold). */
  consumeEdges(): void { this.pressedEdge = false; this.releasedEdge = false; this.jumpEdge = false; this.backEdge = false; }

  /** Test hook: pretend a finger touched/moved/lifted. */
  inject(kind: 'down' | 'move' | 'up', x: number, y: number) {
    this.x = x; this.y = y;
    if (kind === 'down') { this.sx = x; this.sy = y; this.down = true; this.pressedEdge = true; this.downAt = performance.now(); }
    if (kind === 'up') { this.down = false; this.releasedEdge = true; }
  }
}
