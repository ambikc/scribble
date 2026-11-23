// audio.cpp - tones and the spoken countdown on M5.Speaker.   Port of sim/src/audio.ts.
#include "audio.h"
#include <M5Unified.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

namespace scribble {

static const char* const CLIP_NAMES[4] = { "three", "two", "one", "snap" };
#ifdef EMU
static const char* const SFX_NAMES[] = { "tick", "jump", "land", "bounce", "pop", "whoosh", "bloop", "win", "shutter", "tada", "hmm", "pen_ground", "pen_spike", "pen_bouncy", "beat" };
#endif

static uint8_t* clipAlloc(size_t n) {
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
  if (void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return static_cast<uint8_t*>(p);
#endif
  return static_cast<uint8_t*>(std::malloc(n));
}

Audio::~Audio() { for (auto& c : clips_) { std::free(c.data); c.data = nullptr; } }

void Audio::begin() {
  nPend_ = 0;
  loadClips();
  applyVolume();
}

/** Read the four countdown clips if they are there. A missing or malformed file just means "use the tone fallback". */
void Audio::loadClips() {
  for (int i = 0; i < 4; i++) {
    std::free(clips_[i].data); clips_[i] = ClipData();
    for (int variant = 0; variant < 2 && !clips_[i].data; variant++) {
      char path[160];
      if (variant == 0) std::snprintf(path, sizeof path, "%s/%s.wav", clipRoot, CLIP_NAMES[i]);
      else std::snprintf(path, sizeof path, "%s/audio/%s.wav", clipRoot, CLIP_NAMES[i]);
      FILE* f = std::fopen(path, "rb");
      if (!f) continue;
      std::fseek(f, 0, SEEK_END); const long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
      if (sz > 44 && sz <= 128 * 1024) {
        if (uint8_t* buf = clipAlloc(size_t(sz))) {
          if (std::fread(buf, 1, size_t(sz), f) == size_t(sz) && !std::memcmp(buf, "RIFF", 4) && !std::memcmp(buf + 8, "WAVE", 4)) { clips_[i].data = buf; clips_[i].len = size_t(sz); }
          else std::free(buf);
        }
      }
      std::fclose(f);
    }
  }
}

void Audio::applyVolume() {
  volume = volume < 0 ? 0 : volume > 2 ? 2 : volume;
  M5.Speaker.setVolume(volume == 0 ? 0 : volume == 1 ? VOL_LOW : VOL_MED);
}

int Audio::cycleVolume() { volume = (volume + 1) % 3; applyVolume(); play(Sfx::Tick); return volume; }

/** One tone now, or queued for `delayMs` from now. */
void Audio::emit(int freq, int ms, int delayMs) {
  if (delayMs <= 0) { M5.Speaker.tone(float(freq), uint32_t(ms)); return; }
  if (nPend_ >= MAX_PENDING) return;                       // queue full: drop (never block the game for a beep)
  pend_[nPend_++] = { uint32_t(millis()) + uint32_t(delayMs), uint16_t(freq), uint16_t(ms) };
}

/** A tone, or a sweep f0 -> f1 approximated by a few short steps (exponential in frequency, like the oscillator ramp). */
void Audio::tone(int f0, int ms, int f1, int delayMs) {
  if (volume == 0) return;
  if (f1 == 0 || f1 == f0 || ms < 40) { emit(f0, ms, delayMs); return; }
  const int steps = ms < 100 ? 3 : 4, dur = ms / steps;
  for (int i = 0; i < steps; i++) {
    const double u = double(i) / (steps - 1);
    emit(int(std::lround(f0 * std::pow(double(f1) / f0, u))), dur, delayMs + i * dur);
  }
}

void Audio::update(uint32_t nowMs) {
  if (nPend_ == 0) return;
  // Release what is due. Each tone() replaces the one before it, so when several are due at once (a long stall) only the newest
  // plays, and a tone whose whole window has already passed is dropped.
  Pending best = {0, 0, 0}; bool have = false; int w = 0;
  for (int r = 0; r < nPend_; r++) {
    const Pending p = pend_[r];
    const int32_t late = int32_t(nowMs - p.at);
    if (late < 0) { pend_[w++] = p; continue; }                       // not yet
    if (late <= int32_t(p.ms) + 20 && (!have || int32_t(p.at - best.at) >= 0)) { best = p; have = true; }
  }
  nPend_ = w;
  if (have && volume > 0) M5.Speaker.tone(float(best.freq), best.ms);
}

/** The effects table of docs/game-design.md section 7 (same numbers as audio.ts). */
void Audio::play(Sfx s) {
  if (volume == 0) return;
#ifdef EMU
  std::printf("[sfx %s]\n", SFX_NAMES[int(s)]);
#endif
  switch (s) {
    case Sfx::Tick:     tone(1000, 15); break;
    case Sfx::Jump:     tone(400, 80, 700); break;
    case Sfx::Land:     tone(150, 30); break;
    case Sfx::Bounce:   tone(300, 140, 900); break;
    case Sfx::Pop:      tone(700, 160, 150); break;
    case Sfx::Whoosh:   tone(500, 80, 200); break;
    case Sfx::Bloop:    tone(300, 90, 500); break;
    case Sfx::Win:      { static const int f[4] = { 523, 659, 784, 1047 }; for (int i = 0; i < 4; i++) tone(f[i], 90, 0, i * 100); } break;
    case Sfx::Shutter:  tone(1800, 25); tone(900, 40, 0, 45); break;
    case Sfx::Tada:     tone(523, 90); tone(784, 140, 0, 100); break;
    case Sfx::Hmm:      tone(300, 110); tone(240, 160, 0, 140); break;
    case Sfx::PenGround: tone(220, 12); break;
    case Sfx::PenSpike:  tone(880, 12); break;
    case Sfx::PenBouncy: tone(440, 12); break;
    case Sfx::Beat:     tone(660, 60); break;
  }
}

/** Spoken countdown word. Plays even at "low" volume; silent only when sound is Off. */
void Audio::say(Clip c) {
  if (volume == 0) return;
  const int i = int(c);
  const ClipData& cl = clips_[i];
#ifdef EMU
  std::printf("[say %s %s]\n", CLIP_NAMES[i], cl.data ? "wav" : "tone");
#endif
  if (cl.data) {
    // volume >= 1 here, so the speaker is already at Low or Medium: the countdown is never quieter than Low
    M5.Speaker.playWav(cl.data, cl.len, 1, -1, true);
    return;
  }
  static const int f[4] = { 392, 440, 494, 784 };                  // audio.ts fallback: three, two, one, snap
  tone(f[i], 200);
}

}  // namespace scribble
