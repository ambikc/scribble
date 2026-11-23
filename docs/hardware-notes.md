# Hardware notes (fill in on the device)

Numbers measured with the hardware-check app (`firmware/hwcheck/main.cpp`, see [firmware/README.md](../firmware/README.md)).
They decide a few design choices; each row links to the one it affects. Copy values from the serial monitor.

| Measurement | Expected | Measured | Affects |
|---|---|---|---|
| PSRAM size / free | 8 MB | | [system-design §8](system-design.md#8-memory-budget-cores3) memory table |
| Palette looks right (cream paper, near-black ink) | yes | | constants.json palette |
| Full 320×240 push from PSRAM | ≈ 31 ms | | [§6.2](system-design.md#62-rendering-a-double-buffered-sprite-in-psram) rendering |
| 40×40 push from SRAM | ≤ 1.5 ms | | §6.2 dirty rectangles |
| 40×40 clip of the PSRAM sprite | ≤ 1.5 ms | | §6.2 |
| Touch sample rate | ≥ 60 Hz | | input |
| 48 px hit rate (your son, 40 taps) | ≥ 95 % | | screens.md button sizes |
| Dead zones near the edges | none | | JUMP_EDGE_MARGIN |
| Tilt axis and sign (right edge down = +1) | ? | | input mapping |
| Accelerometer noise at rest | < 0.02 g | | TILT_DEADZONE |
| Angle at full scale | ≈ 24° | | TILT_FULL |
| BALL page frame rate | 60 fps | | Step 3 |
| Physics time per tick (double) | < 1 ms | | [§6.3](system-design.md#63-frame-time-budget-play-60-hz--1667-ms) frame budget |
| Comfortable speaker volume | ~64 / 255 | | audio |

## Notes

-

## Camera bring-up

Written without a device: everything below was derived from the M5GFX / M5Unified sources (which probe the camera at I2C address 0x21,
chip id 0x9B, on the internal I2C bus and switch its supply on) and the Espressif esp32-camera API that ships with the Arduino-ESP32 core.
**Nothing here has run on a CoreS3.** Every value is a `// UNVERIFIED on hardware` constant in `firmware/src/app/camera.cpp`.
The code builds and links (`pio run -e cores3`); step 6 of the build plan is where the rows below are filled in.

How it is wired in the firmware: `Camera` (`app/camera.h`) starts a task on core 0 when a paper screen asks for it (`begin()` is a lease, renewed
every tick; two seconds without a renewal switch the camera off: `esp_camera_deinit()` stops XCLK and frees the driver's two VGA buffers).
The task keeps the newest 640 x 480 RGB565 frame in a double buffer (PSRAM, 2 x 614 KB, plus a 77 KB gray copy for the live check) and
averages three frames at the snap. The game loop never waits for it.

| # | UNVERIFIED value (where) | What I assumed | How to check it on the device | If it is wrong |
|---|---|---|---|---|
| 1 | Camera power: AXP2101 ALDO3 = 3.3 V | `M5.begin()` already enables it (M5Unified Power_Class, "ALDO3 ... for camera") | `[camera] sensor pid 0x9b` on the serial monitor after the first Check | no pid line: init failed, read the error code printed above it |
| 2 | Pins: XCLK 2, PCLK 45, VSYNC 46, HREF 38, D0..D7 = 39, 40, 41, 42, 15, 16, 48, 47 (`PIN_*`) | the M5Stack CoreS3 camera example values; M5GFX's own probe list of 8 camera data/sync pins (38 39 40 41 42 46 47 48) agrees with 7 of them | init succeeds and the preview is a picture, not noise | swap D-pins / check the schematic |
| 3 | SCCB (sensor control) on the internal I2C bus: `pin_sccb_sda = -1`, `sccb_i2c_port = M5.In_I2C.getPort()`, `SCCB_ON_INTERNAL_I2C` | M5Unified's I2C driver and esp32-camera use the same (legacy) driver, so the port can be shared | `esp_camera_init` returns ESP_OK; touch and IMU keep working while the camera runs | `ESP_ERR_INVALID_STATE` / `NOT_DETECTED`: set `SCCB_ON_INTERNAL_I2C = false` (then it opens its own port on 11/12, which may disturb M5Unified's bus) or stop M5Unified's In_I2C around the init |
| 4 | CAM_RST = AW9523 P1_0 (`AW9523_OUT_P1`, `CAM_RST_BIT`) | `M5.begin()` leaves it released; only pulsed if the first init fails | the retry line `pulsing CAM_RST` never appears | check the AW9523 pin map in the CoreS3 schematic |
| 5 | XCLK 20 MHz, LEDC timer 0 / channel 0 (`XCLK_HZ`, `ledc_*`) | GC0308 accepts 10-27 MHz; nothing else in this firmware uses LEDC timer 0 | frame rate (about 15-20 fps expected at VGA) | lower XCLK if the picture tears; pick another LEDC timer if something else needs it |
| 6 | Byte order of RGB565 frames (`CAM_SWAP_BYTES = true`) | esp32-camera delivers big-endian byte pairs | the preview shows the paper white and the table brown, not green/magenta noise | flip the constant |
| 7 | Red / blue order (`CAM_SWAP_RB = false`) | R is the high 5 bits | hold a **red** crayon in front of the lens: the preview must show red. If it is blue, the spike and bouncy layers would be swapped | flip the constant |
| 8 | Orientation (`CAM_HMIRROR`, `CAM_VFLIP` = false) | the page is NOT mirrored; rotated by 180 degrees is fine (the ring finds the top-left) | hold the printed template: the legend text must read correctly (mirrored text = wrong) | set `CAM_HMIRROR` (a mirrored page cannot be straightened) |
| 9 | Exposure lock at the countdown: `set_exposure_ctrl / set_whitebal / set_gain_ctrl` | the GC0308 driver implements all three (null-checked, so a missing one is silently skipped) | brightness does not change between the last Check frame and the snap | write the GC0308 registers directly with `set_reg` |
| 10 | `AE_LEVEL` = +1 via `set_ae_level` | helps white paper not to come out grey | compare `paper` in the numbers overlay (hold the ? chip 2 s): about 220 expected | tune `AE_LEVEL` in `shared/constants.json`; if the driver has no `set_ae_level`, the call is skipped and white paper may read dark |
| 11 | Frame time: the camera task copies a frame in about 10 ms and the live check takes N ms | the 2 + 1 core split keeps the UI at 60 fps | `[paper vision N ms]` for the whole pipeline on the device (the PC number is meaningless) and the FPS of the game loop in Check | lower the live-check rate (`LIVE_CHECK_MS`) or the preview frame rate |
| 12 | "Stand me up" gate: `upright` from the IMU (axis choice `TILT_AXIS`, `uprightFromAccel`) | the Standup screen waits for `upright`, but lets Ready work after 8 s anyway | stand the device on its base: the grey "STAND ME UP" bar turns into the green tick | fix the axis in `input.h`; the 8 s fallback means it is never a dead end |
| 13 | Idle power: the camera is off outside the paper flow; the AXP2101 ALDO3 rail is **not** switched off | `esp_camera_deinit()` stops XCLK, the sensor idles | battery current in the Title screen with the camera never used vs. after a paper session | switch ALDO3 off with `M5.Power.Axp2101` when the camera ends (and on again before `esp_camera_init`) |

Measured values go in this table once known (mark the row `VERIFIED <date>`, and delete the constant's `UNVERIFIED` comment).
