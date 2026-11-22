// main.ts - wires the modules together and runs the fixed 60 Hz loop.   (docs/system-design.md section 6.1)
import { Renderer } from './renderer';
import { Input } from './input';
import { UI } from './ui';
import { Audio } from './audio';
import { LevelStore, downloadLevel } from './level_store';
import { Camera, PRESETS, Preset } from './camera';
import { Game } from './modes';
import { VisionResult } from './vision';
import { decodeLevel, levelFromJSON, starterLevel, paintLine, fillRect, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY, isEmpty, LevelError } from './level';
import * as C from './config';

const canvas = document.getElementById('screen') as HTMLCanvasElement;
const renderer = new Renderer(canvas), input = new Input(canvas), ui = new UI(), audio = new Audio(), store = new LevelStore(), cam = new Camera();
const game = new Game(renderer, ui, audio, store, cam, input);
(window as unknown as { __game: Game; __cam: Camera }).__game = game; (window as unknown as { __cam: Camera }).__cam = cam;

// ---- the fixed-timestep loop: physics always advances in 1/60 s steps, whatever the display rate --------------------
const DT = 1 / C.PHYSICS_HZ;
let acc = 0, last = performance.now();
function frame(now: number): void {
  acc += Math.min((now - last) / 1000, 0.1); last = now;                 // clamp: no spiral of death after a stalled tab
  let ticks = 0;
  while (acc >= DT && ticks < 6) {                                       // catch up, but never more than 6 ticks
    game.tick(input.poll());                                             // tilt filtered per tick; touch edges latched
    input.consumeEdges();                                                // each tap/jump edge is seen by exactly one tick
    acc -= DT; ticks++;
  }
  game.draw();
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// ---- the controls under the screen ---------------------------------------------------------------------------------------
const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const note = (s: string) => { $('note').textContent = s; };

$('btn-tilt').addEventListener('click', async () => { const ok = await input.enableOrientation(); note(ok ? 'Phone tilt is on: tilt the phone to roll the ball.' : 'Tilt is not available here. Use the arrow keys.'); });
$('btn-webcam').addEventListener('click', async () => { const ok = await cam.startWebcam(); note(ok ? 'Webcam on. Hold the printed template up (or lean it on something) about 40 cm away.' : 'No webcam available. Try Upload photo or Demo photo.'); if (ok) game.openCheck(); });
$<HTMLInputElement>('file-photo').addEventListener('change', async (e) => {
  const f = (e.target as HTMLInputElement).files?.[0]; if (!f) return;
  try { await cam.loadPhoto(f); note('Photo loaded. Press the big GO button in the simulator to run the paper pipeline on it.'); game.openCheck(); } catch { note('That file could not be read as a picture.'); }
  (e.target as HTMLInputElement).value = '';
});
$('btn-demo').addEventListener('click', () => {
  // render the level you are working on (or a sample) onto the template, as a camera would see it
  let lv = game.currentLevel();
  if (isEmpty(lv)) { lv = starterLevel(); paintLine(lv, MAT_GROUND, 72, 116, 120, 130, 4); paintLine(lv, MAT_GROUND, 120, 130, 200, 124, 4); paintLine(lv, MAT_GROUND, 200, 124, 248, 116, 4); fillRect(lv, MAT_SPIKE, 110, 160, 160, 170); fillRect(lv, MAT_BOUNCY, 170, 160, 215, 170); }
  cam.setDemo(lv); note('Demo photo: your level drawn on the template, as the camera would see it. Try a stress preset, then press GO.'); game.openCheck();
});
const sel = $<HTMLSelectElement>('sel-preset');
sel.innerHTML = PRESETS.map((p) => `<option value="${p.id}">${p.label}</option>`).join('');
sel.addEventListener('change', () => cam.setPreset(sel.value as Preset));
$('btn-download').addEventListener('click', () => { game.saveCurrent(); downloadLevel(game.currentLevel()); note('Saved a .scrb level file (the device format). Copy it to the microSD card or load it back here.'); });
$<HTMLInputElement>('file-level').addEventListener('change', async (e) => {
  const f = (e.target as HTMLInputElement).files?.[0]; if (!f) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer());
    if (bytes[0] === 0x7b) { const lv = levelFromJSON(new TextDecoder().decode(bytes)); store.save(store.firstEmpty() >= 0 ? store.firstEmpty() : store.settings.lastSlot, lv); }
    else { decodeLevel(bytes); store.importBytes(bytes); }
    note('Level loaded into a slot.'); game.showPicker();
  } catch (err) { note(err instanceof LevelError ? 'Not a valid level: ' + err.message : 'That file could not be read.'); }
  (e.target as HTMLInputElement).value = '';
});

// ---- debug panel: what the vision pipeline saw (docs/system-design.md 9.3) ----------------------------------------------
const panels = ['dbg-page', 'dbg-ink', 'dbg-labels'].map((id) => $<HTMLCanvasElement>(id));
game.onVision = (res: VisionResult) => {
  const W = C.WORLD_W, H = C.WORLD_H, w = res.work;
  const draw = (cv: HTMLCanvasElement, f: (i: number) => [number, number, number]) => {
    cv.width = W; cv.height = H; const c = cv.getContext('2d')!, img = c.createImageData(W, H);
    for (let i = 0; i < W * H; i++) { const [r, g, b] = f(i); img.data[i * 4] = r; img.data[i * 4 + 1] = g; img.data[i * 4 + 2] = b; img.data[i * 4 + 3] = 255; }
    c.putImageData(img, 0, 0);
  };
  draw(panels[0], (i) => { const p = w.straight565[i]; return [((p >> 11) << 3) | (p >> 13), (((p >> 5) & 63) << 2) | ((p >> 9) & 3), ((p & 31) << 3) | ((p >> 2) & 7)]; });
  draw(panels[1], (i) => [255 - w.ink[i], 255 - w.ink[i], 255 - w.ink[i]]);
  draw(panels[2], (i) => { const l = w.labels[i]; return l === 1 ? [28, 28, 36] : l === 2 ? [240, 58, 46] : l === 3 ? [47, 125, 246] : [255, 244, 214]; });
  const st = ['OK', 'marks not found (centre-crop used)', 'orientation guessed', 'too dark', 'too little ink', 'too much ink', 'noisy'][res.report.status];
  $('dbg-text').textContent = `status: ${st} | marks: ${res.report.marksFound ? 'found' : 'no'} | ink coverage ${(res.report.inkCover * 100).toFixed(1)}% | paper brightness ${res.report.bgMean.toFixed(0)}`;
  $('debug').hidden = false;
};
