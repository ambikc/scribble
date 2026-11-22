// renderer.ts - everything that puts pixels on the 320 x 240 canvas.   (platform adapter; the firmware uses M5GFX sprites)
//
// The simulator redraws the whole frame every tick (the browser can); the firmware repaints only dirty rectangles
// (docs/system-design.md section 6). Drawing calls are deliberately simple primitives so they port to M5GFX.
import * as C from './config';
import { Level, getBit } from './level';

const hex = (h: string) => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
const P = C.PALETTE_HEX;
export const FONT = "'Fredoka', 'Trebuchet MS', Verdana, sans-serif";

export interface LayerMask { ground: boolean; spike: boolean; bouncy: boolean }
const ALL: LayerMask = { ground: true, spike: true, bouncy: true };

export class Renderer {
  readonly canvas: HTMLCanvasElement;
  readonly ctx: CanvasRenderingContext2D;
  private paper: HTMLCanvasElement;
  private layers: HTMLCanvasElement; private lctx: CanvasRenderingContext2D; private limg: ImageData;
  private tmp: HTMLCanvasElement; private tctx: CanvasRenderingContext2D;
  shakeX = 0; shakeY = 0;

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas; canvas.width = C.SCREEN_W; canvas.height = C.SCREEN_H;
    this.ctx = canvas.getContext('2d', { alpha: false })!; this.ctx.imageSmoothingEnabled = false;
    // pre-rendered paper: cream with a faint dot every 16 px
    this.paper = document.createElement('canvas'); this.paper.width = C.SCREEN_W; this.paper.height = C.SCREEN_H;
    const p = this.paper.getContext('2d')!; p.fillStyle = P.PAPER; p.fillRect(0, 0, C.SCREEN_W, C.SCREEN_H); p.fillStyle = P.PAPER_DOT;
    for (let y = 8; y < C.SCREEN_H; y += 16) for (let x = 8; x < C.SCREEN_W; x += 16) p.fillRect(x - 1, y - 1, 2, 2);
    this.layers = document.createElement('canvas'); this.layers.width = C.WORLD_W; this.layers.height = C.WORLD_H;
    this.lctx = this.layers.getContext('2d')!; this.limg = this.lctx.createImageData(C.WORLD_W, C.WORLD_H);
    this.tmp = document.createElement('canvas'); this.tctx = this.tmp.getContext('2d')!;
  }

  // ---- background ---------------------------------------------------------------------------
  clearPaper(): void { this.ctx.drawImage(this.paper, 0, 0); }
  clear(color: string): void { this.ctx.fillStyle = color; this.ctx.fillRect(0, 0, C.SCREEN_W, C.SCREEN_H); }

  // ---- the level, painted with a texture per material (colour-blind safe) ----------------------------
  /** Call after the level's pixels change. */
  dirty(): void { /* the layers are re-rasterised every frame (61 k pixels), so there is nothing to invalidate */ }

  /** Draw the three layers (transparent where empty) at (x, y). `mask` lets Review reveal them one by one. */
  drawLayers(level: Level, mask: LayerMask = ALL, x = 0, y = 0, alpha = 1): void {
    const d = this.limg.data, ink = hex(P.INK), red = hex(P.RED), blue = hex(P.BLUE), white = [255, 255, 255], lred = [255, 190, 184];
    for (let py = 0; py < C.WORLD_H; py++) for (let px = 0; px < C.WORLD_W; px++) {
      const o = (py * C.WORLD_W + px) * 4;
      let c: number[] | null = null;
      if (mask.ground && getBit(level.ground, px, py)) c = ink;
      else if (mask.spike && getBit(level.spike, px, py)) c = ((px + py) & 3) === 0 ? lred : red;       // diagonal ticks = "spiky"
      else if (mask.bouncy && getBit(level.bouncy, px, py)) c = (px & 3) === 0 && (py & 3) === 0 ? white : blue; // dots = "bubbly"
      if (c) { d[o] = c[0]; d[o + 1] = c[1]; d[o + 2] = c[2]; d[o + 3] = 255; } else d[o + 3] = 0;
    }
    this.lctx.putImageData(this.limg, 0, 0);
    this.ctx.save(); this.ctx.globalAlpha = alpha; this.ctx.drawImage(this.layers, x, y); this.ctx.restore();
  }

  // ---- sprites -----------------------------------------------------------------------------------
  /** The ball: yellow, ink outline, two eyes that turn as it rolls. sx/sy squash and stretch it. */
  ball(x: number, y: number, angle = 0, sx = 1, sy = 1, face: 'normal' | 'happy' = 'normal'): void {
    const c = this.ctx, r = C.BALL_RADIUS;
    c.save(); c.translate(x, y); c.scale(sx, sy);
    c.fillStyle = P.YELLOW; c.strokeStyle = P.INK; c.lineWidth = 2; c.beginPath(); c.arc(0, 0, r - 1, 0, Math.PI * 2); c.fill(); c.stroke();
    c.rotate(angle * 0.5); c.fillStyle = P.INK;
    c.beginPath(); c.arc(-2.6, -1.8, 1.5, 0, 7); c.arc(2.6, -1.8, 1.5, 0, 7); c.fill();
    if (face === 'happy') { c.strokeStyle = P.INK; c.lineWidth = 1.4; c.beginPath(); c.arc(0, 0.5, 3.4, 0.15 * Math.PI, 0.85 * Math.PI); c.stroke(); }
    c.restore();
  }

  /** The flag on its pole; `wave` (0..1) flutters it. (x, y) is the pole base. */
  flag(x: number, y: number, wave = 0): void {
    const c = this.ctx; c.save(); c.lineCap = 'round'; c.lineJoin = 'round';
    c.strokeStyle = P.INK; c.lineWidth = 3; c.beginPath(); c.moveTo(x, y); c.lineTo(x, y - 24); c.stroke();
    const w = Math.sin(wave * 30) * 2 * wave;
    c.fillStyle = P.GREEN; c.lineWidth = 2; c.beginPath(); c.moveTo(x + 1, y - 24); c.lineTo(x + 21, y - 17 + w); c.lineTo(x + 1, y - 10); c.closePath(); c.fill(); c.stroke();
    c.restore();
  }

  /** The Start pin: a ball outline in a green ring with a down arrow ("it will drop"). */
  startPin(x: number, y: number, active = false): void {
    const c = this.ctx; c.save(); c.lineCap = 'round'; c.lineJoin = 'round';
    c.strokeStyle = P.GREEN; c.lineWidth = 3; c.beginPath(); c.moveTo(x - 6, y - 24); c.lineTo(x, y - 17); c.lineTo(x + 6, y - 24); c.stroke();
    c.fillStyle = 'rgba(255,255,255,0.7)'; c.beginPath(); c.arc(x, y, 13, 0, 7); c.fill(); c.stroke();
    c.restore(); this.ball(x, y, 0);
    if (active) { c.save(); c.strokeStyle = P.GREEN; c.lineWidth = 1.5; c.setLineDash([3, 4]); c.beginPath(); c.arc(x, y, C.PIN_GRAB_RADIUS, 0, 7); c.stroke(); c.restore(); }
  }
  flagPin(x: number, y: number, active = false): void {
    const c = this.ctx; c.save(); c.strokeStyle = P.GREEN; c.lineWidth = 2; c.setLineDash([3, 4]);
    c.beginPath(); c.arc(x, y - C.FLAG_CENTER_DY, active ? C.PIN_GRAB_RADIUS : 16, 0, 7); c.stroke(); c.restore();
    this.flag(x, y);
  }

  // ---- text and shapes ---------------------------------------------------------------------------
  text(s: string, x: number, y: number, size: number, color: string = P.INK, align: CanvasTextAlign = 'center', stroke?: string): void {
    const c = this.ctx; c.save(); c.font = `700 ${size}px ${FONT}`; c.textAlign = align; c.textBaseline = 'middle';
    if (stroke) { c.lineWidth = 3; c.strokeStyle = stroke; c.lineJoin = 'round'; c.strokeText(s, x, y); }
    c.fillStyle = color; c.fillText(s, x, y); c.restore();
  }
  roundRect(x: number, y: number, w: number, h: number, r: number, fill: string, stroke: string | null = P.INK, lw = 2): void {
    const c = this.ctx; c.beginPath(); c.roundRect(x, y, w, h, r); c.fillStyle = fill; c.fill(); if (stroke) { c.lineWidth = lw; c.strokeStyle = stroke; c.stroke(); }
  }

  /** A 60 x 36, 4 bits-per-pixel palette thumbnail from the level file, drawn at `scale`. */
  thumb(x: number, y: number, bytes: Uint8Array, scale = 1): void {
    const pal = C.PALETTE_ORDER.map((k) => hex(P[k]));
    const img = this.tctx.createImageData(C.THUMB_W, C.THUMB_H);
    for (let i = 0; i < C.THUMB_W * C.THUMB_H; i++) {
      const v = (i & 1) === 0 ? bytes[i >> 1] >> 4 : bytes[i >> 1] & 15, c = pal[Math.min(8, v)];
      img.data[i * 4] = c[0]; img.data[i * 4 + 1] = c[1]; img.data[i * 4 + 2] = c[2]; img.data[i * 4 + 3] = 255;
    }
    this.tmp.width = C.THUMB_W; this.tmp.height = C.THUMB_H; this.tctx.putImageData(img, 0, 0);
    this.ctx.drawImage(this.tmp, x, y, C.THUMB_W * scale, C.THUMB_H * scale);
  }

  // ---- camera pictures -------------------------------------------------------------------------------
  /** The 640 x 480 RGB565 frame, sampled down to dw x dh at (dx, dy). */
  frame565(f: Uint16Array, dx: number, dy: number, dw: number, dh: number, dim = 1): void {
    const img = this.tctx.createImageData(dw, dh), sx = C.CAM_W / dw, sy = C.CAM_H / dh;
    for (let y = 0; y < dh; y++) for (let x = 0; x < dw; x++) {
      const p = f[Math.min(C.CAM_H - 1, Math.floor((y + 0.5) * sy)) * C.CAM_W + Math.min(C.CAM_W - 1, Math.floor((x + 0.5) * sx))], o = (y * dw + x) * 4;
      img.data[o] = (((p >> 11) << 3) | (p >> 13)) * dim; img.data[o + 1] = ((((p >> 5) & 63) << 2) | ((p >> 9) & 3)) * dim; img.data[o + 2] = (((p & 31) << 3) | ((p >> 2) & 7)) * dim; img.data[o + 3] = 255;
    }
    this.tmp.width = dw; this.tmp.height = dh; this.tctx.putImageData(img, 0, 0); this.ctx.drawImage(this.tmp, dx, dy);
  }
  /** The straightened 320 x 192 page, mixed toward paper colour (Review shows it dimmed, ~35 %). */
  straight565(f: Uint16Array, strength = 0.35): void {
    const img = this.tctx.createImageData(C.WORLD_W, C.WORLD_H), paper = hex(P.PAPER);
    for (let i = 0; i < f.length; i++) {
      const p = f[i], r = ((p >> 11) << 3) | (p >> 13), g = (((p >> 5) & 63) << 2) | ((p >> 9) & 3), b = ((p & 31) << 3) | ((p >> 2) & 7), o = i * 4;
      img.data[o] = paper[0] + (r - paper[0]) * strength; img.data[o + 1] = paper[1] + (g - paper[1]) * strength; img.data[o + 2] = paper[2] + (b - paper[2]) * strength; img.data[o + 3] = 255;
    }
    this.tmp.width = C.WORLD_W; this.tmp.height = C.WORLD_H; this.tctx.putImageData(img, 0, 0); this.ctx.drawImage(this.tmp, 0, 0);
  }

  flash(color: string, alpha: number): void { if (alpha > 0) { this.ctx.save(); this.ctx.globalAlpha = Math.min(1, alpha); this.ctx.fillStyle = color; this.ctx.fillRect(0, 0, C.SCREEN_W, C.SCREEN_H); this.ctx.restore(); } }
  dim(alpha: number): void { this.ctx.save(); this.ctx.globalAlpha = alpha; this.ctx.fillStyle = P.INK; this.ctx.fillRect(0, 0, C.SCREEN_W, C.SCREEN_H); this.ctx.restore(); }
}
