// Interpreter for guest code that has no recompiled function: code the game builds or
// decompresses at runtime (Rogue Leader loads small position-independent modules into its
// heap, e.g. the callback called through 0x8000A3D4). Calls out of interpreted code go to
// recompiled functions when they exist. Semantics match src/recompiler/emit.cpp.
#include <cmath>

#include "runtime.h"

namespace rt {
// RL-NX (PERF_PLAN phase 0b-ii): the oracle test runs one guest function here and must not hang on a
// body that never returns; interpretBounded sets the limit and reads back whether it was hit.
u64 g_interpLimit = 0;
bool g_interpHitLimit = false;


namespace {

inline u32 op(u32 w) { return w >> 26; }
inline u32 fD(u32 w) { return (w >> 21) & 31; }
inline u32 fA(u32 w) { return (w >> 16) & 31; }
inline u32 fB(u32 w) { return (w >> 11) & 31; }
inline u32 fC(u32 w) { return (w >> 6) & 31; }
inline s32 simm(u32 w) { return int16_t(w & 0xFFFF); }
inline u32 uimm(u32 w) { return w & 0xFFFF; }
inline u32 xo10(u32 w) { return (w >> 1) & 0x3FF; }
inline u32 xo9(u32 w) { return (w >> 1) & 0x1FF; }
inline u32 xo5(u32 w) { return (w >> 1) & 0x1F; }

u32 rotMask(u32 mb, u32 me) {
    const u32 begin = 0xFFFFFFFFu >> mb;
    const u32 end = me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0;
    const u32 m = begin ^ end;
    return mb <= me ? m : ~m;
}

bool condOk(Ctx& c, u32 bo, u32 bi) {
    if (!(bo & 4)) c.ctr--;
    const bool ctrOk = (bo & 4) || ((c.ctr != 0) != ((bo & 2) != 0));
    const bool crOk = (bo & 0x10) || (c.crbit(bi) == ((bo & 8) != 0));
    return ctrOk && crOk;
}

bool trapCond(u32 to, u32 a, u32 b) {
    return ((to & 16) && s32(a) < s32(b)) || ((to & 8) && s32(a) > s32(b)) || ((to & 4) && a == b) ||
           ((to & 2) && a < b) || ((to & 1) && a > b);
}

void call(Ctx& c, u32 target);

// One guest function: runs until it returns (blr) or tail-calls out.
void run(Ctx& c, u32 pc) {
    for (u64 steps = 0;; steps++) {
        if (g_interpLimit && steps >= g_interpLimit) { g_interpHitLimit = true; return; }
        if ((steps & 0x3FF) == 0) poll(c, pc);
        // RL-NX (9.55): inside an oracle probe, VM-module code comes from the oracle's pristine copy (the pager
        // keeps most of those pages out of memory; the recompiled code never needs them resident)
        u32 w;
        if (!(g_oracleIo && oracleCode(pc, w))) w = rd32(pc);
        const u32 D = fD(w), A = fA(w), B = fB(w), C = fC(w);
        auto& r = c.r;
        auto& f = c.f;
        auto& p1 = c.ps1;
        u32 next = pc + 4;
        auto eaD = [&]() { return (A ? r[A] : 0) + u32(simm(w)); };
        auto eaX = [&]() { return (A ? r[A] : 0) + r[B]; };
        auto rec = [&](u32 v) {
            if (w & 1) c.rec(v);
        };
        switch (op(w)) {
        case 3: if (trapCond(D, r[A], u32(simm(w)))) trap(c, pc); break;
        case 7: r[D] = u32(s32(r[A]) * simm(w)); break;
        case 8: { u64 t = u64(~r[A]) + u32(simm(w)) + 1; r[D] = u32(t); c.ca = u32(t >> 32); break; }
        case 10: c.cmpu((w >> 23) & 7, r[A], uimm(w)); break;
        case 11: c.cmps((w >> 23) & 7, s32(r[A]), simm(w)); break;
        case 12: r[D] = addCarry(c, r[A], u32(simm(w))); break;
        case 13: r[D] = addCarry(c, r[A], u32(simm(w))); c.rec(r[D]); break;
        case 14: r[D] = (A ? r[A] : 0) + u32(simm(w)); break;
        case 15: r[D] = (A ? r[A] : 0) + (uimm(w) << 16); break;
        case 16: {
            s32 bd = int16_t(w & 0xFFFC);
            const u32 t = (w & 2) ? u32(bd) : pc + u32(bd);
            const bool lk = w & 1;
            const bool ok = condOk(c, (w >> 21) & 31, (w >> 16) & 31);
            if (lk) {
                c.lr = pc + 4;
                if (ok && t != pc + 4) call(c, t);
            } else if (ok) {
                next = t;
            }
            break;
        }
        case 17: sc(c, pc); break;
        case 18: {
            s32 li = s32(w & 0x03FFFFFC);
            if (li & 0x02000000) li -= 0x04000000;
            const u32 t = (w & 2) ? u32(li) : pc + u32(li);
            if (w & 1) {
                c.lr = pc + 4;
                call(c, t);
            } else {
                next = t;
            }
            break;
        }
        case 19:
            switch (xo10(w)) {
            case 0: c.cr[(w >> 23) & 7] = c.cr[(w >> 18) & 7]; break;
            case 16: {  // bclr
                const u32 target = c.lr;
                const bool ok = condOk(c, (w >> 21) & 31, (w >> 16) & 31);
                if (w & 1) {
                    c.lr = pc + 4;
                    if (ok) call(c, target);
                } else if (ok) {
                    return;  // function return
                }
                break;
            }
            case 528: {  // bcctr
                const bool ok = condOk(c, ((w >> 21) & 31) | 4, (w >> 16) & 31);
                if (w & 1) {
                    c.lr = pc + 4;
                    if (ok) call(c, c.ctr);
                } else if (ok) {
                    call(c, c.ctr);  // tail call
                    return;
                }
                break;
            }
            case 50: rfi(c, pc);
            case 150: break;
            case 33: c.setcrbit(D, !(c.crbit(A) | c.crbit(B))); break;
            case 129: c.setcrbit(D, c.crbit(A) & !c.crbit(B)); break;
            case 193: c.setcrbit(D, c.crbit(A) ^ c.crbit(B)); break;
            case 225: c.setcrbit(D, !(c.crbit(A) & c.crbit(B))); break;
            case 257: c.setcrbit(D, c.crbit(A) & c.crbit(B)); break;
            case 289: c.setcrbit(D, c.crbit(A) == c.crbit(B)); break;
            case 417: c.setcrbit(D, c.crbit(A) | !c.crbit(B)); break;
            case 449: c.setcrbit(D, c.crbit(A) | c.crbit(B)); break;
            default: illegal(c, pc, w);
            }
            break;
        case 20: { const u32 m = rotMask(C, (w >> 1) & 31); r[A] = (rotl(r[D], B) & m) | (r[A] & ~m); rec(r[A]); break; }
        case 21: { const u32 m = rotMask(C, (w >> 1) & 31); r[A] = rotl(r[D], B) & m; rec(r[A]); break; }
        case 23: { const u32 m = rotMask(C, (w >> 1) & 31); r[A] = rotl(r[D], r[B]) & m; rec(r[A]); break; }
        case 24: r[A] = r[D] | uimm(w); break;
        case 25: r[A] = r[D] | (uimm(w) << 16); break;
        case 26: r[A] = r[D] ^ uimm(w); break;
        case 27: r[A] = r[D] ^ (uimm(w) << 16); break;
        case 28: r[A] = r[D] & uimm(w); c.rec(r[A]); break;
        case 29: r[A] = r[D] & (uimm(w) << 16); c.rec(r[A]); break;
        case 31: {
            const u32 x = xo10(w);
            switch (xo9(w)) {
            case 266: r[D] = r[A] + r[B]; rec(r[D]); goto done31;
            case 10: r[D] = addCarry(c, r[A], r[B]); rec(r[D]); goto done31;
            case 138: r[D] = addeCarry(c, r[A], r[B]); rec(r[D]); goto done31;
            case 234: r[D] = addeCarry(c, r[A], 0xFFFFFFFFu); rec(r[D]); goto done31;
            case 202: r[D] = addeCarry(c, r[A], 0); rec(r[D]); goto done31;
            case 40: r[D] = r[B] - r[A]; rec(r[D]); goto done31;
            case 8: { u64 t = u64(~r[A]) + r[B] + 1; r[D] = u32(t); c.ca = u32(t >> 32); rec(r[D]); goto done31; }
            case 136: r[D] = addeCarry(c, ~r[A], r[B]); rec(r[D]); goto done31;
            case 232: r[D] = addeCarry(c, ~r[A], 0xFFFFFFFFu); rec(r[D]); goto done31;
            case 200: r[D] = addeCarry(c, ~r[A], 0); rec(r[D]); goto done31;
            case 104: r[D] = 0u - r[A]; rec(r[D]); goto done31;
            case 235: r[D] = u32(s32(r[A]) * s32(r[B])); rec(r[D]); goto done31;
            case 75: r[D] = u32((s64(s32(r[A])) * s32(r[B])) >> 32); rec(r[D]); goto done31;
            case 11: r[D] = u32((u64(r[A]) * r[B]) >> 32); rec(r[D]); goto done31;
            case 491: r[D] = divw(s32(r[A]), s32(r[B])); rec(r[D]); goto done31;
            case 459: r[D] = divwu(r[A], r[B]); rec(r[D]); goto done31;
            }
            switch (x) {
            case 0: c.cmps((w >> 23) & 7, s32(r[A]), s32(r[B])); break;
            case 32: c.cmpu((w >> 23) & 7, r[A], r[B]); break;
            case 4: if (D == 31 || trapCond(D, r[A], r[B])) trap(c, pc); break;
            case 19: r[D] = c.getcr(); break;
            case 144: c.mtcrf((w >> 12) & 0xFF, r[D]); break;
            case 512: c.cr[(w >> 23) & 7] = u8((c.getxer() >> 28) & 15); c.so = c.ov = c.ca = 0; break;
            case 20: { const u32 ea = eaX(); c.resvAddr = ea; r[D] = rd32(ea); break; }
            case 150: wr32(eaX(), r[D]); c.cr[0] = u8(2 | c.so); break;
            case 23: r[D] = rd32(eaX()); break;
            case 87: r[D] = rd8(eaX()); break;
            case 279: r[D] = rd16(eaX()); break;
            case 343: r[D] = u32(s32(s16(rd16(eaX())))); break;
            case 534: r[D] = rd32r(eaX()); break;
            case 790: r[D] = rd16r(eaX()); break;
            case 55: { const u32 ea = r[A] + r[B]; r[D] = rd32(ea); r[A] = ea; break; }
            case 119: { const u32 ea = r[A] + r[B]; r[D] = rd8(ea); r[A] = ea; break; }
            case 311: { const u32 ea = r[A] + r[B]; r[D] = rd16(ea); r[A] = ea; break; }
            case 375: { const u32 ea = r[A] + r[B]; r[D] = u32(s32(s16(rd16(ea)))); r[A] = ea; break; }
            case 151: wr32(eaX(), r[D]); break;
            case 215: wr8(eaX(), r[D]); break;
            case 407: wr16(eaX(), r[D]); break;
            case 662: wr32r(eaX(), r[D]); break;
            case 918: wr16r(eaX(), r[D]); break;
            case 183: { const u32 ea = r[A] + r[B]; wr32(ea, r[D]); r[A] = ea; break; }
            case 247: { const u32 ea = r[A] + r[B]; wr8(ea, r[D]); r[A] = ea; break; }
            case 439: { const u32 ea = r[A] + r[B]; wr16(ea, r[D]); r[A] = ea; break; }
            case 535: f[D] = p1[D] = s2d(rd32(eaX())); break;
            case 567: { const u32 ea = r[A] + r[B]; f[D] = p1[D] = s2d(rd32(ea)); r[A] = ea; break; }
            case 599: f[D] = bdbl(rd64(eaX())); break;
            case 631: { const u32 ea = r[A] + r[B]; f[D] = bdbl(rd64(ea)); r[A] = ea; break; }
            case 663: wr32(eaX(), d2s(f[D])); break;
            case 695: { const u32 ea = r[A] + r[B]; wr32(ea, d2s(f[D])); r[A] = ea; break; }
            case 727: wr64(eaX(), dbits(f[D])); break;
            case 759: { const u32 ea = r[A] + r[B]; wr64(ea, dbits(f[D])); r[A] = ea; break; }
            case 983: wr32(eaX(), u32(dbits(f[D]))); break;
            case 597: lswi(c, D, A ? r[A] : 0, B ? B : 32); break;
            case 725: stswi(c, D, A ? r[A] : 0, B ? B : 32); break;
            case 533: lswi(c, D, eaX(), c.xerBytes); break;
            case 661: stswi(c, D, eaX(), c.xerBytes); break;
            case 28: r[A] = r[D] & r[B]; rec(r[A]); break;
            case 60: r[A] = r[D] & ~r[B]; rec(r[A]); break;
            case 444: r[A] = r[D] | r[B]; rec(r[A]); break;
            case 412: r[A] = r[D] | ~r[B]; rec(r[A]); break;
            case 316: r[A] = r[D] ^ r[B]; rec(r[A]); break;
            case 476: r[A] = ~(r[D] & r[B]); rec(r[A]); break;
            case 124: r[A] = ~(r[D] | r[B]); rec(r[A]); break;
            case 284: r[A] = ~(r[D] ^ r[B]); rec(r[A]); break;
            case 24: r[A] = slw(r[D], r[B]); rec(r[A]); break;
            case 536: r[A] = srw(r[D], r[B]); rec(r[A]); break;
            case 792: r[A] = sraw(c, r[D], r[B]); rec(r[A]); break;
            case 824: r[A] = srawi(c, r[D], B); rec(r[A]); break;
            case 26: r[A] = cntlzw(r[D]); rec(r[A]); break;
            case 954: r[A] = u32(s32(s8(r[D]))); rec(r[A]); break;
            case 922: r[A] = u32(s32(s16(r[D]))); rec(r[A]); break;
            case 83: r[D] = c.msr; break;
            case 146: mtmsr(c, r[D], pc); break;
            case 210: mtsr(c, A & 15, r[D]); break;
            case 242: mtsr(c, r[B] >> 28, r[D]); break;
            case 595: r[D] = mfsr(c, A & 15); break;
            case 659: r[D] = mfsr(c, r[B] >> 28); break;
            case 339: {
                const u32 s = ((w >> 16) & 31) | (((w >> 11) & 31) << 5);
                r[D] = s == 8 ? c.lr : s == 9 ? c.ctr : s == 1 ? c.getxer() : (s >= 912 && s <= 919) ? c.gqr[s - 912] : mfspr(c, s);
                break;
            }
            case 467: {
                const u32 s = ((w >> 16) & 31) | (((w >> 11) & 31) << 5);
                if (s == 8) c.lr = r[D];
                else if (s == 9) c.ctr = r[D];
                else if (s == 1) c.setxer(r[D]);
                else if (s >= 912 && s <= 919) c.gqr[s - 912] = r[D];
                else mtspr(c, s, r[D]);
                break;
            }
            case 371: r[D] = mftb(c, ((w >> 16) & 31) | (((w >> 11) & 31) << 5)); break;
            case 54: case 86: case 278: case 246: case 566: case 598: case 854: break;
            case 306: tlbie(c, r[B]); break;
            case 470: dcbi(c, eaX()); break;
            case 982: icbi(c, eaX()); break;
            case 1014: dcbz(c, eaX()); break;
            default: illegal(c, pc, w);
            }
        done31:
            break;
        }
        case 32: r[D] = rd32(eaD()); break;
        case 34: r[D] = rd8(eaD()); break;
        case 40: r[D] = rd16(eaD()); break;
        case 42: r[D] = u32(s32(s16(rd16(eaD())))); break;
        case 33: { const u32 ea = r[A] + u32(simm(w)); r[D] = rd32(ea); r[A] = ea; break; }
        case 35: { const u32 ea = r[A] + u32(simm(w)); r[D] = rd8(ea); r[A] = ea; break; }
        case 41: { const u32 ea = r[A] + u32(simm(w)); r[D] = rd16(ea); r[A] = ea; break; }
        case 43: { const u32 ea = r[A] + u32(simm(w)); r[D] = u32(s32(s16(rd16(ea)))); r[A] = ea; break; }
        case 36: wr32(eaD(), r[D]); break;
        case 38: wr8(eaD(), r[D]); break;
        case 44: wr16(eaD(), r[D]); break;
        case 37: { const u32 ea = r[A] + u32(simm(w)); wr32(ea, r[D]); r[A] = ea; break; }
        case 39: { const u32 ea = r[A] + u32(simm(w)); wr8(ea, r[D]); r[A] = ea; break; }
        case 45: { const u32 ea = r[A] + u32(simm(w)); wr16(ea, r[D]); r[A] = ea; break; }
        case 46: { const u32 ea = eaD(); for (u32 i = D; i < 32; i++) r[i] = rd32(ea + (i - D) * 4); break; }
        case 47: { const u32 ea = eaD(); for (u32 i = D; i < 32; i++) wr32(ea + (i - D) * 4, r[i]); break; }
        case 48: f[D] = p1[D] = s2d(rd32(eaD())); break;
        case 49: { const u32 ea = r[A] + u32(simm(w)); f[D] = p1[D] = s2d(rd32(ea)); r[A] = ea; break; }
        case 50: f[D] = bdbl(rd64(eaD())); break;
        case 51: { const u32 ea = r[A] + u32(simm(w)); f[D] = bdbl(rd64(ea)); r[A] = ea; break; }
        case 52: wr32(eaD(), d2s(f[D])); break;
        case 53: { const u32 ea = r[A] + u32(simm(w)); wr32(ea, d2s(f[D])); r[A] = ea; break; }
        case 54: wr64(eaD(), dbits(f[D])); break;
        case 55: { const u32 ea = r[A] + u32(simm(w)); wr64(ea, dbits(f[D])); r[A] = ea; break; }
        case 56: case 57: case 60: case 61: {
            s32 d = s32(w & 0xFFF);
            if (d & 0x800) d -= 0x1000;
            const bool upd = op(w) == 57 || op(w) == 61;
            const u32 ea = (A || upd ? r[A] : 0) + u32(d);
            if (op(w) >= 60) psqStore(c, D, ea, (w >> 15) & 1, c.gqr[(w >> 12) & 7]);
            else psqLoad(c, D, ea, (w >> 15) & 1, c.gqr[(w >> 12) & 7]);
            if (upd) r[A] = ea;
            break;
        }
        case 59: {
            double v;
            switch (xo5(w)) {
            case 18: v = f[A] / f[B]; break;
            case 20: v = f[A] - f[B]; break;
            case 21: v = f[A] + f[B]; break;
            case 22: v = std::sqrt(f[B]); break;
            case 24: f[D] = p1[D] = fres(f[B]); goto fp59done;
            case 25: v = f[A] * f[C]; break;
            case 28: v = fma_(f[A], f[C], -f[B]); break;
            case 29: v = fma_(f[A], f[C], f[B]); break;
            case 30: v = -fma_(f[A], f[C], -f[B]); break;
            case 31: v = -fma_(f[A], f[C], f[B]); break;
            default: illegal(c, pc, w);
            }
            f[D] = p1[D] = rsingle(v);
        fp59done:
            if (w & 1) c.setcr1();
            break;
        }
        case 63: {
            switch (xo5(w)) {
            case 18: f[D] = f[A] / f[B]; goto fp63done;
            case 20: f[D] = f[A] - f[B]; goto fp63done;
            case 21: f[D] = f[A] + f[B]; goto fp63done;
            case 22: f[D] = std::sqrt(f[B]); goto fp63done;
            case 23: f[D] = fsel(f[A], f[C], f[B]); goto fp63done;
            case 25: f[D] = f[A] * f[C]; goto fp63done;
            case 26: f[D] = frsqrte(f[B]); goto fp63done;
            case 28: f[D] = fma_(f[A], f[C], -f[B]); goto fp63done;
            case 29: f[D] = fma_(f[A], f[C], f[B]); goto fp63done;
            case 30: f[D] = -fma_(f[A], f[C], -f[B]); goto fp63done;
            case 31: f[D] = -fma_(f[A], f[C], f[B]); goto fp63done;
            }
            switch (xo10(w)) {
            case 0: case 32: c.fcmp((w >> 23) & 7, f[A], f[B]); break;
            case 12: f[D] = p1[D] = rsingle(f[B]); break;
            case 14: f[D] = ctiwBits(fctiw(c, f[B], false)); break;
            case 15: f[D] = ctiwBits(fctiw(c, f[B], true)); break;
            case 38: c.fpscr |= 0x80000000u >> D; break;
            case 70: c.fpscr &= ~(0x80000000u >> D); break;
            case 134: { const u32 sh = 28 - 4 * ((w >> 23) & 7); c.fpscr = (c.fpscr & ~(0xFu << sh)) | (((w >> 12) & 0xF) << sh); break; }
            case 711: {
                const u32 fm = (w >> 17) & 0xFF;
                u32 m = 0;
                for (int i = 0; i < 8; i++)
                    if (fm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
                c.fpscr = (c.fpscr & ~m) | (u32(dbits(f[B])) & m);
                break;
            }
            case 583: f[D] = bdbl(0xFFF8000000000000ull | c.fpscr); break;
            case 64: c.cr[(w >> 23) & 7] = u8((c.fpscr >> (28 - 4 * ((w >> 18) & 7))) & 15); break;
            case 40: f[D] = fneg_(f[B]); break;
            case 72: f[D] = f[B]; break;
            case 136: f[D] = fnabs_(f[B]); break;
            case 264: f[D] = fabs_(f[B]); break;
            default: illegal(c, pc, w);
            }
        fp63done:
            if ((w & 1) && op(w) == 63) c.setcr1();
            break;
        }
        case 4: {
            const double a0 = f[A], a1 = p1[A], b0 = f[B], b1 = p1[B], c0 = f[C], c1 = p1[C];
            auto set = [&](double x, double y) { f[D] = x; p1[D] = y; };
            auto rs = [](double v) { return rsingle(v); };
            switch (xo10(w)) {
            case 0: case 32: c.fcmp((w >> 23) & 7, a0, b0); goto psdone;
            case 64: case 96: c.fcmp((w >> 23) & 7, a1, b1); goto psdone;
            case 40: set(fneg_(b0), fneg_(b1)); goto psdone;
            case 72: set(b0, b1); goto psdone;
            case 136: set(fnabs_(b0), fnabs_(b1)); goto psdone;
            case 264: set(fabs_(b0), fabs_(b1)); goto psdone;
            case 528: set(a0, b0); goto psdone;
            case 560: set(a0, b1); goto psdone;
            case 592: set(a1, b0); goto psdone;
            case 624: set(a1, b1); goto psdone;
            case 1014: dcbzl(c, eaX()); goto psdone;
            }
            switch (xo5(w)) {
            case 10: set(rs(a0 + b1), c1); goto psdone;
            case 11: set(c0, rs(a0 + b1)); goto psdone;
            case 12: set(rs(a0 * c0), rs(a1 * c0)); goto psdone;
            case 13: set(rs(a0 * c1), rs(a1 * c1)); goto psdone;
            case 14: set(rs(fma_(a0, c0, b0)), rs(fma_(a1, c0, b1))); goto psdone;
            case 15: set(rs(fma_(a0, c1, b0)), rs(fma_(a1, c1, b1))); goto psdone;
            case 18: set(rs(a0 / b0), rs(a1 / b1)); goto psdone;
            case 20: set(rs(a0 - b0), rs(a1 - b1)); goto psdone;
            case 21: set(rs(a0 + b0), rs(a1 + b1)); goto psdone;
            case 23: set(fsel(a0, c0, b0), fsel(a1, c1, b1)); goto psdone;
            case 24: set(fres(b0), fres(b1)); goto psdone;
            case 25: set(rs(a0 * c0), rs(a1 * c1)); goto psdone;
            case 26: set(rs(frsqrte(b0)), rs(frsqrte(b1))); goto psdone;
            case 28: set(rs(fma_(a0, c0, -b0)), rs(fma_(a1, c1, -b1))); goto psdone;
            case 29: set(rs(fma_(a0, c0, b0)), rs(fma_(a1, c1, b1))); goto psdone;
            case 30: set(rs(-fma_(a0, c0, -b0)), rs(-fma_(a1, c1, -b1))); goto psdone;
            case 31: set(rs(-fma_(a0, c0, b0)), rs(-fma_(a1, c1, b1))); goto psdone;
            }
            {
                const u32 x6 = (w >> 1) & 0x3F;
                if (x6 == 6 || x6 == 7 || x6 == 38 || x6 == 39) {
                    const bool st = x6 & 1, upd = x6 >= 38;
                    const u32 ea = upd ? r[A] + r[B] : eaX();
                    if (st) psqStore(c, D, ea, (w >> 10) & 1, c.gqr[(w >> 7) & 7]);
                    else psqLoad(c, D, ea, (w >> 10) & 1, c.gqr[(w >> 7) & 7]);
                    if (upd) r[A] = ea;
                    goto psdone;
                }
            }
            illegal(c, pc, w);
        psdone:
            if ((w & 1) && xo5(w) >= 10) c.setcr1();
            break;
        }
        default:
            illegal(c, pc, w);
        }
        pc = next;
    }
}

void call(Ctx& c, u32 target) {
    if (GuestFn fn = lookupFn(target)) fn(c);
    else run(c, target);
}

}  // namespace

u64 g_interpCalls = 0;

// RL-NX: run one guest function under the interpreter with a step limit (oracle test, oracle.cpp).
void interpretBounded(Ctx& c, u32 addr, u64 limit) {
    g_interpLimit = limit;
    g_interpHitLimit = false;
    run(c, addr);
    g_interpLimit = 0;
}

void interpret(Ctx& c, u32 addr) {
    static bool logged = false;
    static const bool interpLog = envFlag("R2_INTERP_LOG");
    if (!logged || interpLog) {
        logf("interp", "interpreting code at %08X (no recompiled function; lr %08X)", addr, c.lr);
        logged = true;
    }
    g_interpCalls++;
    run(c, addr);
}

}  // namespace rt
