// modes.ts - the state machine and every screen.   (app layer; the only module that knows all the others)
// Mirrors firmware modes.cpp. States and transitions: docs/system-design.md section 3; screens: docs/screens.md.
//
//   title -> picker | chooser | standup | check | draw | play
//   check -> countdown -> processing -> review -> play        draw <-> play -> win        menu overlays draw/play
//
// One rule keeps this readable: every tick a screen (1) adds its buttons to the UI, (2) lets the UI decide which
// button fired, (3) reacts. draw() then paints the same screen. Physics and vision are pure and called from here.
import * as C from './config';
import { Input, InputState } from './input';
import { Renderer, FONT } from './renderer';
import { UI, Btn, IconName, drawIcon } from './ui';
import { Audio } from './audio';
import { LevelStore, blankLevel } from './level_store';
import { Camera } from './camera';
import { Level, cloneLevel, copyLevelInto, paintLine, paintDisc, eraseDisc, clearAround, Material, MAT_GROUND, MAT_SPIKE, MAT_BOUNCY } from './level';
import { World, newWorld, physicsStep, EV_JUMP, EV_LAND, EV_BOUNCE, EV_POP, EV_FELL, EV_RESPAWN, EV_WIN, ST_DYING, ST_CELEBRATING, ST_ROLLING } from './physics';
import { liveCheck, grayQvga, LiveCheck, VisionResult, VisionParams, defaultParams, Q_OK, Q_ORIENT_GUESS, Q_MARKS_MISSING, Q_EMPTY, Q_TOO_DARK, Q_TOO_MUCH_INK } from './vision';

const P = C.PALETTE_HEX;
const DT = 1 / C.PHYSICS_HZ;
export type Screen = 'title' | 'picker' | 'chooser' | 'standup' | 'check' | 'countdown' | 'processing' | 'review' | 'draw' | 'play' | 'win' | 'menu';
type Tool = 'ground' | 'spike' | 'bouncy' | 'eraser';

interface Particle { x: number; y: number; vx: number; vy: number; life: number; max: number; color: string; size: number; kind: 'sq' | 'circ' | 'ring' | 'conf' }
const BAR = C.SCREEN_H - C.BAR_H; // y of the bottom bar = 192

export class Game {
  screen: Screen = 'title';
  private screenT = 0; private transition = 0; private nowMs = 0; private t = 0;
  private menuFrom: 'draw' | 'play' = 'draw';
  private cameFrom: Screen = 'title';
  // current level
  slot = 0; pendingSlot = -1; level: Level = blankLevel(); private undo: Level[] = []; private tool: Tool = 'ground';
  private dirtySave = false; private lastEditAt = 0; private levelIsStarter = false;
  // drawing
  private stroke = { active: false, lx: 0, ly: 0, dist: 0 }; private pinDrag: null | 'start' | 'flag' = null; private ghostDone = false; private idleSince = 0;
  // play
  world!: World; private fxStretch = 0; private fxSquash = 0; private fxShake = 0; private fxRed = 0; private fxFlash = 0; private fxFlashColor: string = P.WHITE;
  private parts: Particle[] = []; private puffs: { x: number; y: number; t: number }[] = []; private winT = 0; private tiltHint = 0; private spawnRing = 0; private everPlayed = false;
  // picker
  private deleteMode = false;
  // camera flow
  private lc: LiveCheck | null = null; private lcVersion = -1; private lcAt = 0; private greenFor = 0; private debugNums = false;
  private cd = { t: 0, said: [false, false, false, false], snapped: false };
  private snapFrame: Uint16Array | null = null;
  private review: { res: VisionResult; params: VisionParams; reveal: number; busy: boolean } | null = null;
  private procToken = 0;
  private toast = { text: '', t: 0 };
  /** The page shows the vision stages in a debug panel; it hooks in here. */
  onVision: ((res: VisionResult) => void) | null = null;

  constructor(readonly r: Renderer, readonly ui: UI, readonly audio: Audio, readonly store: LevelStore, readonly cam: Camera, readonly input: Input) {
    audio.volume = store.settings.sound;
    this.level = blankLevel();
    this.world = newWorld(this.level);
  }

  // ===================================================================================================
  // helpers
  // ===================================================================================================
  go(s: Screen): void { this.screen = s; this.screenT = 0; this.transition = 1; this.stroke.active = false; this.pinDrag = null; if (s === 'draw') { this.puffs = []; this.idleSince = this.nowMs; } }
  private say(text: string): void { this.toast = { text, t: 2.2 }; }

  private chip(id: string, icon: IconName, x: number, extra: Partial<Btn> = {}): void {
    this.ui.add({ id, x, y: 4, w: 40, h: 40, kind: 'chip', icon, hit: 4, ...extra });
  }
  private chipBack(extra: Partial<Btn> = {}, icon: IconName = 'back'): void { this.chip('back', icon, 4, extra); }
  private chipMenu(): void { this.chip('menu', 'menu', 276); }
  /** Lay tabs out left to right along the bottom bar. widths must add to 320. */
  private bar(items: (Omit<Btn, 'x' | 'y' | 'h' | 'kind'> & { w: number })[]): void {
    let x = 0; for (const it of items) { this.ui.add({ ...it, x, y: BAR, h: C.BAR_H, kind: 'tab' }); x += it.w; }
  }
  private topBar(titleIcon?: IconName, right?: { id: string; icon: IconName; active?: boolean }): void {
    this.ui.add({ id: 'topbar', x: 0, y: 0, w: 320, h: 48, kind: 'custom', disabled: true, draw: (ctx) => {
      this.r.roundRect(0, 0, 320, 48, 0, P.WHITE, null); ctx.fillStyle = P.INK; ctx.fillRect(0, 46, 320, 2);
      if (titleIcon) drawIcon(ctx, titleIcon, 160, 24, 28);
    } });
    this.ui.add({ id: 'back', x: 0, y: 0, w: 48, h: 48, kind: 'tab', icon: 'back', iconSize: 26, fill: P.WHITE, edge: 'bottom' });
    if (right) this.ui.add({ id: right.id, x: 272, y: 0, w: 48, h: 48, kind: 'tab', icon: right.icon, iconSize: 26, fill: right.active ? P.RED : P.WHITE, active: right.active, edge: 'bottom' });
  }
  private sndIcon(): IconName { return (['sound0', 'sound1', 'sound2'] as const)[this.audio.volume]; }
  private cycleSound(): void { this.store.settings.sound = this.audio.cycleVolume(); this.store.saveSettings(); }

  private spawn(n: number, x: number, y: number, colors: string[], speed: number, life: number, size = 2, kind: Particle['kind'] = 'sq'): void {
    for (let i = 0; i < n; i++) { const a = Math.random() * Math.PI * 2, s = speed * (0.4 + Math.random() * 0.8);
      this.parts.push({ x, y, vx: Math.cos(a) * s, vy: Math.sin(a) * s - (kind === 'conf' ? speed * 0.6 : 0), life, max: life, color: colors[(Math.random() * colors.length) | 0], size, kind }); }
  }
  private updateFx(dt: number): void {
    for (const p of this.parts) { p.x += p.vx * dt; p.y += p.vy * dt; p.vy += (p.kind === 'conf' ? 180 : p.kind === 'ring' ? 0 : 300) * dt; p.life -= dt; }
    this.parts = this.parts.filter((p) => p.life > 0).slice(-96);
    this.fxStretch = Math.max(0, this.fxStretch - dt); this.fxSquash = Math.max(0, this.fxSquash - dt);
    this.fxShake = Math.max(0, this.fxShake - dt); this.fxRed = Math.max(0, this.fxRed - dt); this.fxFlash = Math.max(0, this.fxFlash - dt);
    this.spawnRing = Math.max(0, this.spawnRing - dt); this.toast.t = Math.max(0, this.toast.t - dt);
    this.transition = Math.max(0, this.transition - dt / 0.15);
  }
  private drawParts(): void {
    const c = this.r.ctx;
    for (const p of this.parts) {
      const k = Math.max(0, p.life / p.max); c.globalAlpha = Math.min(1, k * 1.5); c.fillStyle = p.color;
      if (p.kind === 'ring') { c.strokeStyle = p.color; c.lineWidth = 2; c.beginPath(); c.arc(p.x, p.y, (1 - k) * 22 + 3, 0, 7); c.stroke(); }
      else if (p.kind === 'circ') { c.beginPath(); c.arc(p.x, p.y, p.size, 0, 7); c.fill(); }
      else if (p.kind === 'conf') { c.save(); c.translate(p.x, p.y); c.rotate(p.life * 9); c.fillRect(-p.size, -p.size / 2, p.size * 2, p.size); c.restore(); }
      else c.fillRect(p.x - p.size / 2, p.y - p.size / 2, p.size, p.size);
    }
    c.globalAlpha = 1;
  }

  // ---- saving and opening ---------------------------------------------------------------------------------
  saveCurrent(): void { if (this.dirtySave) { this.store.save(this.slot, this.level); this.dirtySave = false; } }
  private markEdited(): void { this.dirtySave = true; this.lastEditAt = this.nowMs; this.r.dirty(); this.puffs = []; }
  openLevel(slot: number, mode: 'play' | 'draw'): void {
    const src = this.store.slots[slot]; this.slot = slot; this.level = src ? cloneLevel(src) : blankLevel();
    this.levelIsStarter = this.level.name === 'Bridge'; this.undo = []; this.dirtySave = false; this.ghostDone = false;
    this.store.settings.lastSlot = slot; this.store.saveSettings(); this.r.dirty();
    if (mode === 'play') this.startPlay(); else this.go('draw');
  }
  private newInSlot(slot: number): void { this.store.save(slot, blankLevel()); this.openLevel(slot, 'draw'); this.say('New level'); }
  private startPlay(): void {
    this.world = newWorld(this.level); this.parts = []; this.winT = 0; this.spawnRing = 0.2; this.tiltHint = this.everPlayed ? 0 : 3; this.everPlayed = true; this.go('play');
  }

  // ===================================================================================================
  // the 60 Hz tick
  // ===================================================================================================
  tick(inp: InputState): void {
    this.nowMs = performance.now(); this.t += DT; this.screenT += DT; this.updateFx(DT);
    this.ui.begin();
    switch (this.screen) {
      case 'title': this.tickTitle(inp); break;
      case 'picker': this.tickPicker(inp); break;
      case 'chooser': this.tickChooser(inp); break;
      case 'standup': this.tickStandup(inp); break;
      case 'check': this.tickCheck(inp); break;
      case 'countdown': this.tickCountdown(inp); break;
      case 'processing': this.tickProcessing(inp); break;
      case 'review': this.tickReview(inp); break;
      case 'draw': this.tickDraw(inp); break;
      case 'play': this.tickPlay(inp); break;
      case 'win': this.tickWin(inp); break;
      case 'menu': this.tickMenu(inp); break;
    }
    if (inp.keyBack && !['title', 'play'].includes(this.screen)) this.goBack();
  }

  /** Esc: "go up one step" (a convenience in the browser; the device uses the on-screen back chip). */
  private goBack(): void {
    switch (this.screen) {
      case 'picker': this.go('title'); break; case 'chooser': this.go('picker'); break;
      case 'standup': case 'check': this.leaveCamera(); break; case 'countdown': this.go('check'); break;
      case 'review': this.go('check'); break; case 'draw': this.saveCurrent(); this.go('picker'); break;
      case 'menu': this.go(this.menuFrom); break; default: break;
    }
  }
  private leaveCamera(): void { this.go(this.cameFrom === 'chooser' ? 'chooser' : 'title'); }

  // ===================================================================================================
  // title
  // ===================================================================================================
  private tickTitle(inp: InputState): void {
    this.chip('sound', this.sndIcon(), 276);
    this.bar([
      { id: 'levels', w: 72, icon: 'grid' }, { id: 'paper', w: 80, icon: 'camera', fill: P.YELLOW, iconSize: 34 },
      { id: 'play', w: 88, icon: 'play', fill: P.GREEN, iconSize: 36 }, { id: 'draw', w: 80, icon: 'pencil' },
    ]);
    const hit = this.ui.update(inp);
    if (hit === 'sound') this.cycleSound();
    else if (hit === 'levels') { this.deleteMode = false; this.go('picker'); }
    else if (hit === 'paper') { this.cameFrom = 'title'; this.go(this.store.settings.standupSeen ? 'check' : 'standup'); }
    else if (hit === 'draw') { const s = this.store.firstEmpty(); if (s < 0) { this.say('All 10 slots are full'); this.go('picker'); } else this.newInSlot(s); }
    else if (hit === 'play') {
      if (!this.store.settings.firstRunDone) { this.openLevel(0, 'draw'); }       // first run: the starter level in Draw, with the ghost finger
      else this.openLevel(this.store.slots[this.store.settings.lastSlot] ? this.store.settings.lastSlot : 0, 'play');
    }
  }
  private drawTitle(): void {
    const r = this.r, c = r.ctx; r.clearPaper();
    c.save(); c.translate(160, 34); c.rotate(-0.035); r.text('SCRIBBLE', 0, 0, 42, P.INK); c.restore();
    c.save(); c.translate(160, 78); c.rotate(0.035); r.text('RUN!', 0, 0, 42, P.RED); c.restore();
    // a hill, a ball that rolls and jumps, a flag
    const hill = (x: number) => 176 - 26 * Math.exp(-(((x - 140) / 34) ** 2));
    c.save(); c.strokeStyle = P.INK; c.lineWidth = 8; c.lineCap = 'round'; c.lineJoin = 'round'; c.beginPath();
    for (let x = -4; x <= 324; x += 4) { if (x === -4) c.moveTo(x, hill(x)); else c.lineTo(x, hill(x)); } c.stroke(); c.restore();
    const u = (this.t * 0.22) % 1, bx = 24 + u * 244, jump = u > 0.5 && u < 0.72 ? Math.sin(((u - 0.5) / 0.22) * Math.PI) * 28 : 0;
    r.ball(bx, hill(bx) - 12 - jump, bx / 8);
    r.flag(272, 172, u > 0.97 ? 1 : 0);
    this.ui.draw(r, this.nowMs);
  }

  // ===================================================================================================
  // level picker
  // ===================================================================================================
  private tickPicker(inp: InputState): void {
    this.topBar('grid', { id: 'bin', icon: 'trash', active: this.deleteMode });
    for (let i = 0; i < C.SLOTS; i++) {
      const filled = this.store.slots[i] !== null, x = (i % 5) * 64, y = 48 + ((i / 5) | 0) * 96;
      this.ui.add({ id: 'slot' + i, x, y, w: 64, h: 96, kind: 'custom', holdMs: this.deleteMode && filled ? C.DELETE_HOLD_MS : undefined, draw: (ctx, b, hold) => this.drawCell(ctx, i, b, hold) });
    }
    const hit = this.ui.update(inp);
    if (hit === 'back') this.go('title');
    else if (hit === 'bin') this.deleteMode = !this.deleteMode;
    else if (hit?.startsWith('slot')) {
      const i = +hit.slice(4), filled = this.store.slots[i] !== null;
      if (this.deleteMode && filled) { this.store.delete(i); this.say('Deleted'); }
      else if (filled && !this.deleteMode) this.openLevel(i, 'play');
      else if (!filled) { this.pendingSlot = i; this.go('chooser'); }
    }
  }
  private drawCell(ctx: CanvasRenderingContext2D, i: number, b: Btn, hold: number): void {
    const r = this.r, lv = this.store.slots[i], th = this.store.thumbs[i], sel = i === this.store.settings.lastSlot && lv;
    if (lv && th) {
      r.roundRect(b.x, b.y, 64, 96, 0, sel ? P.YELLOW : P.WHITE, P.INK, sel ? 3 : 2); r.thumb(b.x + 2, b.y + 4, th, 1);
      r.text(String(i + 1), b.x + 32, b.y + 58, 22);
      if (this.store.isSolved(i)) { ctx.fillStyle = P.GREEN; ctx.strokeStyle = P.INK; ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(b.x + 54, b.y + 33, 6, 0, 7); ctx.fill(); ctx.stroke(); }
      if (lv.fromPaper) drawIcon(ctx, 'camera', b.x + 12, b.y + 12, 14, P.INK);
      if (this.deleteMode) { r.roundRect(b.x + 18, b.y + 66, 28, 24, 6, P.RED, P.INK, 2); drawIcon(ctx, 'trash', b.x + 32, b.y + 78, 16); }
      if (hold > 0) { ctx.strokeStyle = P.GREEN; ctx.lineWidth = 4; ctx.beginPath(); ctx.arc(b.x + 32, b.y + 48, 26, -Math.PI / 2, -Math.PI / 2 + hold * Math.PI * 2); ctx.stroke(); }
    } else {
      r.roundRect(b.x + 1, b.y + 1, 62, 94, 0, P.PAPER, null); ctx.save(); ctx.setLineDash([4, 3]); ctx.strokeStyle = P.GREY; ctx.lineWidth = 2; ctx.strokeRect(b.x + 2, b.y + 2, 60, 92); ctx.restore();
      drawIcon(ctx, 'plus', b.x + 32, b.y + 26, 30, '#5B5646'); r.text(String(i + 1), b.x + 32, b.y + 58, i === 9 ? 20 : 22, '#5B5646');
      if (this.store.broken[i]) { ctx.fillStyle = P.RED; ctx.beginPath(); ctx.arc(b.x + 54, b.y + 10, 4, 0, 7); ctx.fill(); }
    }
  }
  private drawPicker(): void { this.r.clear(P.PAPER); this.ui.draw(this.r, this.nowMs); }

  // ===================================================================================================
  // new level chooser
  // ===================================================================================================
  private tickChooser(inp: InputState): void {
    this.topBar('plus');
    this.ui.add({ id: 'paper', x: 16, y: 72, w: 136, h: 136, kind: 'big', fill: P.YELLOW, icon: 'camera', iconSize: 72 });
    this.ui.add({ id: 'finger', x: 168, y: 72, w: 136, h: 136, kind: 'big', fill: P.WHITE, icon: 'pencil', iconSize: 72 });
    const hit = this.ui.update(inp);
    if (hit === 'back') this.go(this.store.slots.some((s) => s) ? 'picker' : 'title');
    else if (hit === 'paper') { this.cameFrom = 'chooser'; this.go(this.store.settings.standupSeen ? 'check' : 'standup'); }
    else if (hit === 'finger') { const s = this.pendingSlot >= 0 && !this.store.slots[this.pendingSlot] ? this.pendingSlot : this.store.firstEmpty(); if (s < 0) { this.say('All 10 slots are full'); this.go('picker'); } else this.newInSlot(s); }
  }
  private drawChooser(): void { this.r.clearPaper(); this.ui.draw(this.r, this.nowMs); }

  // ===================================================================================================
  // camera: stand-up guide, check, countdown, processing, review
  // ===================================================================================================
  private tickStandup(inp: InputState): void {
    this.topBar();
    this.ui.add({ id: 'ready', x: 0, y: BAR, w: 320, h: C.BAR_H, kind: 'tab', icon: 'check', fill: P.GREEN, iconSize: 38 });
    const hit = this.ui.update(inp);
    if (hit === 'back') this.leaveCamera();
    else if (hit === 'ready') { this.store.settings.standupSeen = true; this.store.saveSettings(); this.go('check'); }
  }
  private drawStandup(): void {
    const r = this.r, c = r.ctx; r.clear(P.PAPER);
    const panel = (x: number, n: number) => { r.roundRect(x, 58, 98, 126, 10, P.WHITE, P.INK, 2); c.fillStyle = P.INK; c.beginPath(); c.arc(x + 17, 73, 11, 0, 7); c.fill(); r.text(String(n), x + 17, 74, 14, P.WHITE); };
    panel(8, 1); panel(111, 2); panel(214, 3);
    // 1: the device standing on its base (the yellow dot is the camera)
    c.fillStyle = P.INK; c.beginPath(); c.roundRect(33, 86, 44, 52, 6); c.fill();
    c.fillStyle = P.PAPER; c.fillRect(38, 91, 34, 34); c.fillStyle = P.YELLOW; c.strokeStyle = P.WHITE; c.lineWidth = 1; c.beginPath(); c.arc(55, 131, 3, 0, 7); c.fill(); c.stroke();
    c.fillStyle = P.GREY; c.strokeStyle = P.INK; c.lineWidth = 2.5; c.beginPath(); c.moveTo(26, 142); c.lineTo(84, 142); c.lineTo(78, 154); c.lineTo(32, 154); c.closePath(); c.fill(); c.stroke();
    // 2: the paper faces it, 40 cm away
    c.fillStyle = P.INK; c.beginPath(); c.roundRect(115, 100, 20, 26, 3); c.fill(); c.fillStyle = P.PAPER; c.fillRect(118, 103, 14, 14);
    r.roundRect(179, 90, 22, 48, 2, P.WHITE, P.INK, 2.5); c.fillStyle = P.INK; c.fillRect(184, 112, 10, 3);
    for (const [dx, dy] of [[184, 96], [196, 96], [196, 132], [184, 132]]) { c.beginPath(); c.arc(dx - 0, dy, 2, 0, 7); c.fill(); }
    c.strokeStyle = P.INK; c.lineWidth = 2; c.beginPath(); c.moveTo(139, 160); c.lineTo(175, 160); c.moveTo(139, 156); c.lineTo(139, 164); c.moveTo(175, 156); c.lineTo(175, 164); c.stroke();
    r.text('40 cm', 157, 148, 14);
    // 3: tap GO, then 3 2 1
    r.text('3', 234, 108, 30, P.INK); r.text('2', 263, 108, 30, P.RED); r.text('1', 290, 108, 30, P.BLUE);
    r.roundRect(236, 140, 50, 28, 8, P.GREEN, P.INK, 2); drawIcon(c, 'play', 261, 154, 18);
    this.ui.draw(r, this.nowMs);
  }

  private tickCheck(inp: InputState): void {
    this.chipBack(); this.chip('help', 'help', 276, { longMs: 2000, longId: 'numbers' });
    // bottom bar: four status lights (display only) + GO
    const lights = this.lc?.lights ?? [0, 0, 0, 0], icons: IconName[] = ['ruler', 'focus', 'bulb', 'level'];
    for (let i = 0; i < 4; i++) this.ui.add({ id: 'light' + i, x: i * 44, y: BAR, w: 44, h: C.BAR_H, kind: 'custom', disabled: true, draw: (ctx, b) => {
      this.r.roundRect(b.x, b.y, 44, 48, 0, P.WHITE, null); ctx.fillStyle = P.INK; ctx.fillRect(b.x, b.y, 44, 2); ctx.fillRect(b.x + 42, b.y, 2, 48);
      drawIcon(ctx, icons[i], b.x + 22, b.y + 18, 26);
      const col = [P.GREY, P.RED, P.YELLOW, P.GREEN][lights[i]];
      this.r.roundRect(b.x + 12, b.y + 34, 20, 9, 4.5, col, P.INK, 1.5);
    } });
    const ready = this.greenFor >= C.GREEN_HOLD_S;
    this.ui.add({ id: 'go', x: 176, y: BAR, w: 144, h: C.BAR_H, kind: 'tab', icon: 'play', fill: ready ? P.GREEN : P.YELLOW, iconSize: 38, pulse: ready });
    const hit = this.ui.update(inp);
    // live check: ~8 Hz, and only when the frame changed
    const frame = this.cam.latest();
    if (frame && (this.cam.version !== this.lcVersion || this.cam.source === 'webcam') && this.nowMs - this.lcAt > 125) {
      this.lcVersion = this.cam.version; this.lcAt = this.nowMs; this.lc = liveCheck(grayQvga(frame), inp.upright);
    }
    if (!frame) this.lc = null;
    this.greenFor = this.lc?.allGreen ? this.greenFor + DT : 0;
    if (hit === 'back') this.leaveCamera();
    else if (hit === 'help') this.go('standup');
    else if (hit === 'numbers') this.debugNums = !this.debugNums;
    else if (hit === 'go') { if (!frame) this.say('Pick a camera, photo or demo below'); else this.startCountdown(); }
  }
  private drawCheck(): void {
    const r = this.r, c = r.ctx; r.clear(P.PAPER_DOT);
    const frame = this.cam.latest();
    if (frame) {
      r.frame565(frame, 32, 0, 256, 192);
      // the target frame: where the four marks should land (about 40 cm)
      c.save(); c.lineWidth = 4.5; c.strokeStyle = 'rgba(28,28,36,0.55)'; c.strokeRect(58, 35, 204, 122); c.lineWidth = 2.5; c.strokeStyle = P.WHITE; c.setLineDash([9, 6]); c.strokeRect(58, 35, 204, 122); c.restore();
      const lc = this.lc;
      if (lc?.quad) {
        const q = lc.quad.map(([x, y]) => [32 + x * 0.8, y * 0.8]);
        c.save(); c.strokeStyle = P.GREEN; c.lineWidth = 2.5; c.lineJoin = 'round'; c.beginPath(); q.forEach(([x, y], i) => (i ? c.lineTo(x, y) : c.moveTo(x, y))); c.closePath(); c.stroke();
        q.forEach(([x, y]) => { c.beginPath(); c.arc(x, y, 12, 0, 7); c.stroke(); }); c.restore();
        if (lc.cue) this.drawCue(lc.cue);
      } else if (lc === null || !lc.marks) { r.text('?', 160, 96, 40, P.WHITE, 'center', P.INK); }
      if (this.debugNums && lc?.marks) {
        r.roundRect(36, 52, 130, 74, 8, 'rgba(255,255,255,0.9)', P.INK, 1.5);
        [`dist ${lc.distCm.toFixed(1)} cm`, `edge ${lc.edgePx.toFixed(1)} px`, `paper ${lc.paperMean.toFixed(0)}  clip ${(lc.clipFrac * 100).toFixed(0)}%`, `contrast ${lc.contrast.toFixed(0)}`, `ratio ${lc.ratioH.toFixed(2)} / ${lc.ratioV.toFixed(2)}`].forEach((s, i) => { c.save(); c.font = `600 10px ${FONT}`; c.fillStyle = P.INK; c.textAlign = 'left'; c.fillText(s, 42, 63 + i * 13); c.restore(); });
      }
    } else {
      r.roundRect(40, 40, 240, 112, 12, P.WHITE, P.INK, 2); drawIcon(c, 'camera', 160, 78, 44);
      r.text('No camera yet', 160, 112, 14); r.text('Use Webcam, Upload photo or Demo photo below the screen', 160, 132, 8, '#5B5646');
    }
    c.fillStyle = P.PAPER; c.fillRect(0, 0, 32, 192); c.fillRect(288, 0, 32, 192);                                  // the side columns
    this.ui.draw(r, this.nowMs);
  }
  private drawCue(cue: string): void {
    const c = this.r.ctx; c.save(); c.strokeStyle = P.YELLOW; c.lineWidth = 3.5; c.lineCap = 'round'; c.lineJoin = 'round';
    if (cue === 'tilt') { c.beginPath(); c.moveTo(142, 176); c.quadraticCurveTo(160, 186, 178, 176); c.moveTo(172, 171); c.lineTo(178, 176); c.lineTo(171, 181); c.stroke(); }
    else { const s = cue === 'closer' ? 1 : -1; for (const x of [80, 240]) { c.beginPath(); c.moveTo(x - 8 * s, 96 - 8); c.lineTo(x, 96); c.lineTo(x - 8 * s, 96 + 8); c.moveTo(x - 18 * s, 96); c.lineTo(x, 96); c.stroke(); } }
    c.restore();
  }

  private startCountdown(): void { this.cd = { t: 0, said: [false, false, false, false], snapped: false }; this.snapFrame = null; this.go('countdown'); }
  private tickCountdown(inp: InputState): void {
    this.chipBack({}, 'stop');
    const hit = this.ui.update(inp);
    if (hit === 'back') { this.go('check'); return; }
    this.cd.t += DT; const words = ['three', 'two', 'one', 'snap'] as const;
    for (let i = 0; i < 4; i++) if (!this.cd.said[i] && this.cd.t >= i * C.COUNTDOWN_STEP_S) { this.cd.said[i] = true; this.audio.say(words[i]); this.audio.play('beat'); this.spawn(6, 160, 70, [P.YELLOW, P.GREEN], 60, 0.5, 2); }
    if (!this.cd.snapped && this.cd.t >= C.SNAP_AT_S) {
      this.cd.snapped = true; const token = ++this.procToken;
      void this.cam.snap().then((f) => {
        if (token !== this.procToken || !f) return;
        this.snapFrame = f; this.audio.play('shutter'); this.fxFlash = 0.08; this.fxFlashColor = P.WHITE;       // the frame is taken FIRST, then the flash
        this.go('processing');
        void this.cam.processFrame(f, defaultParams()).then((res) => {
          if (token !== this.procToken || this.screen !== 'processing') return;
          this.review = { res, params: (res as unknown as { params?: VisionParams }).params ?? defaultParams(), reveal: 0, busy: false };
          const ok = res.report.status === Q_OK || res.report.status === Q_ORIENT_GUESS;
          this.audio.play(ok ? 'tada' : 'hmm'); this.onVision?.(res); this.go('review');
        });
      });
    }
  }
  private drawCountdown(): void {
    const r = this.r, c = r.ctx; r.clear('#3A3A48');
    const f = this.cam.latest(); if (f) r.frame565(f, 32, 0, 256, 192, 0.45);
    const n = this.cd.t < 1 ? 3 : this.cd.t < 2 ? 2 : this.cd.t < 3 ? 1 : 0, frac = this.cd.t % 1;
    const col = n === 3 ? P.INK : n === 2 ? P.RED : n === 1 ? P.BLUE : P.GREEN;
    c.fillStyle = P.PAPER; c.strokeStyle = P.INK; c.lineWidth = 5; c.beginPath(); c.arc(160, 78, 54, 0, 7); c.fill(); c.stroke();
    c.strokeStyle = P.GREEN; c.lineWidth = 7; c.lineCap = 'round'; c.beginPath(); c.arc(160, 78, 54, -Math.PI / 2, -Math.PI / 2 + (1 - frac) * Math.PI * 2); c.stroke();
    const pop = 1 + 0.25 * Math.max(0, 1 - frac * 4);
    c.save(); c.translate(160, 80); c.scale(pop, pop); r.text(n ? String(n) : '!', 0, 0, 92, col); c.restore();
    const by = 150 - Math.abs(Math.sin(frac * Math.PI)) * 26; r.ball(160, by, this.t * 3);
    for (let i = 0; i < 4; i++) { c.fillStyle = this.cd.t >= i + 0.05 ? P.GREEN : P.PAPER; c.strokeStyle = P.INK; c.lineWidth = 2.5; c.beginPath(); c.arc(115 + i * 30, 214, 11, 0, 7); c.fill(); c.stroke(); }
    this.drawParts(); this.ui.draw(r, this.nowMs);
  }

  private tickProcessing(inp: InputState): void { this.ui.update(inp); if (this.screenT < 0.05) this.spawn(0, 0, 0, [], 0, 0); }
  private drawProcessing(): void {
    const r = this.r, c = r.ctx; r.clear('#3A3A48');
    if (this.snapFrame) r.frame565(this.snapFrame, 32, 0, 256, 192, 0.6);
    for (let i = 0; i < 14; i++) { const a = this.t * 2 + i * 1.7, rr = 30 + ((i * 37 + this.t * 60) % 70), x = 160 + Math.cos(a) * rr, y = 96 + Math.sin(a * 1.3) * rr * 0.7; c.fillStyle = [P.YELLOW, P.WHITE, P.GREEN][i % 3]; c.save(); c.translate(x, y); c.rotate(a); c.fillRect(-3, -1, 6, 2); c.fillRect(-1, -3, 2, 6); c.restore(); }
    drawIcon(c, 'pencil', 160, 96, 40, P.WHITE);
  }

  // ---- review -------------------------------------------------------------------------------------------------
  private tickReview(inp: InputState): void {
    const rv = this.review; if (!rv) { this.go('check'); return; }
    rv.reveal += DT; const bad = ![Q_OK, Q_ORIENT_GUESS].includes(rv.res.report.status);
    this.chipBack();
    this.bar([
      { id: 'retake', w: 64, icon: 'restart', iconSize: 28, pulse: bad }, { id: 'less', w: 64, icon: 'inkLess', iconSize: 30 },
      { id: 'more', w: 64, icon: 'inkMore', iconSize: 30 }, { id: 'good', w: 128, icon: 'check', fill: P.GREEN, iconSize: 38 },
    ]);
    const hit = this.ui.update(inp);
    this.dragPins(inp, rv.res.level, false);
    if (hit === 'back' || hit === 'retake') { this.go('check'); }
    else if ((hit === 'less' || hit === 'more') && !rv.busy) {
      rv.busy = true; const step = hit === 'less' ? C.INK_STEP : -C.INK_STEP;
      rv.params = { ...rv.params, inkThresh: Math.max(38, Math.min(128, rv.params.inkThresh + step)) };
      void this.cam.rethreshold(rv.res.work, rv.params).then((res) => { rv.res = res; rv.busy = false; rv.reveal = 1; this.audio.play('tick'); this.onVision?.(res); });
    } else if (hit === 'good') this.acceptReview();
  }
  private acceptReview(): void {
    const rv = this.review!; const slot = this.pendingSlot >= 0 && !this.store.slots[this.pendingSlot] ? this.pendingSlot : this.store.firstEmpty() >= 0 ? this.store.firstEmpty() : this.store.settings.lastSlot;
    const lv = cloneLevel(rv.res.level); lv.name = 'Paper ' + (slot + 1); lv.fromPaper = true;
    this.store.save(slot, lv); this.store.settings.firstRunDone = true; this.pendingSlot = -1; this.openLevel(slot, 'play');
  }
  private drawReview(): void {
    const r = this.r, c = r.ctx, rv = this.review!; r.clear(P.PAPER);
    r.straight565(rv.res.work.straight565, 0.35);
    const t = rv.reveal / 0.13, mask = { ground: t > 0, spike: t > 1, bouncy: t > 2 };       // reveal ground, then spikes, then bouncy
    r.drawLayers(rv.res.level, mask);
    r.startPin(rv.res.level.startX, rv.res.level.startY); r.flagPin(rv.res.level.flagX, rv.res.level.flagY);
    // legend: black / red / blue
    r.roundRect(112, 6, 96, 34, 12, 'rgba(255,255,255,0.9)', P.INK, 2);
    c.fillStyle = P.INK; c.fillRect(128, 15, 14, 14);
    c.fillStyle = P.RED; c.strokeStyle = P.INK; c.lineWidth = 1.6; c.beginPath(); c.moveTo(152, 29); c.lineTo(159, 15); c.lineTo(166, 29); c.closePath(); c.fill(); c.stroke();
    c.fillStyle = P.BLUE; c.beginPath(); c.arc(185, 22, 7, 0, 7); c.fill(); c.stroke();
    const why: Record<number, string> = { [Q_MARKS_MISSING]: 'paper not found: used the whole picture', [Q_EMPTY]: 'draw bigger and darker', [Q_TOO_DARK]: 'more light, please', [Q_TOO_MUCH_INK]: 'is the paper in view?', [Q_ORIENT_GUESS]: '' };
    const s = rv.res.report.status; if (s !== Q_OK && why[s] !== undefined && why[s]) r.text(why[s], 160, 52, 9, P.INK, 'center', P.WHITE);
    this.ui.draw(r, this.nowMs);
  }

  // ===================================================================================================
  // draw / edit
  // ===================================================================================================
  private pushUndo(): void { this.undo.push(cloneLevel(this.level)); if (this.undo.length > C.UNDO_LEVELS) this.undo.shift(); }

  /** Pin dragging (Draw and Review). Returns true while a pin is being dragged. */
  private dragPins(inp: InputState, lv: Level, withUndo: boolean): boolean {
    const t = inp.touch;
    if (t.pressed && !this.ui.touchOnButton && t.y < BAR) {
      const ds = Math.hypot(t.x - lv.startX, t.y - lv.startY), df = Math.hypot(t.x - lv.flagX, t.y - (lv.flagY - C.FLAG_CENTER_DY));
      if (Math.min(ds, df) <= C.PIN_GRAB_RADIUS) { this.pinDrag = ds <= df ? 'start' : 'flag'; if (withUndo) this.pushUndo(); }
    }
    if (this.pinDrag && t.down) {
      const x = Math.max(8, Math.min(C.WORLD_W - 8, t.x)), y = Math.max(8, Math.min(C.WORLD_H - 2, t.y));
      if (this.pinDrag === 'start') { lv.startX = Math.round(x); lv.startY = Math.round(y); } else { lv.flagX = Math.round(x); lv.flagY = Math.round(y + C.FLAG_CENTER_DY); }
    }
    if (this.pinDrag && t.released) {                                                                    // drop: never bury a pin
      if (this.pinDrag === 'start') clearAround(lv, lv.startX, lv.startY, C.START_SAFE_RADIUS); else clearAround(lv, lv.flagX, lv.flagY - C.FLAG_CENTER_DY, C.FLAG_SAFE_RADIUS);
      this.pinDrag = null; if (withUndo) this.markEdited(); this.r.dirty(); this.audio.play('tick');
    }
    return this.pinDrag !== null;
  }

  private tickDraw(inp: InputState): void {
    this.chipBack(); this.chipMenu();
    const tools: [string, number, IconName, Partial<Btn>][] = [
      ['ground', 52, 'ground', { fill: P.WHITE }], ['spike', 56, 'spike', {}], ['bouncy', 52, 'bouncy', {}], ['eraser', 52, 'eraser', {}],
      ['undo', 56, 'undo', { disabled: this.undo.length === 0 }], ['play', 52, 'play', { fill: P.GREEN, iconSize: 32 }],
    ];
    let x = 0;
    for (const [id, w, icon, extra] of tools) {
      const sel = id === this.tool;
      this.ui.add({ id, x, y: BAR, w, h: C.BAR_H, kind: 'custom', icon, ...extra, draw: (ctx, b) => this.drawTool(ctx, b, sel) }); x += w;
    }
    const hit = this.ui.update(inp), t = inp.touch;
    if (t.pressed) { this.ghostDone = true; this.idleSince = this.nowMs; }
    if (hit === 'back') { this.saveCurrent(); this.go('picker'); return; }
    if (hit === 'menu') { this.menuFrom = 'draw'; this.go('menu'); return; }
    if (hit === 'play') { this.saveCurrent(); this.store.settings.firstRunDone = true; this.store.saveSettings(); this.startPlay(); return; }
    if (hit === 'undo') { const s = this.undo.pop(); if (s) { copyLevelInto(this.level, s); this.markEdited(); this.audio.play('tick'); } }
    else if (hit === 'ground' || hit === 'spike' || hit === 'bouncy' || hit === 'eraser') { this.tool = hit; this.audio.play('tick'); }

    // pins first (grabbing a pin beats drawing), then strokes
    const dragging = this.dragPins(inp, this.level, true);
    if (!dragging) {
      const rad = this.tool === 'eraser' ? C.ERASER_RADIUS : C.BRUSH_RADIUS, mat: Material | null = this.tool === 'eraser' ? null : this.tool === 'ground' ? MAT_GROUND : this.tool === 'spike' ? MAT_SPIKE : MAT_BOUNCY;
      const px = Math.max(0, Math.min(C.WORLD_W - 1, t.x)), py = Math.max(0, Math.min(C.WORLD_H - 1, t.y));
      if (t.pressed && !this.ui.touchOnButton && t.y < BAR) {
        this.pushUndo(); this.stroke = { active: true, lx: px, ly: py, dist: 0 };
        if (mat === null) eraseDisc(this.level, px, py, rad); else paintDisc(this.level, mat, px, py, rad); this.markEdited();
      } else if (this.stroke.active && t.down) {
        paintLine(this.level, mat, this.stroke.lx, this.stroke.ly, px, py, rad);
        this.stroke.dist += Math.hypot(px - this.stroke.lx, py - this.stroke.ly); this.stroke.lx = px; this.stroke.ly = py; this.markEdited();
        if (this.stroke.dist > 24) { this.stroke.dist = 0; this.audio.play(this.tool === 'ground' ? 'pen_ground' : this.tool === 'spike' ? 'pen_spike' : this.tool === 'bouncy' ? 'pen_bouncy' : 'tick'); }
      }
      if (t.released && this.stroke.active) this.stroke.active = false;
    }
    // autosave after a few idle seconds
    if (this.dirtySave && !this.stroke.active && (this.nowMs - this.lastEditAt) / 1000 > C.AUTOSAVE_IDLE_S) this.saveCurrent();
  }
  private drawTool(ctx: CanvasRenderingContext2D, b: Btn, sel: boolean): void {
    const r = this.r;
    r.roundRect(b.x, b.y, b.w, b.h, 0, b.id === 'play' ? P.GREEN : sel ? P.YELLOW : b.disabled ? P.PAPER_DOT : P.WHITE, null);
    ctx.fillStyle = P.INK; ctx.fillRect(b.x, b.y, b.w, 2); ctx.fillRect(b.x + b.w - 2, b.y, 2, b.h);
    if (sel) { ctx.strokeStyle = P.INK; ctx.lineWidth = 3; ctx.strokeRect(b.x + 1.5, b.y + 1.5, b.w - 3, b.h - 3); }
    const col = b.id === 'spike' ? P.RED : b.id === 'bouncy' ? P.BLUE : b.disabled ? P.GREY : P.INK;
    drawIcon(ctx, b.icon!, b.x + b.w / 2, b.y + 25, b.iconSize ?? 30, col);
    if (this.ui.pressedId === b.id) { ctx.fillStyle = 'rgba(0,0,0,0.12)'; ctx.fillRect(b.x, b.y, b.w, b.h); }
  }
  private ghostPos(): { x: number; y: number; trail: number } | null {
    if (this.ghostDone || !this.levelIsStarter || this.dirtySave || this.undo.length > 0) return null;
    if ((this.nowMs - this.idleSince) / 1000 < C.GHOST_IDLE_S) return null;
    const cyc = (((this.nowMs - this.idleSince) / 1000 - C.GHOST_IDLE_S) % 4.5) / 2.5; if (cyc > 1.25) return null;
    const u = Math.min(1, cyc); return { x: 72 + u * 176, y: 118 + Math.sin(u * Math.PI) * 6, trail: u };
  }
  private drawDrawScene(): void {
    const r = this.r, c = r.ctx; r.clearPaper();
    r.drawLayers(this.level);
    r.startPin(this.level.startX, this.level.startY, this.pinDrag === 'start'); r.flagPin(this.level.flagX, this.level.flagY, this.pinDrag === 'flag');
    const g = this.ghostPos();
    if (g) { c.save(); c.strokeStyle = 'rgba(91,86,70,0.8)'; c.lineWidth = 6; c.lineCap = 'round'; c.setLineDash([1, 10]); c.beginPath(); c.moveTo(72, 118); c.lineTo(g.x, g.y); c.stroke(); c.restore();
      c.fillStyle = 'rgba(28,28,36,0.25)'; c.strokeStyle = '#5B5646'; c.lineWidth = 2; c.beginPath(); c.arc(g.x, g.y, 13, 0, 7); c.fill(); c.stroke(); }
    if (this.tool === 'eraser' && this.stroke.active) { c.strokeStyle = P.GREY; c.lineWidth = 1.5; c.beginPath(); c.arc(this.stroke.lx, this.stroke.ly, C.ERASER_RADIUS, 0, 7); c.stroke(); }
  }
  private drawDraw(): void { this.drawDrawScene(); this.ui.draw(this.r, this.nowMs); }

  // ===================================================================================================
  // play
  // ===================================================================================================
  private tickPlay(inp: InputState): void {
    this.chipBack({ holdMs: C.HOLD_MS }, 'pencil'); this.chipMenu();
    this.ui.add({ id: 'restart', x: 0, y: BAR, w: 120, h: C.BAR_H, kind: 'tab', icon: 'restart', iconSize: 28, holdMs: C.HOLD_MS });
    this.ui.add({ id: 'jump', x: 120, y: BAR, w: 200, h: C.BAR_H, kind: 'tab', icon: 'up', fill: P.GREEN, iconSize: 38, onPress: true });
    const hit = this.ui.update(inp), t = inp.touch;
    if (hit === 'back') { this.puffs = []; this.go('draw'); return; }
    if (hit === 'menu') { this.menuFrom = 'play'; this.go('menu'); return; }
    if (hit === 'restart') { this.world = newWorld(this.level); this.spawnRing = 0.2; this.audio.play('bloop'); }
    // jump: a tap anywhere on the playfield (not on a button, not a thumb on the screen edge), the pad, or a key
    const gripSafe = t.startX >= C.JUMP_EDGE_MARGIN && t.startX <= C.SCREEN_W - C.JUMP_EDGE_MARGIN;
    const jump = hit === 'jump' || inp.keyJump || (t.pressed && !this.ui.touchOnButton && t.y < BAR && gripSafe);
    this.tiltHint = Math.max(0, this.tiltHint - DT);
    const w = this.world, ev = physicsStep(w, this.level, { tilt: inp.tilt, jumpPressed: jump }), b = w.ball;
    if (ev & EV_JUMP) { this.audio.play('jump'); this.fxStretch = 0.1; }
    if (ev & EV_LAND) { this.audio.play('land'); this.fxSquash = 0.08; this.spawn(3, b.x, b.y + 8, [P.GREY], 30, 0.3, 2); }
    if (ev & EV_BOUNCE) { this.audio.play('bounce'); this.fxStretch = 0.14; this.spawn(4, b.x, b.y, [P.BLUE, P.WHITE], 90, 0.45, 2, 'circ'); }
    if (ev & EV_POP) { this.audio.play('pop'); this.fxShake = 0.15; this.fxRed = 0.1; this.spawn(8, b.x, b.y, [P.RED, P.WHITE], 120, 0.55, 3); this.puffs.push({ x: b.x, y: b.y, t: this.t }); if (this.puffs.length > 5) this.puffs.shift(); }
    if (ev & EV_FELL) this.audio.play('whoosh');
    if (ev & EV_RESPAWN) { this.audio.play('bloop'); this.spawnRing = 0.2; this.parts.push({ x: this.level.startX, y: this.level.startY, vx: 0, vy: 0, life: 0.35, max: 0.35, color: P.GREEN, size: 0, kind: 'ring' }); }
    if (ev & EV_WIN) {
      this.audio.play('win'); this.fxFlash = 0.12; this.fxFlashColor = '#D9F3E4'; this.winT = 0;
      this.parts.push({ x: this.level.flagX, y: this.level.flagY - 12, vx: 0, vy: 0, life: 0.5, max: 0.5, color: P.GREEN, size: 0, kind: 'ring' });
      this.spawn(24, this.level.flagX, this.level.flagY - 12, [P.RED, P.BLUE, P.YELLOW, P.GREEN], 150, 1.4, 3, 'conf');
      this.store.markSolved(this.slot);
    }
    if (w.state === ST_CELEBRATING) { this.winT += DT; if (this.winT >= C.WIN_SCREEN_DELAY) { this.winT = 0; this.go('win'); } }
  }
  private drawPlayScene(): void {
    const r = this.r, c = r.ctx, w = this.world; r.clearPaper();
    c.save(); if (this.fxShake > 0) c.translate((Math.random() - 0.5) * 4, (Math.random() - 0.5) * 4);
    r.drawLayers(this.level);
    for (const p of this.puffs) { c.fillStyle = 'rgba(185,179,160,0.55)'; for (const [dx, dy, rr] of [[-5, 0, 6], [4, -2, 7], [0, 4, 5]]) { c.beginPath(); c.arc(p.x + dx, p.y + dy, rr, 0, 7); c.fill(); } }
    r.flag(this.level.flagX, this.level.flagY, w.state === ST_CELEBRATING ? 1 : 0);
    if (this.spawnRing > 0) { c.strokeStyle = P.GREEN; c.lineWidth = 2; c.beginPath(); c.arc(this.level.startX, this.level.startY, 6 + (1 - this.spawnRing / 0.2) * 12, 0, 7); c.stroke(); }
    if (w.state !== ST_DYING) {
      const b = w.ball, hop = w.state === ST_CELEBRATING ? -Math.abs(Math.sin(w.stateTime * 9)) * 10 : 0;
      const sx = this.fxSquash > 0 ? 1.25 : this.fxStretch > 0 ? 0.85 : 1, sy = this.fxSquash > 0 ? 0.75 : this.fxStretch > 0 ? 1.25 : 1;
      const grow = this.spawnRing > 0 ? 1 - this.spawnRing / 0.2 : 1;
      r.ball(b.x, b.y + hop, b.angle, sx * grow, sy * grow, w.state === ST_CELEBRATING ? 'happy' : 'normal');
    }
    this.drawParts();
    c.restore();
    if (this.tiltHint > 0) { c.save(); c.strokeStyle = '#5B5646'; c.lineWidth = 3; c.lineCap = 'round'; c.lineJoin = 'round'; const k = this.tiltHint > 0.5 ? 1 : this.tiltHint * 2, wob = Math.sin(this.t * 6) * 4;
      c.globalAlpha = k; for (const s of [-1, 1]) { const x = w.ball.x + s * (26 + wob), y = w.ball.y - 26; c.beginPath(); c.moveTo(x - 6 * s, y - 6); c.lineTo(x, y); c.lineTo(x - 6 * s, y + 6); c.moveTo(x - 16 * s, y); c.lineTo(x, y); c.stroke(); } c.restore(); }
    r.flash(P.RED, this.fxRed * 2.2);
  }
  private drawPlay(): void { this.drawPlayScene(); this.ui.draw(this.r, this.nowMs); this.r.flash(this.fxFlashColor, this.fxFlash * 6); }

  // ===================================================================================================
  // win
  // ===================================================================================================
  private tickWin(inp: InputState): void {
    this.bar([{ id: 'edit', w: 104, icon: 'pencil', iconSize: 32 }, { id: 'again', w: 112, icon: 'play', fill: P.GREEN, iconSize: 38 }, { id: 'new', w: 104, icon: 'plus', iconSize: 34 }]);
    const hit = this.ui.update(inp);
    if (this.screenT < 4 && Math.random() < 0.35) this.spawn(1, Math.random() * 320, -4, [P.RED, P.BLUE, P.YELLOW, P.GREEN], 20, 3, 3, 'conf');
    if (hit === 'edit') this.go('draw');
    else if (hit === 'again') this.startPlay();
    else if (hit === 'new') { this.pendingSlot = this.store.firstEmpty(); this.go('chooser'); }
  }
  private drawWin(): void {
    const r = this.r, c = r.ctx; r.clear(P.PAPER);
    c.save(); c.fillStyle = '#D9F3E4'; for (let i = 0; i < 18; i++) { const a = (i * 20 + this.t * 6) * Math.PI / 180; c.beginPath(); c.moveTo(160, 91); c.arc(160, 91, 400, a, a + 10 * Math.PI / 180); c.closePath(); c.fill(); } c.restore();
    const k = Math.min(1, this.screenT / 0.4), ease = k < 1 ? 1 - Math.pow(1 - k, 3) * Math.cos(k * 9) : 1; // bounce in
    r.text('YOU WIN!', 160, 22, 28, P.INK, 'center', P.YELLOW);
    c.save(); c.translate(160, 76); c.scale(ease, ease); c.fillStyle = P.YELLOW; c.strokeStyle = P.INK; c.lineWidth = 4; c.lineJoin = 'round'; c.beginPath();
    for (let i = 0; i < 10; i++) { const a = -Math.PI / 2 + (i * Math.PI) / 5, rr = i % 2 ? 19 : 42; c.lineTo(Math.cos(a) * rr, Math.sin(a) * rr); } c.closePath(); c.fill(); c.stroke();
    c.fillStyle = P.INK; c.beginPath(); c.arc(-9, -4, 3, 0, 7); c.arc(9, -4, 3, 0, 7); c.fill(); c.lineWidth = 3; c.lineCap = 'round'; c.beginPath(); c.arc(0, 4, 9, 0.2 * Math.PI, 0.8 * Math.PI); c.stroke(); c.restore();
    c.strokeStyle = P.INK; c.lineWidth = 8; c.lineCap = 'round'; c.beginPath(); c.moveTo(84, 168); c.lineTo(252, 168); c.stroke();
    r.ball(150, 156 - Math.abs(Math.sin(this.t * 4)) * 5, 0, 1, 1, 'happy'); r.flag(226, 164, 1);
    this.drawParts(); this.ui.draw(r, this.nowMs);
  }

  // ===================================================================================================
  // pause / menu overlay
  // ===================================================================================================
  private tickMenu(inp: InputState): void {
    this.chipBack({}, 'back');
    const fromPlay = this.menuFrom === 'play';
    const cells: [string, IconName, Partial<Btn>][] = fromPlay
      ? [['resume', 'play', { fill: P.GREEN }], ['edit', 'pencil', {}], ['levels', 'grid', {}], ['restart', 'restart', {}], ['sound', this.sndIcon(), {}], ['home', 'home', {}]]
      : [['resume', 'play', { fill: P.GREEN }], ['clear', 'trash', { holdMs: C.CLEAR_HOLD_MS }], ['levels', 'grid', {}], ['sound', this.sndIcon(), {}], ['home', 'home', {}]];
    cells.forEach(([id, icon, extra], i) => this.ui.add({ id, x: 16 + (i % 3) * 96, y: 56 + ((i / 3) | 0) * 80, w: 96, h: 80, kind: 'custom', icon, ...extra,
      draw: (ctx, b, hold) => { this.r.roundRect(b.x, b.y, 96, 80, 0, b.fill ?? P.WHITE, null); ctx.strokeStyle = P.INK; ctx.lineWidth = 2; ctx.strokeRect(b.x, b.y, 96, 80); drawIcon(ctx, b.icon!, b.x + 48, b.y + 40, 44);
        if (hold > 0) { ctx.strokeStyle = P.GREEN; ctx.lineWidth = 4; ctx.beginPath(); ctx.arc(b.x + 48, b.y + 40, 34, -Math.PI / 2, -Math.PI / 2 + hold * Math.PI * 2); ctx.stroke(); } } }));
    const hit = this.ui.update(inp), t = inp.touch;
    const outside = t.released && !this.ui.touchOnButton && !(t.x > 16 && t.x < 304 && t.y > 56 && t.y < 216);
    if (hit === 'back' || hit === 'resume' || outside) this.go(this.menuFrom);
    else if (hit === 'edit') this.go('draw');
    else if (hit === 'levels') { this.saveCurrent(); this.go('picker'); }
    else if (hit === 'restart') { this.world = newWorld(this.level); this.go('play'); }
    else if (hit === 'sound') this.cycleSound();
    else if (hit === 'home') { this.saveCurrent(); this.go('title'); }
    else if (hit === 'clear') { this.pushUndo(); for (const l of [this.level.ground, this.level.spike, this.level.bouncy]) l.fill(0); this.markEdited(); this.say('Cleared (undo brings it back)'); this.go('draw'); }
  }
  private drawMenu(): void {
    const r = this.r;
    if (this.menuFrom === 'play') this.drawPlayScene(); else this.drawDrawScene();
    r.dim(0.62); r.roundRect(13, 53, 294, 166, 12, P.WHITE, P.INK, 3);
    this.ui.draw(r, this.nowMs);
  }

  // ===================================================================================================
  // the frame
  // ===================================================================================================
  draw(): void {
    switch (this.screen) {
      case 'title': this.drawTitle(); break; case 'picker': this.drawPicker(); break; case 'chooser': this.drawChooser(); break;
      case 'standup': this.drawStandup(); break; case 'check': this.drawCheck(); break; case 'countdown': this.drawCountdown(); break;
      case 'processing': this.drawProcessing(); break; case 'review': this.drawReview(); break; case 'draw': this.drawDraw(); break;
      case 'play': this.drawPlay(); break; case 'win': this.drawWin(); break; case 'menu': this.drawMenu(); break;
    }
    if (this.fxFlash > 0 && this.screen !== 'play') this.r.flash(this.fxFlashColor, this.fxFlash * 8);
    if (this.transition > 0) this.r.flash(P.PAPER, this.transition * 0.9);
    if (this.toast.t > 0) { const k = Math.min(1, this.toast.t * 2); this.r.ctx.globalAlpha = k; this.r.roundRect(160 - 100, 186 - 26, 200, 22, 11, P.INK, null); this.r.text(this.toast.text, 160, 160, 11, P.WHITE); this.r.ctx.globalAlpha = 1; }
  }

  /** Webcam / Upload / Demo buttons below the screen jump here. */
  openCheck(): void { if (['check', 'standup', 'countdown', 'processing', 'review'].includes(this.screen)) { if (this.screen !== 'check' && this.screen !== 'standup') return; return; } this.saveCurrent(); this.cameFrom = this.screen === 'chooser' ? 'chooser' : 'title'; this.go('check'); }
  /** The level to export or turn into a demo photo: what you are editing, else the last-used slot. */
  currentLevel(): Level { if (['draw', 'play', 'win', 'menu'].includes(this.screen)) return this.level; return this.store.slots[this.store.settings.lastSlot] ?? this.level; }
  /** Put a level that was loaded from a file into the picker. */
  showPicker(): void { this.deleteMode = false; this.go('picker'); }

  // ---- hooks for the test page / Playwright ----------------------------------------------------------------------------
  /** Jump straight to a screen (used by the demo buttons and the screenshot script). */
  debugGoto(s: Screen): void { if (s === 'play' || s === 'win') { this.level = this.store.slots[0] ? cloneLevel(this.store.slots[0]!) : blankLevel(); this.startPlay(); } if (s === 'review' && !this.review) return; this.go(s); }
  get status() { return { screen: this.screen, slot: this.slot, tool: this.tool, ballX: this.world.ball.x, ballY: this.world.ball.y, state: this.world.state, rolling: this.world.state === ST_ROLLING }; }
}
