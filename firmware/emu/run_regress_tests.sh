#!/usr/bin/env bash
# run_regress_tests.sh - regression checks for the review fixes of the Title / Draw / Play / Win / menu screens (emu/scripts/fix_*.txt).
#   cd firmware && pio run -e emu && emu/run_regress_tests.sh        (run_all.sh calls it)
# Each script has its own `# expect` directives (emu/run_script.sh); this file adds the checks that need the storage folder or the
# order of log lines: exact level digests after the pin drags, slot files, the deferred settings write.
set -u
here=$(cd "$(dirname "$0")" && pwd)
fail=0
run() { bash "$here/run_script.sh" "$here/scripts/$1.txt" > /tmp/regress_one.log 2>&1 && echo "PASS $1" || { echo "FAIL $1"; cat /tmp/regress_one.log; fail=1; }; }
log() { echo "/tmp/scribble_run/$1/run.log"; }
fs()  { echo "/tmp/scribble_run/$1/fs"; }
check() { if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi; }
# line number of the first line containing a fixed string (0 if none)
ln() { grep -nF -m1 -- "$2" "$(log "$1")" | cut -d: -f1 | grep . || echo 0; }

for s in fix_pin_corner fix_pin_corner_start fix_win_lockout fix_win_new fix_win_new_full fix_chooser_full fix_title_paper_full fix_menu_saves fix_win_save; do run $s; done

# ---- pins: the digests of the two saved levels must equal the Bridge with the pins exactly where the clamped drags put them ----
# (a stroke painted by mistake, or a drag that did not take hold, gives a different digest). Same digest as levelDigest() in editing.h.
cat > /tmp/regress_digest.py <<'PY'
import re, sys, zlib
W, H, RB = 320, 192, 40
def bridge():
    g = bytearray(RB * H)
    for x0, x1 in ((16, 72), (248, 304)):
        for y in range(112, 120):
            for x in range(x0, x1): g[y * RB + (x >> 3)] |= 0x80 >> (x & 7)
    return g
def digest(pins):
    b = bytes(bridge()) + bytes(2 * RB * H) + b"".join(int(v).to_bytes(4, "little") for v in pins)
    return "%08X" % (zlib.crc32(b) & 0xffffffff)
def got(p): return re.findall(r"\[level crc ([0-9A-F]{8})\]", open(p).read())
# flag: dragged to (312, 8) -> lands at centre (312, 58), base y 70; then to (200, 100): base (200, 112).  start: (4, 4) -> (8, 58); then (100, 120)
want_f = [digest((40, 104, 312, 70)), digest((40, 104, 200, 112))]
want_s = [digest((8, 58, 280, 112)), digest((100, 120, 280, 112))]
print("flag", "ok" if got(sys.argv[1]) == want_f else "BAD %s != %s" % (got(sys.argv[1]), want_f))
print("start", "ok" if got(sys.argv[2]) == want_s else "BAD %s != %s" % (got(sys.argv[2]), want_s))
PY
digests=$(python3 -I /tmp/regress_digest.py "$(log fix_pin_corner)" "$(log fix_pin_corner_start)")
check pin_corner_flag_can_be_grabbed_again "$(echo "$digests" | sed -n 1p)" "flag ok"
check pin_corner_start_can_be_grabbed_again "$(echo "$digests" | sed -n 2p)" "start ok"

# ---- the menu chip saves first ----
boot=$(mktemp -d /tmp/regress_boot.XXXXXX); printf '10 quit\n' > "$boot/b.txt"
EMU_FS="$boot" EMU_SCRIPT="$boot/b.txt" SDL_VIDEODRIVER=dummy "$here/../.pio/build/emu/program" > /dev/null 2>&1
before=$(md5sum < "$boot/slot0.scrb"); after=$(md5sum < "$(fs fix_menu_saves)/slot0.scrb"); rm -rf "$boot"
[ "$before" != "$after" ] && echo "PASS menu_chip_saved_the_edit" || { echo "FAIL menu_chip_saved_the_edit"; fail=1; }
check menu_saved_before_the_menu_opened "$([ "$(ln fix_menu_saves '[level crc')" -lt "$(ln fix_menu_saves '[screen menu]')" ] && echo yes || echo no)" yes

# ---- the win is saved late: [win slot] < [screen win] < [win saved slot] ----
w=$(ln fix_win_save '[win slot 3]'); s=$(ln fix_win_save '[screen win]'); v=$(ln fix_win_save '[win saved slot 3]')
check win_settings_write_waits_for_the_win_screen "$([ "$w" -gt 0 ] && [ "$w" -lt "$s" ] && [ "$s" -lt "$v" ] && echo yes || echo no)" yes
grep -q '"solvedMask":8' "$(fs fix_win_save)/settings.json" && echo "PASS win_solved_bit_on_disk" || { echo "FAIL win_solved_bit_on_disk"; fail=1; }

# ---- Win -> New: the first free slot gets the new level ----
[ -f "$(fs fix_win_new)/slot3.scrb" ] && echo "PASS win_new_creates_level_in_first_free_slot" || { echo "FAIL win_new_creates_level_in_first_free_slot"; fail=1; }

# ---- full box: nothing was written beyond the ten slots, nothing replaced ----
check full_box_no_replace "$(ls "$(fs fix_chooser_full)" | grep -c 'slot.\.scrb')" 10

[ $fail = 0 ] && echo "ALL REGRESSION TESTS PASSED" || echo "SOME REGRESSION TESTS FAILED"
exit $fail
