# Day-one runbook: the first day with the real CoreS3

For a parent who is comfortable in a terminal but is not an embedded engineer. The firmware has been written and tested on a PC
(native tests plus the PC emulator) but **has never run on the device**. This runbook walks through the first flash, the
measurements, the game, the camera and what to send back. Background: [build-plan.md](build-plan.md) (Steps 0-7),
[hardware-notes.md](hardware-notes.md) (the table you fill in), [firmware/README.md](../firmware/README.md).

How to read it: every step has **Do**, **Success** and **If not**. A line marked *unverified* is something nobody could check
without the device (or without the real tools on the machine that wrote this). Do the steps in order; each one makes the next
one safer. Budget: about 3 hours with a break, plus one longer session for the camera.

| Part | What | Roughly |
|---|---|---|
| 0 | Table, tools, PC sanity check | 30 min |
| 1 | Hardware-check app: screen, touch, tilt, speaker | 40 min |
| 2 | Flash the game, smoke test every screen | 20 min |
| 3 | Power-cut test of saving | 30 min |
| 4 | Camera bring-up | 60 min |
| 5 | Timing the vision | 15 min |
| 6 | Troubleshooting table (use when stuck) | |
| 7 | What to send back | 15 min |

---

## 0. Before you start

### On the table

* The CoreS3 and a **USB-C cable that carries data**. Many cables are charge-only; if no serial port ever appears, swap the cable first.
* A **stand** for the device (its base, or a small phone stand). The camera looks out of the back-facing side of the stand-up position, so it must stand upright.
* The **printed template**: [assets/scribble-run-level-template-a4.pdf](assets/scribble-run-level-template-a4.pdf), printed at **100 % / Actual size** on white A4. Measure the printed 10 cm line with a ruler; it must read 10.0 cm +-1 mm, or every distance number later is wrong.
* A **ruler or tape measure** (distances 30-50 cm), a **fat black marker or crayon**, a **red** and a **blue** one, a **desk lamp**.
* A phone for photos of the screen (several steps ask for them).
* A microSD card is **optional and not used yet**: the firmware has no SD code (build-plan: "microSD import/export" is not started).
* Your son, for the touch and tilt tests. Do the bring-up first; give him the device once parts 1-2 are green.

### Install PlatformIO and get the code

```bash
pip install platformio
```

```bash
git clone https://github.com/<your-account>/scribble.git && cd scribble/firmware
```

(Use the real clone URL of this repository. Every command below is run from the `firmware/` folder.) The first `pio run`
downloads the ESP32 toolchain and libraries (a few hundred MB, a few minutes). `pio` must be on your PATH; if it is not,
use `python -m platformio` instead of `pio`.

### USB serial: which port, per operating system

The CoreS3 has no USB-to-serial chip. Its ESP32-S3 speaks USB itself and shows up as a **USB serial/JTAG device**
(vendor id `303a`). The build flags `-DARDUINO_USB_CDC_ON_BOOT=1` (in `platformio.ini`; the PlatformIO board file adds
`-DARDUINO_USB_MODE=1`) make `Serial` go over that USB port, so there is **one cable for power, flashing and logs**.

| OS | Port looks like | What to do |
|---|---|---|
| Linux | `/dev/ttyACM0` | Add yourself to the serial group, log out and in: `sudo usermod -aG dialout $USER` (Arch: `uucp`). Install PlatformIO's udev rules (below). If the port appears and vanishes, ModemManager may be grabbing it: `sudo systemctl stop ModemManager` (*unverified*, a common cause on Ubuntu) |
| macOS | `/dev/cu.usbmodem*` | No driver needed. Use the `cu.` device, not `tty.` |
| Windows | `COM3` (or similar) | Windows 10/11 normally binds the built-in USB serial/JTAG driver by itself (*unverified*). If Device Manager shows an unknown device, install Espressif's USB-Serial/JTAG driver. Check the port number in Device Manager, "Ports" |

Linux udev rules (so you can flash without `sudo`; the file is PlatformIO's own, *unverified: not fetched here*):

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules && sudo udevadm control --reload-rules && sudo udevadm trigger
```

Find the port with the device plugged in:

```bash
pio device list
```

If PlatformIO cannot guess the port on upload (the board definition says an upload port is required), name it:
`pio run -e hwcheck -t upload --upload-port /dev/ttyACM0`, and `pio device monitor --port /dev/ttyACM0`. Use your own port
name in every command below if autodetection fails.

**Download mode.** Uploads normally reset the board into the bootloader by themselves over USB. If an upload says it cannot
connect or finds no board: **hold the reset button about 3 seconds until the green LED flashes**, then run the upload
again. The port number may change in download mode, so run `pio device list` again. After an upload the board restarts and
the port re-enumerates; `pio device monitor` then needs to be started again.

**Serial gotchas with USB serial.** Messages printed before your monitor connects are lost. To see boot messages, open the
monitor first, then tap the reset button once. The baud rate (115200) is ignored by USB serial and harmless.

### A sanity check with no hardware

Proves your toolchain and the code are fine before you blame the device. About 2 minutes after the first download.

```bash
pio test -e native
```

**Success:** a table ending in `71 test cases: 71 succeeded` (the count may grow). **If not:** the toolchain install is
broken; fix that before touching the device (re-run `pip install -U platformio`).

```bash
pio run -e hwcheck && pio run -e cores3
```

**Success:** both end with `SUCCESS` (hwcheck: RAM 15.9 %, flash 17.9 %; the game is larger). These are the two firmware images you will flash.

Optional, to know what the game *should* look like: the PC emulator (needs SDL2: `sudo apt-get install -y libsdl2-dev`, macOS `brew install sdl2`).

```bash
pio run -e emu && .pio/build/emu/program
```

A window opens (mouse = finger, arrow keys = tilt, Space = jump). Compare it later with
[assets/firmware-emulator-screens.png](assets/firmware-emulator-screens.png). Details: [firmware/emu/README.md](../firmware/emu/README.md).

---

## 1. Flash the hardware-check app

This is a measuring tool, not the game. It does not touch the saved-levels partition, so you can switch between it and the game freely.

### 1.1 Flash and open the monitor

```bash
pio run -e hwcheck -t upload
```

```bash
pio device monitor
```

(`pio device monitor` is also available as `pio run -e hwcheck -t upload -t monitor` in one go, *unverified*.) Leave the
monitor open for the whole of part 1: **every page prints its numbers there**, ready to copy into
[hardware-notes.md](hardware-notes.md). To see the first line (`Scribble Run hardware check. display 320 x 240. Pages: ...`),
tap reset once with the monitor open.

**Success:** the screen shows "Hardware check" with nine coloured squares; the monitor shows the banner line above.
**If not:** see the troubleshooting table in part 6 (no port, blank screen, boot loop).

**Navigation:** tap the **right half of the bottom bar** for the next page, the **left half** to go back. The pages are, in
order: INFO, TOUCH, SPEED, TILT, BALL, SOUND. Write numbers into the "Measured" column of the table in
[hardware-notes.md](hardware-notes.md) as you go (or into a scratch file and paste later).

### 1.2 INFO: palette and memory

Fills: *PSRAM size / free*, *Palette looks right*.

* Read the text under the squares: **PSRAM 8192 KB** expected (the INFO page does not print to serial; read it off the screen or photograph it), flash 16 MB, display 320 x 240. The game also prints `PSRAM ... KB, free ... KB` at boot.
* Look at the nine squares (paper, paper dot, ink, red, blue, green, yellow, white, grey). The paper must look **cream, not white**, the ink **near-black**; red and blue must not be swapped (square 4 red, square 5 blue). Photograph it.

**Decisions**

| You see | Meaning | Do |
|---|---|---|
| PSRAM 0 or missing | The board is running without PSRAM; the game will refuse to start ("No PSRAM found") | part 6, "out of PSRAM" |
| Red and blue swapped, or everything tinted | Colour order of the panel | note it, do not guess a fix; send the photo |
| Paper too yellow / too white / ink washed out | Palette values are guesses | change `PAPER`, `INK`, ... in `shared/constants.json`, run `node ../tools/gen-config.mjs`, rebuild (needs Node) |

### 1.3 TOUCH: his finger, the edges, the sample rate

Fills: *48 px hit rate (your son, 40 taps)*, *Dead zones near the edges*, *Touch sample rate*.

* The page shows a 6 x 3 grid of 48 px squares with one **green** target. Tap the green one; it jumps to another (a high tone for a hit, a low tone for a miss). The counter shows hits, misses, percentage. **Let him do 40 taps** and write down the percentage (target >= 95 %).
* The red dot is where the controller thinks the finger is. Drag a fingertip along all four screen edges and into the four corners: the dot must follow to the very edge. Note any strip where it stops or jumps (tap targets start at x = 16, so use the dot, not the squares, to test edges).
* The `Hz` figure counts touch samples per second **while a finger is held down**. Keep a finger dragging for two seconds and read it. This page's loop is slow (it also pushes a full frame), so a number below 60 here mostly measures the loop, not the touch chip; treat it as informational.

**Decisions**

| Result | Do |
|---|---|
| Hit rate < 95 % with his finger | Which squares did he miss (edges? near the bottom bar?). Report it; button sizes in [screens.md](screens.md) may grow beyond 48 px |
| Dead strip at an edge | Report its width; `JUMP_EDGE_MARGIN` (12 px, `shared/constants.json`) is the grip-safe margin for jumps, a dead strip wider than that needs a UI change |
| Dot is mirrored or offset | Report; the touch panel orientation is not meant to need a fix, so this would be a surprise |

### 1.4 SPEED: does the screen refresh fast enough?

Fills: *Full 320x240 push from PSRAM*, *40x40 push from SRAM*, *40x40 clip of the PSRAM sprite*.

* **Tap once** anywhere above the bottom bar. The screen flashes yellow for a moment, then shows three timings and the serial line `speed: full ... ms, 40x40 sram ... ms, 40x40 clip ... ms`. Tap again two or three times; the numbers should be steady.

**Decisions**

| Number | Expected | If different |
|---|---|---|
| Full-frame push | about 31 ms (about 32 fps) | **Over about 40 ms** (under 25 fps): the game needs the **partial-rectangle present**. Today `Renderer::present()` in `src/app/renderer.cpp` always pushes the whole 320 x 240 sprite. The change: remember the dirty rectangle each frame (the ball, the buttons, the toast) and present only that, with `M5.Display.setClipRect(x, y, w, h)` around `frame.pushSprite(0, 0)`, exactly what this SPEED page's third timing measures. Do not change anything yet; send the numbers |
| 40 x 40 from SRAM | 1.5 ms or less | Much slower: SPI clock is below 40 MHz, report it |
| 40 x 40 clip of PSRAM sprite | 1.5 ms or less | If this is about as slow as the full push, clipped presents do not help and the SRAM composite buffer idea ([system-design 6.2](system-design.md#62-rendering-a-double-buffered-sprite-in-psram)) is needed instead |

The game still plays correctly at about 30 fps: physics always advances in 1/60 s steps (up to 6 catch-up ticks per frame).
The fps matters for feel (ink latency while drawing), not correctness.

### 1.5 TILT: axis and sign, noise, full scale

Fills: *Tilt axis and sign*, *Accelerometer noise at rest*, *Angle at full scale*.

Hold the device the way he will: **landscape, screen towards you**, like a handheld console.

1. **Axis and sign.** The page shows `ax ay az` in g, the "rolling axis" (default **+x**) and a bar. **Tilt the right edge down**: the bar must grow to the right. If it grows left, **tap the right half of the screen (above the bottom bar) to flip the sign**. If tilting does nothing, **tap the left half** to cycle the axis (x, y, z) until the bar reacts to tilting the right edge down and not to tipping the top edge away from you. Write down the final pair (for example `-y`). The line "gravity mostly on x/y/z" tells you which axis carries gravity right now; held upright in landscape it should be the screen's vertical axis.
2. **Noise at rest.** Put the device flat on the table (or standing on its base). Watch the serial lines `imu: ax ... ay ... az ... tilt ...` (one every 0.5 s) for 30 s. The rolling-axis value should sit within about 0.02 g of zero and not drift. Write down the spread (max - min) and any constant offset.
3. **Full scale.** Tilt slowly until the bar turns **green** (full tilt, 1.00). Read the angle with a phone level app, or compute it from the serial value: angle = asin(g) (0.40 g is 23.6 degrees; the design says full scale at about 24 degrees).
4. **Feel.** Does it feel twitchy or numb? Pitching the device back (the natural way) must not change the left-right value much.

**Decisions**

| Result | Do |
|---|---|
| Axis or sign differs from `+x` | Edit `TILT_AXIS` and `TILT_SIGN` in `firmware/src/app/input.h` (lines `constexpr int TILT_AXIS = 0;` x=0 y=1 z=2, and `TILT_SIGN = 1;`). Then rebuild and flash the game (part 2). The hardware-check app's choice is **not saved**; the game constants are the only place it counts. The stand-up detection (`upright`) uses the same axis, so this must be right before the camera work |
| Rolling axis comes out as **z** | Unexpected (z is the screen normal). Report it rather than editing; the stand-up maths assumes the rolling axis is in the screen plane |
| Noise at rest above about 0.03 g, or an offset above 0.06 g | The ball will creep. Raise `TILT_DEADZONE` (now 0.06) in `shared/constants.json`, then `node ../tools/gen-config.mjs` and rebuild (CI also regenerates; commit both the JSON and the generated files) |
| Full scale much later than 24 degrees (feels numb) | Lower `TILT_FULL` (now 0.40 g); much earlier (twitchy): raise it. Same procedure. Tune with him in part 1.6, not by theory |

### 1.6 BALL: the real physics

Fills: *BALL page frame rate*, *Physics time per tick*.

* Tilt to roll, **tap anywhere (away from the 12 px screen edges) to jump**. The top line shows `fps` and `us` (microseconds of physics per tick); the serial prints `ball: ... fps (full-screen pushes), physics ... us/tick` every second. After a win flag the level restarts.
* Let him play for ten minutes. Note "too slow", "too slippery", "too hard to jump", and any **accidental jumps from the way he grips** it.

**Decisions**

| Result | Do |
|---|---|
| fps about 30 (the full push time, see 1.4) | Normal while the present is full-frame. The BALL page pushes the whole screen every pass, so this fps is bounded by the SPEED number |
| Physics under 1000 us/tick | Fine. Budget is 16,667 us per frame ([system-design 6.3](system-design.md#63-frame-time-budget-play-60-hz--1667-ms)). The physics uses `double` in software on the S3, so a number of 100-800 us would be plausible (*a guess*) |
| Physics over about 2000 us/tick | Report it; the frame budget gets tight with drawing on top |
| Ball runs the wrong way | Back to 1.5 |

### 1.7 SOUND: volume by ear

Fills: *Comfortable speaker volume*.

* Tap the **top green box** for the win jingle (C E G C). Tap the **lower box** to step the volume (32, 64, 128, 255; default 64). Find the loudest level that is still comfortable for him in a quiet room, and the one that is comfortable for a car or a living room.

**Decision:** the game's levels are `VOL_LOW = 64` and `VOL_MED = 140` in `src/app/audio.h`. If 64 is already too loud or too faint, report what you chose.
**If silent:** see part 6.

### 1.8 Write it down

Fill the table in [hardware-notes.md](hardware-notes.md) now, while the numbers are fresh. Do not tidy up the code yet.

---

## 2. Flash the game and smoke-test every screen

```bash
pio run -e cores3 -t upload
```

```bash
pio device monitor
```

(If you changed `TILT_AXIS` / `TILT_SIGN` in `input.h`, this build contains it.)

**First boot takes a few seconds with a dark or empty screen**: the first LittleFS mount formats the 10 MB partition, once.
In the monitor you should see:

```text
Scribble Run
PSRAM 8192 KB, free ... KB
storage root: /littlefs
```

**Success:** the **Title** screen with the wobbly "SCRIBBLE RUN!" logo, a rolling ball and a bottom bar of four buttons.
**If not:** a **red screen with white text** is a deliberate fatal error (`No PSRAM found...` or `Could not allocate the screen buffers`); the message names the cause. `LittleFS mount failed: levels will not be saved this session` is not fatal now but must be fixed (part 6).

### 2.1 The smoke-test script

This follows the emulator's end-to-end script, `emu/scripts/e2e_draw_play_win.txt` (Chooser, Finger, draw, Play, win). The
screens are almost all icons, no words. The bottom bar of the Title is: **Levels** (grid), **Paper** (yellow camera), **Play**
(green), **Draw** (pencil). Leave Paper for part 4.

| # | Do | Expect | Compare with the emulator |
|---|---|---|---|
| 1 | Look at the Title for 5 s | Logo wiggles, ball rolls and jumps along the hill to a flag, sound chip (speaker) top-right | first picture of the PNG |
| 2 | Tap the speaker chip a few times | Icon changes Off / Low / Medium; a beep at Low and Medium | |
| 3 | Tap **Levels** | Picker: ten cells, **slots 1-3 show the three lessons** with thumbnails, slots 4-10 empty | picker picture |
| 4 | Tap an **empty** cell (slot 4) | **New-level chooser**: two big cards, yellow Paper (camera) and white **Finger** (pencil) | chooser picture |
| 5 | Tap **Finger** | **Draw** screen opens on the new level, with the ghost-finger hint on a fresh level | draw picture |
| 6 | Drag a finger to draw a low wall across the floor (a short vertical stroke near the middle), tap the other materials, undo, eraser | ink appears **under the fingertip**, no gaps even when scribbling; toolbar buttons are hit reliably | |
| 7 | Tap **Play** (the right end of the bottom toolbar) | **Play**: ball drops onto the ground at the Start pin | |
| 8 | Tilt right; tap to jump the wall; reach the flag | rolls, jumps, flag wave, **Win** screen with the jingle and three buttons (Edit, Play again, New level) | win picture |
| 9 | Hold **Restart** or the corner chips (they need a 0.4 s hold) while playing | the hold ring fills, then it acts; a quick tap does nothing | |
| 10 | Menu chip (top-right in Play) | **Menu**: Resume, Edit, Levels, Restart, Sound, Home | menu picture |
| 11 | **Levels** | Picker again, slot 4 has your thumbnail; the last-played slot is highlighted; the beaten tick shows | picker picture |
| 12 | Power cycle (see 3.1 for how) and open Levels | slot 4 and its thumbnail survived | |

Press and hold the sound chip or step through Off/Low/Med if anything is too loud: the setting is saved.

**Compare** each screen against [assets/firmware-emulator-screens.png](assets/firmware-emulator-screens.png) and photograph the
device screens next to it (see part 7). What matters: colours, text clipped at the edges, buttons off-screen, and lag.

**If not:** touch lands in the wrong place (mirrored or offset) -> part 1.3. Ball rolls the wrong way -> 1.5. Drawing lags far behind the finger -> note the fps from 1.4, this is the full-frame present again. Anything crashes (the screen goes black and the monitor shows a `Guru Meditation Error` or `abort()` backtrace) -> copy the whole backtrace; part 6, "boot loop".

---

## 3. Power-cut test of saving (build-plan Step 5)

**Why:** a save must never corrupt a level. The code writes `slotN.tmp`, flushes it, then renames it over `slotN.scrb`, so an
interrupted save should leave the old file intact. This test checks that on real flash. Each level file is 24,184 bytes.

**When the game saves** (so you can aim at it): a few seconds (5 s) after the last edit while in Draw, and **immediately when
you leave Draw** (Play, Menu -> Levels / Home, back). A save is a short window (a few tens to a few hundred milliseconds; *an
estimate*), so most pulls will land outside it. Tapping **Play** right after a stroke is the way to start a save on cue.

### 3.1 What "pull the power" means on this device

The CoreS3 has a built-in battery (*unverified: about 500 mAh*), so **unplugging USB does not cut power**. Two usable cuts:

* **Reset button** (weaker, instant, repeatable): resets the processor mid-save. The flash chip keeps its power and finishes any
  erase or write it had started, so this tests the file-system logic (temp file, flush, rename) but not a true brown-out.
* **Hold the power button until it switches off** (about 6 s on the AXP2101 power chip, *unverified*): too slow to hit a save; use it for the
  "power cycle" in table row 12, not for pulses.

A true brown-out in the middle of a flash write needs the battery disconnected and USB unplugged (opening the case; not for day
one). The realistic real-world cut is the battery running flat, so also do this once at the end: **start a long Draw
session with a low battery and let it die**, then power up and check the levels.

### 3.2 The procedure (20 pulses)

Have the monitor open (`pio device monitor`) and a pen and paper for a tally.

1. From Title -> Levels, open or create a level in slot 4 (**Finger**) and draw something recognisable (a few strokes, a Start pin and a Flag).
2. Draw one more small stroke (this marks the level as edited).
3. Tap **Play** (this saves right now), and **within about 0.3 s press the reset button once**. Vary the delay between pulses: 0.0 s, 0.1 s, 0.2 s, 0.3 s, ... also try pressing reset 4.5 to 6 s after a stroke, without leaving Draw, to catch the idle autosave.
4. After the reboot, go Title -> Levels. Look at slot 4. Then tick the tally: **OK** if the level is there and correct (either the old or the new version), **BAD** if the cell is empty or has a **warning dot** (a corrupt file).
5. Repeat until you have **20 pulses**, changing the drawing a little between pulses so you can tell old from new.

**Success:** 20 of 20 OK. Both the old and the new version are fine; what must never happen is a warning dot, an empty slot
where there was a level, or a crash at boot. **If not:** note which pulse failed, which timing, and what you saw; do not
"fix" it, send the log and the flash image (3.3).

### 3.3 Inspecting the slot files (the LittleFS partition)

The game keeps `slot0.scrb` ... `slot9.scrb`, `settings.json` (and `slotN.tmp` while a save is in flight, `slotN.bad` for a
corrupt level kept aside) in the LittleFS partition `spiffs`: 0x9E0000 bytes at flash offset 0x610000
(`partitions_16mb.csv`).

**Not implemented:** there is **no serial `ls` or file dump** in the firmware. If you want one, the smallest useful addition is in
`setup()` of `src/main.cpp`, after `store.begin(root)`: walk `opendir("/littlefs")` and print every file name and size to
`Serial` (about 8 lines). A one-line `Serial.printf` around `store.save()` that prints the slot and the time would also let you
aim the reset button at the save. Ask for it if you want it.

Without any code change you can read the whole partition back, unpack it and list it. PlatformIO has installed both tools
(`~/.platformio/packages/tool-esptool/esptool` and `.../tool-mklittlefs/mklittlefs`; on Windows use the same folders under
`%USERPROFILE%\.platformio\packages` and `.exe`). Put the device in download mode first (hold reset about 3 s until the green
LED flashes); in download mode the app is not running, so a half-written `slotN.tmp` is still there to see (a normal boot removes it).

```bash
~/.platformio/packages/tool-esptool/esptool --chip esp32s3 --port /dev/ttyACM0 read-flash 0x610000 0x9E0000 /tmp/scribble-fs.bin
```

(It reads about 10 MB and takes a minute or two. esptool 5.x spells the subcommand `read-flash`; older ones `read_flash`.)

```bash
~/.platformio/packages/tool-mklittlefs/mklittlefs -l -b 4096 -p 256 -s 0x9E0000 /tmp/scribble-fs.bin
```

The listing shows each file with its size; `-l` replaced by `-u some_folder` unpacks the files into `some_folder` (a plain
`.scrb` is the format of [system-design section 4](system-design.md)). The block size 4096 and page size 256 are the
Arduino-ESP32 LittleFS values (*the listing command was exercised here on an image I built myself, not on a device image*).

What to look for: `slot0.scrb`-`slot2.scrb` (the lessons) and your slots, each **24,184 bytes**; `settings.json`; **no
`.bad` files** (a `.bad` file means the game once found a corrupt level and set it aside); a stray `.tmp` right after a pulse
is normal and is cleaned at the next boot.

**Putting files on the device (optional, for the countdown voice clips):** create `firmware/data/`, put `three.wav`, `two.wav`,
`one.wav`, `snap.wav` (16 kHz, 16-bit, mono) in it and run `pio run -e cores3 -t uploadfs`. **This overwrites the whole partition,
including every saved level**, and the build fails with "can't read source directory" if `data/` does not exist.

---

## 4. Camera bring-up

Everything about the camera is a **guess** (every value is a `// UNVERIFIED on hardware` constant in
`firmware/src/app/camera.cpp`; the checklist is the "Camera bring-up" table in [hardware-notes.md](hardware-notes.md),
rows 1-13). The order below goes from the cheapest, safest check to the most committing one. After each step, if it fails,
**stop and fix that before the next**: a wrong guess early makes every later result meaningless.

**Always:** keep the monitor open, keep USB connected (the camera drains the battery), and the device **standing on its stand**,
screen towards you, the camera looking at the sheet. Put the printed template on a clipboard or card leaned against books,
**40 cm away**, filling the on-screen target frame; measure the 40 cm with a ruler or a string.

Camera files may be changing while you read this. Names below are constants in `camera.cpp`; search for them rather than relying on line numbers.

### 4.1 The Check screen with the live preview only

Fills hardware-notes rows **1, 2, 3, 4, 5**.

**Do:** Title -> **Paper** (yellow camera). The **Stand-up** guide appears (three pictures; the bottom bar says "STAND ME UP" until the device is upright, then turns into a green tick). Stand it up and tap the tick. The **Check** screen shows the live camera picture.

Watch the serial monitor. A good start prints, in order:

```text
[paper buffers: VisionWork 770336 bytes, photo 614400 bytes, preview gray 76800 bytes, all in PSRAM]
[camera] on
[camera] sensor pid 0x9b (GC0308 = 0x9b)
```

(`[camera] on` is printed when the sensor is up; the `pid` line appears first, directly after the driver starts. Order may differ.)

**Success:** the pid line says `0x9b` (the GC0308 sensor) and the screen shows a **moving, recognisable picture** at ten frames a second or better (wave a hand). The four lamps at the bottom left are grey until the four corner marks of the template are found. The text overlay `?` (white with a dark outline) on the picture means "marks not found yet" and is normal now.

**If not:**

| Symptom | Likely cause (row) | What to try |
|---|---|---|
| Screen says **"No camera found"** and the monitor prints `[camera] init failed (0x20001)` (`NOT_DETECTED`) or `(0x...)` | 1, 3, 4: power rail, SCCB bus, reset line | The game retries every 3 s; read the full `init failed` lines. Row 3: set `SCCB_ON_INTERNAL_I2C = false` in `camera.cpp`. Row 4: the line `pulsing CAM_RST` shows the reset pulse was needed |
| `init failed (0x105)` or `(0x103)` | ESP error: not found / invalid state: shared I2C port (row 3) | same as above |
| `init failed` and `[camera] out of PSRAM for the live buffers` | PSRAM exhausted | part 6, "out of PSRAM" |
| `pid` line fine, then `[camera] no frames` after about a second | pins (row 2) or clock (row 5) wrong: no data reaches the S3 | check `PIN_*` against the schematic; try a lower `XCLK_HZ` (10 MHz) |
| Picture is **noise, stripes or green/magenta** | byte order (row 6) | flip `CAM_SWAP_BYTES` |
| Picture tears or rolls | clock (row 5) | lower `XCLK_HZ` |
| **Touch or the tilt stops working** once the camera starts | shared I2C bus (row 3) | `SCCB_ON_INTERNAL_I2C = false`, or tell me exactly what stops |
| Screen says "Waking the camera" for ever | camera task stuck | reset, read the monitor |

The Camera screen is the one place the game can fail without losing anything else: if the camera never works, **Draw with a
finger still works completely**.

### 4.2 Colour order: is a red crayon red?

Row **6** (byte order, done above) and row **7** (red/blue order). This one matters: with red and blue swapped, the **spike
and bouncy layers swap**.

**Do:** first in the preview, hold a **red** crayon, then a **blue** one, in front of the lens (at arm's length). The preview must show red, then blue.
**Success:** the same colours as your eyes. **If red looks blue:** flip `CAM_SWAP_RB` (`false` -> `true`) in `camera.cpp`, rebuild, reflash. The white paper must be white and the table brownish, not green or pink (if it is, that is row 6 again).

Later (4.7) confirm it in the full flow: red marks must come out as **spikes** (red hatching) and blue marks as **bouncy** (blue dots).

### 4.3 Mirror test: is the writing readable?

Row **8**. A mirrored page cannot be straightened, so this must be right.

**Do:** hold a page **with writing on it** (the template has a legend and footer text) in front of the camera. Read it on the screen.
**Success:** the text reads **correctly** (a page that appears rotated by 180 degrees is fine: the corner-mark ring decides which corner is top-left, so an upside-down device is not a problem).
**If the text is mirrored:** set `CAM_HMIRROR = true` (left-right mirror) in `camera.cpp`. If the picture is flipped up-down but not mirrored, that is a 180 degree turn combined with a mirror: set `CAM_HMIRROR` too and look again; do not set `CAM_VFLIP` alone unless you also see the text reading backwards (*unverified: do the test, then decide with the picture in your hand*).

### 4.4 Focus distance: the sharpness light

Fixed-focus lens: the GC0308 cannot focus; the design claims it is sharp from about 30 cm.

**Do:** with the template in the frame, find the marks (the green outline and four green rings appear around the corner marks, the `?` disappears). Then hold the **? chip** (top right, a help icon) for about **2 seconds**: a white panel with numbers appears: `dist`, `edge`, `paper`, `clip`, `contrast`, `ratio`. Hold it again to hide it. Move the sheet to **30, 35, 40, 45, 50 cm** and write down `dist` and `edge` each time.

**What the numbers mean (green limits are in `shared/constants.json`):**

| Reading | Green when | Row |
|---|---|---|
| `edge` (sharpness, px of blur on an edge) | 2.0 px or less (yellow up to 3.0) | build-plan Step 6 |
| `dist` | the dot rectangle is 226-291 px wide in the 320-wide preview, about 35-45 cm | focal length row |
| `paper` / `clip` / `contrast` | paper mean 140-245, clipped under 3 %, contrast at least 100 | rows 9, 10 |
| `ratio` x / y | between 0.84 and 1.19 | the **Level** light |

**Success:** `edge` is 2.0 or less at 35-45 cm; the sharpness lamp (second lamp) is green there; a fingerprint on the lens turns it yellow or red and wiping fixes it. **If `edge` is large even at 40 cm:** the sensor is out of focus (it may be set at the factory for a different distance): note the distance at which `edge` is smallest and send the table.

### 4.5 The template and the four lights

The four lamps (left to right): **distance** (ruler), **sharp** (focus), **light** (bulb), **level** (tilt). Grey = no marks, red, yellow, green. When all four have been green for half a second the **GO** button on the right turns green and pulses.

**Do:** with the template at 40 cm, check each lamp, then break it on purpose:

* Distance: move to 30 cm and 55 cm: the lamp goes yellow/red and the picture shows yellow arrows (closer / back).
* Light: dim the room, then shine the lamp so it glares on the paper: the lamp reacts (`paper` and `clip` change).
* Level: tilt the board by 10 and 20 degrees: the lamp and the cue arc react.
* The 10 cm line on the printed sheet really is 10.0 cm (step 0).

**Calibration of the distance:** `dist` should be within **5 %** of the ruler. If not, the focal length is off: compute `f = w x d / 270 mm` from the dot-rectangle width `w` (QVGA px) and `d` (the distance in mm), and send it; the constant is `CAM_FOCAL_PX_320` (expected 377).

**Exposure.** Row 10: compare `paper` in the numbers panel: white paper should read about **220**. Much lower (grey paper): raise `AE_LEVEL` in `shared/constants.json` (now 1) and run `node ../tools/gen-config.mjs`. If it does not change, the driver has no `set_ae_level` and white paper may read dark (the call is skipped silently).

**Check fps and heap.** There is no fps counter on the Check screen. Judge by eye (smooth = at least about 10 fps). If it stutters, report it; the live-check rate is `LIVE_CHECK_MS = 125` in `game_paper.cpp`.

### 4.6 The full photo flow

**Do:** draw on the template with a **fat black marker** (some ground), a **red** mark and a **blue** mark, and the Start and Flag stays printed. Stand the device, put the page at 40 cm and wait for all four lamps green and the green **GO** pulsing. Tap **GO** and step back. The screen counts **3, 2, 1**, then a flash (the shutter) and the **Processing** sparkle, then **Review**.

**Success:** Review shows your drawing straightened, black as ground, **red as spikes**, **blue as bouncy**, the Start and Flag pins; tap the green tick: the level opens in Play. Watch the monitor:

```text
[paper vision N ms ...: status 0 marks found ink ... blobs ...]
[paper accept slot N]
```

The lights and the countdown work with the camera locked: row 9 (exposure lock): the snap should look as bright as the preview just before it. Row 11, 12, 13 are covered by parts 5 and 1.5.

| Visible failure | Cause | What to flip or check |
|---|---|---|
| Red marks become **bouncy** (and blue become spikes) | row 7 | `CAM_SWAP_RB` |
| Whole Review image mirrored / the drawing's text backwards | row 8 | `CAM_HMIRROR` |
| Review image darker than the preview, or the snap looks grey | row 9 or 10 | lock not applied (the driver ignores it) or `AE_LEVEL`; write the GC0308 registers with `set_reg` (see the table) |
| **"The camera missed the picture"** toast after the countdown | grab failed | read the monitor for `[camera] no frames`; retry |
| Retake pulses yellow, **status not 0** | marks not found / page not straight | move closer, retake; send the snapshot (part 7) |
| The level starts with the ball inside ground | pins landed on ink | Start / Flag are draggable on Review; note how often |

### 4.7 Entering and leaving 50 times

Build-plan Step 6: enter and leave the Check screen 50 times (Title -> Paper -> back), watching for a crash and watching the free PSRAM in the monitor (`PSRAM ... free` is printed only at boot, so reboot and compare). Touch must keep working with the camera on. Also note the **battery drop in 5 minutes** on the Check screen.

---

## 5. Timing the vision on the real S3 (build-plan Step 7)

**The number is printed.** After every capture the monitor shows:

```text
[paper vision N ms (this computer, not the device): status S marks found ink 0.0xxx blobs B]
```

The wording "this computer, not the device" is leftover from the emulator: on the device `N` is measured with the chip's own
timer (`esp_timer_get_time()` around `visionProcess`) and **is the number you want**. It covers the whole pipeline **including the automatic silent retry**
(a poor first pass is repeated once, so a 2x number is normal for a dim photo). `Review -> thinner / thicker ink` prints a second
number only in the emulator build (`[paper rethreshold ...]`); on the device it is not printed. The stage-by-stage times and the
live-check time are **not printed** anywhere on the device.

For a PC reference, the whole pass takes about **9 ms** on a desktop CPU (`pio test -e native -f test_vision -vv`); the S3 at
240 MHz with software doubles is much slower, so expect **hundreds of milliseconds to a few seconds** (*a guess*).

**Do:** capture 5 pages (clean, dim, tilted, messy, a blank page) and write down the `N` of each.

**Target:** the plan hopes for **GO to Review in about 4 s** (the 3.2 s countdown plus at most 0.8 s processing). **A few seconds
is acceptable**: the Processing screen animates, and the screen always shows for at least 0.6 s. Under 3 s: nothing to do.

**If it is over 10 s:**

1. Check you built the right thing: `pio run -e cores3` (release); a debug build is far slower.
2. Look for an optimisation flag: add `-O2` to `build_flags` of `[env:cores3]` in `platformio.ini` (PlatformIO defaults the ESP32 builds to size optimisation; *unverified that it takes effect*), rebuild and compare.
3. The vision task runs on core 0 at priority 1 while the camera task (priority 2) is still streaming for the first two seconds after the snap, which may steal time. Report `N` for the first capture after a Check and for one after waiting; a big difference points here (*unverified*).
4. If the device **reboots during Processing** with `Task watchdog got triggered` in the monitor, the long computation starves the idle task of core 0: tell me (the fix is a yield inside the vision loops or a bigger watchdog timeout; do not guess at it on the day).
5. Add stage timings (one `Serial.printf` per stage in `lib/core/src/vision.cpp`) and send them. The PC test already prints them for the desktop: `find marks`, `homography + warp`, `visionProcess`, `re-threshold`, `live check`.

---

## 6. If something goes wrong

| Symptom | Likely cause | What to try |
|---|---|---|
| **No serial port** appears | Charge-only cable; board off; no permission; driver | Swap the cable (data cable!). Press the power button once (a 3 s press on the side may be needed to wake it). `pio device list`. Linux: serial group + udev rules (part 0), `dmesg -w` while plugging in. Windows: Device Manager. Try another USB port (not a hub). Enter download mode (hold reset ~3 s, green LED) |
| Upload says "Failed to connect" / "No serial data" | Not in download mode | Hold reset ~3 s until the green LED flashes, `pio device list` for the new port, upload with `--upload-port` |
| **Upload succeeded but nothing happens** | Wrong partition table / old firmware / monitor opened too late | Press reset with the monitor open; `pio run -e cores3 -t erase`, then upload again |
| **Boot loop** (screen flickers, monitor repeats `rst:` or a backtrace) | Crash at start (PSRAM config, a bad pointer) | Copy the first backtrace into your report. Try the hardware-check app (`-e hwcheck`): if that runs, the fault is in the game's start-up; erase flash (below) and re-flash. Do not keep power-cycling a loop |
| **Blank (black or white) screen** | Backlight on but nothing drawn / first mount formatting | Wait 10 s on the first game boot. Reflash the hardware-check app: if INFO shows, the display is fine and the game is stuck; read the monitor |
| **Red screen with white text** | `fatal()` in the game: no PSRAM, or the screen buffers could not be allocated | The text names it. `No PSRAM found`: check `-DBOARD_HAS_PSRAM` in `platformio.ini` and that PSRAM is present in INFO (*the board file may need the right PSRAM memory type; unverified*) |
| **Touch dead** | Touch controller not started, wrong orientation | Hardware-check TOUCH page: does the red dot move? Reset. If only part of the screen works, note the dead zone |
| **Sound silent** | Volume at Off or too low; the amplifier not enabled | Hardware-check SOUND page, volume 255. In the game, the speaker chip shows Off or Low; step it to Medium. If SOUND is silent on the check app, report it (the M5Unified speaker init is the suspect) |
| **Tilt inverted** or **ball runs the wrong way** | `TILT_SIGN` | Part 1.5: flip `TILT_SIGN` (+1/-1) in `src/app/input.h`, rebuild, reflash |
| **Tilt does nothing** | wrong axis | TILT page; set `TILT_AXIS` |
| **"No camera found"** / `[camera] init failed (0x...)` | Camera power, SCCB bus, pins | Part 4.1 table. The rest of the game is unaffected |
| **"Not enough memory for the camera"** toast, or `[paper] not enough PSRAM for the vision buffers` / `[camera] out of PSRAM` | Out of PSRAM (the camera needs about 1.2 MB, vision 0.8 MB, the game about 0.3 MB) | The boot line `PSRAM ... free ... KB` says how much is left; on a CoreS3 8 MB should be plenty. Report the number. If it is far below 8 MB, PSRAM is not configured right |
| **The app freezes on Processing** | The vision task never finishes or starves a watchdog | Monitor: `Task watchdog got triggered` -> part 5, item 4. Otherwise wait 30 s (a slow first pass); reset; it is not data-destroying (nothing is saved until you tap the tick on Review) |
| `LittleFS mount failed: levels will not be saved this session` | File system not mountable (wrong partition label, damaged flash) | The code mounts with `LittleFS.begin(true, "/littlefs")`, which formats on failure, so this means even formatting failed. **Erase flash** (below) and reflash. If it persists, the partition table is not the one in `partitions_16mb.csv`: check that the build used `board_build.partitions` |
| Levels vanish or show a warning dot | Corrupt level | Corrupt levels are never deleted: the file is set aside as `slotN.bad`; read the partition back (part 3.3) and send it |

**Erase everything and start over** (this wipes the app, the saved levels and settings; you have to upload again afterwards):

```bash
pio run -e cores3 -t erase
```

```bash
pio run -e cores3 -t upload
```

---

## 7. What to send back

Put the numbers where the project keeps them, so the next session can act on them without asking you.

**Numbers (commit them):**

* Fill every row of the table in [hardware-notes.md](hardware-notes.md) (PSRAM, palette, the three display timings, touch rate, hit rate, dead zones, tilt axis and sign, noise, full-scale angle, BALL fps, physics us, volume), and the "Notes" list below it.
* Fill the "Camera bring-up" table: for each of the 13 rows write the result, and mark the row `VERIFIED <date>` (or `FAILED, flipped <CONSTANT>`).
* A small table of the distance runs (30, 35, 40, 45, 50 cm): `dist`, `edge`, `paper` and the lamp colours, in lights: daylight, lamp, dim.
* The `[paper vision N ms ...]` number of each capture, and the power-cut tally (20 pulses, OK/BAD).
* If you changed a constant (`TILT_AXIS`, `TILT_SIGN`, `CAM_SWAP_*`, `CAM_HMIRROR`), commit that change as its own commit with the reason in the message.

**Photos and captures:**

* A phone photo of each game screen next to the emulator picture, and of the hardware-check pages (INFO palette, TOUCH, TILT, BALL).
* The **full serial log** of the session as a text file (`pio device monitor > day-one.log` would capture it, but the monitor wants a terminal: use `pio device monitor | tee day-one.log`; or copy-paste).
* Photos of **every paper test drawing** (phone photos, landscape, about 40 cm, three lights, some tilted 10-20 degrees) go into **`shared/golden/photos/`** (the folder does not exist yet; create it) with a name that says the light and the tilt, e.g. `lamp-tilt15-03.jpg`. These tune the vision in the simulator first (build-plan "Before the device arrives").
* A phone photo of the **device screen at Review** for each capture, next to the paper.
* The flash image of the file system (3.3) if any power pulse failed.

**Not available today (ask if you want them):** there is **no way to save the raw 640 x 480 snapshot from the device**. The emulator
can already read a raw RGB565 dump as a test photo (`EMU_PHOTO`, see [firmware/emu/README.md](../firmware/emu/README.md)),
so the most valuable thing to add after day one is a small function that writes the snapshot buffer (`paper.snap`, 614,400 bytes)
to `/littlefs/snap.raw`, which you could then read back with the 3.3 procedure and feed straight into the vision tests (*unverified idea*).
There is also no fps counter on the Check screen and no per-stage vision timing.
