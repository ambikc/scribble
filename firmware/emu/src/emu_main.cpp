// emu_main.cpp - main() for the PC emulator: runs the firmware's setup()/loop() under M5GFX's SDL window.   MIT licensed.
#include <M5Unified.h>
#include <cstdio>
#include <cstdlib>

void setup();
void loop();

static int user_func(bool* running) {
  setup();
  do {
    loop();
    if (emu::quitRequested()) { std::fflush(stdout); std::_Exit(0); }   // Panel_sdl::main waits for the window; end the process
  } while (*running);
  return 0;
}

int main(int, char**) {
  return lgfx::Panel_sdl::main(user_func, 0);
}
