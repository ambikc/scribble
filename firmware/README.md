# Scribble Run firmware (M5Stack CoreS3)

PlatformIO project. MIT licensed. Design: [`../docs/system-design.md`](../docs/system-design.md); plan: [`../docs/build-plan.md`](../docs/build-plan.md).

## What is here today (build-plan Step 0)

| Path | What |
|------|------|
| `platformio.ini` | `cores3` = the device (Arduino, M5Unified 0.3.0, PSRAM, 16 MB partitions); `native` = PC unit tests (Unity) |
| `partitions_16mb.csv` | app 3 MB, OTA slot 3 MB, LittleFS ≈ 9.9 MB, core dump 64 KB |
| `lib/core/src/level.*` | Level bitmaps, brush, `.scrb` encoder/decoder, CRC-32, thumbnail, built-in lessons. Port of `sim/src/level.ts` |
| `lib/core/src/physics.*` | The ball. Expression-for-expression port of `sim/src/physics.ts` |
| `lib/core/src/vision.*` | The vision pipeline and the live camera check (`visionProcess`, `visionRethreshold`, `visionLiveCheck`). Integer maths per pixel, all buffers in a caller-owned `VisionWork` (770,336 bytes: put it in PSRAM). Expression-for-expression port of `sim/src/vision.ts` |
| `src/main.cpp` | **Hardware-check app** for Steps 1–3 (the game itself comes later) |
| `test/test_core/` | Native tests: files and physics must match the simulator (`shared/generated/golden.h`) |
| `test/test_vision/` | Native tests: marks, warp, ink, layers, pins and the live check must match the simulator byte for byte (`shared/golden/vision_frames.bin`, `shared/generated/vision_golden.h`); also prints timing and buffer sizes. `pio test -e native -f test_vision` |

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
  `shared/golden/physics_vectors.json` and `shared/generated/golden.h`, and (`scripts/make-vision-golden.ts`)
  `shared/golden/vision_frames.bin`, `shared/golden/vision_vectors.json` and `shared/generated/vision_golden.h`.
* Physics uses `double` with float rounding at the same points as `Math.fround` in TypeScript, the same literal
  sin/cos table, and `sqrt(x*x+y*y)` rather than `hypot`, so the two agree to about 1e-6 px (allowed: 0.5 px).
* Level files must be **byte-identical** in both directions.
* Vision is integer maths per pixel; the few doubles use only `+ − × ÷ √` and literal tables (no `hypot`/`acos`/`atan2`/`cos`/`sin`), so the marks, the straightened page and the levels it makes are byte-identical to the simulator's.

**Not tested on real hardware yet.** Everything above compiles and the native tests pass; the first flash on a real
CoreS3 is build-plan Step 1.
