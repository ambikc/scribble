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

Written without a device. Every guess was then checked against the vendor code (status as of 2026-11-24):
**on disk**: M5GFX 0.3.0 / M5Unified 0.3.0 in `firmware/.pio/libdeps/cores3`, and the esp32-camera build inside
`framework-arduinoespressif32-esp32s3-libs` 3.3.12 (its sdkconfig and symbols); **fetched** from raw.githubusercontent.com: the official
M5CoreS3 library (`src/utility/GC0308.cpp`), Espressif's esp-bsp `bsp/m5stack_core_s3/include/bsp/m5stack_core_s3.h`, esp32-camera
master (`driver/esp_camera.c`, `driver/sccb-ng.c`, `driver/cam_hal.c`, `target/esp32s3/ll_cam.c`, `sensors/gc0308.c`), the Linux
`drivers/media/i2c/gc0308.c` and Allwinner's `linux-3.4-sunxi .../sunxi-vfe/device/gc0308.c`. docs.m5stack.com and the GC0308 datasheet
mirrors were blocked by this session's network policy, so the schematic itself was not read.
**Nothing here has run on a CoreS3 yet.** Status: CONFIRMED = matches the vendor sources; CORRECTED = the first guess was wrong and was
fixed from them; UNVERIFIED = no source settles it. The first power-on still checks every row (the "How to check" column).

Build fact that changed the analysis: `espressif32@8.0.0` ships **Arduino-ESP32 3.3.x on ESP-IDF 5.5**, not 2.0.x. Wire and
esp32-camera's SCCB both use the new `i2c_master` driver (sdkconfig `CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW`).

How it is wired in the firmware: `Camera` (`app/camera.h`) starts a task on core 0 when a paper screen asks for it (`begin()` is a lease, renewed
every tick; two seconds without a renewal switch the camera off: `esp_camera_deinit()` frees the driver's two VGA buffers).
The task keeps the newest 640 x 480 RGB565 frame in a double buffer (PSRAM, 2 x 614 KB, plus a 77 KB gray copy for the live check) and
averages three frames at the snap. The game loop never waits for it, except while the task talks to the sensor (row 3).

| # | Value (where) | Status | What the code does now | Source | How to check it on the device | If it is wrong |
|---|---|---|---|---|---|---|
| 1 | Camera power: AXP2101 ALDO3 = 3.3 V | CONFIRMED | `M5.begin()` writes AXP2101 0x90 = 0xBF (ALDO3 on) and 0x94 = 28 (3.3 V) | M5Unified `utility/Power_Class.inl` ("ALDO3 set to 3.3v // for camera"); M5GFX `board_detect/m5/pmic_ops.hpp` | `[camera] sensor pid 0x9b` on the serial monitor after the first Check | no pid line: read the init error code printed above it |
| 2 | Pins: PCLK 45, VSYNC 46, HREF 38, D0..D7 = 39, 40, 41, 42, 15, 16, 48, 47 (`PIN_*`) | CONFIRMED | unchanged | M5CoreS3 `GC0308.cpp` and esp-bsp `BSP_CAMERA_*` agree pin for pin; M5GFX's CoreS3 probe pins (38-42, 46-48) are a subset | init succeeds and the preview is a picture, not noise | check the schematic |
| 2a | XCLK (`PIN_XCLK`) | CORRECTED: was GPIO 2, now -1 | no clock output; the module has its own oscillator | M5CoreS3 `GC0308.cpp` `.pin_xclk = -1`; esp-bsp `BSP_CAMERA_GPIO_XCLK GPIO_NUM_NC`. GPIO 2 is Port A SDA (M5Unified pin table), so the old value would have put 20 MHz on Port A | as row 2 | — |
| 3 | SCCB on the internal I2C bus: `pin_sccb_sda = -1`, `sccb_i2c_port = M5.In_I2C.getPort()` (= I2C_NUM_1, SDA 12, SCL 11) | CONFIRMED, plus a CORRECTED locking rule | M5GFX opens port 1 through `Wire1.begin()`, which creates an IDF `i2c_master` bus; esp32-camera's `SCCB_Use_Port` takes that bus (`i2c_master_get_bus_handle`) and never deletes it (`sccb_owns_i2c_port = false`). But M5GFX drives the controller registers directly with no lock shared with the IDF driver, and says the two "must not run concurrently". So the camera task asks, and `Camera::update()` parks the game loop (no `M5.update()`, touch, IMU) during `esp_camera_init` and the exposure lock (≤ 3 s, normally a few hundred ms) | M5GFX `lgfx/v1/platforms/esp32/common.inl` (i2c `init`, the "shared rather than taken over" comment, `beginTransaction`); M5Unified `M5Unified.inl` (`in_port = I2C_NUM_1`); esp32-camera `sccb-ng.c`, `esp_camera.c`; firmware.map (M5GFX links `TwoWire::setPins`, `esp32-hal-i2c-ng`) | `[camera] start ok in N ms`; touch and IMU keep working while the camera runs; no `failed to get SCCB I2C Bus handle` log | `the game loop did not park` in the log: check that `camera.update()` runs every loop pass |
| 4 | Camera enable = AW9523 P1_0 (`AW9523_OUT_P1`, `CAM_EN_BIT`); no PWDN or RESET pin | CONFIRMED (it was called CAM_RST: it is an enable) | M5GFX's CoreS3 power-on makes P1_0 an output (reg 0x05 = 0x0C) and drives it high (reg 0x03 \|= 0x03) during `M5.begin()`, then reads the camera id. We touch it only if the first init fails (low 20 ms, high, 50 ms) | esp-bsp `BSP_CAMERA_EN = IO_EXPANDER_PIN_NUM_8`, `BSP_CAMERA_RST NC`; M5GFX `pmic_ops.hpp`, `esp32s3_cores3.inl`; M5CoreS3 `pin_pwdn/pin_reset = -1` | the retry line `cycling CAM_EN` never appears | — |
| 5 | XCLK_HZ 20 MHz; LEDC timer 0 / channel 0 | CONFIRMED | with no XCLK pin, 20 MHz only sets the LCD_CAM clock divider; LEDC is never used on the S3 | M5CoreS3 and esp-bsp (`BSP_CAMERA_XCLK_CLOCK_MHZ 20`); esp32-camera `esp_camera.c` (`CAMERA_ENABLE_OUT_CLOCK` empty on the S3), `ll_cam.c` | frame rate about 15 fps at VGA | — |
| 6 | Byte order (`CAM_SWAP_BYTES = true`) | CONFIRMED | swap each pixel's two bytes | Linux `gc0308.c`: output format 6 = `MEDIA_BUS_FMT_RGB565_2X8_BE` (high byte first); esp32-camera stores bytes as received (`cam_hal.c` `swap_data = 0`) | the preview shows the paper white and the table brown, not green/magenta noise | flip the constant |
| 7 | Red / blue order (`CAM_SWAP_RB = false`) | CONFIRMED by source | R in the high 5 bits | same Linux table (RGB565, not BGR565); gc0308.c writes the same format code 6 | hold a **red** crayon in front of the lens: the preview must show red (blue would swap the spike and bouncy layers) | flip the constant |
| 8 | Orientation (`CAM_HMIRROR`, `CAM_VFLIP` = false) | UNVERIFIED (sources agree) | written explicitly to reg 0x14 bits 0 / 1 | esp-bsp `BSP_CAMERA_HFLIP 0`, `VFLIP 0`; gc0308.c default 0x14 = 0x10 | hold the printed template: the legend text must read correctly (mirrored text = wrong); upside down is fine | set `CAM_HMIRROR` (a mirrored page cannot be straightened) |
| 9 | Exposure lock at the countdown | CORRECTED | AWB off with `set_whitebal` (reg 0x22 bit 1) and AEC off with `set_reg(0xd2, 0x80, 0)`. `set_exposure_ctrl` / `set_gain_ctrl` are no longer called: in this esp32-camera they flip bits 0 / 2 of 0x22, which no other GC0308 driver uses for that; the AEC loop sets the gain too | Linux `gc0308.c` (AWB = 0x22 bit 1); Allwinner `gc0308.c` (`sensor_s_autoexp`: 0xd2 bit 7; `sensor_s_autowb`: 0x22 bit 1; autogain unsupported) | brightness does not change between the last Check frame and the snap; unlock restores the live picture | — |
| 10 | `AE_LEVEL` = +1 via `set_ae_level` | CONFIRMED (implemented) | AEC target 0xd3 = 0x48 + 8 x level = 0x50 (a `static_assert` keeps the level in -2..2) | esp32-camera `gc0308.c` `set_ae_level`; Allwinner EV+1 table (0xd3 = 0x50) | compare `paper` in the numbers overlay (hold the ? chip 2 s): about 220 expected | tune `AE_LEVEL` in `shared/constants.json` |
| 11 | Frame time: the camera task copies a frame in about 10 ms and the live check takes N ms | UNVERIFIED | priority 2 on core 0, next to the driver's own `cam_task` (`CONFIG_CAMERA_CORE0`) and the vision task | — | `[paper vision N ms]` on the device and the FPS of the game loop in Check | lower the live-check rate (`LIVE_CHECK_MS`) or the preview frame rate |
| 12 | "Stand me up" gate: `upright` from the IMU (`TILT_AXIS`, `uprightFromAccel`) | UNVERIFIED | the Standup screen waits for `upright`, but lets Ready work after 8 s anyway | — | stand the device on its base: the grey "STAND ME UP" bar turns into the green tick | fix the axis in `input.h` |
| 13 | Idle power: camera off outside the paper flow; ALDO3 **not** switched off | UNVERIFIED | `esp_camera_deinit()` stops the capture; the sensor keeps its own clock and idles | — | battery current in the Title screen with the camera never used vs. after a paper session | switch ALDO3 (or CAM_EN) off when the camera ends, on again before `esp_camera_init` |
| 14 | "Camera not found" | CORRECTED (timing) | `esp_camera_init` fails (0x106 = nothing at 0x21) → one CAM_EN cycle and retry → `Failed` ("No camera found" panel), retried every 3 s while leased. A stream that stops: one `esp_camera_fb_get()` timeout (4 s, `FB_GET_TIMEOUT` in `esp_camera.c`) now means `Failed`; before it took 20 of them (80 s) | esp32-camera `esp_camera.c` | on a normal run the panel never appears and the first frame comes within a second; the emulator covers the failure path (`EMU_CAMERA_FAIL=1`) | — |

Frame size and format: the GC0308 driver supports RGB565, YUV422 and GRAYSCALE (no JPEG) and up to VGA (`set_framesize` clamps above it;
VGA is its 1:1 subsample mode). Measured values go in this table once known (mark the row `VERIFIED <date>`, and delete the constant's tag).
