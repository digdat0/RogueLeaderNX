// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWakerHD (https://github.com/centollOS/SwitchWakerHD) at df8fbde:
// runtime/src/gfx/deko/backend.cpp (device, queue, swapchain, present, frame fences, debug callback,
// queue checks, the set-up cost and the statistics).
// The deko3d device, the frame's command recording and the present pass (dk.h, dk_aurora.h).
// Aurora's render worker opens a frame at Aurora's frame
// start (frame_open: the slot's fence, the descriptor sets), records the frame's passes into it
// (switch/deko/aurora), submits along the way, and presents: the swapchain image cleared, the game's
// picture (the picture callback: Aurora's EFB resampled and fitted) or the test pattern with
// COS_DK_TEST_PATTERN=1, the fence, the submit and the present. One render thread records and submits.
#include <switch.h>
#include <unistd.h>

#include "dk.h"
#include "dk_aurora.h"
#include "../../src/runtime/perfmon.h"  // RL-NX: PERF LOG totals (rlnx_dk_totals)

// The - menu's settings file (switch_menu.cpp); weak so renderer-only builds link without the game.
extern "C" int sfaSwitchMenuIniValue(const char* key, int def) __attribute__((weak));

#include <atomic>  // RL-NX: overlay_fps_set
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

// the renderer's own shaders, compiled by uam at build time (switch/cmake/Deko3D.cmake)
#include "text_fsh_dksh.h"
#include "text_vsh_dksh.h"

extern "C" char* fake_heap_end;
// The heap allocator's statistics (switch/src/malloc_switch.c, dlmalloc 2.8.6): call counts since start,
// heap sizes, and the most frequent sampled allocation sites since the last call (.elf addresses).
struct SfaHeapStats {
    uint64_t mallocCalls, freeCalls;
    size_t footprint, inUse, freeInside, freeChunks, topFree;
    uintptr_t topCaller[8];
    uint32_t topCount[8];
    uint32_t sampleEvery;
    // leak finder (malloc_switch.c): callers whose live memory grew most since the first report
    uintptr_t growCaller[8];
    int64_t growBytes[8];
    uint64_t trackedLive;
    uint32_t trackDropped;
};
extern "C" void sfa_heap_stats(SfaHeapStats* out);
extern "C" int sfa_log_extra_on;  // switch/src/log_switch.cpp: the - menu's Extra logging
// the game's host allocation scope (JKRHeap.cpp, native/src/pc/game_hooks/pc_jkr_heap.cpp)
void JKRPcBeginHostAlloc();              // libnx: the end of the heap malloc grows into (sbrk)
extern "C" void cos_switch_flush_logs(void);  // backend_present.cpp
extern "C" void kartpad_log_flush_sync(void);
extern "C" void kartpad_deko_log_failure_context();
extern "C" unsigned kartpad_gpu_pending_entries(void);      // host/gpu_submit_diagnostics.cpp
extern "C" unsigned kartpad_gpu_max_kickoff_entries(void);  // (and resets it)

namespace swdk {

Renderer R;
Conventions C;

namespace {

constexpr uint32_t kSwapImages = 3;
#ifdef COS_DK_DEBUG_LIB
constexpr bool kDebugLib = true;  // linked against libdeko3dd (COS_DK_DEBUG_LIB, KARTPAD_DEKO_DEBUG in switch/cmake/Deko3D.cmake)
#else
constexpr bool kDebugLib = false;
#endif

DkSwapchain g_swapchain = nullptr;
DkImage g_swapImages[kSwapImages];
ImageAlloc g_swapMem[kSwapImages];
DkImage g_depth;  // the test pattern's depth buffer (the game renders to its own targets)
ImageAlloc g_depthMem;
DkShader g_shaders[kShaderCount];
bool g_shaderOk[kShaderCount] = {};
int g_debugMessages = 0;
bool g_testPattern = false;
size_t g_heapAfterSetup = 0;

// the heap malloc has never grown into, in MiB (libnx's heap end against malloc's break): deko3d's
// memory blocks come from it, so the difference across the set-up is its cost (SwitchWakerHD core.cpp)
size_t heap_never_used_mib() {
    char* top = static_cast<char*>(sbrk(0));
    if (!fake_heap_end || !top || top == reinterpret_cast<char*>(-1)) return 0;
    return size_t(fake_heap_end - top) >> 20;
}

// deko3d's messages. Only the debug library (libdeko3dd, COS_DK_DEBUG_LIB=ON) calls this: its checks
// of every call, warnings (result DkResult_Success) and errors, after which it traps. The release
// library calls nothing on an error: it aborts with result 2359-xxxx.
void debug_message(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success) {
        kartpad_deko_log_failure_context();
        fatal("deko3d error in %s: result %d: %s (frame %llu)", context ? context : "?", int(result),
              message ? message : "", (unsigned long long)R.frame + 1);
    }
    const int n = g_debugMessages++;
    if (n < 200) {
        dklog("deko3d %s: %s", context ? context : "?", message ? message : "");
    } else if (n == 200) {
        dklog("deko3d: more messages, not logged");
    }
}

// Loads the renderer's own text shaders (boot screen, test pattern) from DKSH embedded at build time.
void load_builtin_shaders() {
    static const struct {
        const uint8_t* data;
        size_t size;
        const char* name;
    } kEmbedded[kShaderCount] = {
        {text_vsh_dksh, text_vsh_dksh_size, "text_vsh"},
        {text_fsh_dksh, text_fsh_dksh_size, "text_fsh"},
    };
    size_t bytes = 0;
    int ok = 0;
    for (int i = 0; i < kShaderCount; i++) {
        g_shaderOk[i] = code_load(g_shaders[i], kEmbedded[i].data, uint32_t(kEmbedded[i].size), kEmbedded[i].name);
        ok += g_shaderOk[i];
        bytes += kEmbedded[i].size;
    }
    dklog("embedded shaders (text): %d of %d loaded (%zu bytes of DKSH)%s", ok, int(kShaderCount), bytes,
          ok == kShaderCount ? "" : ": the startup cover cannot be drawn");
}

// Allocates and initialises a window-sized image (swapchain images, the test pattern's depth buffer).
void init_image(DkImage& image, ImageAlloc& mem, DkImageFormat format, uint32_t flags, const char* what) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.flags = flags;
    m.format = format;
    m.dimensions[0] = R.width;
    m.dimensions[1] = R.height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    mem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&image, &layout, mem.block, mem.offset);
    dklog("%s: %ux%u, %u KiB at image heap offset 0x%X", what, R.width, R.height, mem.size >> 10, mem.offset);
}

// ---- the window (the swapchain images): 1280x720 handheld, 1920x1080 docked (the TV's output), the
// swapchain recreated when the operation mode changes; the present pass fits the game's picture into it
// (backend_present.cpp scales by R.width/R.height). SwitchWakerHD backend.cpp window_wanted.
// COS_DK_DOCKED_1080=0: 1280x720 always (the system scales it to the TV, the GL NRO's way).
// COS_DK_WINDOW=WxH (320x180 to 1920x1080): that size in both modes, for A/B tests in handheld.
struct WindowConfig {
    int mode = 1;  // 0 always 1280x720, 1 by the operation mode, 2 forced
    uint32_t w = 0, h = 0;
};
const WindowConfig& window_config() {
    static const WindowConfig c = [] {
        WindowConfig c;
        if (const char* e = getenv("COS_DK_WINDOW"); e && *e) {
            unsigned w = 0, h = 0;
            if (sscanf(e, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 1920 && h <= 1080) {
                c.mode = 2;
                c.w = w;
                c.h = h;
            } else {
                dklog("COS_DK_WINDOW=%s ignored: WxH from 320x180 to 1920x1080", e);
            }
        }
        // KartPad's existing SDL/NVK output is 720p in both modes. Preserve that
        // workload; a larger docked swapchain is an explicit diagnostic opt-in.
        if (c.mode != 2 && !env_flag("COS_DK_DOCKED_1080", false)) c.mode = 0;
        if (c.mode == 2) {
            dklog("window: %ux%u in both modes (COS_DK_WINDOW)", c.w, c.h);
        } else if (c.mode == 1) {
            dklog("window: docked 1920x1080, handheld 1280x720, the swapchain recreated when the mode changes "
                  "(COS_DK_DOCKED_1080=0: 1280x720 always)");
        } else {
            dklog("window: 1280x720 in both modes (COS_DK_DOCKED_1080=0; docked the system scales it)");
        }
        return c;
    }();
    return c;
}
int g_opMode = -1;  // the operation mode the window was last sized for: 1 docked, 0 handheld, -1 not yet
uint64_t g_windowResizes = 0;

// The - menu's Docked Resolution (switch_menu.ini docked_res): 0 720p (the system scales it to the TV),
// 1 900p, 2 1080p. Above 720p the docked swapchain is 1920x1080 and Aurora renders at the chosen size
// (aurora_switch_frame_size at the end of the file). -1 until first read; the menu changes it through
// aurora_switch_set_docked_res. Atomic: read on the main thread (Aurora's window size) and on the
// presenting thread (window_wanted).
std::atomic<int> g_dockedRes{-1};
int docked_res() {
    int v = g_dockedRes.load(std::memory_order_relaxed);
    if (v < 0) {
        v = sfaSwitchMenuIniValue ? sfaSwitchMenuIniValue("docked_res", 0) : 0;
        v = v < 0 ? 0 : v > 2 ? 2 : v;
        g_dockedRes.store(v, std::memory_order_relaxed);
    }
    return v;
}

// the window size for the current operation mode (libnx keeps appletGetOperationMode up to date from
// AppletMessage_OperationModeChanged in the main thread's appletMainLoop)
void window_wanted(uint32_t& w, uint32_t& h) {
    const WindowConfig& c = window_config();
    const int docked = appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0;
    if (docked != g_opMode) {
        dklog("operation mode: %s", docked ? "docked (TV)" : "handheld");
        g_opMode = docked;
    }
    if (c.mode == 2) {
        w = c.w;
        h = c.h;
        return;
    }
    // 1080p output when docked with COS_DK_DOCKED_1080, or with a Docked Resolution above 720p.
    const bool big = docked == 1 && (c.mode == 1 || docked_res() > 0);
    w = big ? 1920 : 1280;
    h = big ? 1080 : 720;
}

// Creates kSwapImages RGBA8 images of the current window size and a swapchain on libnx's default window
// (vsync_switch.cpp creates that window). Swap interval 1: one image per display refresh.
void init_swapchain() {
    // RL-NX: dkSwapchainCreate's first act is nwindowSetDimensions, and the release library aborts
    // inside the call if it fails, so the failure is found here instead - one probe with the same call,
    // before an image is allocated. Ryujinx's window binder answers 0xF59 for it (and for
    // nwindowConfigureBuffer) while its Nv buffer maps succeed, so its swapchain cannot be used; a
    // console whose window is fine passes this and nothing changes.
    {
        NWindow* nwin = nwindowGetDefault();
        const Result probe = nwindowSetDimensions(nwin, R.width, R.height);
        // RL-NX: MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized) is the expected answer once the
        // window has dimensions - it adopts them when it is created from the layer - and deko3d's own
        // call gets the same (patch 5 there keeps it non-fatal). Only any *other* failure means the
        // window cannot take buffers at all.
        const bool alreadySet = R_MODULE(probe) == Module_Libnx &&
                                R_DESCRIPTION(probe) == LibnxError_AlreadyInitialized;
        // RL-NX: the decisive test. libnx's window keeps its configured buffer slots in
        // NWindow::slots_configured, and nwindowConfigureBuffer refuses a slot that is already taken with
        // MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized) - which is what deko3d's swapchain does
        // first, before Dereferencing anything. A window that already has buffers (an emulator's managed
        // layer pre-configures two of them and the app does not own the producer) therefore cannot take a
        // swapchain, and the release library aborts *inside* dkSwapchainCreate instead of returning null.
        dklog("window: %ux%u, slots_configured 0x%llx, slots_requested 0x%llx, app-controlled producer %d, "
              "set-dimensions rc=0x%x",
              nwin->width, nwin->height, (unsigned long long)nwin->slots_configured,
              (unsigned long long)nwin->slots_requested, int(nwin->producer_controlled_by_app), (unsigned)probe);
        if (nwin->slots_configured != 0) {
            // RL-NX: something else holds the window's buffer slots - most often libnx's console
            // framebuffer, which consoleInit configures on the same default window. A window whose slots
            // are taken cannot take deko3d's swapchain, so on a console the renderer ran and presented
            // nothing: a black screen. Hand the slots back and take them again. Only attempted when the
            // window itself answered the probe above (a working window); an emulator whose window binder
            // refuses everything takes the quiet path below instead of aborting inside the swapchain.
            if (!R_FAILED(probe) || alreadySet) {
                const u64 had = nwin->slots_configured;
                const Result rel = nwindowReleaseBuffers(nwin);
                dklog("swapchain: the window had buffers configured (0x%llx): released them (rc=0x%x), now 0x%llx",
                      (unsigned long long)had, (unsigned)rel, (unsigned long long)nwin->slots_configured);
            }
            if (nwin->slots_configured != 0) {
                dklog("swapchain: the window already has buffers configured (0x%llx): running without presenting",
                      (unsigned long long)nwin->slots_configured);
                return;
            }
        }
        if (R_FAILED(probe) && !alreadySet) {
            dklog("swapchain: the window cannot take buffers (nwindowSetDimensions %ux%u rc=0x%x): "
                  "running without presenting",
                  R.width, R.height, (unsigned)probe);
            return;
        }
    }
    const DkImage* images[kSwapImages];
    for (uint32_t i = 0; i < kSwapImages; i++) {
        char what[48];
        snprintf(what, sizeof what, "swapchain image %u (RGBA8)", i);
        init_image(g_swapImages[i], g_swapMem[i], DkImageFormat_RGBA8_Unorm,
                   DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression, what);
        images[i] = &g_swapImages[i];
    }
    init_image(g_depth, g_depthMem, DkImageFormat_Z24S8, DkImageFlags_UsageRender | DkImageFlags_HwCompression,
               "test pattern depth buffer (Z24S8)");
    DkSwapchainMaker m;
    dkSwapchainMakerDefaults(&m, R.device, nwindowGetDefault(), images, kSwapImages);
    dklog("creating the swapchain on the default window");
    log_flush();
    g_swapchain = dkSwapchainCreate(&m);
    // RL-NX: a console (or an emulator) whose window cannot take the buffers must not take the game with
    // it. libdeko3dd returns null here (the release library aborts inside the call), so the renderer keeps
    // its device and drawing and simply presents nothing - the log says why, and everything else runs.
    if (!g_swapchain) {
        dklog("swapchain: CREATION FAILED - the renderer runs without presenting (see the nwindow errors "
              "above); the game, audio and GX are unaffected");
        return;
    }
    dkSwapchainSetSwapInterval(g_swapchain, 1);
    u32 nw = 0, nh = 0;
    nwindowGetDimensions(nwindowGetDefault(), &nw, &nh);
    dklog("swapchain: %u RGBA8 images of %ux%u on the default window (%ux%u), swap interval 1", kSwapImages,
          R.width, R.height, nw, nh);
}

// a new window size (the operation mode changed): the GPU finishes what was submitted (the previous
// presents into the old images), the old swapchain gives the window's buffers back, and images and a
// swapchain of the new size take their place (deko3d sets the window's dimensions from them). The old
// images' memory is freed once the GPU is done with this frame.
void resize_window(uint32_t w, uint32_t h, uint64_t frame) {
    const uint64_t t0 = now_ns();
    const uint32_t ow = R.width, oh = R.height;
    check_queue("recreating the swapchain");
    dkQueueWaitIdle(R.queue);
    const uint64_t t1 = now_ns();
    dkSwapchainDestroy(g_swapchain);
    g_swapchain = nullptr;
    for (uint32_t i = 0; i < kSwapImages; i++) image_free_later(g_swapMem[i]);
    image_free_later(g_depthMem);
    R.width = w;
    R.height = h;
    init_swapchain();
    g_windowResizes++;
    dklog("frame %llu: window %ux%u -> %ux%u: swapchain recreated in %.1f ms (GPU idle wait %.1f ms)",
          (unsigned long long)frame, ow, oh, w, h, double(now_ns() - t0) / 1e6, double(t1 - t0) / 1e6);
}

// ---- statistics every 30 s: presents and where their time went, submits, command and stream memory per
// frame, the image heap, blocks, shader code, descriptors, the heap
struct Times {
    uint64_t presents = 0, fenceNs = 0, acquireNs = 0, recordNs = 0, submitNs = 0, maxNs = 0;
    uint64_t start = 0;
} g_times;
RlPmDkTotals g_pmDk;  // RL-NX: PERF LOG - the same times as g_times, never reset (rlnx_dk_totals)
SubmitStats g_submits;
uint32_t g_entriesInList = 0;  // GPFIFO entries the command list being recorded ends (an estimate)
uint64_t g_submitsAtStats = 0;

// Accumulates per-present times and logs the 30-second summary lines.
void frame_stats(uint64_t presentNs) {
    g_times.presents++;
    g_times.maxNs = presentNs > g_times.maxNs ? presentNs : g_times.maxNs;
    const uint64_t now = now_ns();
    if (g_times.start == 0) g_times.start = now;
    if (now - g_times.start < 30'000'000'000ull) return;
    const double n = double(g_times.presents);
    const MemoryStats m = memory_stats_take();
    const DescriptorStats d = descriptor_stats();
    // freeInside: freed memory malloc holds below its break (with heap_never_used_mib, what is still
    // allocatable); freeChunks: the free chunks it is in, whose growth over a session is fragmentation
    SfaHeapStats heap;
    sfa_heap_stats(&heap);
    dklog("frames %llu-%llu: %.1f presents/s; per present: frame fence %.2f ms, acquire %.2f ms, record %.2f ms, "
          "submit+present %.2f ms, longest %.2f ms, %.1f submits; command memory max %llu KiB/frame (%llu "
          "overflows), stream max %llu KiB/frame (%llu full); image heap %llu KiB in %llu chunks, blocks %llu KiB, "
          "shader code %llu KiB; descriptors: %u images, %u samplers in use, written %llu images, %llu samplers "
          "(%llu sampler evictions); window %ux%u (%llu resizes); deko3d messages %d; heap never "
          "used %zu MiB, free inside %zu MiB in %zu chunks",
          (unsigned long long)(R.frame - g_times.presents + 1), (unsigned long long)R.frame,
          n * 1e9 / double(now - g_times.start), double(g_times.fenceNs) / n / 1e6, double(g_times.acquireNs) / n / 1e6,
          double(g_times.recordNs) / n / 1e6, double(g_times.submitNs) / n / 1e6, double(g_times.maxNs) / 1e6,
          double(g_submits.submits - g_submitsAtStats) / n, (unsigned long long)(m.cmdBytesMax >> 10),
          (unsigned long long)m.cmdOverflows, (unsigned long long)(m.streamBytesMax >> 10),
          (unsigned long long)m.streamFull, (unsigned long long)(m.imageBytes >> 10),
          (unsigned long long)m.imageChunks, (unsigned long long)(m.blockBytes >> 10),
          (unsigned long long)(m.codeBytes >> 10), d.imagesUsed, d.samplersUsed, (unsigned long long)d.imageWrites,
          (unsigned long long)d.samplerWrites, (unsigned long long)d.samplerEvictions,
          R.width, R.height, (unsigned long long)g_windowResizes, g_debugMessages, heap_never_used_mib(),
          heap.freeInside >> 20, heap.freeChunks);
    // The allocator line: calls per present (each one takes the heap lock, so the GX, render and game
    // threads queue on each other's), sizes, and the top allocation sites. Resolve the sites with
    // aarch64-none-elf-addr2line -f -C -e debug/SFA-NX-HHMM.elf <address> (the build's own .elf).
    static uint64_t mallocsAtStats = 0, freesAtStats = 0;
    char sites[8 * 32] = "";
    size_t used = 0;
    for (int k = 0; k < 8 && heap.topCount[k] != 0 && used < sizeof(sites); ++k) {
        // each sample stands for sampleEvery allocations: print the estimated allocations per present
        used += size_t(snprintf(sites + used, sizeof(sites) - used, " 0x%llx:%.1f",
                                (unsigned long long)heap.topCaller[k],
                                double(heap.topCount[k]) * heap.sampleEvery / n));
    }
    dklog("heap (dlmalloc): %.0f mallocs, %.0f frees per present; in use %zu MiB, footprint %zu MiB, free "
          "inside %zu KiB in %zu chunks (top %zu KiB); allocation sites per present:%s",
          double(heap.mallocCalls - mallocsAtStats) / n, double(heap.freeCalls - freesAtStats) / n,
          heap.inUse >> 20, heap.footprint >> 20, heap.freeInside >> 10, heap.freeChunks, heap.topFree >> 10,
          sites);
    // Leak finder: callers ranked by how much more of their memory is live than at the first report
    // (estimates from 1-in-64 samples). A caller that climbs report after report is the leak. It samples
    // only with the - menu's Extra logging on (malloc_switch.c), so the line is printed only then.
    if (__atomic_load_n(&sfa_log_extra_on, __ATOMIC_RELAXED)) {
        char grow[8 * 40] = "";
        used = 0;
        for (int k = 0; k < 8 && heap.growBytes[k] > 0 && used < sizeof(grow); ++k) {
            used += size_t(snprintf(grow + used, sizeof(grow) - used, " 0x%llx:+%lldK",
                                    (unsigned long long)heap.growCaller[k], (long long)(heap.growBytes[k] >> 10)));
        }
        dklog("heap live: %llu MiB sampled live (%u samples dropped); growth since first report by caller:%s",
              (unsigned long long)(heap.trackedLive >> 20), heap.trackDropped, grow);
    }
    mallocsAtStats = heap.mallocCalls;
    freesAtStats = heap.freeCalls;
    static uint64_t entryFlushesAtStats = 0;
    static uint64_t fenceBatchesAtStats = 0;
    dklog("GPFIFO: at most %u entries per kickoff, %llu early flushes (128 entries pending), %llu list splits "
          "(128 entries recorded)",
          kartpad_gpu_max_kickoff_entries(), (unsigned long long)(g_submits.entryFlushes - entryFlushesAtStats),
          (unsigned long long)(g_submits.fenceBatches - fenceBatchesAtStats));
    entryFlushesAtStats = g_submits.entryFlushes;
    fenceBatchesAtStats = g_submits.fenceBatches;
    g_submitsAtStats = g_submits.submits;
    g_times = Times{};
    g_times.start = now;
}

// a 1x1 black texture in kEmptyImage: what a draw samples where its view is missing (the first frame)
bool g_emptyReady = false;
DkImage g_empty;
ImageAlloc g_emptyMem;
// Created on the first frame (it needs the frame's command buffer for its upload).
void empty_image_init() {
    if (g_emptyReady) return;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = 1;
    m.dimensions[1] = 1;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    StreamAlloc s = stream_alloc(4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    if (!s) return;
    g_emptyMem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&g_empty, &layout, g_emptyMem.block, g_emptyMem.offset);
    memset(s.cpu, 0, 4);
    static_cast<uint8_t*>(s.cpu)[3] = 0xFF;
    DkImageView view;
    dkImageViewDefaults(&view, &g_empty);
    const DkCopyBuf src = {s.gpu, 0, 0};
    const DkImageRect r = {0, 0, 0, 1, 1, 1};
    dkCmdBufCopyBufferToImage(R.cmd, &src, &view, &r, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    write_image_descriptor(kEmptyImage, g_empty);
    g_emptyReady = true;
}

uint64_t g_frameOpenNs = 0;

// the commands recorded so far to the GPU (the fences they signal are written under fence_mutex)
void submit_list(const char* why) {
    check_queue("a submit");
    diag_split(why);
    const DkCmdList list = dkCmdBufFinishList(R.cmd);
    std::lock_guard<std::mutex> lock(fence_mutex());
    dkQueueSubmitCommands(R.queue, list);
    fence_submitted();
    g_submits.submits++;
    g_entriesInList = 0;
    // deko3d kicks its GPFIFO entries off only when the queue flushes.
    // RL-NX: kick off after *every* submit. The old policy (flush once 128 entries are pending) never
    // fired in practice: the channel counted a few entries per frame, so the ring filled slowly and
    // deko3d's Queue::appendGpfifoEntries raised after ~28 s of play. A flush per submit costs one
    // syscall (there are one or two submits a frame) and keeps the ring empty.
    dkQueueFlush(R.queue);
    g_submits.entryFlushes++;
}

}  // namespace

// Starts a frame's recording: waits for the frame slot's fence (the GPU finished the frame that last used
// this slot), resets descriptors and per-frame memory, and makes shader code visible to the GPU.
void frame_open() {
    if (R.frameOpen) return;
    const uint64_t frame = R.frame + 1;
    const uint64_t t0 = now_ns();
    check_queue("the frame fence");
    frame_begin(frame);
    g_times.fenceNs += now_ns() - t0;
    g_pmDk.fenceNs += now_ns() - t0;  // RL-NX: PERF LOG
    g_frameOpenNs = now_ns();
    descriptors_frame_start();
    empty_image_init();
    if (g_testPattern) pattern_init();
    shaders_frame_start();
    diag_frame_begin();
    code_make_visible();
    R.frameOpen = true;
}

// Sends everything recorded so far to the GPU (without waiting); `why` documents the call site.
void submit(const char* why) {
    if (!R.frameOpen) return;
    submit_list(why);
}

SubmitStats submit_stats() { return g_submits; }

// ---- RL-NX: the per-frame deko3d line (R2_DEKO_FRAME_LOG=1) ---------------------------------------
// Everything here is counted at the point where it happens, so the line describes the conversion rather
// than the bridge's view of it. The question it answers is whether a frame that lost its world was cut
// into several submits, had its passes skipped for a shader, or never copied its XFB (see 9.27).
// The four biggest passes of the frame, by draws replayed, with the target each drew into: a frame whose
// line has no pass drawing into the presented texture, or whose biggest pass has no draws, says where the
// picture was lost without a capture.
struct PassRec {
    uint64_t draws = 0;
    unsigned blendTags[3] = {};     // the distinct blend modes (see diag_draw) of those draws, up to three
    uint32_t blendTagCounts[3] = {};  // and how many draws used each
    int blendTagCount = 0;
    unsigned cmp[2] = {};  // the distinct depth compare functions (DkCompareOp), up to two
    uint32_t cmpCounts[2] = {};  // and how many draws used each
    int cmpCount = 0;
    uint64_t depthTest = 0;  // how many of them had the depth test on
    char label[24] = {};
    char target[160] = {};  // colour label/size/id and the depth image's, from backend_commands.cpp
};
struct FrameDiag {
    uint64_t passes = 0, draws = 0, skipped = 0, passDraws = 0;
    unsigned passBlendTags[3] = {};   // this pass's distinct blend modes, up to three
    uint32_t passBlendTagCounts[3] = {};  // and how many draws used each
    int passBlendTagCount = 0;
    unsigned passCmp[2] = {};  // this pass's distinct depth compare functions, up to two
    uint32_t passCmpCounts[2] = {};  // and how many draws used each
    int passCmpCount = 0;
    uint64_t passDepthTest = 0;  // this pass's draws with the depth test on
    PassRec top[4];
    // RL-NX: every pass of the frame, in order, with its draw count - including the ones that drew nothing. The
    // four biggest are printed in full; this list answers the other half of "what did the bad frame do", which
    // is whether a pass that should have drawn ran at all.
    struct PassBrief {
        char label[24];
        uint64_t draws;
    };
    PassBrief brief[16];
    int briefCount = 0;
    uint64_t copies = 0, copyBytes = 0, xfbCopies = 0, xfbBytes = 0;
    // The first XFB copy's destination and the image it read: the picture's own target. Printed with the
    // passes' targets so a frame whose biggest pass drew somewhere else says so in one line.
    char xfbSrc[56] = {}, xfbDst[56] = {};
    uint64_t submits = 0, splits = 0;
    uint64_t passesAtCut = 0, cutPass = 0, cutDraws = 0;  // cutPass 0: the last cut fell between passes
    const char* cutWhy = "";
    bool passOpen = false;
} g_diag;

static bool diag_on() {
    static const bool on = getenv("R2_DEKO_FRAME_LOG") != nullptr;
    return on;
}
void diag_frame_begin() { g_diag = FrameDiag{}; }
void diag_pass_begin() {
    ++g_diag.passes;
    g_diag.passOpen = true;
    g_diag.passDraws = 0;
    g_diag.passBlendTagCount = 0;
    g_diag.passDepthTest = 0;
    g_diag.passCmpCount = 0;
}
void diag_pass_end(const char* label, const char* target, uint64_t draws) {
    g_diag.passOpen = false;
    if (g_diag.briefCount < 16) {
        FrameDiag::PassBrief& b = g_diag.brief[g_diag.briefCount++];
        snprintf(b.label, sizeof b.label, "%s", label ? label : "?");
        b.draws = draws;
    }
    int at = 0;
    while (at < 4 && g_diag.top[at].draws >= draws) ++at;
    if (at >= 4) return;  // not one of the frame's four biggest
    for (int i = 3; i > at; --i) g_diag.top[i] = g_diag.top[i - 1];
    PassRec& r = g_diag.top[at];
    r.draws = draws;
    memcpy(r.blendTags, g_diag.passBlendTags, sizeof r.blendTags);
    memcpy(r.blendTagCounts, g_diag.passBlendTagCounts, sizeof r.blendTagCounts);
    r.blendTagCount = g_diag.passBlendTagCount;
    memcpy(r.cmp, g_diag.passCmp, sizeof r.cmp);
    memcpy(r.cmpCounts, g_diag.passCmpCounts, sizeof r.cmpCounts);
    r.cmpCount = g_diag.passCmpCount;
    r.depthTest = g_diag.passDepthTest;
    snprintf(r.label, sizeof r.label, "%s", label ? label : "?");
    snprintf(r.target, sizeof r.target, "%s", target ? target : "?");
}
void diag_draw(bool issued, unsigned blendTag, bool depthTest, unsigned compareOp) {
    if (issued) {
        ++g_diag.draws;
        ++g_diag.passDraws;
        if (depthTest) {
            ++g_diag.passDepthTest;
            // The compare function decides what a depth clear of 0 means: with Less nothing is nearer than 0
            // and every tested draw fails; with Greater 0 is the nearest and the buffer works.
            int at = -1;
            for (int i = 0; i < g_diag.passCmpCount; ++i)
                if (g_diag.passCmp[i] == compareOp) at = i;
            if (at < 0 && g_diag.passCmpCount < 2) {
                at = g_diag.passCmpCount++;
                g_diag.passCmp[at] = compareOp;
                g_diag.passCmpCounts[at] = 0;
            }
            if (at >= 0) ++g_diag.passCmpCounts[at];
        }
        int at = -1;
        for (int i = 0; i < g_diag.passBlendTagCount; ++i)
            if (g_diag.passBlendTags[i] == blendTag) at = i;
        if (at < 0 && g_diag.passBlendTagCount < 3) {
            at = g_diag.passBlendTagCount++;
            g_diag.passBlendTags[at] = blendTag;
            g_diag.passBlendTagCounts[at] = 0;
        }
        if (at >= 0) ++g_diag.passBlendTagCounts[at];
    } else {
        ++g_diag.skipped;
    }
}
void diag_copy_bytes(uint32_t bytes, const char* label, const char* source) {
    ++g_diag.copies;
    g_diag.copyBytes += bytes;
    if (label && strstr(label, "XFB")) {
        if (g_diag.xfbCopies == 0) {
            snprintf(g_diag.xfbDst, sizeof g_diag.xfbDst, "%s", label);
            snprintf(g_diag.xfbSrc, sizeof g_diag.xfbSrc, "%s", source ? source : "?");
        }
        ++g_diag.xfbCopies;
        g_diag.xfbBytes += bytes;
    }
}
void diag_split(const char* why) {
    ++g_diag.submits;
    if (!why) return;  // the frame's own final submit is not a cut
    ++g_diag.splits;
    g_diag.passesAtCut = g_diag.passes;
    g_diag.cutPass = g_diag.passOpen ? g_diag.passes : 0;
    g_diag.cutDraws = g_diag.passOpen ? g_diag.passDraws : 0;
    g_diag.cutWhy = why;
}
void diag_frame_report() {
    if (!diag_on()) return;
    char cut[192];
    if (!g_diag.splits)
        snprintf(cut, sizeof cut, "none");
    else if (g_diag.cutPass)
        snprintf(cut, sizeof cut, "inside pass %llu after %llu of its draws (%s)",
                 (unsigned long long)g_diag.cutPass, (unsigned long long)g_diag.cutDraws, g_diag.cutWhy);
    else
        snprintf(cut, sizeof cut, "between passes, after pass %llu (%s)",
                 (unsigned long long)g_diag.passesAtCut, g_diag.cutWhy);
    char top[4 * 256] = "";
    size_t used = 0;
    for (const PassRec& r : g_diag.top) {
        if (r.draws == 0) continue;
        char modes[44] = "";
        for (int m = 0; m < r.blendTagCount; ++m) {
            const int k = snprintf(modes + strlen(modes), sizeof modes - strlen(modes), "%s%05xx%u",
                                   m ? "," : "", r.blendTags[m], r.blendTagCounts[m]);
            if (k <= 0) break;
        }
        char zops[24] = "";
        for (int z = 0; z < r.cmpCount; ++z) {
            const int k = snprintf(zops + strlen(zops), sizeof zops - strlen(zops), "%s%ux%u", z ? "," : "",
                                   r.cmp[z], r.cmpCounts[z]);
            if (k <= 0) break;
        }
        const int n = snprintf(top + used, sizeof top - used, " %s:%llu->%s[d%llu z%s b%s]", r.label,
                               (unsigned long long)r.draws, r.target, (unsigned long long)r.depthTest,
                               r.cmpCount ? zops : "-", r.blendTagCount ? modes : "-");
        if (n <= 0 || size_t(n) >= sizeof top - used) break;
        used += size_t(n);
    }
    char all[320] = "";
    size_t allUsed = 0;
    for (int i = 0; i < g_diag.briefCount; ++i) {
        const int n = snprintf(all + allUsed, sizeof all - allUsed, " %s:%llu", g_diag.brief[i].label,
                               (unsigned long long)g_diag.brief[i].draws);
        if (n <= 0 || size_t(n) >= sizeof all - allUsed) break;
        allUsed += size_t(n);
    }
    dklog("deko frame %llu: %llu passes, %llu draws replayed, %llu skipped for shaders; %llu submits, %llu "
          "splits, last cut %s; copies %llu (%.1f KiB), XFB %llu (%.1f KiB)%s%s%s%s; passes%s; all%s",
          (unsigned long long)(R.frame + 1), (unsigned long long)g_diag.passes,
          (unsigned long long)g_diag.draws, (unsigned long long)g_diag.skipped,
          (unsigned long long)g_diag.submits, (unsigned long long)g_diag.splits, cut,
          (unsigned long long)g_diag.copies, double(g_diag.copyBytes) / 1024.0,
          (unsigned long long)g_diag.xfbCopies, double(g_diag.xfbBytes) / 1024.0,
          g_diag.xfbCopies ? ", XFB copy " : "", g_diag.xfbSrc,
          g_diag.xfbCopies ? " -> " : "", g_diag.xfbCopies ? g_diag.xfbDst : "", top, all);
}

// SFA-NX (build 2009): a long frame (a level load records hundreds of copies) could put more entries in one
// command list than the GPU's command FIFO holds, and the GPU then hung with 2348-0006. Every 128 entries the
// list is submitted and flushed, so no list or kickoff grows past that.
// RL-NX: the burst bound. Tatooine Training's load records ~2400-4300 such batches in one frame (one
// kickoff each, ~109 entries and 42 syncpoint increments: every upload signals its own flushing fence) and
// never waits for the GPU; the driver then refused a kickoff with 2348-0006 while the GPU was still
// working through the previous one (LOGS 11242026-1427: 20 more uploads completed while the dump printed),
// and deko3d aborted 2359-0001 at the next flush. The attract demo sends the same uploads spread over the
// crawl and survives. So every R2_GPU_BURST_WAIT batches within a frame (default 4, 0 = off: the old
// behaviour) the worker waits for the GPU to go idle, keeping the kickoffs in flight bounded.
// R2_GPU_BATCH_ENTRIES sets the batch size (default 128, the SFA-NX value; 2345-0002 was seen at 1479).
struct BurstStats {
    uint32_t batches = 0, waits = 0;  // this frame
    uint64_t waitNs = 0, maxWaitNs = 0;
    // over the 30-second stats window
    uint64_t burstFrames = 0, totalWaits = 0, totalWaitNs = 0, windowMaxWaitNs = 0;
    uint32_t maxBatches = 0;
} g_burst;
uint32_t burst_env(const char* name, uint32_t fallback) {
    const char* v = getenv(name);
    return v && *v ? uint32_t(strtoul(v, nullptr, 10)) : fallback;
}
void gpfifo_entries_recorded(uint32_t entries) {
    // read on first use: boot.cfg reaches the environment after static constructors
    static const uint32_t batchEntries = std::max<uint32_t>(16, burst_env("R2_GPU_BATCH_ENTRIES", 128));
    static const uint32_t waitEvery = burst_env("R2_GPU_BURST_WAIT", 4);
    static bool logged = false;
    if (!logged) {
        logged = true;
        dklog("GPU bursts: a submit every %u GPFIFO entries; wait for the GPU every %u batches in a frame%s "
              "(R2_GPU_BATCH_ENTRIES, R2_GPU_BURST_WAIT)",
              batchEntries, waitEvery, waitEvery ? "" : " = never (the old behaviour)");
    }
    g_entriesInList += entries;
    if (g_entriesInList < batchEntries || !R.frameOpen) return;
    g_submits.fenceBatches++;
    submit_readback("GPFIFO entry batch");
    ++g_burst.batches;
    if (waitEvery && g_burst.batches % waitEvery == 0) {
        const uint64_t t0 = now_ns();
        dkQueueWaitIdle(R.queue);
        const uint64_t ns = now_ns() - t0;
        ++g_burst.waits;
        g_burst.waitNs += ns;
        ++g_pmDk.burstWaits;  // RL-NX: PERF LOG
        g_pmDk.burstWaitNs += ns;
        g_burst.maxWaitNs = std::max(g_burst.maxWaitNs, ns);
    }
}
// RL-NX: once per frame at its final submit: a frame with more than a few batches logs what it did.
void burst_frame_end(uint64_t frame) {
    BurstStats& b = g_burst;
    if (b.batches > 8) {
        ++b.burstFrames;
        static uint32_t logged = 0;
        if (logged < 64 || b.batches > b.maxBatches) {
            ++logged;
            dklog("GPU burst: frame %llu: %u batch submits, %u GPU waits (%.1f ms, longest %.2f ms)",
                  (unsigned long long)frame, b.batches, b.waits, double(b.waitNs) / 1e6, double(b.maxWaitNs) / 1e6);
        }
    }
    b.maxBatches = std::max(b.maxBatches, b.batches);
    b.totalWaits += b.waits;
    b.totalWaitNs += b.waitNs;
    b.windowMaxWaitNs = std::max(b.windowMaxWaitNs, b.maxWaitNs);
    b.batches = b.waits = 0;
    b.waitNs = b.maxWaitNs = 0;
    // a 30-second summary, only when the window had a burst
    static uint64_t windowStart = 0;
    const uint64_t now = now_ns();
    if (!windowStart) windowStart = now;
    if (now - windowStart < 30'000'000'000ull) return;
    windowStart = now;
    if (b.burstFrames)
        dklog("GPU bursts (30 s): %llu burst frames, most batches in a frame %u, %llu GPU waits (%.1f ms, longest "
              "%.2f ms)", (unsigned long long)b.burstFrames, b.maxBatches, (unsigned long long)b.totalWaits,
              double(b.totalWaitNs) / 1e6, double(b.windowMaxWaitNs) / 1e6);
    b.burstFrames = b.totalWaits = b.totalWaitNs = b.windowMaxWaitNs = 0;
    b.maxBatches = 0;
}

// Submits and flushes at once, for a CPU wait on a fence that follows (readbacks, diagnostics).
void submit_readback(const char* why) {
    submit(why);
    std::lock_guard<std::mutex> lock(fence_mutex());
    dkQueueFlush(R.queue);
}

// ---- RL-NX: capture the frame that breaks from its neighbours (R2_CAPTURE_ON_BREAK=1) -------------
// capture_frame samples one presented frame in five seconds (one in about seventy at the level's 14 fps)
// and a flicker lasts one or two frames, so the cadence misses most of them. This watches every presented
// frame instead: each is copied into a ring of readbacks by the frame's own command list (no extra submit,
// no CPU wait) with the counters of its "deko frame" line, and a slot is judged a present or two later,
// once the GPU has written it, against the median of the last kBreakWindow good frames: passes, draws
// replayed, draws skipped for shaders, XFB copies, copy bytes and the number of lit pixels. A frame that
// differs is written as a PPM together with the frame before it, so a break arrives as a matched pair.
// docs/PERF_HANDOFF.md 9.27.
constexpr int kBreakWindow = 6;  // good frames the median comes from
constexpr int kBreakSlots = 3;   // readbacks in flight; a slot is judged on the presents before its reuse
// RL-NX (R2_CAPTURE_FLICKER): a judged frame's coarse luma grid, for the one-frame change trigger below.
constexpr int kGridW = 64, kGridH = 36;

// RL-NX: a break-capture switch's number, or `fallback` when it is not set.
static uint64_t break_env(const char* name, uint64_t fallback) {
    const char* v = getenv(name);
    return v ? strtoull(v, nullptr, 10) : fallback;
}

struct BreakShape {
    uint64_t passes = 0, draws = 0, skipped = 0, xfbCopies = 0, copyBytes = 0;
    // RL-NX: the frame's biggest pass's draw count, so R2_CAPTURE_MIN_DRAWS can keep the attract mode and
    // the menus out of the judgement (they own every break the 11252026-0843 run wrote).
    uint64_t topPass = 0;
};
struct BreakSlot {
    Block block{};        // the readback this frame was copied into
    DkFence fence{};      // signalled by the frame's own final submit
    uint64_t frame = 0;   // the frame it holds (0: empty)
    // Judged frames keep their `frame`, so the frame after a break can still be found for the pair; the
    // slot is only reused kBreakSlots presents later.
    bool judged = false;
    uint32_t width = 0, height = 0, pitch = 0;
    BreakShape shape;
    // RL-NX (R2_CAPTURE_FLICKER): this frame's luma grid, and its mean difference from the frame before it.
    uint8_t grid[kGridW * kGridH] = {};
    bool hasGrid = false;
    uint32_t diff = 0;
};
struct BreakGood {
    BreakShape shape;
    uint32_t lit = 0;
};
static BreakSlot s_breakRing[kBreakSlots];
static BreakGood s_breakGood[kBreakWindow];
static int s_breakGoodCount = 0, s_breakGoodAt = 0;
// Consecutive judged frames that broke. A one- or two-frame flicker leaves this low; a scene change keeps
// it climbing, and at kBreakResetAfter the baseline is thrown away and restarted from that frame - without
// which a transition would leave the median stuck on the old scene and every frame after it a "break".
static unsigned s_breakRun = 0;
constexpr unsigned kBreakResetAfter = 8;
// The cooldown is in frames, not seconds: frames are the clock this all runs on, and Ryujinx's counter
// reads 0 for cntpct (the time-based version dumped every other frame there). 40 frames is about 3 s at
// the level's 14 fps.
static uint64_t s_breakLastDumpFrame = 0;
constexpr uint64_t kBreakDumpGap = 40;
static unsigned s_breakDumps = 0;
constexpr unsigned kBreakMaxDumps = 24;

// A PPM from a readback buffer, downscaled by `scale` (nearest neighbour). Also the cadence capture's
// writer: one place that gets the row order and the channel order right.
static bool write_ppm(const char* path, const uint8_t* pixels, uint32_t pitch, uint32_t width,
                      uint32_t height, bool bgra, uint32_t scale) {
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    const uint32_t w = (width + scale - 1) / scale, h = (height + scale - 1) / scale;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = pixels + size_t(y) * scale * pitch + size_t(x) * scale * 4;
            uint8_t* d = rgb.data() + (size_t(y) * w + x) * 3;
            d[0] = p[bgra ? 2 : 0];
            d[1] = p[1];
            d[2] = p[bgra ? 0 : 2];
        }
    bool wrote = fprintf(out, "P6\n%u %u\n255\n", w, h) > 0 &&
                 fwrite(rgb.data(), 1, rgb.size(), out) == rgb.size();
    if (fclose(out)) wrote = false;
    return wrote;
}

// ---- RL-NX: the depth of a broken frame (R2_DEPTH_SHOT=1) -------------------------------------------
// The recordings leave exactly one question open: the world's depth-tested draws produced no pixels while
// everything undepth-tested in the same frame was correct. Whether those draws wrote depth at all splits
// that in two - "the draws never rasterised" from "they rasterised and the depth they compared against was
// wrong" - and no counter in the frame can say it. The frame's biggest pass's depth target is copied into a
// ring by the frame's own command list (no extra submit), and when the break trigger writes a pair it writes
// that frame's depth beside it as an 8-bit PGM: 0 = near, 255 = far.
struct DepthShot {
    Block block{};
    DkFence fence{};
    uint64_t frame = 0;
    uint32_t width = 0, height = 0, pitch = 0;
    bool floatDepth = true;  // RL-NX: ZF32 (the EFB's) or Z24X8/Z24S8, see depth_value()
};
static DepthShot s_depthShot[kBreakSlots];
static uint64_t s_depthShotFrame = 0, s_depthShotDraws = 0;
static bool depth_shot_on() {
    static const bool on = getenv("R2_DEPTH_SHOT") != nullptr;
    return on;
}
// RL-NX: one pixel's depth as the float the GPU stored. The EFB's depth target is not Z24: Aurora asks for
// Depth32Float, which switch/deko/aurora/dk_objects.cpp maps to DkImageFormat_ZF32, so every depth dump this
// port has written was decoding byte 2 of a 32-bit float as if it were the top byte of a 24-bit integer. Byte 2
// of 1.0f (0x3F800000, little-endian 00 00 80 3F) is 0x80 - which is where 9.29's "one constant, 0x80 (= 0.502),
// the middle of the depth range" came from. The value in those buffers was 1.0, the far end of the range, which
// is why nothing depth-tested behind it ever drew (9.30).
static float depth_value(const uint8_t* p, bool floatDepth) {
    if (floatDepth) {
        const uint32_t bits = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }
    return float(p[2]) / 255.f;  // Z24X8/Z24S8: the 24-bit depth's top byte
}
// Writes 8-bit depth as a PGM: 0 = 0.0 (the far plane in this port's reversed depth), 255 = 1.0 (near).
static bool write_pgm(const char* path, const uint8_t* pixels, uint32_t pitch, uint32_t width,
                      uint32_t height, bool floatDepth) {
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    std::vector<uint8_t> z(size_t(width) * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = pixels + size_t(y) * pitch + size_t(x) * 4;
            const float d = depth_value(p, floatDepth);
            z[size_t(y) * width + x] = uint8_t(std::clamp(d, 0.f, 1.f) * 255.f + 0.5f);
        }
    bool wrote = fprintf(out, "P5\n%u %u\n255\n", width, height) > 0 &&
                 fwrite(z.data(), 1, z.size(), out) == z.size();
    if (fclose(out)) wrote = false;
    return wrote;
}
// Writes the depth of `frame` when the ring still holds it and the GPU has written it; appends the path.
static void depth_shot_dump(uint64_t frame, char* files, size_t size) {
    for (DepthShot& d : s_depthShot) {
        if (d.frame != frame || !d.block) continue;
        if (dkFenceWait(&d.fence, 0) != DkResult_Success) return;
        char path[192];
        snprintf(path, sizeof path, COS_SWITCH_DATA_DIR "/logs/deko-capture/depth-%04llu.pgm",
                 (unsigned long long)frame);
        if (write_pgm(path, d.block.cpu, d.pitch, d.width, d.height, d.floatDepth) &&
            strlen(files) + strlen(path) + 4 < size) {
            strcat(files, " + ");
            strcat(files, path);
        }
        d.frame = 0;  // written once
        return;
    }
}

// RL-NX (R2_FLICKER_LOG): what the frame's depth buffer holds, as one short signature, for every judged frame.
// Run 2 (11252026-0924) proved the pair dumps alone cannot separate two very different faults: a frame whose
// world is missing but whose depth holds a picture (a gradient, many values) lost its colour, while a frame whose
// depth holds one constant over the world's whole rect never had geometry reach the target at all. The picture
// line carries this beside the luma, so every frame of a run - not just the 30 dumps - says which one it was.
static bool depth_signature(uint64_t frame, char* out, size_t size) {
    for (DepthShot& d : s_depthShot) {
        if (d.frame != frame || !d.block) continue;
        if (dkFenceWait(&d.fence, 0) != DkResult_Success) return false;
        uint32_t hist[256] = {};
        float lo = 2.f, hi = -1.f;
        for (uint32_t y = 0; y < d.height; ++y)
            for (uint32_t x = 0; x < d.width; ++x) {
                const float v = depth_value(d.block.cpu + size_t(y) * d.pitch + size_t(x) * 4, d.floatDepth);
                ++hist[uint8_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f)];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        const uint64_t n = uint64_t(d.width) * d.height;
        int mode = 0;
        for (int i = 1; i < 256; ++i) if (hist[i] > hist[mode]) mode = i;
        // RL-NX: the values themselves, not a byte of them. A frame that lost its world reads "0.000..1.000,
        // 1.000 holds 94% of 640x480" - one constant at the far plane - which is a different fault from a
        // buffer holding a picture, and 9.29 could not tell the two apart.
        // RL-NX: the format is part of the line. 0.502 and 1.0 are the same byte (0x80) on a Z24 and a float
        // target, and 9.29/9.30 spent two runs arguing about which one they were looking at.
        snprintf(out, size, "depth(%s) %.3f..%.3f, %.3f holds %llu%% of %ux%u, %d/%d values",
                 d.floatDepth ? "f32" : "z24", lo, hi, mode / 255.f,
                 (unsigned long long)(uint64_t(hist[mode]) * 100 / n), d.width, d.height,
                 int(std::count_if(std::begin(hist), std::end(hist), [](uint32_t c) { return c != 0; })), 256);
        return true;
    }
    return false;
}

static uint64_t break_median(const uint64_t* v, int n) {
    uint64_t s[kBreakWindow];
    for (int i = 0; i < n && i < kBreakWindow; ++i) s[i] = v[i];
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && s[j] < s[j - 1]; --j) {
            const uint64_t t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    return (n & 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
}

// How far `v` may sit from the median before it counts as a break, once the median is big enough for a
// ratio to mean anything (a median of 4 draws says nothing about a frame's 3).
static bool break_deviates(uint64_t v, uint64_t median, uint64_t floorValue, int percent) {
    if (median < floorValue) return false;
    return v * 100 < median * uint64_t(100 - percent) || v * 100 > median * uint64_t(100 + percent);
}

// Lit pixels, every other pixel in both directions (a quarter of the picture: the count only has to move
// with the world's presence, and a full 921,600-pixel scan each frame is CPU the game needs).
static uint32_t break_lit(const uint8_t* pixels, uint32_t pitch, uint32_t width, uint32_t height) {
    uint32_t lit = 0;
    for (uint32_t y = 0; y < height; y += 2)
        for (uint32_t x = 0; x < width; x += 2) {
            const uint8_t* p = pixels + size_t(y) * pitch + size_t(x) * 4;
            lit += (p[0] | p[1] | p[2]) != 0;
        }
    return lit;
}

// RL-NX (R2_CAPTURE_FLICKER=1): the one-frame change trigger. The lit count above only moves when the picture
// gains or loses *lit* pixels, and the trench is lit edge to edge: a frame that loses the world there keeps
// its sky's non-zero pixels, the count does not move, and that is why not one level frame in the 11252026-0843
// run broke on lit pixels (the 24 dumps it wrote were all attract mode). What does move is the difference
// between two consecutive presented frames: the world vanishing changes the whole picture in one frame, which
// the camera alone cannot. Each judged frame is reduced to a 64x36 luma grid (four samples a cell) and the
// mean absolute difference against the frame before it is compared with the same number for the frame before
// that - a spike is a flicker's edge, whether the frame is entering the flicker or leaving it.
static void break_grid(const uint8_t* pixels, uint32_t pitch, uint32_t width, uint32_t height, uint8_t* out) {
    for (int gy = 0; gy < kGridH; ++gy) {
        const uint32_t y0 = uint32_t(gy) * height / kGridH;
        const uint32_t y1 = std::max(y0 + 1, uint32_t(gy + 1) * height / kGridH) - 1;
        for (int gx = 0; gx < kGridW; ++gx) {
            const uint32_t x0 = uint32_t(gx) * width / kGridW;
            const uint32_t x1 = std::max(x0 + 1, uint32_t(gx + 1) * width / kGridW) - 1;
            const uint32_t xs[2] = {x0, x1}, ys[2] = {y0, y1};
            uint32_t luma = 0;
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j) {
                    const uint8_t* p = pixels + size_t(ys[i]) * pitch + size_t(xs[j]) * 4;
                    luma += (uint32_t(p[0]) * 77 + uint32_t(p[1]) * 150 + uint32_t(p[2]) * 29) >> 8;  // Rec.601
                }
            out[gy * kGridW + gx] = uint8_t(luma / 4);
        }
    }
}
// Mean absolute difference of two grids, 0..255.
static uint32_t break_mean_diff(const uint8_t* a, const uint8_t* b) {
    uint32_t sad = 0;
    for (int i = 0; i < kGridW * kGridH; ++i)
        sad += a[i] > b[i] ? uint32_t(a[i] - b[i]) : uint32_t(b[i] - a[i]);
    return sad / uint32_t(kGridW * kGridH);
}

// RL-NX: writes one slot's frame as a PPM and appends the path (" + path") to `files`.
static bool dump_slot(BreakSlot& s, const char* prefix, char* files, size_t size) {
    if (!s.block || !s.width || !s.height) return false;
    char path[192];
    snprintf(path, sizeof path, COS_SWITCH_DATA_DIR "/logs/deko-capture/%s-%04llu.ppm", prefix,
             (unsigned long long)s.frame);
    const uint32_t scale = getenv("R2_CAPTURE_FULL") ? 1u : 2u;
    if (!write_ppm(path, s.block.cpu, s.pitch, s.width, s.height, false, scale)) return false;
    if (strlen(files) + strlen(path) + 4 < size) {
        if (files[0]) strcat(files, " + ");
        strcat(files, path);
    }
    return true;
}

static void break_remember(const BreakShape& shape, uint32_t lit) {
    s_breakGood[s_breakGoodAt].shape = shape;
    s_breakGood[s_breakGoodAt].lit = lit;
    s_breakGoodAt = (s_breakGoodAt + 1) % kBreakWindow;
    if (s_breakGoodCount < kBreakWindow) ++s_breakGoodCount;
}

// Judges the slot's frame once the GPU has written it: true when it has (the slot may be reused), false
// when the readback is still in flight (the next present tries again). `prev` is the slot holding the
// frame before it, when that is still there, so a break is written as a pair.
static bool break_judge(BreakSlot& s, BreakSlot* prev) {
    if (dkFenceWait(&s.fence, 0) != DkResult_Success) return false;
    s.judged = true;  // every path below consumes the pixels
    // RL-NX (R2_CAPTURE_MIN_DRAWS=n): a frame whose biggest pass drew fewer than n draws is not judged, so
    // the attract mode, the menus and the loading screens cannot spend the run's dumps on themselves.
    static const uint64_t minDraws = break_env("R2_CAPTURE_MIN_DRAWS", 0);
    if (s.shape.topPass < minDraws) return true;
    const uint32_t lit = break_lit(s.block.cpu, s.pitch, s.width, s.height);
    // RL-NX (R2_CAPTURE_FLICKER): this frame's luma grid and its difference from the frame before it.
    break_grid(s.block.cpu, s.pitch, s.width, s.height, s.grid);
    s.hasGrid = true;
    const bool haveBefore = prev && prev->block && prev->hasGrid && prev->frame + 1 == s.frame;
    s.diff = haveBefore ? break_mean_diff(s.grid, prev->grid) : 0;
    static const bool flickerOn = getenv("R2_CAPTURE_FLICKER") != nullptr;
    static const bool flickerLog = getenv("R2_FLICKER_LOG") != nullptr;
    static const uint64_t flickerFloor = break_env("R2_FLICKER_THRESH", 20);
    if (flickerLog) {
        // RL-NX: the depth signature is part of the per-frame line, so a run's whole timeline says whether a
        // world-less frame had a world in its depth (colour lost) or a bare clear (nothing reached the target).
        char depth[160] = "depth not read (its fence was not ready)";
        depth_signature(s.frame, depth, sizeof depth);
        dklog("picture: frame %llu changed %u/255 (the frame before %u/255), top pass %llu draws, lit %u, %s",
              (unsigned long long)s.frame, s.diff, haveBefore ? prev->diff : 0,
              (unsigned long long)s.shape.topPass, lit, depth);
    }
    // A flicker's edge: a jump far beyond the change the frame before it already had. The factor keeps a fast
    // camera pan (a high, steady change) from triggering, and the floor keeps a still one from doing it.
    if (flickerOn && haveBefore && s.diff >= flickerFloor && uint64_t(s.diff) * 2 >= uint64_t(prev->diff) * 5) {
        const uint64_t gap = break_env("R2_CAPTURE_GAP", kBreakDumpGap);
        const bool dump = s_breakDumps < break_env("R2_CAPTURE_DUMPS", kBreakMaxDumps) &&
                          (gap == 0 || s_breakLastDumpFrame == 0 || s.frame >= s_breakLastDumpFrame + gap);
        // RL-NX: 400 bytes, not 200 - two PPMs and two PGMs do not fit in 200, which is why run 2's pairs
        // named only the first depth image.
        char files[400] = "nothing (dumps used up, or within the gap)";
        if (dump) {
            files[0] = 0;
            dump_slot(*prev, "flk-before", files, sizeof files);
            dump_slot(s, "flk-after", files, sizeof files);
            depth_shot_dump(prev->frame, files, sizeof files);
            depth_shot_dump(s.frame, files, sizeof files);
            if (!files[0]) snprintf(files, sizeof files, "nothing (write failed)");
            s_breakLastDumpFrame = s.frame;
            ++s_breakDumps;
        }
        dklog("deko flicker at frame %llu: the picture changed %u/255 where the frame before it changed %u/255 "
              "(floor %llu), %llu draws in the top pass, lit %u; capture %s",
              (unsigned long long)s.frame, s.diff, prev->diff, (unsigned long long)flickerFloor,
              (unsigned long long)s.shape.topPass, lit, files);
        return true;  // a flicker is not a scene change: the baseline is left as it is
    }
    if (s_breakGoodCount < 4) {  // no baseline yet: the first frames are whatever the game starts with
        break_remember(s.shape, lit);
        return true;
    }
    const int n = s_breakGoodCount;
    uint64_t mp[kBreakWindow], md[kBreakWindow], ms[kBreakWindow], mx[kBreakWindow], mc[kBreakWindow],
        ml[kBreakWindow];
    for (int i = 0; i < n; ++i) {
        mp[i] = s_breakGood[i].shape.passes;
        md[i] = s_breakGood[i].shape.draws;
        ms[i] = s_breakGood[i].shape.skipped;
        mx[i] = s_breakGood[i].shape.xfbCopies;
        mc[i] = s_breakGood[i].shape.copyBytes;
        ml[i] = s_breakGood[i].lit;
    }
    const uint64_t medPasses = break_median(mp, n), medDraws = break_median(md, n),
                   medSkipped = break_median(ms, n), medXfb = break_median(mx, n),
                   medCopy = break_median(mc, n), medLit = break_median(ml, n);
    char why[192] = "";
    size_t used = 0;
    auto note = [&](const char* what, uint64_t v, uint64_t m) {
        if (used + 48 >= sizeof why) return;
        const int wrote = snprintf(why + used, sizeof why - used, "%s%s %llu vs %llu", used ? ", " : "", what,
                                   (unsigned long long)v, (unsigned long long)m);
        if (wrote > 0) used += size_t(wrote);
    };
    if (s.shape.passes != medPasses) note("passes", s.shape.passes, medPasses);
    if (break_deviates(s.shape.draws, medDraws, 32, 25)) note("draws", s.shape.draws, medDraws);
    if (s.shape.skipped > 0 && medSkipped == 0) note("skipped draws", s.shape.skipped, medSkipped);
    if (s.shape.xfbCopies != medXfb) note("XFB copies", s.shape.xfbCopies, medXfb);
    if (break_deviates(lit, medLit, 2000, 25)) note("lit pixels", lit, medLit);
    if (break_deviates(s.shape.copyBytes, medCopy, 256u << 10, 50)) note("copy bytes", s.shape.copyBytes, medCopy);
    if (!why[0]) {
        s_breakRun = 0;
        break_remember(s.shape, lit);
        return true;
    }
    if (++s_breakRun >= kBreakResetAfter) {
        // The new shape is the scene's, not a flicker's: restart the baseline from this frame.
        s_breakGoodCount = 0;
        s_breakGoodAt = 0;
        s_breakRun = 0;
        break_remember(s.shape, lit);
        dklog("deko break at frame %llu: %s; %u frames in a row differ, so this is the scene changing - "
              "baseline reset to this frame, nothing written",
              (unsigned long long)s.frame, why, kBreakResetAfter);
        return true;
    }
    // One break can belong to a transition that also moves the next frames; a dump per %llu frames keeps a
    // run of them from using up the captures a run gets.
    const uint64_t gap = break_env("R2_CAPTURE_GAP", kBreakDumpGap);
    const bool dump = s_breakDumps < break_env("R2_CAPTURE_DUMPS", kBreakMaxDumps) &&
                      (gap == 0 || s_breakLastDumpFrame == 0 || s.frame >= s_breakLastDumpFrame + gap);
    char files[160] = "nothing (dumps used up, or within the last 40 frames)";
    if (dump) {
        char path[192];
        snprintf(path, sizeof path, COS_SWITCH_DATA_DIR "/logs/deko-capture/break-%04llu.ppm",
                 (unsigned long long)s.frame);
        const uint32_t scale = getenv("R2_CAPTURE_FULL") ? 1u : 2u;
        const bool wrote = write_ppm(path, s.block.cpu, s.pitch, s.width, s.height, false, scale);
        snprintf(files, sizeof files, "%s%s", path, wrote ? "" : " (write failed)");
        if (prev && prev->block && prev->frame + 1 == s.frame &&
            dkFenceWait(&prev->fence, 0) == DkResult_Success) {
            char prevPath[192];
            snprintf(prevPath, sizeof prevPath, COS_SWITCH_DATA_DIR "/logs/deko-capture/prev-%04llu.ppm",
                     (unsigned long long)prev->frame);
            if (write_ppm(prevPath, prev->block.cpu, prev->pitch, prev->width, prev->height, false, scale) &&
                strlen(files) + strlen(prevPath) + 4 < sizeof files) {
                strcat(files, " + ");
                strcat(files, prevPath);
            }
        }
        depth_shot_dump(s.frame, files, sizeof files);
        s_breakLastDumpFrame = s.frame;
        ++s_breakDumps;
    }
    dklog("deko break at frame %llu: %s; the frame: %llu passes, %llu draws, "
          "%llu skipped, XFB %llu, %.1f KiB copied; capture %s",
          (unsigned long long)s.frame, why,
          (unsigned long long)s.shape.passes, (unsigned long long)s.shape.draws,
          (unsigned long long)s.shape.skipped, (unsigned long long)s.shape.xfbCopies,
          double(s.shape.copyBytes) / 1024.0, files);
    return true;
}

// frame_present, after the picture is drawn: judge the slots that are ready, then copy this frame into the
// slot that is free. The copy is part of the frame's own command list, so the trigger adds no submit.
static void break_capture_present(uint64_t frame, const DkImageView& view, uint32_t width, uint32_t height,
                                  DkImageFormat format) {
    static const bool on = getenv("R2_CAPTURE_ON_BREAK") != nullptr;
    if (!on || format != DkImageFormat_RGBA8_Unorm) return;
    if (!width || !height || width > 1920 || height > 1080) return;
    static const bool ready = [] {
        mkdir(COS_SWITCH_DATA_DIR "/logs", 0777);
        mkdir(COS_SWITCH_DATA_DIR "/logs/deko-capture", 0777);
        dklog("deko break capture on: every presented frame is read back %d frames later and compared with "
              "the median of the last %d good frames; a break is written as break-NNNN.ppm with prev-NNNN.ppm",
              kBreakSlots - 1, kBreakWindow);
        return true;
    }();
    (void)ready;
    for (int k = kBreakSlots - 1; k >= 1; --k) {
        if (frame <= uint64_t(k)) continue;
        BreakSlot& old = s_breakRing[uint32_t((frame - uint64_t(k)) % kBreakSlots)];
        if (!old.frame || old.frame >= frame) continue;
        if (old.judged) continue;
        BreakSlot* prev = old.frame >= 2 ? &s_breakRing[uint32_t((old.frame - 1) % kBreakSlots)] : nullptr;
        if (prev && prev->frame + 1 != old.frame) prev = nullptr;
        break_judge(old, prev);
    }
    BreakSlot& s = s_breakRing[uint32_t(frame % kBreakSlots)];
    if (s.frame && !s.judged)
        dklog("deko break: frame %llu was never judged (its readback never reached the CPU)",
              (unsigned long long)s.frame);
    s.judged = false;
    const uint32_t pitch = (width * 4 + 255) & ~255u;
    if (!s.block || s.block.size < pitch * height) {
        if (s.block) block_free_deferred(s.block.block);
        s.block = block_create(pitch * height, "break-capture readback");
        if (!s.block) {
            dklog("deko break: no %u KiB block for the readback", (pitch * height) >> 10);
            return;
        }
    }
    s.width = width;
    s.height = height;
    s.pitch = pitch;
    s.shape = BreakShape{g_diag.passes, g_diag.draws, g_diag.skipped, g_diag.xfbCopies, g_diag.copyBytes,
                         g_diag.top[0].draws};
    s.fence = DkFence{};
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    const DkCopyBuf dst{s.block.gpu, pitch, pitch * height};
    const DkImageRect rect{0, 0, 0, width, height, 1};
    dkCmdBufCopyImageToBuffer(R.cmd, &view, &rect, &dst, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    dkCmdBufSignalFence(R.cmd, &s.fence, true);
    s.frame = frame;
}

// end_pass_native: copies the frame's biggest pass's depth target into the ring. A later, bigger pass in the
// same frame re-copies into the same slot, so the slot ends up holding the deepest pass's depth.
void depth_shot_pass(const DkImageView& depth, uint32_t width, uint32_t height, uint64_t draws,
                     bool floatDepth) {
    if (!depth_shot_on() || !width || !height || width > 1920 || height > 1080) return;
    const uint64_t frame = R.frame + 1;
    if (s_depthShotFrame != frame) {
        s_depthShotFrame = frame;
        s_depthShotDraws = 0;
    }
    if (draws < s_depthShotDraws || draws == 0) return;  // not the frame's big pass (yet)
    s_depthShotDraws = draws;
    DepthShot& s = s_depthShot[frame % kBreakSlots];
    const uint32_t pitch = (width * 4 + 255) & ~255u;
    if (!s.block || s.block.size < pitch * height) {
        if (s.block) block_free_deferred(s.block.block);
        s.block = block_create(pitch * height, "depth shot readback");
        if (!s.block) return;
    }
    s.frame = frame;
    s.width = width;
    s.height = height;
    s.pitch = pitch;
    s.floatDepth = floatDepth;  // RL-NX: how depth_value() reads this target
    s.fence = DkFence{};
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    const DkCopyBuf dst{s.block.gpu, pitch, pitch * height};
    const DkImageRect rect{0, 0, 0, width, height, 1};
    dkCmdBufCopyImageToBuffer(R.cmd, &depth, &rect, &dst, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    dkCmdBufSignalFence(R.cmd, &s.fence, true);
}

// Gets the next swapchain image before the renderer's locks are taken (see backend_present.cpp). Returns
// false when the window is about to be resized; frame_present then acquires the image itself.
bool present_acquire(PresentImage* image) {
    // R.width/R.height change only in resize_window, which runs on this (the presenting) thread.
    uint32_t ww, wh;
    window_wanted(ww, wh);
    if (ww != R.width || wh != R.height || g_swapchain == nullptr) return false;
    const uint64_t t0 = now_ns();
    dkSwapchainAcquireImage(g_swapchain, &image->slot, &image->fence);
    image->acquireNs = now_ns() - t0;
    return true;
}

// Draws the frame's picture into the swapchain image and presents it. `picture` is called three times:
// stage 0 before the target is bound, 1 to draw, 2 after drawing (the callbacks use only stage 1).
namespace {
// RL-NX: the counter's clock (overlay_fps). One sample a present, smoothed over about ten of them so
// the number is readable, and reset after a gap so a loading screen does not drag it down for seconds.
float g_fpsAvg = 0.0f;
uint64_t g_fpsLastNs = 0;
}  // namespace

// RL-NX: frames actually presented per second. Not the guest's 59.9 - the game paces itself on the
// emulated VI and keeps printing 60 whatever the renderer manages (11-17 a second in level 1).
float overlay_fps() { return g_fpsAvg; }

// RL-NX: R2_FPS=0 hides the counter. On by default: it is the number the performance work needs, and
// the guest's own fps line cannot supply it.
// RL-NX: switchable at run time by the - menu's FRAME RATE row (overlay_fps_set); R2_FPS sets the start state.
std::atomic<int> g_fpsOverlay{-1};  // -1 = not read from R2_FPS yet
bool overlay_fps_enabled() {
    int on = g_fpsOverlay.load(std::memory_order_relaxed);
    if (on < 0) {
        const char* e = getenv("R2_FPS");
        // RL-NX: on unless R2_FPS=0. The old test was inverted (R2_FPS=1 hid the counter), so a saved FRAME
        // RATE ON in config/menu.cfg came back OFF at every boot.
        on = !(e != nullptr && *e == '0');
        int expected = -1;
        if (!g_fpsOverlay.compare_exchange_strong(expected, on, std::memory_order_relaxed)) on = expected;
    }
    return on != 0;
}
void overlay_fps_set(bool on) { g_fpsOverlay.store(on ? 1 : 0, std::memory_order_relaxed); }

void frame_present(PictureFn picture, void* user, const PresentImage* acquired, bool fullWindow) {
    // RL-NX: no swapchain (init_swapchain failed, see there): nothing can be presented, so the frame is
    // dropped instead of dereferencing a null swapchain. The caller's picture is not drawn either - it
    // exists only to fill the window.
    //
    // The swapchain is also what throttles the render worker: acquiring an image waits for the display,
    // so the CPU can never queue far ahead of the GPU. Without it the worker runs free, the queue backs
    // up, and deko3d's GPFIFO ring fills (Queue::appendGpfifoEntries -> RaiseError, measured ~28 s into a
    // run). Waiting for the GPU every other frame bounds what is in flight and keeps a frame of overlap.
    // A console with a swapchain never reaches this branch.
    if (!g_swapchain) return;
    frame_open();
    // RL-NX: one sample of the presented frame rate for the counter (overlay_fps).
    {
        const uint64_t fpsNow = now_ns();
        if (g_fpsLastNs != 0) {
            const float dt = float(fpsNow - g_fpsLastNs) / 1e9f;
            if (dt >= 5.0f) {
                g_fpsAvg = 0.0f;  // a load, not a frame: start again rather than average across it
            } else if (dt > 0.0f) {
                const float inst = 1.0f / dt;
                g_fpsAvg = g_fpsAvg > 0.0f ? g_fpsAvg * 0.9f + inst * 0.1f : inst;
            }
        }
        g_fpsLastNs = fpsNow;
    }
    const uint64_t frame = R.frame + 1;
    // An image acquired before the lock is presented at the size it was acquired for; a mode change
    // since then resizes at the next present.
    if (!acquired) {
        uint32_t ww, wh;
        window_wanted(ww, wh);
        if (ww != R.width || wh != R.height) resize_window(ww, wh, frame);
    }
    const uint64_t t1 = now_ns();
    int slot;
    if (acquired) {
        // dkQueueAcquireImage's second half, at the same point in the queue.
        check_queue("waiting for a swapchain image");
        DkFence fence = acquired->fence;
        dkQueueWaitFence(R.queue, &fence);
        slot = acquired->slot;
    } else {
        check_queue("acquiring a swapchain image");
        slot = dkQueueAcquireImage(R.queue, g_swapchain);
    }
    const uint64_t t2 = now_ns();
    if (picture && !g_testPattern) picture(user, 0);
    DkImageView color, depth;
    dkImageViewDefaults(&color, &g_swapImages[slot]);
    dkImageViewDefaults(&depth, &g_depth);
    const DkImageView* colors[] = {&color};
    // SFA-NX: the game's picture (fullWindow) is one full-window draw without depth, so it binds no depth
    // buffer and skips both clears: two full-window writes a frame (1920x1080 docked). The boot screen and
    // the test pattern still clear and keep the depth buffer.
    const bool skipClears = fullWindow && picture && !g_testPattern;
    dkCmdBufBindRenderTargets(R.cmd, colors, 1, skipClears ? nullptr : &depth);
    static const DkViewportSwizzle kIdentity = {DkSwizzle_PositiveX, DkSwizzle_PositiveY, DkSwizzle_PositiveZ,
                                                DkSwizzle_PositiveW};
    dkCmdBufSetViewportSwizzles(R.cmd, 0, &kIdentity, 1);  // (the game's passes may have flipped y: C.flipY)
    set_view(0, 0, R.width, R.height);
    if (!skipClears) {
        dkCmdBufClearColorFloat(R.cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
        dkCmdBufClearDepthStencil(R.cmd, true, 1.0f, 0xFF, 0);
    }
    if (g_testPattern) {
        pattern_draw();
    } else if (picture) {
        picture(user, 1);
        dkCmdBufSetViewportSwizzles(R.cmd, 0, &kIdentity, 1);  // (C.flipPresent)
        set_view(0, 0, R.width, R.height);
    }
    // RL-NX: the - menu's panel, over the game's picture in the same swapchain pass, after the R2_FPS counter
    // that stage 1 draws with the same draw_text (proven on hardware). Stage 2 records nothing for the game
    // picture (backend_present.cpp returns for every stage but 1), so drawing here or after it is the same.
    // rl_nx_menu_draw records nothing while the panel is closed. R2_MENU_DRAW=0 skips it (bisect). The
    // panel sets its own viewport per box, so the frame's full-window one is restored after it.
    static const bool menuDraw = []() {
        const char* e = getenv("R2_MENU_DRAW");
        return !(e != nullptr && *e == '0');
    }();
    if (menuDraw && picture && !g_testPattern && rl_nx_menu_draw()) set_view(0, 0, R.width, R.height);
    if (picture && !g_testPattern) picture(user, 2);
    // RL-NX (R2_CAPTURE_ON_BREAK): judged against the frames around it, so a one-frame flicker is not
    // missed between the cadence's five-second samples. Before the cadence's own readback, which submits.
    break_capture_present(frame, color, R.width, R.height, DkImageFormat_RGBA8_Unorm);
    if (capture_frame()) capture_image(color, R.width, R.height, DkImageFormat_RGBA8_Unorm, "swapchain");
    frame_end();
    const uint64_t t3 = now_ns();
    submit_list(nullptr);  // the frame's own final submit: not a cut (diag_split)
    burst_frame_end(frame);  // RL-NX: the frame's batch submits and GPU waits (gpfifo_entries_recorded)
    check_queue("the present");
    dkQueuePresentImage(R.queue, g_swapchain, slot);
    const uint64_t t4 = now_ns();
    diag_frame_report();
    R.frame = frame;
    R.frameOpen = false;
    g_submits.frames++;
    if (frame == 1) {
        dklog("first present: acquire %.2f ms, record %.2f ms, submit+present %.2f ms; heap never used %zu MiB",
              double(t2 - t1) / 1e6, double(t3 - t2) / 1e6, double(t4 - t3) / 1e6, heap_never_used_mib());
    }
    g_times.acquireNs += t2 - t1 + (acquired ? acquired->acquireNs : 0);
    g_times.recordNs += (t1 - g_frameOpenNs) + (t3 - t2);
    g_times.submitNs += t4 - t3;
    // RL-NX: PERF LOG totals (g_times above is reset every 30 s)
    g_pmDk.presents++;
    g_pmDk.acquireNs += t2 - t1 + (acquired ? acquired->acquireNs : 0);
    g_pmDk.recordNs += (t1 - g_frameOpenNs) + (t3 - t2);
    g_pmDk.submitNs += t4 - t3;
    frame_stats(t4 - g_frameOpenNs);
}

// Debug builds only: with config/deko-capture.flag present, a few chosen frames dump their passes as small
// PPM images to logs/deko-capture/ (end_pass_native, kartpad_deko_present).
// RL-NX: when the last image was written; capture_image sets it (the capture schedule lives on it).
static uint64_t s_captureLastNs = 0;
// RL-NX: the frame the last image came from (R2_CAPTURE_PASS=1 writes every pass of that one frame).
static uint64_t s_captureFrame = ~0ull;
// RL-NX: R2_CAPTURE_PASS=1 writes the pass images as well as the presented one.
static bool capture_pass_images() {
    static const bool on = getenv("R2_CAPTURE_PASS") != nullptr;
    return on;
}
bool capture_frame() {
    // RL-NX: COS_DK_CAPTURE=1 (boot.cfg) turns this on in any build, bounded to the same few frames. It is
    // the only way to see the renderer's output while no swapchain can be created (see init_swapchain);
    // the flag file path and the output folder are this port's, not SFA-NX's.
#ifndef COS_DK_DEBUG_LIB
    static const bool requested = getenv("COS_DK_CAPTURE") != nullptr;
    if (!requested) return false;
#endif
    static const bool enabled = [] {
        mkdir(COS_SWITCH_DATA_DIR "/logs", 0777);
        mkdir(COS_SWITCH_DATA_DIR "/logs/deko-capture", 0777);
        dklog("pixel capture enabled: bounded readbacks at native frames 1, 2, 121, 601, 1801, 3601");
        return true;
    }();
    if (!enabled) return false;
    // RL-NX: keyed on time, not on the frame counter. R.frame only advances in frame_present(), which a
    // run without a swapchain never reaches, so "frame 1" lasted the whole run and every capture came
    // from the first seconds - all of them the fade-in from black. Two short windows, well after the
    // attract screen is up.
    static const uint64_t start = now_ns();
    const uint64_t now = now_ns();
    const double seconds = double(now - start) / 1e9;
    // RL-NX: R2_CAPTURE_FROM=<seconds> starts the captures later than 20 s (the title screen comes after the
    // attract demo, past the 24 images a run gets from 20 s on).
    static const double from = getenv("R2_CAPTURE_FROM") ? atof(getenv("R2_CAPTURE_FROM")) : 20.0;
    // RL-NX (R2_CAPTURE_BURST=<frame>): capture every frame from that native frame for the next nine, instead
    // of one every 5 s. With a frame poison (R2_DEPTH_POISON_AFTER_CLEAR=2) that puts a poisoned and a clean
    // frame side by side in the same scene, which is the only comparison the demo's own timing cannot spoil -
    // every earlier cross-run comparison here compared different frames and proved nothing.
    static const uint64_t burst =
        getenv("R2_CAPTURE_BURST") ? strtoull(getenv("R2_CAPTURE_BURST"), nullptr, 10) : 0;
    if (burst) {
        const uint64_t frame = R.frame + 1;
        return frame >= burst && frame <= burst + 8;
    }
    if (seconds < from) return false;
    // RL-NX: the schedule advances when an image is actually written (capture_image updates
    // s_captureLastNs), not when this function is called. Every caller calls this twice - once to decide,
    // once from inside capture_image - so a debounce here closed the window on the first call and no image
    // was ever written once the game was busy.
    if (!s_captureLastNs) return true;
    // RL-NX: with R2_CAPTURE_PASS=1 the window stays open for the rest of the frame, so one image comes from
    // every pass of that frame (each pass's image is written a few microseconds after the one before it).
    if (capture_pass_images() && R.frame == s_captureFrame) return true;
    return now - s_captureLastNs >= 5'000'000'000ull;
}

// Reads one RGBA8 image back (waiting up to 1 s), logs its statistics and hash, and writes a downscaled
// PPM (at most about 320 pixels wide). At most 12 images per frame plus the final picture.
void capture_image(const DkImageView& view, uint32_t width, uint32_t height,
                   DkImageFormat format, const char* label) {
    if (!capture_frame()) return;
    static uint64_t lastFrame = 0;
    static unsigned count = 0;
    if (lastFrame != R.frame + 1) { lastFrame = R.frame + 1; count = 0; }
    const bool finalImage = !strcmp(label, "swapchain") || !strcmp(label, "snapshot");
    // RL-NX: the presented picture only. A pass readback runs in the middle of the frame's command
    // recording and came back partial once the game drew its attract screen (5350 of the eventual lit
    // pixels), so a pass image says nothing; the presented one is what the player would see.
    // RL-NX: R2_CAPTURE_PASS=1 also writes the pass images (every render target after its pass, the EFB
    // copies and the copy-conversion textures the frame composites from), to tell a pass that drew
    // nothing from one that drew black into a texture the rest of the frame then samples.
    if (!finalImage && !capture_pass_images()) return;
    if (count >= 12 && !finalImage) return;
    // RL-NX: a hard cap. Without a swapchain R.frame never advances, so "frame 1" never ends and the dump
    // ran for every pass of every frame (653 files, and the readbacks cost 12 fps of the run).
    static unsigned s_total = 0;
    if (s_total >= 24) return;
    ++s_total;
    if (!width || !height || width > 1920 || height > 1080) return;
    const bool bgra = format == DkImageFormat_BGRA8_Unorm || format == DkImageFormat_BGRA8_Unorm_sRGB;
    if (!bgra && format != DkImageFormat_RGBA8_Unorm && format != DkImageFormat_RGBA8_Unorm_sRGB) {
        dklog("pixel capture frame %llu: %s skipped format %u", (unsigned long long)lastFrame, label, unsigned(format));
        return;
    }
    const unsigned sequence = count++;
    const uint32_t pitch = (width * 4 + 255) & ~255u;
    // RL-NX: one block, reused. Allocating it per image failed silently once the game drew its attract
    // screen (the frame's own allocations had taken the memory first), so captures stopped with no line in
    // the log; a failure is now said out loud as well.
    static Block readback{};
    static uint32_t readbackSize = 0;
    if (!readback || readbackSize < pitch * height) {
        if (readback) block_free_deferred(readback.block);
        readback = block_create(pitch * height, "diagnostic pixel readback");
        readbackSize = readback ? pitch * height : 0;
        if (!readback) {
            dklog("pixel capture frame %llu: %s cannot allocate %u KiB for the readback", 
                  (unsigned long long)lastFrame, label, (pitch * height) >> 10);
            return;
        }
    }
    memset(readback.cpu, 0xCD, readback.size);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    const DkCopyBuf dst{readback.gpu, pitch, pitch * height};
    const DkImageRect rect{0, 0, 0, width, height, 1};
    dkCmdBufCopyImageToBuffer(R.cmd, &view, &rect, &dst, 0);
    DkFence fence{};
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    dkCmdBufSignalFence(R.cmd, &fence, true);
    submit_readback("diagnostic pixel readback");
    if (dkFenceWait(&fence, 1000000000) != DkResult_Success) {
        dklog("pixel capture frame %llu: %s timed out", (unsigned long long)lastFrame, label);
        block_free_deferred(readback.block);
        return;
    }
    uint64_t hash = 0xCBF29CE484222325ull;
    uint32_t lit = 0, nonOpaque = 0;
    uint8_t lo[4] = {255, 255, 255, 255}, hi[4] = {};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = readback.cpu + y * pitch + x * 4;
            lit += (p[0] | p[1] | p[2]) != 0;
            nonOpaque += p[3] != 255;
            for (unsigned c = 0; c < 4; ++c) {
                lo[c] = std::min(lo[c], p[c]); hi[c] = std::max(hi[c], p[c]);
                hash = (hash ^ p[c]) * 0x100000001B3ull;
            }
        }
    }
    char path[192];
    snprintf(path, sizeof path, COS_SWITCH_DATA_DIR "/logs/deko-capture/frame-%04llu-%02u.ppm",
             (unsigned long long)lastFrame, sequence);
    // RL-NX: R2_CAPTURE_FULL=1 writes the image at its own size instead of 320 pixels wide. A picture
    // that turns out to be a thin strip needs its full resolution to be readable.
    static const bool full = getenv("R2_CAPTURE_FULL") != nullptr;
    const uint32_t scale = full ? 1u : std::max(1u, (width + 319) / 320);
    const bool written = write_ppm(path, readback.cpu, pitch, width, height, bgra, scale);
    dklog("pixel capture frame %llu #%u: %s %ux%u lit=%u/%u nonopaque=%u range=[%u..%u,%u..%u,%u..%u,%u..%u] hash=%016llx file=%s%s",
          (unsigned long long)lastFrame, sequence, label, width, height, lit, width * height, nonOpaque,
          lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], (unsigned long long)hash,
          path, written ? "" : " (write failed)");
    // RL-NX: the readback block is kept and reused (see above), and this is where the capture schedule
    // advances: one image every five seconds.
    s_captureLastNs = now_ns();
    s_captureFrame = R.frame;
}

// COS_DK_* environment switches (diagnostics; none are set in normal SFA-NX runs).
bool env_flag(const char* name, bool def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0';
}

long env_long(const char* name, long def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    char* end = nullptr;
    const long v = strtol(e, &end, 0);
    return end && end != e ? v : def;
}

// Renderer log line ("[dk] ..." on stderr, which lands in logs/log.txt).
void dklog(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n < 0) return;
    fprintf(stderr, "[dk] %s\n", line);
}

void log_flush() { cos_switch_flush_logs(); }

void host_alloc_thread() {
    // The global operator new gives a thread's allocations to the game's current JKRHeap unless the
    // thread is in a host allocation scope: the render worker's and the uam worker's strings, maps
    // and vectors then took blocks from a game heap (NULL when it was full: a crash in the GPU
    // groups' marker names in the Tower of the Gods; dangling when the game freed it). These
    // threads run no game code: host memory for the whole thread.
    static thread_local bool done = false;
    if (!done) {
        done = true;
        JKRPcBeginHostAlloc();
    }
}

// Logs the message, flushes the log to the card and aborts, so Atmosphere writes a crash report.
void fatal(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    fprintf(stderr, "[dk] FATAL: %s\n", line);
    log_flush();
    kartpad_log_flush_sync();  // fflush alone only reaches the asynchronous RAM sink
    abort();  // Atmosphere writes the crash report (backtrace, registers)
}

// Stops with a clear message if an earlier command list faulted the GPU (the queue's error state).
void check_queue(const char* before) {
    if (!dkQueueIsInErrorState(R.queue)) return;
    fatal("the deko3d queue is in an error state (a GPU fault) before %s of frame %llu; deko3d messages %d", before,
          (unsigned long long)R.frame + 1, g_debugMessages);
}

const DkShader* builtin_shader(ShaderId id) { return g_shaderOk[id] ? &g_shaders[id] : nullptr; }

// Viewport and scissor to the same rectangle.
void set_view(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const DkViewport vp = {float(x), float(y), float(w), float(h), 0.0f, 1.0f};
    const DkScissor sc = {x, y, w, h};
    dkCmdBufSetViewports(R.cmd, 0, &vp, 1);
    dkCmdBufSetScissors(R.cmd, 0, &sc, 1);
}

// Fixed-function state for the renderer's own 2D draws: no culling, no depth, optional alpha blending.
void bind_2d_state(bool blend) {
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(R.cmd, &rs);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkColorStateSetBlendEnable(&cs, 0, blend);
    dkCmdBufBindColorState(R.cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(R.cmd, &cw);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    dkBlendStateSetFactors(&bs, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One,
                           DkBlendFactor_InvSrcAlpha);
    dkCmdBufBindBlendStates(R.cmd, 0, &bs, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = false;
    ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(R.cmd, &ds);
}

}  // namespace swdk

using namespace swdk;

// Called once by Aurora when the GPU backend starts: creates the device and queue, the swapchain, the
// shader caches and the shader worker, then shows the boot screen.
extern "C" void aurora_switch_dk_init(uint32_t gxConfigVersion) {
    // RL-NX: once per process. Aurora's initialize() is called once per candidate backend (it walks a
    // preference list and answers false until one works), and the deko seam sits at the *top* of it, so
    // this ran several times - each run creating another device, queue, image heap and uam worker thread.
    // Three "uam worker started" lines and a thread storm that took the emulator down at ~16 s were this.
    // The device, queue, swapchain and caches are process-wide, so the first call owns them.
    static bool s_initialised = false;
    if (s_initialised) return;
    s_initialised = true;
    const uint64_t t0 = now_ns();
    const size_t heapBefore = heap_never_used_mib();
    const AppletType applet = appletGetAppletType();
    dklog("deko3d renderer: %s library; heap never used before the set-up "
          "%zu MiB",
          kDebugLib ? "debug (libdeko3dd: every call checked)" : "release", heapBefore);
    if (applet != AppletType_Application && applet != AppletType_SystemApplication) {
        dklog("WARNING: not in title mode: the deko3d set-up alone needs ~250 MiB, which an applet does not have");
    }
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_message;
    // window origin top left, depth 0 to 1, clip-space y up: WebGPU's conventions (dk.h)
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    dklog("creating the device");
    log_flush();
    R.device = dkDeviceCreate(&dm);
    dklog("device created: flags 0x%X (origin upper left, depth 0 to 1)", dm.flags);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, R.device);
    // zcull off unless COS_DK_ZCULL=1: SwitchWakerHD saw depth-biased decals lose whole 4x8-pixel tiles
    // with it, and the GL path's driver (Mesa nouveau) never enables it (plan section 7.5)
    R.zcull = swdk::env_flag("COS_DK_ZCULL", sfaSwitchMenuIniValue && sfaSwitchMenuIniValue("zcull", 0) != 0);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio |
               (R.zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
    qm.commandMemorySize = kQueueCommandMemory;
    qm.flushThreshold = qm.commandMemorySize / 8;
    dklog("creating the queue");
    log_flush();
    R.queue = dkQueueCreate(&qm);
    dklog("queue created: graphics, command memory %u KiB, zcull %s", qm.commandMemorySize >> 10,
          R.zcull ? "on (menu Z-cull)" : "off (menu Z-cull)");
    dklog("Buffer-write shader data/uniform cache invalidation enabled");

    C.flipY = env_flag("COS_DK_FLIP_Y", false);
    C.flipFront = env_flag("COS_DK_FLIP_FRONT", false);
    C.flipTexture = env_flag("COS_DK_FLIP_TEXTURE", false);
    C.flipPresent = env_flag("COS_DK_FLIP_PRESENT", false);
    dklog("conventions: clip-space y %s (COS_DK_FLIP_Y=%d), Aurora's front face %s (COS_DK_FLIP_FRONT=%d), "
          "texture uploads %s (COS_DK_FLIP_TEXTURE=%d), present %s (COS_DK_FLIP_PRESENT=%d); the plan's defaults "
          "are all 0 (WebGPU's conventions as they are)",
          C.flipY ? "NEGATED by a viewport swizzle" : "as WebGPU's (up)", int(C.flipY),
          C.flipFront ? "INVERTED" : "as it is (CW -> DkFrontFace_CW)", int(C.flipFront),
          C.flipTexture ? "FLIPPED (bottom row first)" : "row 0 first", int(C.flipTexture),
          C.flipPresent ? "FLIPPED vertically" : "upright", int(C.flipPresent));

    memory_init();
    load_builtin_shaders();
    window_wanted(R.width, R.height);  // the first swapchain at the current mode's size
    init_swapchain();
    g_testPattern = pattern_enabled();
    dklog("present: %s", g_testPattern ? "the TEST PATTERN (COS_DK_TEST_PATTERN=1) instead of the game"
                                       : "the game's picture (the EFB resampled, fitted); "
                                         "COS_DK_TEST_PATTERN=1 shows the test pattern instead");
    shader_cache_init(gxConfigVersion);
    shaders_init(gxConfigVersion);
    // The boot screen from the first frame on; Aurora's shader preload fills its bar. It uses the
    // embedded DKSH only, no Dawn, Tint or uam.
    // The title names the game (upper case: the boot font has no lower-case glyphs). 20 characters at
    // scale 6 is about 485 px wide, inside the 720p window.
    // RL-NX: was "STARFOX ADVENTURES LOADING", left over from the SFA-NX layer this came from.
    aurora_switch_dk_boot_screen("ROGUE LEADER LOADING", -1.0f, nullptr, nullptr);
    dklog("boot screen submitted using embedded native shaders");
    g_heapAfterSetup = heap_never_used_mib();
    dklog("set-up cost: %zu MiB of heap (never used: %zu MiB before, %zu MiB after), %.0f ms",
          heapBefore > g_heapAfterSetup ? heapBefore - g_heapAfterSetup : 0, heapBefore, g_heapAfterSetup,
          double(now_ns() - t0) / 1e6);
    log_flush();
}

namespace {

// The loading screen shown while Aurora preloads shaders: a title, an optional progress bar
// (fraction < 0 hides it), a detail line and a hint line.
struct BootScreen {
    const char* title;
    float fraction;
    const char* detail;
    const char* hint;
};

// centred text of one line, `scale` window pixels per font pixel
void draw_centred(const char* text, int top, int scale, const float fg[4], const float bg[4]) {
    if (!text || !*text) return;
    const int width = (int(strlen(text)) * 4 + 1) * scale;
    draw_text(std::max(0, (int(R.width) - width) / 2), top, scale, &text, 1, fg, bg);
}

void draw_boot_screen(void* user, int stage) {
    if (stage != 1) return;
    const BootScreen& s = *static_cast<const BootScreen*>(user);
    static const float kBackground[] = {0.025f, 0.035f, 0.05f, 1.0f};
    static const float kText[] = {0.9f, 0.95f, 1.0f, 1.0f};
    static const float kDim[] = {0.5f, 0.55f, 0.6f, 1.0f};
    static const float kTrack[] = {0.12f, 0.14f, 0.18f, 1.0f};
    static const float kFill[] = {0.35f, 0.75f, 1.0f, 1.0f};
    draw_rect(0, 0, int(R.width), int(R.height), kBackground);
    const int h = int(R.height);
    draw_centred(s.title, h * 36 / 100, 6, kText, kBackground);
    if (s.fraction >= 0.0f) {
        const int width = int(R.width) * 3 / 4, height = std::max(8, h / 30);
        const int left = (int(R.width) - width) / 2, top = h / 2;
        draw_rect(left, top, width, height, kTrack);
        draw_rect(left, top, int(float(width) * std::min(s.fraction, 1.0f)), height, kFill);
    }
    draw_centred(s.detail, h * 57 / 100, 4, kText, kBackground);
    draw_centred(s.hint, h * 88 / 100, 3, kDim, kBackground);
}

}  // namespace

// Draws and presents one boot screen frame (Aurora calls it repeatedly during the preload).
extern "C" void aurora_switch_dk_boot_screen(const char* title, float fraction, const char* detail, const char* hint) {
    BootScreen s{title, fraction, detail, hint};
    frame_present(draw_boot_screen, &s);
}

// Lets uam compile at higher priority while nothing else needs the CPU (the boot preload).
extern "C" void aurora_switch_dk_compile_boost(bool on) { shader_compile_boost(on); }

// At exit: waits for the GPU and releases the swapchain so the window's buffers go back to the system.
extern "C" void aurora_switch_dk_shutdown(void) {
    if (R.queue == nullptr) return;
    dkQueueWaitIdle(R.queue);
    shaders_shutdown();  // the uam worker must not outlive the process image (Restart returns to hbloader)
    if (g_swapchain != nullptr) {
        dkSwapchainDestroy(g_swapchain);  // gives the window's buffers back (nwindowReleaseBuffers)
        g_swapchain = nullptr;
    }
    dklog("shut down after %llu frames", (unsigned long long)R.frame);
}

// Restart (vi_shim.c), after aurora_shutdown has released Aurora's textures and buffers: frees all GPU
// memory, then the queue and the device, like MKDD-NX's gfx_close. Memory still locked to the GPU would
// stop hbloader mapping the next NRO (2001-0106 inside hbl). The normal quit ends the process instead and
// doesn't need this.
extern "C" void aurora_switch_dk_release_all(void) {
    if (R.device == nullptr) return;
    if (R.queue != nullptr) dkQueueWaitIdle(R.queue);
    const uint32_t blocks = memory_release_all();
    if (R.queue != nullptr) {
        dkQueueDestroy(R.queue);
        R.queue = nullptr;
    }
    dkDeviceDestroy(R.device);
    R.device = nullptr;
    dklog("released for restart: %u memory blocks, queue and device", blocks);
}

// The - menu's Docked Resolution changed (0 720p, 1 900p, 2 1080p): the swapchain follows at the next
// present (window_wanted) and Aurora's frame at its next event poll (window.cpp on Switch).
extern "C" void aurora_switch_set_docked_res(int value) {
    g_dockedRes.store(value < 0 ? 0 : value > 2 ? 2 : value, std::memory_order_relaxed);
}

// Aurora's window size on Switch (extern/aurora/lib/window.cpp get_window_size): when docked with a Docked
// Resolution above 720p, the output (Aurora's surface, what it resamples its frame to) is the 1920x1080
// swapchain and the frame is rendered at 1600x900 or 1920x1080. Returns 0 otherwise, and Aurora keeps
// the SDL window's 1280x720 as before. Called on the main thread; reads the operation mode without
// window_wanted's logging (that state belongs to the presenting thread).
extern "C" int aurora_switch_frame_size(int* outW, int* outH, int* renderW, int* renderH) {
    const int res = docked_res();
    if (res == 0 || window_config().mode == 2 || appletGetOperationMode() != AppletOperationMode_Console) {
        return 0;
    }
    *outW = 1920;
    *outH = 1080;
    *renderW = res == 1 ? 1600 : 1920;
    *renderH = res == 1 ? 900 : 1080;
    return 1;
}

// RL-NX: PERF LOG (switch/src/perfmon_switch.cpp): deko3d's totals since start. Read from the game thread
// once a second; the render worker writes them, and whole 64-bit values are all a statistic needs.
void rlnx_dk_shader_totals(RlPmDkTotals* out);  // shaders.cpp
extern "C" void rlnx_dk_totals(RlPmDkTotals* out) {
    *out = swdk::g_pmDk;
    out->submits = swdk::g_submits.submits;
    rlnx_dk_shader_totals(out);
}
