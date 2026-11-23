// m5emu.cpp - implementation of the PC emulator shim (see emu/include/M5Unified.h).   MIT licensed.
#include <M5Unified.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include "config.h"

EmuSerial Serial;
EmuEsp ESP;
m5::M5Class M5;

namespace {
struct Cmd { double t; std::string op; std::string a; int x = 0, y = 0; float v = 0; bool fired = false; };
std::vector<Cmd> script;
bool haveScript = false, quit = false;
double vclockMs = 0;                       // virtual time, ms
bool scriptDown = false; int scriptX = 0, scriptY = 0; float scriptTilt = 0;
bool prevDown = false, uKeyWas = false;
bool uprightOn = false;                    // the IMU reports "standing on its base" instead of "flat on the table"
bool scriptKeyJump = false, scriptKeyBack = false;
const double FRAME_MS = 1000.0 / 60.0;

// Emulated GPIOs (ACTIVE LOW, see M5GFX Panel_sdl: a key pulls its pin low while it is down).
// M5GFX maps the arrows: up 36, right 37, down 38, left 39. We add: jump 40, back 41, toggle-upright 42.
constexpr int PIN_UP = 36, PIN_RIGHT = 37, PIN_LEFT = 39, PIN_JUMP = 40, PIN_BACK = 41, PIN_UPRIGHT = 42;
bool pinDown(int pin) { return !lgfx::gpio_in(pin); }

void loadScript() {
  const char* p = std::getenv("EMU_SCRIPT");
  if (!p || !*p) return;
  FILE* f = std::fopen(p, "r");
  if (!f) { std::fprintf(stderr, "emu: cannot open EMU_SCRIPT=%s\n", p); std::exit(2); }
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    Cmd c; char op[64] = "", a[400] = "";
    // forms:  <ms> tap x y | down x y | move x y | up | tilt g | upright 0|1 | key jump|back | keydown K | keyup K | shot file | log text | quit
    if (std::sscanf(line, "%lf %63s", &c.t, op) < 2) continue;
    c.op = op;
    if (c.op == "tap" || c.op == "down" || c.op == "move") std::sscanf(line, "%*f %*s %d %d", &c.x, &c.y);
    else if (c.op == "tilt" || c.op == "upright") std::sscanf(line, "%*f %*s %f", &c.v);
    else if (c.op == "shot" || c.op == "log" || c.op == "key" || c.op == "keydown" || c.op == "keyup") { std::sscanf(line, "%*f %*s %399s", a); c.a = a; }
    script.push_back(c);
  }
  std::fclose(f);
  std::stable_sort(script.begin(), script.end(), [](const Cmd& l, const Cmd& r) { return l.t < r.t; });
  // a tap is a down now and an up 80 ms later; `key X` is keydown now and keyup 100 ms later
  std::vector<Cmd> extra;
  for (auto& c : script) {
    if (c.op == "tap") { c.op = "down"; Cmd u; u.t = c.t + 80; u.op = "up"; extra.push_back(u); }
    else if (c.op == "key") { c.op = "keydown"; Cmd u; u.t = c.t + 100; u.op = "keyup"; u.a = c.a; extra.push_back(u); }
  }
  script.insert(script.end(), extra.begin(), extra.end());
  std::stable_sort(script.begin(), script.end(), [](const Cmd& l, const Cmd& r) { return l.t < r.t; });
  haveScript = true;
}

void runScript() {
  for (auto& c : script) {
    if (c.fired || c.t > vclockMs) continue;
    c.fired = true;
    if (c.op == "down") { scriptDown = true; scriptX = c.x; scriptY = c.y; }
    else if (c.op == "move") { scriptX = c.x; scriptY = c.y; }
    else if (c.op == "up") scriptDown = false;
    else if (c.op == "tilt") scriptTilt = c.v;
    else if (c.op == "upright") uprightOn = c.v != 0;
    else if (c.op == "keydown" || c.op == "keyup") {
      const bool on = c.op == "keydown";
      if (c.a == "jump") scriptKeyJump = on; else if (c.a == "back") scriptKeyBack = on;
      else std::fprintf(stderr, "emu: unknown key '%s' (jump|back)\n", c.a.c_str());
    }
    else if (c.op == "shot") { if (emu::writeScreenshot(c.a.c_str()) != 0) std::fprintf(stderr, "emu: screenshot %s failed\n", c.a.c_str()); else std::printf("[shot %s @ %.0f ms]\n", c.a.c_str(), vclockMs); }
    else if (c.op == "log") std::printf("[log %s @ %.0f ms]\n", c.a.c_str(), vclockMs);
    else if (c.op == "quit") quit = true;
    else std::fprintf(stderr, "emu: unknown script command '%s'\n", c.op.c_str());
  }
}
}  // namespace

unsigned long millis() { return haveScript ? (unsigned long)vclockMs : lgfx::millis(); }
unsigned long micros() { return haveScript ? (unsigned long)(vclockMs * 1000.0) : lgfx::micros(); }
void delay(unsigned long ms) { if (!haveScript) lgfx::delay(ms); }

bool emu::scriptActive() { return haveScript; }
bool emu::quitRequested() { return quit; }

bool emu::keyHeld(emu::Key k) {
  if (haveScript) return k == emu::Key::Jump ? scriptKeyJump : scriptKeyBack;
  return k == emu::Key::Jump ? (pinDown(PIN_JUMP) || pinDown(PIN_UP)) : pinDown(PIN_BACK);
}

const char* emu::fsRoot() {
  static std::string root;
  if (root.empty()) {
    const char* e = std::getenv("EMU_FS");
    root = (e && *e) ? e : "./emu_fs";
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    ::mkdir(root.c_str(), 0777);                       // harmless if it already exists
  }
  return root.c_str();
}

int emu::writeScreenshot(const char* path) {
  const int w = M5.Display.width(), h = M5.Display.height();
  std::vector<uint16_t> px(size_t(w) * h);
  M5.Display.readRect(0, 0, w, h, px.data());          // RGB565, as the panel holds it
  FILE* f = std::fopen(path, "wb");
  if (!f) return 1;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (uint16_t v : px) {
    // readRect hands back the panel's byte-swapped RGB565 (high byte first in memory)
    const uint16_t c = uint16_t((v >> 8) | (v << 8));
    const uint8_t r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
    std::fputc(r, f); std::fputc(g, f); std::fputc(b, f);
  }
  std::fclose(f);
  return 0;
}

bool m5::EmuImu::update() {
  float ax = 0;
  if (haveScript) ax = scriptTilt;
  else {
    // "right" key = right edge down = +ax
    ax = (pinDown(PIN_RIGHT) ? 1.0f : 0.0f) - (pinDown(PIN_LEFT) ? 1.0f : 0.0f);
    ax *= float(scribble::TILT_FULL);
  }
  data.accel.x = ax;
  if (uprightOn) { data.accel.y = -0.98f; data.accel.z = 0.05f; }   // standing on its base, screen facing the user
  else           { data.accel.y = 0;      data.accel.z = 0.97f; }   // flat on the table: gravity on z
  return true;
}

void m5::M5Class::begin(const config_t&) {
  loadScript();
  Display.init();
  Display.setBrightness(255);
  if (const char* u = std::getenv("EMU_UPRIGHT")) uprightOn = std::atoi(u) != 0;
  // keys beyond M5GFX's arrows: A/D tilt, W/Space jump, Escape back, U toggles flat/upright (interactive runs)
  using lgfx::Panel_sdl;
  Panel_sdl::addKeyCodeMapping(SDLK_a, PIN_LEFT);  Panel_sdl::addKeyCodeMapping(SDLK_d, PIN_RIGHT);
  Panel_sdl::addKeyCodeMapping(SDLK_w, PIN_JUMP);  Panel_sdl::addKeyCodeMapping(SDLK_SPACE, PIN_JUMP);
  Panel_sdl::addKeyCodeMapping(SDLK_ESCAPE, PIN_BACK); Panel_sdl::addKeyCodeMapping(SDLK_u, PIN_UPRIGHT);
}

void m5::M5Class::update() {
  ++frame;
  if (haveScript) { vclockMs += FRAME_MS; runScript(); }
  else { const bool u = pinDown(PIN_UPRIGHT); if (u && !uKeyWas) uprightOn = !uprightOn; uKeyWas = u; }
  touch_detail_t d;
  int tx = 0, ty = 0; bool down = false;
  if (haveScript) { down = scriptDown; tx = scriptX; ty = scriptY; }
  else { lgfx::touch_point_t tp; down = Display.getTouch(&tp) > 0; tx = tp.x; ty = tp.y; }
  d.down = down; d.x = tx; d.y = ty;
  d.pressed = down && !prevDown; d.released = !down && prevDown;
  prevDown = down;
  Touch.d = d;
}
