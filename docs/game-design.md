# Scribble Run — Game Design

> **Status:** design draft for review · **Target:** M5Stack **CoreS3** (320×240 touch screen, camera, tilt sensor, speaker) · **License:** MIT
>
> Companion docs: [screens.md](screens.md) · [system-design.md](system-design.md) · [build-plan.md](build-plan.md)

## 1. The one-sentence game

**Draw a level with a crayon on paper and snap it with the camera (or draw it with your finger), press Play, then tilt the device to roll a ball through your drawing to the flag. Tap to jump.**

### What the CoreS3 means for the design

| CoreS3 has | We use it for |
|------------|---------------|
| 2.0″ 320×240 capacitive touch screen | All controls (there are **no hardware buttons**), finger drawing |
| GC0308 camera (0.3 MP, front, below the screen) | **Paper mode**: photograph a crayon drawing and turn it into a level |
| BMI270 6-axis IMU | Tilt to roll the ball |
| 1 W speaker + amp | All feedback is **sound + screen effects** |
| 16 MB flash, 8 MB PSRAM, microSD | Levels in flash (LittleFS), optional export to microSD |
| 500 mAh battery | A play session; see §11 |
| **No** vibration motor, **no** A/B/C touch buttons | Nothing buzzes; every control is an on-screen target |

### Design pillars

| # | Pillar | What it means in practice |
|---|--------|---------------------------|
| 1 | **Fun in 10 seconds** | First launch drops you into a half-made level with a visible gap and a ghost finger drawing a bridge. A paper level is two taps from the Title screen. |
| 2 | **No reading** | Every action is an icon, a colour, or a sound. Numbers 1–10 and the words "You win!" are the only text. |
| 3 | **Nothing feels like failing** | No lives, no game-over. A spike is a funny *pop* and the ball is back at the start in half a second. A bad photo is "try again", never an error. |
| 4 | **The drawing is the game** | Winning is the cue to *change the level*. Draw → snap → play → tweak. |
| 5 | **Can't be broken** | Auto-save, undo, and a hold to confirm anything destructive. |

---

## 2. Core loop

```mermaid
flowchart LR
    PA["PAPER<br/>crayon on white paper"] -->|"snap"| RV["REVIEW<br/>looks good?"]
    RV -->|"retake"| PA
    RV -->|"looks good"| PL["PLAY<br/>tilt to roll, tap to jump"]
    FI["FINGER<br/>draw on the screen"] -->|"play"| PL
    PL -->|"spike or pit:<br/>instant retry"| PL
    PL -->|"reach the flag"| WI["WIN<br/>confetti and jingle"]
    WI -->|"edit"| ED["EDIT<br/>touch up with finger"]
    ED -->|"play"| PL
    WI -->|"play again"| PL
    WI -->|"new level"| PA
    WI -->|"new level"| FI
    PL -->|"edit (hold)"| ED
```

### The first 10 seconds (the script we design to)

**Paper path** (the headline):

| Time | What he sees / does | Why |
|------|---------------------|-----|
| 0 s | Power on. Title screen: a ball rolls and jumps over the logo. | Motion + colour say "game". |
| 2 s | Taps the **Paper** tile. The camera preview appears with a dashed framing guide. | One tap to the magic. |
| 3–6 s | Holds up the crayon drawing, lines it up with the guide, taps the big **Snap** button. Shutter click, brief white flash. | Physical and playful. |
| 6–7 s | Review screen: his drawing, recoloured (black ground, red spikes, blue bouncy) with a Start ball and a Flag already placed. | "It understood my drawing!" |
| 8 s | Taps the big green **Looks good**. The ball drops and he tilts. | One obvious next step. |

**Finger path** (no paper handy, and first launch): Title → **Play** opens the **starter level** in Draw mode: Start and Flag on two ledges with a wide gap between. After 3 s of no touching, a ghost finger draws a bridge. He draws, presses the green **Play** button, tilts, and wins.

### Built-in "lesson" levels

Slots 1–3 ship pre-filled. They teach one material each by playing, with no text:

| Slot | Teaches | Layout idea |
|------|---------|-------------|
| 1 | **Ground** | The starter level (gap to bridge). |
| 2 | **Spike** | A hill with a red patch on top; draw a bridge or go around. |
| 3 | **Bouncy** | Flag on a high ledge that a jump can't reach; a blue pad below it. |

They are normal, editable levels.

---

## 3. Ways to make a level

### 3.1 Paper mode (the headline feature)

**The child draws on white paper** with a thick **black** crayon or marker (ground), **red** (spikes) and **blue** (bouncy). He holds the paper up to the camera, sees a live preview with a framing guide, and taps **Snap**. The picture is converted to a level (the pipeline is in [system-design.md §5](system-design.md#5-the-vision-pipeline-paper-mode)):

* ink darker than the paper → **Ground**; red ink → **Spike**; blue ink → **Bouncy**;
* specks are removed, thin lines are thickened to at least 8 px;
* **Start** is placed on the left, **Flag** on the right (both draggable);
* a **Review** screen shows the detected materials in colour over the photo: **Looks good** or **Retake**.

**The "crayon guide"** (we print it on the back of the first sheet of paper, or just tell him):

| Do | Don't |
|----|-------|
| White paper, flat, facing the screen | Coloured or crumpled paper |
| **Fat** black crayon / marker, press hard (strokes ≥ 4 mm wide) | Pencil, thin pens (they vanish at 0.3 MP) |
| Bright, even light from in front | A hand or the device's shadow over the paper |
| Draw big: fill the page | Tiny doodles in a corner |

**Honest limits** (full detail in [system-design.md §5.7](system-design.md#57-limits-and-fallbacks)): the camera is only 0.3 MP, so 1 pixel ≈ 1 mm on an A4 page; glare, shadows and a tilted page all hurt; pale colours (yellow, light blue) aren't detected. That's why there's always a Review screen, a one-tap Retake, "more ink / less ink" buttons, and a finger Edit mode to fix mistakes.

### 3.2 Finger mode

Draw and erase directly on the screen with a toolbar of **Ground, Spike, Bouncy, Eraser, Undo, Play**. **Start** and **Flag** are pins you drag. **Clear** is in the menu (hold to confirm).

### 3.3 Edit mode

The same screen as finger mode, opened on an existing level or right after a paper capture, so he can touch up what the camera got wrong (fill a gap, erase a smudge, move the flag).

---

## 4. The world

### 4.1 Playfield

* **320 × 192 px** logical world, 1 world pixel = 1 screen pixel. It occupies the top 192 px of the screen; the lowest **48 px** is the *bottom bar* (tools or actions; see [screens.md](screens.md)).
* **Why 192 (5 : 3) and not the full 240?** The bottom bar needs ≥ 44 px for touch targets. Paper mode crops the camera frame (320×240) to a centred 320×192, and the framing guide shows exactly that crop.
* Coordinates: `x` right, `y` down, origin top-left; velocities in **px/s**, accelerations in **px/s²**.
* **Edges:** left and right edges are solid invisible walls. The top is open. The bottom is a **pit**: when the ball centre falls below `y = 192 + 3·radius`, it respawns (§9).

### 4.2 Materials and objects

| Thing | Colour on screen | Colour on paper | Behaviour |
|-------|------------------|-----------------|-----------|
| **Ground** | Black | Black crayon/marker | Solid. The ball rolls and stands on it. Low bounce. |
| **Spike** | Red + diagonal ticks | Red crayon | **Not solid.** Touching it with the ball's inner hit-circle (75 % of the ball size, deliberately forgiving) pops the ball → back at Start. |
| **Bouncy** | Blue + dot texture | Blue crayon | Solid. Launches the ball **along the surface normal at 480 px/s** (≈ 2.5× a jump). |
| **Start** | Green ring around the ball | — (auto-placed) | Where the ball (re)appears. Exactly one. |
| **Flag** | Green flag on a pole | — (auto-placed) | Touching it (16 px radius) wins. Exactly one. |
| **Eraser** | — | — | Removes all three materials (finger mode only). |

Every material has a **texture as well as a colour** so the game works for colour-blind players (about 1 in 12 boys); see [screens.md §Visual style](screens.md#visual-style).

### 4.3 Drawing rules (finger mode and Edit)

* **Brush:** round, **8 px** diameter. **Eraser:** **20 px**.
* The brush stamps discs along the segment between successive touch samples (≤ 2 px apart) so fast strokes leave no gaps.
* **Materials are exclusive:** painting one clears the other two at those pixels.
* **Minimum thickness is 8 px** (paper mode enforces this by thickening), which lets the physics prevent tunnelling.
* **Pins** (Start, Flag): drag within a 24 px grab radius. On drop, all material within 12 px (Start) / 14 px (Flag) is cleared so a pin is never buried.
* **Undo:** 8 levels. **Clear** (menu) needs a hold and is undoable. **Auto-save** when leaving Draw/Edit and after 5 s idle.

### 4.4 The starter level

| Item | Value (world px) |
|------|-------------------|
| Start (ball centre) | (40, 104), resting on the left ledge |
| Flag base | (280, 112), pole on the right ledge |
| Left ledge (Ground) | x 16 – 72, y 112 – 120 |
| Right ledge (Ground) | x 248 – 304, y 112 – 120 |
| Gap | 176 px: too wide to jump (max jump ≈ 100 px), so he *must* draw. |

---

## 5. Controls (all on-screen)

### 5.1 Touch targets

* Every target is **≥ 44 × 44 px**; our standard is **≥ 48 px**; primary buttons are 80 px or wider.
* ⚠️ **Physical size:** the screen is 2.0″ ≈ **7.9 px/mm**, so 44 px ≈ **5.6 mm**, smaller than a child's fingertip (≈ 8–10 mm). So: 48 px hit areas everywhere, forgiving hit-testing (nearest target within 6 px wins), and the most-used actions get the biggest buttons.
* Single touch only. A second finger is ignored.

### 5.2 The top-bar pattern (same on every screen)

* **← Back is always top-left. ☰ Menu is always top-right.** Both have 48 × 48 px hit areas.
* On list-style screens (Level picker, New level) they sit in a full-width 48 px **top bar**.
* On screens that show the level (Draw, Play, Camera, Review) they float as small translucent **corner chips** over the playfield, so the canvas keeps its full size.
* What **←** does: *go up one step, saving first.* In Draw → Level picker. In Camera/Review → previous step. In Play → **Edit** (back to Draw), see the hold rule below.
* The Title screen has no ← (nothing above it); the Win screen has no top bar (three big buttons only).

### 5.3 Controls by mode

| Mode | Touch | Tilt |
|------|-------|------|
| **Title** | Tap **Levels**, **Paper**, **Play** (green, biggest) or **Draw** (bottom tiles); tap 🔊 (top-right) to cycle sound | — |
| **Level picker** | Tap a filled slot → Play it; tap an empty slot → **New level** chooser; **←** Title; 🗑 toggles delete mode | — |
| **New level** | Big **Paper** or **Draw** buttons; ← back | — |
| **Camera** | **Snap** (big green) · **Draw** (switch to finger) · ← back | — |
| **Review** | **Looks good** (green) · **Retake** · **− ink / + ink** · drag the Start/Flag pins · ← retake | — |
| **Draw / Edit** | Drag = paint with the selected tool · drag a pin to move it · bottom toolbar: **Ground, Spike, Bouncy, Eraser, Undo, ▶ Play** · ← Levels · ☰ menu | — |
| **Play** | **Tap anywhere = jump** · big **Jump** pad (bottom) · **Restart** (hold 0.4 s) · **←** Edit (hold 0.4 s) · ☰ menu | **Roll** left / right |
| **Win** | **Edit** · **Play again** (green) · **New level** | — |
| **Menu** | Tap a big button, or ← / tap outside to close | — |

> **Two deliberate differences from the brief's toolbar list.** Eight toolbar items (Ground, Spike, Bouncy, Start, Flag, Eraser, Clear, Play) would each be only 40 px wide, below the 44 px minimum. So **Start and Flag are draggable pins** (no tool needed) and **Clear moves to the menu** (hold to confirm). That leaves six 53 px toolbar buttons, including **Undo**. Open question 1 asks whether you'd rather keep them as tools.

* **Why hold for Edit / Restart in Play?** On a small device a child's thumbs rest on the screen edges and bottom corners, exactly where those buttons are. A 0.4 s hold (with a ring that fills) prevents accidental exits. **Jump is instant.**
* **Grip-safe jump:** a touch that *begins* within 12 px of the left or right screen edge doesn't count as a jump (it's a thumb holding the device). The margin is a constant to tune on the device.
* **Jump only from the ground**, with a little forgiveness: a tap up to 0.10 s after rolling off a ledge still jumps, and a tap up to 0.12 s before landing is remembered.

### 5.4 Tilt mapping

* Only **left–right tilt** from the BMI270, as a fraction of gravity `ax` (≈ sin of the tilt angle).
* **Dead-zone** 0.06 g (≈ 3.5°). **Full speed at 0.40 g (≈ 24°).** Linear in between. Light low-pass filter (α = 0.25 per tick).
* Holding the device pitched back doesn't matter: roll is measured on a different axis.
* Axis and sign are confirmed on the real hardware ([build-plan.md Step 2](build-plan.md#step-2--tilt-sensor)).

---

## 6. Physics feel

One shared constants file feeds both the simulator and the firmware ([system-design.md §9](system-design.md#9-code-sharing-between-simulator-and-firmware)). **Starting values** (tune by playing):

| Constant | Start value | Unit | What it feels like |
|----------|------------:|------|--------------------|
| `BALL_RADIUS` | **8** | px | 16 px across; the screen is 20 ball-widths wide. |
| `GRAVITY` | **900** | px/s² | A 100 px drop takes 0.47 s. Snappy. |
| `JUMP_SPEED` | **300** | px/s | Apex = v²/2g = **50 px**, 0.67 s in the air. |
| `BOUNCY_SPEED` | **480** | px/s | Apex = **128 px** (≈ 2.5× a jump), 1.07 s in the air. |
| `MAX_SPEED` | **520** | px/s | Terminal velocity (also bounds tunnelling risk). |
| `MAX_ROLL_SPEED` | **150** | px/s | Across the screen in ~2 s at full tilt. |
| `ROLL_ACCEL` | **700** | px/s² | Full roll speed in 0.21 s. |
| `AIR_CONTROL` | **0.5** | × | Tilt steers at half strength in the air. |
| `GROUND_FRICTION` | **500** | px/s² | With no tilt, stops from full speed in 0.3 s. Ball sticks on gentle slopes (< ~30°). |
| `RESTITUTION` | **0.25** | — | Small bounce on landing. |
| `BOUNCE_MIN_SPEED` | **60** | px/s | Slower impacts don't bounce. |
| `COYOTE_TIME` / `JUMP_BUFFER` | **0.10 / 0.12** | s | Jump forgiveness. |
| `TILT_DEADZONE` / `TILT_FULL` | **0.06 / 0.40** | g | See §5.4. |
| `SPIKE_HIT_SCALE` | **0.75** | × radius | Forgiving spike hitbox. |
| `FLAG_HIT_RADIUS` | **16** | px | Generous goal. |
| `RESPAWN_DELAY` | **0.5** | s | Pop → back at Start. |

**Derived checks:** a full-speed jump travels `150 × 0.67 ≈ 100 px`, so gaps up to ~100 px are jumpable and the starter's 176 px gap is not. To change gravity but keep jump height `h`, set `JUMP_SPEED = √(2·GRAVITY·h)`.

**Rolling model (simple and forgiving)**

```
target_vx = tilt × MAX_ROLL_SPEED
rate      = grounded ? ROLL_ACCEL : ROLL_ACCEL × AIR_CONTROL
if grounded or tilt ≠ 0:  vx moves toward target_vx by at most rate × dt
(no tilt, grounded)       →  rate = GROUND_FRICTION  (the ball settles)
(no tilt, airborne)       →  vx unchanged (momentum is kept)
```

**Paper levels and physics:** a paper level can be much more detailed than a finger-drawn one (ink thicker than 8 px, odd angles). Collision uses the same 1-bit maps either way, so nothing special is needed; the thickening step in the vision pipeline guarantees the minimum thickness.

---

## 7. Feedback: win, lose, and everything in between (no vibration)

The CoreS3 has **no vibration motor**, so every event is **sound + a screen effect**. Sounds are synthesised tones (no audio files in v1). The 1 W speaker is louder than a typical Core2, so the default volume is **Low**.

| Event | Sound | Screen effect |
|-------|-------|---------------|
| Pen down / drawing | Very quiet 12 ms tick every ~24 px; pitch by material (Ground 220 Hz · Spike 880 Hz · Bouncy 440 Hz) | Ink appears instantly under the finger. |
| Button press | 1 kHz tick, 15 ms | Button depresses (shadow 4 px → 1 px). |
| **Snap (camera)** | Shutter "click" (two quick ticks) | White flash 80 ms, then the frame freezes for 0.3 s. |
| **Review ready** | Rising "ta-da" (C5 → G5) | Detected materials pop in colour, one material at a time (0.4 s). |
| **Detection looks poor** | Soft low two-note "hmm" (never a harsh buzz) | Friendly paper-with-question-mark icon; **Retake** pulses. No red crosses, no error text. |
| **Jump** | Rising chirp 400 → 700 Hz, 80 ms | Ball stretches tall (1.25×) for 0.1 s. |
| **Land** (fall > 150 px/s) | Soft thump 150 Hz, 30 ms | Ball squashes flat 0.08 s, dust puff. |
| **Bouncy launch** | "Boing" 300 → 900 Hz, 140 ms | Pad squishes; ball stretches; 4 blue sparkles. |
| **Spike → pop** | Pop + falling 700 → 150 Hz, 160 ms | 8 red/white particles; **2 px screen shake** for 150 ms; red edge flash 100 ms; faint grey **oops puff** stays until the level is edited. |
| **Fell in the pit** | Soft "whoosh" 500 → 200 Hz, 80 ms | Ball fades out (no burst; it's not a punishment). |
| **Respawn** | Tiny "bloop" 300 → 500 Hz | Ball grows from nothing at Start with an expanding ring (0.2 s). |
| **Flag reached (win)** | Jingle C5-E5-G5-C6 (523 · 659 · 784 · 1047 Hz, 90 ms each) | **Full-screen green-white flash 120 ms**, flag waves, ring burst, ball hops, 24 confetti pieces, 0.3 s slow-motion, then the **"You win!"** screen. |
| **Win screen** | Last note held, then silence | Big star bounces in; confetti keeps falling. |
| Sound **Off / Low / Med** | Cycle on the Title and menu 🔊 button | Speaker icon with 0 / 1 / 2 arcs. |

---

## 8. Respawn rules (instant retry)

* **Spike:** pop at once; the ball reappears at Start after **0.5 s**. No input needed.
* **Pit:** the ball fades and reappears (0.4 s).
* **Stuck inside ground** (rare): the physics pushes the ball out; if it can't, it quietly respawns.
* **No counter, no lives, no timer** in v1. Deaths are never shown as a number.
* **Oops puffs** mark the last 5 death places so he can see *where* to fix the level.

---

## 9. Fun for a 7-year-old, and what could go wrong

### 9.1 Why this should work

* **Real crayons become a game.** The leap from paper to playing is the wow moment. Nothing else like it in a kid's hands.
* **Authorship:** he made the level, so winning is *his* win.
* **Immediate feedback:** ink under the finger, the ball moving the instant he tilts, a sound and an effect for every event.
* **Two-player-ish:** "draw me a level I can't beat" is a built-in game with Dad.
* **Discovery:** a blue ramp flings the ball sideways, a red patch under a bridge is a trap. He'll find combinations we didn't design.

### 9.2 Frustration risks and mitigations

| Risk | Why a 7-year-old would hate it | Mitigation |
|------|-------------------------------|------------|
| **Magic fails** | He draws something lovely, the camera mangles it. | The **Review** screen *always* shows what was understood; **Retake** is one tap; **− / + ink** fixes dark/light photos; **Edit** lets him repair with a finger; failure messages are friendly pictures, not errors. |
| **Bad light / shadows / tilted page** | The result is garbage and he can't tell why. | A **LIGHT hint** icon on the camera screen; a live brightness check that shows a lamp icon when the picture is too dark; the framing guide asks for "paper flat, facing the screen"; honest limits documented. |
| **Pencil / thin lines vanish** | His careful pencil drawing produces an empty level. | The crayon guide; "too little ink" detection shows "draw bigger and darker" as a picture; he keeps the photo preview to compare. |
| **Fat finger, tiny screen** | Can't hit small buttons. | ≥ 48 px targets, forgiving hit-testing, a 8 px brush, ink exactly under the touch point. |
| **Spikes feel punishing** | Dying repeatedly. | Instant 0.5 s retry, forgiving 75 % hit-circle, no counters, a funny pop, oops puffs. |
| **Too hard to control** | Ball is twitchy or sluggish. | Gentle tilt (24° = full speed), dead-zone, smoothing, sticky friction, jump forgiveness. Tune together with him. |
| **Impossible level** | He draws one he can't win. | Pit respawn is non-punishing; Edit is one tap away; undo; no "failed" state. |
| **Lost work** | Accidentally erases or exits. | Auto-save, 8-level undo, Clear needs a hold and is undoable, delete needs a hold. |
| **Thumbs trigger buttons** | Gets thrown out of Play. | Edit/Restart need a 0.4 s hold; edge-touch rejection for jumps; test with his real grip. |
| **Holding paper and device** | Needs three hands. | Lean the device against something, or let him hold the paper to the screen while you tap Snap. A self-timer (3-2-1) is a cheap addition. Tested in build Step 6. |
| **Reading** | Can't read menus yet. | Icons and colours; only "You win!" and 1–10. |
| **Sound is annoying** | A 1 W speaker is loud. | Default Low, one-tap mute, short tones. |
| **Battery dies** | Tears. | 500 mAh is modest and the camera + screen are thirsty: auto-save everywhere, a battery icon below 20 %, screen dims after 60 s idle. |

---

## 10. Open questions for review

1. **Start/Flag as pins, Clear in the menu** (because 8 toolbar buttons would be 40 px). Okay? Alternative: a two-row toolbar that costs 44 px of canvas.
2. **Camera crop.** The level is 5 : 3 (320×192) but the camera is 4 : 3 (320×240), so the top and bottom 24 rows of the photo are cropped (the framing guide shows this). Alternatively the level could be the full 320×240 and the toolbar could float over it.
3. **Front-facing camera ergonomics.** The camera is on the *front*, so he holds the paper in front of the screen with the drawing facing the device. We must test this feels natural (propping the device up with a stand helps). The preview is **not mirrored** so left/right match the paper.
4. **Hold-to-activate** for Edit and Restart in Play (0.4 s). Okay?
5. **Reset lessons** (restore slots 1–3) is deferred to later.

---

## 11. Stretch ideas (not in v1; the data format reserves room)

| Idea | Design | Hooks already in the design |
|------|--------|-----------------------------|
| **⭐ Stars to collect** | Up to 3 stars per level; winning with 0–3 stars is shown on the thumbnail. In paper mode, a **yellow** crayon dot could be a star. | `extras` chunk in the level file (`type = 1`, x, y; max 8). |
| **⏱ Timer / best time** | Shown only on the Win screen; none during play (no pressure). | `settings.bin` stores `bestTimeMs[10]`. |
| **👻 Ghost race** | The device records your best run (tilt + jump per tick ≈ 1 byte/tick) and replays it as a faint ball. Needs deterministic physics, which the fixed 60 Hz step gives. | Fixed timestep; no `random()` in physics. |
| **👨 Dad's challenge** | Slot 10 gets a gold border. Dad draws a nasty level and plays it once (his ghost is saved); the child tries to beat Dad's ghost. | Ghost race + a flag bit in the level header. |
| **📷 Scan a shared level** | **The CoreS3 camera can scan a QR code** shown on another device, so levels can be shared without Wi-Fi. Showing a QR needs the level compressed to ≲ 1 KB, so the *scanning* side is easier than the *showing* side; see [system-design.md §10.2](system-design.md#102-sharing-without-wi-fi). | `encoding = 1` (RLE) reserved in the header. |
| **💾 microSD export / import** | Copy `.scrb` files to/from a microSD card; the simulator's "Download level" makes the same file. | Same file format everywhere. |
| **🔄 Auto-deskew** | Find the paper's corners and straighten a tilted page. | Vision pipeline has a stage slot for it. |
| **🎨 Colours for new things** | Green crayon = a moving platform, yellow = a star. | Extra hue ranges in the vision config. |
