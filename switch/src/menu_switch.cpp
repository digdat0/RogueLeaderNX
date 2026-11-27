// RL-NX: the - menu. Tapping Minus on the controller opens a compact options panel over the running game and
// closes it again; the d-pad moves, A (or right) switches the row under the cursor. It carries the port's
// run-time switches (the frame rate counter, the [stats] log lines, the 1 kHz profiler) rather than the
// desktop settings UI, which is not part of this build (gfx/menu.cpp is parked: switch/CMakeLists.txt lists it).
//
// Two threads touch it:
//   - menuDraw() runs on the guest thread once a game frame (gx_bridge.cpp endFrame). It reads the presses
//     hotkeys::tick() latched (one SDL controller query a vblank, shared with the hotkeys) and changes state.
//   - rl_nx_menu_draw() runs on Aurora's render worker inside the present pass (deko/device.cpp), after the
//     game's picture and the R2_FPS counter, using the same 3x5 text pass (switch/deko/pattern.cpp).
// So the state the drawing reads is atomic. While the panel is open menuOpen() is true, which makes input.cpp
// hand the game idle controllers and turns the hotkeys off - A switches a row instead of firing.
//
// A tap means Minus pressed and released with nothing else pressed while it was held (hotkeys.cpp): Minus is
// also the modifier of the save-state hotkeys (Minus + RB / LB / d-pad), which must not open the panel.
#include <switch.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>  // RL-NX: savePrefs keeps the file's other lines
#include <sys/stat.h>  // RL-NX: mkdir for config/

#include "hotkeys.h"  // RL-NX: the - menu's presses (SDL path)

namespace rt {
void logf(const char* tag, const char* fmt, ...);  // runtime.h, which pulls ctx.h (Clang only)
}

// RL-NX: gx_bridge.h pulls in ctx.h, which needs Clang (its atomics use __builtin_rotateleft32) and
// this file is built by the C++ compiler rl_entry uses for its plain sources: the entry points it
// needs are declared here instead.
namespace rt::gfx {
void setStatsEnabled(bool on);
void setBridgeThread(bool on);  // RL-NX (9.53): GX THREAD - GX commands decoded on the bridge worker
bool bridgeThreadOn();
void setGpThread(bool on);  // RL-NX (9.57): GP THREAD - the CP stream split on the bridge worker
bool gpThreadOn();
void setGpRaw(bool on);  // RL-NX (Stage 6): GP RAW - the worker splits the raw CP bytes (no walker on the game thread)
bool gpRawOn();
void setAsyncVblank(bool on);  // RL-NX (audit 2026-11-27): ASYNC VBLANK
bool asyncVblankOn();
void setCpLag(bool on);  // RL-NX (Stage 11): CP LAG - with GP RAW, CP register reads report the worker's progress (no drain)
bool cpLagOn();
void setHdTextures(bool on);  // RL-NX (Stage 12): HD TEXTURES - Dolphin-format packs from sdmc:/switch/RogueLeader-NX/mods/hd
bool hdTexturesOn();
}
namespace rt::input {
bool rumbleOn();  // RL-NX (2026-11-27): RUMBLE / INVERT FLIGHT rows (input.cpp)
void setRumbleOn(bool on);
bool invertFlightOn();
void setInvertFlight(bool on);
}
extern "C" void rlUpdaterActivate(void);  // RL-NX (2026-11-27): the UPDATE row (updater_switch.cpp)
extern "C" int rlUpdaterState(int* percent, char* tag, size_t tagSize);
extern "C" void rlUpdaterMessage(char* out, size_t size);
extern "C" void rl_log_set_extra(int on);  // RL-NX (2026-11-27): GAME LOGGING row (log_switch.cpp)
extern "C" int rl_log_extra_on;
extern "C" const char rl_nx_version_label[];  // RL-NX (2026-11-27): "v0.1.0 4561" (main_switch.cpp)
namespace rt {
void gxSetFastPipe(bool on);  // RL-NX (Stage 4): FAST PIPE - gather pipe stores straight into the buffer (gx_fifo.cpp)
bool gxFastPipe();
void setNativeMtx(bool on);  // RL-NX (Stage 9): NATIVE MTX - the matrix natives (hle_mtx.cpp)
bool nativeMtxOn();
void musyxSetFast(bool on);  // RL-NX (audit 2026-11-27): DSP FAST (hw/musyx_dsp.cpp)
bool musyxFastOn();
void setNativeMovie(bool on);  // RL-NX (Stage 10): NATIVE MOVIE - .movie frames through libjpeg-turbo (hle_movie.cpp)
bool nativeMovieOn();
void vmCensusSet(bool on);  // RL-NX (Stage 9): VM CENSUS - VM access sites to logs/vmcensus.txt (memory_switch.cpp)
bool vmCensusOn();
}
namespace rt::audio {
void setThreaded(bool on);  // RL-NX (9.57): AUDIO THREAD - audout calls on their own thread (audio_switch.cpp)
bool threaded();
}

// The deko3d text pass and the present-rate counter (deko/dk.h). Declared rather than included: this file has
// no other use for deko3d, and these are the signatures dk.h, pattern.cpp and device.cpp already agree on.
namespace swdk {
void draw_text(int left, int top, int scale, const char* const* lines, int count, const float fg[4],
               const float bg[4]);
void draw_rect(int left, int top, int width, int height, const float color[4]);
void surface_size(int* w, int* h);  // RL-NX: swapchain pixels
bool overlay_fps_enabled();     // the top-middle "FPS N" counter (frames actually presented), R2_FPS
void overlay_fps_set(bool on);  // switched by the FRAME RATE row
}  // namespace swdk

extern "C" void rlProfSetEnabled(int on);
extern "C" int rlProfEnabled(void);
extern "C" void rlPmSetEnabled(int on);       // RL-NX: PERF LOG lines (perfmon_switch.cpp)
extern "C" int rlPmEnabled(void);
extern "C" void rlnxGpuTimingSet(int on);     // RL-NX: GPU TIMING (perfmon_switch.cpp -> gpu_timing.cpp)
extern "C" int rlnxGpuTimingEnabled(void);
extern "C" void aurora_set_frame_overlap(bool value);  // RL-NX (Stage 7): FRAME OVERLAP (extern/aurora/lib/aurora.cpp)
extern "C" bool aurora_frame_overlap(void);
extern "C" void rlnxClockInit(void);  // RL-NX: GPU 460.8 MHz handheld profile at boot (clock_switch.c); CPU BOOST row removed 2026-11-26

namespace rt::gfx {

// RL-NX (2026-11-27): two tabs. Game (FRAME RATE, HD TEXTURES) always; Debug (everything else) only with R2_DEBUG_MENU=1
// (config/settings.cfg), as SFA-NX's debug_menu=1. HD TEX 2048 is gone (the 2048 limit keeps its default, R2_HD_LIMIT=0 in settings.cfg lifts it).
enum Row { kFrameRate = 0, kStatsLog, kProfiler, kPerfLog, kGpuTiming, kGxThread, kGpThread, kAudioThread, kFastPipe, kGpRaw, kFrameOverlap, kNativeMtx, kVmCensus, kNativeMovie, kCpLag, kHdTextures, kAsyncVblank, kDspFast, kInvertFlight, kRumble, kGameLog, kRowCount, kUpdate = kRowCount };  // kUpdate is an action row (rlUpdater*), not a switch: no s_on / pref
constexpr int kGameRows[] = {kFrameRate, kInvertFlight, kRumble, kHdTextures, kGameLog, kUpdate};
constexpr int kDebugRows[] = {kStatsLog, kProfiler, kPerfLog, kGpuTiming, kGxThread, kGpThread, kAudioThread, kFastPipe, kGpRaw,
                              kFrameOverlap, kNativeMtx, kVmCensus, kNativeMovie, kCpLag, kAsyncVblank, kDspFast};
constexpr int kGameN = int(sizeof kGameRows / sizeof kGameRows[0]), kDebugN = int(sizeof kDebugRows / sizeof kDebugRows[0]);

namespace {

// The panel's state: written by the guest thread (menuDraw), read by the render worker (rl_nx_menu_draw).
std::atomic<bool> s_open{false};
std::atomic<int> s_sel{0};
std::atomic<int> s_tab{0};         // RL-NX: 0 Game, 1 Debug
std::atomic<bool> s_debug{false};  // RL-NX: the Debug tab is shown (R2_DEBUG_MENU=1)
std::atomic<bool> s_on[kRowCount];  // what each row shows; read back from the switch it drives (syncRows)
bool s_synced = false;              // guest thread only
bool envOn(const char* name) {
    const char* e = getenv(name);
    return e != nullptr && *e != '\0' && *e != '0';
}

// Each row starts from the state its switch is really in: the counter from R2_FPS (on unless 0), the stats
// lines from R2_STATS, the profiler from rlProfEnabled() (settings.ini profiler=1 or R2_PROFILE). The old
// panel started every row at OFF, so the first A on a card with R2_STATS=1 "turned on" what was already on.
void syncRows() {
    s_on[kFrameRate].store(swdk::overlay_fps_enabled(), std::memory_order_relaxed);
    s_on[kStatsLog].store(envOn("R2_STATS"), std::memory_order_relaxed);
    s_on[kProfiler].store(rlProfEnabled() != 0, std::memory_order_relaxed);
    s_on[kPerfLog].store(rlPmEnabled() != 0, std::memory_order_relaxed);        // RL-NX
    s_on[kGpuTiming].store(rlnxGpuTimingEnabled() != 0, std::memory_order_relaxed);
    s_on[kGxThread].store(bridgeThreadOn(), std::memory_order_relaxed);  // RL-NX (9.53)
    s_on[kGpThread].store(gpThreadOn(), std::memory_order_relaxed);      // RL-NX (9.57)
    s_on[kAudioThread].store(rt::audio::threaded(), std::memory_order_relaxed);  // RL-NX (9.57)
    s_on[kFastPipe].store(rt::gxFastPipe(), std::memory_order_relaxed);         // RL-NX (Stage 4)
    s_on[kGpRaw].store(gpRawOn(), std::memory_order_relaxed);                   // RL-NX (Stage 6)
    s_on[kFrameOverlap].store(aurora_frame_overlap(), std::memory_order_relaxed);  // RL-NX (Stage 7)
    s_on[kNativeMtx].store(rt::nativeMtxOn(), std::memory_order_relaxed);         // RL-NX (Stage 9)
    s_on[kVmCensus].store(rt::vmCensusOn(), std::memory_order_relaxed);           // RL-NX (Stage 9)
    s_on[kNativeMovie].store(rt::nativeMovieOn(), std::memory_order_relaxed);     // RL-NX (Stage 10)
    s_on[kCpLag].store(cpLagOn(), std::memory_order_relaxed);                     // RL-NX (Stage 11)
    s_on[kHdTextures].store(hdTexturesOn(), std::memory_order_relaxed);           // RL-NX (Stage 12)
    s_on[kAsyncVblank].store(asyncVblankOn(), std::memory_order_relaxed);         // RL-NX (audit 2026-11-27)
    s_on[kDspFast].store(rt::musyxFastOn(), std::memory_order_relaxed);           // RL-NX (audit 2026-11-27)
    s_on[kInvertFlight].store(rt::input::invertFlightOn(), std::memory_order_relaxed);  // RL-NX (2026-11-27)
    s_on[kRumble].store(rt::input::rumbleOn(), std::memory_order_relaxed);
    s_on[kGameLog].store(rl_log_extra_on != 0, std::memory_order_relaxed);
}

// RL-NX: saved rows. Every switch of a row writes all rows into config/settings.cfg (the file also holds any other R2_* lines, kept as written),
// under the names that set their start state (R2_FPS=1 ...); the next boot reads it (loadOptions, then menuLoadPrefs, from
// main_switch.cpp), so what the panel showed when the game closed is how it starts.
// Delete it to go back to the defaults. PROFILER has no plain form without a time limit
// (R2_PROFILE=<seconds>), so it is saved as R2_PROFILER and switched on from boot by menuApplyBootPrefs.
constexpr const char* kPrefKeys[kRowCount] = {"R2_FPS", "R2_STATS", "R2_PROFILER", "R2_PERFLOG", "R2_GPU_TIMING",
                                              "R2_BRIDGE_THREAD", "R2_GP_THREAD", "R2_AUDIO_THREAD", "R2_FAST_PIPE", "R2_GP_RAW", "R2_FRAME_OVERLAP", "R2_NATIVE_MTX", "R2_VM_CENSUS", "R2_NATIVE_MOVIE", "R2_CP_LAG", "R2_HD_TEXTURES", "R2_ASYNC_VBLANK", "R2_DSP_FAST", "R2_INVERT_FLIGHT", "R2_RUMBLE", "R2_GAME_LOG"};  // RL-NX (2026-11-27): R2_HD_LIMIT row removed (an old menu.cfg line is ignored)
constexpr const char* kPrefDir = "sdmc:/switch/RogueLeader-NX/config";
constexpr const char* kPrefPath = "sdmc:/switch/RogueLeader-NX/config/settings.cfg";  // RL-NX (2026-11-27): was menu.cfg; boot.cfg is gone, this is the only options file
constexpr const char* kPrefTmp = "sdmc:/switch/RogueLeader-NX/config/settings.cfg.tmp";
int s_bootPref[kRowCount];  // -1: not in the file

void savePrefs() {
    mkdir(kPrefDir, 0777);
    // RL-NX (2026-11-27): every line of the file that is not a menu row (R2_DEBUG_MENU, diagnostics, comments the user
    // wrote) is read first and written back unchanged; the rows follow, so switching a row never loses a hand edit.
    std::string keep;
    if (FILE* in = fopen(kPrefPath, "r")) {
        char line[512];
        while (fgets(line, sizeof line, in)) {
            if (strncmp(line, "# RL-NX - settings", 18) == 0) continue;  // our header, rewritten below
            const char* eq = strchr(line, '=');
            bool row = false;
            if (eq && line[0] != '#')
                for (int i = 0; i < kRowCount && !row; i++) row = strlen(kPrefKeys[i]) == size_t(eq - line) && strncmp(line, kPrefKeys[i], eq - line) == 0;
            if (!row) keep += line;
        }
        fclose(in);
    }
    FILE* f = fopen(kPrefTmp, "w");
    if (!f) {
        rt::logf("menu", "perf: could not write %s", kPrefTmp);
        return;
    }
    fprintf(f, "# RL-NX - settings. The - menu rows are written by the game whenever one is switched; other R2_* lines are kept as written.\n");
    for (int i = 0; i < kRowCount; i++) fprintf(f, "%s=%d\n", kPrefKeys[i], s_on[i].load(std::memory_order_relaxed) ? 1 : 0);
    if (!keep.empty()) {
        fputs(keep.c_str(), f);
        if (keep.back() != '\n') fputc('\n', f);
    }
    fclose(f);
    remove(kPrefPath);  // Horizon's rename does not replace an existing file
    if (rename(kPrefTmp, kPrefPath) != 0) rt::logf("menu", "perf: could not save %s", kPrefPath);
    // the env-backed rows: keep the environment in step so syncRows (which reads R2_STATS) shows the truth
    setenv("R2_STATS", s_on[kStatsLog].load(std::memory_order_relaxed) ? "1" : "0", 1);
}

void apply(int row, bool on) {
    switch (row) {
    case kFrameRate: swdk::overlay_fps_set(on); break;
    case kStatsLog: setStatsEnabled(on); break;  // gx_bridge's per-frame counters
    case kProfiler: rlProfSetEnabled(on ? 1 : 0); break;
    case kPerfLog: rlPmSetEnabled(on ? 1 : 0); break;      // RL-NX: the 1 s [pm] lines and [hitch] lines
    case kGpuTiming: rlnxGpuTimingSet(on ? 1 : 0); break;  // RL-NX: per-pass GPU timestamps
    case kGxThread: setBridgeThread(on); break;            // RL-NX (9.53): takes effect at the next vblank
    case kGpThread: setGpThread(on); break;                // RL-NX (9.57): also at the next vblank
    case kAudioThread: rt::audio::setThreaded(on); break;  // RL-NX (9.57): at the next audio block
    case kFastPipe: rt::gxSetFastPipe(on); break;          // RL-NX (Stage 4): at once (this runs on the game thread)
    case kGpRaw: setGpRaw(on); break;                      // RL-NX (Stage 6): at the next vblank
    case kFrameOverlap: aurora_set_frame_overlap(on); break;  // RL-NX (Stage 7): at the next frame end
    case kNativeMtx: rt::setNativeMtx(on); break;             // RL-NX (Stage 9): at the next call
    case kVmCensus: rt::vmCensusSet(on); break;               // RL-NX (Stage 9)
    case kNativeMovie: rt::setNativeMovie(on); break;         // RL-NX (Stage 10): at the next frame
    case kCpLag: setCpLag(on); break;                         // RL-NX (Stage 11): at the next vblank
    case kHdTextures: setHdTextures(on); break;               // RL-NX (Stage 12): at the next vblank
    case kAsyncVblank: setAsyncVblank(on); break;             // RL-NX (audit 2026-11-27): at the next vblank
    case kInvertFlight: rt::input::setInvertFlight(on); break;   // RL-NX (2026-11-27): at once
    case kRumble: rt::input::setRumbleOn(on); break;             // RL-NX (2026-11-27): at once (off also stops a running rumble)
    case kGameLog: rl_log_set_extra(on ? 1 : 0); break;          // RL-NX (2026-11-27): extra detail in logs/log.txt for bug reports
    case kDspFast: rt::musyxSetFast(on); break;               // RL-NX (audit 2026-11-27): at the next DSP list
    default: return;
    }
    s_on[row].store(on, std::memory_order_relaxed);
    savePrefs();  // RL-NX: remembered for the next boot (config/settings.cfg)
    static const char* const kLogNames[kRowCount] = {"frame rate", "stats lines", "profiler", "perf log", "gpu timing", "gx thread", "gp thread", "audio thread", "fast pipe", "gp raw", "frame overlap", "native mtx", "vm census", "native movie", "cp lag", "hd textures", "async vblank", "dsp fast", "invert flight", "rumble", "game logging"};  // RL-NX (Stage 12)  // RL-NX (Stage 11)  // RL-NX (Stage 10)  // RL-NX (Stage 9)  // RL-NX (Stage 7)  // RL-NX (Stage 6); cpu boost removed
    rt::logf("menu", "perf: %s %s", kLogNames[row], on ? "on" : "off");
}

// One frame of input: a Minus tap toggles the panel, up/down move, A switches the row; left/right or L/R change tab
// (when the Debug tab is shown) - only while the panel is open, so the controller is the game's again the moment it closes.
void pollPad() {
    if (hotkeys::padEdge(hotkeys::PadMinus)) {
        const bool open = !s_open.load(std::memory_order_relaxed);
        if (open) syncRows();  // the profiler may have been switched elsewhere (settings, R2_PROFILE expiry)
        s_sel.store(0, std::memory_order_relaxed);
        s_tab.store(0, std::memory_order_relaxed);
        s_open.store(open, std::memory_order_relaxed);
        rt::logf("menu", "perf: panel %s", open ? "open" : "closed");
    }
    // Presses made while the panel is closed are consumed here too, so none fires on the frame it opens.
    const bool up = hotkeys::padEdge(hotkeys::PadUp), down = hotkeys::padEdge(hotkeys::PadDown);
    const bool sw = hotkeys::padEdge(hotkeys::PadA);
    const bool flip = hotkeys::padEdge(hotkeys::PadLeft) | hotkeys::padEdge(hotkeys::PadRight) | hotkeys::padEdge(hotkeys::PadL) |
                      hotkeys::padEdge(hotkeys::PadR);
    if (!s_open.load(std::memory_order_relaxed)) return;
    int tab = s_tab.load(std::memory_order_relaxed);
    if (flip && s_debug.load(std::memory_order_relaxed)) {
        tab = 1 - tab;
        s_tab.store(tab, std::memory_order_relaxed);
        s_sel.store(0, std::memory_order_relaxed);
        return;
    }
    const int n = tab == 0 ? kGameN : kDebugN;
    const int* rows = tab == 0 ? kGameRows : kDebugRows;
    int sel = s_sel.load(std::memory_order_relaxed);
    if (sel >= n) sel = 0;
    if (up) sel = (sel + n - 1) % n;
    if (down) sel = (sel + 1) % n;
    s_sel.store(sel, std::memory_order_relaxed);
    if (sw) {
        if (rows[sel] == kUpdate) rlUpdaterActivate();  // RL-NX: check / download / install + restart, one step a press
        else apply(rows[sel], !s_on[rows[sel]].load(std::memory_order_relaxed));
    }
}

}  // namespace

// (gx_bridge.h) once per game frame, on the guest thread, inside the frame: the panel's input
void menuDraw() {
    if (!s_synced) {
        s_synced = true;
        rlnxClockInit();  // RL-NX: GPU 460.8 MHz handheld profile, always on (replaces CPU BOOST); restored at exit
        syncRows();
        s_debug.store(envOn("R2_DEBUG_MENU"), std::memory_order_relaxed);  // RL-NX (2026-11-27): shows the Debug tab
        // RL-NX: R2_MENU_OPEN=1 opens the panel on the first frame, so an automated run can prove the panel's
        // drawing path without a hand on the controller. Read here, not at static initialisation: boot.cfg is
        // put into the environment after static constructors run, so the old file-scope read never saw it.
        if (envOn("R2_MENU_OPEN")) {
            s_open.store(true, std::memory_order_relaxed);  // R2_MENU_OPEN=1 (first frame only)
            rt::logf("menu", "perf: panel open at start (R2_MENU_OPEN)");
        }
    }
    pollPad();
}

// (gx_bridge.h) input.cpp and hotkeys.cpp ask this: while the panel is open the game must not see the pad
bool menuOpen() { return s_open.load(std::memory_order_relaxed); }

// RL-NX: config/settings.cfg (see savePrefs). At boot, right after boot.cfg: each saved row goes into the environment
// under the name that sets its start state, so every switch starts where the panel left it.
void menuLoadPrefs() {
    for (int& v : s_bootPref) v = -1;
    FILE* f = fopen(kPrefPath, "r");
    if (!f) return;
    char line[128], used[256] = {};
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = '\0';
        const int v = atoi(eq + 1) != 0 ? 1 : 0;
        if (strcmp(line, "R2_DEBUG_MENU") == 0) setenv(line, v ? "1" : "0", 1);  // RL-NX: the Debug tab stays shown
        for (int i = 0; i < kRowCount; i++) {
            if (strcmp(line, kPrefKeys[i]) != 0) continue;
            s_bootPref[i] = v;
            if (i != kProfiler) setenv(kPrefKeys[i], v ? "1" : "0", 1);
            const size_t n = strlen(used);
            snprintf(used + n, sizeof used - n, "%s%s=%d", n ? " " : "", kPrefKeys[i], v);
        }
    }
    fclose(f);
    rt::logf("opts", "settings.cfg (saved - menu rows): %s", used);
}

// RL-NX: the rows that are switched by a call rather than read from the environment, once their switches exist
// (main_switch.cpp, after the profiler's init).
void menuApplyBootPrefs() {
    // a settings.cfg R2_PROFILE=<seconds> run is asked for on purpose: a saved OFF does not cancel it
    const char* timed = getenv("R2_PROFILE");
    if (s_bootPref[kProfiler] == 1 || (s_bootPref[kProfiler] == 0 && !(timed && *timed))) rlProfSetEnabled(s_bootPref[kProfiler]);
    if (s_bootPref[kPerfLog] >= 0) rlPmSetEnabled(s_bootPref[kPerfLog]);
    if (s_bootPref[kGpuTiming] >= 0) rlnxGpuTimingSet(s_bootPref[kGpuTiming]);
    // RL-NX (Stage 7): FRAME OVERLAP is on unless settings.cfg says R2_FRAME_OVERLAP=0 (menuLoadPrefs set the env)
    if (const char* e = getenv("R2_FRAME_OVERLAP"); e && *e) aurora_set_frame_overlap(*e != '0');
    rt::logf("opts", "frame overlap: %s (R2_FRAME_OVERLAP / - menu FRAME OVERLAP)", aurora_frame_overlap() ? "on" : "off");
    // RL-NX (Stage 9): NATIVE MTX is on unless R2_NATIVE_MTX=0; VM CENSUS is off unless R2_VM_CENSUS=1
    if (const char* e = getenv("R2_NATIVE_MTX"); e && *e) rt::setNativeMtx(*e != '0');
    rt::logf("opts", "native mtx: %s (R2_NATIVE_MTX / - menu NATIVE MTX)", rt::nativeMtxOn() ? "on" : "off");
    if (const char* e = getenv("R2_VM_CENSUS"); e && *e == '1') rt::vmCensusSet(true);
    // RL-NX (2026-11-27): the Game rows. RUMBLE is on unless R2_RUMBLE=0; INVERT FLIGHT and GAME LOGGING are off unless =1
    if (const char* e = getenv("R2_RUMBLE"); e && *e == '0') rt::input::setRumbleOn(false);
    if (const char* e = getenv("R2_INVERT_FLIGHT"); e && *e && *e != '0') rt::input::setInvertFlight(true);
    if (const char* e = getenv("R2_GAME_LOG"); e && *e && *e != '0') rl_log_set_extra(1);
    // RL-NX (Stage 10): NATIVE MOVIE is on unless R2_NATIVE_MOVIE=0
    if (const char* e = getenv("R2_NATIVE_MOVIE"); e && *e) rt::setNativeMovie(*e != '0');
    rt::logf("opts", "native movie: %s (R2_NATIVE_MOVIE / - menu NATIVE MOVIE)", rt::nativeMovieOn() ? "on" : "off");
}

}  // namespace rt::gfx

// The panel, drawn into the bound swapchain image by the present path (deko/device.cpp) on the render worker.
// Returns whether it recorded anything (the caller then restores the frame's viewport). The 3x5 font is 4x6
// pixels per cell at scale 1. RL-NX (2026-11-27): the panel is centred on the screen with a double border, a
// title (and the Game / Debug tabs when Debug is shown) above, the rows, and a footer naming the buttons.
// The scale is fitted to the screen (4-5 at 720p); every text line is one draw_text, well inside the
// text pass's 768-cell budget. The panel's fills are opaque so the layered rectangles do not tint each other.
extern "C" bool rl_nx_menu_draw() {
    using namespace rt::gfx;
    if (!s_open.load(std::memory_order_relaxed)) return false;
    static const float kGold[4] = {1.0f, 0.85f, 0.35f, 1.0f};
    static const float kText[4] = {0.92f, 0.95f, 1.0f, 1.0f};
    static const float kDim[4] = {0.55f, 0.60f, 0.72f, 1.0f};
    static const float kGreen[4] = {0.45f, 1.0f, 0.55f, 1.0f};
    static const float kBg[4] = {0.02f, 0.04f, 0.09f, 1.0f};
    static const float kBgRow[4] = {0.10f, 0.16f, 0.30f, 1.0f};
    static const float kLine[4] = {0.30f, 0.42f, 0.70f, 1.0f};
    static const char* const kNames[kRowCount] = {"FRAME RATE", "STATS LOG", "PROFILER", "PERF LOG", "GPU TIMING", "GX THREAD", "GP THREAD", "AUDIO THREAD", "FAST PIPE", "GP RAW", "FRAME OVERLAP", "NATIVE MTX", "VM CENSUS", "NATIVE MOVIE", "CP LAG", "HD TEXTURES", "ASYNC VBLANK", "DSP FAST", "INVERT FLIGHT", "RUMBLE", "GAME LOGGING"};

    const bool debug = s_debug.load(std::memory_order_relaxed);
    const int tab = debug ? s_tab.load(std::memory_order_relaxed) : 0;
    const int n = tab == 0 ? kGameN : kDebugN;
    const int* rows = tab == 0 ? kGameRows : kDebugRows;
    const int sel = s_sel.load(std::memory_order_relaxed);

    int sw = 0, sh = 0;
    swdk::surface_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) return false;
    // RL-NX (2026-11-27): large text: scale 6 at 720p (24x36 px cells' worth of letters), 8 at 1080p. The Game tab fits whole; the Debug tab's
    // 16 rows do not, so it shows as many rows as fit and scrolls with the selection (its range is printed on the tab line).
    int scale = sh / 120;
    if (scale < 3) scale = 3;
    if (scale > 8) scale = 8;
    const int fs = scale - 1;                   // the footer's scale
    const int cw = 4 * scale, lineH = 6 * scale + 4;
    const int rule = 2 * scale;                 // a divider line's gap
    const int border = 2 * scale, frame = scale;  // the gold border, then the thin inner frame
    const int pad = 5 * scale;
    const int inset = border + 2 + frame + 2 + pad;  // panel edge to content
    const int contentW = 30 * cw;
    const int footH = 6 * fs + 7;
    const int nFoot = (debug ? 2 : 1) + 1;      // the button lines, then the version line
    const int fixedH = 2 * inset + lineH + (debug ? lineH : 0) + 2 * rule + nFoot * footH;
    int vis = (sh * 94 / 100 - fixedH) / lineH;
    if (vis > n) vis = n;
    if (vis < 3) vis = 3;
    static int off = 0;                         // render worker only: first row shown
    if (sel < off) off = sel;
    if (sel >= off + vis) off = sel - vis + 1;
    if (off + vis > n) off = n - vis;
    if (off < 0) off = 0;
    const int W = contentW + 2 * inset;
    const int H = fixedH + vis * lineH;
    const int x0 = (sw - W) / 2, y0 = (sh - H) / 2;
    const auto box = [&](int d, const float* c) { swdk::draw_rect(x0 + d, y0 + d, W - 2 * d, H - 2 * d, c); };
    box(0, kGold);
    box(border, kBg);
    box(border + 2, kLine);
    box(border + 2 + frame, kBg);

    const int cx = x0 + inset;
    int y = y0 + inset;
    const auto centred = [&](const char* t, int sc, int yy, const float* fg, const float* bg) {
        const int w = (int(strlen(t)) * 4 + 1) * sc;
        const char* lines[1] = {t};
        swdk::draw_text(x0 + (W - w) / 2, yy, sc, lines, 1, fg, bg);
    };
    centred(debug ? "ROGUELEADER-NX DEBUG OPTIONS" : "ROGUELEADER-NX OPTIONS", scale, y, kGold, kBg);
    y += lineH;
    if (debug) {
        const char* g = " GAME ";
        const char* d = " DEBUG ";
        const int gw = (int(strlen(g)) * 4 + 1) * scale, dw = (int(strlen(d)) * 4 + 1) * scale, gap = 3 * scale;
        const int tx = x0 + (W - (gw + gap + dw)) / 2;
        const char* gl[1] = {g};
        const char* dl[1] = {d};
        swdk::draw_text(tx, y, scale, gl, 1, tab == 0 ? kGold : kDim, tab == 0 ? kBgRow : kBg);
        swdk::draw_text(tx + gw + gap, y, scale, dl, 1, tab == 1 ? kGold : kDim, tab == 1 ? kBgRow : kBg);
        if (vis < n) {  // scrolled: which rows are on show
            char rng[24];
            snprintf(rng, sizeof rng, "%d-%d/%d", off + 1, off + vis, n);
            const char* rl[1] = {rng};
            swdk::draw_text(x0 + W - inset - (int(strlen(rng)) * 4 + 1) * fs, y + (lineH - 6 * fs) / 2, fs, rl, 1, kDim, kBg);
        }
        y += lineH;
    }
    swdk::draw_rect(cx, y, contentW, scale, kLine);
    y += rule;
    for (int k = 0; k < vis; k++) {
        const int i = off + k;
        const bool here = i == sel;
        const int r = rows[i];
        const int rowY = y + k * lineH;
        if (here) swdk::draw_rect(cx - pad / 2, rowY - 2, contentW + pad, lineH, kBgRow);
        char buf[40];
        snprintf(buf, sizeof buf, "%c %s", here ? '>' : ' ', r == kUpdate ? "UPDATE" : kNames[r]);
        const char* lines[1] = {buf};
        swdk::draw_text(cx, rowY, scale, lines, 1, here ? kGold : kText, here ? kBgRow : kBg);
        if (r == kUpdate) {  // RL-NX: the updater's state as short text (A does the next step)
            int pct = 0;
            char tag[32] = {}, msg[64] = {}, txt[20] = {};
            const int st = rlUpdaterState(&pct, tag, sizeof tag);
            rlUpdaterMessage(msg, sizeof msg);
            switch (st) {  // updater_switch.cpp State: 0 idle 1 checking 2 up to date 3 available 4 downloading 5 ready 6 failed
            case 0: snprintf(txt, sizeof txt, "A: CHECK"); break;
            case 1: snprintf(txt, sizeof txt, "CHECKING..."); break;
            case 2: snprintf(txt, sizeof txt, "UP TO DATE"); break;
            case 3: snprintf(txt, sizeof txt, "A: GET %s", tag); break;
            case 4: snprintf(txt, sizeof txt, "GETTING %d%%", pct); break;
            case 5: snprintf(txt, sizeof txt, "A: RESTART"); break;
            default: snprintf(txt, sizeof txt, "%s", msg); break;
            }
            for (char* p = txt; *p; ++p) *p = char(toupper(static_cast<unsigned char>(*p)));
            if (strlen(txt) > 15) txt[15] = '\0';
            const char* val[1] = {txt};
            swdk::draw_text(cx + 9 * cw, rowY, scale, val, 1, st == 6 ? kDim : kGreen, here ? kBgRow : kBg);
            continue;
        }
        const bool on = s_on[r].load(std::memory_order_relaxed);
        const char* val[1] = {on ? "ON" : "OFF"};
        swdk::draw_text(cx + 21 * cw, rowY, scale, val, 1, on ? kGreen : kDim, here ? kBgRow : kBg);
    }
    y += vis * lineH;
    swdk::draw_rect(cx, y, contentW, scale, kLine);
    y += rule;
    centred("UP/DOWN MOVE   A SWITCH   - CLOSE", fs, y, kText, kBg);
    if (debug) centred("L/R OR LEFT/RIGHT: GAME / DEBUG", fs, y + footH, kText, kBg);
    {  // RL-NX (2026-11-27): version and build, e.g. V0.1.0 4561 (main_switch.cpp; the build number counts build.sh runs)
        char ver[48];
        snprintf(ver, sizeof ver, "%s", rl_nx_version_label);
        for (char* q = ver; *q; ++q) *q = char(toupper(static_cast<unsigned char>(*q)));
        centred(ver, fs, y + nFoot * footH - footH, kDim, kBg);
    }
    return true;
}
