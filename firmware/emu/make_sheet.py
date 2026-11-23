#!/usr/bin/env python3
"""make_sheet.py - stitch the 320x240 PPM screenshots of the emulator into one contact-sheet PNG (needs Pillow).

    python3 emu/make_sheet.py /tmp/gallery/p*.ppm /tmp/gallery/sheet.png [--cols 3] [--scale 2]

The last argument is the output PNG; every other argument is a PPM (P6) file written by the script command `shot`.
Each tile is labelled with its file name. With --scale N every pixel is repeated N times so small icons are easy to judge.
"""
import argparse
import sys

from PIL import Image, ImageDraw


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="input .ppm files, then the output .png")
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--scale", type=int, default=1)
    a = ap.parse_args()
    if len(a.files) < 2:
        ap.error("need at least one .ppm and the output .png")
    *srcs, out = a.files
    tiles = [(s, Image.open(s).convert("RGB")) for s in srcs]
    sc = max(1, a.scale)
    w, h = tiles[0][1].width * sc, tiles[0][1].height * sc
    pad, label = 6, 14
    cols = max(1, min(a.cols, len(tiles)))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + label + pad) + pad), (60, 60, 60))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(tiles):
        x, y = pad + (i % cols) * (w + pad), pad + (i // cols) * (h + label + pad)
        d.text((x, y), name.rsplit("/", 1)[-1], fill=(230, 230, 230))
        sheet.paste(im.resize((w, h), Image.NEAREST) if sc > 1 else im, (x, y + label))
    sheet.save(out)
    print(f"wrote {out}: {len(tiles)} tiles, {sheet.width} x {sheet.height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
