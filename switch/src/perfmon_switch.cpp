// RL-NX: the PERF LOG reporter (PERF_HANDOFF 10.x, "Step 1: log everything"). Switched from the - menu's
// PERF LOG row (on by default; boot.cfg R2_PERFLOG=0 starts it off).
//
// Every game frame (prof_switch.cpp rlProfMark calls rlPmFrame with the frame's game and present times)
// it adds the frame to a one-second window. A frame over kHitchNs gets its own "[hitch]" line with what
// that one frame spent (counter deltas since the previous frame), so slow frames are not averaged away.
// Each second it writes five lines, all key=value so switch/tools/perf_report.py can read them:
//
//   [pm]     frame rate, game / present time (avg/max ms), frame-time histogram, the game thread's timed
//            spans per frame (idle, HLE DSP, disc, audio submits, locked cache DMA), level, dock mode,
//            and this reporter's own cost
//   [pm-mem] guest memory accesses that left the inline arena path, per frame: IO writes and reads by
//            register block (gather pipe, CP, PE, VI, PI, DSP, DI/SI/EXI/AI), EFB peeks/pokes, the VM
//            region, the locked cache, anything else; quantized paired-single loads/stores
//   [pm-gx]  draws, Aurora's draws/merges, draws skipped or drawn with the ubershader while pipelines
//            build, pipelines queued/created, texture re-sends and upload KiB, vertex array KiB, EFB copies
//   [pm-dk]  deko3d per present: frame fence wait, acquire, recording, submit; submits per present,
//            GPU burst waits, uam shader compiles (count, ms) and how many are waiting
//   [pm-thr] CPU busy % of each thread the port placed (game, Aurora FIFO, render worker, audio, the
//            pipeline workers), from the kernel's per-thread tick count
//
// Everything is read from plain counters (src/runtime/perfmon.h); nothing here takes a lock the game
// thread could wait on, and the lines go through the async log writer.
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "perfmon.h"

extern "C" void pc_log_line(const char* fmt, ...);
extern "C" int rlnx_perf_threads(Handle* handles, const char** names, int max);  // perf_switch.cpp
extern "C" void rlnx_dk_totals(RlPmDkTotals* out);                               // deko/device.cpp

// The headless build has no deko3d layer: its totals read 0.
extern "C" __attribute__((weak)) void rlnx_dk_totals(RlPmDkTotals* out) { std::memset(out, 0, sizeof *out); }

namespace {

constexpr uint64_t kWindowNs = 1000000000ull;  // one [pm] set per second
constexpr uint64_t kHitchNs = 50000000ull;     // a frame over 50 ms (3 vsyncs) gets a [hitch] line
constexpr int kHitchLinesPerWindow = 6;        // more are counted, not printed
constexpr int kThreads = 12;

std::atomic<bool> g_on{true};
bool g_envRead = false;

// Window state (game thread only).
RlPmCounters g_winStart, g_prevFrame;
RlPmDkTotals g_dkStart, g_dkPrevFrame;
uint64_t g_winStartNs = 0, g_frames = 0, g_gameSum = 0, g_gameMax = 0, g_presentSum = 0, g_presentMax = 0;
uint64_t g_hist[4] = {};  // frames up to 16.7 ms, 33.3 ms, 50 ms, longer
uint64_t g_hitches = 0, g_hitchesPrinted = 0, g_costTicks = 0, g_frameNo = 0;
Handle g_thrHandle[kThreads];
const char* g_thrName[kThreads];
uint64_t g_thrStart[kThreads];
int g_thrCount = 0;
double g_sessionStartS = -1;

uint64_t now_ns() { return armTicksToNs(armGetSystemTick()); }
double ticks_ms(uint64_t t) { return double(armTicksToNs(t)) / 1e6; }

// CPU ticks a thread has run (all cores). InfoType_ThreadTickCount is 13.0.0+; older firmware has the
// deprecated id.
uint64_t thread_ticks(Handle h) {
    u64 t = 0;
    if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h, u64(-1))) &&
        R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, h, u64(-1))))
        return 0;
    return t;
}

void threads_snapshot() {
    g_thrCount = rlnx_perf_threads(g_thrHandle, g_thrName, kThreads);
    for (int i = 0; i < g_thrCount; ++i) g_thrStart[i] = thread_ticks(g_thrHandle[i]);
}

const char* mode_name() { return appletGetOperationMode() == AppletOperationMode_Console ? "dock" : "hh"; }

uint64_t io_sum(const uint64_t* now, const uint64_t* then) {
    uint64_t n = 0;
    for (int i = 0; i < RLPM_IO_BLOCKS; ++i) n += now[i] - then[i];
    return n;
}

// The slow frame just closed: what it alone spent, from the counters' change since the previous frame.
void hitch_line(uint64_t gameNs, uint64_t presentNs, const RlPmCounters& c, const RlPmDkTotals& d) {
    const RlPmCounters& p = g_prevFrame;
    const RlPmDkTotals& q = g_dkPrevFrame;
    pc_log_line("[hitch] frame=%llu ms=%.1f game=%.1f present=%.1f idle=%.1f dsp=%.1f disc=%.1f disc_kib=%llu "
                "audio=%.1f lcdma=%.1f pipe_wr=%llu io=%llu efb_rd=%llu lc=%llu psq_q=%llu draws=%llu copies=%llu "
                "tex_new=%llu tex_up_kib=%llu skipped=%llu uber=%llu shaders=%llu fence=%.1f burst_waits=%llu "
                "burst_ms=%.1f gxsync=%.1f lvl=%llu "
                "dr_gpsync=%.1f dr_gpall=%.1f dr_cpread=%.1f dr_ring=%.1f dr_state=%.1f dr_fifo=%.1f vm_fill=%llu "
                "vm_fault=%llu vm_fault_ms=%.2f",
                (unsigned long long)g_frameNo, double(gameNs + presentNs) / 1e6, double(gameNs) / 1e6,
                double(presentNs) / 1e6, ticks_ms(c.timerTicks[RLPM_IDLE] - p.timerTicks[RLPM_IDLE]),
                ticks_ms(c.timerTicks[RLPM_DSP] - p.timerTicks[RLPM_DSP]),
                ticks_ms(c.timerTicks[RLPM_DISC] - p.timerTicks[RLPM_DISC]),
                (unsigned long long)((c.discBytes - p.discBytes) >> 10),
                ticks_ms(c.timerTicks[RLPM_AUDIO] - p.timerTicks[RLPM_AUDIO]),
                ticks_ms(c.timerTicks[RLPM_LC_DMA] - p.timerTicks[RLPM_LC_DMA]),
                (unsigned long long)(c.ioWr[8] - p.ioWr[8]),
                (unsigned long long)(io_sum(c.ioWr, p.ioWr) + io_sum(c.ioRd, p.ioRd)),
                (unsigned long long)(c.ioRd[RLPM_IO_EFB] - p.ioRd[RLPM_IO_EFB]),
                (unsigned long long)(c.lcRd - p.lcRd + c.lcWr - p.lcWr),
                (unsigned long long)(c.psqLdQ - p.psqLdQ + c.psqStQ - p.psqStQ), (unsigned long long)(c.draws - p.draws),
                (unsigned long long)(c.copyTex - p.copyTex + c.copyXfb - p.copyXfb),
                (unsigned long long)(c.texNew - p.texNew), (unsigned long long)((c.auTexUpload - p.auTexUpload) >> 10),
                (unsigned long long)(c.auSkipped - p.auSkipped), (unsigned long long)(c.auUber - p.auUber),
                (unsigned long long)(d.shadersCompiled - q.shadersCompiled), double(d.fenceNs - q.fenceNs) / 1e6,
                (unsigned long long)(d.burstWaits - q.burstWaits), double(d.burstWaitNs - q.burstWaitNs) / 1e6,
                ticks_ms(c.timerTicks[RLPM_GXSYNC] - p.timerTicks[RLPM_GXSYNC]),  // RL-NX (9.53)
                (unsigned long long)c.level,
                // RL-NX (audit 2026-11-27): the drain by cause, and the VM region's soft fills / pager runs
                ticks_ms(c.drainTicks[RLPM_DRAIN_GPSYNC] - p.drainTicks[RLPM_DRAIN_GPSYNC]),
                ticks_ms(c.drainTicks[RLPM_DRAIN_GPALL] - p.drainTicks[RLPM_DRAIN_GPALL]),
                ticks_ms(c.drainTicks[RLPM_DRAIN_CPREAD] - p.drainTicks[RLPM_DRAIN_CPREAD]),
                ticks_ms(c.drainTicks[RLPM_DRAIN_RINGFULL] - p.drainTicks[RLPM_DRAIN_RINGFULL]),
                ticks_ms(c.drainTicks[RLPM_DRAIN_STATE] - p.drainTicks[RLPM_DRAIN_STATE]),
                ticks_ms(c.drainTicks[RLPM_DRAIN_FIFO] - p.drainTicks[RLPM_DRAIN_FIFO]),
                (unsigned long long)(c.vmFill - p.vmFill), (unsigned long long)(c.vmFault - p.vmFault),
                ticks_ms(c.vmFaultTicks - p.vmFaultTicks));
}

// The one-second lines (see the top of the file).
void window_lines(uint64_t nowNs, const RlPmCounters& c, const RlPmDkTotals& d) {
    const RlPmCounters& s = g_winStart;
    const RlPmDkTotals& k = g_dkStart;
    const double secs = double(nowNs - g_winStartNs) / 1e9;
    const double f = double(g_frames ? g_frames : 1);
    const double t = double(nowNs) / 1e9 - g_sessionStartS;
    auto perF = [&](uint64_t now, uint64_t then) { return double(now - then) / f; };
    auto msF = [&](int timer) { return ticks_ms(c.timerTicks[timer] - s.timerTicks[timer]) / f; };
    pc_log_line("[pm] t=%.1f lvl=%llu mode=%s fps=%.1f frames=%llu game=%.2f/%.2f present=%.2f/%.2f h16=%llu "
                "h33=%llu h50=%llu hx=%llu hitches=%llu idle=%.2f dsp=%.2f dsp_calls=%.1f disc=%.2f disc_kib=%.0f "
                "audio=%.2f lcdma=%.2f lcdma_kib=%.1f gxsync=%.2f gxsync_calls=%.1f pm_us=%.1f",
                t, (unsigned long long)c.level, mode_name(), double(g_frames) / secs, (unsigned long long)g_frames,
                double(g_gameSum) / 1e6 / f, double(g_gameMax) / 1e6, double(g_presentSum) / 1e6 / f,
                double(g_presentMax) / 1e6, (unsigned long long)g_hist[0], (unsigned long long)g_hist[1],
                (unsigned long long)g_hist[2], (unsigned long long)g_hist[3], (unsigned long long)g_hitches,
                msF(RLPM_IDLE), msF(RLPM_DSP), perF(c.timerCalls[RLPM_DSP], s.timerCalls[RLPM_DSP]), msF(RLPM_DISC),
                double(c.discBytes - s.discBytes) / 1024.0, msF(RLPM_AUDIO), msF(RLPM_LC_DMA),
                double(c.lcDmaBytes - s.lcDmaBytes) / 1024.0 / f,
                msF(RLPM_GXSYNC), perF(c.timerCalls[RLPM_GXSYNC], s.timerCalls[RLPM_GXSYNC]),  // RL-NX (9.53)
                double(armTicksToNs(g_costTicks)) / 1e3 / f);
    // RL-NX (audit 2026-11-27): the gxsync wait by cause: ms per game frame / calls per game frame. gpsync, gpall,
    // cpread, ring, bigcmd, menu and state add up to the gxsync= field of [pm]; fifo is the wait for Aurora's FIFO
    // thread and is extra.
    {
        static const char* const kN[RLPM_DRAIN_N] = {"gpsync", "gpall", "cpread", "ring", "bigcmd", "menu", "state", "vblank", "fifo"};
        char b[300];
        int n = snprintf(b, sizeof b, "[pm-drain] t=%.1f", t);
        for (int i = 0; i < RLPM_DRAIN_N; ++i)
            n += snprintf(b + n, sizeof b - n, " %s=%.2f %s_n=%.2f", kN[i],
                          ticks_ms(c.drainTicks[i] - s.drainTicks[i]) / f, kN[i], perF(c.drainCalls[i], s.drainCalls[i]));
        pc_log_line("%s", b);
    }
    // RL-NX (audit 2026-11-27): the guest VM region. fill = soft-TLB refills per frame, fault = pager runs per frame
    // (pages the game had to bring in), fault_ms = their cost, pages = distinct 4 KiB pages missed per
    // frame (average), pages_max = the worst frame since start, pages_total = working set so far, flush/tlbie = invalidations per frame.
    pc_log_line("[pm-vm] t=%.1f fill=%.1f fault=%.2f fault_ms=%.3f pages=%.1f pages_max=%llu pages_total=%llu flush=%.2f "
                "tlbie=%.2f",
                t, perF(c.vmFill, s.vmFill), perF(c.vmFault, s.vmFault), ticks_ms(c.vmFaultTicks - s.vmFaultTicks) / f,
                perF(c.vmPagesFrame, s.vmPagesFrame), (unsigned long long)c.vmPagesFrameMax,
                (unsigned long long)c.vmPagesTotal, perF(c.vmFlush, s.vmFlush), perF(c.vmTlbie, s.vmTlbie));
    pc_log_line("[pm-mem] t=%.1f pipe_wr=%.0f cp_wr=%.0f pe_wr=%.0f vi_wr=%.0f pi_wr=%.0f mi_wr=%.0f dsp_wr=%.0f "
                "dsp_rd=%.0f exi_rw=%.0f cp_rd=%.0f pe_rd=%.0f pi_rd=%.0f vi_rd=%.0f efb_rd=%.0f efb_wr=%.0f "
                "io_other=%.0f vm_rd=%.0f vm_wr=%.0f lc_rd=%.0f lc_wr=%.0f other_rd=%.0f other_wr=%.0f psq_ld_q=%.0f "
                "psq_st_q=%.0f",
                t, perF(c.ioWr[8], s.ioWr[8]), perF(c.ioWr[0], s.ioWr[0]), perF(c.ioWr[1], s.ioWr[1]),
                perF(c.ioWr[2], s.ioWr[2]), perF(c.ioWr[3], s.ioWr[3]), perF(c.ioWr[4], s.ioWr[4]),
                perF(c.ioWr[5], s.ioWr[5]), perF(c.ioRd[5], s.ioRd[5]),
                perF(c.ioWr[6] + c.ioRd[6], s.ioWr[6] + s.ioRd[6]), perF(c.ioRd[0], s.ioRd[0]),
                perF(c.ioRd[1], s.ioRd[1]), perF(c.ioRd[3], s.ioRd[3]), perF(c.ioRd[2], s.ioRd[2]),
                perF(c.ioRd[RLPM_IO_EFB], s.ioRd[RLPM_IO_EFB]), perF(c.ioWr[RLPM_IO_EFB], s.ioWr[RLPM_IO_EFB]),
                perF(c.ioRd[RLPM_IO_OTHER] + c.ioWr[RLPM_IO_OTHER], s.ioRd[RLPM_IO_OTHER] + s.ioWr[RLPM_IO_OTHER]),
                perF(c.vmRd, s.vmRd), perF(c.vmWr, s.vmWr), perF(c.lcRd, s.lcRd), perF(c.lcWr, s.lcWr),
                perF(c.otherRd, s.otherRd), perF(c.otherWr, s.otherWr), perF(c.psqLdQ, s.psqLdQ),
                perF(c.psqStQ, s.psqStQ));
    pc_log_line("[pm-gx] t=%.1f draws=%.0f au_draws=%.0f au_merged=%.0f skipped=%llu uber=%llu queued=%llu "
                "created=%llu tex_new=%.1f tex_up_kib=%.1f arrays_kib=%.1f copy_tex=%.1f copy_tex_kpx=%.1f "
                "copy_xfb=%.1f copy_xfb_kpx=%.1f",
                t, perF(c.draws, s.draws), perF(c.auDraws, s.auDraws), perF(c.auMerged, s.auMerged),
                (unsigned long long)(c.auSkipped - s.auSkipped), (unsigned long long)(c.auUber - s.auUber),
                (unsigned long long)c.auQueued, (unsigned long long)c.auCreated, perF(c.texNew, s.texNew),
                perF(c.auTexUpload, s.auTexUpload) / 1024.0, perF(c.arrayBytes, s.arrayBytes) / 1024.0,
                perF(c.copyTex, s.copyTex), perF(c.copyTexPx, s.copyTexPx) / 1000.0, perF(c.copyXfb, s.copyXfb),
                perF(c.copyXfbPx, s.copyXfbPx) / 1000.0);
    const double pr = double(d.presents - k.presents ? d.presents - k.presents : 1);
    pc_log_line("[pm-dk] t=%.1f presents=%llu fence=%.2f acquire=%.2f record=%.2f submit=%.2f submits=%.1f "
                "burst_waits=%llu burst_ms=%.1f shaders=%llu shader_ms=%.0f shaders_waiting=%llu",
                t, (unsigned long long)(d.presents - k.presents), double(d.fenceNs - k.fenceNs) / 1e6 / pr,
                double(d.acquireNs - k.acquireNs) / 1e6 / pr, double(d.recordNs - k.recordNs) / 1e6 / pr,
                double(d.submitNs - k.submitNs) / 1e6 / pr, double(d.submits - k.submits) / pr,
                (unsigned long long)(d.burstWaits - k.burstWaits), double(d.burstWaitNs - k.burstWaitNs) / 1e6,
                (unsigned long long)(d.shadersCompiled - k.shadersCompiled),
                double(d.shaderCompileNs - k.shaderCompileNs) / 1e6, (unsigned long long)d.shadersWaiting);
    // Thread CPU: ticks run / ticks elapsed. Names are short and space-free for the parser.
    char buf[400];
    int len = snprintf(buf, sizeof buf, "[pm-thr] t=%.1f", t);
    const double winTicks = double(armNsToTicks(nowNs - g_winStartNs));
    for (int i = 0; i < g_thrCount && len < int(sizeof buf) - 40; ++i) {
        const uint64_t now = thread_ticks(g_thrHandle[i]);
        char name[24];
        snprintf(name, sizeof name, "%s", g_thrName[i]);
        for (char* q = name; *q; ++q)
            if (*q == ' ') *q = '_';
        len += snprintf(buf + len, sizeof buf - len, " %s=%.0f", name, 100.0 * double(now - g_thrStart[i]) / winTicks);
    }
    pc_log_line("%s", buf);
}

void window_reset(uint64_t nowNs, const RlPmCounters& c, const RlPmDkTotals& d) {
    g_winStart = c;
    g_dkStart = d;
    g_winStartNs = nowNs;
    g_frames = g_gameSum = g_gameMax = g_presentSum = g_presentMax = 0;
    std::memset(g_hist, 0, sizeof g_hist);
    g_hitches = g_hitchesPrinted = g_costTicks = 0;
    threads_snapshot();
}

}  // namespace

// The - menu's GPU TIMING row: switch/deko/gpu_timing.cpp's per-pass GPU timestamps (one sampled frame in
// 120, "gpu time #N" lines; every 5th sample also times each draw, "gpu draws #N"). Off by default: the
// draw probe frames are slower than the others. deko_glue_switch.cpp's sfaProfEnabled reads it.
std::atomic<bool> g_gpuTiming{false};
extern "C" int rlnxGpuTimingEnabled(void) { return g_gpuTiming.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" void rlnxGpuTimingSet(int on) {
    g_gpuTiming.store(on != 0, std::memory_order_relaxed);
    pc_log_line("[pm] GPU TIMING %s (gpu time / gpu draws lines)", on ? "on" : "off");
}

extern "C" void rlPmSetEnabled(int on) {
    g_on.store(on != 0, std::memory_order_relaxed);
    g_winStartNs = 0;  // the next frame starts a fresh window
    pc_log_line("[pm] PERF LOG %s", on ? "on" : "off");
}

extern "C" int rlPmEnabled(void) {
    if (!g_envRead) {  // boot.cfg is in the environment by the first frame (menu_switch.cpp has the same note)
        g_envRead = true;
        const char* e = getenv("R2_PERFLOG");
        if (e != nullptr && *e == '0') g_on.store(false, std::memory_order_relaxed);
    }
    return g_on.load(std::memory_order_relaxed) ? 1 : 0;
}

// Game thread, once per game frame (prof_switch.cpp rlProfMark, at the next frame's start mark).
extern "C" void rlPmFrame(uint64_t gameNs, uint64_t presentNs) {
    if (!rlPmEnabled()) return;
    rlPmVmFrame();  // RL-NX (audit 2026-11-27): close this frame's distinct-page count before the copy below
    const uint64_t t0 = armGetSystemTick();
    const uint64_t nowNs = armTicksToNs(t0);
    // Copied once: every value below comes from the same moment.
    const RlPmCounters c = rl_pm;
    RlPmDkTotals d;
    rlnx_dk_totals(&d);
    ++g_frameNo;
    if (g_winStartNs == 0) {
        if (g_sessionStartS < 0) {
            g_sessionStartS = double(nowNs) / 1e9;
            pc_log_line("[pm] PERF LOG: [pm] [pm-mem] [pm-gx] [pm-dk] [pm-thr] each second (times ms per game "
                        "frame, counts per game frame unless named), [hitch] for frames over %llu ms; "
                        "switch/tools/perf_report.py reads them",
                        (unsigned long long)(kHitchNs / 1000000));
        }
        window_reset(nowNs, c, d);
        g_prevFrame = c;
        g_dkPrevFrame = d;
        return;
    }
    const uint64_t total = gameNs + presentNs;
    ++g_frames;
    g_gameSum += gameNs;
    g_presentSum += presentNs;
    g_gameMax = gameNs > g_gameMax ? gameNs : g_gameMax;
    g_presentMax = presentNs > g_presentMax ? presentNs : g_presentMax;
    g_hist[total <= 16700000ull ? 0 : total <= 33400000ull ? 1 : total <= kHitchNs ? 2 : 3]++;
    if (total > kHitchNs) {
        ++g_hitches;
        if (g_hitchesPrinted < kHitchLinesPerWindow) {
            ++g_hitchesPrinted;
            hitch_line(gameNs, presentNs, c, d);
        }
    }
    g_prevFrame = c;
    g_dkPrevFrame = d;
    if (nowNs - g_winStartNs >= kWindowNs) {
        window_lines(nowNs, c, d);
        window_reset(nowNs, c, d);
    }
    g_costTicks += armGetSystemTick() - t0;  // this reporter's own time (the [pm] line's pm_us)
}
