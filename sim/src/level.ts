// level.ts - the level data model and the .scrb file format.   (portable core: no I/O, no DOM)
// Mirrors firmware level.h/.cpp. Format spec: docs/system-design.md section 4.
//
//   World is 320 x 192. A layer is 1 bit per pixel, row-major, MSB first, 40 bytes per row.
//   Three layers: ground, spike, bouncy. They are EXCLUSIVE (a pixel is in at most one layer).
import * as C from './config';

// ---- materials ---------------------------------------------------------------
export const MAT_GROUND = 0;
export const MAT_SPIKE = 1;
export const MAT_BOUNCY = 2;
export type Material = 0 | 1 | 2;

export interface Extra { type: number; x: number; y: number } // type 1 = star (reserved, stretch goal)

export interface Level {
  ground: Uint8Array;
  spike: Uint8Array;
  bouncy: Uint8Array;
  startX: number; startY: number;   // ball centre at spawn
  flagX: number; flagY: number;     // pole base
  name: string;                     // up to 23 UTF-8 bytes
  rev: number;                      // +1 on every save
  fromPaper: boolean;               // flags bit2
  extras: Extra[];
}

export function newLevel(): Level {
  return {
    ground: new Uint8Array(C.LAYER_BYTES), spike: new Uint8Array(C.LAYER_BYTES), bouncy: new Uint8Array(C.LAYER_BYTES),
    startX: C.STARTER_START_X, startY: C.STARTER_START_Y, flagX: C.STARTER_FLAG_X, flagY: C.STARTER_FLAG_Y,
    name: '', rev: 0, fromPaper: false, extras: [],
  };
}

export function cloneLevel(l: Level): Level {
  return { ...l, ground: l.ground.slice(), spike: l.spike.slice(), bouncy: l.bouncy.slice(), extras: l.extras.map((e) => ({ ...e })) };
}

/** Copy `src` into `dst` in place (used by undo so references stay valid). */
export function copyLevelInto(dst: Level, src: Level): void {
  dst.ground.set(src.ground); dst.spike.set(src.spike); dst.bouncy.set(src.bouncy);
  dst.startX = src.startX; dst.startY = src.startY; dst.flagX = src.flagX; dst.flagY = src.flagY;
  dst.name = src.name; dst.rev = src.rev; dst.fromPaper = src.fromPaper; dst.extras = src.extras.map((e) => ({ ...e }));
}

export function layerOf(l: Level, m: Material): Uint8Array {
  return m === MAT_GROUND ? l.ground : m === MAT_SPIKE ? l.spike : l.bouncy;
}

// ---- bit access ----------------------------------------------------------------
// bit = (layer[y*40 + (x>>3)] >> (7 - (x&7))) & 1       (no bounds check: callers check)
export function getBit(layer: Uint8Array, x: number, y: number): number {
  return (layer[y * C.ROW_BYTES + (x >> 3)] >> (7 - (x & 7))) & 1;
}
export function setBit(layer: Uint8Array, x: number, y: number): void {
  layer[y * C.ROW_BYTES + (x >> 3)] |= 0x80 >> (x & 7);
}
export function clearBit(layer: Uint8Array, x: number, y: number): void {
  layer[y * C.ROW_BYTES + (x >> 3)] &= ~(0x80 >> (x & 7));
}
export function inWorld(x: number, y: number): boolean {
  return x >= 0 && y >= 0 && x < C.WORLD_W && y < C.WORLD_H;
}

/** 0 = nothing, 1 + Material otherwise. Out of world = 0. */
export function materialAt(l: Level, x: number, y: number): number {
  if (!inWorld(x, y)) return 0;
  if (getBit(l.ground, x, y)) return 1 + MAT_GROUND;
  if (getBit(l.spike, x, y)) return 1 + MAT_SPIKE;
  if (getBit(l.bouncy, x, y)) return 1 + MAT_BOUNCY;
  return 0;
}

// ---- painting (finger mode, built-in levels, pin clearing) ----------------------
/** Stamp a disc of material `m`; clears the other two layers there (materials are exclusive). */
export function paintDisc(l: Level, m: Material, cx: number, cy: number, r: number): void {
  const x0 = Math.max(0, Math.floor(cx - r)), x1 = Math.min(C.WORLD_W - 1, Math.ceil(cx + r));
  const y0 = Math.max(0, Math.floor(cy - r)), y1 = Math.min(C.WORLD_H - 1, Math.ceil(cy + r));
  const own = layerOf(l, m);
  const others = [MAT_GROUND, MAT_SPIKE, MAT_BOUNCY].filter((k) => k !== m).map((k) => layerOf(l, k as Material));
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
    const dx = x + 0.5 - cx, dy = y + 0.5 - cy;
    if (dx * dx + dy * dy <= r * r) { setBit(own, x, y); for (const o of others) clearBit(o, x, y); }
  }
}

/** Erase all three materials in a disc. */
export function eraseDisc(l: Level, cx: number, cy: number, r: number): void {
  const x0 = Math.max(0, Math.floor(cx - r)), x1 = Math.min(C.WORLD_W - 1, Math.ceil(cx + r));
  const y0 = Math.max(0, Math.floor(cy - r)), y1 = Math.min(C.WORLD_H - 1, Math.ceil(cy + r));
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
    const dx = x + 0.5 - cx, dy = y + 0.5 - cy;
    if (dx * dx + dy * dy <= r * r) { clearBit(l.ground, x, y); clearBit(l.spike, x, y); clearBit(l.bouncy, x, y); }
  }
}

/** Brush stroke from (x0,y0) to (x1,y1): discs at most STAMP_SPACING apart so fast strokes leave no gaps.
 *  `m` = null erases. Returns the bounding box touched (for dirty-rect rendering). */
export function paintLine(l: Level, m: Material | null, x0: number, y0: number, x1: number, y1: number, r: number): void {
  const dist = Math.hypot(x1 - x0, y1 - y0);
  const n = Math.max(1, Math.ceil(dist / C.STAMP_SPACING));
  for (let i = 0; i <= n; i++) {
    const t = i / n, x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t;
    if (m === null) eraseDisc(l, x, y, r); else paintDisc(l, m, x, y, r);
  }
}

export function fillRect(l: Level, m: Material, x0: number, y0: number, x1: number, y1: number): void {
  const own = layerOf(l, m);
  for (let y = Math.max(0, y0); y < Math.min(C.WORLD_H, y1); y++) for (let x = Math.max(0, x0); x < Math.min(C.WORLD_W, x1); x++) {
    setBit(own, x, y);
    for (const k of [MAT_GROUND, MAT_SPIKE, MAT_BOUNCY]) if (k !== m) clearBit(layerOf(l, k as Material), x, y);
  }
}

/** Clear all material in a disc, so a dropped pin is never buried (docs/game-design.md 4.3). */
export function clearAround(l: Level, cx: number, cy: number, r: number): void { eraseDisc(l, cx, cy, r); }

export function isEmpty(l: Level): boolean {
  for (let i = 0; i < C.LAYER_BYTES; i++) if (l.ground[i] | l.spike[i] | l.bouncy[i]) return false;
  return true;
}

export function countBits(layer: Uint8Array): number {
  let n = 0;
  for (let i = 0; i < layer.length; i++) { let b = layer[i]; while (b) { n += b & 1; b >>= 1; } }
  return n;
}

// ---- CRC-32 (IEEE) -----------------------------------------------------------------
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
export function crc32(bytes: Uint8Array, start = 0, end = bytes.length): number {
  let c = 0xffffffff;
  for (let i = start; i < end; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// ---- thumbnails (60 x 36, 4 bits per pixel, palette index; 1,080 bytes) -----------------
export const THUMB_BYTES = (C.THUMB_W * C.THUMB_H) / 2; // 1080
const PI_PAPER = 0, PI_INK = 2, PI_RED = 3, PI_BLUE = 4, PI_GREEN = 5;

export function makeThumbnail(l: Level): Uint8Array {
  const out = new Uint8Array(THUMB_BYTES);
  const idx = new Uint8Array(C.THUMB_W * C.THUMB_H);
  const sx = C.WORLD_W / C.THUMB_W, sy = C.WORLD_H / C.THUMB_H; // 5.333 each
  for (let ty = 0; ty < C.THUMB_H; ty++) for (let tx = 0; tx < C.THUMB_W; tx++) {
    const x0 = Math.floor(tx * sx), x1 = Math.max(x0 + 1, Math.floor((tx + 1) * sx));
    const y0 = Math.floor(ty * sy), y1 = Math.max(y0 + 1, Math.floor((ty + 1) * sy));
    let g = 0, s = 0, b = 0, n = 0;
    for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) { n++; g += getBit(l.ground, x, y); s += getBit(l.spike, x, y); b += getBit(l.bouncy, x, y); }
    // priority spike > bouncy > ground > paper, a material wins when >= 25 % of the block is set
    idx[ty * C.THUMB_W + tx] = s * 4 >= n ? PI_RED : b * 4 >= n ? PI_BLUE : g * 4 >= n ? PI_INK : PI_PAPER;
  }
  // Start and Flag as small green dots
  for (const [px, py] of [[l.startX, l.startY], [l.flagX, l.flagY - C.FLAG_CENTER_DY]]) {
    const tx = Math.floor(px / sx), ty = Math.floor(py / sy);
    for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++) {
      const X = tx + dx - 0, Y = ty + dy - 0;
      if (X >= 0 && Y >= 0 && X < C.THUMB_W && Y < C.THUMB_H) idx[Y * C.THUMB_W + X] = PI_GREEN;
    }
  }
  for (let i = 0; i < idx.length; i += 2) out[i >> 1] = (idx[i] << 4) | idx[i + 1]; // high nibble first
  return out;
}

// ---- .scrb encode / decode --------------------------------------------------------------
const HEADER = 64;
const FLAG_EXTRAS = 1, FLAG_THUMB = 2, FLAG_PAPER = 4;

export class LevelError extends Error {
  constructor(public code: 'magic' | 'version' | 'size' | 'crc' | 'dims' | 'range', msg: string) { super(msg); }
}

export function encodeLevel(l: Level, withThumbnail = true): Uint8Array {
  const extras = l.extras.slice(0, 8);
  const thumb = withThumbnail ? makeThumbnail(l) : null;
  const total = HEADER + (thumb ? THUMB_BYTES : 0) + 3 * C.LAYER_BYTES + (extras.length ? 1 + 5 * extras.length : 0);
  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  out.set([0x53, 0x43, 0x52, 0x42], 0);          // "SCRB"
  out[4] = 1;                                    // version
  out[5] = 0;                                    // encoding: raw
  dv.setUint16(6, C.WORLD_W, true); dv.setUint16(8, C.WORLD_H, true);
  dv.setUint16(10, l.startX, true); dv.setUint16(12, l.startY, true);
  dv.setUint16(14, l.flagX, true); dv.setUint16(16, l.flagY, true);
  out[18] = 3;                                   // layerCount
  out[19] = (extras.length ? FLAG_EXTRAS : 0) | (thumb ? FLAG_THUMB : 0) | (l.fromPaper ? FLAG_PAPER : 0);
  const nameBytes = new TextEncoder().encode(l.name);
  let nb = Math.min(nameBytes.length, 23);
  while (nb > 0 && (nameBytes[nb] & 0xc0) === 0x80 && nb < nameBytes.length) nb--; // do not cut a UTF-8 character
  out.set(nameBytes.subarray(0, nb), 20);        // NUL-padded by the zero fill
  dv.setUint32(48, l.rev >>> 0, true);
  let p = HEADER;
  if (thumb) { out.set(thumb, p); p += THUMB_BYTES; }
  for (const layer of [l.ground, l.spike, l.bouncy]) { out.set(layer, p); p += C.LAYER_BYTES; }
  if (extras.length) {
    out[p++] = extras.length;
    for (const e of extras) { out[p++] = e.type; dv.setUint16(p, e.x, true); dv.setUint16(p + 2, e.y, true); p += 4; }
  }
  dv.setUint32(44, crc32(out, HEADER, total), true);
  return out;
}

export interface Decoded { level: Level; thumb: Uint8Array | null }

/** Validates magic, version, size, CRC, dimensions, ranges. Throws LevelError. */
export function decodeLevel(bytes: Uint8Array): Decoded {
  if (bytes.length < HEADER) throw new LevelError('size', 'file too short');
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes[0] !== 0x53 || bytes[1] !== 0x43 || bytes[2] !== 0x52 || bytes[3] !== 0x42) throw new LevelError('magic', 'not a Scribble Run level');
  if (bytes[4] > 1) throw new LevelError('version', 'level from a newer game');
  if (bytes[5] !== 0) throw new LevelError('version', 'unsupported encoding');
  if (dv.getUint16(6, true) !== C.WORLD_W || dv.getUint16(8, true) !== C.WORLD_H || bytes[18] !== 3) throw new LevelError('dims', 'wrong size');
  const flags = bytes[19];
  const hasThumb = (flags & FLAG_THUMB) !== 0, hasExtras = (flags & FLAG_EXTRAS) !== 0;
  let need = HEADER + (hasThumb ? THUMB_BYTES : 0) + 3 * C.LAYER_BYTES;
  if (bytes.length < need + (hasExtras ? 1 : 0)) throw new LevelError('size', 'file truncated');
  let nExtra = 0;
  if (hasExtras) { nExtra = bytes[need]; need += 1 + 5 * nExtra; if (nExtra > 8 || bytes.length < need) throw new LevelError('size', 'bad extras'); }
  if (bytes.length !== need) throw new LevelError('size', 'unexpected length');
  if (crc32(bytes, HEADER, bytes.length) !== dv.getUint32(44, true)) throw new LevelError('crc', 'checksum mismatch');
  const l = newLevel();
  l.startX = dv.getUint16(10, true); l.startY = dv.getUint16(12, true);
  l.flagX = dv.getUint16(14, true); l.flagY = dv.getUint16(16, true);
  if (l.startX >= C.WORLD_W || l.startY >= C.WORLD_H || l.flagX >= C.WORLD_W || l.flagY >= C.WORLD_H) throw new LevelError('range', 'pin outside the world');
  let end = 20; while (end < 44 && bytes[end] !== 0) end++;
  l.name = new TextDecoder().decode(bytes.subarray(20, end));
  l.rev = dv.getUint32(48, true);
  l.fromPaper = (flags & FLAG_PAPER) !== 0;
  let p = HEADER; let thumb: Uint8Array | null = null;
  if (hasThumb) { thumb = bytes.slice(p, p + THUMB_BYTES); p += THUMB_BYTES; }
  l.ground.set(bytes.subarray(p, p + C.LAYER_BYTES)); p += C.LAYER_BYTES;
  l.spike.set(bytes.subarray(p, p + C.LAYER_BYTES)); p += C.LAYER_BYTES;
  l.bouncy.set(bytes.subarray(p, p + C.LAYER_BYTES)); p += C.LAYER_BYTES;
  if (hasExtras) { p++; for (let i = 0; i < nExtra; i++) { l.extras.push({ type: bytes[p], x: dv.getUint16(p + 1, true), y: dv.getUint16(p + 3, true) }); p += 5; } }
  return { level: l, thumb };
}

// ---- JSON twin (for the web tools; docs/system-design.md 4.4) ------------------------------
function b64(bytes: Uint8Array): string { let s = ''; for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]); return btoa(s); }
function unb64(s: string): Uint8Array { const bin = atob(s); const o = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) o[i] = bin.charCodeAt(i); return o; }

export function levelToJSON(l: Level): string {
  return JSON.stringify({
    format: 'scribble-run-level', version: 1, name: l.name, origin: l.fromPaper ? 'paper' : 'finger',
    width: C.WORLD_W, height: C.WORLD_H, start: { x: l.startX, y: l.startY }, flag: { x: l.flagX, y: l.flagY },
    layers: { ground: b64(l.ground), spike: b64(l.spike), bouncy: b64(l.bouncy) },
    thumbnail: { w: C.THUMB_W, h: C.THUMB_H, format: '4bpp-palette', data: b64(makeThumbnail(l)) },
    extras: l.extras.map((e) => ({ type: e.type === 1 ? 'star' : e.type, x: e.x, y: e.y })),
  }, null, 2);
}

export function levelFromJSON(text: string): Level {
  const j = JSON.parse(text);
  if (j.format !== 'scribble-run-level' || j.version !== 1 || j.width !== C.WORLD_W || j.height !== C.WORLD_H) throw new LevelError('dims', 'not a level JSON');
  const l = newLevel();
  const g = unb64(j.layers.ground), s = unb64(j.layers.spike), b = unb64(j.layers.bouncy);
  if (g.length !== C.LAYER_BYTES || s.length !== C.LAYER_BYTES || b.length !== C.LAYER_BYTES) throw new LevelError('size', 'bad layer');
  l.ground.set(g); l.spike.set(s); l.bouncy.set(b);
  l.startX = j.start.x; l.startY = j.start.y; l.flagX = j.flag.x; l.flagY = j.flag.y;
  l.name = String(j.name ?? ''); l.fromPaper = j.origin === 'paper';
  l.extras = (j.extras ?? []).map((e: { type: unknown; x: number; y: number }) => ({ type: e.type === 'star' ? 1 : Number(e.type), x: e.x, y: e.y }));
  return l;
}

// ---- built-in levels (the three "lessons"; docs/game-design.md 2) -----------------------------
export function starterLevel(): Level {
  const l = newLevel();
  l.name = 'Bridge';
  fillRect(l, MAT_GROUND, 16, 112, 72, 120);
  fillRect(l, MAT_GROUND, 248, 112, 304, 120);
  return l;
}

export function lessonSpike(): Level {
  const l = newLevel();
  l.name = 'Spikes';
  fillRect(l, MAT_GROUND, 16, 112, 72, 120);
  fillRect(l, MAT_GROUND, 248, 112, 304, 120);
  fillRect(l, MAT_GROUND, 120, 104, 200, 160);   // a hill in the middle
  fillRect(l, MAT_SPIKE, 120, 94, 200, 104);     // with a red patch on top
  return l;
}

export function lessonBouncy(): Level {
  const l = newLevel();
  l.name = 'Bounce';
  l.startX = 40; l.startY = 142;
  fillRect(l, MAT_GROUND, 16, 150, 72, 158);
  fillRect(l, MAT_GROUND, 72, 166, 100, 174);   // floor ...
  fillRect(l, MAT_GROUND, 164, 166, 196, 174);
  fillRect(l, MAT_BOUNCY, 100, 166, 164, 174);   // ... with a blue pad flush in the middle
  fillRect(l, MAT_GROUND, 232, 70, 304, 78);     // a ledge too high to jump to
  l.flagX = 280; l.flagY = 70;
  return l;
}
