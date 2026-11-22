// audio.ts - synthesised tones and the spoken countdown.   (platform adapter; the firmware version drives the I2S speaker)
//
// Sound effects are tones (no audio files). The countdown "three, two, one, snap!" is SPEECH: the firmware plays four
// small recorded clips; here we use the browser's speech synthesis (and fall back to tones if it is not available).
// Volume: 0 = off, 1 = low (default), 2 = medium.  The countdown is never quieter than Low unless sound is Off.

export type Sfx = 'tick' | 'jump' | 'land' | 'bounce' | 'pop' | 'whoosh' | 'bloop' | 'win' | 'shutter' | 'tada' | 'hmm' | 'pen_ground' | 'pen_spike' | 'pen_bouncy' | 'beat';
export type Clip = 'three' | 'two' | 'one' | 'snap';

export class Audio {
  volume = 1;
  private ctx: AudioContext | null = null;

  private ensure(): AudioContext | null {
    if (!this.ctx) { try { this.ctx = new (window.AudioContext || (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext)(); } catch { return null; } }
    if (this.ctx.state === 'suspended') void this.ctx.resume();
    return this.ctx;
  }
  private gain() { return this.volume === 0 ? 0 : this.volume === 1 ? 0.08 : 0.2; }

  /** One tone, optionally sweeping from f0 to f1, starting `delay` seconds from now. */
  tone(f0: number, ms: number, f1 = f0, delay = 0, type: OscillatorType = 'square', gainScale = 1): void {
    const g = this.gain() * gainScale; if (g === 0) return;
    const ctx = this.ensure(); if (!ctx) return;
    const t0 = ctx.currentTime + delay, o = ctx.createOscillator(), v = ctx.createGain();
    o.type = type; o.frequency.setValueAtTime(f0, t0); if (f1 !== f0) o.frequency.exponentialRampToValueAtTime(f1, t0 + ms / 1000);
    v.gain.setValueAtTime(g, t0); v.gain.exponentialRampToValueAtTime(0.0001, t0 + ms / 1000 + 0.02);
    o.connect(v).connect(ctx.destination); o.start(t0); o.stop(t0 + ms / 1000 + 0.05);
  }

  /** The effects table in docs/game-design.md section 7. */
  play(s: Sfx): void {
    switch (s) {
      case 'tick': this.tone(1000, 15); break;
      case 'jump': this.tone(400, 80, 700); break;
      case 'land': this.tone(150, 30, 150, 0, 'sine', 1.5); break;
      case 'bounce': this.tone(300, 140, 900); break;
      case 'pop': this.tone(700, 160, 150, 0, 'sawtooth'); break;
      case 'whoosh': this.tone(500, 80, 200, 0, 'sine'); break;
      case 'bloop': this.tone(300, 90, 500, 0, 'sine'); break;
      case 'win': [523, 659, 784, 1047].forEach((f, i) => this.tone(f, 90, f, i * 0.1, 'square')); break;
      case 'shutter': this.tone(1800, 25); this.tone(900, 40, 900, 0.045); break;
      case 'tada': this.tone(523, 90, 523); this.tone(784, 140, 784, 0.1); break;
      case 'hmm': this.tone(300, 110, 300, 0, 'sine'); this.tone(240, 160, 240, 0.14, 'sine'); break;
      case 'pen_ground': this.tone(220, 12, 220, 0, 'square', 0.4); break;
      case 'pen_spike': this.tone(880, 12, 880, 0, 'square', 0.4); break;
      case 'pen_bouncy': this.tone(440, 12, 440, 0, 'square', 0.4); break;
      case 'beat': this.tone(660, 60, 660, 0, 'sine'); break;
    }
  }

  /** Spoken countdown word. Plays even at "low" volume; silent only when sound is Off. */
  say(c: Clip): void {
    if (this.volume === 0) return;
    const word = { three: 'three', two: 'two', one: 'one', snap: 'snap!' }[c];
    try {
      if ('speechSynthesis' in window) {
        const u = new SpeechSynthesisUtterance(word); u.rate = 1.15; u.volume = this.volume === 1 ? 0.7 : 1; u.pitch = 1.1;
        window.speechSynthesis.speak(u); return;
      }
    } catch { /* fall through to tones */ }
    const f = { three: 392, two: 440, one: 494, snap: 784 }[c]; this.tone(f, 200, f, 0, 'sine', 1.2);
  }

  cycleVolume(): number { this.volume = (this.volume + 1) % 3; this.play('tick'); return this.volume; }
}
