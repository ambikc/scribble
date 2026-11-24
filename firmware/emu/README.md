# The PC emulator (`pio run -e emu`)

There is no emulator for the CoreS3 itself (Espressif's QEMU does not model its screen, touch, camera or power chip).
So the firmware is built a second way: **the same `src/*.cpp` and `lib/core` compiled for the PC**, with M5GFX's SDL
backend as the display. Every drawing call, sprite, font and colour goes through the real M5GFX code; only the hardware
under it is replaced by [`include/M5Unified.h`](include/M5Unified.h):

| Device | Emulator |
|---|---|
| ILI9342C screen, 320 × 240 | M5GFX SDL window (or headless) |
| Capacitive touch | mouse, left button = one finger; scripts can add a second finger (`down2` / `move2` / `up2`): `getCount()` and `getDetail(0)` then behave like M5Unified (when finger 1 lifts, slot 0 becomes finger 2 with its own `id`) |
| BMI270 tilt | `←` `→` or `A` `D` (right edge down = +). Reports "flat on the table" (az = +0.97 g) until you press `U` (or run with `EMU_UPRIGHT=1`, or the script command `upright 1`): then it reports "standing on its base" (ay = -0.98 g) |
| Keys | `Space` / `W` = jump key, `Esc` = back key (they reach the game as `keyJump` / `keyBack`; the device has neither) |
| Speaker | `tone()` is printed as `[tone 550 Hz 60 ms]`, `playWav()` as `[wav 16044 bytes]`, `setVolume()` as `[volume 64]`. The game's audio adds `[sfx jump]` and `[say three wav]` / `[say three tone]` lines |
| LittleFS | a plain folder: env `EMU_FS`, default `./emu_fs` (created if missing). The game keeps `slotN.scrb`, `settings.json` there and looks for `three.wav` `two.wav` `one.wav` `snap.wav` (16 kHz mono) in it |
| Clock | real clock; **virtual 60 Hz clock when a script runs** (one loop pass = one tick) |
| Camera (GC0308) | **test photos** (see "The camera and the paper flow" below); no sensor, no exposure, no focus |
| microSD, battery, RTC, DMA timing | not emulated |

Timings from the emulator mean nothing. Use it for **logic, layout, screen flow and screenshots**; measure speed on the device.

## Run it

```bash
sudo apt-get install -y libsdl2-dev        # once (macOS: brew install sdl2)
cd firmware
pio run -e emu && .pio/build/emu/program   # a window opens: click = tap, arrows = tilt
```

## The game itself

`pio run -e emu` builds the game (`src/main.cpp`, once the Renderer / UI / Game code is in); `pio run -e emu_hwcheck` builds
the hardware-check app. The game keeps its levels in `$EMU_FS` (default `firmware/emu_fs/`, not committed): delete that folder
for a first run (the three lessons are written into slots 0-2); put a corrupt `slot3.scrb` in it to see a warning dot.

```bash
EMU_FS=/tmp/fs1 EMU_SCRIPT=emu/scripts/io.txt SDL_VIDEODRIVER=dummy .pio/build/emu/program | grep -E "sfx|say|tone|wav"
```

## Scripted, headless runs (used by CI)

```bash
EMU_SCRIPT=emu/scripts/smoke.txt SDL_VIDEODRIVER=dummy .pio/build/emu/program
```

A script is a text file, one command per line, times in **virtual milliseconds** (60 frames = 1000 ms):

```text
100 tap 240 216        # touch down at 100 ms, up 80 ms later
500 down 40 40         # finger down
520 move 60 40         # finger moves
540 up                 # finger up
560 down2 250 150       # a SECOND finger down (move2 x y / up2 too): the game ends the touch and ignores the screen until all are up
600 tilt 0.5           # accelerometer x in g (stays until changed)
650 upright 1          # IMU says "standing on its base" (0 = flat on the table, the default); stays until changed
700 key jump           # the jump key (or: back) pressed for 100 ms; also keydown <k> / keyup <k>
900 shot /tmp/a.ppm    # write the 320x240 screen as a PPM (convert with Pillow, ImageMagick, ...)
950 log checkpoint     # print a marker line
1000 quit              # end the process (exit code 0)
```

`emu/scripts/smoke.txt` visits every page of the hardware-check app; `emu/scripts/io.txt` exercises input, sound and the
level store through the game (taps, a drag, tilt, upright, keys, volume cycling, the win jingle). Output lines such as `page BALL` and `[tone ...]`
are plain text on stdout, so a test can `grep` them.

`pio run -e emu_unit && SDL_VIDEODRIVER=dummy .pio/build/emu_unit/program` runs unit tests of `ui.cpp`, `input.cpp` and `renderer.cpp`
on the emulated M5Unified (`emu/unit/unit_main.cpp`: same-tick taps, the second finger, the flash/dim fast paths); `emu/run_all.sh` runs it first.

## The camera and the paper flow

`app/camera.cpp` has an emulator half (`#ifdef EMU`): `Camera` serves test photos, the paper screens (`app/game_paper.cpp`) run unchanged.

| What | How |
|---|---|
| The photo | the synthetic template photos of `shared/golden/vision_frames.bin`, by case name from `shared/generated/vision_golden.h`: `clean tilt dim dark faded noisy harsh upside-down tilt-noisy nomarks blank`. Choose with env `EMU_PHOTO_CASE=tilt` or the script command `photo tilt` (it can change in the middle of a run); default `clean` |
| Your own photo | env `EMU_PHOTO=<file>` (a P6 PPM of any size, cover-fitted into 640 x 480 like the browser simulator does, or a raw 640 x 480 RGB565 little-endian dump; a phone photo of the printed template works once converted to PPM). `python3 emu/photo_from_golden.py 1 /tmp/tilt.ppm --raw` writes a stored photo in both formats |
| The live view | settles like a person adjusting the paper: out of focus for `EMU_SETTLE_MS` (default 900 ms) after the camera starts, then sharp, with frame-to-frame noise (`EMU_NOISE`, default +-2 levels), about 15 frames a second. The lights go green after the settling and GO pulses 0.5 s later. `EMU_START_MS` (150) = the camera's start-up time, `EMU_GRAB_MS` (200) = how long the three-frame snap takes, `EMU_CAMERA_FAIL=1` = a camera that does not answer |
| The snap | returns the stored photo exactly (the average of three identical frames), so the recovered level has the same CRC as the golden vectors |
| Output lines | `[camera on]`, `[camera off]`, `[camera exposure locked]`, `[paper lights ...]` (when the lights change), `[paper vision N ms ...]` (this computer's time: meaningless for the device), `[paper level crc XXXXXXXX]` (the `.scrb` CRC of the recovered level: the number `shared/golden/vision_vectors.json` lists as `crc.scrb`), `[paper accept slot N]` |
| Vision | runs inline in the tick that starts it (deterministic scripts); on the device it is a FreeRTOS task on core 0 |

```bash
cd firmware && pio run -e emu && emu/paper_test.sh     # about 65 checks over every golden photo and the whole flow; exit code 0 = all passed
OUT=/tmp/shots emu/paper_test.sh                       # keep the screenshots (PPM) and logs there; python3 emu/make_sheet.py /tmp/shots/clean_*.ppm /tmp/shots/s.png
```

`emu/scripts/paper_*.txt` are the scripts (`@OUT@`, `@CASE@`, `@NAME@` are replaced by `paper_test.sh`, which also prepends `photo <case>`).
The stand-up screen needs `upright 1` in a script (the emulated IMU starts "flat on the table").
