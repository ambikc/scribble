// End-to-end smoke test: drives the built simulator in a real browser through every screen.
//   npm run build && npx vite preview --port 4173 &   then:   node scripts/e2e-smoke.cjs
// Needs Playwright: PLAYWRIGHT=/path/to/playwright node scripts/e2e-smoke.cjs   (or `npm i -D playwright`)
const { chromium } = require(process.env.PLAYWRIGHT || 'playwright');
const OUT = process.env.SHOTS || '/tmp/scribble-shots/'; require('fs').mkdirSync(OUT, { recursive: true });
let fails = 0; const check = (name, ok, extra = '') => { console.log((ok ? 'PASS ' : 'FAIL ') + name + (extra ? '  ' + extra : '')); if (!ok) fails++; };
(async () => {
  const b = await chromium.launch(); const ctx = await b.newContext({ viewport: { width: 1060, height: 1150 }, acceptDownloads: true });
  const page = await ctx.newPage(); const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); }); page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(process.env.URL || 'http://127.0.0.1:4173/'); await page.waitForTimeout(600);
  const box = await page.locator('#screen').boundingBox(), S = box.width / 320;
  const tap = async (x, y, hold = 70) => { await page.mouse.move(box.x + x * S, box.y + y * S); await page.mouse.down(); await page.waitForTimeout(hold); await page.mouse.up(); await page.waitForTimeout(140); };
  const st = () => page.evaluate(() => window.__game.status), scr = async () => (await st()).screen;
  const shot = (n) => page.locator('.device').screenshot({ path: OUT + n + '.png' });
  const px = () => page.evaluate(() => window.__game.level.ground.reduce((s, v) => { while (v) { s += v & 1; v >>= 1; } return s; }, 0));

  check('title screen', (await scr()) === 'title'); await shot('01-title');
  // ---- first run: Play opens the starter level in Draw, with the ghost finger after 3 s
  await tap(196, 216); check('first run: Play opens Draw', (await scr()) === 'draw'); await page.waitForTimeout(4200); await shot('02-draw-ghost');
  // ---- draw a bridge with the mouse, undo it, draw it again
  const g0 = await px();
  const bridge = async () => { await page.mouse.move(box.x + 75 * S, box.y + 118 * S); await page.mouse.down(); for (let x = 75; x <= 246; x += 4) { await page.mouse.move(box.x + x * S, box.y + (119 + Math.sin(x / 30) * 3) * S); await page.waitForTimeout(6); } await page.mouse.up(); await page.waitForTimeout(150); };
  await bridge(); const g1 = await px(); check('drawing adds ground pixels', g1 > g0 + 500, `${g0} -> ${g1}`);
  await tap(240, 216); check('undo removes the stroke', (await px()) === g0);
  await bridge(); await shot('03-draw-bridge');
  // tools: spike patch and bouncy pad, then eraser
  await tap(80, 216); await page.mouse.move(box.x + 120 * S, box.y + 170 * S); await page.mouse.down(); await page.mouse.move(box.x + 150 * S, box.y + 170 * S, { steps: 6 }); await page.mouse.up();
  await tap(134, 216); await page.mouse.move(box.x + 180 * S, box.y + 170 * S); await page.mouse.down(); await page.mouse.move(box.x + 210 * S, box.y + 170 * S, { steps: 6 }); await page.mouse.up();
  const kinds = await page.evaluate(() => ({ spike: window.__game.level.spike.some((v) => v), bouncy: window.__game.level.bouncy.some((v) => v) }));
  check('spike and bouncy tools paint', kinds.spike && kinds.bouncy);
  // ---- play it for real: roll right and jump; the ball must reach the flag
  await tap(294, 216); check('Play button starts Play', (await scr()) === 'play');
  await page.keyboard.down('ArrowRight'); let won = false;
  for (let i = 0; i < 160 && !won; i++) { await page.waitForTimeout(100); won = (await scr()) === 'win'; if (i === 8) await shot('04-play'); }
  await page.keyboard.up('ArrowRight'); check('ball rolls across the drawn bridge to the flag (Win screen)', won); await page.waitForTimeout(900); await shot('05-win');
  // ---- win buttons: Edit -> Draw, then back to Play, then menu
  await tap(52, 216); check('Win: Edit goes to Draw', (await scr()) === 'draw');
  await tap(294, 216); await page.waitForTimeout(200); await tap(296, 24); check('menu chip opens the menu', (await scr()) === 'menu'); await shot('06-menu');
  await tap(64, 96); check('menu Resume returns to Play', (await scr()) === 'play');
  await tap(24, 24, 200); check('a short tap on Edit does NOT leave Play (hold needed)', (await scr()) === 'play');
  await tap(24, 24, 700); check('holding Edit 0.4 s leaves Play', (await scr()) === 'draw');
  await tap(24, 24); await page.waitForTimeout(200); check('Back from Draw goes to the picker', (await scr()) === 'picker'); await shot('07-picker');
  // ---- picker: delete mode needs a hold
  await tap(296, 24); const n0 = await page.evaluate(() => window.__game.store.slots.filter(Boolean).length);
  await tap(160, 96, 300); check('delete needs a long hold (short tap does nothing)', (await page.evaluate(() => window.__game.store.slots.filter(Boolean).length)) === n0);
  await tap(296, 24);
  // ---- paper mode
  await tap(24, 24); check('Back to title', (await scr()) === 'title');
  await page.click('#btn-demo'); await page.waitForTimeout(900); check('Demo photo opens the Camera check', (await scr()) === 'check');
  await page.waitForTimeout(1000); await shot('08-check');
  const lights = await page.evaluate(() => 'ok'); void lights;
  await tap(250, 216); check('GO starts the countdown', (await scr()) === 'countdown'); await page.waitForTimeout(1500); await shot('09-countdown');
  await page.waitForTimeout(4600); check('countdown -> processing -> review', (await scr()) === 'review'); await shot('10-review');
  // thinner / thicker ink buttons
  await tap(32 + 64, 216); await tap(32 + 128, 216); await page.waitForTimeout(600);
  await tap(256, 216); await page.waitForTimeout(400); check('Looks good saves the level and starts Play', (await scr()) === 'play');
  const paperSlot = await page.evaluate(() => window.__game.store.slots.findIndex((s) => s && s.fromPaper)); check('a paper level was saved with the from-paper flag', paperSlot >= 0, 'slot ' + paperSlot);
  // ---- stress preset + download
  await page.selectOption('#sel-preset', 'tilt15'); await page.waitForTimeout(900);
  const dl = page.waitForEvent('download'); await page.click('#btn-download'); const d = await dl; const bytes = require('fs').readFileSync(await d.path());
  check('Download level gives a .scrb in the device format', bytes.length >= 24184 && bytes.slice(0, 4).toString() === 'SCRB', `${d.suggestedFilename()} ${bytes.length} bytes`);
  await page.screenshot({ path: OUT + '11-page.png', fullPage: true });
  check('no console errors', errors.length === 0, errors.join(' | '));
  await b.close(); console.log(fails ? `\n${fails} FAILED` : '\nall passed'); process.exit(fails ? 1 : 0);
})();
