# Scribble Run firmware (M5Stack CoreS3)

PlatformIO project. MIT licensed. Design: [`../docs/system-design.md`](../docs/system-design.md); plan: [`../docs/build-plan.md`](../docs/build-plan.md).

## What is here today (build-plan Step 0)

| Path | What |
|------|------|
| `platformio.ini` | `cores3` = the device (Arduino, M5Unified 0.3.0, PSRAM, 16 MB partitions); `native` = PC unit tests (Unity) |
| `partitions_16mb.csv` | app 3 MB, OTA slot 3 MB, LittleFS ≈ 9.9 MB, core dump 64 KB |
| `lib/core/src/level.*` | Level bitmaps, brush, `.scrb` encoder/decoder, CRC-32, thumbnail, built-in lessons. Port of `sim/src/level.ts` |
| `lib/core/src/physics.*` | The ball. Expression-for-expression port of `sim/src/physics.ts` |
| `hwcheck/main.cpp` | **Hardware-check app** for Steps 1–3 (`pio run -e hwcheck`) |
| `src/main.cpp` | The **game** entry point: `setup()` and the fixed 60 Hz `loop()` (see "Storage layout and the loop" below) |
| `test/test_core/` | Native tests: files and physics must match the simulator (`shared/generated/golden.h`) |

The constants come from `../shared/generated/config.h`, generated from `../shared/constants.json` by
`node ../tools/gen-config.mjs`. Never edit the generated files by hand.

## Commands

```bash
pip install platformio
```

```bash
pio test -e native                 # PC tests: no device needed
```

```bash
pio run -e cores3                  # build for the CoreS3
```

```bash
pio run -e cores3 -t upload && pio device monitor
```

If the upload can't find the board: hold the reset button for about 3 s until the green light blinks (download mode), then upload again.

## The hardware-check app

Tap the **right half of the bottom bar** for the next page and the **left half** to go back. Every page also prints its
numbers over serial, so you can paste them into [`../docs/hardware-notes.md`](../docs/hardware-notes.md).

| Page | What to do | Build-plan step |
|------|-----------|-----------------|
| INFO | Check the 9 palette colours (paper should look cream, not white), PSRAM and flash sizes | 1 |
| TOUCH | Tap the green square; it moves. Hit rate and touch sample rate are shown | 1 |
| SPEED | Tap to time a full-screen push and 40 × 40 pushes (SRAM sprite, PSRAM clip) | 1 |
| TILT | Raw accelerometer. Tap left to change the axis, right to flip the sign, until tilting the right edge down grows the bar right | 2 |
| BALL | The real physics on a small level: tilt to roll, tap to jump. Shows fps and physics µs per tick | 3 |
| SOUND | The win jingle and a volume step | 8 (preview) |

The tilt axis and sign you pick on the TILT page are not saved yet; write them in `hardware-notes.md`, and they become
constants.

## Keeping the C++ and the simulator in step

* The simulator writes the golden data: `cd ../sim && npm run golden` → `shared/levels/*.scrb`,
  `shared/golden/physics_vectors.json` and `shared/generated/golden.h`.
* Physics uses `double` with float rounding at the same points as `Math.fround` in TypeScript, the same literal
  sin/cos table, and `sqrt(x*x+y*y)` rather than `hypot`, so the two agree to about 1e-6 px (allowed: 0.5 px).
* Level files must be **byte-identical** in both directions.

**Not tested on real hardware yet.** Everything above compiles and the native tests pass; the first flash on a real
CoreS3 is build-plan Step 1.

## Storage layout and the loop

**Files** (LittleFS mounted at `/littlefs` on the device; a plain folder, `EMU_FS`, in the emulator and the native tests; all
access is ordinary `stdio`, so `src/app/level_store.cpp` also builds for the PC):

| File | What |
|------|------|
| `slot0.scrb` ... `slot9.scrb` | the ten levels, the `.scrb` format of `docs/system-design.md` section 4 (24,184 bytes with a thumbnail) |
| `settings.json` | `{"sound":1,"lastSlot":0,"solvedMask":0,"firstRunDone":true,"standupSeen":false}` (read by a tiny hand-written parser; unknown keys ignored, a corrupt file means defaults) |
| `three.wav` `two.wav` `one.wav` `snap.wav` | optional countdown clips, 16 kHz 16-bit mono; without them the countdown uses tones |
| `slotN.tmp` | only while a save is in flight; removed at the next boot |
| `slotN.bad` | a corrupt level that was saved over or removed; kept, never deleted |

* **Saves are atomic**: write `slotN.tmp`, `fflush` + `fsync`, `rename` over `slotN.scrb`. A power cut, or a full or failing
  flash, leaves the old file intact; `save()` then returns `false` and the in-memory copy still follows the caller.
* **A bad file never crashes and is never deleted**: the slot loads as empty with `broken[i]` set (a warning dot).
* **First run** (no slot file at all and `firstRunDone` false): the three lessons are written into slots 0-2.
* Slot data lives in PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, plain `malloc` on the PC), allocated lazily; the thumbnails
  (1,080 bytes each) are kept in SRAM. Native tests: `test/test_store`.

**The loop** (`src/main.cpp`, `docs/system-design.md` 6.1): `M5.update()` once per pass, then `input.sample()` (latches tap edges
even on a pass that runs no tick), then the accumulator: add the real elapsed time (clamped to 0.1 s), run at most 6 ticks of
`game.tick(input.poll(now)); input.consumeEdges();`, then `audio.update(now)` (releases delayed tones such as the win jingle),
`game.draw()` and `renderer.present()` once per pass. Tilt is filtered once per tick (`lib/core/src/tilt.h`: dead-zone,
linear to `TILT_FULL`, low-pass, same maths as `sim/src/input.ts`). With an emulator script the clock is virtual and one pass
is exactly one tick.

**Input assumptions to confirm on the device** (build-plan Step 2): `TILT_AXIS` / `TILT_SIGN` in `src/app/input.h` (the rolling
axis); `upright` ("standing on its base, screen facing the user") is true when gravity is mostly along the screen's vertical
axis, either sign, and not rolled sideways (`UPRIGHT_*` in `tilt.h`). Speaker levels `VOL_LOW` / `VOL_MED` in `audio.h` are
guesses to tune by ear.
