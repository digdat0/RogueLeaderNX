// Hotkeys: actions on a combination of controller buttons or keys held together (Back + RB
// saves a state, Back + LB loads it, ...). Bindings live in settings (hotkeyPad / hotkeyKeys).
#pragma once
#include <string>

namespace rt::hotkeys {

// RL-NX: the - menu (switch/src/menu_switch.cpp) needs single presses, not held buttons, and must read
// them through the same SDL path the hotkeys use: a second libnx PadState stalls the guest (measured:
// 4 s without a draw when the menu polled one).
enum MenuPad : int { PadMinus = 0, PadUp, PadDown, PadLeft, PadRight, PadA, PadL, PadR, PadCount };  // RL-NX: + the shoulders (menu tabs)
// RL-NX: true once per press (latched by tick(), cleared by this call). PadMinus fires when Minus is released
// without another button pressed while it was held, so Minus + RB (save state) does not open the panel.
bool padEdge(int which);
void tick();  // once per presented frame (guest thread): fire the actions whose buttons went down
// The game sees no buttons or triggers now: a hotkey's modifier (a button in a combination that
// plays no GameCube control, e.g. Back) is held, or a combination just fired and is still down.
bool muteGame();
unsigned fireCount();  // RL-NX (audit 2026-11-27): actions fired so far (ASYNC VBLANK drains after one)

std::string label(int action, bool pad);  // "BACK + RB", "F5", "-" for none

// the settings menu's capture: hold the buttons (or keys), then let go
void captureStart(int action, bool pad);
void captureTick();     // while capturing (menu draw)
bool capturing();
int captureAction();    // the action being captured, -1 none
void captureCancel();
void clear(int action, bool pad);
void resetDefaults(bool pad);

}  // namespace rt::hotkeys
