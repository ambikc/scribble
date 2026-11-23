# The PC emulator (`pio run -e emu`)

There is no emulator for the CoreS3 itself (Espressif's QEMU does not model its screen, touch, camera or power chip).
So the firmware is built a second way: **the same `src/*.cpp` and `lib/core` compiled for the PC**, with M5GFX's SDL
backend as the display. Every drawing call, sprite, font and colour goes through the real M5GFX code; only the hardware
under it is replaced by [`include/M5Unified.h`](include/M5Unified.h):

| Device | Emulator |
|---|---|
| ILI9342C screen, 320 × 240 | M5GFX SDL window (or headless) |
| Capacitive touch | mouse, left button = one finger |
| BMI270 tilt | `←` `→` arrow keys (right edge down = +) |
| Speaker | `tone()` is printed as `[tone 550 Hz 60 ms]` |
| Clock | real clock; **virtual 60 Hz clock when a script runs** |
| Camera, microSD, battery, RTC, DMA timing | not emulated (camera: use the browser simulator or the test photos) |

Timings from the emulator mean nothing. Use it for **logic, layout, screen flow and screenshots**; measure speed on the device.

## Run it

```bash
sudo apt-get install -y libsdl2-dev        # once (macOS: brew install sdl2)
cd firmware
pio run -e emu && .pio/build/emu/program   # a window opens: click = tap, arrows = tilt
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
600 tilt 0.5           # accelerometer x in g (stays until changed)
900 shot /tmp/a.ppm    # write the 320x240 screen as a PPM (convert with Pillow, ImageMagick, ...)
950 log checkpoint     # print a marker line
1000 quit              # end the process (exit code 0)
```

`emu/scripts/smoke.txt` visits every page of the hardware-check app. Output lines such as `page BALL` and `[tone ...]`
are plain text on stdout, so a test can `grep` them.
