#!/usr/bin/env bash
# run_all.sh - every emulator test of the game, headless. Build first:  pio run -e emu   (CI does exactly this)
set -u
here=$(cd "$(dirname "$0")" && pwd); fail=0
# unit tests of ui.cpp / input.cpp / renderer.cpp on the emulated M5Unified (emu/unit/unit_main.cpp; builds env:emu_unit)
(cd "$here/.." && pio run -e emu_unit > /tmp/run_unit_build.log 2>&1 && SDL_VIDEODRIVER=dummy timeout 60 .pio/build/emu_unit/program > /tmp/run_unit.log 2>&1) \
  && echo "PASS unit ($(tail -1 /tmp/run_unit.log))" || { echo "FAIL unit"; tail -20 /tmp/run_unit_build.log /tmp/run_unit.log; fail=1; }
bash "$here/run_menus_tests.sh" > /tmp/run_menus.log 2>&1 && echo "PASS menus" || { echo "FAIL menus"; tail -20 /tmp/run_menus.log; fail=1; }
bash "$here/run_draw_tests.sh"  > /tmp/run_draw.log 2>&1  && echo "PASS draw"  || { echo "FAIL draw";  tail -20 /tmp/run_draw.log;  fail=1; }
bash "$here/paper_test.sh" > /tmp/run_paper.log 2>&1 && echo "PASS paper ($(tail -1 /tmp/run_paper.log))" || { echo "FAIL paper"; tail -20 /tmp/run_paper.log; fail=1; }
for s in "$here"/scripts/play_*.txt "$here"/scripts/e2e_*.txt "$here"/scripts/paper_*.txt; do
  [ -f "$s" ] || continue
  bash "$here/run_script.sh" "$s" > /tmp/run_one.log 2>&1 && echo "PASS $(basename "$s" .txt)" || { echo "FAIL $(basename "$s" .txt)"; tail -15 /tmp/run_one.log; fail=1; }
done
exit $fail
