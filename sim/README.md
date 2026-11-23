# Scribble Run simulator

A single-page browser version of the game: TypeScript + Vite + an HTML canvas of exactly **320 × 240** (the CoreS3 screen),
shown at **3×**. It runs the same physics, level format and paper-vision pipeline that the firmware will.

```bash
npm install
npm run dev        # development server
npm test           # 56 unit tests: level format, physics, golden vectors (physics and vision), vision round-trips
npm run build      # typecheck + production build into dist/
```

## Hosted version

`npm run build && node scripts/make-artifact.mjs` makes `dist/artifact/page.html`, one self-contained page (the app and its vision worker are inlined) that can be hosted as a claude.ai Artifact. Differences there: webcam and phone tilt are blocked by the host, so use **Upload photo** / **Demo photo** and the on-screen tilt arrows; and **Download level** saves the JSON twin (`.json`), because `.scrb` is not an allowed download type. Turn it into the device file with `npx tsx scripts/json-to-scrb.ts level.json`.

## Controls

| Action | Mouse / keyboard | Device |
|--------|------------------|--------|
| Draw, drag pins, press buttons | mouse or finger on the screen | the touch screen |
| Roll the ball | `←` `→` (or `A` `D`), the on-screen arrows (hold), or **Phone tilt** | tilt (BMI270) |
| Jump | `Space` / `↑` / click or tap anywhere on the playfield / the green pad | tap |
| Go back | `Esc` (a convenience; on the device use the on-screen ← chip) | on-screen ← |

## Paper mode in the browser

1. Print `docs/assets/scribble-run-level-template-a4.pdf` at 100 % (it is also linked from the page).
2. Draw on it (fat black, red, blue), photograph it landscape with all four corner marks in view.
3. **Upload photo** (or **Webcam**), then use the big **GO** button on the *Camera check* screen. Four lights show distance,
   sharpness, light and tilt, as on the device. A voice counts "3, 2, 1, snap!" (browser speech), then the *Review* screen
   shows what was understood. **Demo photo** needs no paper at all.
4. The **vision panel** under the screen shows the straightened page, the ink strength and the final ground/spike/bouncy map.

## Module map (simulator file ↔ firmware file)

| `src/` | Role | Firmware |
|--------|------|----------|
| `config.ts` | generated constants | `config.h` |
| `level.ts` | bit maps, `.scrb` codec, thumbnails, built-in lessons | `level.h/.cpp` |
| `physics.ts` | fixed 60 Hz ball simulation and collision | `physics.h/.cpp` |
| `vision.ts` | find marks, straighten, ink, classify, clean up, live check | `vision.h/.cpp` |
| `synth.ts` | a synthetic camera shot of the template (tests and *Demo photo*) | (simulator only) |
| `input.ts`, `camera.ts`, `renderer.ts`, `audio.ts`, `level_store.ts` | hardware adapters (browser versions) | M5Unified, esp32-camera, M5GFX, I2S, LittleFS |
| `ui.ts`, `modes.ts` | widgets and the state machine with every screen | `ui`, `modes` |
| `vision.worker.ts` | runs `vision.ts` off the main thread (the firmware's vision task) | task on core 0 |

Scripts: `scripts/vision-report.ts` (sweep the synthetic camera and print how well levels are recovered),
`scripts/make-golden.ts` and `scripts/make-vision-golden.ts` (regenerate `shared/levels`, `shared/golden` and the generated headers; `npm run golden`), `scripts/e2e-smoke.cjs` (drive the built page
through every screen with Playwright).
