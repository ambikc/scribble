#!/usr/bin/env python3
"""photo_from_golden.py - write one of the stored synthetic template photos (shared/golden/vision_frames.bin) as a test photo file
for the emulated camera (env EMU_PHOTO): a P6 PPM (any viewer shows it) and, with --raw, a raw 640 x 480 RGB565 little-endian dump.

    python3 emu/photo_from_golden.py 0 /tmp/clean.ppm [--raw]      # frame 0 = clean, 1 = tilted, 2 = no marks, 3 = blank sheet
    EMU_PHOTO=/tmp/clean.ppm EMU_START=check .pio/build/emu/program

Any other PPM works too (a phone photo of the printed template): it is cover-fitted into 640 x 480 like the browser simulator does.
"""
import struct, sys
import os
b = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'shared', 'golden', 'vision_frames.bin'), 'rb').read()
assert b[:4] == b'VGF1'
W, H = 640, 480
count = struct.unpack('<I', b[4:8])[0]
pos = 8
frames = []
for k in range(count):
    n = struct.unpack('<I', b[pos:pos + 4])[0]
    frames.append(b[pos + 4:pos + 4 + n]); pos += 4 + n

def decode(data):
    f = [0] * (W * H)
    p = 0; i = 0
    def vi():
        nonlocal p
        v = 0; sh = 0
        while True:
            c = data[p]; p += 1
            v |= (c & 127) << sh; sh += 7
            if not c & 128: return v
    def pred(i): return f[i - W] if i >= W else (f[i - 1] if i > 0 else 0)
    total = W * H
    while True:
        run = vi()
        for _ in range(run):
            if i >= total: break
            f[i] = pred(i); i += 1
        if p >= len(data): break
        z = vi()
        if i < total:
            d = -((z + 1) >> 1) if z & 1 else z >> 1
            f[i] = (pred(i) + d) & 0xffff; i += 1
    while i < total: f[i] = pred(i); i += 1
    return f

f = decode(frames[int(sys.argv[1])])
out = bytearray()
for p in f:
    r = ((p >> 11) << 3) | (p >> 13); g = (((p >> 5) & 63) << 2) | ((p >> 9) & 3); bl = ((p & 31) << 3) | ((p >> 2) & 7)
    out += bytes((r, g, bl))
open(sys.argv[2], 'wb').write(b'P6\n640 480\n255\n' + bytes(out))
raw = b''.join(struct.pack('<H', p) for p in f)
if '--raw' in sys.argv: open(sys.argv[2] + '.raw', 'wb').write(raw)
print('wrote', sys.argv[2])
