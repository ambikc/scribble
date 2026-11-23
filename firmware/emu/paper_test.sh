#!/usr/bin/env bash
# paper_test.sh - runs the paper-mode scripts (emu/scripts/paper_*.txt) in the emulator and checks what the game prints.
#
#   cd firmware && pio run -e emu && emu/paper_test.sh          # all checks; exit code 0 = everything passed
#   OUT=/tmp/paper_shots emu/paper_test.sh                       # where the screenshots (PPM) and logs go (default: a temp folder)
#
# What it proves (the emulator's camera serves the synthetic template photos of shared/golden/vision_frames.bin):
#   * for EVERY golden photo case the game's level (`[paper level crc ...]`, the .scrb CRC of the recovered level) and quality status equal
#     what shared/golden/vision_vectors.json says vision.ts made of it (and test/test_vision checks in C++): clean, tilt, dim, dark, faded,
#     noisy, harsh, upside-down, tilt-noisy, nomarks, blank all go through standup-less check -> countdown -> processing -> review
#   * the full flow with the clean photo: lights turn green over time, GO pulses, the countdown speaks, the snap clicks, the thinner / thicker
#     ink buttons give the golden re-threshold CRCs, a dragged pin is saved, Looks good writes slot 3 and starts Play, the camera switches off
#   * the camera switches itself off after a paper screen is left without a word (Esc key)
# Look at the screenshots too (python3 emu/make_sheet.py $OUT/clean_*.ppm $OUT/sheet.png --cols 4).
set -u
cd "$(dirname "$0")/.."                                  # firmware/
BIN=.pio/build/emu/program
[ -x "$BIN" ] || pio run -e emu >/dev/null || { echo "build failed"; exit 2; }
OUT=${OUT:-$(mktemp -d /tmp/paper_shots.XXXXXX)}
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy
FAILS=0; PASSES=0
ok()   { PASSES=$((PASSES+1)); echo "  ok    $1"; }
bad()  { FAILS=$((FAILS+1));  echo "  FAIL  $1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# run NAME SCRIPT START [CASE] : runs the script with a fresh storage folder; log in $OUT/NAME.log, storage in $OUT/fs_NAME
run() {
  local name=$1 script=$2 start=$3 case_=${4:-} fs="$OUT/fs_$1" script_out="$OUT/$1.txt"
  rm -rf "$fs"; mkdir -p "$fs"
  { [ -n "$case_" ] && echo "1 photo $case_"; sed -e "s#@OUT@#$OUT#g" -e "s#@CASE@#$case_#g" -e "s#@NAME@#$name#g" "$script"; } > "$script_out"
  EMU_START=$start EMU_FS="$fs" EMU_SCRIPT="$script_out" timeout 300 "$BIN" > "$OUT/$name.log" 2>&1
  local rc=$?
  [ $rc -eq 0 ] || bad "$name: the emulator exited with $rc"
}
crc_of_first()   { grep -o '\[paper level crc [0-9A-F]*\]' "$1" | head -1 | sed 's/.*crc \([0-9A-F]*\)\]/\1/'; }
crc_list()       { grep -o '\[paper level crc [0-9A-F]*\]' "$1" | sed 's/.*crc \([0-9A-F]*\)\]/\1/'; }
status_of_first(){ grep -o '\[paper vision [0-9]* ms[^]]*status [0-9]*' "$1" | head -1 | sed 's/.*status //'; }
golden() {   # golden CASE FIELD : crc.scrb | status of the case in shared/golden/vision_vectors.json; "rethresh N" = the scrb CRC of the re-threshold with inkThresh N
  python3 - "$1" "$2" "${3:-}" <<'PY'
import json, sys
d = json.load(open('../shared/golden/vision_vectors.json'))
c = next(x for x in d['cases'] if x['name'] == sys.argv[1])
f = sys.argv[2]
if f == 'scrb': v = c['crc']['scrb']
elif f == 'status': v = c['status']
elif f == 'rethresh': v = next(r['scrb'] for r in c['rethresh'] if r['params'] == {'inkThresh': int(sys.argv[3])})
elif f == 'pins': print(c['start'][0], c['start'][1], c['flag'][0], c['flag'][1]); sys.exit(0)
if f == 'status': print(v)
else: print('%08X' % v)
PY
}

echo "== every golden photo: check -> countdown -> processing -> review =="
for c in clean tilt dim dark faded noisy harsh upside-down tilt-noisy nomarks blank; do
  run "flow_$c" emu/scripts/paper_flow.txt check "$c"
  L="$OUT/flow_$c.log"
  check "$c: level CRC equals the golden .scrb CRC ($(golden $c scrb))" '[ "$(crc_of_first $L)" = "$(golden $c scrb)" ]'
  check "$c: quality status equals the golden one ($(golden $c status))" '[ "$(status_of_first $L)" = "$(golden $c status)" ]'
  check "$c: processed exactly once" '[ "$(grep -c "\[paper vision" $L)" = 1 ]'
  check "$c: the snap clicked after the countdown" 'grep -q "\[sfx shutter\]" $L'
done

echo "== the whole flow with the clean photo (standup -> ... -> accept -> play) =="
run clean emu/scripts/paper_clean.txt standup
L="$OUT/clean.log"; FS="$OUT/fs_clean"
check "buffers allocated once: sizeof(VisionWork) = 770336" '[ "$(grep -c "paper buffers: VisionWork 770336" $L)" = 1 ]'
check "the camera came on after Ready, and went off at accept" 'grep -q "\[camera on\]" $L && grep -q "\[camera off\]" $L'
check "the lights turned green over time (a lights line with allGreen=1 after one with allGreen=0)" \
  'awk "/paper lights/ { if (/allGreen=0/) seen0=1; if (/allGreen=1/ && seen0) found=1 } END { exit !found }" $L'
check "the countdown spoke three, two, one, snap" '[ "$(grep -c "\[say \(three\|two\|one\|snap\) " $L)" = 4 ]'
check "exposure locked at the countdown and released after the snap" 'grep -q "exposure locked" $L && grep -q "exposure auto" $L'
check "clean level CRC equals the golden one" '[ "$(crc_of_first $L)" = "$(golden clean scrb)" ]'
CRCS=$(crc_list $L | tr '\n' ' ')
check "thinner ink (threshold 90) gives the golden re-threshold CRC" 'echo "$CRCS" | grep -q "$(golden clean rethresh 90)"'
check "thicker ink (threshold 64) gives the golden re-threshold CRC" 'echo "$CRCS" | grep -q "$(golden clean rethresh 64)"'
check "Looks good saved into slot 3 and opened it for play" 'grep -q "\[paper accept slot 3\]" $L && [ -s $FS/slot3.scrb ]'
check "standupSeen was saved" 'grep -q "standupSeen.\{1,3\}true" $FS/settings.json'
python3 - "$FS/slot3.scrb" > "$OUT/slot3.txt" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
print(int.from_bytes(b[10:12], 'little'), int.from_bytes(b[12:14], 'little'), int.from_bytes(b[14:16], 'little'), int.from_bytes(b[16:18], 'little'), (b[19] >> 2) & 1, b[20:44].split(b'\0')[0].decode())
PY
read -r SX SY FX FY PAPERFLAG NAME < "$OUT/slot3.txt"
check "the saved level is flagged fromPaper, named 'Paper 4', the dragged Start pin moved (was x=22, now $SX,$SY), the Flag pin untouched" \
  '[ "$PAPERFLAG" = 1 ] && [ "$NAME" = "Paper 4" ] && [ "$SX" -gt 50 ] && [ "$SX" -lt 90 ] && [ "$FX" = 297 ]'

echo "== stand-up gating and the lease =="
run standup emu/scripts/paper_standup.txt standup
L="$OUT/standup.log"
check "flat: Ready did nothing (no camera before the device was stood up)" '[ "$(grep -n "camera on" $L | head -1 | cut -d: -f1)" -gt "$(grep -n "log still-on-standup" $L | head -1 | cut -d: -f1)" ]'
check "upright: Ready led to the check (camera on)" 'grep -q "\[camera on\]" $L'
run standup_flat emu/scripts/paper_standup_flat.txt standup
L="$OUT/standup_flat.log"
check "flat for 8 s: Ready lets go anyway (never a dead end)" '[ "$(grep -n "camera on" $L | head -1 | cut -d: -f1)" -gt "$(grep -n "log not-yet" $L | head -1 | cut -d: -f1)" ]'
run retake emu/scripts/paper_retake.txt check clean
L="$OUT/retake.log"
check "Stop in the countdown released the exposure lock and returned to the check (no snap before the second GO)" '[ "$(grep -c "exposure locked" $L)" = 2 ] && [ "$(grep -c "sfx shutter" $L)" = 1 ]'
check "Retake went back to the check and the camera stayed on" '[ "$(grep -c "\[camera on\]" $L)" = 1 ]'
check "left with the Esc key: the camera switched itself off about 2 s later" \
  '[ "$(grep -n "camera off" $L | head -1 | cut -d: -f1)" -gt "$(grep -n "log left-the-paper-flow" $L | head -1 | cut -d: -f1)" ]'

echo "== a photo file (EMU_PHOTO), a camera that is slow, a camera that is missing =="
python3 emu/photo_from_golden.py 0 "$OUT/clean_file.ppm" --raw >/dev/null
for f in "$OUT/clean_file.ppm" "$OUT/clean_file.ppm.raw"; do
  export EMU_PHOTO="$f"; run "file_$(basename $f)" emu/scripts/paper_flow.txt check file; unset EMU_PHOTO
  check "EMU_PHOTO=$(basename $f): same level CRC as the golden clean photo" '[ "$(crc_of_first $OUT/file_$(basename $f).log)" = "$(golden clean scrb)" ]'
done
export EMU_START_MS=1500; run camera_wake emu/scripts/paper_nocamera.txt check; unset EMU_START_MS
check "a slow camera (1.5 s to start): the live check only starts once frames flow" \
  'grep -q "camera on\]" $OUT/camera_wake.log && grep -q "paper lights" $OUT/camera_wake.log'
export EMU_CAMERA_FAIL=1; run camera_fail emu/scripts/paper_nocamera.txt check; unset EMU_CAMERA_FAIL
check "a missing camera: [camera on: failed], no live check ran, no countdown started" \
  'grep -q "camera on: failed" $OUT/camera_fail.log && ! grep -q "paper lights\|exposure locked" $OUT/camera_fail.log'

echo
echo "screenshots and logs: $OUT"
echo "$PASSES passed, $FAILS failed"
[ $FAILS -eq 0 ]
