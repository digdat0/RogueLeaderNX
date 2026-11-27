// RL-NX: the measurement and correctness groundwork (docs/PERF_PLAN.md, phase 0).
//
// Two things live here, both inert unless a boot.cfg variable turns them on:
//
//  * R2_ICOUNT=1 - guest instruction metering. The recompiler adds one count per basic block
//    (ctx.h: icountAdd), so a frame's cost can be divided by the guest instructions it executed.
//    That ratio - ns per guest instruction - is the only number that compares two codegen builds
//    across scenes: fps moves with the scene, the ratio does not.
//
//  * R2_STATE_HASH=<frame>[,<frame>...] - the differential harness. At those frames it prints an
//    XXH3 over the guest's architectural state and all of guest RAM. Two builds that disagree there
//    disagree about the game, which is something no amount of "it boots" can establish, and which
//    the emitter work of 9.45 needed and did not have.
#include <cstdlib>
#include <cstring>

#include "ctx.h"
#include "gfx/gx_bridge.h"
#include "runtime.h"
#include "xxhash.h"

namespace rt {

u64 g_guestInsns = 0;
bool g_icount = false;

namespace {

constexpr int kMaxHashFrames = 32;
u32 s_hashFrames[kMaxHashFrames];
int s_hashCount = 0;

inline u64 add(u64 h, const void* p, size_t n) { return XXH3_64bits_withSeed(p, n, h); }

}  // namespace

// Reads the two switches once, before the guest boots (main_switch.cpp). Everything else in here is
// inert when they are unset, so an ordinary build pays one predictable branch per basic block.
void meterInit() {
    const char* ic = getenv("R2_ICOUNT");
#if defined(RL_NX_ICOUNT)
    g_icount = ic && *ic && *ic != '0';
#else
    // The counter is compiled into the generated code only for a metering build (RL_NX_ICOUNT=1
    // switch/build.sh), so saying it is on in an ordinary build would report an idle counter.
    g_icount = false;
    if (ic && *ic && *ic != '0') logf("meter", "R2_ICOUNT is set but this build has no counter: RL_NX_ICOUNT=1 switch/build.sh 8");
#endif
    s_hashCount = 0;
    if (const char* sh = getenv("R2_STATE_HASH"); sh && *sh) {
        const char* p = sh;
        while (*p && s_hashCount < kMaxHashFrames) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 10);
            if (end == p) break;
            if (v && v <= 0xFFFFFFFFul) s_hashFrames[s_hashCount++] = u32(v);
            p = (*end == ',') ? end + 1 : end;
        }
    }
    if (g_icount) logf("meter", "guest instruction metering on: the frame line reports ns per guest instruction");
    if (s_hashCount) logf("meter", "guest state hash at %d frame(s), first %u", s_hashCount, s_hashFrames[0]);
}

// The guest's architectural state and memory. Registers are copied into a scratch array first so no
// padding or pointer field (fiber, pendingJmp) can make two identical builds disagree.
u64 guestStateHash() {
    const Ctx& c = curCtx();
    u64 h = 0x9E3779B97F4A7C15ull;
    h = add(h, c.r, sizeof c.r);
    h = add(h, c.f, sizeof c.f);
    h = add(h, c.ps1, sizeof c.ps1);
    h = add(h, c.cr, sizeof c.cr);
    const u32 regs[] = {c.lr,       c.ctr,      c.so,       c.ov,       c.ca,       c.xerBytes, c.fpscr,
                        c.msr,      c.srr0,     c.srr1,     c.sprg[0],  c.sprg[1],  c.sprg[2],  c.sprg[3],
                        c.dar,      c.dsisr,    c.hid0,     c.hid1,     c.hid2,     c.wpar,     c.dmaU,
                        c.dmaL,     c.l2cr,     c.sdr1,     c.ear,      c.resvAddr, c.gqr[0],   c.gqr[1],
                        c.gqr[2],   c.gqr[3],   c.gqr[4],   c.gqr[5],   c.gqr[6],   c.gqr[7]};
    h = add(h, regs, sizeof regs);
    h = add(h, g_ram, kGuestRamSize);
    h = add(h, lockedCachePtr(), kLockedCacheLen);
    return h;
}

// Called once per presented frame (host_switch.cpp's viFrameHook), before the 120-frame report.
void meterFrame(u32 frame) {
    for (int i = 0; i < s_hashCount; i++) {
        if (s_hashFrames[i] != frame) continue;
        const u64 h = guestStateHash();
        logf("state", "frame %u hash %016llx draws %llu", frame, (unsigned long long)h,
             (unsigned long long)gfx::drawCalls());
    }
}

}  // namespace rt
