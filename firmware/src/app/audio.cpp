// audio.cpp - tones and the spoken countdown on M5.Speaker.   Port of sim/src/audio.ts.
#include "audio.h"
#include <M5Unified.h>
#include "i2c_lock.h"
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
  I2cGuard bus;                                 // the speaker amplifier is set up over the internal I2C bus on the CoreS3
  M5.Speaker.setVolume(volume == 0 ? 0 : volume == 1 ? VOL_LOW : VOL_MED);
}

int Audio::cycleVolume() { volume = (volume + 1) % 3; applyVolume(); play(Sfx::Tick); return volume; }

/** Hand one tone to the speaker on TONE_CH. replace = stop what is playing there (stop_current_sound); otherwise it is queued
 *  behind the current tone. Never blocks: M5.Speaker waits for a free slot when a channel already has one tone queued, so a
 *  queued tone is only sent while the channel holds at most one (isPlaying(ch) < 2); else it replaces. The first tone may start
 *  the speaker task, which talks to the amplifier over I2C: hence the guard. */
void Audio::speakerTone(int freq, int ms, bool replace) {
  I2cGuard bus;
  if (!replace && M5.Speaker.isPlaying(uint8_t(TONE_CH)) >= 2) replace = true;
  M5.Speaker.tone(float(freq), uint32_t(ms), TONE_CH, replace);
}

void Audio::beginSound() { nPend_ = 0; firstStep_ = true; }

/** One step of the current sound: now (the first step replaces what is playing), or queued for `delayMs` from now. */
void Audio::emit(int freq, int ms, int delayMs) {
  const uint32_t now = uint32_t(millis()), at = now + uint32_t(delayMs > 0 ? delayMs : 0);
  const bool first = firstStep_;
  const bool chained = !first && int32_t(at - lastEnd_) <= 2 && int32_t(at - lastEnd_) >= -2;
  firstStep_ = false;
  lastEnd_ = at + uint32_t(ms);
  if (delayMs <= 0) { speakerTone(freq, ms, first); return; }
  if (nPend_ >= MAX_PENDING) return;                       // queue full: drop (never block the game for a beep)
  pend_[nPend_++] = { at, uint16_t(freq), uint16_t(ms), chained };
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

/** Release every queued step that is due (in order), dropping one whose whole window has passed (a long stall). A step that follows
 *  the previous one with no gap is handed over up to EARLY_MS early, queued behind the playing tone, so a sweep stays gapless however
 *  long the loop pass. pend_ is in time order: emit() appends steps of one sound in order and a new sound clears it. */
void Audio::update(uint32_t nowMs) {
  constexpr int32_t EARLY_MS = 50;
  int r = 0;
  while (r < nPend_) {
    const Pending& p = pend_[r];
    const int32_t late = int32_t(nowMs - p.at);
    if (late >= 0) {
      if (late <= int32_t(p.ms) + 20 && volume > 0) speakerTone(p.freq, p.ms, false);   // the previous step has ended or is ending
      ++r; continue;
    }
    // not due yet: queue it behind the playing step if it follows it seamlessly and the channel has room for exactly that
    if (p.chained && late >= -EARLY_MS && volume > 0 && M5.Speaker.isPlaying(uint8_t(TONE_CH)) == 1) { speakerTone(p.freq, p.ms, false); ++r; continue; }
    break;
  }
  if (r > 0) { std::memmove(pend_, pend_ + r, sizeof(Pending) * size_t(nPend_ - r)); nPend_ -= r; }
}

/** The effects table of docs/game-design.md section 7 (same numbers as audio.ts). */
void Audio::play(Sfx s) {
  if (volume == 0) return;
  if (s == Sfx::Bloop) {                       // a ball that keeps respawning must not bloop every tick
    const uint32_t now = uint32_t(millis());
    if (bloopEver_ && now - lastBloopAt_ < BLOOP_MIN_MS) return;
    bloopEver_ = true; lastBloopAt_ = now;
  }
  beginSound();
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
  beginSound();
  if (cl.data) {
    // volume >= 1 here, so the speaker is already at Low or Medium: the countdown is never quieter than Low.
    // The clip plays on a free channel (not TONE_CH); a tone still sounding there is stopped so the word is clear.
    I2cGuard bus;
    M5.Speaker.stop(uint8_t(TONE_CH));
    M5.Speaker.playWav(cl.data, cl.len, 1, -1, true);
    return;
  }
  static const int f[4] = { 392, 440, 494, 784 };                  // audio.ts fallback: three, two, one, snap
  tone(f[i], 200);
}

}  // namespace scribble
