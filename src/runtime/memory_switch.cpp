// RL-NX: guest memory on Horizon. Replaces memory.cpp, which assumes mmap/VirtualAlloc, a fixed
// 4 GiB reservation at a chosen address, and a SIGSEGV/VEH handler that commits pages on first
// touch. A Horizon app has none of those, so:
//
//   * guest RAM is one 256 MiB arena allocated at start (ctx.h has the mapping: guest 0x80000000 is
//     g_ram[0], the uncached view 0xC0000000 is folded onto it by masking),
//   * the "pages past the end of MEM1 are committed when first touched" trick is unnecessary: the
//     whole window is already readable and zero-filled, which is what hardware does for those reads,
//   * the locked cache (0xE0000000) gets its own 16 KiB buffer,
//   * the guest threads' fiber stacks come from the heap instead of a fixed reservation.
//
// Nothing here faults, so there is no fault dump and no host crash path for guest memory: a guest
// address outside the arena is a plain out-of-bounds read (see memPtr in ctx.h) and shows up as
// corruption rather than a clean report. Keep R2_WATCH available when debugging that.
#include <cstdio>  // RL-NX (Stage 9): the VM census file
#include <cstddef>  // RL-NX (Stage 11): offsetof (FASTMEM)
#include <utility>  // RL-NX (Stage 11): std::swap (FASTMEM)
#include <cstdlib>
#include <cstring>

#include "perfmon.h"  // RL-NX: slow-path access counters (PERF LOG)
#include "runtime.h"
#include "savestate.h"

namespace rt {

// RL-NX (9.38): hidden so the guest accessors reach it with adrp + ldr instead of a GOT load (see ctx.h).
u8* g_ram __attribute__((visibility("hidden"))) = nullptr;
static u8* s_stacks = nullptr;
// RL-NX (9.53): global and hidden (was a file static) so ctx.h's inline second tier reaches the locked cache
// without a call; s_lockedCache stays as the old name for the code below.
u8* g_lockedCache RT_HIDDEN = nullptr;
#define s_lockedCache g_lockedCache

u8* lockedCachePtr() { return s_lockedCache; }

// RL-NX (9.37): the out-of-line half of the guest accessors (ctx.h: inArena). Each is the chain the inline
// accessors used to carry, in the same order and with the same meanings: IO registers first (0xC8000000
// masks into the arena the way the uncached view does, so it has to be recognised before the mask), then
// the VM region - split byte by byte when the access crosses a 4 KiB page, which is why vmr/vmw cannot be
// used blind there - then the locked cache, then plain arena RAM. Nothing about the results changed; only
// where the code lives, so the fast path is one compare instead of four.
namespace {

// One access of n bytes from a host pointer, big-endian like the guest.
inline u32 rdBytes(const u8* p, int n) {
    if (n == 1) return *p;
    if (n == 2) {
        u16 v;
        memcpy(&v, p, 2);
        return __builtin_bswap16(v);
    }
    u32 v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

inline void wrBytes(u8* p, u32 v, int n) {
    if (n == 1) {
        *p = u8(v);
    } else if (n == 2) {
        const u16 s = __builtin_bswap16(u16(v));
        memcpy(p, &s, 2);
    } else {
        const u32 s = __builtin_bswap32(v);
        memcpy(p, &s, 4);
    }
}

}  // namespace

// RL-NX: each branch counts its access in rl_pm (perfmon.h) - one add, no clock read - so the PERF LOG
// line shows how many guest accesses miss the inline arena path, and where they go.
// RL-NX (9.53): split in two. rdSlow/wrSlow (preserve_most, see ctx.h) answer the common cases with no further
// call - a VM access whose page is in the TLB and stays in it, the locked cache, a gather-pipe append - and
// everything else goes to the old chain below, unchanged (IO first). The VM test comes before IO here safely:
// 0x7E000000-0x7FFFFFFF and the IO window do not overlap. Each access is still counted exactly once.
namespace {
[[gnu::noinline]] u32 rdChain(u32 a, int n) {
    if (isIo(a)) {
        rl_pm.ioRd[rlPmIoBlock(a)]++;
        return ioRead(a, n);
    }
    if (isVm(a)) {
        rl_pm.vmRd++;
        return crossesPage(a, n) ? rdSplit(a, n) : rdBytes(vmr(a), n);
    }
    if (a - kLockedCacheBase < kLockedCacheLen) {
        rl_pm.lcRd++;
        return rdBytes(s_lockedCache + (a & (kLockedCacheLen - 1)), n);
    }
    rl_pm.otherRd++;
    return rdBytes(memPtr(a), n);
}

[[gnu::noinline]] void wrChain(u32 a, u32 v, int n) {
    // The masks match the ioWrite arguments the inline accessors passed (a byte write must not reach
    // the 16/32-bit register paths with the upper bits of the guest's register set).
    if (isIo(a)) {
        rl_pm.ioWr[rlPmIoBlock(a)]++;  // RL-NX: PERF LOG counters (see rdSlow)
        return ioWrite(a, n == 1 ? (v & 0xFF) : n == 2 ? (v & 0xFFFF) : v, n);
    }
    if (isVm(a)) {
        rl_pm.vmWr++;
        if (crossesPage(a, n)) return wrSplit(a, v, n);
        return wrBytes(vmw(a), v, n);
    }
    if (a - kLockedCacheBase < kLockedCacheLen) {
        rl_pm.lcWr++;
        return wrBytes(s_lockedCache + (a & (kLockedCacheLen - 1)), v, n);
    }
    rl_pm.otherWr++;
    wrBytes(memPtr(a), v, n);
}
}  // namespace

// RL-NX (Stage 9): the VM census (- menu VM CENSUS, default off). Each VM access that reaches rdSlow/wrSlow
// records its caller (the return address: a load or store site in the recompiled code) in a small hash table, and
// every 30 s vmCensusFrame writes the table to logs/vmcensus.txt with the runtime address of rl_nx_prof_anchor, so
// switch/tools/vm_census.py can map each site to its gen/code_NNN.cpp line and from there to the guest pc (the
// "// PC: WORD" comment above it). That list (config/GSWE64/vmsites.txt) tells r2recomp which accesses get the
// inline VM TLB form. Off, the cost is one load and a not-taken branch on the VM path; on, writing the file stalls
// the game thread for a moment every 30 s (a diagnostic mode, not for play).
bool g_vmCensus RT_HIDDEN = false;
namespace {
// RL-NX (Stage 11): + ord/owr, the site's other slow accesses (uncached view, IO, the gather pipe through a plain store,
// anything not VM or the locked cache): the sites a fastmem build must keep tested. vm_census.py reads the first three columns only.
struct CensusSlot {
    uintptr_t ra;
    u32 rd, wr, ord, owr;
};
constexpr u32 kCensusSlots = 1u << 15;
CensusSlot* s_census = nullptr;
u32 s_censusUsed = 0, s_censusLost = 0;
[[gnu::noinline]] void censusHit(uintptr_t ra, bool write, bool vm = true) {  // RL-NX (Stage 11): vm
    if (!s_census) return;
    u32 h = u32((ra >> 2) * 0x9E3779B1u) & (kCensusSlots - 1);
    for (u32 probe = 0; probe < 32; probe++, h = (h + 1) & (kCensusSlots - 1)) {
        CensusSlot& e = s_census[h];
        if (e.ra == ra || e.ra == 0) {
            if (e.ra == 0) {
                if (s_censusUsed >= kCensusSlots * 3 / 4) break;
                e.ra = ra;
                s_censusUsed++;
            }
            if (vm) (write ? e.wr : e.rd)++;
            else (write ? e.owr : e.ord)++;  // RL-NX (Stage 11)
            return;
        }
    }
    s_censusLost++;
}
}  // namespace
#define RT_CENSUS(w)                                                                     \
    do {                                                                                 \
        if (g_vmCensus) [[unlikely]]                                                     \
            censusHit(reinterpret_cast<uintptr_t>(__builtin_return_address(0)), (w));    \
    } while (0)
// RL-NX (Stage 11): a slow access that is not a VM TLB hit (counted as VM when the address is in the VM window)
#define RT_CENSUS_ANY(w, a)                                                                              \
    do {                                                                                                 \
        if (g_vmCensus) [[unlikely]]                                                                     \
            censusHit(reinterpret_cast<uintptr_t>(__builtin_return_address(0)), (w), (a) - kVmStart < kVmLen); \
    } while (0)

void vmCensusSet(bool on) {
    if (on && !s_census) s_census = static_cast<CensusSlot*>(calloc(kCensusSlots, sizeof(CensusSlot)));
    g_vmCensus = on && s_census;
    logf("census", "VM CENSUS %s (logs/vmcensus.txt every 30 s; switch/tools/vm_census.py reads it)", g_vmCensus ? "on" : "off");
}
bool vmCensusOn() { return g_vmCensus; }

}  // namespace rt
extern "C" void rl_nx_prof_anchor(void);  // prof_switch.cpp: the address anchor the reports subtract
namespace rt {

// --- RL-NX (Stage 11): FASTMEM (-DRL_NX_FASTMEM; switch/src/fastmem_switch.cpp maps the window, fastmem_entry.S
// catches the faults). rl_fm_slow runs one faulting F access, in ordinary context on the faulting thread: decode
// it (the data abort's syndrome when it is valid - single GPR loads and stores - else the instruction: FP/SIMD
// single loads and stores, and pairs with a plain offset), run it through rdSlow/wrSlow as the tested forms would,
// write a load's value into the record and step its pc past the instruction. Each faulting pc is counted;
// fmFrame logs the busiest every 30 s and writes logs/fmcensus.txt in the census format, so
// switch/tools/vm_census.py --fastmem can turn it into config/GSWE64/fmchecked.txt (those sites then stay tested).
#if defined(RL_NX_FASTMEM)
}  // namespace rt
extern "C" unsigned long long g_rlFmBase;  // fastmem_switch.cpp
extern "C" bool rlnx_fastmem_map(unsigned char** ram, size_t ramSize, unsigned char** lc, size_t lcSize, const char** why);
namespace rt {
namespace {
struct FmRec {
    u64 x[31];
    u64 sp, pc, pstate, far, esr;
    u8 q[32][16];
    u64 fpsr, fpcr;
};
static_assert(offsetof(FmRec, sp) == 0xF8 && offsetof(FmRec, pc) == 0x100 && offsetof(FmRec, esr) == 0x118 &&
                  offsetof(FmRec, q) == 0x120 && offsetof(FmRec, fpsr) == 0x320,
              "fastmem_entry.S has these offsets");
struct FmSite {
    uintptr_t pc;
    u32 rd, wr;
};
constexpr u32 kFmSites = 4096;
FmSite s_fmSite[kFmSites];
u32 s_fmUsed = 0, s_fmLost = 0;
u64 s_fmFaults = 0, s_fmBad = 0;
void fmCount(uintptr_t pc, bool write) {
    s_fmFaults++;
    u32 h = u32((pc >> 2) * 0x9E3779B1u) & (kFmSites - 1);
    for (u32 probe = 0; probe < 32; probe++, h = (h + 1) & (kFmSites - 1)) {
        FmSite& e = s_fmSite[h];
        if (e.pc != pc && e.pc != 0) continue;
        if (!e.pc) {
            if (s_fmUsed >= kFmSites * 3 / 4) break;
            e.pc = pc;
            s_fmUsed++;
        }
        (write ? e.wr : e.rd)++;
        return;
    }
    s_fmLost++;
}
u64 fmLoadRaw(u32 ga, u32 n) {
    switch (n) {
    case 1: return rdSlow(ga, 1);
    case 2: return __builtin_bswap16(u16(rdSlow(ga, 2)));
    case 4: return __builtin_bswap32(rdSlow(ga, 4));
    default: return __builtin_bswap64((u64(rdSlow(ga, 4)) << 32) | rdSlow(ga + 4, 4));
    }
}
void fmStoreRaw(u32 ga, u64 raw, u32 n) {
    switch (n) {
    case 1: wrSlow(ga, u32(raw & 0xFF), 1); break;
    case 2: wrSlow(ga, __builtin_bswap16(u16(raw)), 2); break;
    case 4: wrSlow(ga, __builtin_bswap32(u32(raw)), 4); break;
    default: {
        const u64 v = __builtin_bswap64(raw);
        wrSlow(ga, u32(v >> 32), 4);
        wrSlow(ga + 4, u32(v), 4);
    }
    }
}
// one element: n bytes at guest address ga into or out of register rt (fp: the vector register file)
void fmElement(FmRec* r, u32 ga, u32 n, u32 rt, bool fp, bool load, bool sext, bool sf) {
    if (fp) {
        if (load) {
            u8 b[16] = {};
            const u64 lo = fmLoadRaw(ga, n > 8 ? 8 : n);
            memcpy(b, &lo, n > 8 ? 8 : n);
            if (n > 8) {
                const u64 hi = fmLoadRaw(ga + 8, 8);
                memcpy(b + 8, &hi, 8);
            }
            memcpy(r->q[rt], b, 16);  // a scalar load clears the rest of the register
        } else {
            u64 lo = 0, hi = 0;
            memcpy(&lo, r->q[rt], n > 8 ? 8 : n);
            fmStoreRaw(ga, lo, n > 8 ? 8 : n);
            if (n > 8) {
                memcpy(&hi, r->q[rt] + 8, 8);
                fmStoreRaw(ga + 8, hi, 8);
            }
        }
        return;
    }
    if (load) {
        u64 v = fmLoadRaw(ga, n);
        if (sext && n < 8) {
            const u32 sh = 64 - 8 * n;
            v = u64(s64(v << sh) >> sh);
        }
        if (!sf) v = u32(v);
        if (rt != 31) r->x[rt] = v;
    } else {
        fmStoreRaw(ga, rt == 31 ? 0 : r->x[rt], n);
    }
}
}  // namespace
}  // namespace rt
extern "C" int rl_fm_slow(rt::FmRec* r) {
    using namespace rt;
    const u32 insn = *reinterpret_cast<const u32*>(uintptr_t(r->pc));
    const u64 base = g_rlFmBase;
    const u32 esr = u32(r->esr);
    auto guest = [&](u64 a, u32& ga) {
        if (a - base >= (1ull << 32)) return false;
        ga = u32(a - base);
        return true;
    };
    u32 ga = 0;
    if ((insn & 0x3B800000u) == 0x29000000u) {  // LDP/STP/LDPSW, signed offset (no writeback)
        const u32 opc = insn >> 30, rn = (insn >> 5) & 31, rt = insn & 31, rt2 = (insn >> 10) & 31;
        const bool fp = (insn >> 26) & 1, load = (insn >> 22) & 1;
        u32 n;
        bool sext = false, sf = true;
        if (fp) {
            if (opc == 3) return 0;
            n = 4u << opc;
        } else {
            if (opc == 3 || (opc == 1 && !load)) return 0;
            n = opc == 2 ? 8 : 4;
            sext = opc == 1;
            sf = opc != 0;
        }
        s32 imm = s32((insn >> 15) & 0x7F);
        if (imm & 0x40) imm -= 0x80;
        const u64 a = (rn == 31 ? r->sp : r->x[rn]) + u64(s64(imm) * s64(n));
        if (!guest(a, ga)) return 0;
        fmCount(uintptr_t(r->pc), !load);
        fmElement(r, ga, n, rt, fp, load, sext, sf);
        fmElement(r, ga + n, n, rt2, fp, load, sext, sf);
        r->pc += 4;
        return 1;
    }
    if (!guest(r->far, ga)) return 0;
    if ((esr >> 26) == 0x24 && ((esr >> 24) & 1)) {  // a valid syndrome: a single GPR load or store
        const u32 n = 1u << ((esr >> 22) & 3), rt = (esr >> 16) & 31;
        const bool sext = (esr >> 21) & 1, sf = (esr >> 15) & 1, write = (esr >> 6) & 1;
        fmCount(uintptr_t(r->pc), write);
        fmElement(r, ga, n, rt, false, !write, sext, sf);
        r->pc += 4;
        return 1;
    }
    // load/store register: unsigned offset, unscaled offset, register offset (none writes the base back)
    if ((insn & 0x3B000000u) == 0x39000000u || (insn & 0x3B200C00u) == 0x38000000u || (insn & 0x3B200C00u) == 0x38200800u) {
        const u32 size = insn >> 30, opc = (insn >> 22) & 3, rt = insn & 31;
        const bool fp = (insn >> 26) & 1;
        u32 n;
        bool load, sext = false, sf = true;
        if (fp) {
            n = 1u << (size | ((opc & 2) << 1));
            if (n > 16) return 0;
            load = opc & 1;
        } else {
            n = 1u << size;
            load = opc != 0;
            if (opc >= 2) {
                if (size == 3) return 0;  // prfm / unallocated
                sext = true;
                sf = opc == 2;
            } else sf = size == 3;
        }
        fmCount(uintptr_t(r->pc), !load);
        fmElement(r, ga, n, rt, fp, load, sext, sf);
        r->pc += 4;
        return 1;
    }
    s_fmBad++;
    return 0;
}
namespace rt {
namespace {
// game thread, once per vblank (vmCensusFrame): every 30 s, the faults so far
void fmFrame() {
    static u32 frames = 0;
    static u64 lastFaults = 0;
    if (++frames < 1800) return;
    frames = 0;
    if (s_fmFaults == lastFaults) return;
    u32 top[8] = {};
    u32 nTop = 0;
    for (u32 i = 0; i < kFmSites; i++) {
        if (!s_fmSite[i].pc) continue;
        const u32 c = s_fmSite[i].rd + s_fmSite[i].wr;
        u32 k = nTop < 8 ? nTop++ : 7;
        if (k == 7 && nTop == 8 && c <= s_fmSite[top[7]].rd + s_fmSite[top[7]].wr) continue;
        top[k] = i;
        for (; k > 0 && c > s_fmSite[top[k - 1]].rd + s_fmSite[top[k - 1]].wr; k--) std::swap(top[k], top[k - 1]);
    }
    char line[512];
    int len = snprintf(line, sizeof line, "%llu faults in the last 30 s (%llu in all, %u sites, %llu undecoded); busiest (pc offset from the anchor: count):",
                       (unsigned long long)(s_fmFaults - lastFaults), (unsigned long long)s_fmFaults, s_fmUsed, (unsigned long long)s_fmBad);
    for (u32 i = 0; i < nTop && len < int(sizeof line) - 32; i++)
        len += snprintf(line + len, sizeof line - size_t(len), " %llx:%u",
                        (unsigned long long)(s_fmSite[top[i]].pc - reinterpret_cast<uintptr_t>(&rl_nx_prof_anchor)),
                        s_fmSite[top[i]].rd + s_fmSite[top[i]].wr);
    logf("fastmem", "%s", line);
    lastFaults = s_fmFaults;
    if (FILE* f = fopen("sdmc:/switch/RogueLeader-NX/logs/fmcensus.txt", "w")) {
        fprintf(f, "# RL-NX fastmem faults: faulting pc + 4 (as a return address), 0, 0, reads, writes (cumulative)\n");
        fprintf(f, "anchor %llx\nlost %u\n", (unsigned long long)reinterpret_cast<uintptr_t>(&rl_nx_prof_anchor), s_fmLost);
        for (u32 i = 0; i < kFmSites; i++)
            if (s_fmSite[i].pc)
                fprintf(f, "%llx 0 0 %u %u\n", (unsigned long long)(s_fmSite[i].pc + 4), s_fmSite[i].rd, s_fmSite[i].wr);
        fclose(f);
    }
}
}  // namespace
#endif  // RL_NX_FASTMEM

// Game thread, once per vblank (gx_bridge.cpp vblank): writes the table every 1800 vblanks (30 s).
void vmCensusFrame() {
#if defined(RL_NX_FASTMEM)
    fmFrame();  // RL-NX (Stage 11)
#endif
    static u32 frames = 0;
    if (!g_vmCensus || ++frames < 1800) return;
    frames = 0;
    FILE* f = fopen("sdmc:/switch/RogueLeader-NX/logs/vmcensus.txt", "w");
    if (!f) return;
    fprintf(f, "# RL-NX VM census: return address, VM reads, VM writes, other slow reads, other slow writes (cumulative since VM CENSUS went on)\n");  // RL-NX (Stage 11): + other
    fprintf(f, "anchor %llx\nlost %u\n", (unsigned long long)reinterpret_cast<uintptr_t>(&rl_nx_prof_anchor), s_censusLost);
    for (u32 i = 0; i < kCensusSlots; i++)
        if (s_census[i].ra)
            fprintf(f, "%llx %u %u %u %u\n", (unsigned long long)s_census[i].ra, s_census[i].rd, s_census[i].wr, s_census[i].ord,
                    s_census[i].owr);  // RL-NX (Stage 11)
    fclose(f);
    logf("census", "VM census: %u sites written to logs/vmcensus.txt (%u hits lost)", s_censusUsed, s_censusLost);
}

RT_SLOWCC u32 rdSlow(u32 a, int n) {
    const u32 o = a - kVmStart;
    if (o < kVmLen) {
        const u8* p = g_vmRead[o >> 12];
        if (p && (a & 0xFFFu) <= 0x1000u - u32(n)) [[likely]] {
            rl_pm.vmRd++;
            RT_CENSUS(false);  // RL-NX (Stage 9)
            return rdBytes(p + (a & 0xFFFu), n);
        }
    } else if (a - kUncachedBase < kUncachedLen) {
        // RL-NX (Stage 3): the uncached view, no longer in the inline test (ctx.h inArena); same bytes as 0x80000000
        rl_pm.otherRd++;
        RT_CENSUS_ANY(false, a);  // RL-NX (Stage 11)
        return rdBytes(g_ram + (a - kUncachedBase), n);
    } else if (a - kLockedCacheBase < kLockedCacheLen) {
        rl_pm.lcRd++;  // RL-NX (Stage 11): not in the census - FASTMEM maps the locked cache, so F forms reach it directly
        return rdBytes(s_lockedCache + (a & (kLockedCacheLen - 1)), n);
    }
    RT_CENSUS_ANY(false, a);  // RL-NX (Stage 11): IO, a VM page miss, the rest
    return rdChain(a, n);
}

RT_SLOWCC void wrSlow(u32 a, u32 v, int n) {
    const u32 o = a - kVmStart;
    if (o < kVmLen) {
        u8* p = g_vmWrite[o >> 12];
        if (p && (a & 0xFFFu) <= 0x1000u - u32(n)) [[likely]] {
            rl_pm.vmWr++;
            RT_CENSUS(true);  // RL-NX (Stage 9)
            return wrBytes(p + (a & 0xFFFu), v, n);
        }
    } else if (a - kUncachedBase < kUncachedLen) {
        rl_pm.otherWr++;  // RL-NX (Stage 3): the uncached view (see rdSlow)
        RT_CENSUS_ANY(true, a);  // RL-NX (Stage 11)
        return wrBytes(g_ram + (a - kUncachedBase), v, n);
    } else if (a - kLockedCacheBase < kLockedCacheLen) {
        rl_pm.lcWr++;  // RL-NX (Stage 11): not in the census (see rdSlow)
        return wrBytes(s_lockedCache + (a & (kLockedCacheLen - 1)), v, n);
    } else if ((a & 0xFFFFFFF8u) == 0xCC008000u) {
        RT_CENSUS_ANY(true, a);  // RL-NX (Stage 11): the gather pipe through a plain store
        // RL-NX (9.53): the gather pipe - append to the 32-byte buffer; gx_fifo.cpp bursts it when full. A null
        // g_gpLimit (oracle test, R2_DRAW_CALLER) sends the write down the old ioWrite -> gxFifoWrite path.
        u8* d = g_gpCur;
        if (d < g_gpLimit) [[likely]] {
            rl_pm.ioWr[8]++;
            wrBytes(d, v, n);  // the buffer has 64 bytes: a 4-byte write at offset 31 fits
            g_gpCur = d + n;
            if (d + n >= g_gpLimit) gxGatherBurst();
            return;
        }
    }
    RT_CENSUS_ANY(true, a);  // RL-NX (Stage 11): IO, a VM page miss, the rest
    wrChain(a, v, n);
}

// aligned_alloc with an aligned size: newlib's is a thin wrapper over memalign.
static u8* allocAligned(size_t n, size_t align) {
    void* p = aligned_alloc(align, (n + align - 1) & ~(align - 1));
    return static_cast<u8*>(p);
}

bool memInit() {
#if defined(RL_NX_FASTMEM)
    // RL-NX (Stage 11): FASTMEM - the arena and the locked cache inside the 4 GiB window (fastmem_switch.cpp)
    {
        const char* why = "";
        if (!rlnx_fastmem_map(&g_ram, kGuestRamSize + kArenaSlack, &s_lockedCache, kLockedCacheSize, &why)) {
            logf("mem", "FASTMEM: cannot map the guest window (%s); this NRO needs it - use the ordinary RL-NX.nro", why);
            return false;
        }
        logf("mem", "FASTMEM: window %llx, guest RAM at +0x80000000, locked cache at +0xE0000000; other guest addresses fault into rdSlow/wrSlow",
             g_rlFmBase);
    }
#else
    g_ram = allocAligned(kGuestRamSize + kArenaSlack, 0x10000);  // RL-NX (Stage 3): + slack (ctx.h inArena)
    if (!g_ram) {
        logf("mem", "cannot allocate the %u MiB guest RAM arena", kGuestRamSize >> 20);
        return false;
    }
    memset(g_ram, 0, kGuestRamSize + kArenaSlack);
    s_lockedCache = allocAligned(kLockedCacheSize, 0x1000);
    if (!s_lockedCache) return false;
    memset(s_lockedCache, 0, kLockedCacheSize);
#endif
    s_stacks = allocAligned(u64(kMaxFibers) * kFiberStackSize, 0x1000);
    if (!s_stacks) return false;
    logf("mem", "guest RAM %p (%u MiB, 0x%08X-0x%08X), stacks %p (%u x %u KiB), locked cache %p", g_ram,
         kGuestRamSize >> 20, kGuestRamBase, kGuestRamBase + kGuestRamSize - 1, s_stacks, kMaxFibers,
         unsigned(kFiberStackSize >> 10), s_lockedCache);
    return true;
}

u8* fiberStackRegion() { return s_stacks; }

// A physical address (what the DMA engines and the GX FIFO use) as a host pointer. 0x80000000 is
// g_ram[0], so MEM1 and its heap extension are g_ram + phys, and the cached/uncached split is a
// property of the *guest* address only: the physical one is the same byte either way.
u8* physPtr(u32 phys, u32 len) {
    phys &= 0x3FFFFFFF;
    if (phys >= kMem1Mapped || len > kMem1Mapped - phys) return nullptr;
    return g_ram + phys;
}

// Save states hold host stacks with return addresses into the exe, so upstream pins the exe, the
// guest base and the stacks to fixed addresses. A Horizon process cannot choose its load address,
// so a state made on the Switch belongs to the session that made it (exeBase/exeHash 0 marks that).
HostLayout hostLayout() { return {0, 0, u64(g_ram), u64(s_stacks)}; }

void memSaveState(state::Writer& w) {
    w.section("MEM1");
    w.bytes(g_ram, kMem1Mapped);  // with the heap extension
    w.bytes(s_lockedCache, kLockedCacheSize);
    w.bytes(memPtr(kPortArena), kPortArenaSize);
}

void memLoadState(state::Reader& r) {
    r.section("MEM1");
    r.bytes(g_ram, kMem1Mapped);
    r.bytes(s_lockedCache, kLockedCacheSize);
    r.bytes(memPtr(kPortArena), kPortArenaSize);
}

}  // namespace rt
