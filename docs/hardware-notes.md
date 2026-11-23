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
