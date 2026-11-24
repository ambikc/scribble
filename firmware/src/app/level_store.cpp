// level_store.cpp - the ten level slots, settings and progress, on plain stdio.   Port of sim/src/level_store.ts.
// Needs only C++17, stdio/POSIX and lib/core, so the native unit tests (test/test_store) compile this file directly.
#include "level_store.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif

namespace scribble {

// ---- memory: big buffers go to PSRAM on the device, plain malloc everywhere else -----------------------------------------
static void* bigAlloc(size_t n) {
#if defined(ARDUINO) && defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // what ps_malloc() does
  if (p) return p;                                                       // no PSRAM (or full): fall back to internal RAM
#endif
  return std::malloc(n);
}

void blankLevel(Level& out) {
  levelInit(out);
  out.name[0] = 0;
  fillRect(out, MAT_GROUND, 0, 168, WORLD_W, 176);
  out.startX = 40; out.startY = 160; out.flagX = 280; out.flagY = 168;
}

// ---- file helpers ------------------------------------------------------------------------------------------------
static void slotPath(char* out, size_t cap, const char* root, int slot, const char* ext) { std::snprintf(out, cap, "%s/slot%d.%s", root, slot, ext); }

/** Is there a regular file at `path`? Decided by stat(), never by an errno guess: a path that is missing, a directory, or not
 *  reachable at all (ENOTDIR, ENAMETOOLONG, ...) holds no file. */
static bool fileExists(const char* path) {
  struct stat st;
  return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/** The whole file into a malloc'd buffer. nullptr + *missing=true if there is no regular file there (fileExists); nullptr +
 *  *missing=false if the file exists but could not be read whole (an error, too large, out of memory). */
static uint8_t* readFile(const char* path, size_t maxBytes, size_t* n, bool* missing) {
  *n = 0; *missing = false;
  if (!fileExists(path)) { *missing = true; return nullptr; }
  FILE* f = std::fopen(path, "rb");
  if (!f) return nullptr;
  uint8_t* buf = nullptr;
  if (std::fseek(f, 0, SEEK_END) == 0) {
    const long sz = std::ftell(f);
    if (sz >= 0 && size_t(sz) <= maxBytes && std::fseek(f, 0, SEEK_SET) == 0) {
      buf = static_cast<uint8_t*>(bigAlloc(size_t(sz) + 1));
      if (buf) {
        const size_t got = sz ? std::fread(buf, 1, size_t(sz), f) : 0;
        if (got != size_t(sz)) { std::free(buf); buf = nullptr; } else *n = got;
      }
    } else if (sz >= 0) { *n = size_t(sz); }      // too big: report the size, no buffer (caller sees buf == nullptr, n > 0)
  }
  std::fclose(f);
  return buf;
}

/** Write `data` to <path minus extension>.tmp, flush it to the medium, then rename over `path`. The old file is never
 *  touched until the new one is complete (docs/system-design.md 4.4), so losing power mid-save keeps the old level. */
bool LevelStore::writeAtomic(const char* path, const uint8_t* data, size_t n) {
  char tmp[sizeof root_ + 24];
  std::snprintf(tmp, sizeof tmp, "%s", path);
  char* dot = std::strrchr(tmp, '.'); char* slash = std::strrchr(tmp, '/');
  if (dot && (!slash || dot > slash)) *dot = 0;
  std::strncat(tmp, ".tmp", sizeof tmp - std::strlen(tmp) - 1);

  FILE* f = std::fopen(tmp, "wb");
  if (!f) return false;
  bool ok = std::fwrite(data, 1, n, f) == n;
  ok = (std::fflush(f) == 0) && ok;
  if (ok && ::fsync(::fileno(f)) != 0 && errno != ENOSYS && errno != EINVAL) ok = false;
  ok = (std::fclose(f) == 0) && ok;
  if (ok && std::rename(tmp, path) != 0) ok = false;
  if (!ok) std::remove(tmp);                      // never leave a half-written temp file behind
  return ok;
}

// ---- tiny settings.json reader / writer ------------------------------------------------------------------------------
// {"sound":1,"lastSlot":0,"solvedMask":0,"firstRunDone":false,"standupSeen":false}
static bool jsonFind(const char* json, const char* key, const char** valueStart) {
  char pat[40]; std::snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = std::strstr(json, pat);
  if (!p) return false;
  p += std::strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p != ':') return false;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  *valueStart = p;
  return true;
}
static bool jsonInt(const char* json, const char* key, long long* out) {
  const char* v; if (!jsonFind(json, key, &v)) return false;
  char* end; const long long x = std::strtoll(v, &end, 10);
  if (end == v) return false;
  *out = x; return true;
}
static bool jsonBool(const char* json, const char* key, bool* out) {
  const char* v; if (!jsonFind(json, key, &v)) return false;
  if (!std::strncmp(v, "true", 4)) { *out = true; return true; }
  if (!std::strncmp(v, "false", 5)) { *out = false; return true; }
  long long x; if (jsonInt(json, key, &x)) { *out = x != 0; return true; }      // tolerate 0 / 1
  return false;
}

static void parseSettings(const char* json, Settings& s) {
  long long v; bool b;
  if (jsonInt(json, "sound", &v)) s.sound = int(v < 0 ? 0 : v > 2 ? 2 : v);
  if (jsonInt(json, "lastSlot", &v)) s.lastSlot = int(v < 0 ? 0 : v >= SLOTS ? SLOTS - 1 : v);
  if (jsonInt(json, "solvedMask", &v)) s.solvedMask = uint32_t(v) & ((1u << SLOTS) - 1);
  if (jsonBool(json, "firstRunDone", &b)) s.firstRunDone = b;
  if (jsonBool(json, "standupSeen", &b)) s.standupSeen = b;
  if (jsonBool(json, "lessonsInstalled", &b)) s.lessonsInstalled = b;
  else s.lessonsInstalled = s.firstRunDone;                            // older settings files: the lessons went in with the first run
}
static int formatSettings(char* out, size_t cap, const Settings& s) {
  return std::snprintf(out, cap, "{\"sound\":%d,\"lastSlot\":%d,\"solvedMask\":%lu,\"firstRunDone\":%s,\"standupSeen\":%s,\"lessonsInstalled\":%s}\n",
                       s.sound, s.lastSlot, static_cast<unsigned long>(s.solvedMask), s.firstRunDone ? "true" : "false", s.standupSeen ? "true" : "false", s.lessonsInstalled ? "true" : "false");
}

// ---- the store -----------------------------------------------------------------------------------------------------
LevelStore::~LevelStore() { freeAll(); }

void LevelStore::freeAll() {
  for (int i = 0; i < SLOTS; i++) {
    std::free(slots[i]); slots[i] = nullptr;
    std::free(thumbs[i]); thumbs[i] = nullptr;
    broken[i] = false;
  }
}

Level* LevelStore::ensureSlot(int i) {
  if (!slots[i]) slots[i] = static_cast<Level*>(bigAlloc(sizeof(Level)));
  if (!thumbs[i]) thumbs[i] = static_cast<uint8_t*>(std::malloc(THUMB_BYTES));
  if (!slots[i] || !thumbs[i]) { std::free(slots[i]); std::free(thumbs[i]); slots[i] = nullptr; thumbs[i] = nullptr; return nullptr; }
  return slots[i];
}

void LevelStore::clearSlot(int i) { std::free(slots[i]); slots[i] = nullptr; std::free(thumbs[i]); thumbs[i] = nullptr; }

/** Load slotN.scrb into memory. No file (stat) = empty slot. A file that exists but is unreadable or invalid = empty slot + broken[i];
 *  the file is kept. Returns true when a slot FILE exists (good or broken), which is what decides "first run". */
bool LevelStore::loadSlot(int i) {
  clearSlot(i); broken[i] = false;
  char path[sizeof root_ + 24]; slotPath(path, sizeof path, root_, i, "scrb");
  size_t n; bool missing;
  uint8_t* bytes = readFile(path, MAX_FILE_BYTES, &n, &missing);
  if (!bytes) { if (!missing) broken[i] = true; return !missing; }   // unreadable or too large: warn, keep the file
  Level* l = ensureSlot(i);
  bool hasThumb = false;
  if (!l) { std::free(bytes); return true; }                         // out of memory: leave the slot empty (not "broken")
  if (decodeLevel(bytes, n, *l, thumbs[i], &hasThumb) != LEVEL_OK) { clearSlot(i); broken[i] = true; std::free(bytes); return true; }
  if (!hasThumb) makeThumbnail(*l, thumbs[i]);
  std::free(bytes);
  return true;
}

bool LevelStore::begin(const char* root) {
  freeAll();
  settings = Settings();
  std::snprintf(root_, sizeof root_, "%s", root ? root : "");
  size_t rl = std::strlen(root_);
  while (rl > 1 && root_[rl - 1] == '/') root_[--rl] = 0;           // no trailing slash
  if (rl == 0 || rl + 24 > sizeof root_) return false;
  DIR* d = ::opendir(root_);
  const bool rootOk = d != nullptr;
  if (d) ::closedir(d);

  // settings (a corrupt or missing file just means the defaults)
  {
    char path[sizeof root_ + 24]; std::snprintf(path, sizeof path, "%s/settings.json", root_);
    size_t n; bool missing; uint8_t* b = readFile(path, 4096, &n, &missing);
    if (b) { b[n] = 0; parseSettings(reinterpret_cast<const char*>(b), settings); std::free(b); }
  }

  // First run = no slot FILE exists at all (stat) and settings say it never happened. Decided by the files, not by broken[i] or by
  // an errno: a store holding only corrupt files is not a first run (they are the child's levels), and a path that is not a regular
  // file (a directory, an unreachable path) holds no level.
  bool anyFile = false;
  for (int i = 0; i < SLOTS; i++) {
    char tmp[sizeof root_ + 24]; slotPath(tmp, sizeof tmp, root_, i, "tmp"); std::remove(tmp);   // a save that never finished
    if (loadSlot(i)) anyFile = true;
  }
  if (!anyFile && !settings.lessonsInstalled) {                              // factory lessons in slots 0-2 (docs/game-design.md section 2)
    void (*const lessons[3])(Level&) = { starterLevel, lessonSpike, lessonBouncy };
    for (int i = 0; i < 3; i++) {
      Level* l = ensureSlot(i);
      if (!l) break;
      lessons[i](*l);
      save(i, *l);
    }
    settings.lessonsInstalled = true;                                   // NOT firstRunDone: that becomes true when he presses Play inside Draw
    saveSettings();
  }
  return rootOk;
}

bool LevelStore::save(int slot, Level& level) {
  if (slot < 0 || slot >= SLOTS) return false;
  level.rev += 1;                                                    // the caller's copy carries the new revision too
  bool ok = false;
  if (uint8_t* buf = static_cast<uint8_t*>(bigAlloc(MAX_FILE_BYTES))) {
    const size_t n = encodeLevel(level, true, buf, MAX_FILE_BYTES);
    if (n) {
      char path[sizeof root_ + 24], bad[sizeof root_ + 24];
      slotPath(path, sizeof path, root_, slot, "scrb"); slotPath(bad, sizeof bad, root_, slot, "bad");
      // A corrupt file is never deleted: the new level replaces it only after the bytes are moved aside to slotN.bad.
      const bool aside = broken[slot] && std::rename(path, bad) == 0;
      ok = writeAtomic(path, buf, n);
      if (aside && !ok) std::rename(bad, path);                      // the write failed: put the corrupt file back
    }
    std::free(buf);
  }
  // The in-memory copy follows the caller even when the write failed (the child keeps what he drew; the old file stays on flash).
  if (Level* l = ensureSlot(slot)) {
    if (l != &level) std::memcpy(static_cast<void*>(l), &level, sizeof(Level));
    makeThumbnail(*l, thumbs[slot]);
    if (ok) broken[slot] = false;
  }
  return ok;
}

void LevelStore::remove(int slot) {
  if (slot < 0 || slot >= SLOTS) return;
  char path[sizeof root_ + 24], bad[sizeof root_ + 24], tmp[sizeof root_ + 24];
  slotPath(path, sizeof path, root_, slot, "scrb"); slotPath(bad, sizeof bad, root_, slot, "bad"); slotPath(tmp, sizeof tmp, root_, slot, "tmp");
  if (broken[slot]) std::rename(path, bad);                          // a corrupt file is kept, as slotN.bad
  else std::remove(path);
  std::remove(tmp);
  clearSlot(slot); broken[slot] = false;
  settings.solvedMask &= ~(1u << slot);
  saveSettings();
}

int LevelStore::firstEmpty() const {
  for (int i = 0; i < SLOTS; i++) if (!slots[i]) return i;
  return -1;
}

void LevelStore::markSolved(int slot) {
  if (slot < 0 || slot >= SLOTS) return;
  settings.solvedMask |= 1u << slot;
  saveSettings();
}

void LevelStore::saveSettings() {
  char json[256]; const int n = formatSettings(json, sizeof json, settings);
  char path[sizeof root_ + 24]; std::snprintf(path, sizeof path, "%s/settings.json", root_);
  if (n > 0 && size_t(n) < sizeof json) writeAtomic(path, reinterpret_cast<const uint8_t*>(json), size_t(n));
}

int LevelStore::importBytes(const uint8_t* bytes, size_t n, int slot) {
  if (!bytes) return -1;
  Level* l = static_cast<Level*>(bigAlloc(sizeof(Level)));
  if (!l) return -1;
  int result = -1;
  if (decodeLevel(bytes, n, *l, nullptr) == LEVEL_OK) {
    if (slot < 0) slot = firstEmpty();                               // all full: refuse rather than overwrite something
    if (slot >= 0 && slot < SLOTS) {
      const bool wasEmpty = slots[slot] == nullptr;
      if (save(slot, *l)) result = slot;
      else if (wasEmpty) clearSlot(slot);                            // nothing reached flash: do not show a phantom level
    }
  }
  std::free(l);
  return result;
}

bool LevelStore::copyTo(int slot, Level& out) const {
  if (slot < 0 || slot >= SLOTS || !slots[slot]) return false;
  std::memcpy(static_cast<void*>(&out), slots[slot], sizeof(Level));
  return true;
}

}  // namespace scribble
