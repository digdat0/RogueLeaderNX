// The game's GX command stream rendered with Aurora (gx_bridge.cpp).
#pragma once
#include <string>

#include "../ctx.h"

namespace rt::gfx {
// window + renderer; registers the GX command sink. disc: the image the game starts with (""
// if none was found); without a usable one the launcher asks for it and disc is its choice.
// discNote: why there is none (shown on the launcher's disc page).
bool init(int argc, char** argv, std::string& disc, const std::string& discNote);
void vblank();                     // once per emulated retrace (keeps the window responsive)
u64 frames();                      // frames presented
std::string stats();               // Aurora's per-frame statistics
// RL-NX: how many draw calls the guest has asked Aurora for, counted since boot. Unchanged for
// whole seconds it means the guest has stopped submitting - a black screen, not a slow frame.
u64 drawCalls();

// menu.cpp: the settings menu (F1 or the controller's Back/Select button)
void menuEvent(const void* sdlEvent);  // every SDL event (opens/closes the menu)
// RL-NX: the - menu's switch for the per-frame counters (one [stats] line per 60 frames), the same switch
// as R2_STATS=1 without the restart.
void setStatsEnabled(bool on);
// RL-NX (9.53): the - menu's GX THREAD row - GX commands decoded on the bridge worker (gx_bridge.cpp)
void setBridgeThread(bool on);
bool bridgeThreadOn();
// RL-NX (9.57): the - menu's GP THREAD row - the CP stream split on the bridge worker, PE events asynchronous
void setGpThread(bool on);
void setGpRaw(bool on);  // RL-NX (Stage 6): the - menu's GP RAW row (the worker splits the raw CP bytes)
void setAsyncVblank(bool on);  // RL-NX (audit 2026-11-27): the - menu's ASYNC VBLANK row (no per-vblank bridge drain)
bool asyncVblankOn();
void setCpLag(bool on);  // RL-NX (Stage 11): the - menu's CP LAG row (CP reads report the worker's progress)
void setHdTextures(bool on);  // RL-NX (Stage 12): the - menu's HD TEXTURES row (sdmc:/switch/RogueLeader-NX/mods/hd)
bool hdTexturesOn();
void setHdLimit(bool on);     // RL-NX (Stage 12): the - menu's HD TEX 2048 row (top mips above 2048 px dropped)
bool hdLimitOn();
bool cpLagOn();
bool gpRawOn();
bool gpThreadOn();
void menuDraw();                       // inside a frame, before it ends (ImGui)
// menu.cpp: the startup screen while shaders compile and textures prefetch (inside a frame)
void drawShaderProgress(u32 done, u32 total, u64 texDone, u64 texTotal, u64 texBytes);
void applyDisplaySettings();           // resolution, aspect, fullscreen, frame rate -> Aurora/VI
int uberShaderMode();                  // AuroraSetUberShaders' mode: 0 off, 1 hybrid (R2_UBER overrides)
void menuImGuiInit(const void* windowSize);  // Aurora's ImGui init callback: fonts and style
// menu.cpp: the launcher - the settings menu before the game boots (gx_bridge.cpp runLauncher)
void launcherOpen();
bool launcherFrame();  // one frame, inside an ImGui frame; true: the player started the game
void launcherSetDisc(const std::string& path, const std::string& note);  // the disc found at start (also in game)
std::string launcherDisc();  // the disc chosen (the launcher only starts the game with a usable one)
}  // namespace rt::gfx
