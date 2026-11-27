/* RL-NX: performance monitor counters (PERF_HANDOFF 10.x, "Step 1: log everything").
 *
 * One set of plain counters that every layer adds to - the recompiled code's slow memory path, the GX
 * bridge, the HLE DSP, disc reads, audio, deko3d - and one reporter (switch/src/perfmon_switch.cpp) that
 * turns them into a "[pm]" line every second and a "[hitch]" line for each slow game frame, so a hardware
 * run says where every millisecond of a frame went without a second build.
 *
 * Rules that keep it cheap enough to leave on:
 *   - Frequent events (a slow guest memory access, a quantized paired-single op, a draw) are counted,
 *     never timed: one add to a global, no clock read. The 1 kHz sampler (prof_switch.cpp) is what
 *     attributes time inside those paths.
 *   - Rare events (an idle wait, a DSP mail, a disc read, an audio submit, the frame hand-off) are timed
 *     with the ARM system counter (one register read each side).
 *   - Each field has one writer thread. Fields are 64-bit and aligned, so the reporter's plain reads see
 *     whole values; a count that is a frame late is fine for a statistic.
 *
 * C header: included by the Clang-built runtime (C++), the GCC-built Switch layer and the deko3d layer. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timed spans on the game thread, in ARM system counter ticks (19.2 MHz on the Switch; 0 elsewhere). */
enum RlPmTimer {
    RLPM_IDLE = 0,   /* events.cpp idleWait: the guest has nothing to run until the next event */
    RLPM_DSP,        /* musyx_dsp.cpp: one HLE DSP mail (MusyX command list / mixing) */
    RLPM_DISC,       /* main.cpp discRead: reading the disc image (DVD DMA, file loads) */
    RLPM_AUDIO,      /* audio_switch.cpp: handing a buffer to audout, on the game thread */
    RLPM_LC_DMA,     /* cpu.cpp lockedCacheDma: locked cache <-> RAM block moves */
    RLPM_GXSYNC,     /* RL-NX (9.53) gx_bridge.cpp: the game thread waiting for the bridge worker to catch up */
    RLPM_TIMER_COUNT
};

/* GX / IO register blocks for the slow-path access counters: 0xCC00x000 by its x digit (0 CP, 1 PE, 2 VI,
 * 3 PI, 4 MI, 5 DSP, 6 DI/SI/EXI/AI, 8 the write-gather pipe), 14 = the EFB window 0xC8000000 (EFB peeks
 * and pokes), 15 = anything else in the IO range. */
enum { RLPM_IO_BLOCKS = 16, RLPM_IO_EFB = 14, RLPM_IO_OTHER = 15 };

/* RL-NX (audit 2026-11-27): who the game thread was waiting for. Everything but RLPM_DRAIN_FIFO adds up to
 * RLPM_GXSYNC; RLPM_DRAIN_FIFO is the wait for Aurora's FIFO thread after gpSync/gpDrainAll and is not in it. */
enum {
    RLPM_DRAIN_GPSYNC = 0, /* gpSync: a token / draw-done / PE barrier, then the bridge worker catches up */
    RLPM_DRAIN_GPALL,      /* gpDrainAll: everything queued has run (GP thread sync) */
    RLPM_DRAIN_CPREAD,     /* a CP register read in GP RAW mode (gx_fifo.cpp s_gpDrainBridge) */
    RLPM_DRAIN_RINGFULL,   /* ringPush: the ring was full, waiting for the worker to free space */
    RLPM_DRAIN_BIGCMD,     /* ringPush: a command over half the ring ran inline after a drain */
    RLPM_DRAIN_MENU,       /* bridgeApplyWanted: a menu switch applied at a vblank */
    RLPM_DRAIN_STATE,      /* state the worker owns (tiling, resets, readbacks, any other bridgeDrain caller) */
    RLPM_DRAIN_VBLANK,     /* vblank(): the per-frame drain before input, hotkeys and window events (ASYNC VBLANK skips it) */
    RLPM_DRAIN_FIFO,       /* fifo::drain(): waiting for Aurora's FIFO thread (not part of RLPM_GXSYNC) */
    RLPM_DRAIN_N
};

typedef struct RlPmCounters {
    uint64_t drainTicks[RLPM_DRAIN_N], drainCalls[RLPM_DRAIN_N]; /* RLPM_DRAIN_* */
    /* The guest's VM region (vm.cpp): the numbers that decide a fixed-window fastmem design. */
    uint64_t vmFill;               /* vmMiss calls: walked the page table and refilled the soft TLB */
    uint64_t vmFault;              /* ...of which the page was absent and the game's DSI handler (pager) ran */
    uint64_t vmFaultTicks;         /* time spent in that handler */
    uint64_t vmFlush;              /* whole-TLB flushes (mtsr, SDR1, state load) */
    uint64_t vmTlbie;              /* single-page invalidations (tlbie) */
    uint64_t vmPagesFrame, vmPagesFrameMax; /* distinct VM pages missed in a frame: sum over frames, worst frame */
    uint64_t vmPagesTotal;         /* distinct VM pages missed since start (working-set size) */
    /* -- game thread (the recompiled code and everything it calls) -- */
    uint64_t timerTicks[RLPM_TIMER_COUNT];
    uint64_t timerCalls[RLPM_TIMER_COUNT];
    uint64_t ioRd[RLPM_IO_BLOCKS], ioWr[RLPM_IO_BLOCKS]; /* rdSlow / wrSlow reaching an IO register */
    uint64_t vmRd, vmWr;           /* the guest's VM (page-table) region */
    uint64_t lcRd, lcWr;           /* the locked cache (0xE0000000) through the slow path */
    uint64_t otherRd, otherWr;     /* any other address the fast arena test sent to the slow path */
    uint64_t psqLdQ, psqStQ;       /* quantized paired-single loads / stores (cpu.cpp psqLoadQ / psqStoreQ) */
    uint64_t discBytes;            /* bytes read from the disc image */
    uint64_t lcDmaBytes;           /* bytes moved by locked cache DMA */
    uint64_t draws;                /* GX draws the bridge sent to Aurora */
    uint64_t copyTex, copyTexPx;   /* EFB copies to a texture, and their pixels */
    uint64_t copyXfb, copyXfbPx;   /* EFB copies to the XFB (display copies) */
    uint64_t texNew, texNewBytes;  /* textures whose contents were (re)hashed and sent: new or changed */
    uint64_t arrayBytes;           /* vertex array bytes sent to Aurora */
    /* Aurora, read once per frame after aurora_end_frame (gx_bridge.cpp endFrame) */
    uint64_t auDraws, auMerged;    /* draws Aurora recorded, and draws merged into others */
    uint64_t auSkipped, auUber;    /* since start: skipped (pipeline not built) / drawn with the ubershader */
    uint64_t auQueued, auCreated;  /* pipelines waiting to be built / built since start (latest values) */
    uint64_t auTexUpload;          /* texture upload bytes */
    uint64_t level;                /* the game's level number (the byte the 30 s perf line shows) */
} RlPmCounters;

/* deko3d's totals since start (switch/deko/device.cpp, render worker; shaders.cpp, compile worker). */
typedef struct RlPmDkTotals {
    uint64_t presents;
    uint64_t fenceNs, acquireNs, recordNs, submitNs; /* render worker: waits for the frame fence, swapchain
                                                         image, recording, submit + present */
    uint64_t submits;                                /* queue submits (GPFIFO kickoffs follow from these) */
    uint64_t burstWaits, burstWaitNs;                /* R2_GPU_BURST_WAIT: GPU idle waits inside a frame */
    uint64_t shadersCompiled, shaderCompileNs;       /* uam compiles on the console */
    uint64_t shadersWaiting;                         /* queued or compiling now (latest value) */
} RlPmDkTotals;

extern RlPmCounters rl_pm;

/* The ARM system counter (cntpct_el0, the one libnx armGetSystemTick reads). 0 on hosts without one: the
 * timers then read 0 and say so.
 * RL-NX: NOT cntvct_el0 - Horizon traps EL0 reads of the virtual counter (build 19:49 died with a 2168-0002
 * data abort on the first idleWait); Ryujinx allows both, so only hardware shows it. */
static inline uint64_t rlPmTick(void) {
#if defined(__aarch64__)
    uint64_t t;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
#else
    return 0;
#endif
}
static inline void rlPmAddTimer(int timer, uint64_t t0) {
    rl_pm.timerTicks[timer] += rlPmTick() - t0;
    rl_pm.timerCalls[timer]++;
}
/* The IO block index of a slow-path IO address (see RLPM_IO_BLOCKS). */
static inline unsigned rlPmIoBlock(uint32_t a) {
    if ((a >> 24) == 0xC8u) return RLPM_IO_EFB;
    if ((a >> 16) == 0xCC00u) return (a >> 12) & 0xFu;
    return RLPM_IO_OTHER;
}

/* Switch reporter (perfmon_switch.cpp); called only by the Switch layer (the desktop build just counts). */
void rlPmFrame(uint64_t gameNs, uint64_t presentNs); /* prof_switch.cpp rlProfMark: a game frame closed */
void rlPmVmFrame(void);                              /* vm.cpp: closes the frame's distinct-page count */
void rlPmSetEnabled(int on);                         /* the - menu's PERF LOG row */
int rlPmEnabled(void);

#ifdef __cplusplus
}
/* C++: times the enclosing scope into one RlPmTimer. */
struct RlPmScope {
    int timer;
    uint64_t t0;
    explicit RlPmScope(int t) : timer(t), t0(rlPmTick()) {}
    ~RlPmScope() { rlPmAddTimer(timer, t0); }
};
#endif
