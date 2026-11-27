// RL-NX (PERF_PLAN phase 0b-ii): the oracle test - the correctness gate for codegen.
//
// The problem it solves: a state hash taken at a frame index cannot compare two builds, because the guest's
// timeline is wall-clock and the same frame is a different amount of work each run (PERF_HANDOFF 9.46), and
// freezing that clock deadlocks the boot (9.47). So the comparison is moved off the game clock entirely:
//
//   at a guest function entry, run that one function twice - once through the interpreter (correct by
//   construction; the path the port was brought up on) and once through the recompiled code - each from
//   the SAME Ctx and the SAME copy of guest memory, with every device muted, then hash the resulting Ctx
//   and memory. Equal hashes mean the two engines agree about that function; a mismatch names it.
//
// Nothing here touches the live state: both passes run on a scratch copy of the 256 MiB arena, the Ctx is
// copied, and ioRead/ioWrite/gxFifoWrite are silenced while a pass runs (ctx.h g_oracleIo), so the game
// continues exactly as if the probe had not happened, and neither pass can submit a draw.
//
// Off by default; R2_ORACLE=<n> arms n probes, and a probe costs two 256 MiB copies plus two 256 MiB hashes
// (~100 ms), so a run takes a handful.
#include <cstdio>  // RL-NX (9.55)
#include <string>
#include <unordered_set>
#include <vector>  // RL-NX (9.55): functions already probed
#include <cstdlib>
#include <cstring>

#include "ctx.h"
#include "sjlj.h"  // RL-NX (9.55): oracleAbandon
#include "runtime.h"
#include "hw/gx_fifo.h"  // RL-NX (9.53): gxGatherRefresh
#include "table.h"
#include "xxhash.h"

namespace rt {

bool g_oracleIo = false;  // ctx.h: devices muted during a probe
bool g_oracleTaint = false;  // RL-NX (9.55)
u64 g_oracleTb = 0;          // RL-NX (9.55)
namespace {
JmpBuf* s_escape = nullptr;  // RL-NX (9.55): where oracleAbandon() returns to (runOne)
}
// RL-NX (9.55): the VM module's text (config/GSWE64/layout.txt: `embedded 80100000`, a DOL image in MEM1 at
// boot), copied out of guest memory at the first probe - long before the game moves that data to ARAM (~15 s;
// the first probe is ~5 s in). The interpreter fetches VM code from here inside a probe, since the pager keeps
// most of those pages out of memory. Same bytes the recompiler compiled.
struct CodeSec {
    u32 addr, size;
    std::vector<u8> bytes;
};
std::vector<CodeSec> s_vmCode;
void captureVmCode() {
    constexpr u32 kAt = 0x80100000u;
    auto be = [&](u32 o) { return rd32(kAt + o); };
    u32 total = 0;
    for (int i = 0; i < 7; i++) {
        const u32 off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
        if (!size) continue;
        if (addr < 0x7E000000u || addr >= 0x80000000u || size > (4u << 20) || off > (8u << 20)) {
            logf("oracle", "no VM module header at %08X (section %d: %08X %08X %08X): VM functions stay unprobed",
                 kAt, i, off, addr, size);
            s_vmCode.clear();
            return;
        }
        CodeSec s{addr, size, std::vector<u8>(size)};
        memcpy(s.bytes.data(), memPtr(kAt + off), size);
        s_vmCode.push_back(std::move(s));
        total += size;
    }
    logf("oracle", "VM module text captured: %zu section(s), %u KiB", s_vmCode.size(), total >> 10);
}
bool oracleCode(u32 pc, u32& w) {
    for (const CodeSec& s : s_vmCode)
        if (pc - s.addr < s.size && pc - s.addr + 4 <= s.size) {
            w = ld32be(s.bytes.data() + (pc - s.addr));
            return true;
        }
    return false;
}
// RL-NX (9.55): a pass reached something a probe cannot do (an idle wait for a live interrupt, a thread switch,
// a fatal): taint the probe and unwind straight back to runOne. The pass's state is scratch and is discarded.
void oracleAbandon() {
    if (!s_escape) {
        g_oracleIo = false;  // (or fatal() would come straight back here)
        fatal("oracleAbandon outside an oracle probe");
    }
    g_oracleTaint = true;
    rt_longjmp(s_escape, 1);
}
// RL-NX (9.55): with nothing delivered, a pass that waits on a flag only an interrupt handler sets spins for
// ever (a sweep hung after 22 probes). The interpreter pass has its step limit; this is the recompiled pass's.
u32 s_probeSafepoints = 0;
void oracleSafepoint() {
    if (++s_probeSafepoints > 200000) oracleAbandon();
}

namespace {

bool s_on = false;
u32 s_budget = 0;
u8* s_scratch = nullptr;      // 256 MiB: one pass's private copy of guest memory
u8* s_locked = nullptr;       // 16 KiB: the locked cache, saved across both passes
bool s_inProbe = false;
u64 s_pass = 0, s_fail = 0, s_skip = 0;

constexpr u64 kSteps = 400000;  // interpreter step limit for one function body

inline u64 add(u64 h, const void* p, size_t n) { return XXH3_64bits_withSeed(p, n, h); }

u64 stateHash(const Ctx& c, const u8* mem, const u8* locked) {
    u64 h = 0x9E3779B97F4A7C15ull;
    h = add(h, c.r, sizeof c.r);
    h = add(h, c.f, sizeof c.f);
    h = add(h, c.ps1, sizeof c.ps1);
    h = add(h, c.cr, sizeof c.cr);
    const u32 regs[] = {c.lr,      c.ctr,     c.so,       c.ov,       c.ca,       c.xerBytes, c.fpscr,
                        c.msr,     c.srr0,    c.srr1,     c.sprg[0],  c.sprg[1],  c.sprg[2],  c.sprg[3],
                        c.dar,     c.dsisr,   c.resvAddr, c.gqr[0],   c.gqr[1],   c.gqr[2],   c.gqr[3],
                        c.gqr[4],  c.gqr[5],  c.gqr[6],   c.gqr[7]};
    h = add(h, regs, sizeof regs);
    h = add(h, mem, kGuestRamSize);
    h = add(h, locked, kLockedCacheLen);
    return h;
}

GuestFn findFn(u32 pc) {
    for (u32 i = 0; i < g_funcCount; i++)
        if (g_funcTable[i].addr == pc) return g_funcTable[i].fn;
    return nullptr;
}

// One engine's run of one function, from the live bytes, on a private copy, with the devices muted.
// Returns the hash of the resulting Ctx + memory, or 0 with hitLimit set if it did not return.
// RL-NX (9.55): per-64 KiB chunk hashes of each pass's memory, and its final Ctx, so a mismatch can say WHAT
// differs (which registers, which memory) instead of only that something did.
constexpr u32 kChunk = 64u << 10, kChunks = kGuestRamSize / kChunk;
u64 s_chunks[2][kChunks];
Ctx s_outCtx[2];
u64 runOne(const Ctx& start, u32 pc, bool interpreted, bool& hitLimit) {
    memcpy(s_scratch, g_ram, kGuestRamSize);
    memcpy(lockedCachePtr(), s_locked, kLockedCacheLen);
    u8* const live = g_ram;
    g_ram = s_scratch;  // every guest access in both engines goes through memPtr() -> g_ram
    // RL-NX (9.55): the VM TLB caches host pointers into the LIVE arena, so VM-region accesses went around the
    // scratch copy: the interpreter pass wrote live memory and the recompiled pass then started from what it
    // left (false mismatches, and the live game corrupted - an illegal instruction at 7FC3DA04 in a sweep).
    // For the pass, every cached page is moved to the same offset in the scratch copy (emptying the TLB instead
    // lost mappings the scratch page table cannot rebuild without the pager: 107 of 407 probes were skipped,
    // most of them the hot 7FCxxxxx functions), and the live TLB is put back afterwards.
    static u8* savedR[kVmLen >> 12];
    static u8* savedW[kVmLen >> 12];
    memcpy(savedR, g_vmRead, sizeof savedR);
    memcpy(savedW, g_vmWrite, sizeof savedW);
    auto toScratch = [live](u8* p) -> u8* {
        return p && p >= live && p < live + kGuestRamSize ? s_scratch + (p - live) : nullptr;
    };
    for (u32 i = 0; i < (kVmLen >> 12); i++) {
        g_vmRead[i] = toScratch(g_vmRead[i]);
        g_vmWrite[i] = toScratch(g_vmWrite[i]);
    }
    g_oracleIo = true;
    gxGatherRefresh();  // RL-NX (9.53): the inline gather pipe steps aside while devices are muted
    static Ctx work;  // RL-NX (9.55): static, so its contents are well defined after oracleAbandon's longjmp
    work = start;
    hitLimit = false;
    JmpBuf esc;
    s_escape = &esc;
    s_probeSafepoints = 0;
    if (rt_setjmp(&esc) != 0) {
        // RL-NX (9.55): abandoned (g_oracleTaint is set); fall through to the cleanup below
    } else if (interpreted) {
        interpretBounded(work, pc, kSteps);
        hitLimit = g_interpHitLimit;
    } else if (GuestFn fn = findFn(pc)) {
        fn(work);
        // R2_ORACLE_FAULT=1: prove the gate can fail. Corrupt one register of the recompiled result, so a
        // run with it must report a MISMATCH for every probe - the check that this is a gate and not a stamp.
        static const bool fault = getenv("R2_ORACLE_FAULT") != nullptr;
        if (fault) work.r[3] ^= 1u;
    } else {
        hitLimit = true;
    }
    s_escape = nullptr;
    g_oracleIo = false;
    gxGatherRefresh();  // RL-NX (9.53)
    u64 h = 0;
    // RL-NX (Stage 1): the generated code writes back only what the PowerPC ABI keeps live across a call (emit.cpp
    // g_abiOn), so the scratch registers of the two passes differ by design. Both results get the scratch slots of
    // the start state before hashing: r0, r5-r12, f0, f3-f13 (both halves), cr0/1/5/6/7 and CTR. Memory, the live
    // registers, LR, XER and FPSCR are compared as before. R2_ORACLE_ABI=0 compares every register again.
    static const bool abiCmp = [] {
        const char* e = getenv("R2_ORACLE_ABI");
        return !(e && *e == '0');
    }();
    if (!hitLimit && abiCmp) {
        work.r[0] = start.r[0];
        for (int i = 5; i <= 12; i++) work.r[i] = start.r[i];
        for (int i = 0; i <= 13; i++)
            if (i != 1 && i != 2) work.f[i] = start.f[i], work.ps1[i] = start.ps1[i];
        for (int i : {0, 1, 5, 6, 7}) work.cr[i] = start.cr[i];
        work.ctr = start.ctr;
    }
    // RL-NX (Stage 8): the 64 KiB below the entry stack pointer are dead once the probed function has returned, and
    // r2recomp's ABI2 lets the Ctx copies of callee-saved registers go stale (emit.cpp g_abi2), so the recompiled
    // pass saves different (stale) values into the frames of the calls it made. Both passes get that range zeroed.
    if (!hitLimit && abiCmp) {
        const u32 sp = start.r[1];
        if (inArena(sp) && (sp & kGuestRamMask) >= kChunk)
            memset(s_scratch + ((sp & kGuestRamMask) - kChunk), 0, kChunk);
    }
    if (!hitLimit) {
        h = stateHash(work, s_scratch, lockedCachePtr());
        const int k = interpreted ? 0 : 1;  // RL-NX (9.55): for the mismatch report
        s_outCtx[k] = work;
        for (u32 i = 0; i < kChunks; i++) s_chunks[k][i] = XXH3_64bits(s_scratch + size_t(i) * kChunk, kChunk);
    }
    g_ram = live;
    memcpy(g_vmRead, savedR, sizeof savedR);  // RL-NX (9.55): the live TLB back, no scratch pointer survives
    memcpy(g_vmWrite, savedW, sizeof savedW);
    memcpy(lockedCachePtr(), s_locked, kLockedCacheLen);
    return h;
}

}  // namespace

void oracleInit() {
    const char* v = getenv("R2_ORACLE");
    if (!v || !*v || *v == '0') return;
    s_budget = u32(atoi(v));
    if (!s_budget) return;
    s_on = true;  // the 256 MiB scratch is taken on the first probe, once the guest arena exists
#if defined(RL_NX_ORACLE)
    logf("oracle", "%u probe(s) armed: interpreter vs recompiled, same Ctx and memory, devices muted", s_budget);
#else
    logf("oracle", "R2_ORACLE is set but this build has no probe hook: RL_NX_ORACLE=1 switch/build.sh 8");
#endif
}

// Emitted at every guest function entry of a probe build (emit.cpp), after the trace hook.
void oracleProbe(u32 pc, Ctx& c) {
    if (!s_on || s_budget == 0 || s_inProbe) return;
    // Let the guest get past bring-up before touching it: the first entries run before the guest arena
    // exists and are not worth the 256 MiB copy (an earlier build that probed from entry hung in a run
    // with no log at all).
    static u32 s_seen = 0;
    if (++s_seen <= 256) return;
    // RL-NX (9.55): a sweep instead of the first few entries. R2_ORACLE_EVERY=<k> probes only every k-th entry,
    // and each function is probed once, so a budget of a few hundred covers a few hundred different functions
    // across the run (the locals v2 emitter is gated on this).
    static const u32 every = [] {
        const char* e = getenv("R2_ORACLE_EVERY");
        const u32 k = e ? u32(atoi(e)) : 1u;
        return k ? k : 1u;
    }();
    // RL-NX (9.58): R2_ORACLE_ONLY=<hex>[,<hex>...] probes just those functions (their first entry after bring-up),
    // for code a sweep does not reach - the movie decoder runs only on the intro and the level-select screen.
    static const std::unordered_set<u32> s_only = [] {
        std::unordered_set<u32> set;
        if (const char* e = getenv("R2_ORACLE_ONLY"))
            for (const char* p = e; *p;) {
                char* end = nullptr;
                const unsigned long a = strtoul(p, &end, 16);
                if (end == p) break;
                set.insert(u32(a));
                p = *end ? end + 1 : end;
            }
        return set;
    }();
    if (!s_only.empty()) {
        if (!s_only.count(pc)) return;
    } else if (s_seen % every) {
        return;
    }
    static std::unordered_set<u32> s_done;
    if (!s_done.insert(pc).second) return;
    if (!s_scratch) {
        captureVmCode();  // RL-NX (9.55): before anything can overwrite the image
        s_scratch = static_cast<u8*>(calloc(1, kGuestRamSize + kArenaSlack));  // RL-NX (Stage 3): + slack (ctx.h inArena)
        s_locked = static_cast<u8*>(malloc(kLockedCacheLen));
        if (!s_scratch || !s_locked) {
            logf("oracle", "no memory for the 256 MiB scratch arena: oracle test off");
            s_on = false;
            return;
        }
    }
    s_inProbe = true;
    memcpy(s_locked, lockedCachePtr(), kLockedCacheLen);
    bool hitA = false, hitB = false;
    g_oracleTaint = false;
    g_oracleTb = tbNow();
    logf("oracle", "probing %08X", pc);  // RL-NX (9.55): a hang inside a probe names its function  // RL-NX (9.55): both passes see the same clock
    const u64 hA = runOne(c, pc, true, hitA);
    const u64 hB = hitA || g_oracleTaint ? 0ull : runOne(c, pc, false, hitB);
    if (g_oracleTaint) {  // RL-NX (9.55): a pass needed the pager or idled; the result means nothing
        ++s_skip;
        logf("oracle", "%08X skipped: it needs the pager or an idle wait, which cannot run inside a probe", pc);
    } else if (hitA || hitB) {
        ++s_skip;
        if (s_skip <= 3)
            logf("oracle", "%08X skipped: %s did not return inside %llu steps", pc, hitA ? "the interpreter" : "the recompiled body",
                 (unsigned long long)kSteps);
    } else {
        --s_budget;
        if (hA == hB) {
            ++s_pass;
            logf("oracle", "%08X ok (interpreter %016llx) - %llu passed, %llu failed, %llu skipped", pc,
                 (unsigned long long)hA, (unsigned long long)s_pass, (unsigned long long)s_fail,
                 (unsigned long long)s_skip);
        } else {
            ++s_fail;
            logf("oracle", "%08X MISMATCH: interpreter %016llx, recompiled %016llx (%llu passed, %llu failed)", pc,
                 (unsigned long long)hA, (unsigned long long)hB, (unsigned long long)s_pass, (unsigned long long)s_fail);
            // RL-NX (9.55): what differs - registers as interpreter/recompiled, then the memory chunks
            const Ctx& a = s_outCtx[0];
            const Ctx& b = s_outCtx[1];
            std::string d;
            char buf[96];
            for (int i = 0; i < 32; i++)
                if (a.r[i] != b.r[i]) snprintf(buf, sizeof buf, " r%d=%08X/%08X", i, a.r[i], b.r[i]), d += buf;
            for (int i = 0; i < 32; i++)
                if (memcmp(&a.f[i], &b.f[i], 8) || memcmp(&a.ps1[i], &b.ps1[i], 8)) {
                    u64 x, y, z, w;
                    memcpy(&x, &a.f[i], 8), memcpy(&y, &b.f[i], 8), memcpy(&z, &a.ps1[i], 8), memcpy(&w, &b.ps1[i], 8);
                    snprintf(buf, sizeof buf, " f%d=%016llx/%016llx ps1=%016llx/%016llx", i, (unsigned long long)x,
                             (unsigned long long)y, (unsigned long long)z, (unsigned long long)w);
                    d += buf;
                }
            for (int i = 0; i < 8; i++)
                if (a.cr[i] != b.cr[i]) snprintf(buf, sizeof buf, " cr%d=%X/%X", i, a.cr[i], b.cr[i]), d += buf;
            const u32 ra[] = {a.lr, a.ctr, a.so, a.ov, a.ca, a.fpscr, a.msr, a.srr0, a.srr1, a.dar, a.dsisr, a.resvAddr};
            const u32 rb[] = {b.lr, b.ctr, b.so, b.ov, b.ca, b.fpscr, b.msr, b.srr0, b.srr1, b.dar, b.dsisr, b.resvAddr};
            static const char* rn[] = {"lr", "ctr", "so", "ov", "ca", "fpscr", "msr", "srr0", "srr1", "dar", "dsisr", "resv"};
            for (int i = 0; i < 12; i++)
                if (ra[i] != rb[i]) snprintf(buf, sizeof buf, " %s=%08X/%08X", rn[i], ra[i], rb[i]), d += buf;
            int nch = 0;
            for (u32 i = 0; i < kChunks; i++)
                if (s_chunks[0][i] != s_chunks[1][i]) {
                    if (nch++ < 6) snprintf(buf, sizeof buf, " mem@%08X", kGuestRamBase + i * kChunk), d += buf;
                }
            if (nch) snprintf(buf, sizeof buf, " (%d chunk(s) of 64 KiB differ)", nch), d += buf;
            logf("oracle", "%08X differs:%s", pc, d.empty() ? " (locked cache only)" : d.c_str());
        }
        if (s_budget == 0) logf("oracle", "budget spent: %llu passed, %llu failed, %llu skipped", (unsigned long long)s_pass,
                                (unsigned long long)s_fail, (unsigned long long)s_skip);
    }
    s_inProbe = false;
}

}  // namespace rt
