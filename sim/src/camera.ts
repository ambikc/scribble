// camera.ts - where camera frames come from in the simulator.   (platform adapter; the firmware uses esp32-camera)
//
//   webcam : getUserMedia, raw un-mirrored frames (like the CoreS3 sensor)
//   photo  : an uploaded picture of a printed, drawn template (a phone photo works)
//   demo   : a synthetic photo made by synth.ts (no paper, no camera needed)
//
// Every source ends up as a 640 x 480 RGB565 frame, exactly what the device's vision code receives.
import * as C from './config';
import { Level } from './level';
import { renderTemplateShot, ShotOptions } from './synth';
import VisionWorker from './vision.worker?worker&inline';
import { pack565, VisionParams, VisionResult, VisionWork, visionProcess, visionRethreshold } from './vision';

export type CamSource = 'none' | 'webcam' | 'photo' | 'demo';
export type Preset = 'none' | 'dim' | 'uneven' | 'shadow' | 'noise' | 'blur' | 'tilt15' | 'far' | 'near' | 'upside';
export const PRESETS: { id: Preset; label: string }[] = [
  { id: 'none', label: 'No stress' }, { id: 'dim', label: 'Dim light' }, { id: 'uneven', label: 'Uneven light' }, { id: 'shadow', label: 'Hard shadow' },
  { id: 'noise', label: 'Sensor noise' }, { id: 'blur', label: 'Out of focus' }, { id: 'tilt15', label: 'Sheet tilted 15°' },
  { id: 'far', label: 'Far (47 cm)' }, { id: 'near', label: 'Near (36 cm)' }, { id: 'upside', label: 'Upside down' },
];

/** The synthetic camera model for each preset (physical: distance, tilt, light). */
function shotOptions(p: Preset): ShotOptions {
  switch (p) {
    case 'dim': return { brightness: 0.5 }; case 'uneven': return { gradient: 0.4 }; case 'shadow': return { shadow: true };
    case 'noise': return { noise: 12 }; case 'blur': return { blurPx: 3 }; case 'tilt15': return { yawDeg: 15, pitchDeg: -5 };
    case 'far': return { distanceCm: 47 }; case 'near': return { distanceCm: 36 }; case 'upside': return { rollDeg: 180 };
    default: return {};
  }
}

export class Camera {
  source: CamSource = 'none';
  preset: Preset = 'none';
  version = 0;                       // bumps whenever the frame changes (so the live check only re-runs when needed)
  private frame: Uint16Array | null = null;
  private video: HTMLVideoElement | null = null; private stream: MediaStream | null = null;
  private canvas = document.createElement('canvas'); private cx: CanvasRenderingContext2D;
  private photo: HTMLImageElement | null = null;
  private demoLevel: Level | null = null;
  private lastPull = 0;
  private worker: Worker | null = null; private nextId = 1; private pending = new Map<number, (r: unknown) => void>();

  constructor() {
    this.canvas.width = C.CAM_W; this.canvas.height = C.CAM_H;
    this.cx = this.canvas.getContext('2d', { willReadFrequently: true })!;
    try { this.worker = new VisionWorker();                       // inlined blob worker; if it cannot start we use this thread
      this.worker.onerror = () => { this.worker = null; this.pending.forEach((f) => f(null)); this.pending.clear(); };
      this.worker.onmessage = (e) => { const f = this.pending.get(e.data.id); if (f) { this.pending.delete(e.data.id); f(e.data.result); } }; } catch { this.worker = null; }
  }

  get hasFrame(): boolean { return this.source !== 'none' && this.frame !== null; }

  // ---- sources -----------------------------------------------------------------------------
  async startWebcam(): Promise<boolean> {
    try {
      this.stream = await navigator.mediaDevices.getUserMedia({ video: { width: { ideal: 640 }, height: { ideal: 480 } }, audio: false });
      this.video = document.createElement('video'); this.video.srcObject = this.stream; this.video.muted = true; this.video.playsInline = true;
      await this.video.play(); this.source = 'webcam'; this.pull(true); return true;
    } catch { return false; }
  }
  stopWebcam(): void { this.stream?.getTracks().forEach((t) => t.stop()); this.stream = null; this.video = null; if (this.source === 'webcam') { this.source = 'none'; this.frame = null; } }

  async loadPhoto(file: File): Promise<void> {
    const img = new Image(); const url = URL.createObjectURL(file);
    await new Promise<void>((res, rej) => { img.onload = () => res(); img.onerror = () => rej(new Error('bad image')); img.src = url; });
    this.stopWebcam(); this.photo = img; this.source = 'photo'; this.renderPhoto(); URL.revokeObjectURL(url);
  }

  setDemo(level: Level): void { this.stopWebcam(); this.demoLevel = level; this.source = 'demo'; this.renderDemo(); }

  setPreset(p: Preset): void { this.preset = p; if (this.source === 'photo') this.renderPhoto(); else if (this.source === 'demo') this.renderDemo(); }

  private renderDemo(): void { if (this.demoLevel) { this.frame = renderTemplateShot(this.demoLevel, { seed: 5, ...shotOptions(this.preset) }); this.version++; } }

  /** Draw the photo cover-fitted into 640 x 480 (centre-crop to 4:3), apply a stress preset, quantise to RGB565. */
  private renderPhoto(): void {
    if (!this.photo) return;
    const { cx } = this, w = C.CAM_W, h = C.CAM_H, iw = this.photo.naturalWidth, ih = this.photo.naturalHeight;
    const s = Math.max(w / iw, h / ih), dw = iw * s, dh = ih * s;
    cx.save(); cx.fillStyle = '#464038'; cx.fillRect(0, 0, w, h);
    cx.translate(w / 2, h / 2);
    if (this.preset === 'tilt15') cx.rotate((12 * Math.PI) / 180); else if (this.preset === 'upside') cx.rotate(Math.PI);
    const z = this.preset === 'far' ? 0.88 : this.preset === 'near' ? 1.12 : 1; cx.scale(z, z);
    if (this.preset === 'blur') cx.filter = 'blur(3px)';
    cx.drawImage(this.photo, -dw / 2, -dh / 2, dw, dh); cx.restore();
    this.frame = this.quantise(); this.version++;
  }

  /** Pull a video frame into the 640 x 480 canvas (cover-fit) and quantise it. Throttled to ~15 fps. */
  private pull(force = false): void {
    if (!this.video || this.video.readyState < 2) return;
    const now = performance.now(); if (!force && now - this.lastPull < 66) return; this.lastPull = now;
    const vw = this.video.videoWidth, vh = this.video.videoHeight, s = Math.max(C.CAM_W / vw, C.CAM_H / vh);
    this.cx.save(); this.cx.filter = this.preset === 'blur' ? 'blur(3px)' : 'none';
    this.cx.drawImage(this.video, (C.CAM_W - vw * s) / 2, (C.CAM_H - vh * s) / 2, vw * s, vh * s); this.cx.restore();
    this.frame = this.quantise(); this.version++;
  }

  private quantise(): Uint16Array {
    const id = this.cx.getImageData(0, 0, C.CAM_W, C.CAM_H), d = id.data, out = new Uint16Array(C.CAM_W * C.CAM_H);
    const p = this.preset;
    let seed = 99;
    for (let i = 0, j = 0; i < out.length; i++, j += 4) {
      let r = d[j], g = d[j + 1], b = d[j + 2];
      if (this.source === 'webcam' || this.source === 'photo') {                      // presets that act on pixels
        const x = i % C.CAM_W, y = (i / C.CAM_W) | 0;
        let k = 1;
        if (p === 'dim') k = 0.5; else if (p === 'uneven') k = 1 - 0.4 * (x / C.CAM_W); else if (p === 'shadow' && x < 150 + 0.35 * y) k = 0.62;
        r *= k; g *= k; b *= k;
        if (p === 'noise') { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; const n = ((seed >>> 16) / 65536 - 0.5) * 36; r += n; g += n; b += n; }
      }
      out[i] = pack565(Math.max(0, Math.min(255, r | 0)), Math.max(0, Math.min(255, g | 0)), Math.max(0, Math.min(255, b | 0)));
    }
    return out;
  }

  /** The newest frame (the webcam is refreshed on demand). */
  latest(): Uint16Array | null { if (this.source === 'webcam') this.pull(); return this.frame; }

  /** At "snap": average FRAME_AVG frames for the webcam (the device stands still); static sources are returned as they are. */
  async snap(): Promise<Uint16Array | null> {
    if (this.source !== 'webcam') return this.frame ? this.frame.slice() : null;
    const acc = new Uint32Array(C.CAM_W * C.CAM_H * 3);
    for (let n = 0; n < C.FRAME_AVG; n++) {
      this.pull(true); const f = this.frame!;
      for (let i = 0, j = 0; i < f.length; i++, j += 3) { const p = f[i]; acc[j] += ((p >> 11) << 3) | (p >> 13); acc[j + 1] += (((p >> 5) & 63) << 2) | ((p >> 9) & 3); acc[j + 2] += ((p & 31) << 3) | ((p >> 2) & 7); }
      await new Promise((r) => setTimeout(r, 34));
    }
    const out = new Uint16Array(C.CAM_W * C.CAM_H);
    for (let i = 0, j = 0; i < out.length; i++, j += 3) out[i] = pack565(Math.round(acc[j] / C.FRAME_AVG), Math.round(acc[j + 1] / C.FRAME_AVG), Math.round(acc[j + 2] / C.FRAME_AVG));
    return out;
  }

  // ---- vision runs in a worker (like the firmware's vision task); falls back to this thread ---------------------
  private ask<T>(msg: Record<string, unknown>, transfer: Transferable[], fallback: () => T): Promise<T> {
    if (!this.worker) return new Promise((r) => setTimeout(() => r(fallback()), 0));
    const id = this.nextId++;
    return new Promise((res) => {
      this.pending.set(id, (r) => res(r === null ? fallback() : (r as T)));      // a dead worker answers null: do it here instead
      this.worker!.postMessage({ id, ...msg }, transfer);
    });
  }
  processFrame(vga: Uint16Array, params: VisionParams): Promise<VisionResult> {
    const copy = vga.slice();
    return this.ask({ kind: 'process', vga: copy, params }, [copy.buffer], () => visionProcess(vga, params));
  }
  rethreshold(work: VisionWork, params: VisionParams): Promise<VisionResult> {
    return this.ask({ kind: 'rethreshold', work, params }, [], () => visionRethreshold(work, params));
  }
}
