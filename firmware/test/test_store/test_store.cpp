// Native tests of the level store (src/app/level_store.cpp, compiled straight into this test):  pio test -e native
// Everything runs against a scratch directory under /tmp, through the same stdio code the device uses on LittleFS.
#include <unity.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../src/app/level_store.cpp"
#include "golden.h"

using namespace scribble;

static char dir[64];

static std::string P(const char* name) { return std::string(dir) + "/" + name; }
static bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v; FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return v;
  uint8_t b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  std::fclose(f); return v;
}
static void spit(const std::string& p, const void* d, size_t n) { FILE* f = std::fopen(p.c_str(), "wb"); std::fwrite(d, 1, n, f); std::fclose(f); }
static void spitStr(const std::string& p, const char* s) { spit(p, s, std::strlen(s)); }

static void wipeDir() {
  if (DIR* d = ::opendir(dir)) {
    while (dirent* e = ::readdir(d)) {
      if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
      const std::string p = P(e->d_name);
      struct stat st; if (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ::rmdir(p.c_str()); else std::remove(p.c_str());
    }
    ::closedir(d);
  }
}
void setUp() {
  std::snprintf(dir, sizeof dir, "/tmp/scribble_store_XXXXXX");
  TEST_ASSERT_NOT_NULL(::mkdtemp(dir));
}
void tearDown() { wipeDir(); ::rmdir(dir); }

static std::vector<uint8_t> encoded(const Level& l) { std::vector<uint8_t> v(MAX_FILE_BYTES); v.resize(encodeLevel(l, true, v.data(), v.size())); return v; }
static bool sameLevel(const Level& a, const Level& b) { return encoded(a) == encoded(b); }

// ---- first run ------------------------------------------------------------------------------------------------------
void test_first_run_writes_the_three_lessons() {
  LevelStore s;
  TEST_ASSERT_TRUE(s.begin(dir));
  for (int i = 0; i < 3; i++) { TEST_ASSERT_NOT_NULL(s.slots[i]); TEST_ASSERT_NOT_NULL(s.thumbs[i]); TEST_ASSERT_TRUE(exists(P(("slot" + std::to_string(i) + ".scrb").c_str()))); }
  for (int i = 3; i < SLOTS; i++) { TEST_ASSERT_NULL(s.slots[i]); TEST_ASSERT_FALSE(s.broken[i]); }
  TEST_ASSERT_EQUAL_STRING("Bridge", s.slots[0]->name);
  TEST_ASSERT_EQUAL_STRING("Spikes", s.slots[1]->name);
  TEST_ASSERT_EQUAL_STRING("Bounce", s.slots[2]->name);
  TEST_ASSERT_TRUE(s.settings.firstRunDone);
  TEST_ASSERT_EQUAL_INT(3, s.firstEmpty());
  TEST_ASSERT_TRUE(exists(P("settings.json")));
  TEST_ASSERT_FALSE(exists(P("slot0.tmp")));

  LevelStore t;                                           // a second boot loads them back, writes nothing new
  TEST_ASSERT_TRUE(t.begin(dir));
  Level want; lessonSpike(want); want.rev = 1;            // saved once: rev 1
  TEST_ASSERT_TRUE(sameLevel(want, *t.slots[1]));
  TEST_ASSERT_TRUE(t.settings.firstRunDone);
}

void test_lessons_are_not_rewritten_after_the_child_deletes_everything() {
  { LevelStore s; s.begin(dir); s.remove(0); s.remove(1); s.remove(2); }
  LevelStore t; t.begin(dir);
  for (int i = 0; i < SLOTS; i++) TEST_ASSERT_NULL(t.slots[i]);
  TEST_ASSERT_EQUAL_INT(0, t.firstEmpty());
}

void test_first_run_needs_an_empty_store() {
  spit(P("slot5.scrb"), golden::level_bridged, sizeof golden::level_bridged);   // a level exists but settings.json is lost
  LevelStore s; s.begin(dir);
  TEST_ASSERT_NOT_NULL(s.slots[5]);
  TEST_ASSERT_NULL(s.slots[0]); TEST_ASSERT_NULL(s.slots[1]); TEST_ASSERT_NULL(s.slots[2]);
}

// ---- save / load ----------------------------------------------------------------------------------------------------
void test_save_then_load_round_trips_the_exact_bytes() {
  static Level l; blankLevel(l); std::strcpy(l.name, "Rainbow Run");
  paintLine(l, MAT_SPIKE, 100, 150, 140, 150, 3); paintLine(l, MAT_BOUNCY, 160, 150, 190, 150, 3);
  LevelStore s; s.begin(dir);
  const uint32_t before = l.rev;
  TEST_ASSERT_TRUE(s.save(7, l));
  TEST_ASSERT_EQUAL_UINT32(before + 1, l.rev);            // save bumps the caller's rev
  TEST_ASSERT_NOT_NULL(s.slots[7]); TEST_ASSERT_TRUE(sameLevel(l, *s.slots[7]));

  const std::vector<uint8_t> file = slurp(P("slot7.scrb"));
  TEST_ASSERT_TRUE(sameLevel(l, l) && file == encoded(l));    // the file IS the .scrb encoding (with thumbnail)
  TEST_ASSERT_EQUAL_UINT32(64 + 1080 + 3 * 7680, file.size());
  TEST_ASSERT_FALSE(exists(P("slot7.tmp")));

  LevelStore t; t.begin(dir);
  TEST_ASSERT_NOT_NULL(t.slots[7]); TEST_ASSERT_TRUE(sameLevel(l, *t.slots[7]));
  uint8_t th[THUMB_BYTES]; makeThumbnail(l, th);
  TEST_ASSERT_EQUAL_MEMORY(th, t.thumbs[7], THUMB_BYTES);
  Level c; TEST_ASSERT_TRUE(t.copyTo(7, c)); TEST_ASSERT_TRUE(sameLevel(l, c));
  TEST_ASSERT_FALSE(t.copyTo(9, c));
  TEST_ASSERT_EQUAL_STRING("Rainbow Run", t.slots[7]->name);
}

void test_a_level_without_a_thumbnail_gets_one_on_load() {
  Level l; starterLevel(l); std::strcpy(l.name, "NoThumb");
  std::vector<uint8_t> v(MAX_FILE_BYTES); v.resize(encodeLevel(l, false, v.data(), v.size()));
  spit(P("slot2.scrb"), v.data(), v.size());
  LevelStore s; s.begin(dir);
  TEST_ASSERT_NOT_NULL(s.slots[2]); TEST_ASSERT_NOT_NULL(s.thumbs[2]);
  uint8_t th[THUMB_BYTES]; makeThumbnail(l, th);
  TEST_ASSERT_EQUAL_MEMORY(th, s.thumbs[2], THUMB_BYTES);
}

// ---- atomic writes --------------------------------------------------------------------------------------------------
void test_failed_temp_write_leaves_the_old_file_intact() {
  static Level l; blankLevel(l); std::strcpy(l.name, "first");
  LevelStore s; s.begin(dir);
  TEST_ASSERT_TRUE(s.save(4, l));
  const std::vector<uint8_t> old = slurp(P("slot4.scrb"));
  TEST_ASSERT_EQUAL_UINT32(64 + 1080 + 3 * 7680, old.size());

  TEST_ASSERT_EQUAL_INT(0, ::mkdir(P("slot4.tmp").c_str(), 0777));      // the temp path is a directory: fopen("wb") must fail (even as root)
  std::strcpy(l.name, "second"); fillRect(l, MAT_SPIKE, 10, 10, 30, 30);
  TEST_ASSERT_FALSE(s.save(4, l));
  TEST_ASSERT_TRUE(slurp(P("slot4.scrb")) == old);                       // untouched

  LevelStore t; t.begin(dir);                                            // and it still loads
  TEST_ASSERT_NOT_NULL(t.slots[4]); TEST_ASSERT_EQUAL_STRING("first", t.slots[4]->name);

  ::rmdir(P("slot4.tmp").c_str());                                       // the problem goes away: saving works again
  TEST_ASSERT_TRUE(s.save(4, l));
  LevelStore u; u.begin(dir); TEST_ASSERT_EQUAL_STRING("second", u.slots[4]->name);
}

void test_failed_save_into_an_empty_slot_creates_no_file() {
  static Level l; blankLevel(l);
  LevelStore s; s.begin(dir);
  ::mkdir(P("slot6.tmp").c_str(), 0777);
  TEST_ASSERT_FALSE(s.save(6, l));
  TEST_ASSERT_FALSE(exists(P("slot6.scrb")));
  ::rmdir(P("slot6.tmp").c_str());
}

void test_a_stale_temp_file_from_a_power_cut_is_ignored_and_removed() {
  static Level l; blankLevel(l); std::strcpy(l.name, "kept");
  { LevelStore s; s.begin(dir); s.save(8, l); }
  spitStr(P("slot8.tmp"), "half written garbage");
  LevelStore t; t.begin(dir);
  TEST_ASSERT_EQUAL_STRING("kept", t.slots[8]->name);
  TEST_ASSERT_FALSE(exists(P("slot8.tmp")));
}

void test_save_to_an_unwritable_root_returns_false() {
  static Level l; blankLevel(l);
  LevelStore s; s.begin("/tmp/scribble_no_such_dir_hopefully");
  TEST_ASSERT_FALSE(s.save(0, l));
  TEST_ASSERT_NOT_NULL(s.slots[0]);                                      // the game keeps the level in memory
}

// ---- corrupt files --------------------------------------------------------------------------------------------------
void test_corrupt_files_are_flagged_and_never_deleted() {
  std::vector<uint8_t> flipped(golden::level_bridged, golden::level_bridged + sizeof golden::level_bridged); flipped[5000] ^= 0x40;
  const std::string garbage = "this is not a level";
  spit(P("slot3.scrb"), garbage.data(), garbage.size());
  spit(P("slot6.scrb"), flipped.data(), flipped.size());
  spit(P("slot9.scrb"), golden::level_bridged, sizeof golden::level_bridged);   // one good level
  { LevelStore s; s.begin(dir);
    TEST_ASSERT_TRUE(s.broken[3]); TEST_ASSERT_TRUE(s.broken[6]); TEST_ASSERT_FALSE(s.broken[9]);
    TEST_ASSERT_NULL(s.slots[3]); TEST_ASSERT_NULL(s.slots[6]); TEST_ASSERT_NOT_NULL(s.slots[9]);
    TEST_ASSERT_NULL(s.slots[0]);                                              // not the first run: there are files
    TEST_ASSERT_EQUAL_INT(0, s.firstEmpty());
    TEST_ASSERT_FALSE(s.broken[0]); }
  TEST_ASSERT_TRUE(slurp(P("slot3.scrb")) == std::vector<uint8_t>(garbage.begin(), garbage.end()));
  TEST_ASSERT_TRUE(slurp(P("slot6.scrb")) == flipped);
  LevelStore t; t.begin(dir); TEST_ASSERT_TRUE(t.broken[3]);                   // still there on the next boot
}

void test_only_corrupt_files_still_count_as_not_first_run() {
  spitStr(P("slot1.scrb"), "garbage");
  LevelStore s; s.begin(dir);
  TEST_ASSERT_TRUE(s.broken[1]);
  TEST_ASSERT_NULL(s.slots[0]); TEST_ASSERT_NULL(s.slots[2]);                  // the lessons were NOT written next to it
  TEST_ASSERT_FALSE(s.settings.firstRunDone);
}

void test_a_slot_path_that_is_not_a_file_is_empty_not_broken() {
  // fopen() of a directory fails (or reads nothing) with an errno other than ENOENT: that must not mark the slot broken, and a
  // store whose only "slot" is such a path is still a first run (existence is decided by stat(), not by errno or broken[i])
  TEST_ASSERT_EQUAL_INT(0, ::mkdir(P("slot5.scrb").c_str(), 0777));
  LevelStore s; s.begin(dir);
  TEST_ASSERT_FALSE(s.broken[5]); TEST_ASSERT_NULL(s.slots[5]);
  TEST_ASSERT_NOT_NULL(s.slots[0]); TEST_ASSERT_NOT_NULL(s.slots[1]); TEST_ASSERT_NOT_NULL(s.slots[2]);   // the lessons
  TEST_ASSERT_TRUE(s.settings.firstRunDone);
  for (int i = 0; i < SLOTS; i++) TEST_ASSERT_FALSE(s.broken[i]);
}

void test_an_unreachable_slot_path_is_empty_not_broken() {
  // the root itself is a FILE: every slot path fails with ENOTDIR, not ENOENT. Nothing is broken; nothing can be saved.
  const std::string fileRoot = P("not_a_dir");
  spitStr(fileRoot, "x");
  LevelStore s; s.begin(fileRoot.c_str());
  for (int i = 0; i < SLOTS; i++) TEST_ASSERT_FALSE(s.broken[i]);
}

void test_oversized_files_are_broken_too() {
  std::vector<uint8_t> big(MAX_FILE_BYTES + 100, 0x55);
  spit(P("slot2.scrb"), big.data(), big.size());
  LevelStore s; s.begin(dir);
  TEST_ASSERT_TRUE(s.broken[2]); TEST_ASSERT_NULL(s.slots[2]); TEST_ASSERT_TRUE(exists(P("slot2.scrb")));
}

void test_saving_over_a_corrupt_slot_keeps_the_old_bytes_as_bad() {
  spitStr(P("slot3.scrb"), "garbage");
  static Level l; blankLevel(l);
  LevelStore s; s.begin(dir);
  TEST_ASSERT_TRUE(s.broken[3]);
  TEST_ASSERT_TRUE(s.save(3, l));
  TEST_ASSERT_FALSE(s.broken[3]);
  TEST_ASSERT_TRUE(slurp(P("slot3.bad")) == std::vector<uint8_t>({'g','a','r','b','a','g','e'}));
  LevelStore t; t.begin(dir); TEST_ASSERT_NOT_NULL(t.slots[3]); TEST_ASSERT_FALSE(t.broken[3]);
}

void test_failed_save_over_a_corrupt_slot_puts_it_back() {
  spitStr(P("slot3.scrb"), "garbage");
  static Level l; blankLevel(l);
  LevelStore s; s.begin(dir);
  ::mkdir(P("slot3.tmp").c_str(), 0777);
  TEST_ASSERT_FALSE(s.save(3, l));
  TEST_ASSERT_TRUE(slurp(P("slot3.scrb")) == std::vector<uint8_t>({'g','a','r','b','a','g','e'}));
  TEST_ASSERT_TRUE(s.broken[3]);
  ::rmdir(P("slot3.tmp").c_str());
}

// ---- remove / solved / import ---------------------------------------------------------------------------------------
void test_remove_deletes_the_file_and_clears_the_solved_bit() {
  LevelStore s; s.begin(dir);                              // lessons in 0-2
  s.markSolved(1); s.markSolved(2);
  TEST_ASSERT_TRUE(s.isSolved(1)); TEST_ASSERT_FALSE(s.isSolved(0));
  s.remove(1);
  TEST_ASSERT_NULL(s.slots[1]); TEST_ASSERT_NULL(s.thumbs[1]);
  TEST_ASSERT_FALSE(exists(P("slot1.scrb")));
  TEST_ASSERT_FALSE(s.isSolved(1)); TEST_ASSERT_TRUE(s.isSolved(2));
  TEST_ASSERT_EQUAL_INT(1, s.firstEmpty());
  LevelStore t; t.begin(dir);                              // and it is persisted
  TEST_ASSERT_NULL(t.slots[1]); TEST_ASSERT_FALSE(t.isSolved(1)); TEST_ASSERT_TRUE(t.isSolved(2));
  s.remove(9); s.remove(-1); s.remove(99);                 // no crash on empty / out-of-range slots
}

void test_removing_a_corrupt_slot_keeps_the_bytes() {
  spitStr(P("slot4.scrb"), "garbage");
  LevelStore s; s.begin(dir); TEST_ASSERT_TRUE(s.broken[4]);
  s.remove(4);
  TEST_ASSERT_FALSE(s.broken[4]); TEST_ASSERT_FALSE(exists(P("slot4.scrb"))); TEST_ASSERT_TRUE(exists(P("slot4.bad")));
}

void test_import_puts_a_file_into_the_first_empty_slot() {
  LevelStore s; s.begin(dir);                              // 0-2 taken
  const int a = s.importBytes(golden::level_bridged, sizeof golden::level_bridged);
  TEST_ASSERT_EQUAL_INT(3, a);
  TEST_ASSERT_EQUAL_STRING("Bridged", s.slots[3]->name);
  TEST_ASSERT_EQUAL_INT(8, s.importBytes(golden::level_lesson2_spikes, sizeof golden::level_lesson2_spikes, 8));   // explicit slot
  TEST_ASSERT_TRUE(exists(P("slot8.scrb")));
  uint8_t bad[100]; std::memset(bad, 1, sizeof bad);
  TEST_ASSERT_EQUAL_INT(-1, s.importBytes(bad, sizeof bad));                                                      // not a level
  TEST_ASSERT_EQUAL_INT(-1, s.importBytes(golden::level_bridged, 5000));                                          // truncated
  TEST_ASSERT_EQUAL_INT(4, s.firstEmpty());
}

void test_import_with_no_free_slot_overwrites_nothing() {
  LevelStore s; s.begin(dir);
  for (int i = 3; i < SLOTS; i++) TEST_ASSERT_EQUAL_INT(i, s.importBytes(golden::level_lesson3_bounce, sizeof golden::level_lesson3_bounce));
  TEST_ASSERT_EQUAL_INT(-1, s.firstEmpty());
  const std::vector<uint8_t> before = slurp(P("slot0.scrb"));
  TEST_ASSERT_EQUAL_INT(-1, s.importBytes(golden::level_bridged, sizeof golden::level_bridged));
  TEST_ASSERT_TRUE(slurp(P("slot0.scrb")) == before);
  TEST_ASSERT_EQUAL_STRING("Bridge", s.slots[0]->name);
}

// ---- settings -------------------------------------------------------------------------------------------------------
void test_settings_round_trip() {
  { LevelStore s; s.begin(dir);
    s.settings.sound = 2; s.settings.lastSlot = 6; s.settings.standupSeen = true; s.saveSettings();
    s.markSolved(0); s.markSolved(9); }
  const std::vector<uint8_t> raw = slurp(P("settings.json"));
  const std::string text(raw.begin(), raw.end());
  TEST_ASSERT_NOT_NULL(std::strstr(text.c_str(), "\"sound\":2"));
  TEST_ASSERT_NOT_NULL(std::strstr(text.c_str(), "\"solvedMask\":513"));
  TEST_ASSERT_NOT_NULL(std::strstr(text.c_str(), "\"standupSeen\":true"));
  LevelStore t; t.begin(dir);
  TEST_ASSERT_EQUAL_INT(2, t.settings.sound); TEST_ASSERT_EQUAL_INT(6, t.settings.lastSlot);
  TEST_ASSERT_EQUAL_UINT32(513, t.settings.solvedMask);
  TEST_ASSERT_TRUE(t.settings.firstRunDone); TEST_ASSERT_TRUE(t.settings.standupSeen);
}

void test_settings_parser_is_tolerant() {
  spitStr(P("settings.json"), "{ \"extra\" : [1,2], \"lastSlot\" :\n 4 , \"sound\":7, \"solvedMask\": 4294967295,\"standupSeen\" : true }");
  LevelStore s; s.begin(dir);
  TEST_ASSERT_EQUAL_INT(2, s.settings.sound);                          // clamped to 0..2
  TEST_ASSERT_EQUAL_INT(4, s.settings.lastSlot);
  TEST_ASSERT_EQUAL_UINT32((1u << SLOTS) - 1, s.settings.solvedMask);   // only ten bits
  TEST_ASSERT_TRUE(s.settings.standupSeen);
  TEST_ASSERT_TRUE(s.settings.firstRunDone);                           // key missing and the store empty: the first run happened now
}

void test_corrupt_settings_mean_defaults() {
  spitStr(P("settings.json"), "\x01\x02 not json {{{");
  LevelStore s; s.begin(dir);
  TEST_ASSERT_EQUAL_INT(1, s.settings.sound); TEST_ASSERT_EQUAL_INT(0, s.settings.lastSlot);
  TEST_ASSERT_EQUAL_UINT32(0, s.settings.solvedMask);
}

// ---- helpers --------------------------------------------------------------------------------------------------------
void test_blank_level_is_playable_and_valid() {
  static Level l, m; blankLevel(l);
  TEST_ASSERT_EQUAL_INT(40, l.startX); TEST_ASSERT_EQUAL_INT(160, l.startY);
  TEST_ASSERT_EQUAL_INT(280, l.flagX); TEST_ASSERT_EQUAL_INT(168, l.flagY);
  TEST_ASSERT_EQUAL_INT(1, materialAt(l, 100, 170));   // 1 + MAT_GROUND: a floor under the ball
  TEST_ASSERT_EQUAL_INT(0, materialAt(l, 100, 150));
  TEST_ASSERT_EQUAL_INT(0, materialAt(l, 100, 176));
  const std::vector<uint8_t> v = encoded(l);
  TEST_ASSERT_EQUAL_INT(LEVEL_OK, decodeLevel(v.data(), v.size(), m, nullptr));
  TEST_ASSERT_TRUE(sameLevel(l, m));
}

void test_slot_numbers_out_of_range_are_harmless() {
  static Level l; blankLevel(l);
  LevelStore s; s.begin(dir);
  TEST_ASSERT_FALSE(s.save(-1, l)); TEST_ASSERT_FALSE(s.save(SLOTS, l));
  s.markSolved(-3); s.markSolved(77);
  TEST_ASSERT_EQUAL_UINT32(0, s.settings.solvedMask);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_first_run_writes_the_three_lessons);
  RUN_TEST(test_lessons_are_not_rewritten_after_the_child_deletes_everything);
  RUN_TEST(test_first_run_needs_an_empty_store);
  RUN_TEST(test_save_then_load_round_trips_the_exact_bytes);
  RUN_TEST(test_a_level_without_a_thumbnail_gets_one_on_load);
  RUN_TEST(test_failed_temp_write_leaves_the_old_file_intact);
  RUN_TEST(test_failed_save_into_an_empty_slot_creates_no_file);
  RUN_TEST(test_a_stale_temp_file_from_a_power_cut_is_ignored_and_removed);
  RUN_TEST(test_save_to_an_unwritable_root_returns_false);
  RUN_TEST(test_corrupt_files_are_flagged_and_never_deleted);
  RUN_TEST(test_only_corrupt_files_still_count_as_not_first_run);
  RUN_TEST(test_a_slot_path_that_is_not_a_file_is_empty_not_broken);
  RUN_TEST(test_an_unreachable_slot_path_is_empty_not_broken);
  RUN_TEST(test_oversized_files_are_broken_too);
  RUN_TEST(test_saving_over_a_corrupt_slot_keeps_the_old_bytes_as_bad);
  RUN_TEST(test_failed_save_over_a_corrupt_slot_puts_it_back);
  RUN_TEST(test_remove_deletes_the_file_and_clears_the_solved_bit);
  RUN_TEST(test_removing_a_corrupt_slot_keeps_the_bytes);
  RUN_TEST(test_import_puts_a_file_into_the_first_empty_slot);
  RUN_TEST(test_import_with_no_free_slot_overwrites_nothing);
  RUN_TEST(test_settings_round_trip);
  RUN_TEST(test_settings_parser_is_tolerant);
  RUN_TEST(test_corrupt_settings_mean_defaults);
  RUN_TEST(test_blank_level_is_playable_and_valid);
  RUN_TEST(test_slot_numbers_out_of_range_are_harmless);
  return UNITY_END();
}
