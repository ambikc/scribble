#!/usr/bin/env bash
# run_script.sh - run one emulator script headless in a private storage folder and check its output.
#
#   emu/run_script.sh emu/scripts/play_win1.txt            (needs `pio run -e emu` first; any working directory)
#
# The first lines of the script may carry directives (all are ordinary '#' comments to the emulator):
#   # start: play            EMU_START (title|picker|chooser|standup|check|countdown|processing|review|play|draw|win|menu)
#   # slot: 3                EMU_SLOT
#   # levels: 3=bridge 4=hop test levels written into the folder (emu/mk_level.py) after the three lessons were created
#   # mkdir: slot5.tmp/x     directories created in the folder after that (e.g. a directory in the way of slot 5's temp file: saves fail)
#   # expect: [win slot 3]   a fixed string that must appear in the output (several allowed)
#   # expect-not: [sfx jump] a fixed string that must NOT appear
#   # expect-count: 2 [sfx pop]   the string must appear exactly that many times
# Output goes to $OUT (default /tmp/scribble_run/<script name>/): run.log, and the screenshots of the script: a `shot OUT/a.ppm` line
# is rewritten to that folder. Exit code 0 = every expectation met.
set -u
script=${1:?usage: run_script.sh <script.txt>}
here=$(cd "$(dirname "$0")" && pwd)
fw=$(dirname "$here")
prog=$fw/.pio/build/emu/program
[ -x "$prog" ] || { echo "build first: (cd $fw && pio run -e emu)"; exit 2; }
name=$(basename "$script" .txt)
out=${OUT:-/tmp/scribble_run/$name}
rm -rf "$out"; mkdir -p "$out/fs"
get() { grep -m1 -E "^# $1:" "$script" | sed -E "s/^# $1:[[:space:]]*//"; }

# the first run creates the three lessons (slots 0-2); then the extra test levels are added
printf '10 quit\n' > "$out/boot.txt"
EMU_FS="$out/fs" EMU_SCRIPT="$out/boot.txt" SDL_VIDEODRIVER=dummy "$prog" > /dev/null 2>&1
levels=$(get levels)
if [ -n "$levels" ]; then python3 "$here/mk_level.py" "$out/fs" $levels || exit 2; fi
for d in $(get mkdir); do mkdir -p "$out/fs/$d"; done

sed -E "s#^([0-9]+ shot )OUT/#\1$out/#" "$script" > "$out/script.txt"
start=$(get start); slot=$(get slot)
if [ -n "$start" ]; then export EMU_START=$start EMU_SLOT=${slot:-0}; fi
EMU_FS="$out/fs" EMU_SCRIPT="$out/script.txt" SDL_VIDEODRIVER=dummy timeout 120 "$prog" > "$out/run.log" 2>&1
rc=$?
fail=0
[ $rc -eq 0 ] || { echo "FAIL $name: program exit code $rc"; fail=1; }
while IFS= read -r pat; do
  [ -z "$pat" ] && continue
  grep -qF -- "$pat" "$out/run.log" || { echo "FAIL $name: missing  $pat"; fail=1; }
done < <(grep -E "^# expect:" "$script" | sed -E 's/^# expect:[[:space:]]*//')
while IFS= read -r pat; do
  [ -z "$pat" ] && continue
  if grep -qF -- "$pat" "$out/run.log"; then echo "FAIL $name: unexpected  $pat"; fail=1; fi
done < <(grep -E "^# expect-not:" "$script" | sed -E 's/^# expect-not:[[:space:]]*//')
while IFS= read -r line; do
  [ -z "$line" ] && continue
  n=${line%% *}; pat=${line#* }
  got=$(grep -cF -- "$pat" "$out/run.log")
  [ "$got" -eq "$n" ] || { echo "FAIL $name: expected $n x  $pat  got $got"; fail=1; }
done < <(grep -E "^# expect-count:" "$script" | sed -E 's/^# expect-count:[[:space:]]*//')
[ $fail -eq 0 ] && echo "PASS $name   (log: $out/run.log)"
exit $fail
