#!/usr/bin/env python3
"""mk_level.py - write hand-made test levels (.scrb) into an emulator storage folder.

    python3 emu/mk_level.py <EMU_FS folder> <slot>=<name> [<slot>=<name> ...]

Names: bridge   the Bridge lesson with its gap filled (win by tilting right, no jump)
       hop      one flat floor with a 16 px wide spike patch in the middle (roll into it: pop; jump over it: win)
       steps    a flat floor with a 12 px high step the ball cannot roll over (needs a jump)
Format: docs/system-design.md section 4 (no thumbnail, no extras). A level is three 320 x 192 1-bit layers, MSB first.
"""
import struct
import sys
import zlib

W, H, RB = 320, 192, 40


def layer():
    return bytearray(RB * H)


def rect(l, x0, y0, x1, y1):          # [x0, x1) x [y0, y1)
    for y in range(y0, y1):
        for x in range(x0, x1):
            l[y * RB + (x >> 3)] |= 0x80 >> (x & 7)


def make(name):
    g, s, b = layer(), layer(), layer()
    start, flag = (40, 104), (280, 112)
    if name == "bridge":                                   # starter lesson + a bridge
        rect(g, 16, 112, 72, 120); rect(g, 248, 112, 304, 120); rect(g, 72, 112, 248, 120)
    elif name == "hop":
        rect(g, 16, 112, 304, 120); rect(s, 150, 104, 166, 112)
    elif name == "steps":
        rect(g, 16, 112, 304, 120); rect(g, 150, 100, 166, 112)
    elif name == "busy":                                   # a worst-case child scribble: ~half of the world is ink (for timing only)
        for k in range(0, 192, 24):
            rect(g, 0, k, 320, k + 10)
        for k in range(0, 320, 40):
            rect(s, k, 0, k + 12, 192); rect(b, k + 20, 0, k + 30, 192)
        start, flag = (40, 60), (280, 112)
    else:
        raise SystemExit("unknown level " + name)
    return g, s, b, start, flag


def encode(name, title):
    g, s, b, start, flag = make(name)
    body = bytes(g) + bytes(s) + bytes(b)
    h = bytearray(64)
    h[0:4] = b"SCRB"; h[4] = 1; h[5] = 0
    struct.pack_into("<HH", h, 6, W, H)
    struct.pack_into("<HHHH", h, 10, start[0], start[1], flag[0], flag[1])
    h[18] = 3; h[19] = 0
    h[20:20 + len(title)] = title.encode()
    struct.pack_into("<I", h, 44, zlib.crc32(body) & 0xFFFFFFFF)
    struct.pack_into("<I", h, 48, 0)
    return bytes(h) + body


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    folder = sys.argv[1]
    for arg in sys.argv[2:]:
        slot, name = arg.split("=")
        with open("%s/slot%d.scrb" % (folder, int(slot)), "wb") as f:
            f.write(encode(name, name.capitalize()))


main()
