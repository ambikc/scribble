// M5Unified.h - a tiny stand-in for M5Unified, used ONLY by the PC emulator (env:emu).   MIT licensed.
//
// The real firmware includes <M5Unified.h>. In `pio run -e emu` this file is found first, so the SAME source files
// (src/*.cpp) compile for the PC. M5GFX itself has an SDL backend, so the display is a real M5GFX display: every
// drawing call, sprite and font behaves like the device. What is emulated here:
//
//   M5.Display   M5GFX over SDL, 320 x 240 (a window; under xvfb or SDL_VIDEODRIVER=dummy it is headless)
//   M5.Touch     the mouse (left button = one finger)           | script: tap/down/move/up; a SECOND finger: down2/move2/up2
//                (getCount() counts both, and getDetail(0) follows the first raw slot like the real library: when finger 1
//                lifts while finger 2 stays, getDetail(0) becomes finger 2, with its own id)
//   M5.Imu       arrow keys LEFT/RIGHT (or A/D) = tilt right/left edge down (+/- TILT_FULL g) | script: tilt <g>
//                reports "flat on the table" (az = +0.97 g); key U or script `upright 1` = "standing on its base" (ay = -0.98 g)
//   M5.Speaker   tone() is printed as "[tone <Hz> <ms>]", playWav() as "[wav <bytes> bytes]" (no sound)
//   keys         Space / W = jump, Escape = back (emu::keyHeld) | script: key jump|back
//   Serial, ESP, millis(), micros(), delay()
//
// With a script (env EMU_SCRIPT=file) time is VIRTUAL: every M5.update() advances 16.667 ms and delay() does nothing,
// so a run is deterministic and fast. See emu/README.md for the script language.
//
// Not emulated: the camera, the battery chip, microSD, the RTC, real DMA timing and real display speed. Timing numbers
// from the emulator mean nothing; measure those on the device.
#pragma once
#include <M5GFX.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstring>

// ---- Arduino-ish clock (virtual in script mode, real otherwise) -----------------------------------------------
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);

// ---- Serial ------------------------------------------------------------------------------------------------------
struct EmuSerial {
  void begin(unsigned long) {}
  void println(const char* s) { std::printf("%s\n", s); std::fflush(stdout); }
  void print(const char* s) { std::printf("%s", s); }
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap; va_start(ap, fmt); const int n = std::vprintf(fmt, ap); va_end(ap); std::fflush(stdout); return n;
  }
};
extern EmuSerial Serial;

// ---- ESP --------------------------------------------------------------------------------------------------------
struct EmuEsp {
  size_t getPsramSize() { return 8u * 1024 * 1024; }
  size_t getFreePsram() { return 7u * 1024 * 1024; }
  size_t getFreeHeap() { return 300u * 1024; }
  size_t getFlashChipSize() { return 16u * 1024 * 1024; }
};
extern EmuEsp ESP;

namespace m5 {
struct touch_detail_t {
  int x = 0, y = 0;
  uint16_t id = 0;                               // like m5gfx::touch_point_t::id: which finger (the controller's touch id)
  bool down = false, pressed = false, released = false;
  bool isPressed() const { return down; }
  bool wasPressed() const { return pressed; }
  bool wasReleased() const { return released; }
  bool isReleased() const { return !down; }
};

// Like M5Unified's Touch_Class: one detail per finger id; getDetail(i) is the detail of the finger in RAW slot i (raw slots hold the
// fingers that are down, in id order; slots past the count keep their old ids, as the real buffer does); getCount() counts the fingers
// that are down plus the ones released in this update.
struct EmuTouch {
  static constexpr int FINGERS = 2;
  touch_detail_t det[FINGERS];
  uint16_t raw[FINGERS] = {0, 0};
  int count = 0;
  const touch_detail_t& getDetail(size_t index = 0) const { const uint16_t id = raw[index < FINGERS ? index : 0]; return det[id < FINGERS ? id : 0]; }
  int getCount() const { return count; }
};

struct imu_3d_t { float x = 0, y = 0, z = 0; };
struct imu_data_t { imu_3d_t accel, gyro; };
struct EmuImu {
  imu_data_t data;
  bool update();                                 // returns true: new data every call
  imu_data_t getImuData() const { return data; }
};

struct EmuSpeaker {
  int tones = 0, wavs = 0; int lastFreq = 0; int lastMs = 0; int volume = 64;
  bool tone(float freq, uint32_t ms = 0xFFFFFFFF, int = -1, bool = true) {
    ++tones; lastFreq = int(freq); lastMs = int(ms);
    std::printf("[tone %d Hz %u ms]\n", int(freq), unsigned(ms == 0xFFFFFFFFu ? 0u : ms)); std::fflush(stdout); return true;
  }
  // like M5Unified: plays a RIFF/WAVE image from memory (the buffer must stay valid while it plays). Here: validated and printed.
  bool playWav(const uint8_t* wav, size_t len = ~size_t(0), uint32_t = 1, int = -1, bool = false) {
    ++wavs;
    const bool ok = wav && len >= 44 && !std::memcmp(wav, "RIFF", 4) && !std::memcmp(wav + 8, "WAVE", 4);
    std::printf("[wav %u bytes%s]\n", unsigned(len == ~size_t(0) ? 0u : len), ok ? "" : " INVALID"); std::fflush(stdout); return ok;
  }
  void stop() {}
  void stop(uint8_t) {}
  void setVolume(uint8_t v) { volume = v; std::printf("[volume %d]\n", int(v)); std::fflush(stdout); }
  uint8_t getVolume() const { return uint8_t(volume); }
  bool isPlaying() const { return false; }
  size_t isPlaying(uint8_t) const { return 0; }  // like M5Unified: tones on that channel (playing + queued); none here, a tone is only printed
};

struct config_t { int dummy = 0; };

struct M5Class {
  M5GFX Display;
  EmuTouch Touch;
  EmuImu Imu;
  EmuSpeaker Speaker;
  config_t config() { return {}; }
  void begin(const config_t& = {});
  void update();
  uint32_t frame = 0;                            // M5.update() calls so far
};
}  // namespace m5
extern m5::M5Class M5;

// ---- test hooks (emu/src/m5emu.cpp) -------------------------------------------------------------------------------
namespace emu {
bool scriptActive();
bool quitRequested();
int writeScreenshot(const char* ppmPath);        // 320 x 240 binary PPM; returns 0 on success
const char* photoName();                         // script command `photo <case>`: which test photo the emulated camera serves ("" = its default); see src/app/camera.h

enum class Key { Jump, Back };                   // Space / W / Up and Escape, or the script's `key jump|back`
bool keyHeld(Key k);

/** The directory the game uses as its "LittleFS": env EMU_FS, or ./emu_fs; created when missing. No trailing slash. */
const char* fsRoot();

/** Test hook (emu/unit): put finger `id` (0 or 1) down at (x, y) or lift it; the next M5.update() reports it. Script runs only. */
void setFinger(int id, bool down, int x = 0, int y = 0);
}
