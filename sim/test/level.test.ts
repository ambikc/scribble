import { describe, it, expect } from 'vitest';
import * as C from '../src/config';
import { newLevel, starterLevel, lessonSpike, lessonBouncy, encodeLevel, decodeLevel, makeThumbnail, levelToJSON, levelFromJSON, paintDisc, paintLine, getBit, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY, LevelError, THUMB_BYTES, crc32, isEmpty, fillRect } from '../src/level';
import { LevelStore, blankLevel } from '../src/level_store';

class MemStorage implements Storage {
  private m = new Map<string, string>(); get length() { return this.m.size; }
  clear() { this.m.clear(); } getItem(k: string) { return this.m.get(k) ?? null; } key(i: number) { return [...this.m.keys()][i] ?? null; }
  removeItem(k: string) { this.m.delete(k); } setItem(k: string, v: string) { this.m.set(k, v); }
}

describe('level format (.scrb)', () => {
  const l = lessonSpike(); l.name = 'Spikes'; l.rev = 7; l.extras = [{ type: 1, x: 100, y: 60 }];
  it('has the documented size: 64 + 1080 + 3*7680 (+ extras)', () => {
    expect(encodeLevel(starterLevel(), true).length).toBe(64 + 1080 + 23040);
    expect(encodeLevel(starterLevel(), false).length).toBe(64 + 23040);
    expect(encodeLevel(l, true).length).toBe(64 + 1080 + 23040 + 1 + 5);
  });
  it('writes the documented header bytes', () => {
    const b = encodeLevel(l); const dv = new DataView(b.buffer);
    expect(String.fromCharCode(...b.slice(0, 4))).toBe('SCRB'); expect(b[4]).toBe(1); expect(b[5]).toBe(0);
    expect(dv.getUint16(6, true)).toBe(320); expect(dv.getUint16(8, true)).toBe(192);
    expect(dv.getUint16(10, true)).toBe(l.startX); expect(dv.getUint16(16, true)).toBe(l.flagY); expect(b[18]).toBe(3);
    expect(b[19] & 3).toBe(3);                                  // extras + thumbnail
    expect(new TextDecoder().decode(b.slice(20, 26))).toBe('Spikes'); expect(dv.getUint32(48, true)).toBe(7);
    expect(dv.getUint32(44, true)).toBe(crc32(b, 64, b.length));
  });
  it('round-trips everything', () => {
    const d = decodeLevel(encodeLevel(l)); const r = d.level;
    expect(r.ground).toEqual(l.ground); expect(r.spike).toEqual(l.spike); expect(r.bouncy).toEqual(l.bouncy);
    expect([r.startX, r.startY, r.flagX, r.flagY, r.name, r.rev]).toEqual([l.startX, l.startY, l.flagX, l.flagY, 'Spikes', 7]);
    expect(r.extras).toEqual(l.extras); expect(d.thumb!.length).toBe(THUMB_BYTES);
  });
  it('rejects bad files without crashing', () => {
    const good = encodeLevel(l); const bad = (f: (b: Uint8Array) => void, code: string) => { const b = good.slice(); f(b); try { decodeLevel(b); throw new Error('accepted'); } catch (e) { expect((e as LevelError).code).toBe(code); } };
    bad((b) => { b[0] = 0x58; }, 'magic'); bad((b) => { b[4] = 9; }, 'version'); bad((b) => { b[7000] ^= 1; }, 'crc'); bad((b) => { b[6] = 0; }, 'dims');
    expect(() => decodeLevel(good.slice(0, 5000))).toThrow(); expect(() => decodeLevel(new Uint8Array(10))).toThrow();
  });
  it('cuts long names on a character boundary (23 bytes max)', () => {
    const x = newLevel(); x.name = 'Dad’s challenge – the very very long one'; const d = decodeLevel(encodeLevel(x)); expect(new TextEncoder().encode(d.level.name).length).toBeLessThanOrEqual(23);
    expect(x.name.startsWith(d.level.name)).toBe(true);
  });
  it('JSON twin round-trips', () => { const r = levelFromJSON(levelToJSON(l)); expect(r.ground).toEqual(l.ground); expect(r.bouncy).toEqual(l.bouncy); expect(r.name).toBe('Spikes'); expect(r.extras).toEqual(l.extras); });
  it('bit layout: MSB first, 40 bytes per row', () => { const x = newLevel(); paintDisc(x, MAT_GROUND, 0.5, 0.5, 0.6); expect(x.ground[0]).toBe(0x80); expect(getBit(x.ground, 0, 0)).toBe(1); x.ground.fill(0); fillRect(x, MAT_GROUND, 8, 1, 9, 2); expect(x.ground[40 + 1]).toBe(0x80); });
  it('materials are exclusive', () => {
    const x = newLevel(); paintDisc(x, MAT_GROUND, 50, 50, 6); paintDisc(x, MAT_SPIKE, 50, 50, 3); paintDisc(x, MAT_BOUNCY, 52, 50, 2);
    for (let yy = 40; yy < 60; yy++) for (let xx = 40; xx < 60; xx++) expect(getBit(x.ground, xx, yy) + getBit(x.spike, xx, yy) + getBit(x.bouncy, xx, yy)).toBeLessThanOrEqual(1);
  });
  it('a fast stroke leaves no gaps', () => { const x = newLevel(); paintLine(x, MAT_GROUND, 10, 100, 300, 100, 4); for (let xx = 10; xx <= 300; xx++) expect(getBit(x.ground, xx, 100)).toBe(1); });
  it('thumbnail uses palette indices 0-8 and shows the materials', () => {
    const t = makeThumbnail(lessonBouncy()); const idx = new Set<number>(); for (const b of t) { idx.add(b >> 4); idx.add(b & 15); }
    expect([...idx].every((v) => v <= 8)).toBe(true); expect(idx.has(2)).toBe(true); expect(idx.has(4)).toBe(true); expect(idx.has(5)).toBe(true);
  });
  it('the three lessons are valid levels', () => { for (const lv of [starterLevel(), lessonSpike(), lessonBouncy()]) { expect(isEmpty(lv)).toBe(false); expect(() => decodeLevel(encodeLevel(lv))).not.toThrow(); } });
});

describe('level store', () => {
  it('first run fills slots 1-3 with the lessons, and keeps edits across a reload', () => {
    const st = new MemStorage(); let s = new LevelStore(st);
    expect(s.slots.filter(Boolean).length).toBe(3);
    const lv = blankLevel(); lv.name = 'Mine'; s.save(5, lv); s.markSolved(5); s.settings.lastSlot = 5; s.saveSettings();
    s = new LevelStore(st);
    expect(s.slots[5]?.name).toBe('Mine'); expect(s.isSolved(5)).toBe(true); expect(s.settings.lastSlot).toBe(5); expect(s.thumbs[5]!.length).toBe(THUMB_BYTES);
  });
  it('a corrupt slot is an empty slot with a warning, never a crash', () => {
    const st = new MemStorage(); new LevelStore(st); st.setItem('scribble.slot.1', btoa('garbage'));
    const s = new LevelStore(st); expect(s.slots[1]).toBeNull(); expect(s.broken[1]).toBe(true); expect(s.slots[0]).not.toBeNull();
  });
  it('delete frees the slot and clears its solved flag', () => { const s = new LevelStore(new MemStorage()); s.markSolved(0); s.delete(0); expect(s.slots[0]).toBeNull(); expect(s.isSolved(0)).toBe(false); expect(s.firstEmpty()).toBe(0); });
  it('imports a .scrb into the first empty slot', () => { const s = new LevelStore(new MemStorage()); const i = s.importBytes(encodeLevel(lessonSpike())); expect(i).toBe(3); expect(s.slots[3]).not.toBeNull(); });
  it('constants: layer size matches the world', () => { expect(C.ROW_BYTES * C.WORLD_H).toBe(C.LAYER_BYTES); expect(C.THUMB_W * C.THUMB_H / 2).toBe(1080); });
});
