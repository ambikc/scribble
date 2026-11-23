// audio.h - tones and the spoken countdown.   Port of sim/src/audio.ts.   (platform adapter: M5.Speaker)
//
// Effects are tones (no files). The countdown "three, two, one, snap!" is four recorded clips (shared/audio/*.wav,
// 16 kHz mono, CC0) played from flash / LittleFS; until the clips exist, fall back to tones exactly like audio.ts does.
// Volume: 0 = off, 1 = low (default), 2 = medium. The countdown is never quieter than Low unless sound is Off.
// Tones with a start delay (the win jingle) are queued and released by update().
#pragma once
#include <cstdint>

namespace scribble {

enum class Sfx : uint8_t { Tick, Jump, Land, Bounce, Pop, Whoosh, Bloop, Win, Shutter, Tada, Hmm, PenGround, PenSpike, PenBouncy, Beat };
enum class Clip : uint8_t { Three, Two, One, Snap };

class Audio {
 public:
  int volume = 1;
  void begin();
  /** Call every loop pass: releases queued (delayed) tones. */
  void update(uint32_t nowMs);
  void play(Sfx s);
  void say(Clip c);
  /** Cycle 0 -> 1 -> 2 -> 0, apply it, play a tick, return the new volume. */
  int cycleVolume();
  void applyVolume();
};

}  // namespace scribble
