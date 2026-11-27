// Software MMU for the VM region (0x7E000000-0x7FFFFFFF).
//
// Rogue Leader runs its main code and keeps its data in virtual memory backed by ARAM: it
// builds a PowerPC hashed page table (SDR1, segment registers), and its DSI handler pages
// 4 KiB frames in from ARAM on demand. We walk that page table on a TLB miss, keep the
// referenced/changed bits the game's pager relies on, and deliver page faults to the game's
// own handler (exception 2) exactly where the access happens.
#include <unordered_map>

#include "runtime.h"
#include "perfmon.h"  // RL-NX: rl_pm VM counters

namespace rt {

u8* g_vmRead[kVmLen >> 12] RT_HIDDEN;   // RL-NX (9.53): hidden, read inline by ctx.h's second tier
u8* g_vmWrite[kVmLen >> 12] RT_HIDDEN;

namespace {
u32 s_sr[16];
u32 s_sdr1 = 0;
u64 s_faults = 0;
bool s_log = false;

inline u32 physRd32(u32 pa) {
    const u8* p = physPtr(pa, 4);
    if (!p) fatal("page table read outside MEM1 at %08X", pa);
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
inline void physWr32(u32 pa, u32 v) {
    u8* p = physPtr(pa, 4);
    p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
}

struct Walk {
    bool found = false;
    u32 pteAddr = 0;  // physical address of the PTE
    u32 w1 = 0;       // second PTE word (RPN, R, C, WIMG, PP)
};

Walk walk(u32 ea) {
    Walk r;
    const u32 sr = s_sr[ea >> 28];
    if (sr & 0x80000000u) return r;  // direct-store segment: not used
    const u32 vsid = sr & 0xFFFFFF;
    const u32 pageIndex = (ea >> 12) & 0xFFFF;
    const u32 api = pageIndex >> 10;
    const u32 htabOrg = s_sdr1 & 0xFFFF0000u, htabMask = s_sdr1 & 0x1FF;
    for (u32 h = 0; h < 2; h++) {
        u32 hash = (vsid & 0x7FFFF) ^ pageIndex;
        if (h) hash = ~hash;
        const u32 pteg = htabOrg | ((((hash >> 10) & htabMask) | ((htabOrg >> 16) & ~htabMask & 0x1FF)) << 16) |
                         ((hash & 0x3FF) << 6);
        for (u32 i = 0; i < 8; i++) {
            const u32 pa = pteg + i * 8;
            const u32 w0 = physRd32(pa);
            if (!(w0 >> 31)) continue;
            if (((w0 >> 7) & 0xFFFFFF) != vsid || ((w0 >> 6) & 1) != h || (w0 & 0x3F) != api) continue;
            r.found = true;
            r.pteAddr = pa;
            r.w1 = physRd32(pa + 4);
            return r;
        }
    }
    return r;
}

bool ppAllows(u32 sr, u32 pp, bool write) {
    const bool key = (sr >> 30) & 1;  // Ks: supervisor (the GameCube never runs in user mode)
    if (!key) return write ? pp != 3 : true;
    switch (pp) {
    case 0: return false;
    case 2: return true;
    default: return !write;
    }
}
// The game patches the hardware DSI vector (0x80000300, see 0x800098EC) with a stub that
// sends faults on 0x7xxxxxxx to its own entry at 0x800097AC. That entry builds an OSContext-like
// frame on the current stack, calls the pager whose address it keeps at 0x80095D00 as
// pager(2, frame), restores every register from the frame and returns with rfi. We do the
// same around the recompiled pager. (The ISI path is 0x800099A4 / 0x80095D04 / 3; recompiled
// code never fetches instructions, so it is never taken.)
constexpr u32 kPagerPtr = 0x80095D00;
constexpr u32 kFrameSize = 716;

// The pager's swap state (r13 = 0x8009ADA0): slot array (12-byte records: u16 next, prev,
// flags (state in the top 4 bits), u32 virtual page), the virtual page -> slot map (u16 per
// page from 0x7F000000, 0xFFFF = none) and the list counts. Every page the game touches takes
// a slot when it is paged in (0x8000831C) and keeps it until VMFreeRange (0x80008B9C, from
// HeapFree of the VM heap) releases it; when the free list and the two reclaimable lists are
// empty the game stops: "FATAL VM ERROR: Out of ARAM memory" (0x800082D4).
constexpr u32 kSlotArrayPtr = 0x80095CD0, kSlotMapPtr = 0x80095CE4, kSlots = 1536, kGsysVmBase = 0x7F000000;
int s_aramLog = -1;  // R2_VM_SLOTS: 1 new pages + slot counts every 2 s

void noteNewPage(Ctx& c) {
    const u32 a = c.dar, map = rd32(kSlotMapPtr);
    if (!map || a < kGsysVmBase) return;
    if (rd16(map + ((a - kGsysVmBase) >> 12) * 2) != 0xFFFF) return;  // has a slot: paged in again
    std::string chain = guestBacktrace();
    if (!chain.empty() && chain.back() == '\n') chain.pop_back();
    logf("vmslot", "new page %08X (%s %08X) in %s; %s", a & ~0xFFFu, c.dsisr & 0x02000000u ? "write" : "read", a,
         describeAddr(c.curFn).c_str(), chain.c_str());
}

void pageFault(Ctx& c) {
    const u32 pager = rd32(kPagerPtr);
    if (!pager) {
        deliverException(c, 2, c.curFn);  // not installed yet: the OS reports it
        return;
    }
    if (s_aramLog < 0) s_aramLog = getenv("R2_VM_SLOTS") ? atoi(getenv("R2_VM_SLOTS")) : 0;
    if (s_aramLog) noteNewPage(c);
    const Ctx saved = c;
    const u32 sp = c.r[1] - kFrameSize;
    const u32 frame = sp + 8;
    wr32(sp, c.r[1]);  // back chain
    for (int i = 0; i < 32; i++) wr32(frame + 4 * i, c.r[i]);
    wr32(frame + 0x80, c.getcr());
    wr32(frame + 0x84, c.lr);
    wr32(frame + 0x88, c.ctr);
    wr32(frame + 0x8C, c.getxer());
    wr32(frame + 0x198, c.curFn);  // SRR0 (the recompiled code has no exact PC)
    wr32(frame + 0x19C, c.msr);
    c.srr0 = c.curFn;
    c.srr1 = c.msr;
    c.msr = (c.msr & ~0x8000u) | 0x2032;  // exception: interrupts off; IR DR FP RI
    c.r[1] = sp;
    c.r[3] = 2;  // OS_EXCEPTION_DSI
    c.r[4] = frame;
    c.lr = 0;
    dispatch(c, pager);
    // the entry's epilogue reloads everything from the frame; the pager does not edit it
    const u32 curFn = c.curFn;
    c = saved;
    c.curFn = curFn;
}
}  // namespace

// R2_VM_SLOTS: the pager's slots by state and, for slots holding a page, by 1 MiB of the VM
// (0x7F0..0x7FF), with the 64 KiB regions that changed since the last report.
void vmAramReport() {
    if (s_aramLog <= 0) return;
    const u32 arr = rd32(kSlotArrayPtr);
    if (!arr) return;
    u32 byState[16] = {}, byMiB[16] = {};
    static u32 last64[256], cur64[256];
    memset(cur64, 0, sizeof cur64);
    for (u32 i = 0; i < kSlots; i++) {
        const u32 st = rd16(arr + i * 12 + 4) >> 12, va = rd32(arr + i * 12 + 8);
        byState[st]++;
        if (st && va >= kGsysVmBase) byMiB[(va - kGsysVmBase) >> 20]++, cur64[(va - kGsysVmBase) >> 16]++;
    }
    std::string mib, diff;
    char b[48];
    for (u32 i = 0; i < 16; i++)
        if (byMiB[i]) snprintf(b, sizeof b, " %X:%u", 0x7F0 + i, byMiB[i]), mib += b;
    for (u32 i = 0; i < 256; i++)
        if (cur64[i] != last64[i])
            snprintf(b, sizeof b, " %04X:%+d", 0x7F00 + i, int(cur64[i]) - int(last64[i])), diff += b;
    memcpy(last64, cur64, sizeof last64);
    logf("vmslot", "slots: free %u, reclaimable %u/%u, held %u, written %u | by MiB:%s | changed:%s", byState[0],
         byState[0xC], byState[0x8], byState[0xF], byState[0x4], mib.c_str(), diff.empty() ? " none" : diff.c_str());
}

void vmFlushAll() {
    rl_pm.vmFlush++;
    memset(g_vmRead, 0, sizeof g_vmRead);
    memset(g_vmWrite, 0, sizeof g_vmWrite);
}

void vmSetSdr1(u32 v) {
    s_sdr1 = v;
    vmFlushAll();
    s_log = envFlag("R2_VM_LOG");
    if (s_log) logf("vm", "SDR1 = %08X", v);
}

void mtsr(Ctx&, u32 n, u32 v) {
    s_sr[n & 15] = v;
    vmFlushAll();
}
u32 mfsr(Ctx&, u32 n) { return s_sr[n & 15]; }

void tlbie(Ctx&, u32 ea) {
    if (isVm(ea)) {
        rl_pm.vmTlbie++;
        const u32 i = (ea - kVmStart) >> 12;
        g_vmRead[i] = nullptr;
        g_vmWrite[i] = nullptr;
    }
}

// R2_RWATCH=<addr>,<len> (hex, VM region): log guest reads of that range, by function (with
// tracing on) and LR. Its pages stay out of the read TLB so every read comes through here.
// Each reader (function, LR, address) is logged at most once a second, with its read count.
static u32 s_rwLo = 0, s_rwHi = 0;
static bool rwatchPage(u32 a) { return s_rwHi && (a & ~0xFFFu) < s_rwHi && (a | 0xFFFu) >= s_rwLo; }
static void rwatchNote(u32 a) {
    if (a < s_rwLo || a >= s_rwHi) return;
    struct Seen { u64 logged; u64 count; };
    static std::unordered_map<u64, Seen> seen;
    const Ctx& c = curCtx();
    const u64 key = (u64(c.curFn) << 32) ^ (u64(c.lr) << 12) ^ a, now = tbNow();
    auto [it, fresh] = seen.try_emplace(key, Seen{now, 0});
    it->second.count++;
    if (fresh || now - it->second.logged > kTbClock) {
        logf("rwatch", "read %08X in %s (lr %08X), %llu reads", a, describeAddr(c.curFn).c_str(), c.lr,
             (unsigned long long)it->second.count);
        it->second.logged = now;
    }
}

// RL-NX (audit 2026-11-27): per-frame distinct VM pages missed (one bit per 4 KiB page of the 32 MiB region),
// and since start. Counters only: nothing here changes what vmMiss does. rlPmVmFrame (perfmon_switch.cpp,
// once per game frame) folds the frame's count into rl_pm and clears the bits.
static u32 s_pageSeenFrame[kVmLen / 0x1000 / 32];
static u32 s_pageSeenEver[kVmLen / 0x1000 / 32];
static u32 s_pagesThisFrame;
static inline void vmNotePage(u32 idx) {
    const u32 w = idx >> 5, m = 1u << (idx & 31);
    if (!(s_pageSeenFrame[w] & m)) {
        s_pageSeenFrame[w] |= m;
        s_pagesThisFrame++;
    }
    if (!(s_pageSeenEver[w] & m)) {
        s_pageSeenEver[w] |= m;
        rl_pm.vmPagesTotal++;
    }
}
extern "C" void rlPmVmFrame(void) {
    rl_pm.vmPagesFrame += s_pagesThisFrame;
    if (s_pagesThisFrame > rl_pm.vmPagesFrameMax) rl_pm.vmPagesFrameMax = s_pagesThisFrame;
    s_pagesThisFrame = 0;
    memset(s_pageSeenFrame, 0, sizeof s_pageSeenFrame);
}

u8* vmMiss(u32 a, bool write) {
    const u32 idx = (a - kVmStart) >> 12;
    rl_pm.vmFill++;
    vmNotePage(idx);
    static const bool rwInit = [] {
        if (const char* s = getenv("R2_RWATCH")) {
            char* end;
            s_rwLo = u32(strtoul(s, &end, 16));
            s_rwHi = s_rwLo + (*end == ',' ? u32(strtoul(end + 1, nullptr, 16)) : 4);
            logf("rwatch", "watching reads of %08X-%08X", s_rwLo, s_rwHi);
        }
        return true;
    }();
    (void)rwInit;
    if (!write && rwatchPage(a)) rwatchNote(a);
    for (int attempt = 0; attempt < 8; attempt++) {
        const Walk w = walk(a);
        u32 dsisr;
        if (w.found) {
            const u32 pp = w.w1 & 3;
            if (!write || ppAllows(s_sr[a >> 28], pp, true)) {
                u32 w1 = w.w1 | 0x100;  // R
                if (write) w1 |= 0x80;   // C
                if (w1 != w.w1) physWr32(w.pteAddr + 4, w1);
                const u32 frame = w1 & 0xFFFFF000u;
                u8* host = physPtr(frame, 0x1000);
                if (!host) fatal("VM page %08X maps to frame %08X outside MEM1", a & ~0xFFFu, frame);
                g_vmRead[idx] = rwatchPage(a) ? nullptr : host;
                // writes go straight through once C is set and the page is writable
                g_vmWrite[idx] = ((w1 & 0x80) && ppAllows(s_sr[a >> 28], pp, true)) ? host : nullptr;
                return host;
            }
            dsisr = 0x08000000u | 0x02000000u;  // protection violation on a store
        } else {
            dsisr = 0x40000000u | (write ? 0x02000000u : 0);  // no translation
        }
        // RL-NX (9.55): inside an oracle probe the pager must not run - it starts ARAM transfers through HLE
        // state that lives outside the probe's scratch copy, and the live game then found its VM code pages
        // zeroed (illegal instruction at 7FC4xxxx right after the VM init). Hand back a dummy page and taint the
        // probe, which is then reported as skipped.
        if (g_oracleIo) {
            static u8 s_dummy[0x1000];
            g_oracleTaint = true;
            return s_dummy;
        }
        // Page fault: the game's DSI handler pages the frame in (or maps it writable).
        Ctx& c = curCtx();
        c.dar = a;
        c.dsisr = dsisr;
        s_faults++;
        rl_pm.vmFault++;
        const u64 pgT0 = rlPmTick();
        if (s_log) logf("vm", "DSI %s %08X (dsisr %08X), fault #%llu", write ? "write" : "read", a, dsisr, (unsigned long long)s_faults);
        pageFault(c);
        rl_pm.vmFaultTicks += rlPmTick() - pgT0;
    }
    fatal("the page fault handler did not map %08X (%s)", a, write ? "write" : "read");
}

u32 rdSplit(u32 a, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | *vmr(a + u32(i));
    return v;
}
void wrSplit(u32 a, u32 v, int n) {
    for (int i = 0; i < n; i++) *vmw(a + u32(i)) = u8(v >> (8 * (n - 1 - i)));
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "savestate.h"
namespace rt {
void vmSaveState(state::Writer& w) {
    w.pod(s_sr);
    w.pod(s_sdr1);
}
void vmLoadState(state::Reader& r) {
    r.pod(s_sr);
    r.pod(s_sdr1);
    vmFlushAll();  // the page table in guest memory changed: walk it again
}
}  // namespace rt
