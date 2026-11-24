#!/bin/bash
# run_menus_tests.sh - scripted checks of Title, Picker, Chooser and the pause Menu in the PC emulator (src/app/game_title.cpp, game_menus.cpp).
#   cd firmware && pio run -e emu && emu/run_menus_tests.sh
# Each test runs emu/scripts/menus_*.txt against a private EMU_FS, compares the "[screen X]" log lines of the game with the
# expected flow, and checks the files under EMU_FS (slotN.scrb) where a test deletes or creates a level. Screenshots go to
# /tmp/scribble_menus/*.ppm (look at them with Pillow + the Read tool). Exit code 0 = all passed.
set -u
cd "$(dirname "$0")/.."
BIN=.pio/build/emu/program
OUT=/tmp/scribble_menus; ROOT=/tmp/scribble_menus_fs
mkdir -p "$OUT" "$ROOT"
fail=0

# fs <name> [mix] [seen]   a fresh EMU_FS with the three lessons (and: mix = filled 3 and 8, broken 5, solved 1+8, last 2; seen = standupSeen)
fs() {
  local d="$ROOT/$1"; rm -rf "$d"; mkdir -p "$d"
  echo "10 quit" > "$ROOT/init.txt"
  EMU_FS="$d" EMU_SCRIPT="$ROOT/init.txt" SDL_VIDEODRIVER=dummy $BIN > /dev/null 2>&1
  if [ "${2:-}" = mix ]; then
    cp "$d/slot1.scrb" "$d/slot3.scrb"; cp "$d/slot2.scrb" "$d/slot8.scrb"
    head -c 777 /dev/urandom > "$d/slot5.scrb"
    python3 -I -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[19]|=4; open(p,'wb').write(b)" "$d/slot8.scrb"   # slot 8 came from the paper (camera mark)
    echo '{"sound":1,"lastSlot":2,"solvedMask":258,"firstRunDone":true,"standupSeen":false}' > "$d/settings.json"
  fi
  if [ "${3:-}" = seen ] || [ "${2:-}" = seen ]; then sed -i 's/"standupSeen":false/"standupSeen":true/' "$d/settings.json"; fi
  echo "$d"
}
# screens <fsdir> <start> <script> [ENV=VAL...]   prints the screen flow, e.g. "title>picker>play"
screens() {
  local d=$1 start=$2 script=$3; shift 3
  env "$@" EMU_START="$start" EMU_FS="$d" EMU_SCRIPT="emu/scripts/$script" SDL_VIDEODRIVER=dummy $BIN 2>&1 | tee "$OUT/last_$script.log" |
    sed -n 's/^\[screen \(.*\)\]$/\1/p' | tail -n +2 | paste -sd '>'   # line 1 is begin() going to the Title
}
check() {   # check <name> <got> <want>
  if [ "$2" = "$3" ]; then echo "PASS $1: $2"; else echo "FAIL $1: got '$2' want '$3'"; fail=1; fi
}
has()  { [ -f "$1" ] && echo yes || echo no; }
last() { sed -n 's/.*"lastSlot":\([0-9]*\).*/\1/p' "$1/settings.json"; }

# ---- Title ----
d=$(fs title); check title "$(screens $d title menus_title.txt)" "title>picker>title>standup>title>draw>picker"
check title_new_level_in_first_empty_slot "$(has $d/slot3.scrb)" yes
check title_sound_cycled "$(grep -c '^\[volume' $OUT/last_menus_title.txt.log)" 2     # begin() sets it once, the chip cycles it once
d=$(fs title_play mix); check title_play "$(screens $d title menus_title_play.txt)" "title>play"; check title_play_last_slot "$(last $d)" 2
d=$(fs title_play2 mix); sed -i 's/"lastSlot":2/"lastSlot":7/' $d/settings.json     # last played slot is empty -> the first filled one
screens $d title menus_title_play.txt > /dev/null; check title_play_falls_back_to_first_level "$(last $d)" 0

# first launch: the Play tile opens the starter level in Draw (the lesson's gap cannot be crossed) until he has pressed Play inside Draw once
d=$(fs title_first); sed -i 's/"firstRunDone":true/"firstRunDone":false/' $d/settings.json
check title_first_run_play_opens_draw "$(screens $d title menus_title_first.txt)" "title>draw>play>menu>title>play"
check title_first_run_done_after_draws_play "$(grep -c '"firstRunDone":true' $d/settings.json)" 1
python3 -I - $OUT/first_noghost.ppm $OUT/first_ghost.ppm <<'PY' && echo "PASS title_first_run_ghost_finger_after_3s" || { echo "FAIL title_first_run_ghost_finger_after_3s"; fail=1; }
import sys
def ppm(p):
    b = open(p, 'rb').read(); parts = b.split(b'\n', 3); w, h = map(int, parts[1].split()); return w, parts[3]
w, a = ppm(sys.argv[1]); _, b = ppm(sys.argv[2])
diff = sum(1 for y in range(100, 136) for x in range(60, 130) if a[(y * w + x) * 3:(y * w + x) * 3 + 3] != b[(y * w + x) * 3:(y * w + x) * 3 + 3])
sys.exit(0 if diff > 100 else 1)
PY
# full box: the paper tile and the Draw tile both refuse at once
d=$(fs title_full); for i in 3 4 5 6 7 8 9; do cp $d/slot1.scrb $d/slot$i.scrb; done
check title_full_box_refuses_paper_and_draw "$(screens $d title menus_title_paper_full.txt)" "title>picker>title>picker"
check title_full_box_nothing_created "$(ls $d | grep -c 'slot.\.scrb')" 10

# ---- Picker (the MIX fixture) ----
d=$(fs picker mix)
check picker "$(screens $d picker menus_picker.txt)" "picker>chooser>draw>picker>chooser>draw>picker>play"
check picker_short_tap_keeps_level "$(has $d/slot0.scrb)" yes
check picker_early_release_keeps_level "$(has $d/slot2.scrb)" yes
# slot 3 (a copy of slot 1) was deleted by the 1.5 s hold (the chooser opened when it was tapped: only an empty slot does that), then the new level went into it
[ "$(md5sum < $d/slot3.scrb)" != "$(md5sum < $d/slot1.scrb)" ] && echo "PASS picker_hold_deleted_and_slot_reused" || { echo "FAIL picker_hold_deleted_and_slot_reused"; fail=1; }
check picker_new_level_in_tapped_slot "$(has $d/slot6.scrb)" yes
check picker_broken_slot_untouched "$(has $d/slot5.scrb)" yes
check picker_opened_slot_1 "$(last $d)" 1
# ---- Chooser ----
d=$(fs chooser); check chooser_paper_first_time "$(screens $d chooser menus_chooser_paper.txt)" "chooser>standup>chooser>picker>title"
d=$(fs chooser_f); check chooser_finger "$(screens $d chooser menus_chooser_finger.txt)" "chooser>draw"; check chooser_finger_slot "$(has $d/slot3.scrb)" yes
d=$(fs chooser_c seen); check chooser_paper_after_standup_seen "$(screens $d chooser menus_chooser_check.txt)" "chooser>check"

# ---- Menu overlay: EMU_START=menu opens it from Play on slot 0 ----
for t in resume:play back:play outside:play edit:draw levels:picker restart:play home:title; do
  n=${t%%:*}; want=${t##*:}; d=$(fs menu_$n)
  check menu_play_$n "$(screens $d menu menus_menu_$n.txt)" "play>menu>$want"
done
d=$(fs menu_shot); check menu_play_sound_cell "$(screens $d menu menus_menu_shot.txt >/dev/null; grep -c '^\[volume' $OUT/last_menus_menu_shot.txt.log)" 3
# ---- Menu from Draw ----
d=$(fs menu_draw); before=$(md5sum < $d/slot0.scrb)
check menu_draw_clear_needs_hold "$(screens $d menu menus_menu_draw.txt EMU_MENU_FROM=draw)" "play>menu>draw>picker"
[ "$before" != "$(md5sum < $d/slot0.scrb)" ] && echo "PASS menu_draw_clear_saved_empty_level" || { echo "FAIL menu_draw_clear_saved_empty_level"; fail=1; }
d=$(fs menu_dr); check menu_draw_resume "$(screens $d menu menus_menu_draw_resume.txt EMU_MENU_FROM=draw)" "play>menu>draw"
d=$(fs menu_dh); check menu_draw_home "$(screens $d menu menus_menu_draw_home.txt EMU_MENU_FROM=draw)" "play>menu>title"

[ $fail = 0 ] && echo "ALL MENUS TESTS PASSED" || echo "SOME MENUS TESTS FAILED"
exit $fail
