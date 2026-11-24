#!/usr/bin/env bash
# camera_test.sh - the camera's failure paths and its memory, in the emulator (virtual clock). Build first: pio run -e emu
#
#   cd firmware && emu/camera_test.sh            exit code 0 = every check passed;  OUT=/tmp/cam emu/camera_test.sh keeps logs + shots
#
#   * a stalled snap (EMU_CAMERA_STALL=1: no frame ever arrives for the snap): the snap reports Failed / Timeout within
#     CAMERA_GRAB_BUDGET_MS (3 s) instead of freezing the countdown, the game goes back to the Check screen, the live view keeps going
#     and a second GO starts a second countdown
#   * a camera that starts but never delivers a frame (EMU_CAMERA_STALL=2): Failed after CAMERA_NO_FRAMES_MS (1.5 s), no live check, no countdown
#   * 20 start / stop cycles through the real screens (Chooser -> Paper -> Check -> back): every buffer camera.cpp allocates is freed at
#     each stop (Camera::bufferBytes() back to 0) and every start holds the same amount
set -u
cd "$(dirname "$0")/.."                                  # firmware/
BIN=.pio/build/emu/program
[ -x "$BIN" ] || { echo "build first: pio run -e emu"; exit 2; }
OUT=${OUT:-$(mktemp -d /tmp/camera_test.XXXXXX)}
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy
FAILS=0; PASSES=0
ok()   { PASSES=$((PASSES+1)); echo "  ok    $1"; }
bad()  { FAILS=$((FAILS+1));  echo "  FAIL  $1"; }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
# run NAME START SCRIPT-FILE [VAR=VALUE ...] : fresh storage folder, log in $OUT/NAME.log
run() {
  local name=$1 start=$2 script=$3; shift 3
  rm -rf "$OUT/fs_$name"; mkdir -p "$OUT/fs_$name"
  env "$@" EMU_START="$start" EMU_FS="$OUT/fs_$name" EMU_SCRIPT="$script" timeout 120 "$BIN" > "$OUT/$name.log" 2>&1 || bad "$name: the emulator exited with $?"
}
line_of() { grep -n -m1 -F -- "$2" "$1" | cut -d: -f1; }   # first line number of a fixed string (empty if missing)

echo "== a stalled snap times out within the budget and the Check screen recovers =="
cat > "$OUT/stall.txt" <<EOF
100 upright 1
2400 tap 250 216
5000 log snap-asked-about-now
6000 shot $OUT/stall_waiting.ppm
9000 shot $OUT/stall_back_in_check.ppm
9200 log back-in-check
10500 tap 250 216
11000 shot $OUT/stall_second_countdown.ppm
11200 quit
EOF
run stall check "$OUT/stall.txt" EMU_CAMERA_STALL=1
L="$OUT/stall.log"
check "the snap reported a timeout" 'grep -q "\[camera snap timed out after" $L'
MS=$(grep -o "snap timed out after [0-9]*" $L | head -1 | grep -o "[0-9]*$")
check "... within the budget (3000 ms + one 60 Hz tick; got ${MS:-none} ms)" '[ -n "$MS" ] && [ "$MS" -ge 3000 ] && [ "$MS" -le 3017 ]'
check "the snap was asked for at the countdown's end, the timeout came before 'back-in-check' (9.2 s)" \
  '[ -n "$(line_of $L "snap timed out")" ] && [ "$(line_of $L "snap timed out")" -gt "$(line_of $L "log snap-asked-about-now")" ] && [ "$(line_of $L "snap timed out")" -lt "$(line_of $L "log back-in-check")" ]'
check "no shutter click, no processing (nothing was snapped)" '! grep -q "sfx shutter\|paper vision" $L'
check "the exposure was released and the sensor went back to QVGA" \
  '[ "$(line_of $L "exposure auto")" -gt "$(line_of $L "snap timed out")" ] && [ "$(grep -c "camera size qvga" $L)" -ge 1 ]'
check "the camera stayed on (no off, no failure) and a second GO started a second countdown" \
  '! grep -q "camera off\|camera on: failed" $L && [ "$(grep -c "exposure locked" $L)" = 2 ]'

echo "== a camera that starts but never sends a frame =="
cat > "$OUT/noframes.txt" <<EOF
100 upright 1
1500 log one-and-a-half-s
1800 log after-the-watchdog
2000 shot $OUT/noframes.ppm
2400 tap 250 216
2600 quit
EOF
run noframes check "$OUT/noframes.txt" EMU_CAMERA_STALL=2
L="$OUT/noframes.log"
check "Failed after 1.5 s without frames (between the 1.5 s and the 1.8 s marks: start 150 ms + 1500 ms)" \
  '[ -n "$(line_of $L "no frames for 1500 ms")" ] && [ "$(line_of $L "no frames for 1500 ms")" -gt "$(line_of $L "log one-and-a-half-s")" ] && [ "$(line_of $L "no frames for 1500 ms")" -lt "$(line_of $L "log after-the-watchdog")" ]'
check "no live check ran, GO did not start a countdown" '! grep -q "paper lights\|exposure locked" $L'

echo "== 20 start / stop cycles leave nothing allocated =="
{
  echo "100 upright 1"
  echo "300 tap 84 140"          # Chooser: Paper -> the stand-up guide (fresh storage)
  echo "700 tap 160 216"         # Ready -> Check (camera on)
  echo "1500 tap 24 24"          # back: camera off at once, back to the Chooser
  for i in $(seq 1 19); do t=$((1500 + i * 1000)); echo "$t tap 84 140"; echo "$((t + 700)) tap 24 24"; done
  echo "$((1500 + 20 * 1000 + 500)) quit"
} > "$OUT/cycles.txt"
run cycles chooser "$OUT/cycles.txt"
L="$OUT/cycles.log"
check "20 x camera on, 20 x camera off" '[ "$(grep -c "^\[camera on\]" $L)" = 20 ] && [ "$(grep -c "^\[camera off\]" $L)" = 20 ]'
check "every stop: camera buffers back to 0 bytes" \
  '[ "$(grep -c "camera buffers after off" $L)" = 20 ] && [ -z "$(grep "camera buffers after off" $L | grep -v " 0 bytes")" ]'
check "every start: the same amount held ($(grep -m1 -o "camera buffers [0-9]* bytes" $L))" \
  '[ "$(grep -c "^\[camera buffers [0-9]* bytes\]" $L)" = 20 ] && [ "$(grep "^\[camera buffers [0-9]* bytes\]" $L | sort -u | wc -l)" = 1 ]'

echo
echo "logs: $OUT"
echo "$PASSES passed, $FAILS failed"
[ $FAILS -eq 0 ]
