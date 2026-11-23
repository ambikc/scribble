// audio.h - tones and the spoken countdown.   Port of sim/src/audio.ts.   (platform adapter: M5.Speaker)
//
// Effects are tones (no files). The countdown "three, two, one, snap!" is four recorded clips (shared/audio/*.wav,
// 16 kHz mono, CC0) played from flash / LittleFS; until the clips exist, fall back to tones exactly like audio.ts does.
// Volume: 0 = off, 1 = low (default), 2 = medium. The countdown is never quieter than Low unless sound is Off.
// Tones with a start delay (the win jingle) are queued and released by update().
//
// Speaker volume per level (M5.Speaker.setVolume 0..255): Off = 0, Low = VOL_LOW, Medium = VOL_MED. TUNABLE: set by ear on
// the device (build-plan Step 8); the speaker is loud close to the ear, hence the small numbers.
//
// Clips: <clipRoot>/three.wav, two.wav, one.wav, snap.wav (also tried: <clipRoot>/audio/<name>.wav), 16 kHz 16-bit mono PCM.
// They are read once in begin() and kept in memory (M5.Speaker.playWav plays from a buffer that must stay valid).
//
// Sweeps (f0 -> f1) are approximated by 3-4 short constant tones (the speaker has no frequency ramp); per-tone gain
// (audio.ts gainScale) is not modelled, so the quiet pen ticks are simply very short.
#pragma once
#include <cstddef>
#include <cstdint>

namespace scribble {

enum class Sfx : uint8_t { Tick, Jump, Land, Bounce, Pop, Whoosh, Bloop, Win, Shutter, Tada, Hmm, PenGround, PenSpike, PenBouncy, Beat };
enum class Clip : uint8_t { Three, Two, One, Snap };

constexpr int VOL_LOW = 64, VOL_MED = 140;      // M5.Speaker volume for sound level 1 and 2 (tunable)

class Audio {
 public:
  int volume = 1;
  const char* clipRoot = "/littlefs";           // where the countdown .wav clips live; main() points it at the emulator's folder
  ~Audio();
  void begin();
  /** Call every loop pass: releases queued (delayed) tones. */
  void update(uint32_t nowMs);
  void play(Sfx s);
  void say(Clip c);
  /** Cycle 0 -> 1 -> 2 -> 0, apply it, play a tick, return the new volume. */
  int cycleVolume();
  void applyVolume();
  /** True when the real recorded clip for `c` was found (false: say() falls back to a tone). */
  bool hasClip(Clip c) const { return clips_[int(c)].data != nullptr; }

 private:
  struct Pending { uint32_t at; uint16_t freq, ms; };
  struct ClipData { uint8_t* data = nullptr; size_t len = 0; };
  static constexpr int MAX_PENDING = 32;
  Pending pend_[MAX_PENDING]; int nPend_ = 0;
  ClipData clips_[4];
  void tone(int f0, int ms, int f1 = 0, int delayMs = 0);   // f1 != 0 and != f0: a sweep in short steps
  void emit(int freq, int ms, int delayMs);
  void loadClips();
};

}  // namespace scribble
