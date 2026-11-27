// RL-NX (Stage 9): native forms of the game's three hottest matrix routines (hardware profile 15:32, level 22:
// f_7FC90610 0.67 ms, f_7FC8F9F0 0.26 ms, f_7FC904DC 0.15 ms a game frame). The recompiled bodies send every
// paired-single load and store through the guest accessors, and the matrices mostly live in the VM region
// (0x7E/0x7F, the TLB call per access) or the locked cache, so each 48-byte matrix cost ~24 out-of-line calls.
// These forms find one host pointer per operand and do the arithmetic on host floats, in the same order and with
// the same fused multiply-adds as the recompiled code (emit.cpp's fmaF / fma_), so results match bit for bit.
// An operand that is not one contiguous mapped host block (a VM page not in the TLB yet, a range crossing a page,
// IO, the uncached view, the gather pipe) runs the recompiled body (<fn>_orig) instead, which also takes any page
// fault the game's handler must see. hooks.txt marks the three as "abi" hooks, so callers keep their call-site
// summaries (emit.cpp) instead of the full spill a native hook otherwise costs. The natives write no registers:
// only volatile ones differ from the recompiled body, and callers never read those after a call.
// The - menu's NATIVE MTX row (R2_NATIVE_MTX, default on) switches them off for A/B tests and oracle runs.
#include "funcs.h"
#include "runtime.h"

#include <atomic>
#include <cmath>
#include <cstring>

#pragma GCC optimize("fp-contract=off")  // RL-NX: no fusing beyond the explicit fmaf calls (the gen code's rounding)

namespace rt {

namespace {
std::atomic<bool> s_nativeMtx{true};

#if defined(__SWITCH__)
// Host pointer for n bytes at guest address a, or null when they are not one mapped host block.
inline u8* rangePtr(u32 a, u32 n, bool write) {
    if (inArena(a)) return arenaPtr(a);  // the arena's slack covers any n here
    if (isVm(a)) {
        if (crossesPage(a, n)) return nullptr;
        u8* p = (write ? g_vmWrite : g_vmRead)[(a - kVmStart) >> 12];
        return p ? p + (a & 0xFFF) : nullptr;
    }
    if (a - kLockedCacheBase <= kLockedCacheLen - n) return lockedCachePtr() + (a - kLockedCacheBase);
    return nullptr;
}
inline float ldf(const u8* p, u32 i) { return bflt(ld32be(p + 4 * i)); }
inline void stf(u8* p, u32 i, float v) { st32be(p + 4 * i, fbits(v)); }
#endif
}  // namespace

void setNativeMtx(bool on) { s_nativeMtx.store(on, std::memory_order_relaxed); }
bool nativeMtxOn() { return s_nativeMtx.load(std::memory_order_relaxed); }

// PSMTXConcat(ab = r3, a = r4, b = r5): ab = a * b for 3x4 matrices. Every load comes before the first store in
// the original too, so ab may alias a or b.
void hle_PSMTXConcat(Ctx& c) {
#if defined(__SWITCH__)
    if (s_nativeMtx.load(std::memory_order_relaxed)) {
        const u8* pa = rangePtr(c.r[4], 48, false);
        const u8* pb = rangePtr(c.r[5], 48, false);
        u8* pab = rangePtr(c.r[3], 48, true);
        if (pa && pb && pab) [[likely]] {
            float a[12], b[12], o[12];
            for (u32 i = 0; i < 12; i++) a[i] = ldf(pa, i), b[i] = ldf(pb, i);
            static const float kUnit[2] = {0.0f, 1.0f};  // the routine's constant at 0x800A8D10
            for (u32 i = 0; i < 3; i++) {
                for (u32 j = 0; j < 4; j++) {
                    float x = b[j] * a[4 * i];
                    x = std::fmaf(b[4 + j], a[4 * i + 1], x);
                    x = std::fmaf(b[8 + j], a[4 * i + 2], x);
                    if (j >= 2) x = std::fmaf(kUnit[j - 2], a[4 * i + 3], x);
                    o[4 * i + j] = x;
                }
            }
            for (u32 i = 0; i < 12; i++) stf(pab, i, o[i]);
            return;
        }
    }
#endif
    f_7FC90610_orig(c);
}

// PSMTXMultVec(m = r4, src = r5, dst = r3): dst = m * (src, 1). src is read before dst is written.
void hle_PSMTXMultVec(Ctx& c) {
#if defined(__SWITCH__)
    if (s_nativeMtx.load(std::memory_order_relaxed)) {
        const u8* pm = rangePtr(c.r[4], 48, false);
        const u8* pv = rangePtr(c.r[5], 12, false);
        u8* pd = rangePtr(c.r[3], 12, true);
        if (pm && pv && pd) [[likely]] {
            const float v0 = ldf(pv, 0), v1 = ldf(pv, 1), v2 = ldf(pv, 2);
            float o[3];
            for (u32 i = 0; i < 3; i++) {
                const float t0 = std::fmaf(v2, ldf(pm, 4 * i + 2), ldf(pm, 4 * i) * v0);    // ps_mul + ps_madd, ps0
                const float t1 = std::fmaf(1.0f, ldf(pm, 4 * i + 3), ldf(pm, 4 * i + 1) * v1);  // ps1 (psq_l w=1)
                o[i] = t0 + t1;                                                                 // ps_sum0
            }
            for (u32 i = 0; i < 3; i++) stf(pd, i, o[i]);
            return;
        }
    }
#endif
    f_7FC8F9F0_orig(c);
}

// The rigid inverse (dst = r3, src = r4): copy src to dst unless they are the same, transpose the 3x3 part in
// place, then the translation becomes -(R^T t) row by row. The copy and the swaps move raw bits (lwz/stw, lfs/stfs).
void hle_PSMTXInvRigid(Ctx& c) {
#if defined(__SWITCH__)
    if (s_nativeMtx.load(std::memory_order_relaxed)) {
        const u32 dst = c.r[3], src = c.r[4];
        u8* pd = rangePtr(dst, 48, true);
        const u8* ps = src == dst ? pd : rangePtr(src, 48, false);
        if (pd && ps) [[likely]] {
            u32 m[12];
            for (u32 i = 0; i < 12; i++) m[i] = ld32be(ps + 4 * i);
            std::swap(m[1], m[4]);
            std::swap(m[2], m[8]);
            std::swap(m[6], m[9]);
            const float t0 = bflt(m[3]), t1 = bflt(m[7]), t2 = bflt(m[11]);
            for (u32 i = 0; i < 3; i++) {
                const float a = bflt(m[4 * i]) * t0;      // ps_mul, ps0 (rounded to single)
                const float b = bflt(m[4 * i + 1]) * t1;  // ps_mul, ps1
                const double d = std::fma(double(bflt(m[4 * i + 2])), double(t2), double(a));  // fmadd (double)
                const float r = float(d + double(b));     // ps_sum0
                m[4 * i + 3] = fbits(-r);                 // fneg, stfs
            }
            for (u32 i = 0; i < 12; i++) st32be(pd + 4 * i, m[i]);
            return;
        }
    }
#endif
    f_7FC904DC_orig(c);
}

}  // namespace rt
