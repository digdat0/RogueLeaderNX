// RL-NX: the host side of the runtime on the Switch.
//
// src/runtime/main.cpp holds the guest's boot, its disc, its logging and its frame hook, and the
// desktop-only files around it (settings, mods, save states, achievements, gyro, input, audio) hold
// the rest of what the guest core and Aurora call. This file is the Switch side of that surface:
// the boot path is real (disc, log, frame hook), and the parts whose Switch version is a later step
// are here as small, honest placeholders rather than silence at link time. Each placeholder names
// what replaces it.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <switch.h>

#include "../common/disc_image.h"
#include "achievements.h"  // rt::ra::*
#include "gfx/gx_bridge.h"  // rt::gfx::vblank
#include "gyro.h"          // rt::gyro::setFlight

// RL-NX: the profiler's on-demand report (switch/src/prof_switch.cpp), for the stall watchdog below.
extern "C" void rlProfDumpNow(const char* why);
// RL-NX: the menu's PERF page keeps the profiler's switch in settings.ini; applied once here.
extern "C" void rlProfSetEnabled(int on);
#include "hotkeys.h"       // rt::hotkeys::tick
#include "hw/musyx_dsp.h"  // rt::audio::*
#include "log_switch.h"    // melee_nx_log_write_critical (fatal)
#include "mods.h"          // rt::mods::userDir
#include "runtime.h"
#include "savestate.h"     // rt::state::g_pending, service, scriptTick
#include "settings.h"      // rt::settings::Settings

namespace rt {

// --- logging and termination ---------------------------------------------------------------
// logf, describeAddr and peekGuest32 live in runtime_switch.cpp (they need Clang for ctx.h).

extern bool g_oracleIo;              // RL-NX (9.55): oracle.cpp / ctx.h (this file is built without ctx.h)
[[noreturn]] void oracleAbandon();
void logf(const char* tag, const char* fmt, ...);
void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    // RL-NX (9.55): a fatal inside an oracle probe pass (code from an unmapped VM page, which the probe may not
    // page in) ends that probe as skipped, not the game: the pass ran on a scratch copy.
    if (g_oracleIo) {
        logf("oracle", "fatal inside a probe pass, probe skipped: %s", buf);
        oracleAbandon();
    }
    char line[2100];
    const int n = snprintf(line, sizeof line, "[fatal] %s\n", buf);
    if (n > 0) melee_nx_log_write_critical(line, size_t(n < int(sizeof line) ? n : sizeof line - 1));
    consoleInit(NULL);  // the screen may already be the log's; a fatal message is worth a re-init
    printf("fatal: %s\n\npress + to exit\n", buf);
    PadState pad = {};
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    }
    melee_nx_log_flush();
    consoleExit(NULL);
    quit(1);
}

void quit(int code) {
    melee_nx_log_flush();
    fflush(nullptr);
    svcExitProcess();
    __builtin_unreachable();
}

bool envFlag(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

// --- the disc ------------------------------------------------------------------------------
// The player's own image, opened with Red Two's reader (src/common/disc_image.cpp: ISO, GCM, and
// the compressed RVZ/WIA forms).
namespace {
std::unique_ptr<disc::Image> s_image;
}

bool discOpen(const std::string& path) {
    std::string err;
    s_image = disc::open(path, err);
    if (!s_image) {
        logf("disc", "%s", err.c_str());
        return false;
    }
    logf("disc", "%s image %s (%llu MiB)", s_image->kind(), path.c_str(), (unsigned long long)(s_image->size() >> 20));
    return s_image->size() > 0x440;
}

bool discRead(u64 off, void* dst, u32 len) { return s_image && s_image->read(off, dst, len); }

// --- the frame hook (main.cpp's viFrameHook) -----------------------------------------------
// Called by the VI when it finishes a frame. On the PC this polls input and hands the frame to
// Aurora; here it counts frames and says so every two seconds, which is the cheapest way to tell a
// guest that is running from one that is stuck (the perf line comes with the renderer, §E 5).
void viFrameHook() {
    static u32 s_frames = 0;
    static u64 s_last = 0;
    // RL-NX: R2_DETERMINISTIC=1 - one presented frame is the only thing that moves the guest's clock, so
    // the same frame index is the same amount of emulated time on every run (events.cpp detAdvanceFrame).
    detAdvanceFrame();
    // RL-NX: the profiler switched on from the menu's PERF page stays on for the next start (settings.ini
    // profiler=1), so the state has to be applied once the settings are loaded.
    static const bool s_profApplied = []() {
        if (settings::get().profiler) rlProfSetEnabled(1);
        return true;
    }();
    (void)s_profApplied;
    gfx::vblank();  // the per-frame tick: input poll, window events (gx_bridge.cpp)
    ++s_frames;
    meterFrame(s_frames);  // RL-NX: R2_STATE_HASH=<frame>[,...] - the differential harness's probe
    if (s_frames % 120 == 0) {
        const u64 now = svcGetSystemTick();
        const double fps = s_last ? 120.0 / (double(now - s_last) / 19200000.0) : 0.0;
        s_last = now;
        // The renderer's own counters: what the guest asked Aurora to draw this second. A guest that
        // boots but draws nothing looks identical to a working one in the frame line alone.
        const std::string st = gfx::stats();
        // Headroom: how much of each frame the guest's host thread spent waiting for its next event
        // (g_idleTb, 40.5 MHz ticks) rather than running. At a locked 60 fps this is the only signal
        // that says whether the port has room to spare or is right at the edge - fps cannot, because
        // the guest paces itself on the emulated retrace.
        static u64 s_lastIdle = 0;
        const u64 idle = g_idleTb;
        const double idleMs = double(idle - s_lastIdle) / 40500.0;
        const double busyMs = 2000.0 - idleMs;  // 120 frames at 59.94 Hz
        s_lastIdle = idle;
        logf("frame", "%u frames, %.1f fps; idle %.0f ms / busy %.0f ms per 120 frames; %s", s_frames, fps, idleMs, busyMs,
             st.c_str());
        // RL-NX: R2_ICOUNT=1 - the codegen metric. fps moves with the scene; ns per guest instruction
        // does not, so this is the number that compares two builds of the recompiler. The guest thread
        // is busy throughout these runs, so the interval is guest work and almost nothing else.
        if (g_icount) {
            static u64 s_insns = 0, s_tick = 0;
            const u64 ins = g_guestInsns;
            if (s_tick) {
                const double secs = double(now - s_tick) / 19200000.0;
                const double di = double(ins - s_insns);
                logf("meter", "%.2f M guest insn/frame, %.2f ns/insn, %.0f M insn/s", di / 120.0 / 1e6,
                     di > 0.0 ? secs * 1e9 / di : 0.0, secs > 0.0 ? di / secs / 1e6 : 0.0);
            }
            s_insns = ins;
            s_tick = now;
        }
        // RL-NX: stall watchdog. On a black screen the present loop keeps ticking at 60 fps and only the
        // guest goes quiet, so this is the one place the stop is visible. Four seconds with no new draw
        // call is not a slow frame or a level load: it is the guest not submitting. Say so, and ask the
        // profiler for a report right then, which names the guest function it is sitting in - the two
        // minute cadence would otherwise have to be lucky to cover it.
        static u64 s_lastDraws = 0;
        static u32 s_quietChecks = 0;
        static u32 s_stallDumps = 0;
        const u64 draws = gfx::drawCalls();
        if (draws != s_lastDraws) {
            if (s_quietChecks >= 2) {
                logf("stall", "guest drawing again after %u s quiet (frame %u)", s_quietChecks * 2, s_frames);
            }
            s_lastDraws = draws;
            s_quietChecks = s_stallDumps = 0;
        } else if (++s_quietChecks == 2) {
            logf("stall", "guest quiet: no Aurora draws for %u s at frame %u (last frame's picture: %s)", s_quietChecks * 2,
                 s_frames, st.c_str());
            rlProfDumpNow("stalled");
            ++s_stallDumps;
        } else if (s_quietChecks > 2 && s_stallDumps < 6 && (s_quietChecks - 2) % 15 == 0) {
            logf("stall", "guest still quiet: %u s, no Aurora draws (frame %u)", s_quietChecks * 2, s_frames);
            rlProfDumpNow("still stalled");
            ++s_stallDumps;
        }
    }
}

// --- guest memory card: not on the Switch yet ---------------------------------------------
// The runtime owns EXI/CARD through hw.cpp; the image files are a later step (§E 6). Until then the
// game runs without a memory card, which is exactly what a console with no card inserted does.
namespace {
void cardUnsupported(const char* what) { logf("card", "%s: memory cards are not wired up on Switch yet", what); }
}  // namespace

// --- settings -------------------------------------------------------------------------------
// Defaults, in memory only: the Switch uses the console's own menus for its settings, so there is
// no settings.ini to read. The fields the game's behaviour depends on keep their desktop defaults.
settings::Settings& settings::get() {
    static Settings s;
    return s;
}

// --- files ---------------------------------------------------------------------------------
// Everything the port writes lives in its folder on the SD card (logs/, saves/, textures/).
std::string mods::userDir() { return "sdmc:/switch/RogueLeader-NX/"; }

// --- save states ----------------------------------------------------------------------------
// No save states on the Switch yet (docs/SWITCH_PORT.md §B: not required for the first version).
// The safepoint must still answer, or every guest frame would branch into nothing.
volatile bool state::g_pending = false;
void state::service(Ctx&) {}
void state::scriptTick() {}
void state::request(Op, int) {}   // (save states/hotkeys ask here; nothing to serve yet)
std::string state::slotInfo(int) { return {}; }

// --- achievements ---------------------------------------------------------------------------
// RetroAchievements needs rcheevos (not fetched) and the HTTP stack; off for now, and silent.
namespace ra {
void frame() {}
}  // namespace ra

// --- widescreen helpers (gfx/menu.cpp) ------------------------------------------------------
// The menu owns the widescreen settings; with no menu, the game runs at its native aspect.
namespace gfx {
void noteWidenedAspect(float) {}  // (the menu's widescreen settings; see gx_bridge.h)
void setHudAnchoring(bool) {}
// The desktop F1 menu and the Android touch controls have no Switch counterpart yet (§E 6).
void menuEvent(const void*) {}
void touchDraw() {}
void touchEvent(const void*) {}
bool touchApply(void*) { return false; }
// A short message on screen (the desktop draws it over the frame, menu.cpp). Nowhere to draw yet: log it.
void toast(const std::string& text) { logf("toast", "%s", text.c_str()); }
// Aurora's ubershader mode, from menu.cpp: R2_UBER overrides, else the setting (on by default).
int uberShaderMode() {
    if (const char* uberEnv = getenv("R2_UBER")) return atoi(uberEnv);
    return settings::get().ubershaders ? 1 : 0;
}
}  // namespace gfx

// --- texture packs (mods.cpp) ----------------------------------------------------------------
// Dolphin texture packs and cheat codes are a later step; the game runs with the disc's own assets.
namespace mods {
std::string findPackFile(const std::string&) { return {}; }
std::string toggleTexturesShown() { return "texture packs are not available on this build"; }
std::size_t loadedTextureCount() { return 0; }
std::string textureStats() { return {}; }
void textureFrame(bool) {}
}  // namespace mods

// --- settings helpers -----------------------------------------------------------------------
namespace settings {
// The game's own 4:3 (the aspect settings live in the desktop menu).
float aspectValue(Aspect a) { return a == Aspect::R16_9 ? 16.0f / 9.0f : 4.0f / 3.0f; }
const char* stickCurveName(StickCurve c) {
    switch (c) {
    case StickCurve::Smooth: return "smooth";
    case StickCurve::Linear: return "linear";
    case StickCurve::GameCube: return "gamecube";
    default: return "smooth";
    }
}
void save() {}  // nothing to write: the settings are the defaults above
}  // namespace settings

// --- quit -----------------------------------------------------------------------------------
// The desktop quit path flushes the save state and the memory card; there are neither here yet.
[[noreturn]] void quitToDesktop() { quit(0); }

// --- audio ----------------------------------------------------------------------------------
// The output itself is switch/src/audio_switch.cpp (audout + the 32 kHz -> 48 kHz resample).

}  // namespace rt
