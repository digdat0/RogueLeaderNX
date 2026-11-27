// Renders the game's GX command stream with Aurora.
//
// Aurora decodes raw BP/CP/XF registers and draws itself, but it takes memory references
// (vertex arrays, textures, palettes, EFB copy destinations) as host pointers through its own
// GX_AURORA sub-commands, as a source port's GX calls would send them. The game's stream has
// the hardware form instead: physical addresses in CP 0xA0-0xAF, BP TX_SETIMAGE3, the TLUT
// load registers and the copy registers. This bridge forwards the stream and translates those:
//   * vertex arrays: sized from the indices the draws actually use (Aurora uploads whole arrays)
//   * textures: one LOAD_TEXOBJ per changed texture map before a draw, from the raw registers
//     (Aurora caches textures by content hash, so memory the game rewrites shows up)
//   * palettes: TMEM TLUT loads are remembered and bound to Aurora TLUT slots at draw time
//   * EFB copies: source rect, destination size/format/address; display copies end the frame
// Display lists are inlined by the CP model (hw/gx_fifo.cpp), so CALL_DL never arrives here.
#include "gx_bridge.h"
#include "../settings.h"  // RL-NX: the menu's PERF page switches the per-frame counters

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <xxhash.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>

#include <algorithm>
#include <atomic>  // RL-NX (9.53): the bridge worker
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#if defined(__SWITCH__)
extern "C" int rlnxRelaunchRequested(void);   // RL-NX (2026-11-27): updater_switch.cpp
extern "C" void rlnxRelaunchTeardown(void);  // relaunch_switch.cpp, does not return
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <initializer_list>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "../../../extern/aurora/lib/gx/fifo.hpp"

#ifdef _WIN32
// user32 SetWindowPos without pulling <windows.h> into this file (its macros clash with GX names).
extern "C" __declspec(dllimport) int __stdcall SetWindowPos(void*, void*, int, int, int, int, unsigned);
extern "C" __declspec(dllimport) long long __stdcall GetWindowLongPtrW(void*, int);
extern "C" __declspec(dllimport) long long __stdcall SetWindowLongPtrW(void*, int, long long);
extern "C" __declspec(dllimport) int __stdcall GetSystemMetrics(int);
extern "C" __declspec(dllimport) int __stdcall ShowWindow(void*, int);
#endif
#include "../achievements.h"
#include "../hw/gx_fifo.h"
#include "../hw/musyx_dsp.h"
#include "../hotkeys.h"
#include "../mods.h"
#include "tex_dump.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../settings.h"
#include "../perfmon.h"  // RL-NX: PERF LOG counters (draws, copies, textures, Aurora stats)

#ifdef AURORA_GFX_DEKO3D
// RL-NX: switch/src/vsync_switch.cpp creates the display, layer and default window on demand (the
// deko3d swapchain presents to them).
extern "C" void rlWindowEnsure(void);
#define RL_NX_DEKO_NOTE 1
#endif

#ifdef __SWITCH__
// RL-NX: frame phase marks for the sampling profiler (switch/src/prof_switch.cpp, boot.cfg R2_PROFILE):
// 0 frame opened, 1 game done (aurora_end_frame starts), 2 aurora_end_frame done, 3 wait done. The game
// does its own waiting inside the guest (VIWaitForRetrace idles in events.cpp), so 3 follows 2 directly
// and that wait is inside the next frame's "game" section. Cheap when the profiler is off (a clock read).
extern "C" void rlProfMark(int id);
// RL-NX (9.53): the bridge worker places itself (switch/src/perf_switch.cpp)
extern "C" void rlnxBridgeThreadStarted(void);
#endif

// RL-NX (Stage 12): Aurora's texture counters (lib/gx/texture.hpp, which needs abseil): declared here as mods.cpp does
namespace aurora::gx {
struct TextureStats {
    uint64_t uploadBytes = 0, hashedBytes = 0, objectHits = 0, contentHits = 0, replacementHits = 0, misses = 0, evictions = 0;
    uint64_t contentCacheBytes = 0, contentCacheEntries = 0, copyCacheBytes = 0, copyCacheEntries = 0, copyCacheHits = 0;
    uint64_t copyCacheMisses = 0, copyCacheEvictions = 0, currentCopyBytes = 0, pendingLoads = 0, publishes = 0, publishBytes = 0;
};
const TextureStats& texture_stats() noexcept;
}  // namespace aurora::gx

namespace rt::gfx {
namespace fifo = aurora::gx::fifo;

namespace {

bool s_enabled = false;
bool s_inFrame = false;
u64 s_lastFrameTb = 0;
u64 s_frames = 0;
u64 s_vblanks = 0;    // retraces seen (R2_GX_SKIP windows)
u32 s_frameDraws = 0;  // draws so far in this presented frame (R2_GX_SKIP i<from>-<to>)
bool s_topCopied = false;  // this frame already has a display copy at the XFB's first line

// --- raw state shadowed from the stream ---
u32 s_bpMask = 0xFFFFFF;
u32 s_bp[256] = {};
u32 s_vcdLo = 0, s_vcdHi = 0, s_vatA[8] = {}, s_vatB[8] = {}, s_vatC[8] = {};
u32 s_cpMtxA = 0;         // CP 0x30: position matrix index (draws without per-vertex indices)
float s_xfMtx[256] = {};  // XF 0x000-0x0FF: position/texture matrices (where sprites are, drawExtentX)
// RL-NX: a position matrix, kept inside that shadow. `(s_cpMtxA & 0x3F) * 4` reaches word 252, so its
// m[8..11] read past the end, and a per-vertex matrix index byte reaches 255 (85 * 12); both handed the
// measurer whatever followed the array - which is where 9.3's 1e13 matrices and 1e33 extents came from,
// and why the same quads measured sanely in one run and not in the next. nullptr: not measurable.
const float* xfPosMatrix(u32 word) {
    // RL-NX: GX_IDENTITY is matrix 60, i.e. XF words 240..251, and the hardware hardwires it: no game
    // writes it, and the .dff ground truth of the title page has **zero** writes to XF 0x00F0 (index 60
    // arrives as CP 0x30 = 0x3CF3CF3C, low six bits 0x3C). The shadow there is therefore all zeros, so
    // every draw that uses the identity matrix measured as "behind the eye" (its x/y/z rows all zero, and
    // a perspective projection divides by w = -z = 0) or as an empty strip under an orthographic one -
    // that is the 330 behind-the-eye draws of 9.3 and the 1184-draw all-behind frames of 9.5, not a
    // renderer fault. Aurora returns its own identity for that index (position_matrix(), MaxPnMtx = 10,
    // GX_IDENTITY row at 20), so the two must agree here as well. R2_NO_IDENT_MTX=1 restores the read.
    static const float kIdentity[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    static const bool noIdent = envFlag("R2_NO_IDENT_MTX");
    if (!noIdent && word == 240) return kIdentity;
    // RL-NX: R2_NO_MTX_GUARD=1 restores the unguarded read (the 1e13 matrices of the 19:00 runs), so the
    // read that is being suspected can be reproduced with the same binary.
    static const bool unguarded = envFlag("R2_NO_MTX_GUARD");
    if (unguarded) return &s_xfMtx[word];
    return word + 12 <= 256 ? &s_xfMtx[word] : nullptr;
}
// Everything the stream set, for a save state: after a load it is replayed (bridgeAfterLoad)
std::bitset<256> s_bpValid;   // BP registers written so far
u32 s_cp[256] = {};           // CP registers as written
std::bitset<256> s_cpValid;
constexpr u32 kXfShadow = 0x1058;  // XF memory (matrices, lights) and registers
u32 s_xfMem[kXfShadow] = {};
u16 s_xfBlock[kXfShadow] = {};     // XF loads by start address: their length (Aurora takes blocks)

struct Array {
    u32 base = 0;       // physical
    u32 stride = 0;
    u32 sentSize = 0;   // size Aurora has; 0 = not sent since the base changed
};
Array s_arrays[16];

struct TexMap {
    bool dirty = true;
    u64 frame = ~0ull;  // frame the map was last sent
};
TexMap s_tex[8];

struct TlutLoad {
    u32 addr = 0, entries = 0;
};
std::unordered_map<u32, TlutLoad> s_tlutLoads;  // TMEM offset -> last load
struct TlutSlot {
    u32 tmem = ~0u, fmt = 0, addr = 0, entries = 0, id = 0;
};
TlutSlot s_tlutSlots[16];
u32 s_tlutNext = 0;
std::unordered_map<u64, u32> s_tlutIds;  // content key -> Aurora TLUT object id
u32 s_nextTlutId = 1;

inline u32 be16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }
inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

void aur(u16 sub) {
    fifo::write_u8(GX_AURORA);
    fifo::write_u16(sub);
}

// ---------------------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------------------
u64 s_lastPollTb = 0;

// RL-NX (audit 2026-11-27): ASYNC VBLANK. vblank() used to drain the whole bridge ring every frame so that the
// game thread's input/hotkey/SDL work never ran beside the worker's own pollEvents (3.6 ms a frame on level 22,
// the whole of gxsync). With ASYNC VBLANK on the two are serialized by this mutex instead and the drain is gone.
std::recursive_mutex s_inputMx;

void pollEvents() {
    std::lock_guard<std::recursive_mutex> inputLock(s_inputMx);
    s_lastPollTb = tbNow();
    const AuroraEvent* e = aurora_update();
    for (; e && e->type != AURORA_NONE; ++e) {
        if (e->type == AURORA_EXIT) {
            logf("gfx", "window closed");
            quitToDesktop();
        }
        if (e->type == AURORA_SDL_EVENT) {
            menuEvent(&e->sdl);
            touchEvent(&e->sdl);
            audio::deviceEvent(&e->sdl);
        }
        // F11 or Alt+Enter: toggle fullscreen
        if (e->type == AURORA_SDL_EVENT && e->sdl.type == SDL_EVENT_KEY_DOWN && !e->sdl.key.repeat &&
            (e->sdl.key.key == SDLK_F11 || (e->sdl.key.key == SDLK_RETURN && (e->sdl.key.mod & SDL_KMOD_ALT)))) {
            if (SDL_Window* w = aurora_get_window()) {
                const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(w, !full);
                settings::get().fullscreen = !full;
                settings::save();
            }
        }
    }
}

// Every pipeline in the cache - each one the game has drawn with on this PC - is built before
// the game starts (the player's choice: nothing seen before pops in while its shader compiles;
// Aurora skips a draw until its pipeline exists). Aurora queues them all when it initializes
// (rebuild_pipeline_cache, in the order the game first used them); with Dawn's blob cache that
// takes ~2 s, after a graphics driver update each one compiles again (~25 ms on Aurora's one
// worker: ~55 s for 2,100; patch 0020's 8 workers: ~10 s). The texture packs' prefetch (mods.cpp,
// setting prefetch_textures) runs meanwhile and is waited for too. The setting
// precompile_shaders off (Display page) or R2_NO_PRECOMPILE=1 starts the game at once: the
// queue is still built, on the workers while the game runs, and a draw whose pipeline is not
// built yet is skipped (or drawn by the ubershader, if that is on) until it is.
void precompileShaders() {
    const AuroraStats* st = aurora_get_stats();
    if (!st) return;
    if (envFlag("R2_NO_PRECOMPILE") || !settings::get().precompileShaders) {
        logf("gfx", "shaders not precompiled (setting): %u from the cache compile while the game runs",
             st->createdPipelines + st->queuedPipelines);
        return;
    }
    const u64 t0 = tbNow();
    u32 total = 0;
    bool waited = false;
    for (;;) {
        const u32 queued = st->queuedPipelines;
        const auto tex = aurora::texture::prefetch_status();
        if (queued == 0 && tex.done >= tex.total) break;
        waited = true;
        const u32 done = st->createdPipelines;
        total = std::max(total, done + queued);
        pollEvents();
        if (aurora_begin_frame()) {
            drawShaderProgress(done, total, tex.done, tex.total, tex.bytes);
            aurora_end_frame();
        }
        SDL_Delay(16);
    }
    if (!waited) return;
    const auto tex = aurora::texture::prefetch_status();
    logf("gfx", "before the game started (%.1f s): %u pipelines built, %llu textures prefetched (%.1f GiB%s)",
         double(tbNow() - t0) / double(kTbClock), st->createdPipelines, (unsigned long long)tex.total,
         double(tex.bytes) / double(1ull << 30), tex.budgetFull ? ", memory budget reached" : "");
}

// The launcher: the settings menu (menu.cpp, in a launcher mode) before the game boots, so
// everything - display, texture packs, shader preparation, controls, achievements - can be set
// before anything of the game runs. On by default (settings::launcher, the first page's "Show
// launcher at start"); R2_LAUNCHER=1/0 overrides (a restart sets it: a reset goes straight into
// the game, "Back to launcher" here), then automated runs skip it, then --launcher /
// --no-launcher, then the setting. Aurora is up meanwhile: its workers build the cached
// pipelines and prefetch textures while the player looks at the settings.
bool launcherWanted(int argc, char** argv) {
    if (const char* e = getenv("R2_LAUNCHER"); e && *e) return *e == '1';
    if (envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT") || envFlag("R2_MENU")) return false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--launcher")) return true;
        if (!strcmp(argv[i], "--no-launcher")) return false;
    }
    return settings::get().launcher;
}

void runLauncher() {
    logf("gfx", "launcher open");
    const u64 t0 = tbNow();
    launcherOpen();
    for (;;) {
        pollEvents();  // (closing the window quits)
        ra::idle();    // a login from the ACHIEVEMENTS page
        bool start = false;
        if (aurora_begin_frame()) {
            start = launcherFrame();
            aurora_end_frame();
        }
        if (start) break;
        SDL_Delay(8);
    }
    logf("gfx", "launcher: game started after %.1f s; settings: %s", double(tbNow() - t0) / double(kTbClock),
         settings::summary().c_str());
}

void testQuad();
bool s_testClear = false;  // R2_GFX_TEST: clear each frame to magenta (is presentation working?)
bool s_xfb = true;         // R2_NO_XFB: present the EFB instead of the emulated XFB
bool s_copyLog = false;  // R2_COPY_LOG: every display copy and where it lands
// RL-NX: the diagnosis switches added for the "assets missing" bug (see PERF_HANDOFF 9.8):
//   R2_STATS=1    per-frame outcome counters for draws and texture binds (enders, not screenshots)
//   R2_TEX_FLAT=1 every texture is sent as a single 8x8 tile, so each asset samples one flat colour
//   R2_STRICT=1   the identity heuristics off: no draw merging, every bind rehashed, palettes part of the
//                 texture's identity, display-copy destinations no longer permanently "still the copy"
bool s_statsOn = false;
// RL-NX: the - menu's switch (switch/src/menu_switch.cpp), so the counters can be turned on and off in game
// instead of with R2_STATS=1 and a restart.
bool s_statsMenu = false;
bool s_flatOn = false;
bool s_strictOn = false;
// RL-NX: R2_STATS - the renderer's command backlog in bytes, sampled at the end of each frame before the
// drain (see endFrame) and reported by statsFrameEnd. Nothing here is on the hot path when R2_STATS is off.
u32 s_statsBacklog = 0;

// Aurora renders on its own thread (GX_PROCESSING_THREAD); gpSync waits until it has caught
// up with every command sent. The game sees the GPU's progress only through draw done, draw
// sync tokens and the CP's status and pointers: syncing there keeps memory the game reuses
// afterwards (vertex arrays, textures) from being read late. R2_GX_THREAD=0 renders inline.
bool s_gxThread = false;

// ---------------------------------------------------------------------------------------
// RL-NX (PERF_HANDOFF 9.53): the bridge worker. Everything this file does with a GX command (onCommand: the
// shadow state, arrays, texture identity and hashing, Aurora's FIFO writes, the frame boundary) ran on the game
// thread, the one thread the port is limited by (20:14 run: 95 % busy, 7.4 ms a frame in the GX path while the
// FIFO and render threads idled at 19 % and 11 %). gx_fifo.cpp still splits the stream on the game thread - it
// owns the CP/PI registers, breakpoints and the PE events the game sees - but the commands it splits now go into
// a ring that a worker drains into onCommand.
//
// One producer (the game thread), one consumer (the worker), and the rule that keeps the bridge single-threaded
// in effect: the game thread calls into this file only after bridgeDrain(), which waits until the worker has
// run every command already queued. The worker cannot get new work while the game thread is in here, because
// the game thread is the only one that queues. So each piece of state still has one thread touching it at a
// time; only *which* thread changes. Drains happen where the game can observe the GPU or reaches into the
// bridge: CP status reads (gpSync, as before), PE draw-done/tokens that raise an interrupt (hw.cpp ->
// gxPeBarrier, before the game can reuse the memory behind earlier draws), PE register access after a token
// without one, every vblank (input, window events, the menu's presses), save states, setMenuTiling and stats().
// R2_BRIDGE_THREAD=0 starts with it off; the - menu's GX THREAD row switches it (applied at the next vblank).
void onCommand(const u8* p, u32 len);  // below

constexpr u32 kRingBytes = 8u << 20;   // commands queued ahead of the worker (a heavy frame is ~1-2 MiB)
constexpr u32 kWrapMark = 0xFFFFFFFFu;  // "the rest of the ring is unused, continue at its start"
u8* s_ring = nullptr;
std::atomic<u64> s_ringWr{0}, s_ringRd{0};  // bytes written / consumed since start (position = value % size)
std::atomic<bool> s_async{false};           // the stream goes through the ring (changed by the game thread)
std::atomic<int> s_asyncWant{-1};           // a state requested by the menu (any thread); -1 none
std::atomic<bool> s_workerSleeping{false};
std::atomic<u64> s_waitFor{~0ull};          // the game thread sleeps until s_ringRd reaches this
std::mutex s_ringMx;
std::condition_variable s_workCv, s_doneCv;
pthread_t s_worker;
bool s_workerUp = false;
std::atomic<bool> s_workerStop{false};  // RL-NX (2026-11-27): bridgeShutdown
bool s_peLazy = false;  // a token without an interrupt was queued: drain before the game touches PE
// RL-NX (9.55): batched publish. ringPush used to publish every command with a seq_cst store and a seq_cst load
// of s_workerSleeping - two full barriers per GX command, ~30k commands a frame on the one thread the port is
// limited by (9.54: bridgeSink/ringPush 1.33 ms a frame). Commands are now copied in at s_wrLocal (game thread
// only) and published together once kPublishBytes have piled up, and always before the game thread waits for
// the worker (bridgeDrain, a full ring), so no wait can miss a command. The worker sees work a little later and
// in bigger pieces; it is at ~34 % busy, so that costs nothing.
u64 s_wrLocal = 0;                       // game thread: bytes written, published or not
constexpr u64 kPublishBytes = 16u << 10;  // publish after this much
// RL-NX (9.57): GP thread records. A record's 4-byte header is the command length, or one of these: raw CP bytes
// for gx_fifo.cpp's splitter (gxGpParse) with the length in the low bits, or "drop the partial command".
constexpr u32 kRawBit = 0x40000000u;
constexpr u32 kDlBit = 0x20000000u;  // a copied display list (gxGpParseDl)
constexpr u32 kResetMark = 0xFFFFFFFEu;
// The raw record still open for appending (absolute ring position of its header), or ~0. Only an unpublished
// record grows: the worker reads a header once it is published, so ringPublish closes it.
u64 s_rawOpen = ~0ull;
void ringPublish() {
    s_rawOpen = ~0ull;  // RL-NX (9.57)
    if (s_ringWr.load(std::memory_order_relaxed) == s_wrLocal) return;
    s_ringWr.store(s_wrLocal, std::memory_order_release);
    // Dekker with the worker's sleep: it sets s_workerSleeping and then re-reads s_ringWr (both seq_cst); this
    // fence orders the store above before the load below, so one of the two always sees the other.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (s_workerSleeping.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(s_ringMx);
        s_workCv.notify_one();
    }
}

bool onWorker() { return s_workerUp && pthread_equal(pthread_self(), s_worker); }
inline void cpuRelax() {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

// Game thread: sleep until the worker has consumed up to `pos`.
void waitConsumed(u64 pos) {
    for (int i = 0; i < 4000; i++) {  // a short spin: the worker is usually just behind
        if (s_ringRd.load(std::memory_order_acquire) >= pos) return;
        cpuRelax();
    }
    std::unique_lock<std::mutex> lk(s_ringMx);
    s_waitFor.store(pos, std::memory_order_seq_cst);
    if (s_workerSleeping.load(std::memory_order_seq_cst)) s_workCv.notify_one();
    while (s_ringRd.load(std::memory_order_seq_cst) < pos) s_doneCv.wait_for(lk, std::chrono::milliseconds(100));
    s_waitFor.store(~0ull, std::memory_order_seq_cst);
}

// RL-NX (audit 2026-11-27): the wait for Aurora's FIFO thread; counted by cause only (not in RLPM_GXSYNC).
static inline void rlPmAddDrainOnly(int cause, u64 t0) {
    rl_pm.drainTicks[cause] += rlPmTick() - t0;
    rl_pm.drainCalls[cause]++;
}

// RL-NX (audit 2026-11-27): a wait of `cause` (RLPM_DRAIN_*) that began at t0: counted in RLPM_GXSYNC as before, and
// by cause, so the [pm] line says who the game thread was waiting for.
static inline void addDrain(int cause, u64 t0) {
    const u64 dt = rlPmTick() - t0;
    rl_pm.timerTicks[RLPM_GXSYNC] += dt;
    rl_pm.timerCalls[RLPM_GXSYNC]++;
    rl_pm.drainTicks[cause] += dt;
    rl_pm.drainCalls[cause]++;
}

// Game thread: every queued command has run (the worker is then idle until the next push). `cause` is only for
// the counters; it is the first argument so the callers read as "drain for X".
void bridgeDrain(int cause = RLPM_DRAIN_STATE) {
    if (!s_async.load(std::memory_order_relaxed) || onWorker()) return;
    ringPublish();  // RL-NX (9.55): everything queued is visible before waiting for it
    const u64 w = s_wrLocal;
    if (s_ringRd.load(std::memory_order_acquire) >= w) return;
    const u64 t0 = rlPmTick();
    waitConsumed(w);
    addDrain(cause, t0);
}
// gx_fifo.cpp's CP-read hook takes a plain void(): the same drain, counted as a CP read.
void bridgeDrainCpRead() { bridgeDrain(RLPM_DRAIN_CPREAD); }

// RL-NX (Stage 11): CP LAG (gx_fifo.cpp s_cpLag): the game thread's write position, published or not, and the worker's
// read position. Raw records consumed means their commands and the lists they call have run.
u64 ringWritePos() { return s_wrLocal; }
u64 ringReadPos() { return s_ringRd.load(std::memory_order_acquire); }

// RL-NX (9.57): `tag` is 0 for a command, or kRawBit / kResetMark (gx_fifo.cpp's raw CP bytes for the GP thread).
void ringPush(const u8* p, u32 len, u32 tag = 0) {
    s_rawOpen = ~0ull;
    const u32 need = (4 + len + 3) & ~3u;
    if (need > kRingBytes / 2) {  // never seen; run it here rather than size the ring for it
        bridgeDrain(RLPM_DRAIN_BIGCMD);
        if (tag == kRawBit) gxGpParse(p, len);  // RL-NX (9.57): the worker is idle, so the splitter may run here
        else if (tag == kDlBit) gxGpParseDl(p, len);
        else onCommand(p, len);
        return;
    }
    u64 w = s_wrLocal;  // RL-NX (9.55): the unpublished write position
    u32 pos = u32(w % kRingBytes);
    const u32 tail = kRingBytes - pos;  // >= 4: positions are 4-aligned
    const u32 total = need <= tail ? need : tail + need;
    if (w + total - s_ringRd.load(std::memory_order_acquire) > kRingBytes) {
        const u64 t0 = rlPmTick();
        ringPublish();  // RL-NX (9.55): the worker can only free what it can see
        waitConsumed(w + total - kRingBytes);  // full: wait for the worker to free what this needs
        addDrain(RLPM_DRAIN_RINGFULL, t0);
    }
    if (need > tail) {
        memcpy(s_ring + pos, &kWrapMark, 4);
        w += tail;
        pos = 0;
    }
    const u32 head = tag == kResetMark ? kResetMark : len | tag;  // RL-NX (9.57)
    memcpy(s_ring + pos, &head, 4);
    if (len) memcpy(s_ring + pos + 4, p, len);
    s_wrLocal = w + need;
    if (tag == kRawBit) s_rawOpen = w;  // RL-NX (9.57): the next raw bytes may join it
    if (s_wrLocal - s_ringWr.load(std::memory_order_relaxed) >= kPublishBytes) ringPublish();  // RL-NX (9.55)
}

// RL-NX (9.57): gx_fifo.cpp's chunk sink (gxSetGpThread). The walker hands over runs of whole commands, often a
// burst or less at a time, so a run joins the open raw record when it can: one header per publish, not per run.
// The record's payload ends at header + 4 + len; the ring position after it is that rounded up to 4.
void gpChunkSink(const u8* p, u32 n) {
    if (!p) return ringPush(nullptr, 0, kResetMark);
    if (s_rawOpen != ~0ull) {
        const u32 hpos = u32(s_rawOpen % kRingBytes);
        u32 head;
        memcpy(&head, s_ring + hpos, 4);
        const u32 len = head & ~kRawBit;
        const u64 newEnd = s_rawOpen + ((4 + u64(len) + n + 3) & ~3ull);
        if (u64(hpos) + 4 + len + n <= kRingBytes && len + n < kDlBit &&
            newEnd - s_ringRd.load(std::memory_order_acquire) <= kRingBytes) {
            memcpy(s_ring + hpos + 4 + len, p, n);
            head = (len + n) | kRawBit;
            memcpy(s_ring + hpos, &head, 4);
            s_wrLocal = newEnd;
            if (s_wrLocal - s_ringWr.load(std::memory_order_relaxed) >= kPublishBytes) ringPublish();
            return;
        }
    }
    ringPush(p, n, kRawBit);
}
// RL-NX (9.57): a display list the walker copied, in place of its call
void gpDlSink(const u8* p, u32 n) { ringPush(p, n, kDlBit); }

void* workerMain(void*) {
#ifdef __SWITCH__
    rlnxBridgeThreadStarted();  // switch/src/perf_switch.cpp: core, priority, profiler, PERF LOG thread list
#endif
    u64 r = s_ringRd.load(std::memory_order_relaxed);
    for (;;) {
        u64 w = s_ringWr.load(std::memory_order_acquire);
        if (r == w) {
            for (int i = 0; i < 20000 && r == w; i++) {  // tens of us: the next command is usually close
                cpuRelax();
                w = s_ringWr.load(std::memory_order_acquire);
            }
            if (r == w) {
                if (s_workerStop.load(std::memory_order_acquire)) return nullptr;  // RL-NX: bridgeShutdown
                std::unique_lock<std::mutex> lk(s_ringMx);
                s_workerSleeping.store(true, std::memory_order_seq_cst);
                if (s_ringWr.load(std::memory_order_seq_cst) == r) s_workCv.wait_for(lk, std::chrono::milliseconds(100));
                s_workerSleeping.store(false, std::memory_order_seq_cst);
                continue;
            }
        }
        while (r != w) {
            const u32 pos = u32(r % kRingBytes);
            u32 len;
            memcpy(&len, s_ring + pos, 4);
            if (len == kWrapMark) {
                r += kRingBytes - pos;
                continue;
            }
            // RL-NX (9.57): GP thread records (raw CP bytes, partial-command drop)
            if (len == kResetMark) {
                gxGpParse(nullptr, 0);
                len = 0;
            } else if (len & kRawBit) {
                len &= ~kRawBit;
                gxGpParse(s_ring + pos + 4, len);
            } else if (len & kDlBit) {
                len &= ~kDlBit;
                gxGpParseDl(s_ring + pos + 4, len);
            } else {
                onCommand(s_ring + pos + 4, len);
            }
            r += (4 + len + 3) & ~3u;
            s_ringRd.store(r, std::memory_order_seq_cst);
            if (r >= s_waitFor.load(std::memory_order_seq_cst)) {
                std::lock_guard<std::mutex> lk(s_ringMx);
                s_doneCv.notify_all();
            }
        }
    }
    return nullptr;
}

// gx_fifo.cpp's sink: queue the command, or run it here with the worker off.
void bridgeSink(const u8* cmd, u32 len) {
    // RL-NX (9.57): with the GP thread on, the splitter itself runs on the worker: its commands go straight in
    if (s_async.load(std::memory_order_relaxed) && !onWorker()) ringPush(cmd, len);
    else onCommand(cmd, len);
}

// RL-NX (9.57): the GP thread's hooks (gx_fifo.h gxSetGpThread) and its - menu row (GP THREAD).
std::atomic<int> s_gpWant{-1};  // a state requested by the menu (any thread); -1 none
std::atomic<int> s_gpRawWant{-1};  // RL-NX (Stage 6): the - menu's GP RAW row, as s_gpWant
std::atomic<int> s_cpLagWant{-1};  // RL-NX (Stage 11): the - menu's CP LAG row, as s_gpWant
std::atomic<int> s_avWant{-1};     // RL-NX (audit 2026-11-27): the - menu's ASYNC VBLANK row, as s_gpWant
bool s_asyncVblank = false;        // game thread only; set from R2_ASYNC_VBLANK, then by the menu row
bool s_avInit = false;
void gpKick() {
    if (s_async.load(std::memory_order_relaxed)) ringPublish();
}
void gpDrainAll();  // below (gpSync's full drain)
void gpFence() {
    if (s_gxThread && s_inFrame) fifo::drain();  // the worker owns the frame; Aurora's thread reads guest memory
}

void bridgeStartWorker() {
    if (s_workerUp) return;
    s_ring = static_cast<u8*>(malloc(kRingBytes));
    if (!s_ring) {
        logf("gfx", "bridge worker: no memory for the %u MiB ring, staying on the game thread", kRingBytes >> 20);
        return;
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);  // onCommand's diagnostics format on the stack
    s_workerUp = pthread_create(&s_worker, &at, workerMain, nullptr) == 0;
    pthread_attr_destroy(&at);
    if (!s_workerUp) {
        logf("gfx", "bridge worker: pthread_create failed, staying on the game thread");
        return;
    }
    const char* e = getenv("R2_BRIDGE_THREAD");
    const bool on = !(e && *e == '0');
    s_async.store(on, std::memory_order_relaxed);
    logf("gfx", "bridge worker: started, GX commands %s (R2_BRIDGE_THREAD / - menu GX THREAD)",
         on ? "decoded on the worker" : "decoded on the game thread");
    // RL-NX (9.57): the GP thread - on unless R2_GP_THREAD=0, and never in a deterministic run (its PE events
    // arrive when the worker gets there, not at a fixed point of the guest's execution). Oracle probes are
    // unaffected: their gather pipe is silent (gxFifoWrite).
    gxSetGpThread(gpChunkSink, gpDlSink, gpKick, gpDrainAll, gpFence);
    gxSetGpDrain(bridgeDrainCpRead);  // RL-NX (Stage 6): raw mode's CP register reads wait for the worker
    gxSetGpProgress(ringWritePos, ringReadPos);  // RL-NX (Stage 11): CP LAG's progress
    const char* g = getenv("R2_GP_THREAD");
    const bool gp = on && !(g && *g == '0') && !envFlag("R2_DETERMINISTIC");
    gxGpSetAsync(gp);
    logf("gfx", "GP thread: %s (R2_GP_THREAD / - menu GP THREAD)", gp ? "on, the CP stream is split on the worker" : "off");
    logf("gfx", "GP raw split: %s (R2_GP_RAW / - menu GP RAW)", gxGpRaw() ? "on, the worker splits the raw CP bytes" : "off, the game thread walks");  // RL-NX (Stage 6)
}

// RL-NX (2026-11-27): the Switch relaunch (the updater's restart): lets the worker finish everything queued, ends it and joins it.
// Game thread. After this every GX command runs on the calling thread.
void bridgeShutdown() {
    if (!s_workerUp) return;
    bridgeDrain(RLPM_DRAIN_STATE);
    s_workerStop.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(s_ringMx); }
    s_workCv.notify_all();
    pthread_join(s_worker, nullptr);
    s_workerUp = false;
    s_async.store(false, std::memory_order_relaxed);
    gxGpSetAsync(false);
    logf("gfx", "bridge worker ended for the relaunch");
}

#if defined(__SWITCH__)
// RL-NX (Stage 12): HD TEXTURES (- menu rows HD TEXTURES and HD TEX 2048; R2_HD_TEXTURES, R2_HD_LIMIT). Dolphin-format
// texture packs (tex1_<w>x<h>_<hash>_<fmt>.dds / .png, any folder layout) under sdmc:/switch/RogueLeader-NX/mods/hd are
// registered with Aurora's replacement registry, the one the desktop Red Two uses for Dolphin packs: Aurora hashes
// each game texture as Dolphin does and streams a matching file in on its texture workers (the game's own texture
// until it is ready). HD TEX 2048 (on by default) has Aurora drop the top mip levels of anything larger than 2048
// pixels (aurora::texture::set_replacement_max_size): the GSWE64 pack's 4096x4096 BC7 textures are 22 MiB each with
// their chain, a level's worth would not fit beside the game, and 720p/1080p never samples those levels. Switched
// at a vblank on the game thread; the folder scan of a 1,200-file pack takes about a second.
std::atomic<int> s_hdWant{-1}, s_hdLimitWant{-1};
bool s_hdOn = false, s_hdLimit = true;
aurora::texture::ReplacementGroup s_hdGroup;
constexpr const char* kHdDir = "sdmc:/switch/RogueLeader-NX/mods/hd";
void hdApply(bool on) {
    if (s_hdOn) {
        aurora::texture::unregister_replacements(s_hdGroup);
        aurora::texture::clear_prefetched_replacements();
        s_hdGroup = {};
        s_hdOn = false;
    }
    aurora::texture::set_replacement_max_size(s_hdLimit ? 2048u : 0u);
    if (!on) {
        logf("mods", "HD textures: off");
        return;
    }
    const u64 t0 = rlPmTick();
    s_hdGroup = aurora::texture::load_replacement_directory(kHdDir);
    s_hdOn = true;
    logf("mods", "HD textures: %zu textures registered from %s in %.0f ms (largest size %s; - menu HD TEXTURES / HD TEX 2048)",
         s_hdGroup.registrations.size(), kHdDir, double(rlPmTick() - t0) / 19200.0, s_hdLimit ? "2048" : "as shipped");
}
// RL-NX (Stage 12): a [mods] line every 30 s while a pack is registered: replacements bound, loaded (and how
// much), still loading - the counters Aurora keeps per frame (read before and after aurora_end_frame, as mods.cpp does)
u64 s_hdHits = 0, s_hdLoaded = 0, s_hdBytes = 0;
void hdFrame(bool ended) {
    if (!s_hdOn) return;
    const auto& st = aurora::gx::texture_stats();
    if (!ended) {
        s_hdHits += st.replacementHits;
        return;
    }
    s_hdLoaded += st.publishes;
    s_hdBytes += st.publishBytes;
    static u64 last = 0;  // every 30 s of the system counter (19.2 MHz)
    const u64 now = rlPmTick();
    if (!last) last = now;
    if (now - last < 30ull * 19200000ull) return;
    last = now;
    logf("mods", "HD textures: %llu binds replaced, %llu loaded (%llu MiB) so far, %llu loading",
         (unsigned long long)s_hdHits, (unsigned long long)s_hdLoaded, (unsigned long long)(s_hdBytes >> 20),
         (unsigned long long)st.pendingLoads);
}
void hdStart() {
    const char* lim = getenv("R2_HD_LIMIT");
    s_hdLimit = !(lim && *lim == '0');
    const char* e = getenv("R2_HD_TEXTURES");
    hdApply(!(e && *e == '0'));
}
#endif

// Game thread: apply the menu's request (at a vblank).
void bridgeApplyWanted() {
    // RL-NX (audit 2026-11-27): ASYNC VBLANK - only changes how vblank() waits, so no drain is needed to switch it
    if (const int av = s_avWant.exchange(-1, std::memory_order_relaxed); av >= 0 && bool(av) != s_asyncVblank) {
        s_asyncVblank = av != 0;
        logf("gfx", "async vblank: now %s", s_asyncVblank ? "on (no per-frame drain; input serialized by a lock)" : "off (the game thread drains the bridge every vblank)");
    }
#if defined(__SWITCH__)
    // RL-NX (Stage 12): HD TEXTURES / HD TEX 2048 (a limit change reloads the pack so the new size applies)
    {
        const int lw = s_hdLimitWant.exchange(-1, std::memory_order_relaxed);
        const int hw = s_hdWant.exchange(-1, std::memory_order_relaxed);
        const bool limitChanged = lw >= 0 && bool(lw) != s_hdLimit;
        if (limitChanged) s_hdLimit = lw != 0;
        if ((hw >= 0 && bool(hw) != s_hdOn) || (limitChanged && s_hdOn)) hdApply(hw >= 0 ? hw != 0 : s_hdOn);
    }
#endif
    // RL-NX (9.57): GP THREAD first; it needs the bridge worker, so GX THREAD off also turns it off
    const int gpWant = s_gpWant.exchange(-1, std::memory_order_relaxed);
    if (gpWant >= 0 && s_workerUp && bool(gpWant) != gxGpAsync()) {
        bridgeDrain(RLPM_DRAIN_MENU);
        gxGpSetAsync(gpWant != 0 && s_async.load(std::memory_order_relaxed));
        logf("gfx", "GP thread: now %s", gxGpAsync() ? "on" : "off");
    }
    // RL-NX (Stage 6): GP RAW - the worker splits the raw CP bytes (gx_fifo.cpp s_gpRaw); switched with it drained
    const int rawWant = s_gpRawWant.exchange(-1, std::memory_order_relaxed);
    if (rawWant >= 0 && bool(rawWant) != gxGpRaw()) {
        bridgeDrain(RLPM_DRAIN_MENU);
        gxGpSetRaw(rawWant != 0);
        logf("gfx", "GP raw split: now %s", gxGpRaw() ? "on (the worker splits)" : "off (the game thread walks)");
    }
    // RL-NX (Stage 11): CP LAG, switched with the worker drained (nothing pending)
    const int lagWant = s_cpLagWant.exchange(-1, std::memory_order_relaxed);
    if (lagWant >= 0 && bool(lagWant) != gxCpLag()) {
        bridgeDrain(RLPM_DRAIN_MENU);
        gxSetCpLag(lagWant != 0);
        logf("gfx", "CP lag: now %s", gxCpLag() ? "on (CP reads report the worker's progress)" : "off (CP reads drain the worker)");
    }
    const int want = s_asyncWant.exchange(-1, std::memory_order_relaxed);
    if (want < 0 || !s_workerUp || bool(want) == s_async.load(std::memory_order_relaxed)) return;
    bridgeDrain(RLPM_DRAIN_MENU);
    if (!want && gxGpAsync()) gxGpSetAsync(false);  // RL-NX (9.57)
    s_async.store(want != 0, std::memory_order_relaxed);
    logf("gfx", "bridge worker: GX commands now %s", want ? "decoded on the worker" : "decoded on the game thread");
}

// RL-NX (9.57): everything queued has run, the renderer's thread included (gpSync before the GP thread).
void gpDrainAll() {
    if (onWorker()) return;
    bridgeDrain(RLPM_DRAIN_GPALL);
    if (s_gxThread && s_inFrame) {
        const u64 t0 = rlPmTick();
        fifo::drain();
        rlPmAddDrainOnly(RLPM_DRAIN_FIFO, t0);
    }
}

void gpSync() {
    // RL-NX (9.53): the in-stream draw-done/token syncs (onCommand's BP case) arrive on the worker; the game
    // thread already drained at that point (peBarrier) or will before it touches PE, so nothing to do there.
    if (onWorker()) return;
    bridgeDrain(RLPM_DRAIN_GPSYNC);
    if (s_gxThread && s_inFrame) {
        const u64 t0 = rlPmTick();
        fifo::drain();
        rlPmAddDrainOnly(RLPM_DRAIN_FIFO, t0);
    }
}

// hw.cpp (through gx_fifo.cpp's barrier hook), game thread: 0 a token without an interrupt ran (drain lazily),
// 1 a PE event that raises an interrupt (drain now, before the game can react and reuse memory behind earlier
// draws), 2 the game reads or writes a PE register (drain if a lazy token is pending).
void peBarrier(int kind) {
    if (!s_async.load(std::memory_order_relaxed)) return;
    if (kind == 0) {
        s_peLazy = true;
        return;
    }
    if (kind == 2 && !s_peLazy) return;
    s_peLazy = false;
    gpSync();
}

void ensureFrame() {
    if (s_inFrame) return;
#if defined(__SWITCH__)
    // RL-NX (2026-11-27): the updater asked to leave (installed an update, or a restart): at a frame boundary, with no frame
    // open, end the threads and return to hbloader (relaunch_switch.cpp). Never returns.
    {
        if (rlnxRelaunchRequested()) {
            bridgeShutdown();
            rlnxRelaunchTeardown();
        }
    }
#endif
    pollEvents();
    s_inFrame = aurora_begin_frame();
    if (s_inFrame && s_testClear) {
        aur(GX_AURORA_CLEAR);
        fifo::write_u8(0x0F);  // whole target, color, alpha, depth
        for (int i = 0; i < 4; i++) fifo::write_u32(0);
        fifo::write_u8(255); fifo::write_u8(0); fifo::write_u8(255); fifo::write_u8(255);
        fifo::write_u32(0xFFFFFF);
        testQuad();
    }
}

// A green quad drawn purely with raw hardware registers, as the game's own GX code would.
void testQuad() {
    auto bp = [](u32 v) { fifo::write_u8(0x61); fifo::write_u32(v); };
    auto cp = [](u8 r, u32 v) { fifo::write_u8(0x08); fifo::write_u8(r); fifo::write_u32(v); };
    auto xf = [](u16 a, std::initializer_list<u32> vals) {
        fifo::write_u8(0x10);
        fifo::write_u32((u32(vals.size() - 1) << 16) | a);
        for (u32 v : vals) fifo::write_u32(v);
    };
    auto f = [](float x) { u32 u; memcpy(&u, &x, 4); return u; };
    cp(0x50, 0x2200);      // POS, CLR0 direct
    cp(0x60, 0);
    cp(0x70, 0x40016009);  // POS XYZ f32, CLR0 RGBA8
    xf(0x1008, {1});       // one color
    xf(0x1009, {1});
    xf(0x100E, {1});       // color0: vertex color, no lighting
    xf(0x1010, {1});
    xf(0x103F, {0});
    bp(0x00000010);        // genmode: 1 color, 0 texgens, 1 stage, no culling
    bp(0xC008FFFA);        // TEV color = RASC
    bp(0xC108FFD0);        // TEV alpha = RASA
    bp(0x2800003F);        // stage 0: no texture, color0
    bp(0x41000018);        // color+alpha update, no blending
    bp(0x40000000);        // no Z
    bp(0xF33F0000);        // alpha test always
    xf(0x0000, {f(1), 0, 0, 0, 0, f(1), 0, 0, 0, 0, f(1), 0});
    cp(0x30, 0);
    xf(0x1018, {0});
    xf(0x101A, {f(320), f(-240), f(16777215), f(660), f(580), f(16777215)});
    xf(0x1020, {f(2.f / 640), f(-1), f(-2.f / 480), f(1), 0, f(-0.5f), 1});
    bp(0x20154154);
    bp(0x213D3333);
    bp(0x590D5B56);  // scissor offset 340 in Aurora's (negated) convention
    fifo::write_u8(0x80);  // quads, format 0
    fifo::write_u16(4);
    const float vtx[4][2] = {{100, 100}, {540, 100}, {540, 380}, {100, 380}};
    for (auto& v : vtx) {
        fifo::write_u32(f(v[0]));
        fifo::write_u32(f(v[1]));
        fifo::write_u32(0);
        fifo::write_u32(0x00FF00FF);
    }
    fifo::finish_draw();
}

// R2_FRAME_LOG: every 10 s, presented frame intervals against the retrace period (how many
// overran it) and the time Aurora's end of frame (submit, present) took.
void frameLog(u64 endStart, u64 endDone) {
    // R2_FRAME_LOG=1: every 10 s; =<n> (2 or more): every n seconds
    static const int secs = getenv("R2_FRAME_LOG") ? std::max(1, atoi(getenv("R2_FRAME_LOG"))) : 0;
    if (!secs) return;
    static u64 last = 0, windowStart = 0, frames = 0, over = 0, maxInterval = 0, endSum = 0, endMax = 0, drawsSum = 0,
               drawsMax = 0;
    // Aurora's per-frame buffers, largest frame (its stats describe the last frame it finished)
    static u32 vertMax = 0, idxMax = 0, uniMax = 0, storMax = 0;
    const u64 period = u64(double(kTbClock) / viRate());
    if (last) {
        const u64 interval = endDone - last;
        if (interval > period + period / 2) over++;
        maxInterval = std::max(maxInterval, interval);
    }
    last = endDone;
    if (!windowStart) windowStart = endDone;
    frames++;
    endSum += endDone - endStart;
    endMax = std::max(endMax, endDone - endStart);
    drawsSum += s_frameDraws;
    drawsMax = std::max<u64>(drawsMax, s_frameDraws);
    if (const AuroraStats* st = aurora_get_stats()) {
        vertMax = std::max(vertMax, st->lastVertSize);
        idxMax = std::max(idxMax, st->lastIndexSize);
        uniMax = std::max(uniMax, st->lastUniformSize);
        storMax = std::max(storMax, st->lastStorageSize);
    }
    if (endDone - windowStart >= u64(secs == 1 ? 10 : secs) * kTbClock) {
        auto ms = [](u64 tb) { return double(tb) * 1000.0 / double(kTbClock); };
        logf("frame", "%llu frames (%.1f fps), %llu over 1.5 periods, longest %.2f ms; end of frame avg %.2f max %.2f ms; "
             "%llu draws/frame (max %llu); largest frame: verts %u KiB, indices %u KiB, uniforms %u KiB, storage %u KiB",
             (unsigned long long)frames, double(frames) * double(kTbClock) / double(endDone - windowStart),
             (unsigned long long)over, ms(maxInterval), ms(endSum) / double(frames), ms(endMax),
             (unsigned long long)(drawsSum / frames), (unsigned long long)drawsMax, vertMax >> 10, idxMax >> 10,
             uniMax >> 10, storMax >> 10);
        windowStart = endDone;
        frames = over = maxInterval = endSum = endMax = drawsSum = drawsMax = 0;
        vertMax = idxMax = uniMax = storMax = 0;
    }
}

void tileFrameEnd();
void drawBoundsFrameEnd();  // RL-NX: R2_DRAW_BOUNDS, the frame's union of draw extents
void drawListFrameEnd();    // RL-NX: R2_DRAW_LIST, the frame's buffered draws and its largest draw
void statsFrameEnd();       // RL-NX: R2_STATS, the frame's draw and bind outcomes

// RL-NX: R2_MATWATCH=<phys>,<len> (hex): does anything ever write this guest block? The cutscene-video
// draws index a position-matrix array in the game's heap (docs/PERF_HANDOFF.md 9.12) and every slot is
// zero both when LOAD_INDX reads it and at the end of the frame. Sampling the block at every frame end
// tells "nothing ever fills it" apart from "filled and zeroed again within a frame", which the guest
// store watch cannot do on the Switch (ctx.h RT_WATCHING is compiled out there). Read-only.
void matWatchFrameEnd() {
    static const char* spec = getenv("R2_MATWATCH");
    static u32 at = 0, len = 0;
    static std::vector<u8> prev;
    if (!spec) return;
    if (!at) {
        char* end;
        at = u32(strtoul(spec, &end, 16));
        len = *end == ',' ? u32(strtoul(end + 1, nullptr, 16)) : 0x40;
        if (!len || len > 0x10000) len = 0x40;
        prev.assign(len, 0xFF);
        logf("gfx", "matrix watch %08X+%X: sampled at every frame end", at, len);
    }
    const u8* p = physPtr(at, len);
    if (!p) return;
    u32 nz = 0;
    for (u32 o = 0; o + 4 <= len; o += 4) if (be32(p + o)) nz++;
    if (memcmp(p, prev.data(), len) == 0) {
        if (s_frames % 120 == 0)
            logf("gfx", "matrix watch frame %llu: %08X+%X unchanged, %u/%u words nonzero",
                 (unsigned long long)s_frames, at, len, nz, len / 4);
        return;
    }
    memcpy(prev.data(), p, len);
    char words[160];
    int n = snprintf(words, sizeof words, "%08X", be32(p));
    for (u32 o = 4, i = 1; o + 4 <= len && i < 8; o += 4, i++)
        n += snprintf(words + n, sizeof words - n, " %08X", be32(p + o));
    logf("gfx", "matrix watch frame %llu: %08X+%X changed, %u/%u words nonzero, first words: %s",
         (unsigned long long)s_frames, at, len, nz, len / 4, words);
}

void endFrame() {
    if (!s_inFrame) return;
    menuDraw();
    touchDraw();
    const u64 endStart = tbNow();
#ifdef __SWITCH__
    rlProfMark(1);  // RL-NX: the profiler's "game" section ends here
#endif
    // RL-NX: R2_STATS - how much of the command stream the renderer has not decoded yet, sampled where the
    // guest can no longer add to it. aurora_end_frame() drains the rest, so this is the window in which a
    // late decode of an indexed XF load can see memory the game has already rewritten.
    s_statsBacklog = s_statsOn ? fifo::get_buffer_size() : 0;
    mods::textureFrame(false);  // (Aurora's replacement counters reset at the end of a frame)
#if defined(__SWITCH__)
    hdFrame(false);  // RL-NX (Stage 12): HD TEXTURES counters
#endif
    aurora_end_frame();
    mods::textureFrame(true);
#if defined(__SWITCH__)
    hdFrame(true);  // RL-NX (Stage 12)
#endif
    // RL-NX: PERF LOG - what Aurora did with the frame just handed over (its stats describe that frame).
    if (const AuroraStats* st = aurora_get_stats()) {
        rl_pm.auDraws += st->drawCallCount;
        rl_pm.auMerged += st->mergedDrawCallCount;
        rl_pm.auTexUpload += st->lastTextureUploadSize;
        rl_pm.auSkipped = st->skippedDraws;
        rl_pm.auUber = st->uberDraws;
        rl_pm.auQueued = st->queuedPipelines;
        rl_pm.auCreated = st->createdPipelines;
    }
    rl_pm.level = peekGuest32(0x800962EC) >> 24;  // RL-NX: PERF LOG context (as profiler.cpp's perf line)
    const u64 endDone = tbNow();
#ifdef __SWITCH__
    rlProfMark(2);  // RL-NX: "present" = aurora_end_frame (recording hand-off to the render worker)
    rlProfMark(3);
    rlProfMark(0);  // the next frame opens
#endif
    frameLog(endStart, endDone);
    drawBoundsFrameEnd();  // RL-NX: R2_DRAW_BOUNDS
    drawListFrameEnd();    // RL-NX: R2_DRAW_LIST
    statsFrameEnd();       // RL-NX: R2_STATS
    matWatchFrameEnd();    // RL-NX: R2_MATWATCH
    if (s_frames % 30 == 0) gxDrawCallerLog(s_frames);  // RL-NX: R2_DRAW_CALLER, who submits the draws
    diagFramePresented(s_frameDraws, endDone - endStart);  // session log: perf lines, stall reports (profiler.cpp)
    tileFrameEnd();
    s_inFrame = false;
    s_frames++;
    s_frameDraws = 0;
    s_topCopied = false;
    s_lastFrameTb = tbNow();
}

// ---------------------------------------------------------------------------------------
// vertex arrays
// ---------------------------------------------------------------------------------------
u64 s_arraySends = 0, s_arrayBytes = 0;  // statistics (stats())

void sendArray(u32 n, u32 need) {
    Array& a = s_arrays[n];
    if (!a.base) return;
    if (a.sentSize && need <= a.sentSize) return;
    u32 size = a.sentSize ? a.sentSize : 256;
    while (size < need) size *= 2;
    const u32 maxSize = kMem1Mapped - (a.base & 0x01FFFFFF);
    if (size > maxSize) size = maxSize;
    u8* host = physPtr(a.base, size);
    if (!host) return;
    aur(u16(GX_AURORA_LOAD_ARRAYBASE | n));
    fifo::write_u64(reinterpret_cast<u64>(host));
    fifo::write_u32(size);
    fifo::write_u8(0);  // big-endian data
    a.sentSize = size;
    s_arraySends++;
    s_arrayBytes += size;
    rl_pm.arrayBytes += size;  // RL-NX: PERF LOG
}

u32 compSize(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : fmt == 4 ? 4 : 0; }
u32 colSize(u32 fmt) {
    static const u32 k[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return k[fmt & 7];
}

// Walk the vertices of a draw: grow each indexed array to cover the largest index used.
void prepareArrays(const u8* p) {
    const u32 fmt = p[0] & 7;
    const u32 count = be16(p + 1);
    const u32 lo = s_vcdLo, hi = s_vcdHi, A = s_vatA[fmt], B = s_vatB[fmt], C = s_vatC[fmt];
    struct Ref {
        u32 array, offset, bytes, count;  // count: indices per vertex for this attribute
    } refs[12];
    u32 nrefs = 0, off = 0;
    for (int i = 0; i < 9; i++)
        if (lo & (1u << i)) off += 1;
    auto attr = [&](u32 type, u32 array, u32 directBytes, u32 idxPerVtx) {
        if (type == 1) off += directBytes;
        else if (type >= 2) {
            const u32 b = type == 2 ? 1 : 2;
            refs[nrefs++] = {array, off, b, idxPerVtx};
            off += b * idxPerVtx;
        }
    };
    attr((lo >> 9) & 3, 0, compSize((A >> 1) & 7) * ((A & 1) ? 3 : 2), 1);
    const bool nbt = (A >> 9) & 1;
    attr((lo >> 11) & 3, 1, compSize((A >> 10) & 7) * (nbt ? 9 : 3), (nbt && (A >> 31)) ? 3 : 1);
    attr((lo >> 13) & 3, 2, colSize((A >> 14) & 7), 1);
    attr((lo >> 15) & 3, 3, colSize((A >> 18) & 7), 1);
    const u32 texCnt[8] = {(A >> 21) & 1, B & 1, (B >> 9) & 1, (B >> 18) & 1, (B >> 27) & 1, (C >> 5) & 1, (C >> 14) & 1, (C >> 23) & 1};
    const u32 texFmt[8] = {(A >> 22) & 7, (B >> 1) & 7, (B >> 10) & 7, (B >> 19) & 7, (B >> 28) & 7, (C >> 6) & 7, (C >> 15) & 7, (C >> 24) & 7};
    for (u32 t = 0; t < 8; t++) attr((hi >> (2 * t)) & 3, 4 + t, compSize(texFmt[t]) * (texCnt[t] ? 2 : 1), 1);
    // 8-bit indices (the common case) cannot pass 255: size those arrays for 256 entries at
    // once instead of scanning every draw's indices. 16-bit ones are scanned unless the array
    // already went up big enough for any index.
    u32 n = 0;
    for (u32 r = 0; r < nrefs; r++) {
        const Array& a = s_arrays[refs[r].array];
        const u32 stride = a.stride ? a.stride : 16;
        if (a.sentSize && a.sentSize >= (refs[r].bytes == 1 ? 256u : 65536u) * stride + 16) continue;
        refs[n++] = refs[r];
    }
    nrefs = n;
    if (!nrefs) return;
    // one tight loop per index (attribute outer, vertices inner: no per-vertex branching)
    const u32 vsize = off;
    for (u32 r = 0; r < nrefs; r++) {
        u32 maxIdx = 0;
        for (u32 k = 0; k < refs[r].count; k++) {
            const u8* q = p + 3 + refs[r].offset + k * refs[r].bytes;
            if (refs[r].bytes == 1) {
                for (u32 i = 0; i < count; i++, q += vsize) maxIdx = std::max<u32>(maxIdx, q[0]);
            } else {
                for (u32 i = 0; i < count; i++, q += vsize) maxIdx = std::max<u32>(maxIdx, u32(q[0]) << 8 | q[1]);
            }
        }
        const Array& a = s_arrays[refs[r].array];
        const u32 stride = a.stride ? a.stride : 16;
        sendArray(refs[r].array, (maxIdx + 1) * stride + 16);
    }
}

// ---------------------------------------------------------------------------------------
// textures and palettes
// ---------------------------------------------------------------------------------------
u32 texRegBase(u32 map) { return map < 4 ? 0x80 + map : 0xA0 + (map - 4); }
bool isPalette(u32 fmt) { return fmt == 8 || fmt == 9 || fmt == 10; }

u64 hashBytes(const u8* p, u32 n) {
    u64 h = 1469598103934665603ull;  // FNV-1a (palettes are at most 512 bytes)
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Bind the palette the texture names (TX_SETTLUT: TMEM offset + format) to an Aurora slot.
u32 bindTlut(u32 tlutReg) {
    const u32 tmem = tlutReg & 0x3FF, fmt = (tlutReg >> 10) & 3;
    auto it = s_tlutLoads.find(tmem);
    if (it == s_tlutLoads.end()) return 0;
    const TlutLoad& ld = it->second;
    const u32 bytes = ld.entries * 2;
    const u8* src = physPtr(ld.addr, bytes);
    if (!src) return 0;
    const u64 key = hashBytes(src, bytes) ^ (u64(fmt) << 60) ^ (u64(ld.entries) << 40);
    u32& id = s_tlutIds[key];
    if (!id) id = s_nextTlutId++;
    u32 slot = 0;
    for (; slot < 16; slot++)
        if (s_tlutSlots[slot].tmem == tmem && s_tlutSlots[slot].fmt == fmt) break;
    if (slot == 16) slot = (s_tlutNext++) % 16;
    TlutSlot& s = s_tlutSlots[slot];
    if (s.tmem != tmem || s.fmt != fmt || s.id != id || s.addr != ld.addr || s.entries != ld.entries) {
        s = {tmem, fmt, ld.addr, ld.entries, id};
        aur(GX_AURORA_LOAD_TLUT);
        fifo::write_u8(u8(slot));
        fifo::write_u64(reinterpret_cast<u64>(src));
        fifo::write_u32(fmt);
        fifo::write_u16(u16(ld.entries));
        fifo::write_u32(id);
        fifo::write_u32(0);
    }
    return slot;
}

// Texture identity for Aurora's cache: one object id per (address, size, format, mips), and a
// version that changes when the texture's bytes do (hashed at most once per frame).
struct TexInfo {
    u32 id = 0, version = 0;
    u64 hash = 0, frame = ~0ull;
    u64 hashTb = 0;      // when the contents were last hashed
    u64 tlutHash = 0;    // RL-NX: the palette the version was built from (R2_STRICT)
    // RL-NX: the hash cache (9.34). `probe` is the cheap fingerprint taken with the last full hash and `epoch`
    // the texture-cache generation it was taken in; the full hash runs again only when one of them moves.
    u64 probe = 0;
    u32 epoch = 0;
};
std::unordered_map<u64, TexInfo> s_texInfo;
u32 s_nextTexId = 1;
// bits a texel takes in memory: texture formats (GX_TF_*) and EFB copy formats (copyFormat)
u32 texelBits(u32 fmt) {
    switch (fmt) {
    case 0x00: case 0x08: case 0x0E: case 0x20: case 0x30: return 4;  // I4 C4 CMPR, R4 Z4
    case 0x01: case 0x02: case 0x09: case 0x11: case 0x22: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x39:
    case 0x3A: return 8;  // I8 IA4 C8, Z8 RA4 A8 R8 G8 B8 Z8M Z8L
    case 0x06: case 0x16: case 0x26: return 32;  // RGBA8, Z24X8 YUVA8
    default: return 16;
    }
}

// EFB copy destinations. Their contents live on the GPU (Aurora's copy textures, keyed by the
// destination's host pointer), not in guest memory, and Aurora samples the copy for any texture
// at that address - also after the game put other textures there: the Millennium Falcon's
// cockpit (The Asteroid Field) drew scraps of a rendered frame, or nothing, after the space slug
// cinematic. What the memory held at the copy is hashed; a texture read from there once it
// changed is what the game loaded there (RL-NX: of any size; see prepareTextures): the
// copy is dropped and the address is an ordinary texture again (Dolphin's texture cache drops
// its EFB copies the same way). Memory changes alone are not enough: guest memory does not
// hold what the hardware's copies would have written there (display copies never reach it),
// and leaving SELECT GAME the game draws a 640x452 texture at the second XFB's address, where
// it made a 640x450 copy at boot before filling memory with junk (that junk showed, full
// screen). Display copy destinations therefore always keep their copy.
struct CopyDest {
    u32 bytes = 0;         // RL-NX: the last copy made there, in memory (was the largest)
    u32 w = 0, h = 0, bits = 0;  // the last copy: texels and bits a texel
    u64 hash = 0;          // of those bytes in guest memory, at the last copy
    u64 checked = ~0ull;   // frame of the last comparison
};
std::unordered_map<u32, CopyDest> s_copyDests;
std::unordered_set<u32> s_dispDests;  // display copy (XFB) destinations: their memory says nothing

// RL-NX: R2_STATS - what a frame's draws and texture binds actually did. This is the measurement the bug
// needs: for every asset, was the draw submitted, did it land inside the scissor, and did the texture come
// from guest memory or from a copy - per frame, without a screenshot per asset.
struct FrameStats {
    u64 binds = 0, bindRepeat = 0, bindChanged = 0, bindCopy = 0, bindFlat = 0;
    u64 draws = 0, measured = 0, unmeasurable = 0, clipped = 0, absurd = 0;
    // RL-NX: the unmeasurable ones split by why (see BoundsMiss): an empty strip and a packet the
    // measurer cannot decode are both "unmeasurable" but only the second is a blind spot.
    u64 empty = 0, behind = 0;
    // RL-NX: R2_DRAW_BOUNDS - the largest draw this frame's scissor test rejected, as a rectangle (both
    // sides in the scissor's own space) instead of a count: x0,x1,y0,y1 then scL,scR,scT,scB.
    float rej[8] = {};
    float rejArea = 0.f;
    u8 rejOp = 0;
    u32 rejVerts = 0;
    bool rej2D = false, rejHave = false;
    // RL-NX: R2_STATS - indexed XF loads submitted this frame, and the renderer's command backlog in bytes
    // when the guest finished building the frame (see endFrame). Neither exists before a level: menus and
    // cutscenes use no LOAD_INDX at all and leave the queue empty, while a level frame submits thousands and
    // leaves hundreds of KiB for the renderer to work through after the frame is built.
    u64 indxSnap = 0;
    // RL-NX: the texture hash cache's work this frame (9.34): full 64-bit hashes, and cheap fingerprints.
    // Before the cache these were the same number, and it was every bound texture, every frame.
    u64 hashFull = 0, hashProbe = 0;
    // RL-NX (PERF_PLAN 1b): bytes hashed this frame, split by why the hash was taken - the epoch (GX's own
    // invalidate, BP 0x66), the safety net, or a fingerprint that moved - plus the copy destinations, which
    // are hashed unconditionally for now. Bytes, not milliseconds: the emulator cannot time the guest, and a
    // byte count times a measured XXH3 rate is an honest bound on what a fix is worth.
    u64 hashBytesFirst = 0, hashBytesNet = 0, hashBytesProbe = 0, hashBytesCopy = 0;
};
FrameStats s_statsFrame;
bool s_lastBindChanged = false;  // the last textureIdentity bumped its version
// RL-NX: the texture-cache generation. GX's "invalidate texture cache" (BP 0x66) means the game has changed
// texture memory and the hardware's cache is stale; the hashing cache below uses it as its own signal.
u32 s_texEpoch = 0;
// RL-NX: how many times a full hash found a changed image whose fingerprint had not moved - a probe miss, i.e.
// the one thing that could make the cache show a stale texture. It should be 0; the stats line reports it.
u64 s_probeMiss = 0;

u32 textureBytes(u32 fmt, u32 w, u32 h, u32 levels) {
    u32 bw = 4, bh = 4, bytes = 32;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; break;  // I4 C4 CMPR
    case 1: case 2: case 9: bw = 8; bh = 4; break;   // I8 IA4 C8
    case 6: bytes = 64; break;                        // RGBA8
    default: break;                                   // 4x4 blocks of 32 bytes
    }
    u32 total = 0;
    for (u32 l = 0; l < levels; l++) {
        total += ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

// RL-NX: a fingerprint of eight 64-byte windows spread over an image: about a thirtieth of a full hash, and it
// only ever answers "could the bytes have moved?". Whole cache lines from an even spread, so an image that is
// rewritten in place (a movie frame, a menu fade, a texture the CPU decodes) moves it as soon as it touches one
// of them. The full hash still decides; this only says whether to bother taking it.
u64 textureProbe(const u8* p, u32 bytes) {
    constexpr u32 kWindow = 64, kWindows = 8;
    if (bytes <= kWindow * kWindows) return XXH3_64bits(p, bytes);
    const u32 step = (bytes / kWindows) & ~u32(63);
    u64 h = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < kWindows; i++) h = XXH3_64bits_withSeed(p + size_t(step) * i, kWindow, h);
    return h;
}

void textureIdentity(u32 addr, u32 image0, bool mips, u32 levels, const u8* host, u32& id, u32& version,
                     u64 tlutHash = 0) {
    id = version = 0;
    s_lastBindChanged = false;
    if (s_copyDests.count(addr)) {
        s_lastBindChanged = true;  // render target: let Aurora resolve it every time
        return;
    }
    const u32 fmt = (image0 >> 20) & 0xF, w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
    const u32 bytes = textureBytes(fmt, w, h, levels);
    if (!physPtr(addr, bytes)) return;
    const u64 key = (u64(addr) << 32) ^ (u64(image0) << 1) ^ (mips ? 1 : 0) ^ (u64(levels) << 56);
    TexInfo& t = s_texInfo[key];
    if (!t.id) t.id = s_nextTexId++;
    // RL-NX (9.34): the content hash is cached, not retaken every frame. At level 1's frame rate (60-90 ms a
    // frame, so the old 60 Hz throttle never bit) this hashed every bound texture every frame - 1.9 ms of the
    // guest thread - and it almost never found anything: `changed 0` on every stats line of the 1130 run, with
    // 300-870 binds a frame. So a full hash is taken only when something says the bytes could have moved:
    // no hash yet, the texture-cache epoch moved (GX's own invalidate, BP 0x66), the cheap fingerprint moved,
    // or the safety net below reaches this texture. The net is a round robin - one texture in R2_TEX_HASH_NET
    // (64) gets an unconditional full hash each frame - so a change the eight windows happen to miss is still
    // corrected within a second at level frame rates, without ever all-at-once costing a frame.
    // RL-NX: both read once, on the first bind (a namespace-scope static would read the environment before it is
    // set up). R2_TEX_HASH_EVERY=1 restores the every-frame hashing for an A/B; R2_TEX_HASH_NET sets the safety
    // net's period in frames (64 by default, 0 turns the net off).
    static const bool s_texHashEvery = envFlag("R2_TEX_HASH_EVERY");
    static const u32 s_texHashNet = []() {
        const char* e = getenv("R2_TEX_HASH_NET");
        return u32(e != nullptr && *e != '\0' ? atoi(e) : 64);
    }();
    if (t.frame != s_frames) {
        t.frame = s_frames;
        // RL-NX: R2_STRICT keeps the old every-frame hashing (the throttle is irrelevant when the question is
        // whether 60 Hz sampling leaves a streamed texture stale); R2_TEX_HASH_EVERY=1 restores it for an A/B.
        if (s_strictOn || s_texHashEvery) {
            t.hashTb = tbNow();
            const u64 hsh = XXH3_64bits(host, bytes);
            if (hsh != t.hash) {
                t.hash = hsh;
                t.version++;
                rl_pm.texNew++;  // RL-NX: PERF LOG - a texture re-sent (new or changed contents)
                s_lastBindChanged = true;
            }
            t.probe = textureProbe(host, bytes);
            t.epoch = s_texEpoch;
            if (s_statsOn) s_statsFrame.hashFull++;
        } else {
            const bool net = s_texHashNet && ((s_frames + u64(t.id)) % u64(s_texHashNet)) == 0;
            // RL-NX (PERF_PLAN 1b): the epoch is no longer a reason to take a full hash. GX's own invalidate
            // (BP 0x66) means "the hardware's texture cache is stale, look at memory again" - and the else
            // branch below does look, every frame, for 512 bytes instead of the whole image. Counting it as a
            // full hash made the cache revert to hashing every bound texture on the frames the game
            // invalidates, which is exactly what 9.34 removed. A change the eight windows miss is still
            // caught: by the probe-miss counter (`missed` in the stats line), by the round-robin net, and by
            // the fact that any rewrite of the image that touches a window moves the fingerprint.
            if (!t.hashTb || net) {
                // RL-NX (PERF_PLAN 1b): who asked for this full hash - the first one, or the round-robin net.
                // Reported in bytes (KiB in the stats line) because the emulator cannot time the guest.
                if (s_statsOn) {
                    if (t.hashTb && net) s_statsFrame.hashBytesNet += bytes;
                    else s_statsFrame.hashBytesFirst += bytes;
                }
                t.hashTb = tbNow();
                const u64 pr = textureProbe(host, bytes);
                const u64 hsh = XXH3_64bits(host, bytes);
                if (hsh != t.hash) {
                    // RL-NX: a change the fingerprint did not see. Counted rather than hidden: this is the one
                    // number that says whether eight windows are enough (0 says they are), and the net exists so
                    // such a change is still corrected, within R2_TEX_HASH_NET frames.
                    if (t.probe != 0 && pr == t.probe) {
                        ++s_probeMiss;
                        if (s_probeMiss <= 8)
                            logf("tex", "texture %08X (%ux%u): contents changed without moving the probe (miss %llu)",
                                 addr, w, h, (unsigned long long)s_probeMiss);
                    }
                    t.hash = hsh;
                    t.version++;
                    rl_pm.texNew++;  // RL-NX: PERF LOG - a texture re-sent (new or changed contents)
                    s_lastBindChanged = true;
                }
                t.probe = pr;
                t.epoch = s_texEpoch;
                if (s_statsOn) s_statsFrame.hashFull++;
            } else {
                const u64 pr = textureProbe(host, bytes);
                if (s_statsOn) s_statsFrame.hashProbe++;
                if (pr != t.probe) {  // could have moved: take the real hash and let it decide
                    t.probe = pr;
                    if (s_statsOn) s_statsFrame.hashBytesProbe += bytes;  // RL-NX (PERF_PLAN 1b)
                    const u64 hsh = XXH3_64bits(host, bytes);
                    if (hsh != t.hash) {
                        t.hash = hsh;
                        t.version++;
                        rl_pm.texNew++;  // RL-NX: PERF LOG - a texture re-sent (new or changed contents)
                        s_lastBindChanged = true;
                    }
                    if (s_statsOn) s_statsFrame.hashFull++;
                }
            }
        }
    }
    // RL-NX: a paletted texture's identity includes its palette (the key and hash only ever covered the
    // index data, so a reloaded TLUT - menu fades, per-slot palettes - kept the first colours forever).
    if (tlutHash != 0 && t.tlutHash != tlutHash) {
        t.tlutHash = tlutHash;
        t.version++;
        rl_pm.texNew++;  // RL-NX: PERF LOG - a texture re-sent (new or changed contents)
        s_lastBindChanged = true;
    }
    id = t.id;
    version = t.version;
}

void prepareTextures() {
    for (u32 m = 0; m < 8; m++) {
        TexMap& t = s_tex[m];
        const u32 base = texRegBase(m);
        const u32 image3 = s_bp[base + 0x14];
        const u32 tlutReg = s_bp[base + 0x18];
        const u32 image0 = s_bp[base + 0x08];
        const u32 fmt = (image0 >> 20) & 0xF;
        // re-sent when the registers change, once a frame (contents may change behind
        // unchanged registers: movies, render targets), and for palettes
        if (!t.dirty && t.frame == s_frames && !isPalette(fmt)) continue;
        t.dirty = false;
        t.frame = s_frames;
        const u32 addr = (image3 & 0x1FFFFF) << 5;
        if (!addr) continue;
        const u32 w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
        u8* host = physPtr(addr, 32);
        if (!host) continue;
        if (const auto it = s_copyDests.find(addr); it != s_copyDests.end() && it->second.checked != s_frames) {
            it->second.checked = s_frames;
            const u8* mem = physPtr(addr, it->second.bytes);
            if (s_statsOn) s_statsFrame.hashBytesCopy += it->second.bytes;  // RL-NX (PERF_PLAN 1b)
            const u64 hsh = mem ? XXH3_64bits(mem, it->second.bytes) : it->second.hash;
            if (hsh != it->second.hash) {  // see CopyDest
                CopyDest& c = it->second;
                // RL-NX: memory that changed after an EFB copy went there was written by the CPU (copies never
                // reach guest memory here), so the texture is what the CPU wrote, whatever its size - Dolphin
                // drops its EFB copies on a hash change the same way. Keeping a same-size copy hid every image
                // the game decodes into a buffer that once took a copy: the 640x480 RGB565 loading-screen and
                // menu movies at 00CDF7A0 (hardware 10:16, "still the copy" each frame) showed a stale copy
                // (black) instead. Display copy destinations still keep theirs (see CopyDest).
                // R2_COPY_KEEP_SAME=1 brings back the old rule (a copy of the same size and depth is kept).
                static const bool keepSame = envFlag("R2_COPY_KEEP_SAME");
                // RL-NX: R2_STRICT drops the display-destination exemption: a destination that always keeps
                // its copy is a permanent claim of ownership over memory the game has since reused.
                const bool same = (keepSame && w == c.w && h == c.h && texelBits(fmt) == c.bits) ||
                                  (!s_strictOn && s_dispDests.count(addr));
                static u32 logs = 0;
                if (logs < 50 && ++logs)
                    logf("gfx", "texture at %08X (%ux%u, %u bits): memory changed since an EFB copy (%ux%u, %u bits) went there: %s",
                         addr, w, h, texelBits(fmt), c.w, c.h, c.bits, same ? "still the copy" : "copy dropped");
                if (same) {
                    c.hash = hsh;
                } else {
                    s_copyDests.erase(it);
                    aur(GX_AURORA_DESTROY_COPY_TEX);
                    fifo::write_u64(reinterpret_cast<u64>(host));
                }
            }
        }
        const u32 mode0 = s_bp[base + 0x00], mode1 = s_bp[base + 0x04];
        const u32 minFilter = (mode0 >> 5) & 7;
        const bool mips = (minFilter == 1 || minFilter == 2 || minFilter == 5 || minFilter == 6) && ((mode1 >> 8) & 0xFF) > 0;
        const u32 tlut = isPalette(fmt) ? bindTlut(tlutReg) : 0;
        // RL-NX: R2_STRICT - the palette's bytes are part of the texture's identity (see textureIdentity).
        u64 tlutHash = 0;
        if (s_strictOn && isPalette(fmt)) {
            const auto it = s_tlutLoads.find(tlutReg & 0x3FF);
            if (it != s_tlutLoads.end()) {
                const u8* pal = physPtr(it->second.addr, it->second.entries * 2);
                if (pal) tlutHash = XXH3_64bits(pal, it->second.entries * 2) | 1;  // 0 means "no palette"
            }
        }
        // RL-NX: R2_TEX_FLAT - send one 8x8 tile instead of the image (a valid block size for every GX
        // format): each asset then samples a single flat colour, so a screen that is still empty is not a
        // texture-identity problem, and a screen of flat blocks has correct geometry, state and placement.
        const u32 sendW = s_flatOn ? 8 : w, sendH = s_flatOn ? 8 : h;
        static const bool logTex = envFlag("R2_TEX_LOG");
        if (logTex) {
            // with Dolphin's replacement name (tex1_<w>x<h>[_m]_<XXH64 of level 0>[_<palette>]_<fmt>)
            const u32 bytes0 = textureBytes(fmt, w, h, 1);
            const u8* all = physPtr(addr, bytes0);
            const u64 h64 = all ? XXH64(all, bytes0, 0) : 0;
            logf("tex", "map %u: %08X %ux%u fmt %u tlut %u mips %d mode0 %06X mode1 %06X: tex1_%ux%u%s_%016llx%s_%u", m, addr,
                 w, h, fmt, tlut, int(mips), mode0, mode1, w, h, mips ? "_m" : "", (unsigned long long)h64,
                 isPalette(fmt) ? "_<tlut>" : "", fmt);
        }
        {  // prompts baked into textures (button_icons.cpp; it only looks at a few sizes)
            const u32 bytes0 = textureBytes(fmt, w, h, 1);
            buttonIconsNoteTexture(fmt, w, h, physPtr(addr, bytes0), bytes0);
        }
        if (texdump::directory() && !s_copyDests.count(addr)) {  // R2_TEX_DUMP (tex_dump.cpp)
            const u8* all = physPtr(addr, textureBytes(fmt, w, h, 1));
            const u8* pal = nullptr;
            u32 entries = 0, palFmt = 0;
            if (isPalette(fmt)) {
                const auto it = s_tlutLoads.find(tlutReg & 0x3FF);
                if (it != s_tlutLoads.end()) {
                    entries = it->second.entries;
                    pal = physPtr(it->second.addr, entries * 2);
                    palFmt = (tlutReg >> 10) & 3;
                }
            }
            texdump::dump(fmt, w, h, mips, all, palFmt, pal, entries);
        }
        aur(GX_AURORA_LOAD_TEXOBJ);
        fifo::write_u8(u8(m));
        fifo::write_u64(reinterpret_cast<u64>(host));
        fifo::write_u32(sendW);
        fifo::write_u32(sendH);
        fifo::write_u32(fmt);
        fifo::write_u32(tlut);
        fifo::write_u8(mips && !s_flatOn ? 1 : 0);
        u32 id, version;
        const u32 levels = mips ? ((mode1 >> 8) & 0xFF) / 16 + 1 : 1;
        textureIdentity(addr, image0, mips, levels, host, id, version, tlutHash);
        if (s_statsOn) {
            s_statsFrame.binds++;
            if (s_flatOn) s_statsFrame.bindFlat++;
            if (s_copyDests.count(addr)) s_statsFrame.bindCopy++;
            else if (s_lastBindChanged) s_statsFrame.bindChanged++;
            else s_statsFrame.bindRepeat++;
        }
        fifo::write_u32(id);
        fifo::write_u32(version);
    }
}

// ---------------------------------------------------------------------------------------
// EFB copies
// ---------------------------------------------------------------------------------------
// Inverse of GXSetTexCopyDst's encoding: format bits 3-6, intensity (bits 15-16 = 3), and a
// depth copy when the PE pixel format is Z24.
u32 copyFormat(u32 cp) {
    const u32 f = ((cp >> 4) & 7) | (((cp >> 3) & 1) << 3);
    const bool intensity = ((cp >> 15) & 3) == 3;
    const bool depth = (s_bp[0x43] & 7) == 3;
    if (depth) {
        switch (f) {
        case 0: return 0x30;            // GX_CTF_Z4
        case 1: case 8: return 0x11;    // GX_TF_Z8
        case 6: return 0x16;            // GX_TF_Z24X8
        case 9: return 0x39;            // GX_CTF_Z8M
        case 10: return 0x3A;           // GX_CTF_Z8L
        case 11: case 3: return 0x13;   // GX_TF_Z16
        case 12: return 0x3C;           // GX_CTF_Z16L
        default: return 0x16;
        }
    }
    if (intensity) {
        switch (f) {
        case 0: return 0;     // GX_TF_I4
        case 1: return 1;     // GX_TF_I8
        case 2: return 2;     // GX_TF_IA4
        case 3: return 3;     // GX_TF_IA8
        case 6: return 0x26;  // GX_CTF_YUVA8
        default: return 1;
        }
    }
    switch (f) {
    case 0: return 0x20;           // GX_CTF_R4
    case 1: case 8: return 0x28;   // GX_CTF_R8
    case 2: return 0x22;           // GX_CTF_RA4
    case 3: return 0x23;           // GX_CTF_RA8
    case 4: return 4;              // GX_TF_RGB565
    case 5: return 5;              // GX_TF_RGB5A3
    case 6: return 6;              // GX_TF_RGBA8
    case 7: return 0x27;           // GX_CTF_A8
    case 9: return 0x29;           // GX_CTF_G8
    case 10: return 0x2A;          // GX_CTF_B8
    case 11: return 0x2B;          // GX_CTF_RG8
    case 12: return 0x2C;          // GX_CTF_GB8
    default: return 6;
    }
}

// bytes an EFB copy of w x h texels takes in memory, by its texture format (copyFormat)
u32 copyBytes(u32 fmt, u32 w, u32 h) {
    const u32 bits = texelBits(fmt);
    const u32 bw = bits <= 8 ? 8 : 4, bh = bits == 4 ? 8 : 4, bytes = bits == 32 ? 64 : 32;
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

void prepareCopy(u32 cp) {
    const u32 tl = s_bp[0x49], wh = s_bp[0x4A];
    const u32 left = tl & 0x3FF, top = (tl >> 10) & 0x3FF;
    const u32 w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    if (cp & (1u << 14)) {  // to the XFB
        rl_pm.copyXfb++;  // RL-NX: PERF LOG - display copies and their pixels
        rl_pm.copyXfbPx += u64(w) * h;
        s_dispDests.insert((s_bp[0x4B] & 0xFFFFFF) << 5);  // (see CopyDest)
        aur(GX_AURORA_LOAD_DISP_COPY_SRC);
        fifo::write_u32(left);
        fifo::write_u32(top);
        fifo::write_u32(w);
        fifo::write_u32(h);
        return;
    }
    const bool half = (cp >> 9) & 1;
    const u32 dw = half ? (w + 1) / 2 : w, dh = half ? (h + 1) / 2 : h;
    const u32 dest = (s_bp[0x4B] & 0xFFFFFF) << 5;
    u8* host = physPtr(dest, 32);
    rl_pm.copyTex++;  // RL-NX: PERF LOG - EFB copies to textures and their source pixels
    rl_pm.copyTexPx += u64(w) * h;
    {
        CopyDest& d = s_copyDests[dest];
        const u32 fmt = copyFormat(cp);
        d.w = dw;
        d.h = dh;
        d.bits = texelBits(fmt);
        // RL-NX: the hash covers this copy's bytes only (it was the largest copy ever made there). With any
        // change now dropping the copy, other data the game keeps past a smaller copy's end must not count.
        d.bytes = copyBytes(fmt, dw, dh);
        const u8* mem = physPtr(dest, d.bytes);
        d.hash = mem ? XXH3_64bits(mem, d.bytes) : 0;
        d.checked = s_frames;
    }
    aur(GX_AURORA_LOAD_COPY_SRC);
    fifo::write_u32(left);
    fifo::write_u32(top);
    fifo::write_u32(w);
    fifo::write_u32(h);
    aur(GX_AURORA_LOAD_COPY_DST);
    fifo::write_u32(dw);
    fifo::write_u32(dh);
    fifo::write_u32(copyFormat(cp));
    fifo::write_u8(half ? 1 : 0);
    aur(GX_AURORA_LOAD_COPY_DEST);
    fifo::write_u64(reinterpret_cast<u64>(host));
}

// ---------------------------------------------------------------------------------------
// the stream
// ---------------------------------------------------------------------------------------
// R2_GX_DUMP=<frame>[,<count>]: write every command of those presented frames to
// build/gx_frame_<frame>.txt
FILE* s_dump = nullptr;
u64 s_dumpFrame = ~0ull;
u64 s_dumpCount = 1;  // R2_GX_DUMP=<first>[,<count>]
u64 s_dumpAtTb = 0;   // R2_GX_DUMP=t<seconds>[;t<seconds>...][,<count>]: frames after those times
std::vector<u64> s_dumpTimes;
u64 s_dumpLast = ~0ull;
// indexed XF loads seen in the dump: their memory is printed again when the dump closes, to
// compare what the GP read with what the CPU had written by the end of the frame
struct DumpedLoad {
    u32 addr, words;
};
std::vector<DumpedLoad> s_dumpLoads;

void dumpCommand(const u8* p, u32 len) {
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        const u32 n = be16(p + 1), vs = n ? (len - 3) / n : 0;
        fprintf(s_dump, "DRAW prim %02X fmt %u count %u vsize %u vcd %08X/%08X vat %08X %08X %08X:", op & 0xF8, op & 7, n,
                vs, s_vcdLo, s_vcdHi, s_vatA[op & 7], s_vatB[op & 7], s_vatC[op & 7]);
        for (u32 i = 3; i < len && i < 3 + 4 * vs && i < 67; i++) fprintf(s_dump, " %02X", p[i]);
        fputc('\n', s_dump);
        // Indexed positions: the array and the raw data behind the first indices (offline decoding).
        const u32 posType = (s_vcdLo >> 9) & 3;
        if (posType >= 2 && n && vs) {
            const u32 off = (s_vcdLo & 1) + u32(__builtin_popcount((s_vcdLo >> 1) & 0xFF));
            const Array& a = s_arrays[0];
            fprintf(s_dump, "  POS array %08X stride %u:", a.base, a.stride);
            for (u32 v = 0; v < n && v < 6; v++) {
                const u8* vp = p + 3 + v * vs + off;
                const u32 idx = posType == 2 ? vp[0] : be16(vp);
                fprintf(s_dump, " [%u]", idx);
                const u32 bytes = a.stride && a.stride < 12 ? a.stride : 12;
                if (const u8* d = physPtr(a.base + idx * a.stride, bytes))
                    for (u32 b = 0; b < bytes; b++) fprintf(s_dump, "%02X", d[b]);
            }
            fputc('\n', s_dump);
        }
    } else if (op == 0x61) {
        fprintf(s_dump, "BP %02X = %06X\n", p[1], be32(p + 1) & 0xFFFFFF);
    } else if (op == 0x08) {
        fprintf(s_dump, "CP %02X = %08X\n", p[1], be32(p + 2));
    } else if (op == 0x10) {
        const u32 h = be32(p + 1), cnt = (h >> 16) + 1, addr = h & 0xFFFF;
        fprintf(s_dump, "XF %04X x%u:", addr, cnt);
        for (u32 i = 0; i < cnt && i < 16; i++) {
            const u32 w = be32(p + 5 + 4 * i);
            float f;
            memcpy(&f, &w, 4);
            fprintf(s_dump, " %08X(%g)", w, f);
        }
        fputc('\n', s_dump);
    } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        const u32 idx = be16(p + 1), cnt = (be16(p + 3) >> 12) + 1;
        const Array& a = s_arrays[12 + (op - 0x20) / 8];
        fprintf(s_dump, "LOAD_INDX %c idx %u len %u dst %03X from %08X:", 'A' + (op - 0x20) / 8, idx, cnt, be16(p + 3) & 0xFFF,
                a.base + idx * a.stride);
        if (const u8* d = physPtr(a.base + idx * a.stride, cnt * 4))
            for (u32 i = 0; i < cnt; i++) fprintf(s_dump, " %08X", be32(d + 4 * i));
        fputc('\n', s_dump);
        s_dumpLoads.push_back({a.base + idx * a.stride, cnt});
    } else {
        fprintf(s_dump, "OP %02X len %u\n", op, len);
    }
}

// --- scissor box offset ------------------------------------------------------------------
// GXSetScissorBoxOffset moves rendering in the EFB (EFB = screen - offset). Aurora only moves
// its scissor, so while an offset is active the bridge re-sends the projection with the same
// shift folded into its translation terms (the viewport itself must stay inside the target).
u32 s_xfVp[6] = {};    // XF 0x101A..0x101F: viewport scale x/y/z, origin x/y/z
u32 s_xfProj[7] = {};  // XF 0x1020..0x1026: projection p0..p5, type
bool s_projKnown = false;
u64 s_projTypeWrites = 0;  // RL-NX: how often the game wrote the type word (XF 0x1026) - 0 means the 0 the
                           // measurer reads there is our own initial value, not the game's
s32 s_boxOffX = 0, s_boxOffY = 0;
bool s_boxBiasSent = false;  // the game has written a scissor box offset at least once
// RL-NX: the R2_BOX_BIAS / R2_BOX_VIEWPORT rework that lived here (one fixed bias that Aurora was said
// to subtract, with the rest of the offset moved into the XF viewport's origin instead of the
// projection) is gone. It did not explain the band-shaped picture, and translating the viewport origin
// as well put every draw a whole offset off in the other direction. The offset is applied as it was: the
// whole of it, to the scissor box (sendScissor) and to the projection (sendProjection).

float asFloat(u32 b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
u32 asBits(float f) {
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

// A BP write that is complete on its own (no pending mask from the game's stream).
void bpFull(u32 v) {
    fifo::write_u8(0x61);
    fifo::write_u32(0xFEFFFFFFu);
    fifo::write_u8(0x61);
    fifo::write_u32(v);
}

void sendScissor() {
    const u32 s0 = s_bp[0x20], s1 = s_bp[0x21];
    auto adj = [](u32 r, s32 off) { return u32(std::clamp(s32(r) - off, 0, 2047)); };
    // RL-NX: the whole offset, as on hardware (EFB = screen - offset; the projection carries the same
    // shift, sendProjection).
    const s32 offX = s_boxBiasSent ? s_boxOffX : 0, offY = s_boxBiasSent ? s_boxOffY : 0;
    const u32 tp = adj(s0 & 0x7FF, offY), lf = adj((s0 >> 12) & 0x7FF, offX);
    const u32 bm = adj(s1 & 0x7FF, offY), rt = adj((s1 >> 12) & 0x7FF, offX);
    bpFull(0x20000000u | (lf << 12) | tp);
    bpFull(0x21000000u | (rt << 12) | bm);
}

// shiftX: orthographic draws moved by that much (NDC); mirror: orthographic draws reflected
// about x = axis (NDC): x' = 2 axis - x.
void sendProjection(float shiftX = 0.f, bool mirror = false, float axis = 0.f) {
    if (!s_projKnown) return;
    u32 pr[7];
    memcpy(pr, s_xfProj, sizeof pr);
    if (shiftX != 0.f && pr[6] != 0) pr[1] = asBits(asFloat(pr[1]) + shiftX);  // orthographic: NDC
    if (mirror && pr[6] != 0) {
        pr[0] = asBits(-asFloat(pr[0]));
        pr[1] = asBits(2.f * axis - asFloat(pr[1]));
    }
    // RL-NX: the scissor box offset, folded into the projection (restored after the viewport-translation
    // experiment; this is where the port has always applied it). EFB = screen - offset and Aurora moves
    // only its scissor, so the geometry moves by the same amount, expressed in NDC through the viewport's
    // own scale. A perspective projection takes the shift on p1/p3, whose z divides back out.
    if (s_boxBiasSent) {
        const float sx = asFloat(s_xfVp[0]), sy = asFloat(s_xfVp[1]);
        const float dx = sx != 0.f ? -float(s_boxOffX) / sx : 0.f;
        const float dy = sy != 0.f ? -float(s_boxOffY) / sy : 0.f;
        const bool ortho = pr[6] != 0;
        pr[1] = asBits(asFloat(pr[1]) + (ortho ? dx : -dx));
        pr[3] = asBits(asFloat(pr[3]) + (ortho ? dy : -dy));
    }
    fifo::write_u8(0x10);
    fifo::write_u32((6u << 16) | 0x1020);
    for (u32 w : pr) fifo::write_u32(w);
}

// --- screen-space 2D ---------------------------------------------------------------------
// The targeting computer (cockpit view, Y) draws its orange screen as a 2D quad in 640x480
// screen units at corners the game projected from 3D with the camera's own projection, which
// widescreen already made wide: those are positions on the widened screen, and Aurora's 4:3
// layout for 2D squeezed the quad a second time (the screen came out narrower than its frame by
// the widening, and the targets it shows did not line up). hle_TargetingPass brackets that
// drawing with a BP write to an unused register (never forwarded); orthographic draws in
// screen units (not the identity projection: the centered reticle diamond keeps its shape)
// inside it go to Aurora as screen space (GX_AURORA_SET_SCREEN_SPACE_2D).
constexpr u8 kScreenSpaceMarker = 0x7E;
constexpr u32 kScreenSpaceMagic = 0x5C5200;
// hle_ReticleDraw's bracket (the flight reticle): every orthographic draw, the identity
// projection included - the reticle's own (issue #10)
constexpr u32 kScreenSpaceAllMagic = 0x5C5220;
bool s_screenSpaceWindow = false, s_screenSpaceAllWindow = false, s_screenSpaceSent = false;

void screenSpace2D() {
    const bool ortho = s_projKnown && s_xfProj[6] != 0;
    // RL-NX: R2_NO_SS2D=1 keeps the renderer out of screen-space 2D mode, to tell whether a draw is being
    // mapped through it when it should not be.
    static const bool disabled = envFlag("R2_NO_SS2D");
    const bool want = !disabled && ortho &&
                      ((s_screenSpaceWindow && asFloat(s_xfProj[0]) != 1.f) || s_screenSpaceAllWindow);
    if (want == s_screenSpaceSent) return;
    s_screenSpaceSent = want;
    static int logs = 0;
    if (s_copyLog && logs < 20) {
        logs++;
        logf("gfx", "screen-space 2D %s (projection p0 %g)", want ? "on" : "off", asFloat(s_xfProj[0]));
    }
    aur(GX_AURORA_SET_SCREEN_SPACE_2D);
    fifo::write_u8(want ? 1 : 0);
}

// --- movies filling the screen -------------------------------------------------------------
// Cutscene movies (the Death Star's explosion at the end of mission 1, the capital ship's in the
// Factor 5 intro) are quads in the 3D scene that cover the game's own view with some margin. A
// wider screen (or, for a letterboxed cinematic, a narrower one) showed the scene past their
// edges. hle_CutsceneVideoDraw brackets their drawing with BP writes to the unused register the
// targeting computer uses (another magic value); draws inside go to Aurora with
// GX_AURORA_SET_FILL_SCREEN: edges that stop inside the screen move out to it. The NDC shift a
// scissor box offset put into the projection (two-pass frames: sendProjection) goes along, so
// the frame's edges count, not the pass's.
constexpr u32 kFillMagic = 0x5C5210;
bool s_fillWindow = false, s_fillSent = false;
float s_fillShiftX = 0.f, s_fillShiftY = 0.f;

void fillScreen() {
    // RL-NX: R2_NO_FILL=1 never asks the renderer to stretch a draw out to the screen's edges. If a screen
    // that should not be in this mode is, its geometry is pinned to the screen's bounds - band-shaped
    // output is exactly what that looks like.
    static const bool disabled = envFlag("R2_NO_FILL");
    float dx = 0.f, dy = 0.f;
    // RL-NX: the NDC shift sendProjection folds into the projection for the scissor box offset goes along
    // again (it was dropped with the viewport-translation experiment and never restored with the projection
    // shift). Aurora measures the quad less this shift, i.e. against the whole frame. Sent as 0, the bottom
    // pass of a two-pass frame (offset 238) saw a backdrop that stops inside its band and stretched it to
    // fill the band: the whole picture once in the top half and again in the bottom (the title screen,
    // hardware 08:38/08:56). Both projection types move NDC by +dx/+dy (sendProjection adds dx to an
    // orthographic p1/p3 and -dx to a perspective one, whose z divides out with the sign).
    if (s_fillWindow && s_boxBiasSent) {
        const float sx = asFloat(s_xfVp[0]), sy = asFloat(s_xfVp[1]);
        dx = sx != 0.f ? -float(s_boxOffX) / sx : 0.f;
        dy = sy != 0.f ? -float(s_boxOffY) / sy : 0.f;
        // RL-NX: says on the console whether a fill-screen backdrop is drawn in a shifted pass (the first
        // few times only), so a hardware log tells whether this is what the doubled screen was.
        static int logged = 0;
        if (logged < 4 && (dx != 0.f || dy != 0.f)) {
            logged++;
            logf("gfx", "fill-screen draw in a pass with box offset %d,%d: NDC shift %.3f,%.3f sent", s_boxOffX,
                 s_boxOffY, dx, dy);
        }
    }
    const bool on = s_fillWindow && !disabled;
    if (on == s_fillSent && dx == s_fillShiftX && dy == s_fillShiftY) return;
    s_fillSent = on;
    s_fillShiftX = dx;
    s_fillShiftY = dy;
    aur(GX_AURORA_SET_FILL_SCREEN);
    fifo::write_u8(on ? 1 : 0);
    fifo::write_f32(dx);
    fifo::write_f32(dy);
}

// Debugging: R2_GX_SKIP=<first>-<last>:<filter>[;...] drops the draws matching <filter> while
// the retrace count is in [first, last] (s<first>-<last>: seconds since the start instead; the
// retrace count runs ahead while a cinematic is skipped, and with the frame rate). Filters: v<VCD low hex> (e.g. v1E00),
// p<primitive hex> (e.g. pB8), t<map 0 image address hex> (BP 0x94 value, e.g. t47F9B),
// b<BP 0x41 blend mode hex>, z<BP 0x40 z mode hex>, i<first>-<last> (draw index within the
// frame, decimal), or * (everything). One rule per window: screenshots taken
// meanwhile show what each group of draws is.
struct SkipRule {
    u64 first, last;
    bool seconds;
    char kind;
    u32 value, value2;
};
std::vector<SkipRule> parseSkipRules() {
    std::vector<SkipRule> rules;
    const char* s = getenv("R2_GX_SKIP");
    while (s && *s) {
        char* end;
        SkipRule r{};
        r.seconds = *s == 's';
        if (r.seconds) s++;
        r.first = strtoull(s, &end, 10);
        r.last = *end == '-' ? strtoull(end + 1, &end, 10) : r.first;
        if (*end != ':') break;
        r.kind = end[1];
        if (r.kind == '*') {
            end += 2;
        } else if (r.kind == 'i') {
            r.value = u32(strtoul(end + 2, &end, 10));
            r.value2 = *end == '-' ? u32(strtoul(end + 1, &end, 10)) : r.value;
        } else {
            r.value = u32(strtoul(end + 2, &end, 16));
        }
        rules.push_back(r);
        logf("gfx", "skip rule: vblanks %llu-%llu drop %c%X", (unsigned long long)r.first, (unsigned long long)r.last,
             r.kind, r.value);
        s = *end == ';' ? end + 1 : end;
    }
    return rules;
}
bool skipDraw(u8 op) {
    static const std::vector<SkipRule> rules = parseSkipRules();
    const u32 draw = s_frameDraws++;
    rl_pm.draws++;  // RL-NX: PERF LOG
    static u64 announced = ~0ull;
    for (size_t i = 0; i < rules.size(); i++) {
        const SkipRule& r = rules[i];
        static const u64 startTb = tbNow();
        const u64 now = r.seconds ? (tbNow() - startTb) / kTbClock : s_vblanks;
        if (now < r.first || now > r.last) continue;
        if (announced != i) {
            announced = i;
            logf("gfx", "skip rule %zu active (vblank %llu, frame %llu)", i, (unsigned long long)s_vblanks, (unsigned long long)s_frames);
        }
        if (r.kind == '*' || (r.kind == 'v' && s_vcdLo == r.value) || (r.kind == 'p' && (op & 0xF8) == r.value) ||
            (r.kind == 't' && (s_bp[0x94] & 0xFFFFFF) == r.value) || (r.kind == 'i' && draw >= r.value && draw <= r.value2) ||
            (r.kind == 'b' && (s_bp[0x41] & 0xFFFF) == r.value) || (r.kind == 'z' && (s_bp[0x40] & 0x1F) == r.value))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// widescreen menu backgrounds
// ---------------------------------------------------------------------------------------
// The menu screens (title, SELECT GAME, Options...) lay a mosaic of panel tiles over the 4:3
// screen: dozens of orthographic CMPR quads, each with its own texture, the textures allocated
// one after another and drawn in that order. In widescreen those quads keep their shape inside
// the 4:3 area (bars at the sides). Tiling fills the sides: each tile is drawn again one 4:3
// width to the left and to the right (the projection's x translation +-2 in NDC; Aurora then
// squeezes it like the original), so the right side continues with the background's left
// edge and the left side with its right edge.
// A run of at least kTileRun such draws with ascending texture addresses marks those textures
// as background tiles; draws using them are tiled from then on (the run's first frame is not).
// Off in missions (setMenuTiling): HUD pieces must never be repeated.
// The mosaic was not made to repeat, so where a copy meets the original the panels break off
// in a hard vertical line. Once the run is drawn (seamBlend, before the next draw that is not
// a tile), a band just outside each seam starts as the mirror image of the original's edge,
// which continues it seamlessly, and fades into the copy (a custom Aurora draw sampling a
// snapshot of the EFB).
constexpr u32 kTileRun = 8;
bool s_menuTiling = false;
struct TileTex {
    u64 lastFrame;
};
std::unordered_map<u64, TileTex> s_tileTex;  // texture (address | format/size << 32)
std::vector<u64> s_tileRun;                   // current run of ascending candidate textures
u32 s_tileRunLast = 0;
bool s_tileRunOk = false;    // the current run reached kTileRun: its later tiles join at once
bool s_seamPending = false;  // tiles were copied since the last seam blend

// how many copies each side needs to reach the screen's edge (0: not wider than 4:3)
int tileCopies() {
    const settings::Settings& st = settings::get();
    float a = settings::aspectValue(st.aspect);
    if (a <= 0.f) {
        u32 w = 0, h = 0;
        AuroraGetRenderSize(&w, &h);
        a = h ? float(w) / float(h) : 4.f / 3.f;
    }
    const float k = a / (4.f / 3.f);
    if (k < 1.01f) return 0;
    return int((k - 1.f) / 2.f) + 1;  // copy n covers up to 2n + 1 widths
}

// a candidate: orthographic, textured from map 0 with a CMPR texture of tile size
bool tileCandidate(u64& key) {
    if (!s_projKnown || s_xfProj[6] == 0) return false;
    if ((s_vcdHi & 3) == 0 || (s_bp[0x00] & 0xF) == 0) return false;  // no texture coordinates / texgens
    const u32 img = s_bp[0x88];
    if (((img >> 20) & 0xF) != 0xE) return false;  // CMPR
    const u32 w = (img & 0x3FF) + 1, h = ((img >> 10) & 0x3FF) + 1;
    if (w < 16 || h < 16 || w > 512 || h > 512) return false;
    const u32 addr = (s_bp[0x94] & 0xFFFFFF) << 5;
    key = u64(addr) | (u64(img & 0xFFFFFF) << 32);
    return true;
}

// the draw at p (already sent once): its copies at the sides
void tileDraw(const u8* p, u32 len) {
    if (!s_menuTiling) return;
    u64 key;
    if (!tileCandidate(key)) return;
    const u32 addr = u32(key);
    // run detection: ascending addresses, a few KiB apart (the same texture again continues it)
    if (s_tileRun.empty() || addr != s_tileRunLast) {
        const bool continues = !s_tileRun.empty() && addr > s_tileRunLast && addr - s_tileRunLast <= 0x10000;
        if (!continues) {
            s_tileRun.clear();
            s_tileRunOk = false;
        }
        if (s_tileRunOk) {
            s_tileTex[key] = {s_frames};
        } else {
            s_tileRun.push_back(key);
            if (s_tileRun.size() >= kTileRun) {
                for (u64 k : s_tileRun) s_tileTex[k] = {s_frames};
                s_tileRunOk = true;
            }
        }
    }
    s_tileRunLast = addr;
    auto it = s_tileTex.find(key);
    if (it == s_tileTex.end()) return;
    it->second.lastFrame = s_frames;
    const int n = tileCopies();
    if (!n) return;
    for (int i = 1; i <= n; i++)
        for (const float side : {2.f, -2.f}) {
            sendProjection(side * float(i));
            fifo::write_data(p, len);
            fifo::finish_draw();
        }
    sendProjection();
    s_seamPending = true;
}

// is the draw about to be sent one of the known background tiles?
bool isTileDraw() {
    u64 key;
    return s_menuTiling && tileCandidate(key) && s_tileTex.count(key);
}

// --- seam blend ---------------------------------------------------------------------------
// Runs on Aurora's render worker inside the EFB pass: two bands, one outside each seam, as
// wide as kSeamBand of the target's height. The seams are where the 4:3 picture ends: the
// logical frame (640x480) maps to the target's full height, so they sit 2/3 of the height
// either side of the center. Each pixel mixes the mirror image across the seam (continues the
// original) into what is there (the copy) along a smoothstep. Shaders run per pixel even with
// MSAA; the snapshot is resolved.
constexpr float kSeamBand = 0.08f;
aurora::gfx::DrawTypeId s_seamType = aurora::gfx::InvalidDrawType;

struct SeamPayload {
    WGPUTextureView snapshot;  // a reference of its own, released by the draw
    float band;
};

const char* const kSeamShader = R"(
@group(0) @binding(0) var snap: texture_2d<f32>;
struct Band { band: f32 };
@group(0) @binding(1) var<uniform> u: Band;

struct VOut { @builtin(position) pos: vec4<f32> };

// the mirror axes: the seams moved in by two logical pixels (the picture's outermost texels
// carry the tiles' edge filtering, a bright line the mirror would double)
fn seams(dim: vec2<f32>) -> vec2<f32> {
    let half = dim.y * (2.0 / 3.0) - dim.y / 240.0;
    return vec2<f32>(dim.x * 0.5 - half, dim.x * 0.5 + half);
}

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VOut {
    let dim = vec2<f32>(textureDimensions(snap));
    let s = seams(dim);
    let band = dim.y * u.band;
    let right = vi >= 6u;
    let x0 = select(s.x, s.y, right);
    let x1 = select(s.x - band, s.y + band, right);
    let c = vi % 6u;
    let fu = select(0.0, 1.0, c == 1u || c == 4u || c == 5u);
    let fv = select(0.0, 1.0, c == 2u || c == 3u || c == 5u);
    let px = mix(x0, x1, fu);
    var o: VOut;
    o.pos = vec4<f32>(px / dim.x * 2.0 - 1.0, 1.0 - fv * 2.0, 0.0, 1.0);
    return o;
}

@fragment
fn fs_main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> {
    let dimi = vec2<i32>(textureDimensions(snap));
    let dim = vec2<f32>(dimi);
    let s = seams(dim);
    let seam = select(s.x, s.y, p.x > dim.x * 0.5);
    let t = smoothstep(0.0, 1.0, abs(p.x - seam) / (dim.y * u.band));
    let y = clamp(i32(p.y), 0, dimi.y - 1);
    let mx = clamp(i32(2.0 * seam - p.x), 0, dimi.x - 1);
    let a = textureLoad(snap, vec2<i32>(mx, y), 0);
    let b = textureLoad(snap, vec2<i32>(clamp(i32(p.x), 0, dimi.x - 1), y), 0);
    return mix(a, b, t);
}
)";

void seamDraw(const aurora::gfx::DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* payload,
              size_t size, void*) {
    if (size != sizeof(SeamPayload)) return;
    SeamPayload sp;
    memcpy(&sp, payload, sizeof sp);
    const wgpu::TextureView snapshot = wgpu::TextureView::Acquire(sp.snapshot);
    const auto& layout = ctx.layout;
    if (!layout.colorAttachmentCount || !snapshot) return;
    // one pipeline per render target layout (render worker thread only)
    struct Pipe {
        wgpu::RenderPipeline pipeline;
        wgpu::BindGroupLayout bgl;
        wgpu::Buffer uniform;
        float band = -1.f;
    };
    static std::unordered_map<u64, Pipe> pipes;
    Pipe& pp = pipes[layout.key];
    if (!pp.pipeline) {
        wgpu::ShaderSourceWGSL wgsl{};
        wgsl.code = kSeamShader;
        const wgpu::ShaderModuleDescriptor md{.nextInChain = &wgsl, .label = "menu seams"};
        const wgpu::ShaderModule module = ctx.device.CreateShaderModule(&md);
        const wgpu::BindGroupLayoutEntry entries[2] = {
            {.binding = 0,
             .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
             .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                         .viewDimension = wgpu::TextureViewDimension::e2D}},
            {.binding = 1,
             .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
             .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = 16}},
        };
        const wgpu::BindGroupLayoutDescriptor bd{.label = "menu seams", .entryCount = 2, .entries = entries};
        pp.bgl = ctx.device.CreateBindGroupLayout(&bd);
        const wgpu::PipelineLayoutDescriptor pld{.label = "menu seams", .bindGroupLayoutCount = 1,
                                                 .bindGroupLayouts = &pp.bgl};
        const wgpu::PipelineLayout pl = ctx.device.CreatePipelineLayout(&pld);
        std::array<wgpu::ColorTargetState, aurora::gfx::MaxColorAttachments> targets{};
        for (u32 i = 0; i < layout.colorAttachmentCount; i++)
            targets[i] = {.format = layout.colorAttachments[i].format,
                          .writeMask = i == aurora::gfx::SceneColorAttachmentIndex ? wgpu::ColorWriteMask::All
                                                                                   : wgpu::ColorWriteMask::None};
        const wgpu::FragmentState fs{
            .module = module, .entryPoint = "fs_main", .targetCount = layout.colorAttachmentCount, .targets = targets.data()};
        const wgpu::DepthStencilState ds{.format = layout.depthStencilFormat,
                                         .depthWriteEnabled = false,
                                         .depthCompare = wgpu::CompareFunction::Always};
        const wgpu::RenderPipelineDescriptor rd{
            .label = "menu seams",
            .layout = pl,
            .vertex = {.module = module, .entryPoint = "vs_main"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
            .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &ds : nullptr,
            .multisample = {.count = layout.sampleCount},
            .fragment = &fs,
        };
        pp.pipeline = ctx.device.CreateRenderPipeline(&rd);
        const wgpu::BufferDescriptor ubd{.label = "menu seams",
                                         .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                         .size = 16};
        pp.uniform = ctx.device.CreateBuffer(&ubd);
    }
    if (pp.band != sp.band) {
        const float u[4] = {sp.band, 0.f, 0.f, 0.f};
        ctx.queue.WriteBuffer(pp.uniform, 0, u, sizeof u);
        pp.band = sp.band;
    }
    const wgpu::BindGroupEntry bge[2] = {{.binding = 0, .textureView = snapshot},
                                         {.binding = 1, .buffer = pp.uniform, .size = 16}};
    const wgpu::BindGroupDescriptor bgd{.label = "menu seams", .layout = pp.bgl, .entryCount = 2, .entries = bge};
    const wgpu::BindGroup bg = ctx.device.CreateBindGroup(&bgd);
    const u32 w = layout.colorAttachments[0].width, h = layout.colorAttachments[0].height;
    pass.SetPipeline(pp.pipeline);
    pass.SetBindGroup(0, bg);
    pass.SetViewport(0.f, 0.f, float(w), float(h), 0.f, 1.f);
    pass.SetScissorRect(0, 0, w, h);
    pass.Draw(12);
}

// After a run of tiles (and their copies): blend the seams on what is in the EFB now.
void seamBlend() {
    s_seamPending = false;
    if (aurora::gfx::is_offscreen()) return;
    if (s_seamType == aurora::gfx::InvalidDrawType)
        s_seamType = aurora::gfx::register_draw_type({.label = "menu seams", .draw = seamDraw});
    static const float band = [] {
        const char* e = getenv("R2_SEAM_BAND");  // tuning: fraction of the height (0 = off)
        return e ? float(atof(e)) : kSeamBand;
    }();
    if (band <= 0.f) return;
    aurora::gfx::ResolvedTargets out;
    if (!aurora::gfx::resolve_pass({.color = true}, out) || !out.color) return;
    SeamPayload sp{out.color.Get(), band};
    wgpuTextureViewAddRef(sp.snapshot);
    if (!aurora::gfx::push_custom_draw(s_seamType, &sp, sizeof sp)) wgpuTextureViewRelease(sp.snapshot);
}

// --- SELECT GAME's X-wing -----------------------------------------------------------------
// SELECT GAME shows an X-wing from behind (or below) as three orthographic CMPR sprites: the
// upper and the lower wing (560x160, rotated by their position matrices) and then the fuselage
// (224x256), right of the menu with both wings reaching left. In widescreen the picture goes
// on right of the fuselage, where the other two wings belong: each wing is drawn again,
// mirrored about the fuselage's center line (projection x scale negated, culling swapped for
// the flipped winding), so it moves and turns with the original. The fuselage comes after
// the wings, so the axis is the wing's center plus the offset to the fuselage measured before
// (the sprites move together).
bool s_wingSeen = false;  // this frame
float s_wingCenter = 0.f;
bool s_wingOffsetKnown = false;
float s_wingOffset = 0.f;  // fuselage center - wing center (NDC)

// The draw's horizontal extent in (4:3) NDC: its positions through the current position
// matrix and projection. Plain formats only: no per-vertex matrix index.
bool drawExtentX(const u8* p, u32 len, float& minX, float& maxX) {
    const u32 fmt = p[0] & 7, count = be16(p + 1);
    if (!count || !s_projKnown || (s_vcdLo & 1)) return false;
    const u32 vs = (len - 3) / count;
    const u32 A = s_vatA[fmt];
    const u32 posType = (s_vcdLo >> 9) & 3, compType = (A >> 1) & 7, comps = (A & 1) ? 3 : 2;
    const u32 cs = compSize(compType);
    if (!posType || !cs) return false;
    const float scale = 1.f / float(1u << ((A >> 4) & 0x1F));
    const u32 off = u32(__builtin_popcount((s_vcdLo >> 1) & 0xFF));  // texture matrix indices
    const float* m = xfPosMatrix((s_cpMtxA & 0x3F) * 4);
    if (!m) return false;
    const float p0 = asFloat(s_xfProj[0]), p1 = asFloat(s_xfProj[1]);
    minX = 1e30f;
    maxX = -1e30f;
    for (u32 i = 0; i < count; i++) {
        const u8* d = p + 3 + i * vs + off;
        if (posType >= 2) {
            const u32 idx = posType == 2 ? d[0] : be16(d);
            d = physPtr(s_arrays[0].base + idx * s_arrays[0].stride, comps * cs);
            if (!d) return false;
        }
        float v[3] = {0.f, 0.f, 0.f};
        for (u32 c = 0; c < comps; c++) {
            const u8* q = d + c * cs;
            switch (compType) {
            case 0: v[c] = float(q[0]) * scale; break;
            case 1: v[c] = float(s8(q[0])) * scale; break;
            case 2: v[c] = float(be16(q)) * scale; break;
            case 3: v[c] = float(s16(be16(q))) * scale; break;
            default: v[c] = asFloat(be32(q)); break;
            }
        }
        const float ex = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
        float x;
        if (s_xfProj[6]) {
            x = p0 * ex + p1;
        } else {
            const float ez = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
            if (ez >= 0.f) return false;
            x = (p0 * ex + p1 * ez) / -ez;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
    }
    return true;
}

// --- texture coordinates and the texels they sample (RL-NX: R2_DRAW_LIST) -----------------------
// Which texels a draw asks for: its texture coordinates in texel space (GX units - 1/1024 for 16-bit
// coordinates, 1/256 for 8-bit ones, direct for f32), the texgen that feeds the texture, and the texels
// guest memory holds at the corners of that range. A wrong UV range (coordinates into empty atlas space,
// or coordinates that were never filled) and a wrong texture object (the whole image missing at that
// address) look identical on screen and differ only here.
struct DrawUv {
    bool ok = false;
    bool copyDest = false;  // the address took an EFB copy: memory does not hold what the GPU would
    const char* why = nullptr;
    u32 texgen = 0, src = 0, coordFmt = 0, coordCnt = 0, wrapS = 0, wrapT = 0, frac = 0;
    float ru0 = 0, rv0 = 0, ru1 = 0, rv1 = 0;  // the raw values the vertices carry
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // texels
    u32 t00 = 0, t10 = 0, t01 = 0, t11 = 0;  // RGBA at (u0,v0) (u1,v0) (u0,v1) (u1,v1)
};

// level 0 of a texture, decoded and kept for the frame (only logged draws get here: a few decodes per
// flush, not per draw)
const u8* texelImage(u32 addr, u32 fmt, u32 w, u32 h) {
    struct Entry {
        u32 addr = 0, fmt = 0, w = 0, h = 0;
        u64 frame = ~0ull;
        std::vector<u8> rgba;
    };
    static Entry cache[4];
    static u32 next = 0;
    for (u32 i = 0; i < 4; i++) {
        const Entry& c = cache[i];
        if (c.frame == s_frames && c.addr == addr && c.fmt == fmt && c.w == w && c.h == h)
            return c.rgba.empty() ? nullptr : c.rgba.data();
    }
    const u8* src = physPtr(addr, textureBytes(fmt, w, h, 1));
    if (!src) return nullptr;
    const u8* pal = nullptr;
    u32 entries = 0, palFmt = 0;
    if (isPalette(fmt)) {
        const auto it = s_tlutLoads.find(s_bp[0x98] & 0x3FF);
        if (it != s_tlutLoads.end()) {
            entries = it->second.entries;
            pal = physPtr(it->second.addr, entries * 2);
            palFmt = (s_bp[0x98] >> 10) & 3;
        }
    }
    Entry& c = cache[next++ & 3];
    c = Entry{};
    c.addr = addr, c.fmt = fmt, c.w = w, c.h = h, c.frame = s_frames;
    if (!texdump::decodeRgba(fmt, w, h, src, palFmt, pal, entries, c.rgba)) c.rgba.clear();
    return c.rgba.empty() ? nullptr : c.rgba.data();
}

// the texel at a coordinate (wrap modes are printed next to it: clamping here keeps the number in range)
u32 texelAt(const u8* img, u32 w, u32 h, float u, float v) {
    if (!img || !w || !h) return 0;
    const s32 x = std::clamp<s32>(s32(std::floor(u)), 0, s32(w) - 1);
    const s32 y = std::clamp<s32>(s32(std::floor(v)), 0, s32(h) - 1);
    const u8* q = img + (size_t(y) * w + u32(x)) * 4;
    return (u32(q[0]) << 24) | (u32(q[1]) << 16) | (u32(q[2]) << 8) | q[3];
}

void drawUv(const u8* p, u32 len, DrawUv& o) {
    o = DrawUv{};
    o.wrapS = s_bp[0x80] & 3;  // map 0's wrap modes: 0 clamp, 1 repeat, 2 mirror
    o.wrapT = (s_bp[0x80] >> 2) & 3;
    if (len < 3) {
        o.why = "packet too short";
        return;
    }
    // texgen 0, as Aurora's xf_texgen reads it (regs.cpp): projection at bit 1, type at 4-6 (0 matrix,
    // 1 bump, 2/3 SRTG colour) and the input row at 7-11 (5..12 = TEX0..TEX7).
    const u32 tg = s_xfMem[0x1040];
    o.texgen = (tg >> 4) & 7;
    o.src = (tg >> 7) & 0x1F;
    const u32 count = be16(p + 1);
    if (!count) {
        o.why = "no vertices";
        return;
    }
    if (o.texgen != 0) {  // bump / colour: the coordinates are generated, not in the vertex stream
        o.why = "texgen 0 is not a matrix texgen: coordinates generated";
        return;
    }
    if (o.src < 5 || o.src > 12) {  // 0..4 are POS, NRM, COLOR0, BINRM, TANGENT
        o.why = "texgen 0 reads a position/normal/colour row, not a vertex texcoord";
        return;
    }
    const u32 t = o.src - 5;
    const u32 type = (s_vcdHi >> (2 * t)) & 3;  // TEXn is bits 2n-2n+1 of the VCD high word (CP 0x60)
    if (type == 0) {
        o.why = "the vertex format sends no coordinates for that texgen source";
        return;
    }
    if (type >= 2) {
        o.why = "the coordinates are indexed, not read here";
        return;
    }
    const u32 fmt = p[0] & 7, A = s_vatA[fmt], B0 = s_vatB[fmt], C0 = s_vatC[fmt];
    const u32 tc[8] = {(A >> 21) & 1, B0 & 1, (B0 >> 9) & 1, (B0 >> 18) & 1, (B0 >> 27) & 1, (C0 >> 5) & 1,
                       (C0 >> 14) & 1, (C0 >> 23) & 1};
    const u32 tf[8] = {(A >> 22) & 7, (B0 >> 1) & 7, (B0 >> 10) & 7, (B0 >> 19) & 7, (B0 >> 28) & 7, (C0 >> 6) & 7,
                       (C0 >> 15) & 7, (C0 >> 24) & 7};
    o.coordFmt = tf[t];
    o.coordCnt = tc[t] ? 2 : 1;
    const u32 cs = compSize(tf[t]);
    const u32 vs = (len - 3) / count;
    if (!cs || vs < 1) {
        o.why = "no coordinate size";
        return;
    }
    // the offset of TEX<t> in the vertex: matrix indices, position, normal, both colours, then the
    // texture coordinates before it (the same walk prepareArrays does)
    auto attrBytes = [](u32 ty, u32 direct) -> u32 { return ty == 1 ? direct : ty >= 2 ? (ty == 2 ? 1u : 2u) : 0u; };
    const bool nbt = (A >> 9) & 1;
    u32 off = u32(__builtin_popcount(s_vcdLo & 0x1FF));
    off += attrBytes((s_vcdLo >> 9) & 3, compSize((A >> 1) & 7) * ((A & 1) ? 3 : 2));
    off += attrBytes((s_vcdLo >> 11) & 3, compSize((A >> 10) & 7) * (nbt ? 9 : 3));
    off += attrBytes((s_vcdLo >> 13) & 3, colSize((A >> 14) & 7));
    off += attrBytes((s_vcdLo >> 15) & 3, colSize((A >> 18) & 7));
    for (u32 i = 0; i < t; i++) off += attrBytes((s_vcdHi >> (2 * i)) & 3, compSize(tf[i]) * (tc[i] ? 2 : 1));
    if (off + cs * o.coordCnt > vs) {
        o.why = "the coordinates fall outside the vertex";
        return;
    }
    // Aurora's own conversion (shader.cpp fetch_fixed16_attr): the VAT's frac field is the number of
    // fixed-point bits, so the raw value is divided by 2^frac. Printed next to the raw values, so the
    // number can be read back either way.
    o.frac = t == 0 ? (A >> 25) & 0x1F : 0;
    const float scale = std::ldexp(1.f, -int(o.frac));
    o.u0 = o.v0 = 1e30f;
    o.u1 = o.v1 = -1e30f;
    o.ru0 = o.rv0 = 1e30f;
    o.ru1 = o.rv1 = -1e30f;
    float cu[4], cv[4];
    for (u32 i = 0; i < count && i < 4; i++) {
        const u8* d = p + 3 + i * vs + off;
        float u = 0.f, v = 0.f;
        for (u32 c = 0; c < o.coordCnt; c++) {
            const u8* q = d + c * cs;
            float val = 0.f;
            switch (o.coordFmt) {
            case 0: val = float(q[0]); break;
            case 1: val = float(s8(q[0])); break;
            case 2: val = float(be16(q)); break;
            case 3: val = float(s16(be16(q))); break;
            default: val = asFloat(be32(q)); break;
            }
            if (c == 0) {
                o.ru0 = std::min(o.ru0, val);
                o.ru1 = std::max(o.ru1, val);
            } else {
                o.rv0 = std::min(o.rv0, val);
                o.rv1 = std::max(o.rv1, val);
            }
            val *= scale;
            if (c == 0) u = val;
            else v = val;
        }
        cu[i] = u;
        cv[i] = v;
        o.u0 = std::min(o.u0, u);
        o.u1 = std::max(o.u1, u);
        o.v0 = std::min(o.v0, v);
        o.v1 = std::max(o.v1, v);
    }
    o.ok = true;
    // the texels those coordinates point at, in the texture the draw binds
    const u32 addr = (s_bp[0x94] & 0xFFFFFF) << 5;
    const u32 w = (s_bp[0x88] & 0x3FF) + 1, h = ((s_bp[0x88] >> 10) & 0x3FF) + 1;
    o.copyDest = s_copyDests.count(addr) != 0;
    const u8* img = texelImage(addr, (s_bp[0x88] >> 20) & 0xF, w, h);
    o.t00 = texelAt(img, w, h, o.u0, o.v0);
    o.t10 = texelAt(img, w, h, o.u1, o.v0);
    o.t01 = texelAt(img, w, h, o.u0, o.v1);
    o.t11 = texelAt(img, w, h, o.u1, o.v1);
}

// --- draw bounds (R2_DRAW_BOUNDS) -------------------------------------------------------------
// Where a draw's geometry lands in the EFB, transformed on the CPU with the matrices the draw is
// sent with: the current position matrix, the projection as sendProjection writes it (scissor box
// offset folded in) and the XF viewport as Aurora reads it (its viewport is built as
// origin - 340 - half, i.e. scale * ndc + origin - 340). Two placements come out of it:
//   renderer  what Aurora will actually draw:  scale * (ndc + fold) + origin - 340
//   hardware  where the game's own matrix and box offset put it:  scale * ndc + origin - offset
// A picture that is a narrow band of the target is either geometry that covers only that band or a
// clip placed there. Printing both placements next to the scissor that was sent splits the two apart
// with no GPU work: a hardware rectangle that covers the frame while the renderer one does not points
// at the bridge's own transform, and both covering it points at the renderer or the clip.
struct DrawBounds {
    // RL-NX: an indexed position read, kept so it can be printed (logDrawIndex): the array the indices are
    // read through, the indices themselves and the positions they resolve to. "Measured nothing" and "the
    // array was never filled" look the same as a number otherwise.
    struct PosIndex {
        bool any = false;        // positions come from an array (VCD position type 2 or 3)
        bool unreadable = false; // at least one index resolved to unmapped memory
        u8 type = 0;             // VCD position type: 2 = u8 index, 3 = u16 index
        u32 base = 0, stride = 0;
        u32 n = 0;               // indices recorded (the first few)
        u16 idx[4] = {};
        float v[4][3] = {};      // decoded position for each of them (raw units, as the VAT scales them)
    };
    float x0, y0, x1, y1;      // renderer placement (EFB pixels)
    float hx0, hy0, hx1, hy1;  // hardware placement (EFB pixels)
    PosIndex pos;              // RL-NX: R2_DRAW_LIST, what an indexed position read was made of
};
bool s_boundsOn = false;  // R2_DRAW_BOUNDS (set in init)
bool s_rawOn = false;     // RL-NX: R2_DRAW_RAW, what an absurd placement was made of (set in init)
// RL-NX: R2_DRAW_LIST, the per-draw texture/placement log (set in init with the others).
bool s_drawListOn = false;
u32 s_boundsDraws = 0;    // draws measured this frame
u32 s_boundsFrames = 0;  // frames measured so far (the first few are logged)
float s_bUx0 = 1e30f, s_bUy0 = 1e30f, s_bUx1 = -1e30f, s_bUy1 = -1e30f;  // frame union, renderer
float s_bHx0 = 1e30f, s_bHy0 = 1e30f, s_bHx1 = -1e30f, s_bHy1 = -1e30f;  // frame union, hardware
u32 s_bPerspDraws = 0;                                                    // 3D draws measured this frame
float s_bPx0 = 1e30f, s_bPy0 = 1e30f, s_bPx1 = -1e30f, s_bPy1 = -1e30f;  // their union, renderer

// RL-NX: R2_STATS - why a draw could not be placed. Kept apart because most misses are legitimate: an
// empty strip has no geometry to place and a 3D draw behind the eye lands outside the frame. Only the last
// three are the measurer's own blind spot, and counting all of them together is what made "half the
// frame's draws" look like a placement fault.
enum BoundsMiss : u8 {
    kMissNone = 0,
    kMissEmpty,   // no vertices: nothing to place
    kMissBehind,  // every vertex behind the eye (perspective)
    kMissNoProj,  // no projection sent yet
    kMissShort,   // the packet does not hold the vertices it claims
    kMissLayout,  // a vertex layout the measurer does not decode
    kMissIndex,   // a position index pointing at unreadable memory
    kMissMtx,     // a position matrix word outside the XF shadow (see xfPosMatrix)
};

// One line per *distinct* unmeasurable packet, for the first 40 (a decode that never fits would
// otherwise look exactly like "nothing was drawn"; 40 repeats of the same line say nothing new).
void drawBoundsMiss(u8 op, u32 len, const char* why, u8 reason) {
    static u32 keys[40] = {};
    static u32 nKeys = 0;
    const u32 fmt = op & 7;
    const u32 key = (u32(reason) << 28) ^ (fmt << 24) ^ (((s_vcdLo >> 9) & 3) << 20) ^ (((s_vatA[fmt] >> 1) & 7) << 16) ^
                    ((s_vatA[fmt] & 1) << 15) ^ ((s_vcdLo & 0x1FF) << 4) ^ (s_vcdHi & 0x3F);
    for (u32 i = 0; i < nKeys; i++)
        if (keys[i] == key) return;
    if (nKeys >= 40) return;
    keys[nKeys++] = key;
    logf("gfx", "bounds: no extent for v%02X (len %u, reason %u): %s; vcd %08X/%08X vatA %08X pos type %u comp %u/%u", op, len,
         reason, why, s_vcdLo, s_vcdHi, s_vatA[fmt], (s_vcdLo >> 9) & 3, ((s_vatA[fmt] >> 1) & 7), (s_vatA[fmt] & 1) ? 3u : 2u);
}

bool drawBoundsEfb(const u8* p, u32 len, DrawBounds& b, u8* miss = nullptr) {
    const u8 op = p[0];
    if (!s_projKnown) {
        if (miss) *miss = kMissNoProj;
        drawBoundsMiss(op, len, "no projection yet", kMissNoProj);
        return false;
    }
    if (len < 3) {
        if (miss) *miss = kMissShort;
        drawBoundsMiss(op, len, "packet too short", kMissShort);
        return false;
    }
    const u32 fmt = p[0] & 7, count = be16(p + 1);
    if (!count) {
        if (miss) *miss = kMissEmpty;
        drawBoundsMiss(op, len, "no vertices", kMissEmpty);
        return false;
    }
    const u32 vs = (len - 3) / count;
    const u32 A = s_vatA[fmt];
    const u32 posType = (s_vcdLo >> 9) & 3, compType = (A >> 1) & 7, comps = (A & 1) ? 3 : 2;
    const u32 cs = compSize(compType);
    if (!posType || !cs || vs < 1) {
        const u8 why = !posType || !cs ? kMissLayout : kMissShort;
        if (miss) *miss = why;
        drawBoundsMiss(op, len, !posType ? "positions absent" : !cs ? "bad component type" : "empty vertex stride", why);
        return false;
    }
    const float scale = 1.f / float(1u << ((A >> 4) & 0x1F));
    // Vertex layout before the position: one byte each for the position matrix index (bit 0) and for
    // every texture matrix index (bits 1-8), the same walk prepareArrays does.
    const u32 off = u32(__builtin_popcount(s_vcdLo & 0x1FF));
    const bool perVertexMtx = (s_vcdLo & 1) != 0;  // GX_VA_PNMTXIDX in the stream
    // The draw's own position matrix (CP 0x30, as an XF word address: (matrix * 3) * 4), or the one the
    // vertex names.
    const float* mDraw = xfPosMatrix((s_cpMtxA & 0x3F) * 4);
    if (!mDraw && !perVertexMtx) {
        if (miss) *miss = kMissMtx;
        drawBoundsMiss(op, len, "position matrix outside the XF shadow", kMissMtx);
        return false;
    }
    const float p0 = asFloat(s_xfProj[0]), p1 = asFloat(s_xfProj[1]), p2 = asFloat(s_xfProj[2]),
                p3 = asFloat(s_xfProj[3]);
    const bool ortho = s_xfProj[6] != 0;
    const float sx = asFloat(s_xfVp[0]), sy = asFloat(s_xfVp[1]);
    const float ox = asFloat(s_xfVp[3]), oy = asFloat(s_xfVp[4]);
    // the NDC shift sendProjection folds into the projection while an offset is active
    const float foldX = (s_boxBiasSent && sx != 0.f) ? -float(s_boxOffX) / sx : 0.f;
    const float foldY = (s_boxBiasSent && sy != 0.f) ? -float(s_boxOffY) / sy : 0.f;
    const float offXf = float(s_boxBiasSent ? s_boxOffX : 0), offYf = float(s_boxBiasSent ? s_boxOffY : 0);
    b.x0 = b.y0 = b.hx0 = b.hy0 = 1e30f;
    b.x1 = b.y1 = b.hx1 = b.hy1 = -1e30f;
    b.pos = DrawBounds::PosIndex{};  // RL-NX: the indexed position read starts fresh for this draw
    b.pos.type = u8(posType);        // 0/1 direct, 2 u8 index, 3 u16 index (printed either way)
    if (posType >= 2) {
        b.pos.any = true;
        b.pos.base = s_arrays[0].base;
        b.pos.stride = s_arrays[0].stride;
    }
    u32 used = 0;
    bool behind = false, badIndex = false;  // RL-NX: what an unmeasurable draw was made of
    // RL-NX: R2_DRAW_RAW - what an absurd placement was made of: the packet's vertex layout, the first
    // vertex's raw bytes and decoded position, the position matrix and the projection. Garbage in v[]
    // means the vertex data (or the way it is read) is the fault; sane v[] with a garbage matrix or
    // projection means the XF state is. R2_DRAW_RAW_FROM=<frame> skips the intro's own absurd draws.
    static int rawLogs = 0;
    static const u64 rawFrom = getenv("R2_DRAW_RAW_FROM") ? strtoull(getenv("R2_DRAW_RAW_FROM"), nullptr, 10) : 0;
    for (u32 i = 0; i < count; i++) {
        const u8* d = p + 3 + i * vs + off;
        // the position matrix index holds matrix * 3, so the matrix's XF words start at (b / 3) * 12
        const float* m = mDraw;
        if (perVertexMtx) {
            m = xfPosMatrix((u32((p + 3 + i * vs)[0]) / 3) * 12);
            if (!m) {
                badIndex = true;
                continue;
            }
        }
        int recSlot = -1;  // RL-NX: which recorded slot this vertex's index/position goes to
        if (posType >= 2) {
            const u32 idx = posType == 2 ? d[0] : be16(d);
            if (b.pos.n < 4) {
                recSlot = int(b.pos.n++);
                b.pos.idx[recSlot] = u16(idx);
            }
            d = physPtr(s_arrays[0].base + idx * s_arrays[0].stride, comps * cs);
            if (!d) {
                badIndex = true;
                b.pos.unreadable = true;  // RL-NX: the index (not the layout) is what could not be read
                continue;
            }
        }
        float v[3] = {0.f, 0.f, 0.f};
        for (u32 c = 0; c < comps; c++) {
            const u8* q = d + c * cs;
            switch (compType) {
            case 0: v[c] = float(q[0]) * scale; break;
            case 1: v[c] = float(s8(q[0])) * scale; break;
            case 2: v[c] = float(be16(q)) * scale; break;
            case 3: v[c] = float(s16(be16(q))) * scale; break;
            default: v[c] = asFloat(be32(q)); break;
            }
        }
        // RL-NX: R2_DRAW_LIST - the position this vertex's index resolved to (logDrawIndex)
        if (recSlot >= 0)
            for (u32 c = 0; c < 3; c++) b.pos.v[recSlot][c] = v[c];
        const float ex = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
        const float ey = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
        const float ez = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
        float nx, ny;
        if (ortho) {
            nx = p0 * ex + p1;
            ny = p2 * ey + p3;
        } else {
            if (ez >= 0.f) {  // behind the eye: no window position
                behind = true;
                // RL-NX: R2_DRAW_RAW - a perspective draw the measurer drops entirely: the vertex, its
                // view-space z and the matrix, so "behind the camera" is a number, not an assumption.
                if (s_rawOn && s_frames >= rawFrom && i < 2 && rawLogs < 16) {
                    rawLogs++;
                    logf("gfx",
                         "raw v%02X behind the eye: vertex %g %g %g -> view z %g; matrix z row %g %g %g %g; projection "
                         "%g %g %g %g; type word %08X (XF 0x1026 written %llu times); array %08X stride %u",
                         op, v[0], v[1], v[2], ez, m[8], m[9], m[10], m[11], p0, p1, p2, p3, s_xfProj[6],
                         (unsigned long long)s_projTypeWrites, s_arrays[0].base, s_arrays[0].stride);
                }
                continue;
            }
            const float w = -ez;
            nx = (p0 * ex + p1 * ez) / w;
            ny = (p2 * ey + p3 * ez) / w;
        }
        const float ax = sx * (nx + foldX) + ox - 340.f, ay = sy * (ny + foldY) + oy - 340.f;
        const float hx = sx * nx + ox - offXf, hy = sy * ny + oy - offYf;
        if (s_rawOn && s_frames >= rawFrom && i == 0 && (std::fabs(ax) > 4096.f || std::fabs(ay) > 4096.f) &&
            rawLogs < 16) {
            rawLogs++;
            const u8* q0 = off + 8 <= vs ? p + 3 + off : p + 3;
            // RL-NX: the format had four conversions where three arguments were passed ("pos type %u comp
            // %u/%u, %u comps"), so every value after it was printed one place off - which is where the
            // "array base 0x0C / stride 0x20000000" reading came from. The matrix word and the per-vertex
            // matrix byte are printed now: a word outside 256 says the read was past the shadow, a word in
            // range says the shadow's contents are the problem, and the vertex byte says which path set it.
            logf("gfx",
                 "raw v%02X: %u verts stride %u, vcd %08X/%08X vatA %08X (pos type %u comp %u, %u comps), bytes "
                 "%02X %02X %02X %02X %02X %02X %02X %02X -> v %g %g %g; matrix word %u (cp 0x30 %08X, per-vertex "
                 "%d) %g %g %g %g / %g %g %g %g; projection %g %g %g %g (type %08X); array %08X stride %u; EFB %g,%g",
                 op, count, vs, s_vcdLo, s_vcdHi, A, posType, compType, comps, q0[0], q0[1], q0[2], q0[3], q0[4], q0[5],
                 q0[6], q0[7], v[0], v[1], v[2], (s_cpMtxA & 0x3F) * 4, s_cpMtxA,
                 perVertexMtx ? int((p + 3)[0]) : -1, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], p0, p1, p2, p3,
                 s_xfProj[6], s_arrays[0].base, s_arrays[0].stride, ax, ay);
        }
        b.x0 = std::min(b.x0, ax);
        b.x1 = std::max(b.x1, ax);
        b.y0 = std::min(b.y0, ay);
        b.y1 = std::max(b.y1, ay);
        b.hx0 = std::min(b.hx0, hx);
        b.hx1 = std::max(b.hx1, hx);
        b.hy0 = std::min(b.hy0, hy);
        b.hy1 = std::max(b.hy1, hy);
        used++;
    }
    if (!used) {
        // a 3D draw entirely behind the eye has no placement and is not a blind spot; a failed index
        // read is (the measurer is looking at memory the renderer is not).
        const u8 why = badIndex ? kMissIndex : kMissBehind;
        if (miss) *miss = why;
        drawBoundsMiss(op, len, badIndex ? "an unreadable position index" : "every vertex behind the eye", why);
    }
    return used != 0;
}

// RL-NX: R2_STATS - one draw's outcome: submitted and measurable, unmeasurable, entirely outside the
// scissor (a delivered draw that no pixel of the frame can show), or with an absurd extent (a broken
// transform). Approximate on purpose - the scissor is compared raw, without the box offset.
void drawStatsNote(const u8* p, u32 len, u8 op) {
    s_statsFrame.draws++;
    // RL-NX (9.35): the measurement below is the single most expensive thing the bridge did, and it exists to
    // print counts. The 9.34 profile puts drawBoundsEfb at 6.7 ms of the guest thread's 75 ms frame - 9% of a
    // level frame, on the one thread nothing else can help - decoding each draw list and transforming its
    // vertices so the line can say how many draws fell outside the scissor. The cheap counters (draws, binds,
    // indexed XF loads) are unaffected; R2_STATS_BOUNDS=1 brings the measured ones back for a diagnostic run.
    static const bool boundsOn = envFlag("R2_STATS_BOUNDS");
    if (!boundsOn) return;
    DrawBounds b{};
    u8 miss = kMissNone;
    if (!drawBoundsEfb(p, len, b, &miss)) {
        if (miss == kMissEmpty) s_statsFrame.empty++;          // RL-NX: nothing to place
        else if (miss == kMissBehind) s_statsFrame.behind++;   // RL-NX: outside the frame by construction
        else s_statsFrame.unmeasurable++;                      // RL-NX: the measurer's own blind spot
        return;
    }
    s_statsFrame.measured++;
    // the raw BP scissor against the hardware placement (b.hx*/b.hy*), never the renderer placement,
    // which has the 340 bias and the offset folding already taken out of it.
    // RL-NX: both sides are in the EFB+340 space already (b.hx/b.hy carry the viewport's own 340 bias and
    // have the scissor box offset taken out; the raw scissor register is biased and needs no such correction),
    // so the comparison stays as it was - a +340 on the box (the first attempt at this) rejected draws that
    // are plainly on screen.
    const float scL = float((s_bp[0x20] >> 12) & 0x7FF), scT = float(s_bp[0x20] & 0x7FF);
    const float scR = float((s_bp[0x21] >> 12) & 0x7FF), scB = float(s_bp[0x21] & 0x7FF);
    if (b.hx1 <= scL || b.hx0 >= scR || b.hy1 <= scT || b.hy0 >= scB) {
        s_statsFrame.clipped++;
        // RL-NX: R2_DRAW_BOUNDS - keep the largest rejection of this stats frame (printed at its end), so the
        // title page's rejected draws are a rectangle with a size rather than a count.
        const float area = (b.hx1 - b.hx0) * (b.hy1 - b.hy0);
        if (area > s_statsFrame.rejArea) {
            s_statsFrame.rejArea = area;
            s_statsFrame.rej[0] = b.hx0;
            s_statsFrame.rej[1] = b.hx1;
            s_statsFrame.rej[2] = b.hy0;
            s_statsFrame.rej[3] = b.hy1;
            s_statsFrame.rej[4] = scL;
            s_statsFrame.rej[5] = scR;
            s_statsFrame.rej[6] = scT;
            s_statsFrame.rej[7] = scB;
            s_statsFrame.rejOp = op;
            s_statsFrame.rejVerts = be16(p + 1);
            s_statsFrame.rej2D = s_xfProj[6] != 0;
            s_statsFrame.rejHave = true;
        }
    }
    if (std::fabs(b.hx0) > 4096.f || std::fabs(b.hx1) > 4096.f || std::fabs(b.hy0) > 4096.f ||
        std::fabs(b.hy1) > 4096.f)
        s_statsFrame.absurd++;
}

// RL-NX: R2_STATS - the per-frame summary (every 60th frame), then the counters start again. Aurora's own
// frame line carries the other half ("aurora: N draws (M merged)"), so a frame that loses draws to
// merging shows up there and a frame that never submits them shows up here.
void statsFrameEnd() {
    if (s_statsOn && s_frames % 60 == 0) {
        // RL-NX (9.35): the draws group says whether its breakdown was measured at all, because measuring it is
        // the 6.7 ms the bounds pass costs and it is now opt-in (R2_STATS_BOUNDS=1). A line of zeros would look
        // like a frame with nothing in it.
        char drawsPart[224];
        static const bool statsBoundsOn = envFlag("R2_STATS_BOUNDS");
        if (statsBoundsOn)
            std::snprintf(drawsPart, sizeof drawsPart,
                          "%llu draws (%llu measured, %llu unmeasurable, %llu empty strips, %llu behind the eye, "
                          "%llu wholly outside the scissor, %llu absurd)",
                          (unsigned long long)s_statsFrame.draws, (unsigned long long)s_statsFrame.measured,
                          (unsigned long long)s_statsFrame.unmeasurable, (unsigned long long)s_statsFrame.empty,
                          (unsigned long long)s_statsFrame.behind, (unsigned long long)s_statsFrame.clipped,
                          (unsigned long long)s_statsFrame.absurd);
        else
            std::snprintf(drawsPart, sizeof drawsPart,
                          "%llu draws (bounds not measured: R2_STATS_BOUNDS=1 adds the breakdown)",
                          (unsigned long long)s_statsFrame.draws);
        logf("stats", "frame %llu: %s; texture binds %llu (changed %llu, repeated %llu, copy "
             "%llu, flat %llu, hashed %llu of %llu, missed %llu); %llu indexed XF loads, %u KiB still queued at frame end; hashed KiB first %llu net "
             "%llu probe %llu copy %llu",  // RL-NX: PERF_PLAN 1b, where the frame's hashing went
             (unsigned long long)s_frames, drawsPart, (unsigned long long)s_statsFrame.binds, (unsigned long long)s_statsFrame.bindChanged,
             (unsigned long long)s_statsFrame.bindRepeat,
             (unsigned long long)s_statsFrame.bindCopy, (unsigned long long)s_statsFrame.bindFlat,
             (unsigned long long)s_statsFrame.hashFull,
             (unsigned long long)(s_statsFrame.hashFull + s_statsFrame.hashProbe), (unsigned long long)s_probeMiss,
             (unsigned long long)s_statsFrame.indxSnap, s_statsBacklog >> 10,
             (unsigned long long)(s_statsFrame.hashBytesFirst >> 10), (unsigned long long)(s_statsFrame.hashBytesNet >> 10),
             (unsigned long long)(s_statsFrame.hashBytesProbe >> 10), (unsigned long long)(s_statsFrame.hashBytesCopy >> 10));
    }
    s_statsBacklog = 0;
    // RL-NX: R2_DRAW_BOUNDS - the frame's largest scissor rejection: the draw's box (EFB+340, offset taken
    // out), the scissor it was tested against, and the box the renderer was handed, so the three can be read
    // off against each other. Comparison only; nothing here changes what is drawn.
    if (s_boundsOn && s_statsFrame.rejHave && s_statsOn && s_frames % 60 == 0)
        logf("gfx",
             "scissor reject frame %llu: largest of %llu, v%02X %u verts (%s), box x %.0f..%.0f y %.0f..%.0f vs "
             "scissor x %.0f..%.0f y %.0f..%.0f",
             (unsigned long long)s_frames, (unsigned long long)s_statsFrame.clipped, s_statsFrame.rejOp,
             s_statsFrame.rejVerts, s_statsFrame.rej2D ? "2D" : "3D", s_statsFrame.rej[0], s_statsFrame.rej[1],
             s_statsFrame.rej[2], s_statsFrame.rej[3], s_statsFrame.rej[4], s_statsFrame.rej[5], s_statsFrame.rej[6],
             s_statsFrame.rej[7]);
    s_statsFrame = FrameStats{};
}

// RL-NX: R2_DRAW_LIST - the first draws of a frame, with the texture each binds and where it lands,
// flushed when the frame ends. The first version logged only frames whose index was a multiple of 600,
// which caught the title transition (frame 600: the mosaic atlases and the movie quad) and then only
// the START arrow / icon frames, never Select Game; the flush now happens for any frame with 200 or
// more draws (menus, 3D scenes) or with a draw covering 40 % or more of the EFB, at most 8 frames and
// 120 apart, and every sampled frame reports its largest single draw, so "is a screen-filling quad
// submitted" is answered per frame. R2_DRAW_LIST_AT=<frame> forces frames <frame>..<frame>+15.
struct DrawListEntry {
    u8 op;
    u16 verts;
    u32 tex;
    u16 w, h;
    u8 fmt;
    bool measured;
    DrawBounds b;
    DrawUv uv;  // RL-NX: the coordinates it samples and the texels there
};
constexpr u32 kDrawListMax = 160;
DrawListEntry s_dl[kDrawListMax];
u32 s_dlCount = 0;    // draws buffered this frame (the first kDrawListMax)
u32 s_dlDraws = 0;    // draws submitted this frame
u32 s_dlBig = 0;      // of them, ones covering 40 % or more of the EFB
u32 s_dlFlushed = 0;  // frames flushed so far
u64 s_dlLastFlush = ~0ull;
DrawListEntry s_dlLargest;
u32 s_dlLargestIdx = 0;  // RL-NX: its draw index (the "covering draw" the UV log names)
float s_dlLargestArea = 0.f;
// RL-NX: the frame's large or absurd draws, kept apart from the "first 40" buffer: a draw covering 40 % or
// more of the EFB, or one whose measured corners are past any sane frame (+-4096 px), which is a broken
// transform rather than a small quad. Select Game submits 1024x1024 CMPR atlases and 20+ such draws while
// the screen still shows only text, so these are the draws the missing image has to be in.
DrawListEntry s_dlBigList[12];
u32 s_dlBigIdx[12];
u32 s_dlBigN = 0;

void drawListNote(const u8* p, u32 len, u8 op) {
    const u32 img = s_bp[0x88];
    DrawListEntry e{};
    e.op = op;
    e.verts = u16(be16(p + 1));
    e.tex = (s_bp[0x94] & 0xFFFFFF) << 5;
    e.w = u16((img & 0x3FF) + 1);
    e.h = u16(((img >> 10) & 0x3FF) + 1);
    e.fmt = u8((img >> 20) & 0xF);
    e.measured = drawBoundsEfb(p, len, e.b);
    drawUv(p, len, e.uv);  // RL-NX: what it samples, even when it cannot be placed
    const float area = e.measured ? std::max(0.f, e.b.x1 - e.b.x0) * std::max(0.f, e.b.y1 - e.b.y0) : 0.f;
    const bool absurd = e.measured && (std::fabs(e.b.x0) > 4096.f || std::fabs(e.b.x1) > 4096.f ||
                                      std::fabs(e.b.y0) > 4096.f || std::fabs(e.b.y1) > 4096.f);
    if (area >= 0.4f * 640.f * 480.f || absurd) {
        s_dlBig++;
        if (s_dlBigN < 12) {
            s_dlBigIdx[s_dlBigN] = s_dlDraws;
            s_dlBigList[s_dlBigN++] = e;
        }
    }
    if (area > s_dlLargestArea) {
        s_dlLargestArea = area;
        s_dlLargest = e;
        s_dlLargestIdx = s_dlDraws;
    } else if (!e.measured && e.tex && !s_dlLargest.tex) {
        // RL-NX: nothing placed yet and this draw is textured: the first packet we cannot place is the
        // lead to follow when no covering draw can be measured at all.
        s_dlLargest = e;
        s_dlLargestIdx = s_dlDraws;
    }
    if (s_dlCount < kDrawListMax) s_dl[s_dlCount++] = e;
    s_dlDraws++;
}

// RL-NX: R2_DRAW_LIST - the sampling half of a logged draw: the texels its coordinates ask for and what
// the texture holds there. "Empty atlas space" (wrong UV, wrong atlas offset, a coordinate never filled)
// and "the wrong texture object" (the whole image is not at that address) show the same on screen.
// RL-NX: R2_DRAW_LIST - a draw whose positions come from an indexed array, decoded: the index each vertex
// carries, the array base/stride they are read through and the position each resolves to. This is the
// transition draws' blind spot (9.11): "EFB x 0..0" says nothing about whether that is a real quad placed
// wrongly or an array the game never filled, and only these numbers separate the two. A quad lands
// correctly here and wrongly in the frame if the transform is at fault; indices that read unmapped memory,
// or that all resolve to the same point, say the array itself was never written.
void logDrawIndex(u32 idx, const DrawListEntry& e) {
    const DrawBounds::PosIndex& p = e.b.pos;
    char buf[384];
    if (!p.any) {
        // direct positions: no array, so nothing was missed - the packet carries its own coordinates
        logf("gfx", "list frame %llu draw %u: positions direct (VCD 0x%08X/0x%08X, type %u), no array read",
             (unsigned long long)s_frames, idx, s_vcdLo, s_vcdHi, p.type);
        return;
    }
    int n = snprintf(buf, sizeof buf, "list frame %llu draw %u: positions indexed (%u-bit), array %08X stride %u, %u recorded%s:",
                     (unsigned long long)s_frames, idx, p.type == 2 ? 8u : 16u, p.base, p.stride, p.n,
                     p.unreadable ? " (an index reads unmapped memory)" : "");
    for (u32 i = 0; i < p.n && n < int(sizeof buf) - 80; i++)
        n += snprintf(buf + n, sizeof buf - size_t(n), " [%u]=(%g %g %g)", p.idx[i], p.v[i][0], p.v[i][1], p.v[i][2]);
    logf("gfx", "%s", buf);
}

void logDrawUv(u32 idx, const DrawListEntry& e) {
    const DrawUv& u = e.uv;
    if (!u.ok) {
        if (u.why)
            logf("gfx", "list frame %llu draw %u: texture %08X %ux%u fmt %u, coordinates: %s",
                 (unsigned long long)s_frames, idx, e.tex, e.w, e.h, e.fmt, u.why);
        return;
    }
    logf("gfx",
         "list frame %llu draw %u: texture %08X %ux%u fmt %u%s, texgen0 type %u src %u, coords fmt %u x %u frac %u, raw u "
         "%g..%g v %g..%g -> texels u %g..%g v %g..%g, wrap %u/%u, corner texels %08X %08X %08X %08X (u0v0 u1v0 u0v1 "
         "u1v1)",
         (unsigned long long)s_frames, idx, e.tex, e.w, e.h, e.fmt, u.copyDest ? " (EFB copy destination)" : "", u.texgen,
         u.src, u.coordFmt, u.coordCnt, u.frac, u.ru0, u.ru1, u.rv0, u.rv1, u.u0, u.u1, u.v0, u.v1, u.wrapS, u.wrapT, u.t00,
         u.t10, u.t01, u.t11);
}

void drawListFlush(const char* why) {
    logf("gfx", "list frame %llu (%s): %u draws, %u cover 40 %% or more of the EFB; largest draw EFB x %.0f..%.0f y "
         "%.0f..%.0f, texture %08X %ux%u fmt %u%s",
         (unsigned long long)s_frames, why, s_dlDraws, s_dlBig, s_dlLargest.b.x0, s_dlLargest.b.x1, s_dlLargest.b.y0,
         s_dlLargest.b.y1, s_dlLargest.tex, s_dlLargest.w, s_dlLargest.h, s_dlLargest.fmt,
         s_dlLargestArea > 0.f ? "" : " (no draw measurable)");
    if (s_dlLargest.tex) logDrawUv(s_dlLargestIdx, s_dlLargest);  // RL-NX: what the covering draw samples
    if (s_dlLargest.tex) logDrawIndex(s_dlLargestIdx, s_dlLargest);  // RL-NX: and its indexed positions
    for (u32 i = 0; i < s_dlCount; i++) {
        const DrawListEntry& e = s_dl[i];
        logf("gfx", "list frame %llu draw %u: op %02X verts %u, texture %08X %ux%u fmt %u, EFB x %.0f..%.0f y %.0f..%.0f",
             (unsigned long long)s_frames, i, e.op, e.verts, e.tex, e.w, e.h, e.fmt, e.measured ? e.b.x0 : 0.f,
             e.measured ? e.b.x1 : 0.f, e.measured ? e.b.y0 : 0.f, e.measured ? e.b.y1 : 0.f);
        // RL-NX: an indexed draw that placed nothing (unmeasurable, or every vertex at one point) is the
        // case 9.11 could not settle; the placed ones need no explaining.
        if (!e.measured || (e.b.x1 - e.b.x0) * (e.b.y1 - e.b.y0) <= 0.f) logDrawIndex(i, e);
    }
    for (u32 i = 0; i < s_dlBigN; i++) {  // RL-NX: R2_DRAW_LIST's large / absurd draws, with their index
        const DrawListEntry& e = s_dlBigList[i];
        logf("gfx", "list frame %llu big draw %u: op %02X verts %u, texture %08X %ux%u fmt %u, EFB x %.0f..%.0f y "
             "%.0f..%.0f",
             (unsigned long long)s_frames, s_dlBigIdx[i], e.op, e.verts, e.tex, e.w, e.h, e.fmt, e.b.x0, e.b.x1, e.b.y0,
             e.b.y1);
        if (e.tex) logDrawUv(s_dlBigIdx[i], e);
        logDrawIndex(s_dlBigIdx[i], e);  // RL-NX: the frame's large draws, and what their indices read
    }
}

void drawListFrameEnd() {
    if (!s_drawListOn) return;
    static const u64 at = getenv("R2_DRAW_LIST_AT") ? strtoull(getenv("R2_DRAW_LIST_AT"), nullptr, 10) : ~0ull;
    const bool forced = s_frames >= at && s_frames < at + 16;
    const bool interesting = s_dlDraws >= 200 || s_dlBig != 0;
    const bool spaced = s_dlLastFlush == ~0ull || s_frames >= s_dlLastFlush + 60;
    if (forced || (interesting && spaced && s_dlFlushed < 40)) {
        drawListFlush(forced ? "forced" : "selected");
        s_dlFlushed++;
        s_dlLastFlush = s_frames;
    } else if (s_frames % 300 == 0) {
        drawListFlush("sample");  // the timeline: which draw is the largest, and does it change?
    }
    s_dlCount = s_dlDraws = s_dlBig = s_dlBigN = 0;
    s_dlLargestArea = 0.f;
    s_dlLargestIdx = 0;
    s_dlLargest = DrawListEntry{};
}

// The first few distinct placements we have not seen, each once, kept apart for perspective and
// orthographic draws: the 2D draws are the first of a frame and were always right, so the 3D ones need
// their own list.
void drawBoundsLog(u8 op, const DrawBounds& b, u32 verts, bool persp) {
    static std::vector<DrawBounds> seen[2];
    std::vector<DrawBounds>& list = seen[persp ? 1 : 0];
    for (const DrawBounds& s : list) {
        const bool same = s.x0 - b.x0 < 1.f && b.x0 - s.x0 < 1.f && s.y0 - b.y0 < 1.f && b.y0 - s.y0 < 1.f &&
                          s.x1 - b.x1 < 1.f && b.x1 - s.x1 < 1.f && s.y1 - b.y1 < 1.f && b.y1 - s.y1 < 1.f;
        if (same) return;
    }
    if (list.size() >= 8) return;
    list.push_back(b);
    const u32 tp = s_bp[0x20] & 0x7FF, bm = s_bp[0x21] & 0x7FF;
    const u32 lfS = (s_bp[0x20] >> 12) & 0x7FF, rtS = (s_bp[0x21] >> 12) & 0x7FF;
    const s32 offX = s_boxBiasSent ? s_boxOffX : 0, offY = s_boxBiasSent ? s_boxOffY : 0;
    const float sx = asFloat(s_xfVp[0]), sy = asFloat(s_xfVp[1]), ox = asFloat(s_xfVp[3]), oy = asFloat(s_xfVp[4]);
    logf("gfx",
         "bounds v%02X %u verts (%s): EFB x %.0f..%.0f y %.0f..%.0f (renderer), x %.0f..%.0f y %.0f..%.0f (hardware, "
         "offset %d,%d); scissor sent x %d..%d y %d..%d; aurora viewport x %.0f..%.0f y %.0f..%.0f",
         op, verts, persp ? "3D" : "2D", b.x0, b.x1, b.y0, b.y1, b.hx0, b.hx1, b.hy0, b.hy1, offX, offY,
         int(lfS) - offX, int(rtS) - offX, int(tp) - offY, int(bm) - offY, ox - 340.f - sx, ox - 340.f + sx,
         oy - 340.f + sy, oy - 340.f - sy);
}

// At the frame's end: what all of its draws cover together, against the scissor in force.
void drawBoundsFrameEnd() {
    if (!s_boundsOn || !s_boundsDraws) return;
    if (s_boundsFrames < 6 || s_boundsFrames % 60 == 0) {
        const u32 tp = s_bp[0x20] & 0x7FF, bm = s_bp[0x21] & 0x7FF;
        const u32 lfS = (s_bp[0x20] >> 12) & 0x7FF, rtS = (s_bp[0x21] >> 12) & 0x7FF;
        const s32 offX = s_boxBiasSent ? s_boxOffX : 0, offY = s_boxBiasSent ? s_boxOffY : 0;
        logf("gfx",
             "bounds frame %u: %u draws cover EFB x %.0f..%.0f y %.0f..%.0f (renderer), x %.0f..%.0f y %.0f..%.0f "
             "(hardware, offset %d,%d); %u of them 3D cover x %.0f..%.0f y %.0f..%.0f; last scissor x %d..%d y %d..%d",
             s_boundsFrames, s_boundsDraws, s_bUx0, s_bUx1, s_bUy0, s_bUy1, s_bHx0, s_bHx1, s_bHy0, s_bHy1, offX,
             offY, s_bPerspDraws, s_bPx0, s_bPx1, s_bPy0, s_bPy1, int(lfS) - offX, int(rtS) - offX, int(tp) - offY,
             int(bm) - offY);
    }
    s_boundsFrames++;
    s_boundsDraws = 0;
    s_bPerspDraws = 0;
    s_bUx0 = s_bUy0 = s_bHx0 = s_bHy0 = s_bPx0 = s_bPy0 = 1e30f;
    s_bUx1 = s_bUy1 = s_bHx1 = s_bHy1 = s_bPx1 = s_bPy1 = -1e30f;
}

// the draw at p (already sent once): a wing gets its mirror image
void xwingDraw(const u8* p, u32 len) {
    if (!s_menuTiling || !s_projKnown || s_xfProj[6] == 0) return;
    const u32 img = s_bp[0x88];
    if (((img >> 20) & 0xF) != 0xE) return;  // CMPR
    const u32 w = (img & 0x3FF) + 1, h = ((img >> 10) & 0x3FF) + 1;
    const bool wing = w == 560 && h == 160, body = w == 224 && h == 256;
    if (!wing && !body) return;
    float x0, x1;
    if (!drawExtentX(p, len, x0, x1)) return;
    const float c = (x0 + x1) * 0.5f;
    if (body) {
        if (s_wingSeen) {
            s_wingOffset = c - s_wingCenter;
            s_wingOffsetKnown = true;
        }
        return;
    }
    s_wingSeen = true;
    s_wingCenter = c;
    if (!s_wingOffsetKnown || !tileCopies()) return;
    const u32 gen = s_bp[0x00] & 0xFFFFFF, cull = (gen >> 14) & 3;
    const bool swap = cull == 1 || cull == 2;
    if (swap) bpFull((gen & ~(3u << 14)) | ((3 - cull) << 14));
    sendProjection(0.f, true, c + s_wingOffset);
    fifo::write_data(p, len);
    fifo::finish_draw();
    sendProjection();
    if (swap) bpFull(gen);
}

// --- full-frame cards in widened views ------------------------------------------------------
// The crawl's "A long time ago..." card is one perspective quad (640x384 CMPR: blue text on
// black, clamped) placed to fill the cinematic's own 2.105:1 view edge to edge. Widescreen opens
// that view, and on displays wider than 2.105 (21:9, 32:9) the starfield drawn before it showed
// past its sides; on displays below about 3:2 (windows) the view opens taller and it showed above
// and below. Such a quad (four vertices covering the game's view, direct f32 positions, no indexed
// attributes) gets four extensions drawn after it: from each edge outward, every vertex carrying
// that edge's attributes (texture coordinates: the clamped edge column or row, the card's
// border). They do not overlap the card, so a fading card blends the same everywhere; where the
// view does not reach past the card they lie off-screen.
void cardFill(const u8* p, u32 len) {
    if (p[0] != 0x80 || be16(p + 1) != 4 || !s_projKnown || s_xfProj[6] != 0) return;  // one perspective quad
    const u32 lo = s_vcdLo, hi = s_vcdHi, fmt = p[0] & 7, A = s_vatA[fmt];
    if (lo & 0x1FF) return;                    // per-vertex matrix indices
    if (((lo >> 9) & 3) != 1 || ((A >> 1) & 7) != 4 || !(A & 1)) return;  // direct f32 xyz positions
    for (u32 sh = 11; sh <= 15; sh += 2)
        if (((lo >> sh) & 3) > 1) return;     // indexed normals / colors
    for (u32 t = 0; t < 8; t++)
        if (((hi >> (2 * t)) & 3) > 1) return;  // indexed texture coordinates
    const u32 vs = (len - 3) / 4;
    if (vs < 12) return;
    const float* m = xfPosMatrix((s_cpMtxA & 0x3F) * 4);
    if (!m) return;
    const float p0 = asFloat(s_xfProj[0]), p1 = asFloat(s_xfProj[1]), p2 = asFloat(s_xfProj[2]),
                p3 = asFloat(s_xfProj[3]);
    float pos[4][3], nx[4], ny[4];
    for (u32 i = 0; i < 4; i++) {
        const u8* d = p + 3 + i * vs;  // positions come first (no matrix indices)
        for (u32 c = 0; c < 3; c++) pos[i][c] = asFloat(be32(d + 4 * c));
        const float* v = pos[i];
        const float ex = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
        const float ey = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
        const float ez = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
        if (ez >= 0.f) return;
        nx[i] = (p0 * ex + p1 * ez) / -ez;
        ny[i] = (p2 * ey + p3 * ez) / -ez;
    }
    const float minX = std::min({nx[0], nx[1], nx[2], nx[3]}), maxX = std::max({nx[0], nx[1], nx[2], nx[3]});
    const float minY = std::min({ny[0], ny[1], ny[2], ny[3]}), maxY = std::max({ny[0], ny[1], ny[2], ny[3]});
    if (minX > -0.98f || maxX < 0.98f || minY > -0.98f || maxY < 0.98f) return;  // not the whole view
    // a screen-aligned rectangle: two vertices on each side edge, two on the top and bottom
    const float tolX = 0.02f * (maxX - minX), tolY = 0.02f * (maxY - minY);
    bool left[4], top[4];
    for (u32 i = 0; i < 4; i++) {
        left[i] = nx[i] - minX < tolX;
        if (!left[i] && maxX - nx[i] >= tolX) return;
        top[i] = maxY - ny[i] < tolY;
        if (!top[i] && ny[i] - minY >= tolY) return;
    }
    u32 corners = 0;  // one vertex at each
    for (u32 i = 0; i < 4; i++) corners |= 1u << (left[i] * 2 + top[i]);
    if (corners != 15) return;
    // the vertex on the other side edge in the same row (h), on the other of top and bottom in
    // the same column (v)
    u32 partner[2][4] = {};
    for (u32 i = 0; i < 4; i++)
        for (u32 j = 0; j < 4; j++) {
            if (left[j] != left[i] && top[j] == top[i]) partner[0][i] = j;
            if (top[j] != top[i] && left[j] == left[i]) partner[1][i] = j;
        }
    constexpr float kOut = 4.f;  // how far out (card sizes): past a 32:9 screen's sides
    static std::vector<u8> buf;
    buf.assign(p, p + len);
    for (int side = 0; side < 4; side++) {  // left, right, top, bottom extensions
        const bool* edge = side < 2 ? left : top;
        const u32* other = partner[side / 2];
        for (u32 i = 0; i < 4; i++) {
            const bool onSide = edge[i] == (side % 2 == 0);
            const u32 src = onSide ? i : other[i];  // that edge's vertex in this row / column
            u8* d = buf.data() + 3 + i * vs;
            memcpy(d, p + 3 + src * vs, vs);
            if (onSide) {  // moved outward, away from the opposite edge
                const u32 o = other[i];
                for (u32 c = 0; c < 3; c++) {
                    const u32 w = asBits(pos[i][c] + (pos[i][c] - pos[o][c]) * kOut);
                    d[4 * c] = u8(w >> 24); d[4 * c + 1] = u8(w >> 16); d[4 * c + 2] = u8(w >> 8); d[4 * c + 3] = u8(w);
                }
            }
        }
        fifo::write_data(buf.data(), len);
        fifo::finish_draw();
    }
}

void tileFrameEnd() {
    s_tileRun.clear();
    s_tileRunLast = 0;
    s_tileRunOk = false;
    s_seamPending = false;
    s_wingSeen = false;
    // a screen's tiles stop being drawn: forget them (the memory may hold other textures later)
    if (s_frames % 64 == 0)
        for (auto it = s_tileTex.begin(); it != s_tileTex.end();)
            it = s_frames - it->second.lastFrame > 120 ? s_tileTex.erase(it) : std::next(it);
}
}  // namespace

void setMenuTiling(bool on) {
    if (on == s_menuTiling) return;
    bridgeDrain();  // RL-NX (9.53): hle_game.cpp calls this on the game thread; the tile state is the worker's
    s_menuTiling = on;
    s_tileTex.clear();
    s_tileRun.clear();
    s_tileRunOk = false;
    s_seamPending = false;
    s_wingOffsetKnown = false;
}

namespace {

void onCommand(const u8* p, u32 len) {
    ensureFrame();
    if (s_dumpAtTb && tbNow() >= s_dumpAtTb && !s_dump) [[unlikely]] {
        s_dumpAtTb = 0;
        if (!s_dumpTimes.empty()) {
            s_dumpAtTb = s_dumpTimes.front();
            s_dumpTimes.erase(s_dumpTimes.begin());
        }
        s_dumpFrame = s_frames + 1;
    }
    if (s_frames >= s_dumpFrame && s_frames < s_dumpFrame + s_dumpCount) [[unlikely]] {
        if (!s_dump) {
            char name[96];
#ifdef __SWITCH__
            // RL-NX: on the Switch the working directory is not the app's folder: "build/" never opened
            snprintf(name, sizeof name, "sdmc:/switch/RogueLeader-NX/logs/gx_frame_%llu.txt", (unsigned long long)s_frames);
#else
            snprintf(name, sizeof name, "build/gx_frame_%llu.txt", (unsigned long long)s_frames);
#endif
            s_dump = fopen(name, "w");
        }
        if (s_dump && s_frames != s_dumpLast) {
            // the frame's starting state: every BP register and the vertex arrays
            fprintf(s_dump, "=== presented frame %llu\nBP state:", (unsigned long long)s_frames);
            for (int r = 0; r < 256; r++) fprintf(s_dump, "%s%02X=%06X", r % 16 ? " " : "\n  ", r, s_bp[r] & 0xFFFFFF);
            fprintf(s_dump, "\nCP arrays:");
            for (int r = 0; r < 16; r++) fprintf(s_dump, " %d:%08X/%u", r, s_arrays[r].base, s_arrays[r].stride);
            fputc('\n', s_dump);
            s_dumpLast = s_frames;
        }
        if (s_dump) dumpCommand(p, len);
    } else if (s_dump) {
        for (const auto& l : s_dumpLoads) {
            fprintf(s_dump, "LATER %08X:", l.addr);
            if (const u8* d = physPtr(l.addr, l.words * 4))
                for (u32 i = 0; i < l.words; i++) fprintf(s_dump, " %08X", be32(d + 4 * i));
            fputc('\n', s_dump);
        }
        s_dumpLoads.clear();
        fclose(s_dump);
        s_dump = nullptr;
        s_dumpFrame = ~0ull;
        logf("gfx", "frames dumped to build/gx_frame_*.txt");
    }
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        // skipping a cinematic (hle_game.cpp): the game runs flat out and nothing is drawn
        if (cinematicSkip()) return;
        if (skipDraw(op)) [[unlikely]]
            return;
        if (s_seamPending && !isTileDraw()) seamBlend();  // the background is complete
        // RL-NX diagnostic (R2_DRAW_LOG): which scissor bands the draws use, with the viewport that goes
        // with the first of each. The first picture the Switc renderer drew had content in one 30-row
        // strip of a 480-row target, which is what a per-strip scissor (or the projection shift that must
        // accompany it) that never reaches the renderer looks like.
        {
            static const bool drawLog = envFlag("R2_DRAW_LOG");
            static std::map<u32, u32> seen;
            if (drawLog && seen.size() < 24) {
                const u32 tp = s_bp[0x20] & 0x7FF, bm = s_bp[0x21] & 0x7FF;
                // The viewport is part of the key: a picture squeezed into a band means either the game
                // asked for a short viewport or the renderer is not applying the one it asked for.
                u32 key = (tp << 16) | bm;
                for (int i = 0; i < 5; ++i) key = key * 1000003u ^ s_xfVp[i];
                if (++seen[key] == 60)
                    logf("gfx", "draws at scissor x %u..%u y %u..%u, viewport scale %g %g origin %g %g, bp59 0x%05X "
                              "(offset %d,%d, seen %d), proj p0 %g p1 %g p3 %g",
                         (s_bp[0x20] >> 12) & 0x7FF, (s_bp[0x21] >> 12) & 0x7FF, tp, bm, asFloat(s_xfVp[0]),
                         asFloat(s_xfVp[1]), asFloat(s_xfVp[3]), asFloat(s_xfVp[4]), s_bp[0x59] & 0xFFFFF, s_boxOffX,
                         s_boxOffY, int(s_boxBiasSent), asFloat(s_xfProj[0]), asFloat(s_xfProj[1]), asFloat(s_xfProj[3]));
            }
        }
        // RL-NX diagnostic (R2_DRAW_LIST): every draw buffered with the texture it binds and where it lands
        // (drawBoundsEfb), dumped at the frame's end (drawListFrameEnd) - to see whether a screen-filling
        // image quad is submitted at all: the title's and the menus' backdrops are missing while text, icons
        // and medals (all textured) draw.
        if (s_drawListOn) drawListNote(p, len, op);
        if (s_statsOn) drawStatsNote(p, len, op);  // RL-NX: R2_STATS
        // RL-NX diagnostic (R2_DRAW_BOUNDS): where this draw lands in the EFB, transformed on the CPU
        // (drawBoundsEfb). Nothing is drawn differently; the numbers are what the picture is judged by.
        if (s_boundsOn) {
            DrawBounds b;
            if (drawBoundsEfb(p, len, b)) {
                drawBoundsLog(op, b, be16(p + 1), !s_xfProj[6]);
                s_bUx0 = std::min(s_bUx0, b.x0);
                s_bUx1 = std::max(s_bUx1, b.x1);
                s_bUy0 = std::min(s_bUy0, b.y0);
                s_bUy1 = std::max(s_bUy1, b.y1);
                s_bHx0 = std::min(s_bHx0, b.hx0);
                s_bHx1 = std::max(s_bHx1, b.hx1);
                s_bHy0 = std::min(s_bHy0, b.hy0);
                s_bHy1 = std::max(s_bHy1, b.hy1);
                s_boundsDraws++;
                if (!s_xfProj[6]) {  // perspective: the 3D scene, kept apart from the 2D overlays
                    s_bPerspDraws++;
                    s_bPx0 = std::min(s_bPx0, b.x0);
                    s_bPx1 = std::max(s_bPx1, b.x1);
                    s_bPy0 = std::min(s_bPy0, b.y0);
                    s_bPy1 = std::max(s_bPy1, b.y1);
                }
            }
        }
        screenSpace2D();
        fillScreen();
        prepareArrays(p);
        // RL-NX: R2_NO_GATES - the bisect that asks the renderer instead of the measurer: every draw goes out
        // with the scissor opened to the whole EFB, culling off, depth compare "always" with update on, and
        // colour+alpha update on. A screen that is black in the port and appears with this on is hidden by a
        // gate; one that stays black with every gate open is not being drawn at all. Nothing here touches the
        // bridge's shadow (bpFull only writes the FIFO), so the stream's own state is unaffected.
        {
            static const bool noGates = envFlag("R2_NO_GATES");
            if (noGates) {
                bpFull(0x20000000u | (340u << 12) | 340u);  // scissor TL: the whole screen, in register space
                bpFull(0x21000000u | (979u << 12) | 819u);  // scissor BR
                bpFull(s_bp[0x00] & ~(3u << 14));           // genMode: culling off
                bpFull(0xF3000000u | ((s_bp[0xF3] & ~0x1Fu) | 1u | (7u << 1) | (1u << 4)));  // zMode: always
                bpFull(0x41000000u | (s_bp[0x41] | (1u << 3) | (1u << 4)));  // cmode0: colour+alpha update
            }
        }
        prepareTextures();
        // Aurora merges consecutive draws with unchanged state. That broke on this game's
        // empty/short strips until Aurora patch 0005; R2_NO_MERGE=1 makes every draw stand alone
        // again (a TLUT load into slot 1023, none, is always handled and dirties the textures).
        // RL-NX: R2_STRICT also turns Aurora's draw merging off (a merged-away draw is invisible and
    // indistinguishable from one the game never sent, which is the whole ambiguity this bug is about).
    static const bool allowMerge = !envFlag("R2_NO_MERGE") && !envFlag("R2_STRICT");
        if (!allowMerge && s_bpMask == 0xFFFFFF) {
            fifo::write_u8(0x61);
            fifo::write_u32(0x650003FFu);
        }
        fifo::write_data(p, len);
        fifo::finish_draw();
        tileDraw(p, len);
        xwingDraw(p, len);
        cardFill(p, len);
        return;
    }
    switch (op) {
    case 0x08: {  // CP register
        const u8 reg = p[1];
        const u32 v = be32(p + 2);
        s_cp[reg] = v;
        s_cpValid.set(reg);
        if (reg >= 0xA0 && reg <= 0xAF) {  // array base: sent as a host pointer when used
            Array& a = s_arrays[reg - 0xA0];
            if (a.base != (v & 0x03FFFFFF)) {
                a.base = v & 0x03FFFFFF;
                a.sentSize = 0;
            }
            return;
        }
        if (reg >= 0xB0 && reg <= 0xBF) s_arrays[reg - 0xB0].stride = v & 0xFF;
        else if (reg == 0x50) s_vcdLo = v;
        else if (reg == 0x60) s_vcdHi = v;
        else if ((reg & 0xF8) == 0x70) s_vatA[reg & 7] = v;
        else if ((reg & 0xF8) == 0x80) s_vatB[reg & 7] = v;
        else if ((reg & 0xF8) == 0x90) s_vatC[reg & 7] = v;
        else if (reg == 0x30) s_cpMtxA = v;
        fifo::write_data(p, len);
        return;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {  // indexed XF load from arrays 12-15
        const u32 n = 12 + (op - 0x20) / 8;
        const u32 idx = be16(p + 1), cnt = (be16(p + 3) >> 12) + 1;
        const u32 stride = s_arrays[n].stride ? s_arrays[n].stride : 4;
        const u32 dst = be16(p + 3) & 0xFFF;
        if (const u8* d = physPtr(s_arrays[n].base + idx * stride, cnt * 4)) {
            // RL-NX: R2_ZEROMTX=keep drops a 12-word position-matrix load whose block is all zero, so slot 0
            // keeps the matrix it held before (out of our shadow and out of Aurora's FIFO). The game issues
            // one of these immediately before each cutscene-video strip draw, so all 12 strips of the
            // transition are transformed by a zero matrix (docs/PERF_HANDOFF.md 9.12/9.13). Whether the
            // backdrop then appears decides between "these draws are the missing picture" and "a zero
            // matrix is this game's way of hiding a draw".
            static const bool keepZeroMtx = [] {
                const char* v = getenv("R2_ZEROMTX");
                return v && !strcmp(v, "keep");
            }();
            if (keepZeroMtx && n == 12 && cnt == 12 && dst == 0) {
                bool allZero = true;
                for (u32 i = 0; i < cnt && allZero; i++) allZero = be32(d + 4 * i) == 0;
                if (allZero) {
                    static int logged = 0;
                    if (logged < 8) {
                        logged++;
                        logf("gfx", "R2_ZEROMTX=keep: zero 12-word position-matrix load from %08X left out; "
                                     "slot 0 keeps its previous matrix", s_arrays[n].base + idx * stride);
                    }
                    return;
                }
            }
            for (u32 i = 0; i < cnt && dst + i < kXfShadow; i++) {
                const u32 w = be32(d + 4 * i);
                s_xfMem[dst + i] = w;
                if (n == 12 && dst + i < 256) memcpy(&s_xfMtx[dst + i], &w, 4);  // position matrices
            }
            if (dst < kXfShadow) s_xfBlock[dst] = std::max(s_xfBlock[dst], u16(cnt));
        }
        sendArray(n, idx * stride + cnt * 4);
        // RL-NX: R2_STATS counts this load; it is forwarded unchanged. An A/B in which the words were
        // snapshotted here and sent as an immediate XF load (0x10) instead of being read by the renderer at
        // decode time produced pixel-identical frames with the same 900 KiB queue (docs/PERF_HANDOFF.md
        // 9.20), so the late read is not this port's problem in these scenes. The counter stays because it is
        // the only measure of how much of a frame takes this path: none before a level, hundreds to thousands
        // per level frame.
        if (s_statsOn) s_statsFrame.indxSnap++;
        fifo::write_data(p, len);
        return;
    }
    case 0x61: {  // BP register
        const u32 raw = be32(p + 1);
        const u8 reg = u8(raw >> 24);
        if (reg == kScreenSpaceMarker && (raw & 0xFFFFFE) == kScreenSpaceMagic) {
            s_screenSpaceWindow = raw & 1;  // hle_TargetingPass's bracket: not for the GPU (and
            return;                         // a pending BP mask stays for the game's next write)
        }
        if (reg == kScreenSpaceMarker && (raw & 0xFFFFFE) == kScreenSpaceAllMagic) {
            s_screenSpaceAllWindow = raw & 1;  // hle_ReticleDraw's bracket
            return;
        }
        if (reg == kScreenSpaceMarker && (raw & 0xFFFFFE) == kFillMagic) {
            s_fillWindow = raw & 1;  // hle_CutsceneVideoDraw's bracket
            return;
        }
        if (reg == 0xFE) {
            s_bpMask = raw & 0xFFFFFF;
            fifo::write_data(p, len);
            return;
        }
        const u32 v = (s_bp[reg] & ~s_bpMask) | (raw & s_bpMask & 0xFFFFFF);
        s_bpMask = 0xFFFFFF;
        s_bp[reg] = v;
        s_bpValid.set(reg);
        // RL-NX: BP 0x66 is GXInvalidateTexAll - the game telling the GPU that texture memory changed. The
        // hashing cache (9.34) takes it as its own signal: every texture's next bind takes a full hash again.
        if (reg == 0x66) ++s_texEpoch;
        if ((reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB)) {
            const u32 idx = reg & 0x1F;
            if (idx < 0x1C) s_tex[(idx & 3) + (reg >= 0xA0 ? 4 : 0)].dirty = true;
        } else if (reg == 0x59) {
            // Scissor box offset. Hardware: field = (offset + 340) / 2, and both the scissor
            // box and the primitives move by -offset in the EFB. Aurora only moves its scissor,
            // by 2 * a 9-bit signed field (its GXSetScissorBoxOffset writes (offset - 342) / 2),
            // which cannot hold this game's offsets (the bottom-half pass uses 238). So Aurora
            // always gets the fixed bias (-340) and the bridge applies the offset itself: to the
            // scissor box and, through the projection, to the geometry.
            const s32 offX = s32(2 * (v & 0x3FF)) - 340, offY = s32(2 * ((v >> 10) & 0x3FF)) - 340;
            // RL-NX: the offsets themselves, once per value - the two-pass frames move by 238, the frame
            // itself by 340, and a wrong reading of the field would show up here first.
            {
                static s32 loggedX = -1, loggedY = -1;
                if (s_copyLog && (offX != loggedX || offY != loggedY)) {
                    loggedX = offX;
                    loggedY = offY;
                    logf("gfx", "scissor box offset %d,%d (field 0x%x, bp59 0x%05X)", offX, offY, v & 0xFFFFF, v);
                }
            }
            if (offX != s_boxOffX || offY != s_boxOffY || !s_boxBiasSent) {
                s_boxOffX = offX;
                s_boxOffY = offY;
                s_boxBiasSent = true;
                const u32 f = (0u - 170u) & 0x3FF;
                bpFull(0x59000000u | (f << 10) | f);
                sendScissor();
                sendProjection();
            }
            return;
        } else if (reg == 0x20 || reg == 0x21) {  // scissor box: moved by the box offset
            sendScissor();
            return;
        } else if (reg == 0x65) {  // TLUT load into TMEM: remember it, bind at draw time
            s_tlutLoads[v & 0x3FF] = {(s_bp[0x64] & 0x1FFFFF) << 5, ((v >> 10) & 0x7FF) * 16};
            return;
        } else if (reg == 0x52) {
            if (s_seamPending) seamBlend();  // before the copy takes the background
            const bool disp = v & (1u << 14);
            // A display copy makes a picture only if it lands in the frame buffer the VI scans
            // out (front or back). The game builds each 640x450 XFB from two 640x226 copies
            // (top and bottom halves rendered in separate EFB passes), and also uses display
            // copies into a scratch buffer just to clear EFB bands: those go to Aurora as
            // texture copies, which keeps the clear without touching the picture.
            const u32 dest = (s_bp[0x4B] & 0xFFFFFF) << 5, stride = (s_bp[0x4D] & 0x3FF) * 32;
            const u32 h = ((s_bp[0x4A] >> 10) & 0x3FF) + 1;
            u32 line = 0, lines = 0;
            const bool shown = disp && viLocateCopy(dest, stride, line, lines);
            // Reaching the bottom completes the frame. So does a second copy to the top (a
            // frame that never reached the bottom was a whole picture after all), which bounds
            // how many game frames can pile into one of Aurora's. Battle of Hoth's intro copies
            // its first 304-line frame while the VI still scans 450 lines (the new VI setup
            // lands at the next retrace), the second into an XFB the VI does not show yet, and
            // only the third completes: ~54,000 draws in one frame (Aurora patch 0018).
            const bool completes = shown && (line + h >= lines || (line == 0 && s_topCopied));
            if (shown && line == 0) s_topCopied = true;
            if (disp && s_copyLog)
                logf("gfx", "display copy src %u,%u %ux%u stride %u -> %08X: %s line %u of %u (%u draws in the frame)", s_bp[0x49] & 0x3FF,
                     (s_bp[0x49] >> 10) & 0x3FF, (s_bp[0x4A] & 0x3FF) + 1, h, stride, dest, shown ? "XFB" : "not in XFB",
                     line, lines, s_frameDraws);
            static const bool presentAtTexCopy = getenv("R2_NO_XFB") && atoi(getenv("R2_NO_XFB")) == 2;
            if (!disp && presentAtTexCopy) {  // debugging: show the EFB as a texture copy sees it
                endFrame();
                ensureFrame();
            }
            if (disp && !shown) {
                const u32 tex = v & ~(1u << 14);
                prepareCopy(tex);
                fifo::write_u8(0x61);
                fifo::write_u32(0x52000000u | tex);
                fifo::publish();
                return;
            }
            if (completes && !s_xfb) {  // present the EFB as it is before the copy clears it
                endFrame();
                ensureFrame();
            }
            if (shown && s_copyLog) {
                // RL-NX: R2_COPY_LOG diagnostic - does the CPU draw into the frame buffer? Copies never reach
                // guest memory here, so XFB bytes that differ from the previous copy into the same place were
                // written by the CPU (a movie decoder writing YUYV straight into the XFB), which the GPU-side
                // present never shows.
                static std::unordered_map<u32, u64> s_xfbHash;
                static u32 logs = 0, copies = 0, changed = 0;
                const u32 bytes = stride * h;
                if (const u8* m = physPtr(dest, bytes)) {
                    const u64 hsh = XXH3_64bits(m, bytes);
                    const auto it = s_xfbHash.find(dest);
                    const bool diff = it != s_xfbHash.end() && it->second != hsh;
                    s_xfbHash[dest] = hsh;
                    copies++;
                    if (diff) changed++;
                    if (diff && logs < 40 && ++logs)
                        logf("gfx", "XFB %08X (line %u, %u lines): memory changed since the last copy there - the CPU drew into it (%u draws in the frame)",
                             dest, line, h, s_frameDraws);
                    if ((copies % 600) == 0) logf("gfx", "XFB copies %u, %u over CPU-written memory", copies, changed);
                }
            }
            if (shown) {
                aur(GX_AURORA_LOAD_DISP_COPY_DST);
                fifo::write_u32(line);
                fifo::write_u32(lines);
            }
            prepareCopy(v);
            fifo::write_data(p, len);
            fifo::publish();
            if (completes && s_xfb) endFrame();  // the XFB is complete
            return;
        }
        // Debugging: R2_GX_OVERRIDE=nocull,zalways,aalways,nofog relaxes per-draw tests to find
        // what rejects missing geometry.
        static const char* ovr = getenv("R2_GX_OVERRIDE");
        if (ovr && (reg == 0x00 || reg == 0x40 || reg == 0xF3 || reg == 0xF1 || reg == 0x41)) {
            u32 w = v;
            if (reg == 0x00 && strstr(ovr, "nocull") && ((w >> 14) & 3) != 3) w &= ~(3u << 14);  // not CULL_ALL
            if (reg == 0x40 && strstr(ovr, "zalways")) w |= 7u << 1;
            if (reg == 0xF3 && strstr(ovr, "aalways")) w = (w & ~(0xFFu << 16)) | (7u << 16) | (7u << 19);
            if (reg == 0xF1 && strstr(ovr, "nofog")) w &= ~(7u << 21);
            // RL-NX: noblend clears BP 0x41 bit 0 (blend enable), so a draw writes its own colour instead of
            // blending it. The title page's 2D quads go out with blend on and type subtract (BP 0x41 = 0x0004BD)
            // and the alpha channel takes its value from the vertex while the format sends none, so a zero
            // alpha subtracts nothing and the quad leaves the EFB untouched: that is the shape of a draw that
            // is submitted every frame, transforms correctly and paints no pixels.
            if (reg == 0x41 && strstr(ovr, "noblend")) w &= ~1u;
            bpFull((u32(reg) << 24) | w);
            return;
        }
        fifo::write_data(p, len);
        // Draw done / draw sync tokens: the game may reuse the memory behind earlier draws once
        // it sees them (Aurora's worker reads arrays and textures when it gets to a draw)
        if ((reg == 0x45 && (v & 2)) || reg == 0x47 || reg == 0x48) gpSync();
        return;
    }
    case 0x10: {  // XF register load: shadow the viewport and projection (scissor box offset)
        const u32 hdr = be32(p + 1);
        const u32 addr = hdr & 0xFFFF, cnt = (len - 5) / 4;
        if (addr < kXfShadow) s_xfBlock[addr] = std::max(s_xfBlock[addr], u16(cnt));  // (the longest: it covers most)
        for (u32 i = 0; i < cnt; i++) {
            const u32 a = addr + i, w = be32(p + 5 + 4 * i);
            if (a < kXfShadow) s_xfMem[a] = w;
            if (a < 0x100) {
                memcpy(&s_xfMtx[a], &w, 4);
            } else if (a >= 0x101A && a <= 0x101F) {
                s_xfVp[a - 0x101A] = w;
            } else if (a >= 0x1020 && a <= 0x1026) {
                s_xfProj[a - 0x1020] = w;
                s_projKnown = true;
                if (a == 0x1026) s_projTypeWrites++;  // RL-NX: is the projection type ever written directly?
            }
        }
        // RL-NX: the viewport goes through untouched. (The rework that translated its origin by the
        // scissor box offset was here; that theory is the one that did not hold - see the scissor box
        // offset section. The offset goes into the projection, sendProjection.)
        // RL-NX: R2_NO_EMBOSS=1 (debugging) turns emboss texgens (XF 0x1040-0x1047 type 1, bump mapping from
        // the binormal/tangent) into plain matrix texgens, to tell whether bump-mapped draws (ships, the title's
        // metal backdrop) are what renders black.
        static const bool noEmboss = envFlag("R2_NO_EMBOSS");
        if (noEmboss && addr <= 0x1047 && addr + cnt > 0x1040 && len <= 5 + 4 * 64) {
            u8 buf[5 + 4 * 64];
            memcpy(buf, p, len);
            for (u32 i = 0; i < cnt; i++) {
                const u32 a = addr + i;
                if (a < 0x1040 || a > 0x1047) continue;
                u32 w = be32(buf + 5 + 4 * i);
                if (((w >> 4) & 7) == 1) {
                    w &= ~(7u << 4);
                    buf[5 + 4 * i] = u8(w >> 24); buf[6 + 4 * i] = u8(w >> 16);
                    buf[7 + 4 * i] = u8(w >> 8);  buf[8 + 4 * i] = u8(w);
                }
            }
            fifo::write_data(buf, len);
        } else {
            fifo::write_data(p, len);
        }
        // RL-NX: a projection (or viewport, whose scale sets the shift) loaded while a scissor box offset is
        // active must carry the offset too. It was forwarded only as the game wrote it, and the shifted
        // projection went out only when the offset itself changed. The attract scenes load their projection
        // before the offset and came out right; menus and the HUD load their 2D projection after it, so in the
        // bottom pass of a two-pass frame (offset 238) they drew as in the top pass: the top half of the
        // screen again in the bottom half (title, options, mode select, results; hardware 10:33).
        // R2_NO_PROJ_REFOLD=1 restores the old behaviour.
        {
            static const bool noRefold = envFlag("R2_NO_PROJ_REFOLD");
            const bool touches = addr <= 0x1026 && addr + cnt > 0x101A;
            if (!noRefold && touches && s_boxBiasSent && (s_boxOffX || s_boxOffY)) sendProjection();
        }
        return;
    }
    case 0x40:  // CALL_DL: inlined by the CP model
        return;
    default:
        fifo::write_data(p, len);
        return;
    }
}

#ifdef _WIN32
void sendToBack(void* hwnd) {
    constexpr unsigned SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10;
    SetWindowPos(hwnd, reinterpret_cast<void*>(1) /* HWND_BOTTOM */, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    // Out of the way entirely: left of every monitor and without a taskbar button (a tool
    // window), so a test run cannot be clicked, closed or minimized by someone working at the
    // machine. DWM still renders it, and PrintWindow still captures it. R2_ONSCREEN=1 keeps it
    // visible.
    if (envFlag("R2_ONSCREEN")) return;
    constexpr int GWL_EXSTYLE = -20, SM_XVIRTUALSCREEN = 76, SW_HIDE = 0, SW_SHOWNOACTIVATE = 4;
    constexpr long long WS_EX_TOOLWINDOW = 0x80, WS_EX_APPWINDOW = 0x40000;
    ShowWindow(hwnd, SW_HIDE);  // (the taskbar only drops the button of a hidden window)
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
    SetWindowPos(hwnd, reinterpret_cast<void*>(1), GetSystemMetrics(SM_XVIRTUALSCREEN) - 8000, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
#else
void sendToBack(void*) {  // (Windows only: test windows out of the way)
#endif
}

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const char* names[] = {"debug", "info", "warn", "error", "fatal"};
    static const bool debug = envFlag("R2_AURORA_DEBUG");
    if (level == LOG_DEBUG && !debug) return;
    logf("aurora", "%s %s: %.*s", names[level], module, int(len), message);
    if (level == LOG_FATAL) fatal("Aurora: %.*s", int(len), message);
}

// RL-NX: R2_GX_DUMP's parsing, moved out of init() so switchInit reads it too (on the console the
// option was silently inert, like R2_COPY_LOG was before).
void parseGxDump() {
    const char* d = getenv("R2_GX_DUMP");
    if (!d) return;
    char* end = nullptr;
    if (*d == 't') {
        end = const_cast<char*>(d);
        while (*end == 't') {
            s_dumpTimes.push_back(u64(strtod(end + 1, &end) * double(kTbClock)));
            if (*end == ';' || *end == '+') end++;
        }
        s_dumpAtTb = s_dumpTimes.front();
        s_dumpTimes.erase(s_dumpTimes.begin());
    } else {
        s_dumpFrame = strtoull(d, &end, 10);
    }
    if (end && *end == ',') s_dumpCount = strtoull(end + 1, nullptr, 10);
}

}  // namespace

bool init(int argc, char** argv, std::string& disc, const std::string& discNote) {
    AuroraConfig cfg{};
    cfg.appName = "Red Two";
    // Pipeline cache and other Aurora files: %APPDATA%\Red Two for the player. Automated runs
    // (R2_BACKGROUND, R2_HEADLESS or R2_INPUT) keep their own in build/test_cache, so debugging
    // overrides never leave pipelines in the player's cache (Aurora warms the whole cache at
    // startup: one bad entry crashed every start). R2_CACHE_DIR picks any directory.
    static std::string cacheDir;
    if (const char* dir = getenv("R2_CACHE_DIR")) cacheDir = dir;
    else if (envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT")) cacheDir = "build/test_cache/";
    else if (mods::portable()) cacheDir = mods::userDir();  // (else Aurora's own: %APPDATA%\Red Two, the same)
    if (!cacheDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        cfg.cachePath = cacheDir.c_str();
        cfg.userPath = cacheDir.c_str();  // controller/keyboard bindings (input.json) too
    }
    cfg.desiredBackend = BACKEND_AUTO;
    cfg.msaa = settings::get().msaa >= 4 ? 4 : 1;  // (WebGPU: 1 or 4 samples)
    cfg.vsync = false;  // the game paces itself on the emulated retrace
    s_testClear = envFlag("R2_GFX_TEST");
    parseGxDump();  // RL-NX: shared with switchInit
    s_xfb = !envFlag("R2_NO_XFB");
    s_copyLog = envFlag("R2_COPY_LOG");
    s_boundsOn = envFlag("R2_DRAW_BOUNDS");  // RL-NX: per draw, the EFB rectangle its geometry covers
    s_drawListOn = envFlag("R2_DRAW_LIST");  // RL-NX: the first draws of a frame, with their texture
    s_rawOn = envFlag("R2_DRAW_RAW");        // RL-NX: the vertex data behind an absurd placement
    s_statsOn = envFlag("R2_STATS");         // RL-NX: per-frame draw/bind outcome counters
    s_flatOn = envFlag("R2_TEX_FLAT");        // RL-NX: every texture as one flat 8x8 tile
    s_strictOn = envFlag("R2_STRICT");        // RL-NX: no merging, no identity heuristics
    cfg.emulateXfb = s_xfb;
    cfg.windowWidth = 1280;
    cfg.windowHeight = 960;
    // (Linux desktops match the window to red-two.desktop - the AppImage's - by this id)
    SDL_SetHint(SDL_HINT_APP_ID, "red-two");
    // smaller screens (the Steam Deck's 1280x800 desktop): a 4:3 window that fits its usable area
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.h > 0 && usable.h < 1000) {
            const int h = std::max(480, usable.h - 60), w = h * 4 / 3;
            cfg.windowWidth = u32(std::min(w, std::max(640, usable.w)));
            cfg.windowHeight = u32(h);
        }
    }
    if (const char* ws = getenv("R2_WINDOW")) {  // R2_WINDOW=<w>x<h> (screenshots)
        unsigned w = 0, h = 0;
        if (sscanf(ws, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240) {
            cfg.windowWidth = w;
            cfg.windowHeight = h;
        }
    }
    cfg.windowPosX = -1;
    cfg.windowPosY = -1;
    cfg.logCallback = logCallback;
    cfg.imGuiInitCallback = [](const AuroraWindowSize* size) { menuImGuiInit(size); };
    // Controllers keep working when the window is not focused (SDL drops their events
    // otherwise, and a window started from another program often opens unfocused). Not for
    // automated runs: someone's controller must not steer a scripted test (R2_BG_PADS=0/1
    // overrides).
    const char* bgPads = getenv("R2_BG_PADS");
    cfg.allowJoystickBackgroundEvents = bgPads ? *bgPads == '1'
                                               : !envFlag("R2_BACKGROUND") && !envFlag("R2_HEADLESS") && !getenv("R2_INPUT");
    cfg.logLevel = envFlag("R2_AURORA_DEBUG") ? LOG_DEBUG : LOG_INFO;
    {
        const char* t = getenv("R2_GX_THREAD");
        s_gxThread = !t || *t != '0';
    }
    cfg.gxProcessingMode = s_gxThread ? GX_PROCESSING_THREAD : GX_PROCESSING_INLINE;
    cfg.mem1Size = 0;
    cfg.mem2Size = 0;
    if (envFlag("R2_HEADLESS")) cfg.surface.type = AURORA_SURFACE_HEADLESS;
    cfg.startFullscreen = settings::get().fullscreen;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--fullscreen")) cfg.startFullscreen = settings::get().fullscreen = true;
    // R2_BACKGROUND: automated runs - the window opens behind everything without taking focus
    // (it still renders, and PrintWindow can capture it).
    const bool background = envFlag("R2_BACKGROUND");
    if (background) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    }
    // Controllers are updated on the input thread (input.cpp): SDL's device scans take seconds
    // on some machines, and the event pump would run them on the game's thread.
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
#ifdef R2_HAS_ICON
    // the icon built into the exe (res/red_two.rc) for the window and taskbar too
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
    // The ubershader mode before Aurora loads the pipeline cache: with them off, the uber
    // pipelines are neither derived from the cached shaders nor built from their stored rows
    // (patch 0028) - Aurora's own default is on, and applyDisplaySettings below comes too late.
    AuroraSetUberShaders(uberShaderMode());
    const AuroraInfo info = aurora_initialize(argc, argv, &cfg);
    if (background) {
        if (SDL_Window* w = aurora_get_window()) {
            if (void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                sendToBack(hwnd);
        }
    }
    applyDisplaySettings();  // (also the ubershader mode)
    mods::texturesInit();  // texture packs (Dolphin's format) into Aurora's replacement registry
    buttonIconsUpdate();   // Xbox button prompts (button_icons.cpp)
    static const char* backends[] = {"auto", "D3D11", "D3D12", "Metal", "Vulkan", "OpenGL", "OpenGL ES", "WebGPU", "null"};
    logf("gfx", "Aurora up: %s backend, window %ux%u", backends[info.backend], info.windowSize.width, info.windowSize.height);
    input::init(true);  // (before the launcher: the controllers drive it)
    // the launcher; also when there is no usable disc (it asks for one: setting off or not)
    launcherSetDisc(disc, discNote);
    if (disc.empty() || launcherWanted(argc, argv)) {
        if (disc.empty()) logf("disc", "none found: the launcher asks for it%s%s", discNote.empty() ? "" : ": ", discNote.c_str());
        runLauncher();
        disc = launcherDisc();
    }
    precompileShaders();
    s_enabled = true;
    gxSetSink(bridgeSink);  // RL-NX (9.53): through the bridge worker's ring
    gxSetSyncHook(gpSync);
    gxSetPeBarrierHook(peBarrier);  // RL-NX (9.53)
    bridgeStartWorker();
    return true;
}

// RL-NX: Aurora bring-up for the Switch. The same configuration as init() above with the desktop
// parts left out (no launcher, no ImGui, no texture packs, no window): the surface is HEADLESS for
// now, so Aurora renders into its own EFB/XFB targets with no swapchain. What that buys is the whole
// GX path - FIFO, textures, shaders, pipelines - running on the console, which is what the renderer
// step builds on. switch/deko replaces the headless surface with a deko3d one (§C.10).
bool switchInit() {
    AuroraConfig cfg{};
    cfg.appName = "RL-NX";
    // RL-NX: SD layout (2026-11-26) - the folder root holds only the NRO and the disc; Aurora's user files
    // (input.json, imgui.ini) live in config/ and its caches (dawn_cache.db, pipeline_cache.db) in cache/aurora/.
    cfg.userPath = "sdmc:/switch/RogueLeader-NX/config/";
    cfg.cachePath = "sdmc:/switch/RogueLeader-NX/cache/aurora/";
    cfg.desiredBackend = BACKEND_AUTO;
    cfg.msaa = 1;
    cfg.vsync = false;
    cfg.emulateXfb = true;  // the game presents through GXCopyDisp
    cfg.logCallback = logCallback;
    cfg.logLevel = envFlag("R2_AURORA_DEBUG") ? LOG_DEBUG : LOG_INFO;
    // RL-NX: GX processing on Aurora's own thread by default, as on PC. The hardware profile (build
    // 08:56) had Aurora's command processing at ~7.5 ms of the game thread's ~97 ms frame, with the
    // game thread at 100 % busy; the "tuned" placement puts the "Aurora FIFO processor" on core 0, off
    // the game's core 1 (switch/src/perf_switch.cpp). It costs a frame of latency. boot.cfg
    // R2_GX_THREAD=0 puts it back inline on the guest's thread.
    const char* gxThread = getenv("R2_GX_THREAD");
    cfg.gxProcessingMode = gxThread && *gxThread == '0' ? GX_PROCESSING_INLINE : GX_PROCESSING_THREAD;
    // RL-NX: gpSync must drain Aurora's thread at draw done / sync tokens in this mode (the PC entry sets
    // it the same way), or the game reuses vertex arrays and textures before that thread has read them.
    s_gxThread = cfg.gxProcessingMode == GX_PROCESSING_THREAD;
    logf("gfx", "GX processing: %s (boot.cfg R2_GX_THREAD=0/1)",
         cfg.gxProcessingMode == GX_PROCESSING_THREAD ? "Aurora's FIFO thread" : "inline on the game thread");
    cfg.mem1Size = 0;  // the port owns guest memory (src/runtime/memory_switch.cpp)
    cfg.mem2Size = 0;
    cfg.surface.type = AURORA_SURFACE_HEADLESS;
    AuroraSetUberShaders(uberShaderMode());
#ifdef AURORA_GFX_DEKO3D
    // RL-NX (AURORA_GFX_DEKO3D is set by switch/cmake/Deko3D.cmake): the deko3d renderer presents to
    // libnx's default window, so the display, the layer and the
    // window must exist before Aurora initialises (deko3d's swapchain is created from inside it).
    // switch/src/vsync_switch.cpp owns them and only libnx's own startup would have made them.
    rlWindowEnsure();
#endif
    const AuroraInfo info = aurora_initialize(0, nullptr, &cfg);
    logf("gfx", "Aurora up: backend %d, window %ux%u", int(info.backend), info.windowSize.width, info.windowSize.height);
    hdStart();  // RL-NX (Stage 12): HD TEXTURES
    s_xfb = true;
    s_enabled = true;
    // RL-NX: the diagnostic switches are read here too. This is the console's entry (main_switch.cpp
    // calls it, the desktop init() above is never reached on the Switch), and without these two lines
    // R2_COPY_LOG and R2_DRAW_BOUNDS were silently inert there - so an earlier reading that BP 0x59
    // "is never written" rested on a log line that could not have appeared.
    s_copyLog = envFlag("R2_COPY_LOG");
    s_boundsOn = envFlag("R2_DRAW_BOUNDS");
    s_drawListOn = envFlag("R2_DRAW_LIST");  // RL-NX: re-read here (boot.cfg is parsed before this)
    s_rawOn = envFlag("R2_DRAW_RAW");        // RL-NX: re-read here too (console boot.cfg)
    s_statsOn = envFlag("R2_STATS");         // RL-NX: re-read here too
    s_flatOn = envFlag("R2_TEX_FLAT");        // RL-NX: re-read here too
    s_strictOn = envFlag("R2_STRICT");        // RL-NX: re-read here too
    parseGxDump();  // RL-NX: R2_GX_DUMP on the console (frames go to logs/gx_frame_<n>.txt)
    input::init(true);  // Aurora's PAD (the desktop init() does this after aurora_initialize)
    gxSetSink(bridgeSink);  // RL-NX (9.53): through the bridge worker's ring
    gxSetSyncHook(gpSync);
    gxSetPeBarrierHook(peBarrier);  // RL-NX (9.53)
    bridgeStartWorker();
    return true;
}

// R2_RESIZE="<seconds>:<w>x<h>[,...]": resize the window at those times (tests of what a
// player's resizing does, e.g. with the aspect matching the window)
static void scriptedResize() {
    struct Resize {
        u64 tb;
        int w, h;
    };
    static std::vector<Resize> list = [] {
        std::vector<Resize> v;
        if (const char* e = getenv("R2_RESIZE")) {
            for (const char* p = e; *p;) {
                double s = 0;
                int w = 0, h = 0;
                if (sscanf(p, "%lf:%dx%d", &s, &w, &h) == 3 && w >= 160 && h >= 120)
                    v.push_back({u64(s * double(kTbClock)), w, h});
                const char* comma = strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        }
        return v;
    }();
    if (list.empty()) return;
    static const u64 start = tbNow();
    if (tbNow() - start < list.front().tb) return;
    if (SDL_Window* w = aurora_get_window()) {
        SDL_SetWindowSize(w, list.front().w, list.front().h);
        logf("gfx", "R2_RESIZE: window %dx%d", list.front().w, list.front().h);
    }
    list.erase(list.begin());
}

void vblank() {
    s_vblanks++;
    rt::vmCensusFrame();  // RL-NX (Stage 9): VM CENSUS, every 1800 vblanks while on
    if (!s_enabled) return;
    // RL-NX (9.53): hotkeys, input, window events and the menu's latched presses meet the bridge's state here,
    // so the worker finishes what is queued first; then a GX THREAD change from the menu takes effect.
    // RL-NX (audit 2026-11-27): ASYNC VBLANK skips the drain (the 3.6 ms a frame of gxsync on level 22) when nothing
    // here needs the worker idle: no menu switch pending (those change worker state), the menu closed. The input
    // work shares SDL/Aurora's pad state with the worker's pollEvents, so it takes the same lock instead; a hotkey
    // that fired, or the menu opening, drains once afterwards (they change what the worker reads).
    if (!s_avInit) {
        s_avInit = true;
        const char* e = getenv("R2_ASYNC_VBLANK");
        s_asyncVblank = !(e && *e == '0');  // RL-NX (audit 2026-11-27): default ON (hardware A/B: +1..4 ms a frame); R2_ASYNC_VBLANK=0 = off
    }
    bool pending = s_asyncWant.load(std::memory_order_relaxed) >= 0 || s_gpWant.load(std::memory_order_relaxed) >= 0 ||
                   s_gpRawWant.load(std::memory_order_relaxed) >= 0 || s_cpLagWant.load(std::memory_order_relaxed) >= 0 ||
                   s_avWant.load(std::memory_order_relaxed) >= 0;
#if defined(__SWITCH__)
    pending = pending || s_hdWant.load(std::memory_order_relaxed) >= 0 || s_hdLimitWant.load(std::memory_order_relaxed) >= 0;
#endif
    const bool lite = s_asyncVblank && s_workerUp && s_async.load(std::memory_order_relaxed) && !pending && !menuOpen();
    if (!lite) bridgeDrain(RLPM_DRAIN_VBLANK);
    bridgeApplyWanted();
    scriptedResize();
    {
        std::lock_guard<std::recursive_mutex> inputLock(s_inputMx);
        const unsigned fired = hotkeys::fireCount();
        hotkeys::tick();  // (before the game's input: a hotkey's buttons are muted for it)
        input::poll();
        if (lite && (hotkeys::fireCount() != fired || menuOpen())) bridgeDrain(RLPM_DRAIN_VBLANK);
    }
    // Keep the window responsive (and closable) while the game is not presenting: loading
    // screens, or a frame that never ends. Polling only reads SDL events, so it is safe mid-frame.
    if (tbNow() - s_lastPollTb > kTbClock / 10) pollEvents();
}

u64 frames() { return s_frames; }

// RL-NX: the running total the stall watchdog in switch/src/host_switch.cpp watches. Read from
// Aurora's own counter, so it moves exactly when the guest submits, not when the port presents.
u64 drawCalls() {
    const AuroraStats* st = aurora_get_stats();
    return st ? st->drawCallCount : 0;
}

// RL-NX (9.53): the - menu's GX THREAD row (gx_bridge.h). Runs on whichever thread draws the menu, so it only
// asks; the game thread switches at its next vblank (bridgeApplyWanted).
void setBridgeThread(bool on) { s_asyncWant.store(on ? 1 : 0, std::memory_order_relaxed); }
// RL-NX (9.57): the - menu's GP THREAD row, applied at the next vblank as GX THREAD is
void setGpThread(bool on) { s_gpWant.store(on ? 1 : 0, std::memory_order_relaxed); }
// RL-NX (Stage 6): the - menu's GP RAW row, applied at the next vblank as GP THREAD is
void setGpRaw(bool on) { s_gpRawWant.store(on ? 1 : 0, std::memory_order_relaxed); }
bool gpRawOn() {
    const int want = s_gpRawWant.load(std::memory_order_relaxed);
    return want >= 0 ? want != 0 : gxGpRaw();
}
#if defined(__SWITCH__)
// RL-NX (Stage 12): the - menu's HD TEXTURES and HD TEX 2048 rows, applied at the next vblank
void setHdTextures(bool on) { s_hdWant.store(on ? 1 : 0, std::memory_order_relaxed); }
bool hdTexturesOn() {
    const int want = s_hdWant.load(std::memory_order_relaxed);
    return want >= 0 ? want != 0 : s_hdOn;
}
void setHdLimit(bool on) { s_hdLimitWant.store(on ? 1 : 0, std::memory_order_relaxed); }
bool hdLimitOn() {
    const int want = s_hdLimitWant.load(std::memory_order_relaxed);
    return want >= 0 ? want != 0 : s_hdLimit;
}
#endif
// RL-NX (audit 2026-11-27): the - menu's ASYNC VBLANK row, applied at the next vblank
void setAsyncVblank(bool on) { s_avWant.store(on ? 1 : 0, std::memory_order_relaxed); }
bool asyncVblankOn() {
    const int want = s_avWant.load(std::memory_order_relaxed);
    if (want >= 0) return want != 0;
    if (!s_avInit) {
        const char* e = getenv("R2_ASYNC_VBLANK");
        return !(e && *e == '0');  // default on
    }
    return s_asyncVblank;
}
// RL-NX (Stage 11): the - menu's CP LAG row, applied at the next vblank as GP RAW is
void setCpLag(bool on) { s_cpLagWant.store(on ? 1 : 0, std::memory_order_relaxed); }
bool cpLagOn() {
    const int want = s_cpLagWant.load(std::memory_order_relaxed);
    return want >= 0 ? want != 0 : gxCpLag();
}
bool gpThreadOn() {
    const int want = s_gpWant.load(std::memory_order_relaxed);
    return want >= 0 ? want != 0 : gxGpAsync();
}
bool bridgeThreadOn() {
    const int want = s_asyncWant.load(std::memory_order_relaxed);
    return want >= 0 ? want != 0 : s_async.load(std::memory_order_relaxed);
}

// RL-NX: the - menu's stats switch (gx_bridge.h)
void setStatsEnabled(bool on) {
    s_statsMenu = on;
    // RL-NX: the - menu's switch wins over R2_STATS once used. `on || R2_STATS` made the row unable to turn the
    // lines off on a card whose boot.cfg sets R2_STATS=1; the menu starts its row from R2_STATS instead.
    s_statsOn = on;
}

std::string stats() {
    bridgeDrain();  // RL-NX (9.53): resets counters the worker adds to
    const AuroraStats* st = aurora_get_stats();
    if (!st) return {};
    char b[320];
    snprintf(b, sizeof b,
             "aurora: %u draws (%u merged), pipelines %u queued %u created, %u skipped %u uber draws, %u KiB verts, "
             "%u KiB tex upload; arrays sent: %llu (%llu MiB)",
             st->drawCallCount, st->mergedDrawCallCount, st->queuedPipelines, st->createdPipelines, st->skippedDraws,
             st->uberDraws, st->lastVertSize / 1024,
             st->lastTextureUploadSize / 1024, (unsigned long long)s_arraySends, (unsigned long long)(s_arrayBytes >> 20));
    s_arraySends = s_arrayBytes = 0;
    std::string out = b;
    if (mods::loadedTextureCount()) out += "; " + mods::textureStats();
    return out;
}

}  // namespace rt::gfx

// --- save states ----------------------------------------------------------------------------
// The register state the stream built (BP, CP, XF as loaded, palette loads) is part of a state.
// After a load it goes to Aurora again through onCommand, as the game's own writes would
// (actions left out: EFB copies, palette loads, draw done and tokens), so Aurora's state, which
// belongs to the session's own timeline, matches the restored game. Textures and arrays are
// sent again at their next use; one frame may mix the two timelines.
namespace rt::gfx {
namespace {
struct TlutRec {
    u32 tmem, addr, entries;
};
bool replayedBp(u32 reg) {
    switch (reg) {
    case 0x45: case 0x47: case 0x48:  // draw done, tokens
    case 0x52:                        // EFB copy
    case 0x64: case 0x65: case 0x66:  // palette load, texture cache invalidation
    case 0x67:                        // performance counters
    case kScreenSpaceMarker:
    case 0xFE:                        // mask
        return false;
    default:
        return true;
    }
}
}  // namespace

void gpuDrain() { gpSync(); }

void bridgeSaveState(state::Writer& w) {
    bridgeDrain();  // RL-NX (9.53): the worker owns this state between drains
    w.section("GFX ");
    w.pod(s_bp);
    w.pod(s_bpValid);
    w.pod(s_bpMask);
    w.pod(s_cp);
    w.pod(s_cpValid);
    w.pod(s_xfMem);
    w.pod(s_xfBlock);
    std::vector<TlutRec> tluts;
    for (const auto& [tmem, ld] : s_tlutLoads) tluts.push_back({tmem, ld.addr, ld.entries});
    w.vec(tluts);
    w.pod(s_screenSpaceWindow);
    w.pod(s_fillWindow);
}

void bridgeLoadState(state::Reader& r) {
    bridgeDrain();  // RL-NX (9.53): the worker owns this state between drains
    r.section("GFX ");
    r.pod(s_bp);
    r.pod(s_bpValid);
    r.pod(s_bpMask);
    r.pod(s_cp);
    r.pod(s_cpValid);
    r.pod(s_xfMem);
    r.pod(s_xfBlock);
    std::vector<TlutRec> tluts;
    r.vec(tluts);
    s_tlutLoads.clear();
    for (const TlutRec& t : tluts) s_tlutLoads[t.tmem] = {t.addr, t.entries};
    r.pod(s_screenSpaceWindow);
    r.pod(s_fillWindow);
}

void bridgeAfterLoad() {
    bridgeDrain();  // RL-NX (9.53): the worker owns this state between drains
    if (!s_enabled) return;
    const u32 mask = s_bpMask;
    std::vector<u8> cmd;
    auto put32 = [&](u32 v) {
        for (int s = 24; s >= 0; s -= 8) cmd.push_back(u8(v >> s));
    };
    for (u32 reg = 0; reg < 256; reg++) {  // CP (array bases included: the bridge keeps those)
        if (!s_cpValid[reg]) continue;
        cmd = {0x08, u8(reg)};
        put32(s_cp[reg]);
        onCommand(cmd.data(), u32(cmd.size()));
    }
    for (u32 a = 0; a < kXfShadow; a++) {  // XF, in the blocks the game loaded
        const u32 n = std::min<u32>(s_xfBlock[a], kXfShadow - a);
        if (!n) continue;
        cmd = {0x10};
        put32(((n - 1) << 16) | a);
        for (u32 i = 0; i < n; i++) put32(s_xfMem[a + i]);
        onCommand(cmd.data(), u32(cmd.size()));
    }
    for (u32 reg = 0; reg < 256; reg++) {  // BP
        if (!s_bpValid[reg] || !replayedBp(reg)) continue;
        cmd = {0x61};
        put32((reg << 24) | (s_bp[reg] & 0xFFFFFF));
        onCommand(cmd.data(), u32(cmd.size()));
    }
    sendScissor();
    sendProjection();
    if (mask != 0xFFFFFF) {  // the game's pending BP mask (its next write takes it)
        s_bpMask = mask;
        fifo::write_u8(0x61);
        fifo::write_u32(0xFE000000u | mask);
    }
    for (Array& a : s_arrays) a.sentSize = 0;
    for (TexMap& t : s_tex) t.dirty = true;
    for (TlutSlot& t : s_tlutSlots) t = TlutSlot{};
    for (auto& [key, t] : s_texInfo) {
        t.frame = ~0ull;
        t.hashTb = 0;
    }
    setMenuTiling(false);
    logf("state", "renderer state replayed");
}

}  // namespace rt::gfx
