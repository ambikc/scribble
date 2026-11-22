// level_store.ts - the ten level slots, settings and progress.   (platform adapter; the firmware uses LittleFS)
// Browser storage is localStorage (each slot is the .scrb bytes, base64). Same file format as the device, so
// "Download level" produces a file the device can load, and a .scrb from the device can be loaded here.
import * as C from './config';
import { Level, newLevel, cloneLevel, encodeLevel, decodeLevel, makeThumbnail, starterLevel, lessonSpike, lessonBouncy, fillRect, MAT_GROUND, LevelError, levelToJSON } from './level';

export interface Settings { sound: number; lastSlot: number; solvedMask: number; firstRunDone: boolean; standupSeen: boolean }

function toB64(bytes: Uint8Array): string { let s = ''; for (let i = 0; i < bytes.length; i += 0x2000) s += String.fromCharCode(...bytes.subarray(i, i + 0x2000)); return btoa(s); }
function fromB64(s: string): Uint8Array { const bin = atob(s); const o = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) o[i] = bin.charCodeAt(i); return o; }

/** A new blank level: a floor, a Start ball and a Flag, so Play works at once. */
export function blankLevel(): Level {
  const l = newLevel();
  fillRect(l, MAT_GROUND, 0, 168, C.WORLD_W, 176);
  l.startX = 40; l.startY = 160; l.flagX = 280; l.flagY = 168;
  return l;
}

/** localStorage can throw (private windows, sandboxed frames); then we simply run without saving. */
function safeStorage(): Storage | null { try { return typeof localStorage !== 'undefined' ? localStorage : null; } catch { return null; } }

export class LevelStore {
  slots: (Level | null)[] = Array(C.SLOTS).fill(null);
  thumbs: (Uint8Array | null)[] = Array(C.SLOTS).fill(null);
  broken: boolean[] = Array(C.SLOTS).fill(false);   // a corrupt file shows as an empty slot with a warning dot
  settings: Settings = { sound: 1, lastSlot: 0, solvedMask: 0, firstRunDone: false, standupSeen: false };
  private storage: Storage | null;

  constructor(storage: Storage | null = safeStorage()) { this.storage = storage; this.load(); }

  private get(k: string): string | null { try { return this.storage?.getItem(k) ?? null; } catch { return null; } }
  private set(k: string, v: string): boolean { try { this.storage?.setItem(k, v); return true; } catch { return false; } }

  load(): void {
    try { const s = this.get('scribble.settings'); if (s) this.settings = { ...this.settings, ...JSON.parse(s) }; } catch { /* keep defaults */ }
    let any = false;
    for (let i = 0; i < C.SLOTS; i++) {
      const b = this.get('scribble.slot.' + i);
      if (!b) continue;
      any = true;
      try { const d = decodeLevel(fromB64(b)); this.slots[i] = d.level; this.thumbs[i] = d.thumb ?? makeThumbnail(d.level); }
      catch (e) { if (e instanceof LevelError || e instanceof DOMException) this.broken[i] = true; else throw e; }
    }
    if (!any && !this.settings.firstRunDone) {            // factory lessons in slots 1-3 (docs/game-design.md section 2)
      [starterLevel(), lessonSpike(), lessonBouncy()].forEach((l, i) => this.save(i, l));
    }
  }

  /** Atomic enough for a browser: encode first, then a single setItem. Returns false if storage is full. */
  save(slot: number, level: Level): boolean {
    level.rev = (level.rev + 1) >>> 0;
    const bytes = encodeLevel(level, true);
    const ok = this.set('scribble.slot.' + slot, toB64(bytes));
    this.slots[slot] = cloneLevel(level); this.thumbs[slot] = makeThumbnail(level); this.broken[slot] = false;
    return ok;
  }
  delete(slot: number): void {
    try { this.storage?.removeItem('scribble.slot.' + slot); } catch { /* ignore */ }
    this.slots[slot] = null; this.thumbs[slot] = null; this.broken[slot] = false;
    this.settings.solvedMask &= ~(1 << slot); this.saveSettings();
  }
  firstEmpty(): number { const i = this.slots.findIndex((s) => s === null); return i; }
  isSolved(slot: number): boolean { return (this.settings.solvedMask & (1 << slot)) !== 0; }
  markSolved(slot: number): void { this.settings.solvedMask |= 1 << slot; this.saveSettings(); }
  saveSettings(): void { this.set('scribble.settings', JSON.stringify(this.settings)); }

  /** Put a loaded .scrb file into the first empty slot (or the given one). Throws LevelError if it is not valid. */
  importBytes(bytes: Uint8Array, slot = this.firstEmpty()): number {
    const d = decodeLevel(bytes);
    if (slot < 0) slot = this.settings.lastSlot;
    this.save(slot, d.level);
    return slot;
  }
}

function slug(level: Level, fallback: string): string { return (level.name || fallback).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || fallback; }

interface DownloadsCap { save(r: { filename: string; data: string | Blob }): Promise<{ status: string }> }

/**
 * Save a level to the user's disk. Returns a short message for the page to show.
 *  - normal page : the device format, `<name>.scrb`
 *  - hosted page : the viewer's sandbox blocks plain download links, so we use the `downloads` capability; it only allows
 *                  certain file types and `.scrb` is not one of them, so the hosted page saves the JSON twin (`<name>.json`).
 *                  `sim/scripts/json-to-scrb.ts` turns that into a `.scrb` for the device.
 */
export async function exportLevel(level: Level, fallbackName = 'my-level'): Promise<string> {
  const name = slug(level, fallbackName);
  const claude = (window as unknown as { claude?: { use?: (n: string) => Promise<unknown> } }).claude;
  if (claude?.use) {
    let cap: DownloadsCap | null = null;
    try { cap = (await claude.use('downloads')) as DownloadsCap | null; } catch { cap = null; }
    if (!cap) return 'Saving files is not available in this view.';
    try { await cap.save({ filename: name + '.json', data: levelToJSON(level) }); return `Saved ${name}.json (the level's JSON twin). Run sim/scripts/json-to-scrb.ts to make the device's .scrb.`; }
    catch (e) { const code = (e as { code?: string }).code; return code === 'declined' ? 'Save cancelled.' : 'Could not save the file here.'; }
  }
  const bytes = encodeLevel(level, true);
  const blob = new Blob([bytes as unknown as BlobPart], { type: 'application/octet-stream' });
  const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = name + '.scrb';
  document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  return `Saved ${name}.scrb (the device format). Copy it to the microSD card or load it back here.`;
}
