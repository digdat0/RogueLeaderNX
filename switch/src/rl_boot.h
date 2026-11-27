// RL-NX: the Switch boot helpers that replace src/runtime/main.cpp's static ones (boot_switch.cpp).
#pragma once
#include <string>

#include "ctx.h"

namespace rt {

// The first disc image (.iso/.gcm/.rvz/.wia) in the port's folder on the SD card, or "" if none.
std::string findDiscSwitch();
// Reads the DOL into guest memory and sets up the low-memory block the apploader would leave;
// returns the guest entry point.
u32 bootDiscSwitch();

namespace gfx {
// gx_bridge.cpp: Aurora's configuration for the Switch (headless surface until switch/deko lands).
bool switchInit();
}  // namespace gfx

}  // namespace rt
