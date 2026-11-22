// ui.ts - icons, buttons, hit-testing and hold-to-activate.   (app layer; the firmware version draws with M5GFX primitives)
//
// Rules from docs/screens.md:
//   * every target is >= 48 px (corner chips: 40 px drawn, 48 px hit area)
//   * hit-testing is forgiving: the nearest target within HIT_FORGIVE (6 px) of the touch wins
//   * "hold" buttons (Edit, Restart in Play; Clear; delete) need HOLD_MS and show a ring that fills
//   * the big green button is always the "go" action; icons use ink glyphs (white on green is too low contrast)
import * as C from './config';
import { InputState } from './input';
import { Renderer } from './renderer';

const P = C.PALETTE_HEX;

// ---- icons: SVG path data on a 24 x 24 grid (the same paths as the design mock-ups) -------------------------
export type IconName = 'back' | 'menu' | 'play' | 'grid' | 'camera' | 'pencil' | 'undo' | 'restart' | 'home' | 'plus' | 'trash' | 'sound0' | 'sound1' | 'sound2' | 'up' | 'check' | 'inkLess' | 'inkMore' | 'help' | 'stop' | 'eraser' | 'ground' | 'spike' | 'bouncy' | 'ruler' | 'focus' | 'bulb' | 'level';
interface IconDef { stroke?: string[]; fill?: string[]; w?: number }
const ICONS: Record<IconName, IconDef> = {
  back: { stroke: ['M10 6l-6 6 6 6M4.5 12H20'], w: 2.6 },
  menu: { stroke: ['M4.5 7h15M4.5 12h15M4.5 17h15'], w: 2.8 },
  play: { fill: ['M7 4.5v15l12-7.5z'] },
  grid: { stroke: ['M5.5 4h4a1.5 1.5 0 011.5 1.5v4A1.5 1.5 0 019.5 11h-4A1.5 1.5 0 014 9.5v-4A1.5 1.5 0 015.5 4zM14.5 4h4A1.5 1.5 0 0120 5.5v4a1.5 1.5 0 01-1.5 1.5h-4A1.5 1.5 0 0113 9.5v-4A1.5 1.5 0 0114.5 4zM5.5 13h4a1.5 1.5 0 011.5 1.5v4A1.5 1.5 0 019.5 20h-4A1.5 1.5 0 014 18.5v-4A1.5 1.5 0 015.5 13zM14.5 13h4a1.5 1.5 0 011.5 1.5v4a1.5 1.5 0 01-1.5 1.5h-4a1.5 1.5 0 01-1.5-1.5v-4a1.5 1.5 0 011.5-1.5z'], w: 2.2 },
  camera: { stroke: ['M3.5 8.5h3.5l1.5-2.5h7l1.5 2.5h3.5v11h-17z', 'M15.6 13.5a3.6 3.6 0 11-7.2 0 3.6 3.6 0 017.2 0z'], w: 2.2 },
  pencil: { stroke: ['M4.5 19.5l1-4.5L16 4.5l3.5 3.5L9 18.5zM14 6.5l3.5 3.5'], w: 2.4 },
  undo: { stroke: ['M8.5 6L4 10.5 8.5 15M4.5 10.5H14a5.5 5.5 0 010 11h-3'], w: 2.4 },
  restart: { stroke: ['M19.5 12a7.5 7.5 0 11-2.5-5.6M19.5 4.5v4.5H15'], w: 2.4 },
  home: { stroke: ['M3.5 11.5L12 4l8.5 7.5M6 10v9.5h12V10'], w: 2.4 },
  plus: { stroke: ['M12 5v14M5 12h14'], w: 3 },
  trash: { stroke: ['M5 7h14M9.5 7V4.5h5V7M7 7l.8 12.5h8.4L17 7M10.5 11v5M13.5 11v5'], w: 2.2 },
  sound0: { fill: ['M4 9.5h4l5-4v13l-5-4H4z'], stroke: ['M16 9.5l5 5M21 9.5l-5 5'], w: 2 },
  sound1: { fill: ['M4 9.5h4l5-4v13l-5-4H4z'], stroke: ['M16 9a4 4 0 010 6'], w: 2.2 },
  sound2: { fill: ['M4 9.5h4l5-4v13l-5-4H4z'], stroke: ['M16 9a4 4 0 010 6M18.6 6.4a8 8 0 010 11.2'], w: 2.2 },
  up: { stroke: ['M12 19.5v-14M6 11.5l6-6 6 6'], w: 3 },
  check: { stroke: ['M5 12.5l4.5 4.5L19 7'], w: 3.2 },
  inkLess: { stroke: ['M5 9h14', 'M6 16h12'], w: 2 },
  inkMore: { stroke: ['M5 8h14', 'M6 17h12'], w: 4.4 },
  help: { stroke: ['M9 9a3 3 0 116 0c0 2-3 2.2-3 4.5M12 17.5v.5'], w: 2.8 },
  stop: { fill: ['M6 6h12v12H6z'] },
  eraser: { stroke: ['M6.5 17.5l-2-2 9-9 5 5-6 6H7.5zM11 20h8'], w: 2.2 },
  ground: { stroke: ['M3 12h18'], w: 5 },
  spike: { stroke: ['M2.5 18L7 6l4.5 12L16 6l4.5 12'], w: 3 },
  bouncy: { stroke: ['M3 17h4l3-10 4 10 3-10 3 10h1'], w: 2.8 },
  ruler: { stroke: ['M3 12h18M3 12l4-3.5M3 12l4 3.5M21 12l-4-3.5M21 12l-4 3.5'], w: 2.2 },
  focus: { stroke: ['M5 9V5h4M19 9V5h-4M5 15v4h4M19 15v4h-4'], fill: ['M15.5 12a3.5 3.5 0 11-7 0 3.5 3.5 0 017 0z'], w: 2.4 },
  bulb: { stroke: ['M9 18h6M10 21h4M8.5 14.5a6 6 0 1 1 7 0c-.8.7-1.5 1.5-1.5 2.5h-4c0-1-.7-1.8-1.5-2.5z'], w: 2 },
  level: { stroke: ['M5.5 7h13a2.5 2.5 0 012.5 2.5v0a2.5 2.5 0 01-2.5 2.5h-13A2.5 2.5 0 013 9.5v0A2.5 2.5 0 015.5 7z', 'M12 12v5'], w: 2.2 },
};

/** Draw an icon centred at (cx, cy), `size` px square. */
export function drawIcon(ctx: CanvasRenderingContext2D, name: IconName, cx: number, cy: number, size: number, color: string = P.INK): void {
  const d = ICONS[name]; ctx.save(); ctx.translate(cx - size / 2, cy - size / 2); ctx.scale(size / 24, size / 24);
  ctx.strokeStyle = color; ctx.fillStyle = color; ctx.lineWidth = d.w ?? 2.4; ctx.lineCap = 'round'; ctx.lineJoin = 'round';
  d.fill?.forEach((p) => ctx.fill(new Path2D(p))); d.stroke?.forEach((p) => ctx.stroke(new Path2D(p)));
  ctx.restore();
}

// ---- buttons ---------------------------------------------------------------------------------------------
export type BtnKind = 'tab' | 'chip' | 'big' | 'custom';
export interface Btn {
  id: string; x: number; y: number; w: number; h: number; kind: BtnKind;
  icon?: IconName; iconSize?: number; fill?: string;
  holdMs?: number;            // needs a hold of this many ms
  onPress?: boolean;          // fires on touch-down (the Jump pad)
  longId?: string; longMs?: number; // a long press fires `longId` instead of `id`
  edge?: 'top' | 'bottom';    // which edge of a tab carries the ink line (the top bar uses 'bottom')
  active?: boolean;           // pushed-in, yellow halo (the selected tool)
  disabled?: boolean;
  pulse?: boolean;            // gentle pulse (GO when all lights are green)
  hit?: number;               // extra hit padding (corner chips use 4 px so 40 px draws as a 48 px target)
  draw?: (ctx: CanvasRenderingContext2D, b: Btn, hold: number) => void; // custom drawing
}

export class UI {
  btns: Btn[] = [];
  /** The button the current touch began on. Buttons are rebuilt every tick, so always compare by id, never by object. */
  private pressed: Btn | null = null; private fired = false; private holdFrac = 0;
  /** True while the current touch began on a button: the screen must not also treat it as a stroke/jump. */
  touchOnButton = false;

  begin(): void { this.btns = []; }
  add(b: Btn): void { this.btns.push(b); }

  private find(x: number, y: number): Btn | null {
    let best: Btn | null = null, bd = 1e9;
    for (const b of this.btns) {
      if (b.disabled) continue;
      const pad = b.hit ?? 0, dx = Math.max(b.x - pad - x, 0, x - (b.x + b.w + pad)), dy = Math.max(b.y - pad - y, 0, y - (b.y + b.h + pad)), d = Math.hypot(dx, dy);
      if (d === 0) return b;                                                   // inside: done
      if (d <= C.HIT_FORGIVE && d < bd) { best = b; bd = d; }                   // near: the nearest wins
    }
    return best;
  }

  /** Returns the id of the button activated this tick, or null. */
  update(inp: InputState): string | null {
    const t = inp.touch; let out: string | null = null;
    if (t.pressed) {
      this.pressed = this.find(t.x, t.y); this.fired = false; this.holdFrac = 0; this.touchOnButton = this.pressed !== null;
      if (this.pressed?.onPress) { out = this.pressed.id; this.fired = true; }
    }
    if (t.down && this.pressed && !this.fired) {
      const b = this.pressed, over = this.find(t.x, t.y)?.id === b.id;
      if (b.holdMs) { this.holdFrac = over ? Math.min(1, t.heldMs / b.holdMs) : 0; if (over && t.heldMs >= b.holdMs) { out = b.id; this.fired = true; } }
      else if (b.longMs && b.longId && over && t.heldMs >= b.longMs) { out = b.longId; this.fired = true; }
    }
    if (t.released) {
      const b = this.pressed;
      if (b && !this.fired && !b.holdMs && this.find(t.x, t.y)?.id === b.id) out = b.id;
      this.pressed = null; this.holdFrac = 0; this.touchOnButton = false;
    }
    if (!t.down && !t.released && !t.pressed) { this.pressed = null; this.holdFrac = 0; }
    return out;
  }

  holdFor(id: string): number { return this.pressed && this.pressed.id === id && this.pressed.holdMs ? this.holdFrac : 0; }
  get pressedId(): string { return this.pressed?.id ?? ''; }

  draw(r: Renderer, now: number): void {
    const ctx = r.ctx;
    for (const b of this.btns) {
      if (b.kind === 'custom') { b.draw?.(ctx, b, this.holdFor(b.id)); continue; }
      const down = this.pressed?.id === b.id, hold = this.holdFor(b.id);
      if (b.kind === 'tab') {
        r.roundRect(b.x, b.y, b.w, b.h, 0, b.disabled ? P.PAPER_DOT : b.fill ?? P.WHITE, null);
        ctx.fillStyle = P.INK; ctx.fillRect(b.x, b.edge === 'bottom' ? b.y + b.h - 2 : b.y, b.w, 2); ctx.fillRect(b.x + b.w - 2, b.y, 2, b.h);
        if (b.active) { ctx.strokeStyle = P.INK; ctx.lineWidth = 3; ctx.strokeRect(b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3); }
        if (down) { ctx.fillStyle = 'rgba(0,0,0,0.12)'; ctx.fillRect(b.x, b.y, b.w, b.h); }
        if (b.pulse) { ctx.fillStyle = `rgba(255,255,255,${0.15 + 0.15 * Math.sin(now / 160)})`; ctx.fillRect(b.x, b.y, b.w, b.h); }
        if (b.icon) this.glyph(ctx, b, hold, b.iconSize ?? 30);
      } else if (b.kind === 'chip') {
        const x = b.x, y = b.y + (down ? 1 : 0);
        r.roundRect(x, y, b.w, b.h, 12, b.fill ?? 'rgba(255,244,214,0.88)', P.INK, 2);
        if (b.icon) this.glyph(ctx, { ...b, y }, hold, b.iconSize ?? 22);
      } else {
        const off = down ? 1 : 4;
        r.roundRect(b.x + off, b.y + off, b.w, b.h, 14, P.INK, null); r.roundRect(b.x + (4 - off), b.y + (4 - off), b.w, b.h, 14, b.fill ?? P.WHITE, P.INK, 3);
        if (b.icon) this.glyph(ctx, { ...b, x: b.x + (4 - off), y: b.y + (4 - off) }, hold, b.iconSize ?? 56);
      }
    }
  }

  /** The icon, with a green ring around it that fills while a hold button is held. */
  private glyph(ctx: CanvasRenderingContext2D, b: Btn, hold: number, size: number): void {
    const cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    drawIcon(ctx, b.icon!, cx, cy, size, b.disabled ? P.GREY : P.INK);
    if (b.holdMs) {
      const R = Math.min(b.w, b.h) / 2 - 3;
      ctx.save(); ctx.lineWidth = 3; ctx.strokeStyle = P.PAPER_DOT; ctx.beginPath(); ctx.arc(cx, cy, R, 0, Math.PI * 2); ctx.stroke();
      if (hold > 0) { ctx.strokeStyle = P.GREEN; ctx.lineCap = 'round'; ctx.beginPath(); ctx.arc(cx, cy, R, -Math.PI / 2, -Math.PI / 2 + hold * Math.PI * 2); ctx.stroke(); }
      ctx.restore();
    }
  }
}
