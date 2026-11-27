// RL-NX: GPU 460.8 MHz in handheld, always on (2026-11-26; replaces Stage 1's CPU BOOST row, which the user removed:
// the port must not clock the CPU past retail, players who want more use their own OC tool).
//
// apm performance configuration 0x92220008 is one of the console's own retail handheld profiles (CPU 1020 MHz,
// GPU 460.8 MHz, memory 1331.2 MHz) that official games ask for; homebrew otherwise gets the 307.2 MHz default.
// 0x00020004 (GPU 384 MHz) is the fallback if the first is refused. Only the Normal (handheld) mode's configuration
// is changed, so docked stays at the system's 768 MHz. Same profile and fallback as SFA-NX's GPU clock row.
//
// The original configuration is saved at boot and put back at every exit (rlnxClockRestore, from
// clang_tls_switch.c's two exit paths), and apm is closed there: a session left open breaks the next launch
// from hbloader (port skill 3.4), and a boosted configuration left behind would be saved as the next run's
// "original". The clocks in effect go into the run's log ([clock] line, read through clkrst; Ryujinx has none).
#include <switch.h>
#include <stdbool.h>
#include <stdio.h>

#define RLNX_GPU_CONF 0x92220008u      // RL-NX: retail handheld profile, GPU 460.8 MHz
#define RLNX_GPU_FALLBACK 0x00020004u  // RL-NX: retail handheld profile, GPU 384 MHz

static bool s_apm = false;    // RL-NX: apm initialised by us
static bool s_saved = false;  // RL-NX: s_orig holds the configuration before ours
static u32 s_orig = 0;

// RL-NX: one module's clock in MHz through a short-lived clkrst session (0 if clkrst is unavailable)
static u32 clockMhz(PcvModuleId module) {
    ClkrstSession s;
    u32 hz = 0;
    if (R_SUCCEEDED(clkrstOpenSession(&s, module, 3))) {
        clkrstGetClockRate(&s, &hz);
        clkrstCloseSession(&s);
    }
    return hz / 1000000u;
}

// RL-NX: the run's log (log_switch.cpp's stdout redirect): the clocks and apm configuration actually in effect
static void clockLog(const char* when) {
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    u32 conf = 0;
    if (s_apm) {
        apmGetPerformanceMode(&mode);
        apmGetPerformanceConfiguration(mode, &conf);
    }
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) {  // RL-NX: clkrst from 8.0.0; closed at once
        printf("[clock] %s: CPU %u MHz, GPU %u MHz, EMC %u MHz, mode %d, configuration 0x%08X\n", when,
               clockMhz(PcvModuleId_CpuBus), clockMhz(PcvModuleId_GPU), clockMhz(PcvModuleId_EMC), (int)mode, conf);
        clkrstExit();
    } else {
        printf("[clock] %s: clkrst unavailable, mode %d, configuration 0x%08X\n", when, (int)mode, conf);
    }
}

// RL-NX: once, on the first game frame (menu_switch.cpp menuDraw): the 460.8 MHz handheld profile
void rlnxClockInit(void) {
    if (s_apm) return;
    s_apm = R_SUCCEEDED(apmInitialize());
    if (s_apm) s_saved = R_SUCCEEDED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &s_orig));
    if (s_apm && s_saved) {
        if (R_FAILED(apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, RLNX_GPU_CONF)))
            apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, RLNX_GPU_FALLBACK);
    }
    clockLog(s_saved ? "GPU 460.8 MHz handheld profile" : "apm unavailable, system clocks");
}

// RL-NX: both exit paths (clang_tls_switch.c): the original configuration back and apm closed, before the log closes
void rlnxClockRestore(void) {
    if (!s_apm) return;
    if (s_saved) apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, s_orig);
    apmExit();  // RL-NX: drops our reference; libnx's appletExit drops its own
    s_apm = false;
    s_saved = false;
}
