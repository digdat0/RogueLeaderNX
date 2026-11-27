// MusyX CPU-side hooks (hooks.txt), see hw/musyx_dsp.cpp for the DSP side.
#include "funcs.h"
#include "runtime.h"

namespace rt {

// salBuildCommandList(dest, nsDelay): nsDelay is the time since the AI interrupt, which MusyX
// turns into DSP cycles already lost this frame; past the budget it kills voices as "DSP
// overload". Our DSP finishes a list at once, so no time is lost: report none. (The host can
// make the delay look like milliseconds: the GPU runs on the guest thread.)
void hle_salBuildCommandList(Ctx& c) {
    static u64 late = 0;
    if (c.r[4] > 3000 && late++ < 20) logf("musyx", "command list started %u us after the AI interrupt (ignored)", c.r[4]);
    c.r[4] = 0;
    f_8001D9EC_orig(c);
}

}  // namespace rt
