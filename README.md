# Scribble Run

**Draw a level with a crayon on paper (or with your finger), press Play, then tilt the device to roll a ball
through your drawing to the flag. Tap to jump.** An open-source game for the **M5Stack CoreS3**, designed to be built
by a parent and a 7-year-old together. MIT licensed.

> **Status:** design approved, browser simulator built, device firmware not started (the device is on its way).

## Try it now (no hardware needed)

```bash
cd sim
npm install
npm run dev        # open the address it prints
```

Draw with the mouse, tilt with the arrow keys, jump with Space. For **paper mode** print
[the A4 template](docs/assets/scribble-run-level-template-a4.pdf) (100 %, not "fit to page"), draw on it with a fat black
marker plus red and blue, photograph it, and press **Upload photo**, or use **Webcam**, or press **Demo photo** to see a
level drawn onto the template by a simulated camera (with stress presets: dim light, shadows, tilt, noise, blur).
**Download level** saves a `.scrb` file in the same format the device will use.

More in [sim/README.md](sim/README.md).

## What is in here

| Path | What |
|------|------|
| [`docs/`](docs/) | The design: [game design](docs/game-design.md), [screens](docs/screens.md), [system design](docs/system-design.md), [build plan](docs/build-plan.md) |
| [`sim/`](sim/) | The browser simulator (TypeScript, Vite, canvas, 320 × 240 shown at 3×) |
| [`shared/constants.json`](shared/constants.json) | One source of truth for physics, layout and vision numbers; `node tools/gen-config.mjs` writes `sim/src/config.ts` and `shared/generated/config.h` |
| [`shared/levels/`](shared/levels/), [`shared/golden/`](shared/golden/) | Built-in `.scrb` levels and golden physics vectors the firmware tests will replay |
| [`tools/template/`](tools/template/) | Generates the printable A4 level template PDF |

## How the pieces fit

The simulator uses the **same module structure, constants, level file format, physics and paper-vision code** as the
planned C++ firmware (`level`, `physics`, `vision`, `input`, `camera`, `renderer`, `audio`, `level_store`, `ui`, `modes`),
so it ports line by line. See [docs/system-design.md](docs/system-design.md).

## Licence

MIT, see [LICENSE](LICENSE).
