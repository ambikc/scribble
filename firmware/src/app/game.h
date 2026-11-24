// game.h - the state machine and every screen.   Port of sim/src/modes.ts.   (app layer; the only module that knows all the others)
// States and transitions: docs/system-design.md section 3; screens: docs/screens.md.
//
//   title -> picker | chooser | standup | check | draw | play
//   check -> countdown -> processing -> review -> play        draw <-> play -> win        menu overlays draw/play
//
// FILE OWNERSHIP (so parallel work never collides). Every screen has tickX(in) and drawX(); a screen's two functions and its
// state struct live in its own files:
//     game_core.cpp    go/say/chip/bar/topBar/particles/openLevel/startPlay/goBack, tick() and draw() dispatch   (Core agent)
//     game_title.cpp   title                                                                                     (Menus agent)
//     game_play.cpp    play + win          state: game_play.h                                                     (Play agent)
//     game_draw.cpp    draw / edit         state: game_draw.h                                                     (Draw agent)
//     game_menus.cpp   picker, chooser, menu overlay    state: game_menus.h                                      (Menus agent)
//     game_paper.cpp   standup, check, countdown, processing, review   state: game_paper.h                       (Paper agent)
//     game_stubs.cpp   (removed: every screen is real now) weak do-nothing versions of every tickX/drawX, so the project always links. Real code OVERRIDES them;
//                      delete the stub when you add the real function.
// Shared data (level, slot, toast, particles) lives here in Game and is owned by the Core agent; ask before changing it.
#pragma once
#include <cstdint>
#include "config.h"
#include "level.h"
#include "input.h"
#include "renderer.h"
#include "ui.h"
#include "audio.h"
#include "level_store.h"
#include "game_play.h"
#include "game_draw.h"
#include "game_menus.h"
#include "game_paper.h"

namespace scribble {

class Camera;   // paper agent: camera.h (esp32-camera on the device, a test-photo loader in the emulator)

enum class Screen : uint8_t { Title, Picker, Chooser, Standup, Check, Countdown, Processing, Review, Draw, Play, Win, Menu };

struct Particle { float x, y, vx, vy, life, max; uint16_t color; uint8_t size; uint8_t kind; };   // kind: 0 square, 1 circle, 2 ring, 3 confetti
constexpr int MAX_PARTICLES = 96;

class Game {
 public:
  Game(Renderer& r, UI& ui, Audio& audio, LevelStore& store, Input& input, Camera* cam = nullptr)
      : r(r), ui(ui), audio(audio), store(store), input(input), cam(cam) {}
  void begin();                                      // first-run setup; starts on the Title screen
  void tick(const InputState& in);                   // one 60 Hz tick
  void draw();                                       // paint the current screen into the Renderer (main() then calls r.present())

  // ---- shared services, available to every screen --------------------------------------------------------
  Renderer& r; UI& ui; Audio& audio; LevelStore& store; Input& input; Camera* cam;
  Screen screen = Screen::Title;
  float screenT = 0;                                 // seconds in this screen (the first tick after go() sees one tick, ~0.017)
  double t = 0;                                      // seconds for idle animations (title ball, win rays, pulses); a double that wraps
                                                     // every T_WRAP s, so it never stops advancing or gets steppy (a float did after hours)
  static constexpr double T_WRAP = 3600.0;           // multiple of 2 s and of 1/0.22 s (the title's fmod periods): one small jump an hour at most
  uint32_t nowMs = 0;                                // millis() at the start of this tick
  Screen menuFrom = Screen::Draw;                    // which screen the menu overlay returns to (Draw or Play)
  Screen cameFrom = Screen::Title;                   // where "back" in the camera flow goes

  Level level;                                       // the level being played / edited
  int slot = 0;
  bool dirtySave = false; uint32_t lastEditAt = 0;   // saveCurrent() clears dirtySave only when the write worked (else it retries later)
  uint32_t saveFailSaidAt = 0; bool saveFailSaid = false;   // "Could not save" toast, at most every SAVE_FAIL_TOAST_MS
  bool levelIsStarter = false;

  PlayState play; DrawState drawS; MenusState menus; PaperState paper;

  // ---- core helpers (game_core.cpp) ---------------------------------------------------------------------
  void go(Screen s);                                 // switch screen: resets screenT, starts the 0.15 s paper fade-in
  void say(const char* text);                        // toast for ~2.2 s
  void chip(const char* id, Icon icon, int x, const Btn* extra = nullptr);
  void chipBack(const Btn* extra = nullptr, Icon icon = Icon::Back);
  void chipMenu();
  void bar(const Btn* items, const int* widths, int n);   // lay tabs left to right along the bottom bar; widths add to 320
  void topBar(Icon titleIcon, const char* rightId = nullptr, Icon rightIcon = Icon::None, bool rightActive = false);
  Icon soundIcon() const;
  void cycleSound();
  void spawn(int n, float x, float y, const uint16_t* colors, int nColors, float speed, float life, int size = 2, int kind = 0);
  void updateFx(float dt);                           // particles + toast + transition + the play fx timers (playFxStep); tick() calls it
  void playFxStep(float dt);                         // weak default: decays the PlayState fx timers; the Play agent may define its own
  void drawParts();                                  // idempotent per frame: draw() calls it after the screen unless the screen already did
  void saveCurrent();                                // write the level if dirtySave; on failure keep it dirty, toast (rate limited), retry later
  void markEdited();
  void openLevel(int slotIndex, bool play);          // load from the store; play ? startPlay : go(Draw)
  void newInSlot(int slotIndex);
  void startPlay();
  void goBack();                                     // "up one step" for the emulator's Esc key (the device uses the back chip)
  void leaveCamera();                                // back out of the paper flow: to the Chooser if it came from there, else the Title
  void flash(uint16_t color, float seconds);         // full-screen flash that fades out (the shutter): drawn over every screen but Play (which draws its own)
  const Level& currentLevel() const { return level; }
  uint32_t levelSerial = 0;                          // +1 every time openLevel() loads a level: Draw resets its undo/ghost state when it changes

  Particle parts[MAX_PARTICLES]; int nParts = 0;
  struct Toast { char text[48] = ""; float t = 0; } toast;
  float transition = 0;
  bool partsDrawn = false;                           // set by drawParts(), cleared at the start of draw()

  // ---- screens: implemented in the files listed at the top -----------------------------------------------
  void tickTitle(const InputState&);      void drawTitle();
  void tickPicker(const InputState&);     void drawPicker();
  void tickChooser(const InputState&);    void drawChooser();
  void tickStandup(const InputState&);    void drawStandup();
  void tickCheck(const InputState&);      void drawCheck();
  void tickCountdown(const InputState&);  void drawCountdown();
  void tickProcessing(const InputState&); void drawProcessing();
  void tickReview(const InputState&);     void drawReview();
  void tickDraw(const InputState&);       void drawDraw();
  void tickPlay(const InputState&);       void drawPlay();
  void tickWin(const InputState&);        void drawWin();
  void tickMenu(const InputState&);       void drawMenu();
};

}  // namespace scribble
