// RL-NX: the performance monitor's counters (perfmon.h). Defined here, in the runtime, so every build has
// them - the Switch layer's reporter (switch/src/perfmon_switch.cpp) reads them; the desktop build only
// counts. Zero-initialised: nothing to set up.
#include "perfmon.h"

extern "C" {
RlPmCounters rl_pm = {};
}
