# Writing a screen: Renderer, UI and Game core

For the people (and agents) who write `game_<screen>.cpp`. Everything here is verified in the emulator
(`emu/README.md`); look at `emu/gallery` to see each building block drawn. Design: `docs/screens.md`; the reference
implementation of every screen is `sim/src/modes.ts`.

```text
 Game::tick(in)   once per 60 Hz tick      nowMs, screenT, t (a double, wraps every hour) advance; ui.begin(); your tickX(in); updateFx
 Game::draw()     once per frame           your drawX(); particles; shutter flash; paper fade; toast
 main()           r.present() afterwards   pushes the 320 x 240 sprite to the display (the shake offset is applied there)
```

## The two functions of a screen

* `tickX(const InputState& in)`: add this tick's buttons, call `ui.update(in)`, react to the id that fired, change state.
  `Game::tick()` has **already called `ui.begin()`**, so just `ui.add()` (via the helpers or directly).
* `drawX()`: paint the scene with `r.` calls, then `ui.draw(r, nowMs)` last so the buttons are on top. Never read input here.
* The first tick after `go()` sees `screenT` ~ 0.017 s: use `screenT < 0.02f` to initialise per-screen state; `levelSerial`
  changes whenever `openLevel()` loaded a level (Draw: reset undo and the ghost finger when it changes).
* Never call `M5.Display` directly: everything goes to the frame sprite through `r`. `r.g()` is that sprite (an `M5Canvas`)
  for the rare M5GFX call; `r.px()` is its raw buffer (RGB565, **byte-swapped**: write `Renderer::swap565(PAL_INK)`).

## Renderer (`renderer.h`)

Coordinates are screen pixels, (0,0) top-left. Colours are the `PAL_*` RGB565 constants of `config.h`. Alpha is 0..255.

| Call | Notes |
|------|-------|
| `clearPaper()` / `clear(color)` | start of every `drawX()`; paper = cream with a faint dot every 16 px (one memcpy) |
| `drawLayers(level, mask, x, y, alpha)` | the three materials with their textures; empty bytes are skipped, so it is cheap. `mask` reveals layers one by one (Review); `alpha` fades them |
| `ball(x, y, angle, sx, sy, Face)` | `angle` as `ball.angle` of the physics (the eyes turn by half of it); `sx, sy` squash/stretch; `Face::Happy` for Win |
| `flag(x, y, wave)`, `startPin`, `flagPin` | `(x, y)` = the pole base / the pin centre; `active` adds the dashed grab circle |
| `text(s, x, y, sizePx, color, Align, stroke)` | `y` = the **vertical middle of the capitals**. `stroke >= 0` adds an outline of that colour |
| `textWidth(s, sizePx)` | for layout (toast pills, centring a label next to an icon) |
| `roundRect(x, y, w, h, r, fill, stroke, lw)` | like canvas: the outline is centred on the edge; `stroke < 0` none; `roundRectA` takes a fill alpha |
| `disc`, `ring(.., a0, sweep)`, `dashedRing`, `line`, `polygon`, `polyline` | anti-aliased, float positions, canvas angles (0 = +x, clockwise); `line` has round caps |
| `fillRectA(x, y, w, h, color, alpha)` | translucent rectangle (the fast one for dimming a region) |
| `thumb(x, y, bytes, scale)` | the 60 x 36 4-bit thumbnail of a level file (`LevelStore::thumbs[i]`), integer scale |
| `frame565`, `straight565` | camera pictures (see the comments in the header) |
| `flash(color, alpha01)`, `dim(alpha01)` | full-screen blends (about 77 k pixels each: use sparingly, not every frame on a slow path) |
| `shakeX`, `shakeY` | set to a few pixels; `present()` shifts the whole frame (Play's hit shake). Reset to 0 |

**Text sizes.** `sizePx` is the simulator's nominal font size; it is mapped to the nearest built-in font by capital height
(no fractional scaling, it looks bad): <= 10 -> 6x8 bitmap (cap 7 px), 11-16 -> 16 px bitmap, bolded (cap 10), 17-21 ->
FreeSansBold 9pt (13), 22-31 -> 12pt (18), 32-43 -> 18pt (26), 44-67 -> 24pt (34), larger -> 24pt x 2. Plain ASCII only
(the fonts have no accents). The built-in fonts are wider than the simulator's rounded font: **measure with `textWidth()`
instead of assuming**, and check a long string in the gallery (text pages). Examples at 28 px: "YOU WIN!" is about 115 px wide.

**Speed.** Everything draws into PSRAM; the frame is one 153 KB push. `drawLayers` of a busy level and a full-screen
`flash`/`dim` are the heavy calls (each well under a frame on the device in estimates; measure on hardware, the
emulator's timings mean nothing). Ten icons, three buttons and a ball are noise.

## UI (`ui.h`)

Every tick: add buttons, `const char* hit = ui.update(in);`, compare with `strcmp` (the id is a string literal; the
pointer is not stable), react. Buttons are rebuilt every tick; pressed/hold state is kept by id.

| Kind | Default size | Look |
|------|--------------|------|
| `BtnKind::Tab` | any (bottom bar 48 px high, top bar 48 x 48) | square; white / `fill`; 2 px ink line on the `edge` side and on the right; `active` = 3 px ink frame; `pulse` = breathing white veil; `disabled` = dotted-paper colour, grey icon |
| `BtnKind::Chip` | 40 x 40 at y = 4 | rounded, translucent paper fill, 2 px ink outline; drawn 40, hit area 48 (`hit = 4`) |
| `BtnKind::Big` | e.g. 136 x 136 | rounded card with a hard shadow that it sinks into when pressed; icon 56 px unless `iconSize` |
| `BtnKind::Custom` | any | you draw it: `draw(r, btn, hold, user)`; `disabled = true` makes a display-only region that cannot be hit |

* `holdMs` (use `HOLD_MS`, `CLEAR_HOLD_MS`, `DELETE_HOLD_MS`): fires only after the hold, a ring fills green around the icon
  (`ui.holdFor(id)` is 0..1 for Custom buttons). `longId` + `longMs`: a long press fires `longId` *instead of* `id`.
  `onPress`: fires on touch-down (the Jump pad). Sliding off a button before release fires nothing.
* A touch within `HIT_FORGIVE` (6 px) of a button, and not inside another, still hits the nearest one.
* `ui.touchOnButton` is true while the current touch began on a button, including the tick of its release (so a press and release
  that land in the same tick still count): **a screen that also treats touches as strokes or jumps must ignore a touch that began
  on a button** (`t.pressed && !ui.touchOnButton`). `ui.touchStartedOnButton` is the same flag but keeps describing the last touch
  until the next press.
* Touch is one finger: a second finger ends the touch (a release edge) and the screen is ignored until every finger is up.
* Helpers in Game: `chip(id, icon, x, extra)`, `chipBack()`, `chipMenu()` (back top-left, menu top-right),
  `bar(items, widths, n)` (tabs along the bottom bar; widths add up to 320), `topBar(titleIcon, rightId, rightIcon, rightActive)`
  (list screens). `extra` is an optional `Btn` carrying `fill/active/holdMs/longId/longMs/iconSize`; id, position and size are set for you.
* `drawIcon(r, Icon, cx, cy, size, color)`: all 28 icons of `ui.ts`, any size (anti-aliased masks are cached per size; a
  screen with more than ~20 different icon/size pairs would thrash the cache, which is only slow, not wrong).
  `Icon::{Back, Menu, Play, Grid, Camera, Pencil, Undo, Restart, Home, Plus, Trash, Sound0, Sound1, Sound2, Up, Check,
  InkLess, InkMore, Help, Stop, Eraser, Ground, Spike, Bouncy, Ruler, Focus, Bulb, Level}`. Ink glyphs on coloured buttons.

## Game core (`game.h`, `game_core.cpp`)

| Call | What it does |
|------|--------------|
| `go(Screen)` | switch screen; `screenT = 0`, 0.15 s paper fade-in. State of the screen you leave stays in its `XState` |
| `goBack()` | "up one step" for the emulator's Esc key; the device uses the back chip, which each screen handles itself |
| `say("text")` | toast pill for 2.2 s (ASCII, <= 47 chars) |
| `spawn(n, x, y, colors, nColors, speed, life, size, kind)` | particles: kind 0 square, 1 disc, 2 expanding ring, 3 confetti (falls slower, spins); at most 96 (oldest dropped). Updated by `tick()`, drawn by `draw()` after your screen |
| `drawParts()` | only call it yourself to put particles *under* your buttons; it draws once per frame |
| `flash(color, seconds)` | the shutter flash, drawn over every screen but Play (Play draws `play.fxFlash` itself) |
| `cycleSound()`, `soundIcon()` | sound chip: cycles Off/Low/Med, saves settings, picks the icon |
| `openLevel(slot, play)` | load slot into `level` (blank if empty), remember it as last slot, then `startPlay()` or `go(Draw)` |
| `newInSlot(slot)` | save a blank level there, open it in Draw, toast "New level" |
| `startPlay()` | clears the pin-drop hole around a buried Start/Flag (and saves), new `World` from `level`, clear particles, `play.winT/spawnRing/tiltHint`, `go(Play)` |
| `markEdited()` / `saveCurrent()` | set the autosave flag (`dirtySave`, `lastEditAt`) / write the level if flagged; a failed write keeps the flag (retried after the next idle period) and toasts "Could not save" at most every 30 s |
| `leaveCamera()` | back out of the paper flow to the Chooser or the Title (`cameFrom`) |
| `playFxStep(dt)` | weak: decays the `PlayState` fx timers every tick; the Play agent may define a strong one |

## A worked example: a screen with two buttons

This is the real New-level chooser (`tickChooser`/`drawChooser`), the whole screen:

```cpp
// game_menus.cpp (excerpt)
#include "game.h"
#include <cstring>
namespace scribble {

void Game::tickChooser(const InputState& in) {
  topBar(Icon::Plus);                                          // white band, back tab at the left, plus icon in the middle
  Btn paper; paper.id = "paper"; paper.x = 16; paper.y = 72; paper.w = 136; paper.h = 136;
  paper.kind = BtnKind::Big; paper.fill = PAL_YELLOW; paper.icon = Icon::Camera; paper.iconSize = 72;
  ui.add(paper);
  Btn finger = paper; finger.id = "finger"; finger.x = 168; finger.fill = PAL_WHITE; finger.icon = Icon::Pencil;
  ui.add(finger);

  const char* hit = ui.update(in);                             // the id that fired this tick, or nullptr
  if (!hit) return;
  if (!std::strcmp(hit, "back")) go(Screen::Picker);
  else if (!std::strcmp(hit, "paper")) { cameFrom = Screen::Chooser; go(store.settings.standupSeen ? Screen::Check : Screen::Standup); }
  else if (!std::strcmp(hit, "finger")) {
    const int s = store.firstEmpty();
    if (s < 0) { say("All 10 slots are full"); go(Screen::Picker); } else newInSlot(s);
  }
}

void Game::drawChooser() { r.clearPaper(); ui.draw(r, nowMs); }   // the buttons are the whole picture

}  // namespace scribble
```

Delete the weak stub of the screen from `game_stubs.cpp` when you add the real functions (the real one overrides it anyway).

## Seeing what you drew

```bash
cd firmware
pio run -e emu_gallery && EMU_SCRIPT=emu/scripts/gallery.txt SDL_VIDEODRIVER=dummy .pio/build/emu_gallery/program
python3 emu/make_sheet.py /tmp/gallery/p[0-9][0-9].ppm /tmp/gallery/sheet.png --cols 4    # one contact sheet (needs Pillow)
```

The gallery (`emu/gallery/gallery_main.cpp`) shows the icons at 22/30/56 px, every button kind and state (tap and hold
them with the mouse), text at every size, a level with the three textures, ball and pins, thumbnails, particles/toast/flash/dim,
the pause-menu look and the camera pictures. Add a page there when you add a primitive. For your own screen, use the
full emulator (`pio run -e emu`) with a script of taps and `shot` commands (`emu/README.md`).
