// RL-NX (2026-11-27): the restart the in-game updater ends with, done as SFA-NX's Restart does it
// (port/src/vi_shim.c shutdown_and_exit): every thread of this NRO must have ended, the GPU's memory and the services given
// back, and then libnx's own _exit returns to hbloader, which starts the NRO set by envSetNextLoad (the new version).
// Anything left running would execute the unloaded image, so if any thread cannot be ended the process is ended as a normal
// quit does and the update takes effect the next time the player starts RL-NX.
//
// Called on the game thread at a frame boundary (gx_bridge.cpp ensureFrame, after bridgeShutdown ended the GX worker).
#include <switch.h>

#include <cstdlib>

extern "C" {
void pc_log_line(const char* fmt, ...);
void melee_nx_log_flush(void);               // log_switch.cpp: wait until the log is on the card
int rlRelaunchPending(void);                  // updater_switch.cpp: the NRO to start next was set
void rlUpdaterReleaseNet(void);               // updater_switch.cpp: curl + sockets
void aurora_shutdown(void);                   // Aurora: ends its threads, SDL and the renderer's frame objects
void aurora_switch_dk_release_all(void);      // switch/deko/device.cpp: frees all GPU memory, queue and device
int kartpad_end_threads(void);                // clang_tls_switch.c: waits for the threads made through pthread, returns how many remain
__attribute__((noreturn)) void kartpad_exit_to_loader(void);
int rlProfThreadsStarted(void);               // prof_switch.cpp: libnx threads nothing can end
}
namespace rt {
void eventsShutdown();                        // events.cpp: the timer thread
namespace input { void inputShutdown(); }     // input.cpp: the joystick thread
namespace audio { void shutdown(); }          // audio_switch.cpp: the audio-out thread and audout
}  // namespace rt

// RL-NX: flushed step marker, so a freeze in the teardown names its step in the log
#define STEP(m) do { pc_log_line("[rl-nx] leaving: " m); melee_nx_log_flush(); } while (0)
extern "C" __attribute__((noreturn)) void rlnxRelaunchTeardown(void) {
    const bool relaunch = rlRelaunchPending() != 0;
    pc_log_line("[rl-nx] leaving: %s", relaunch ? "relaunching through hbloader" : "ending the process");
    STEP("input"); rt::input::inputShutdown();   // before SDL goes: it calls SDL_UpdateJoysticks
    STEP("events"); rt::eventsShutdown();
    STEP("audio"); rt::audio::shutdown();
    STEP("aurora"); aurora_shutdown();
    STEP("aurora ended");
    if (relaunch && rlProfThreadsStarted() == 0) {
        if (kartpad_end_threads() == 0) {
            STEP("gpu release"); aurora_switch_dk_release_all();  // memory still locked to the GPU would stop hbloader mapping the next NRO
            STEP("net release"); rlUpdaterReleaseNet();           // the updater's bsd session and transfer memory
            STEP("exit to loader"); kartpad_exit_to_loader();        // flushes, gives the fast-mem window back, libnx _exit -> hbloader
        }
        pc_log_line("[rl-nx] leaving: threads still running, plain exit (start RL-NX again to use the update)");
    } else if (relaunch) {
        pc_log_line("[rl-nx] leaving: the profiler's threads ran, plain exit (start RL-NX again to use the update)");
    }
    exit(0);  // wrapped: flushes the log, svcExitProcess
    __builtin_unreachable();
}
