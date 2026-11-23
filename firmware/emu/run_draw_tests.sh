#!/usr/bin/env bash
# run_draw_tests.sh - end-to-end check of the Draw / Edit screen in the PC emulator against the portable core.
#
#   cd firmware && emu/run_draw_tests.sh            (needs pio and libsdl2-dev; Pillow only for the contact sheet)
#
# 1. `pio test -e native -f test_draw -v` prints DRAW_EXPECT n XXXXXXXX: the level digest after each step of emu/scripts/draw_strokes.txt,
#    computed with level.cpp's paintLine / paintDisc / eraseDisc (not by hand).
# 2. The real game runs that script headless (EMU_START=draw, a blank level in an empty slot, a private EMU_FS). After every 6 s of idle
#    it autosaves and prints `[level crc XXXXXXXX]`: the sequence must equal the expected one, so strokes, undo, eraser, exclusive
#    materials, fast scribbles, pin drags, pin drops and strokes next to a pin are all byte-exact.
# 3. Between the log lines tb-begin and tb-end the script drags from the toolbar and the corner chips into the playfield: nothing
#    may be painted, so no `[level crc` line may appear there.
# 4. Screenshots land in $SHOTS (default /tmp/draw_shots) with a contact sheet draw_sheet.png.
set -euo pipefail
cd "$(dirname "$0")/.."
SHOTS=${SHOTS:-/tmp/draw_shots}
FS=$(mktemp -d /tmp/draw_fs.XXXXXX)
mkdir -p "$SHOTS"; rm -f "$SHOTS"/draw_*.ppm

if ! pio test -e native -f test_draw -v > "$SHOTS/native.log" 2>&1; then cat "$SHOTS/native.log"; echo "native test_draw FAILED"; exit 1; fi
grep -E "test cases:" "$SHOTS/native.log" || true

pio run -e emu 2>&1 | tail -2
EMU_START=draw EMU_SLOT=5 EMU_FS="$FS" EMU_SCRIPT=emu/scripts/draw_strokes.txt SDL_VIDEODRIVER=dummy .pio/build/emu/program > "$SHOTS/emu.log" 2>&1

rc=0
python3 -I - "$SHOTS/native.log" "$SHOTS/emu.log" <<'EOF' || rc=$?
import re, sys
expect = [m.group(1) for m in (re.search(r"DRAW_EXPECT \d+ ([0-9A-F]{8})", l) for l in open(sys.argv[1])) if m]
lines = open(sys.argv[2]).read().splitlines()
got = [m.group(1) for m in (re.search(r"\[level crc ([0-9A-F]{8})\]", l) for l in lines) if m]
ok = True
if got != expect:
    ok = False
    print("FAIL: level checksums differ"); print(" expected:", expect); print(" emulator:", got)
else:
    print(f"ok: {len(got)} saved levels match the portable core byte for byte")
b = next(i for i, l in enumerate(lines) if "[log tb-begin" in l); e = next(i for i, l in enumerate(lines) if "[log tb-end" in l)
if any("[level crc" in l for l in lines[b:e]):
    ok = False; print("FAIL: something was painted/saved by touches that began on the toolbar or a chip")
else:
    print("ok: touches that begin on the toolbar / corner chips paint nothing")
pen = {k: sum(1 for l in lines if f"[sfx {k}]" in l) for k in ("pen_ground", "pen_spike", "pen_bouncy")}
print("pen clicks:", pen)
if not all(pen.values()): ok = False; print("FAIL: a pen click is missing")
sys.exit(0 if ok else 1)
EOF
python3 -I emu/make_sheet.py "$SHOTS"/draw_*.ppm "$SHOTS/draw_sheet.png" --cols 4 2>/dev/null && echo "contact sheet: $SHOTS/draw_sheet.png" || true
rm -rf "$FS"
exit $rc
