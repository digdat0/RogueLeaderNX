// C++ emitter: one C++ function per guest entry point.
#include "emit.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdlib>  // RL-NX: getenv
#include <cstdio>
#include <cstring>  // RL-NX (9.55): strlen
#include <bitset>   // RL-NX (9.57): call-site summaries
#include <fstream>
#include <map>

#include "ppc.h"

using namespace ppc;

namespace {

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// RL-NX: with g_locals the guest GPRs are emitted as function locals (r0..r31), so the host compiler
// can keep them in registers between statements instead of reloading a Ctx field per instruction.
// R2RECOMP_NOLOCALS=1 restores the old c.r[] form, for an A/B build of the generated code.
//
// RL-NX (9.55, locals v2): the register model is rebuilt. v1 (9.45) tracked "dirty" registers in text order,
// which is wrong across control flow: the loop back-edge cleared the set inside `if (g_irqPending)`, and a
// forward goto past a flushing call reached a later return with nothing spilled - modified registers were
// silently lost. v2 has no dirty tracking at all:
//   * every guest slot the body mentions is a local: GPRs r0..r31, FPRs f0..f31 and their ps1 halves p0..p31,
//     the CR fields cr0..cr7 and CTR (the old form used c.f[], c.ps1[], c.cr[] and c.ctr in memory);
//   * at every point where Ctx must be coherent (before a call that may run guest code or read Ctx, before
//     every return, inside the interrupt check) the markers below expand to a store of every slot the function
//     WRITES anywhere, and after such a call to a reload of every slot it MENTIONS. Storing a slot that did
//     not change rewrites the same value, so this is correct on every path without knowing which path ran;
//   * r1 (the stack pointer) is written through to c.r[1] on every change. A VM page miss inside an ordinary
//     load (rdSlow -> vmMiss -> pageFault) builds the game's DSI frame below c.r[1] with no spill, and a stale
//     r1 would put that frame on top of the current function's own stack frame;
//   * the guest RAM base is read once per function into the local M and passed to the accessors, so a guest
//     store (a byte store that may alias any global) no longer forces g_ram to be reloaded.
bool g_locals = true;
std::string R(u32 n) { return g_locals ? fmt("r%u", n) : fmt("c.r[%u]", n); }
// Marker left where a call may have changed guest registers; filled in once the function's whole
// body is known and its touched-register set is final.
const char kReloadTok = '\x01';
const char kSpillTok = '\x02';  // RL-NX (9.55): store every written slot to Ctx here
std::string F(u32 n) { return g_locals ? fmt("f%u", n) : fmt("c.f[%u]", n); }    // RL-NX (9.55): FPR local
std::string P(u32 n) { return g_locals ? fmt("p%u", n) : fmt("c.ps1[%u]", n); }  // RL-NX (9.55): ps1 local
std::string CR(u32 n) { return g_locals ? fmt("cr%u", n) : fmt("c.cr[%u]", n); } // RL-NX (9.55): CR field local
std::string CTR() { return g_locals ? "ctr" : "c.ctr"; }                          // RL-NX (9.55): CTR local
// RL-NX (9.55): one CR bit as 0/1, and the statement that sets it
std::string crBit(u32 b) { return fmt("((%s >> %u) & 1)", CR(b >> 2).c_str(), 3 - (b & 3)); }
std::string setCrBit(u32 b, const std::string& v) {
    const u32 m = 1u << (3 - (b & 3));
    return fmt("%s = u8((%s & 0x%02Xu) | ((%s) ? 0x%02Xu : 0u));", CR(b >> 2).c_str(), CR(b >> 2).c_str(), ~m & 0xFF,
               v.c_str(), m);
}
// RL-NX (Stage 11): XER[SO] as a constant 0. The game never sets SO: there is no o-form (addo, mullwo...) in its code
// and its only XER writes are mtxer restores of saved contexts. So every compare and record form ORs in 0 instead of
// loading c.so (one load and an orr per compare). Every mtxer goes through soCheck (ctx.h), which logs once if SO
// ever arrives set; an o-form, if a new build has one, checks too. R2RECOMP_SO0=0 reads c.so as before.
bool g_so0 = true;
std::string SO() { return g_so0 ? "0u" : "c.so"; }
std::string ovTail() { return g_so0 ? " c.so |= c.ov; soCheck(c.so << 31);" : " c.so |= c.ov;"; }
// RL-NX (9.55): CR0 from a recording integer result, CR1 from FPSCR
std::string recStr(const std::string& reg) { return fmt("%s = crS(s32(%s), 0, %s);", CR(0).c_str(), reg.c_str(), SO().c_str()); }
std::string cr1Str() { return CR(1) + " = u8(c.fpscr >> 28);"; }
// RL-NX (9.55): a statement that may run guest code: spill before, reload after (plain in the old form)
std::string callStmt(const std::string& s) {
    return g_locals ? std::string(1, kSpillTok) + s + std::string(1, kReloadTok) : s;
}
std::string retStmt() { return g_locals ? std::string(1, kSpillTok) + "return;" : "return;"; }
std::string hex(u32 v) { return fmt("0x%08Xu", v); }

bool isFloatFpr(u32 n);  // RL-NX (9.58): below
// RL-NX (9.55): a guest slot named by a local (r3, f1, p1, cr0, ctr) and its Ctx field and type.
struct Slot {
    char kind;  // 'r' 'f' 'p' 'c' (CR field) 't' (CTR)
    u32 n;
    bool operator<(const Slot& o) const { return kind != o.kind ? kind < o.kind : n < o.n; }
    std::string local() const {
        switch (kind) {
        case 'r': return fmt("r%u", n);
        case 'f': return fmt("f%u", n);
        case 'p': return fmt("p%u", n);
        case 'c': return fmt("cr%u", n);
        default: return "ctr";
        }
    }
    std::string field() const {
        switch (kind) {
        case 'r': return fmt("c.r[%u]", n);
        case 'f': return fmt("c.f[%u]", n);
        case 'p': return fmt("c.ps1[%u]", n);
        case 'c': return fmt("c.cr[%u]", n);
        default: return "c.ctr";
        }
    }
    const char* type() const {
        if (kind == 'r' || kind == 't') return "u32";
        if (kind == 'c') return "u8";
        return isFloatFpr(n) ? "float" : "double";  // RL-NX (9.58)
    }
};
bool parseSlot(const std::string& id, Slot& s) {
    if (id == "ctr") {
        s = {'t', 0};
        return true;
    }
    size_t k;
    char kind;
    if (id.size() >= 3 && id[0] == 'c' && id[1] == 'r') kind = 'c', k = 2;
    else if (id.size() >= 2 && (id[0] == 'r' || id[0] == 'f' || id[0] == 'p')) kind = id[0], k = 1;
    else return false;
    if (id.size() - k > 2) return false;
    u32 n = 0;
    for (size_t i = k; i < id.size(); i++) {
        if (!std::isdigit((unsigned char)id[i])) return false;
        n = n * 10 + u32(id[i] - '0');
    }
    if (id.size() - k == 2 && id[k] == '0') return false;  // "r05" is not a slot
    if (n >= (kind == 'c' ? 8u : 32u)) return false;
    s = {kind, n};
    return true;
}
// RL-NX (9.58): single-precision FPR typing. An FPR (and its ps1 half) whose every use in a function is single
// precision becomes a `float` local instead of a `double`. The ps and single ops are emitted as before -
// rsingle(double op) - and LLVM folds fptrunc(op(fpext a, fpext b)) to the float op, which is exact for + - * /
// sqrt (53 >= 2*24+2: double rounding is innocuous); the fused ops keep their double fma and one rounding, as
// now. Loads and stores of a float FPR move raw bits (bflt/fbits) instead of s2d/d2s, which also keeps SNaN
// payloads exact. A register stays double when a double-precision op, lfd/stfd, fctiw or mffs/mtfsf touches it,
// or when it is linked by a move (fmr, fneg, ps_mr, merges, ps_sel, ps_sum) to one that does. Two exceptions
// keep the callee-saved FPRs float in ps-heavy code: an stfd to the stack in the entry block before the register
// is written saves the exact entry value from Ctx, and an lfd from the stack followed only by non-FP
// instructions and blr writes Ctx directly and sets a flag that stops the local overwriting it on the way out.
// R2RECOMP_FLOATFPR=0 turns the typing off.
using SlotSet = std::bitset<112>;  // RL-NX (9.57): call-site summaries (declared early for abiMask)
bool g_floatOn = true;
unsigned g_fFloat = 0;    // bit n: f<n>/p<n> are float in the function being emitted
unsigned g_fRestore = 0;  // bit n: the function has an epilogue lfd restore of f<n> (flag rs<n>)
std::set<u32> g_saveInsn, g_restoreInsn;  // the stfd saves / lfd restores, by pc
unsigned long long g_floatRegs = 0, g_floatFns = 0;  // statistics
bool isFloatFpr(u32 n) { return (g_fFloat >> n) & 1; }

// RL-NX (Stage 1): GQR0 as a constant. The SDK sets GQR0 to 0 (plain floats) at boot and the game never changes
// it: its only writes are OSContext restores of a saved 0 and an explicit 0 (gen of 9.58). A psq op through GQR0
// then needs no runtime type test - the float path is chosen at compile time. Every mtspr GQR0 goes through
// gqr0Check (ctx.h), which logs once if a nonzero type ever arrives. R2RECOMP_GQR0=0 reads c.gqr[0] as before.
bool g_gqr0Const = true;
std::string gqrArg(u32 i) { return i == 0 && g_gqr0Const ? std::string("0u") : fmt("c.gqr[%u]", i); }

// RL-NX (Stage 1): ABI-only write-back. Under the PowerPC EABI that every compiled function follows, a caller can
// rely only on r1, r2, r13-r31, f14-f31, cr2-cr4 surviving a call and on r3/r4, f1/f2 (and their ps1 halves) as
// results; r0, r5-r12, f0, f3-f13, cr0/1/5/6/7 and CTR are garbage afterwards. So a function's returns store only
// those slots to Ctx, and a summarised call site reloads only those - the rest stay in the caller's locals.
// Functions that leave through rfi or a saved context (exception handlers, OSLoadContext) keep the full store.
// The oracle compares the same live set (oracle.cpp R2_ORACLE_ABI). R2RECOMP_ABI=0 turns it off.
bool g_abiOn = true;
bool abiLive(const Slot& s) {
    switch (s.kind) {
    case 'r': return s.n <= 4 ? s.n != 0 : s.n >= 13;
    case 'f':
    case 'p': return s.n == 1 || s.n == 2 || s.n >= 14;
    case 'c': return s.n >= 2 && s.n <= 4;
    default: return false;  // CTR
    }
}
// RL-NX (Stage 2): single-precision arithmetic in float. When every FPR an op reads and writes is float-typed
// (typeFprs), the op is emitted on the float locals directly - one host instruction per lane - instead of widening
// each operand to double, computing in double and rounding back (two fcvt per operand per lane before). For add,
// subtract, multiply, divide and square root this is bit-identical: double has 53 >= 2*24+2 bits, so rounding the
// double result to single gives the correctly rounded single result (double rounding is innocuous). Fused
// multiply-add is the one difference - fmaf rounds once where fma+round rounds twice, which differs only in rare
// last-bit cases. R2RECOMP_PSFLOAT=0 keeps the double forms.
bool g_psFloat = true;
// the float form of an expression the double path builds: same text with the float helpers (ctx.h)
std::string toFloatExpr(std::string e) {
    auto sub = [&](const char* a, const char* b) {
        for (size_t k = e.find(a); k != std::string::npos; k = e.find(a, k + std::strlen(b))) e.replace(k, std::strlen(a), b);
    };
    sub("fma_(", "fmaF(");
    sub("fsel(", "fselF(");
    sub("fres(", "fresF(");
    sub("frsqrte(", "frsqrteF(");
    sub("__builtin_sqrt(", "__builtin_sqrtf(");
    sub("rsingle(", "(");
    return e;
}
// a compare of two float-typed FPRs compares the floats (no widening); anything else the doubles
const char* cmpFn(u32 a, u32 b) { return g_psFloat && isFloatFpr(a) && isFloatFpr(b) ? "fcmpvF" : "fcmpv"; }
bool usesTok(const std::string& e0, const std::string& e1, const char* t) {
    return e0.find(t) != std::string::npos || e1.find(t) != std::string::npos;
}

SlotSet abiMask() {
    SlotSet m;
    for (u32 n = 0; n < 32; n++) {
        if (abiLive(Slot{'r', n})) m.set(size_t(n));
        if (abiLive(Slot{'f', n})) m.set(size_t(32 + n)), m.set(size_t(64 + n));
    }
    for (u32 n = 0; n < 8; n++)
        if (abiLive(Slot{'c', n})) m.set(size_t(96 + n));
    return m;
}
// Ctx -> local and local -> Ctx for an FPR slot, by type
std::string slotLoadExpr(char kind, u32 n) {
    const std::string fld = fmt(kind == 'f' ? "c.f[%u]" : "c.ps1[%u]", n);
    return isFloatFpr(n) ? "bflt(d2s(" + fld + "))" : fld;
}
std::string slotStoreStmt(char kind, u32 n) {
    const std::string fld = fmt(kind == 'f' ? "c.f[%u]" : "c.ps1[%u]", n), loc = fmt(kind == 'f' ? "f%u" : "p%u", n);
    if (!isFloatFpr(n)) return fmt(" %s = %s;", fld.c_str(), loc.c_str());
    const std::string st = fmt(" %s = s2d(fbits(%s));", fld.c_str(), loc.c_str());
    if (kind == 'f' && ((g_fRestore >> n) & 1)) return fmt(" if (!rs%u)", n) + st;  // restored by the epilogue lfd
    return st;
}

// RL-NX (9.55): every slot a piece of generated code mentions, and the ones it assigns (`x = ...` that is not
// `==`, or `x--`/`x++`). Identifiers after '.' or '->' are members (c.ctr), numbers are skipped whole (hex in
// comments and constants), so a substring such as "r3" inside "r13" or "0x...r3" is never miscounted - the
// mistake v1's text search made.
void scanSlots(const std::string& b, std::set<Slot>* touched, std::set<Slot>* written) {
    size_t i = 0;
    const size_t n = b.size();
    while (i < n) {
        const unsigned char ch = (unsigned char)b[i];
        if (std::isdigit(ch)) {
            while (i < n && (std::isalnum((unsigned char)b[i]) || b[i] == '_')) i++;
            continue;
        }
        if (!(std::isalpha(ch) || ch == '_')) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && (std::isalnum((unsigned char)b[j]) || b[j] == '_')) j++;
        bool member = false;
        {
            size_t k = i;
            while (k > 0 && b[k - 1] == ' ') k--;
            if (k > 0 && b[k - 1] == '.') member = true;
            if (k > 1 && b[k - 1] == '>' && b[k - 2] == '-') member = true;
        }
        Slot s;
        if (!member && parseSlot(b.substr(i, j - i), s)) {
            if (touched) touched->insert(s);
            size_t k = j;
            while (k < n && b[k] == ' ') k++;
            const bool assign = k < n && b[k] == '=' && !(k + 1 < n && b[k + 1] == '=');
            const bool incdec = k + 1 < n && ((b[k] == '-' && b[k + 1] == '-') || (b[k] == '+' && b[k + 1] == '+'));
            if (written && (assign || incdec)) written->insert(s);
        }
        i = j;
    }
}
// RL-NX (9.55): the generated code passes the function's guest RAM base M to every memory accessor
// (ctx.h's rd32(M, a) etc.), so it is read once per function instead of after every guest store.
// RL-NX (Stage 3): an access whose address is the stack pointer (r1 or r1 + d) takes the stack form (rd32S...,
// ctx.h): no arena test and no slow-path call. R2RECOMP_STACKFAST=0 keeps the tested forms.
bool g_stackFast = true;
// RL-NX (Stage 4): set while a function that loads 0xCC01 with lis (the gather pipe's base, GXWGFifo) is emitted:
// its non-stack wr8/wr16/wr32 become wr8P/wr16P/wr32P (ctx.h), which append a store to 0xCC008000 straight to the
// gather buffer. R2RECOMP_PIPEFAST=0 keeps the plain forms.
bool g_pipeFast = true;
bool g_fnPipe = false;
// RL-NX (Stage 9): set while an instruction listed in vmsites.txt is emitted: its non-stack accessors take the V forms
// (ctx.h: the VM TLB test inline). R2RECOMP_VMSITES=0 keeps the ordinary forms.
bool g_vmInline = true;
bool g_vmSite = false;
// RL-NX (audit 2026-11-27): R2RECOMP_VMFNS=0 ignores the "F <entry>" lines of vmsites.txt (whole functions given the V forms, 510 of
// them, ~24k V forms); only the single instructions the census listed keep them (~5k, as gen-1709). Default on = as v12.
bool g_vmFns = true;
// RL-NX (Stage 11): FASTMEM (R2RECOMP_FASTMEM=1, off by default). Every other access takes the F forms (ctx.h rd32F...),
// which in a -DRL_NX_FASTMEM build add the address to the 4 GiB window and fault when it is not plain RAM (and are the
// ordinary tested forms in any other build). An access stays tested in a function that builds an IO, uncached or
// EFB address itself (lis 0xC000-0xC1FF, 0xC800-0xC8FF, 0xCC00-0xCDFF) and at the sites fmchecked.txt lists (the
// census's other slow accesses); the stack, small-data, VM and gather pipe forms are unchanged.
bool g_fastmem = false;
bool g_fnIo = false;      // the function builds an IO / uncached / EFB address
bool g_fmChecked = false; // set while a tested instruction is emitted
unsigned long long g_fmCount = 0;  // statistics: accesses given the F forms
unsigned long long g_vmSiteCount = 0;  // statistics: instructions given the V forms
std::string withBase(const std::string& s) {
    if (!g_locals) return s;
    static const char* kAcc[] = {"rd8", "rd16", "rd32", "rd64", "rd16r", "rd32r",
                                 "wr8", "wr16", "wr32", "wr64", "wr16r", "wr32r"};
    std::string o;
    o.reserve(s.size() + 32);
    for (size_t i = 0; i < s.size();) {
        bool hit = false;
        if ((s[i] == 'r' || s[i] == 'w') && (i == 0 || !(std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) {
            for (const char* a : kAcc) {
                const size_t L = std::strlen(a);
                if (s.compare(i, L, a) == 0 && i + L < s.size() && s[i + L] == '(') {
                    const size_t r = i + L + 1;
                    const bool stk = g_stackFast && (s.compare(r, 3, "r1,") == 0 || s.compare(r, 3, "r1)") == 0 ||
                                                     s.compare(r, 6, "(r1 + ") == 0);
                    o.append(a);
                    if (stk) o += 'S';  // RL-NX (Stage 3)
                    else if (g_vmSite && L <= 4) o += 'V';  // RL-NX (Stage 9): rd8-rd64 / wr8-wr64 at a census site
                    else if (g_fnPipe && a[0] == 'w' && L <= 4 && a[2] != '6') o += 'P';  // RL-NX (Stage 4): wr8/wr16/wr32
                    else if (g_fastmem && !g_fmChecked) o += 'F', g_fmCount++;  // RL-NX (Stage 11): FASTMEM
                    o.append("(M, ");
                    i += L + 1;
                    hit = true;
                    break;
                }
            }
        }
        if (!hit) o += s[i++];
    }
    return o;
}

// RL-NX (9.57): call-site summaries. Locals v2 spilled every slot the caller writes before every call and
// reloaded every slot it mentions after it (~60 stores and ~75 loads per call in the 00:56 profile: 13 % of guest
// time on Ctx loads/stores, 12 % on stack spills of the locals). Each function now gets two sets, closed over
// everything it can call: the slots it may READ from Ctx (every slot it mentions - its entry loads them all) and
// the slots it may WRITE. A call site to it then stores only the caller's written slots in its read set, and
// reloads only the caller's slots in its write set; the rest stay in the caller's locals, which the callee never
// sees. A function is opaque (both sets full) when it can run guest code we cannot see: an indirect call
// (dispatch), a native hook (hle_), a context switch (RT_SAVECTX_CALL, rfi, sc, trap), a function that returns
// twice, or any other runtime call not known to be pure. An interrupt taken at a safepoint inside a callee is not
// opaque: the handler saves the interrupted registers and restores the same values, and the caller's own copies
// are in its locals either way (R2RECOMP_SUMMARY=strict makes safepoints opaque too, R2RECOMP_SUMMARY=0 turns the
// whole thing off and emits the v2 call sites).
int slotIndex(const Slot& s) {
    switch (s.kind) {
    case 'r': return int(s.n);
    case 'f': return 32 + int(s.n);
    case 'p': return 64 + int(s.n);
    case 'c': return 96 + int(s.n);
    default: return 104;
    }
}
struct Summary {
    SlotSet read, write;
    SlotSet pres;  // RL-NX (Stage 8): callee-saved slots its prologue saves (and its epilogue restores)
    std::set<u32> callees;
    bool opaque = false;
    bool indirect = false;  // RL-NX (Stage 8): calls through a pointer (dispatch), taken as an ABI call
};
std::map<u32, Summary> g_summ;  // by entry; filled by Emitter::write's first pass
bool g_summOn = true;
bool g_summStrict = false;
bool g_summPass = false;   // the first pass: collect, do not expand
std::map<std::string, unsigned> g_opaqueWhy;  // what made functions opaque on their own (statistics)
u32 g_summEntry = 0;       // the function being emitted (first pass)
unsigned long long g_sitesSumm = 0, g_sitesFull = 0, g_spillSaved = 0, g_reloadSaved = 0;  // statistics
unsigned long long g_retSaved = 0;  // RL-NX (Stage 1): return stores left out by the ABI write-back
// RL-NX (Stage 8): the ABI call model for callee-saved registers (MKDD-NX gen_c.py: r14-r31 are never spilled at a
// call). A function whose prologue saves a callee-saved slot to its stack frame (stw/stmw r14-r31, stfd f14-f31,
// psq_st for both halves, all with rA = r1) reads the caller's value only to save it and restores it before it
// returns, so its summary neither reads nor writes that slot: a summarised call site no longer stores the caller's
// r14-r31/f14-f31 before the call and no longer reloads them after it. The Ctx copies of those slots may then be
// stale, and that is consistent: the true value lives in the local of the innermost function that touches it, the
// stale copy is saved and restored unchanged by everything below, and every caller that needs Ctx to be exact
// (dispatch, an opaque callee, the interpreter) still spills in full first. Only slots that a function does not
// save keep its own reads and writes, and a callee's writes reach its caller's summary only for slots the caller
// does not save itself. lfd restores only ps0, so a ps1 half counts as saved only after a psq_st. CR fields are
// unchanged (mfcr saves go through a GPR). R2RECOMP_ABI2=0 turns it off.
bool g_abi2 = true;
unsigned long long g_presSlots = 0;  // statistics: saved callee-saved slots over all functions
SlotSet savedSlots(const Image& img, const Function& f) {
    SlotSet m;
    if (f.insns.empty() || f.insns.front() != f.entry) return m;  // entered past its prologue: no saves known
    for (const u32 pc : f.insns) {
        const u32 w = img.word(pc), op = w >> 26, rs = (w >> 21) & 31, ra = (w >> 16) & 31;
        if (ra != 1) continue;
        if ((op == 36 || op == 37) && rs >= 14) m.set(size_t(rs));                          // stw, stwu
        else if (op == 47) for (u32 n = rs < 14 ? 14 : rs; n < 32; n++) m.set(size_t(n));  // stmw
        else if ((op == 54 || op == 55) && rs >= 14) m.set(size_t(32 + rs));                 // stfd, stfdu
        else if ((op == 60 || op == 61) && rs >= 14) {                                        // psq_st, psq_stu
            m.set(size_t(32 + rs));
            if (!((w >> 15) & 1)) m.set(size_t(64 + rs));  // W = 0: both halves
        }
    }
    return m;
}

bool isHex8(const std::string& t, size_t k) {
    if (k + 8 > t.size()) return false;
    for (size_t i = k; i < k + 8; i++)
        if (!std::isxdigit((unsigned char)t[i]) || std::islower((unsigned char)t[i])) return false;
    return true;
}
u32 hex8(const std::string& t, size_t k) { return u32(std::strtoul(t.substr(k, 8).c_str(), nullptr, 16)); }

// Every runtime call in a body ("name(c," / "name(c)"): direct guest calls are collected, the known pure helpers
// and the interrupt check are skipped, anything else makes the function opaque.
void scanCalls(const std::string& b, Summary& out) {
    static const char* kSkip[] = {"addCarry", "addeCarry", "addOv", "sraw", "srawi", "fctiw", "psqLd", "psqSt", "psqLdF", "psqStF",
                                  "psqLdV", "psqStV", "psqLdFV", "psqStFV",  /* RL-NX (Stage 9) */
                                  "fcmpv", "fcmpvF", "mfspr", "mtspr", "mftb", "mfsr", "mtsr", "dcbi", "icbi", "dcbz",
                                  "dcbzl", "tlbie", "traceEnter", "illegal", "oracleProbe", "lswi", "stswi"};
    for (size_t k = b.find("(c"); k != std::string::npos; k = b.find("(c", k + 2)) {
        if (k + 2 >= b.size() || !(b[k + 2] == ',' || b[k + 2] == ')')) continue;
        size_t a = k;
        while (a > 0 && (std::isalnum((unsigned char)b[a - 1]) || b[a - 1] == '_')) a--;
        const std::string name = b.substr(a, k - a);
        if (name.empty()) continue;
        if (name.size() == 10 && name[0] == 'f' && name[1] == '_' && isHex8(name, 2)) {
            out.callees.insert(hex8(name, 2));
            continue;
        }
        bool skip = false;
        for (const char* p : kSkip) skip = skip || name == p;
        if (!g_summStrict && (name == "safepoint" || name == "idleWait" || name == "mtmsr")) skip = true;
        // RL-NX (Stage 8): with ABI2 a call through a pointer reads the argument registers and writes the volatile ones
        // (Emitter::write adds both), and keeps every callee-saved slot, as any compiled C/C++ target does. The site
        // itself still spills and reloads in full; only the function's summary stops being opaque.
        if (g_abi2 && name == "dispatch") {
            out.indirect = true;
            skip = true;
        }
        if (!skip) {
            out.opaque = true;  // dispatch, hle_*, rfi, sc, trap, RT_SAVECTX_CALL, ...
            g_opaqueWhy[name.compare(0, 4, "hle_") ? name : std::string("hle_*")]++;
        }
    }
}

// The statement between a spill and a reload marker, if it is exactly one direct call: its callee's summary.
const Summary* directCallee(const std::string& stmt) {
    size_t a = 0, b = stmt.size();
    while (a < b && (stmt[a] == ' ' || stmt[a] == '\t')) a++;
    while (b > a && (stmt[b - 1] == ' ' || stmt[b - 1] == '\t')) b--;
    const std::string t = stmt.substr(a, b - a);
    if (t.size() != 14 || t.compare(0, 2, "f_") || !isHex8(t, 2) || t.compare(10, 4, "(c);")) return nullptr;
    auto it = g_summ.find(hex8(t, 2));
    if (it == g_summ.end() || it->second.opaque) return nullptr;
    return &it->second;
}

// RL-NX (9.58): decide which FPRs of a function are float (see g_floatOn).
void typeFprs(const Image& img, const Function& f) {
    g_fFloat = g_fRestore = 0;
    g_saveInsn.clear();
    g_restoreInsn.clear();
    if (!g_floatOn || !g_locals) return;
    int parent[32];
    bool dbl[32] = {}, used[32] = {};
    for (int i = 0; i < 32; i++) parent[i] = i;
    auto find = [&](int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto link = [&](u32 a, u32 b) { parent[find(int(a))] = find(int(b)); };
    auto isFpOp = [](u32 w) {
        const u32 op = w >> 26;
        if (op == 4 || op == 59 || op == 63 || (op >= 48 && op <= 57) || op == 60 || op == 61) return true;
        if (op == 31) {
            const u32 x = (w >> 1) & 0x3FF;
            return x == 535 || x == 567 || x == 599 || x == 631 || x == 663 || x == 695 || x == 727 || x == 759 || x == 983;
        }
        return false;
    };
    auto isFpStore = [](u32 w) {
        const u32 op = w >> 26;
        if ((op >= 52 && op <= 55) || op == 60 || op == 61) return true;
        if (op == 4) {
            const u32 x6 = (w >> 1) & 0x3F;
            return x6 == 7 || x6 == 39;
        }
        if (op == 31) {
            const u32 x = (w >> 1) & 0x3FF;
            return x == 663 || x == 695 || x == 727 || x == 759 || x == 983;
        }
        return false;
    };
    // the prologue saves: stfd fN, d(r1) in the entry block, before anything else touches fN
    {
        unsigned touched = 0;
        bool started = false;
        for (u32 pc : f.insns) {
            if (pc == f.entry) started = true;
            if (!started) continue;
            if (pc != f.entry && f.labels.count(pc)) break;
            const u32 w = img.word(pc), op = w >> 26;
            if (op == 16 || op == 18 || op == 19) break;  // any branch ends the entry block
            const u32 D = rD(w);
            if (op == 54 && rA(w) == 1 && !((touched >> D) & 1)) {
                g_saveInsn.insert(pc);
                continue;
            }
            if (isFpOp(w)) touched |= 1u << D;  // (over-approximate: any FP instruction naming fD)
        }
    }
    // the epilogue restores: lfd fN, d(r1) followed by non-FP instructions (or other restores) up to a blr
    for (size_t i = 0; i < f.insns.size(); i++) {
        const u32 w = img.word(f.insns[i]);
        if ((w >> 26) != 50 || rA(w) != 1) continue;
        bool ok = false;
        for (size_t j = i + 1; j < f.insns.size() && j < i + 64; j++) {
            if (f.labels.count(f.insns[j])) break;
            const u32 v = img.word(f.insns[j]), op = v >> 26;
            if (v == 0x4E800020u) {
                ok = true;
                break;
            }
            if (op == 16 || op == 18 || op == 19) break;
            if (op == 50 && rA(v) == 1 && rD(v) != rD(w)) continue;  // another restore
            if (isFpOp(v)) break;
        }
        if (ok) g_restoreInsn.insert(f.insns[i]);
    }
    for (u32 pc : f.insns) {
        const u32 w = img.word(pc), op = w >> 26;
        const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
        if (!isFpOp(w)) continue;
        auto mark = [&](u32 r) { dbl[r] = true; };
        const u32 x5 = (w >> 1) & 0x1F, x10 = (w >> 1) & 0x3FF;
        if (op == 50 || op == 51 || op == 54 || op == 55) {
            if (!g_saveInsn.count(pc) && !g_restoreInsn.count(pc)) mark(D);
            used[D] = true;
        } else if (op == 31) {
            if (x10 == 599 || x10 == 631 || x10 == 727 || x10 == 759 || x10 == 983) mark(D);
            used[D] = true;
        } else if (op == 63) {
            switch (x5) {
            case 18: case 20: case 21: mark(D), mark(A), mark(B); continue;
            case 22: case 26: mark(D), mark(B); continue;
            case 23: case 28: case 29: case 30: case 31: mark(D), mark(A), mark(B), mark(C); continue;
            case 25: mark(D), mark(A), mark(C); continue;
            }
            switch (x10) {
            case 14: case 15: case 583: mark(D); break;
            case 711: mark(B); break;
            case 40: case 72: case 136: case 264: link(D, B); break;
            }
        } else if (op == 4) {
            switch (x10) {
            case 40: case 72: case 136: case 264: link(D, B); continue;
            case 528: case 560: case 592: case 624: link(D, A), link(D, B); continue;
            case 0: case 32: case 64: case 96: case 1014: continue;
            }
            if (x5 == 23) link(D, B), link(D, C);
            else if (x5 == 10 || x5 == 11) link(D, C);
        }
        (void)used;
    }
    for (int r = 0; r < 32; r++)
        if (dbl[r]) dbl[find(r)] = true;
    for (u32 r = 0; r < 32; r++)
        if (!dbl[find(int(r))]) g_fFloat |= 1u << r;
    for (u32 pc : g_restoreInsn) {
        const u32 n = rD(img.word(pc));
        if (isFloatFpr(n)) g_fRestore |= 1u << n;
    }
}

// rA|0 + d
std::string eaD(u32 w) {
    const s32 d = simm(w);
    if (rA(w) == 0) return hex(u32(d));
    if (d == 0) return R(rA(w));  // RL-NX: through R() so the local, not the stale Ctx slot, is used
    return fmt("(%s + %s)", R(rA(w)).c_str(), hex(u32(d)).c_str());
}
// rA|0 + rB
std::string eaX(u32 w) {
    if (rA(w) == 0) return R(rB(w));
    return fmt("(%s + %s)", R(rA(w)).c_str(), R(rB(w)).c_str());
}

}  // namespace

std::string Emitter::fn(u32 a) const { return fmt("f_%08X", a); }

std::string Emitter::callTo(u32 t, u32 pc, bool needJb) const {
    if (!an_.funcs.count(t)) return fmt("dispatch(c, %s);", hex(t).c_str());
    if (cfg_.saveCtx.count(t) && needJb) return fmt("RT_SAVECTX_CALL(c, %s, jb);", fn(t).c_str());
    (void)pc;
    return fn(t) + "(c);";
}

// Branch condition for BO/BI (CTR decrement emitted separately).
static std::string cond(u32 bo, u32 bi) {
    std::string ctr, crs;
    if (!(bo & 4)) ctr = CTR() + ((bo & 2) ? " == 0" : " != 0");  // RL-NX (9.55): CTR/CR locals
    if (!(bo & 0x10)) {
        const u32 m = 1u << (3 - (bi & 3));
        crs = fmt((bo & 8) ? "(%s & %u)" : "!(%s & %u)", CR(bi >> 2).c_str(), m);
    }
    if (ctr.empty() && crs.empty()) return "true";
    if (ctr.empty()) return crs;
    if (crs.empty()) return ctr;
    return "(" + ctr + ") && " + crs;
}

static std::string trapCond(u32 to, const std::string& a, const std::string& b) {
    std::vector<std::string> t;
    if (to & 16) t.push_back(fmt("s32(%s) < s32(%s)", a.c_str(), b.c_str()));
    if (to & 8) t.push_back(fmt("s32(%s) > s32(%s)", a.c_str(), b.c_str()));
    if (to & 4) t.push_back(fmt("%s == %s", a.c_str(), b.c_str()));
    if (to & 2) t.push_back(fmt("u32(%s) < u32(%s)", a.c_str(), b.c_str()));
    if (to & 1) t.push_back(fmt("u32(%s) > u32(%s)", a.c_str(), b.c_str()));
    if (t.empty()) return "false";
    std::string s;
    for (size_t i = 0; i < t.size(); i++) s += (i ? " || " : "") + t[i];
    return s;
}

bool Emitter::insn(const Function& f, u32 pc, u32 w, std::string& o, bool& usesJb) const {
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { out(o, s); };  // RL-NX: spill/reload aware
    auto recIf = [&](const std::string& reg) {
        if (rc(w)) line(recStr(reg));  // RL-NX (9.55): CR0 local
    };
    auto fpRec = [&]() {
        if (rc(w)) line(cr1Str());
    };
    (void)fpRec;
    auto branchTo = [&](u32 t, const std::string& c) {
        // intra-function goto or tail call into another entry
        const bool intra = f.labels.count(t) && std::binary_search(f.insns.begin(), f.insns.end(), t);
        std::string body;
        if (intra && t <= pc && cfg_.idleLoops.count(t))
            body = callStmt(fmt("idleWait(c, %s);", hex(t).c_str())) + fmt(" goto L_%08X;", t);
        else if (intra) {
            // RL-NX: back edge of a loop. The interrupt branch spills the register locals only when an
            // interrupt is really pending, so a hot iteration pays two instructions, not a full spill.
            // RL-NX (9.55): the spill is the whole written set and the reload the whole touched set (markers).
            if (t <= pc && g_locals) body = fmt("if (g_irqPending) { %s } goto L_%08X;",
                                                callStmt(fmt("safepoint(c, %s);", hex(t).c_str())).c_str(), t);
            else body = (t <= pc ? fmt("poll(c, %s); ", hex(t).c_str()) : std::string()) + fmt("goto L_%08X;", t);
        }
        else body = std::string(g_locals ? std::string(1, kSpillTok) : "") + callTo(t, pc, false) + " return;";
        if (c == "true") line(body);
        else line("if (" + c + ") { " + body + " }");
    };

    switch (op(w)) {
    case 3:  // twi
        line(fmt("if (%s) trap(c, %s);", trapCond(D, R(A), hex(u32(simm(w)))).c_str(), hex(pc).c_str()));
        return true;
    case 4:
        return ps(f, pc, w, o);
    case 7:
        line(fmt("%s = u32(s32(%s) * %d);", R(D).c_str(), R(A).c_str(), simm(w)));
        return true;
    case 8:
        line(fmt("{ u64 t = u64(~%s) + %s + 1; %s = u32(t); c.ca = u32(t >> 32); }", R(A).c_str(),
                 hex(u32(simm(w))).c_str(), R(D).c_str()));
        return true;
    case 10:  // RL-NX (9.55): compares write a CR field local
        line(fmt("%s = crU(%s, %s, %s);", CR(crfD(w)).c_str(), R(A).c_str(), hex(uimm(w)).c_str(), SO().c_str()));  // RL-NX (Stage 11): SO()
        return true;
    case 11:
        line(fmt("%s = crS(s32(%s), %d, %s);", CR(crfD(w)).c_str(), R(A).c_str(), simm(w), SO().c_str()));  // RL-NX (Stage 11): SO()
        return true;
    case 12:
    case 13:
        line(fmt("%s = addCarry(c, %s, %s);", R(D).c_str(), R(A).c_str(), hex(u32(simm(w))).c_str()));
        if (op(w) == 13) line(recStr(R(D)));
        return true;
    case 14:
        if (A == 0) line(fmt("%s = %s;", R(D).c_str(), hex(u32(simm(w))).c_str()));
        else line(fmt("%s = %s + %s;", R(D).c_str(), R(A).c_str(), hex(u32(simm(w))).c_str()));
        return true;
    case 15:
        if (A == 0) line(fmt("%s = %s;", R(D).c_str(), hex(uimm(w) << 16).c_str()));
        else line(fmt("%s = %s + %s;", R(D).c_str(), R(A).c_str(), hex(uimm(w) << 16).c_str()));
        return true;
    case 16: {
        const u32 t = bcTarget(w, pc);
        const u32 bo = BO(w), bi = BI(w);
        if (!(bo & 4)) line(CTR() + "--;");  // RL-NX (9.55): CTR local
        const std::string cnd = cond(bo, bi);
        if (LK(w)) {
            line(fmt("c.lr = %s;", hex(pc + 4).c_str()));
            if (t == pc + 4) return true;  // bcl $+4: reads the PC
            if (cnd == "true") line(callTo(t, pc, true));
            else line("if (" + cnd + ") { " + callTo(t, pc, false) + " }");
            if (cfg_.saveCtx.count(t)) usesJb = true;
            return true;
        }
        branchTo(t, cnd);
        return true;
    }
    case 17:
        line(fmt("sc(c, %s);", hex(pc).c_str()));
        return true;
    case 18: {
        const u32 t = bTarget(w, pc);
        if (LK(w)) {
            line(fmt("c.lr = %s;", hex(pc + 4).c_str()));
            line(callTo(t, pc, true));
            if (cfg_.saveCtx.count(t)) usesJb = true;
            return true;
        }
        branchTo(t, "true");
        return true;
    }
    case 19:
        switch (xo10(w)) {
        case 0:  // mcrf
            line(fmt("%s = %s;", CR(crfD(w)).c_str(), CR(crfS(w)).c_str()));  // RL-NX (9.55)
            return true;
        case 16: {  // bclr
            const u32 bo = BO(w), bi = BI(w);
            if (!(bo & 4)) line(CTR() + "--;");
            const std::string cnd = cond(bo, bi);
            if (LK(w)) {
                line(fmt("{ u32 t = c.lr; c.lr = %s; if (%s) dispatch(c, t); }", hex(pc + 4).c_str(), cnd.c_str()));
                return true;
            }
            // RL-NX (9.55): the spill goes inside the branch, so a conditional return not taken costs nothing
            if (cnd == "true") line(retStmt());
            else line("if (" + cnd + ") { " + retStmt() + " }");
            return true;
        }
        case 528: {  // bcctr
            const u32 bo = BO(w), bi = BI(w);
            const std::string cnd = cond(bo | 4, bi);
            const std::string ctr = CTR();  // RL-NX (9.55): CTR local
            if (LK(w)) {
                line(fmt("c.lr = %s;", hex(pc + 4).c_str()));
                if (cnd == "true") line(fmt("dispatch(c, %s);", ctr.c_str()));
                else line(fmt("if (%s) dispatch(c, %s);", cnd.c_str(), ctr.c_str()));
                return true;
            }
            const std::string sp = g_locals ? std::string(1, kSpillTok) : std::string();
            auto it = f.tables.find(pc);
            if (it != f.tables.end() && cnd == "true") {
                std::string s = "switch (" + ctr + ") {";
                std::set<u32> done;
                for (u32 t : it->second.targets) {
                    if (!done.insert(t).second) continue;
                    s += fmt(" case %s: goto L_%08X;", hex(t).c_str(), t);
                }
                s += fmt(" default: %sdispatch(c, %s); return; }", sp.c_str(), ctr.c_str());
                line(s);
                return true;
            }
            if (cnd == "true") line(fmt("%sdispatch(c, %s); return;", sp.c_str(), ctr.c_str()));
            else line(fmt("if (%s) { %sdispatch(c, %s); return; }", cnd.c_str(), sp.c_str(), ctr.c_str()));
            return true;
        }
        case 50:
            line(fmt("rfi(c, %s);", hex(pc).c_str()));
            return true;
        case 150:  // isync
            return true;
        // RL-NX (9.55): CR logical ops on the CR field locals
        case 33: line(setCrBit(D, "!(" + crBit(A) + " | " + crBit(B) + ")")); return true;
        case 129: line(setCrBit(D, crBit(A) + " & !" + crBit(B))); return true;
        case 193: line(setCrBit(D, crBit(A) + " ^ " + crBit(B))); return true;
        case 225: line(setCrBit(D, "!(" + crBit(A) + " & " + crBit(B) + ")")); return true;
        case 257: line(setCrBit(D, crBit(A) + " & " + crBit(B))); return true;
        case 289: line(setCrBit(D, crBit(A) + " == " + crBit(B))); return true;
        case 417: line(setCrBit(D, crBit(A) + " | !" + crBit(B))); return true;
        case 449: line(setCrBit(D, crBit(A) + " | " + crBit(B))); return true;
        }
        return false;
    case 20: {
        const u32 m = rotMask(C, (w >> 1) & 31);
        line(fmt("%s = (rotl(%s, %u) & %s) | (%s & %s);", R(A).c_str(), R(D).c_str(), B, hex(m).c_str(), R(A).c_str(),
                 hex(~m).c_str()));
        recIf(R(A));
        return true;
    }
    case 21: {
        const u32 m = rotMask(C, (w >> 1) & 31);
        if (B == 0) line(fmt("%s = %s & %s;", R(A).c_str(), R(D).c_str(), hex(m).c_str()));
        else line(fmt("%s = rotl(%s, %u) & %s;", R(A).c_str(), R(D).c_str(), B, hex(m).c_str()));
        recIf(R(A));
        return true;
    }
    case 23: {
        const u32 m = rotMask(C, (w >> 1) & 31);
        line(fmt("%s = rotl(%s, %s) & %s;", R(A).c_str(), R(D).c_str(), R(B).c_str(), hex(m).c_str()));
        recIf(R(A));
        return true;
    }
    case 24:
        if (A == 0 && D == 0 && uimm(w) == 0) return true;  // nop
        line(fmt("%s = %s | %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w)).c_str()));
        return true;
    case 25: line(fmt("%s = %s | %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w) << 16).c_str())); return true;
    case 26: line(fmt("%s = %s ^ %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w)).c_str())); return true;
    case 27: line(fmt("%s = %s ^ %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w) << 16).c_str())); return true;
    case 28:
        line(fmt("%s = %s & %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w)).c_str()));
        line(recStr(R(A)));  // RL-NX (9.55)
        return true;
    case 29:
        line(fmt("%s = %s & %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w) << 16).c_str()));
        line(recStr(R(A)));  // RL-NX (9.55)
        return true;
    case 31:
        return op31(pc, w, o);
    // --- integer loads/stores (D-form) ---
    case 32: line(fmt("%s = rd32(%s);", R(D).c_str(), eaD(w).c_str())); return true;
    case 34: line(fmt("%s = rd8(%s);", R(D).c_str(), eaD(w).c_str())); return true;
    case 40: line(fmt("%s = rd16(%s);", R(D).c_str(), eaD(w).c_str())); return true;
    case 42: line(fmt("%s = u32(s32(s16(rd16(%s))));", R(D).c_str(), eaD(w).c_str())); return true;
    case 33: case 35: case 41: case 43: {
        const char* ld = op(w) == 33 ? "rd32(ea)" : op(w) == 35 ? "rd8(ea)" : op(w) == 41 ? "rd16(ea)" : "u32(s32(s16(rd16(ea))))";
        line(fmt("{ u32 ea = %s + %s; %s = %s; %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(), R(D).c_str(), ld,
                 R(A).c_str()));
        return true;
    }
    case 36: line(fmt("wr32(%s, %s);", eaD(w).c_str(), R(D).c_str())); return true;
    case 38: line(fmt("wr8(%s, %s);", eaD(w).c_str(), R(D).c_str())); return true;
    case 44: line(fmt("wr16(%s, %s);", eaD(w).c_str(), R(D).c_str())); return true;
    case 37: case 39: case 45: {
        const char* st = op(w) == 37 ? "wr32" : op(w) == 39 ? "wr8" : "wr16";
        line(fmt("{ u32 ea = %s + %s; %s(ea, %s); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(), st, R(D).c_str(),
                 R(A).c_str()));
        return true;
    }
    case 46: {  // lmw
        // RL-NX (9.55): one arena test for the whole range (inArenaN), then plain loads; otherwise word by word
        std::string s = fmt("{ u32 ea = %s; if (inArenaN(ea, %u)) [[likely]] { const u8* q_ = arenaPtrM(M, ea);",
                            eaD(w).c_str(), (32 - D) * 4);
        for (u32 r = D; r < 32; r++) s += fmt(" %s = ld32be(q_ + %u);", R(r).c_str(), (r - D) * 4);
        s += " } else {";
        for (u32 r = D; r < 32; r++) s += fmt(" %s = rd32(ea + %u);", R(r).c_str(), (r - D) * 4);
        line(s + " } }");
        return true;
    }
    case 47: {  // stmw
        std::string s = fmt("{ u32 ea = %s; if (inArenaN(ea, %u)) [[likely]] { u8* q_ = arenaPtrM(M, ea);",
                            eaD(w).c_str(), (32 - D) * 4);
        for (u32 r = D; r < 32; r++) s += fmt(" st32be(q_ + %u, %s);", (r - D) * 4, R(r).c_str());
        s += " } else {";
        for (u32 r = D; r < 32; r++) s += fmt(" wr32(ea + %u, %s);", (r - D) * 4, R(r).c_str());
        line(s + " } }");
        return true;
    }
    // --- FP loads/stores ---
    case 48: line(fmt("%s = %s = %s(rd32(%s));", F(D).c_str(), P(D).c_str(), isFloatFpr(D) ? "bflt" : "s2d", eaD(w).c_str())); return true;
    case 49:
        line(fmt("{ u32 ea = %s + %s; %s = %s = %s(rd32(ea)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), P(D).c_str(), isFloatFpr(D) ? "bflt" : "s2d", R(A).c_str()));
        return true;
    case 50:
        if (g_restoreInsn.count(pc) && isFloatFpr(D)) {  // RL-NX (9.58): the epilogue restore goes straight to Ctx
            line(fmt("c.f[%u] = bdbl(rd64(%s)); rs%u = true; (void)%s;", D, eaD(w).c_str(), D, F(D).c_str()));
            return true;
        }
        line(fmt("%s = bdbl(rd64(%s));", F(D).c_str(), eaD(w).c_str())); return true;
    case 51:
        line(fmt("{ u32 ea = %s + %s; %s = bdbl(rd64(ea)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), R(A).c_str()));
        return true;
    case 52: line(fmt("wr32(%s, %s(%s));", eaD(w).c_str(), isFloatFpr(D) ? "fbits" : "d2s", F(D).c_str())); return true;
    case 53:
        line(fmt("{ u32 ea = %s + %s; wr32(ea, %s(%s)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 isFloatFpr(D) ? "fbits" : "d2s", F(D).c_str(), R(A).c_str()));
        return true;
    case 54:
        if (g_saveInsn.count(pc) && isFloatFpr(D)) {  // RL-NX (9.58): the prologue save stores the exact entry value
            line(fmt("wr64(%s, dbits(c.f[%u])); (void)%s;", eaD(w).c_str(), D, F(D).c_str()));
            return true;
        }
        line(fmt("wr64(%s, dbits(%s));", eaD(w).c_str(), F(D).c_str())); return true;
    case 55:
        line(fmt("{ u32 ea = %s + %s; wr64(ea, dbits(%s)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), R(A).c_str()));
        return true;
    case 56: case 57: case 60: case 61: {  // psq_l / psq_lu / psq_st / psq_stu
        s32 d = s32(w & 0xFFF);
        if (d & 0x800) d -= 0x1000;
        const u32 W = (w >> 15) & 1, I = (w >> 12) & 7;
        const bool upd = op(w) == 57 || op(w) == 61;
        const bool st = op(w) >= 60;
        std::string ea = A == 0 && !upd ? hex(u32(d)) : fmt("%s + %s", R(A).c_str(), hex(u32(d)).c_str());
        // RL-NX (9.55): psqLd/psqSt take the FPR locals by reference and the RAM base M
        if (st)
            line(fmt("{ u32 ea = %s; %s(c, M, ea, %s, %s, %s, %s);%s }", ea.c_str(), isFloatFpr(D) ? "psqStF" : "psqSt", W ? "true" : "false", gqrArg(I).c_str(),  // RL-NX (Stage 1): gqrArg
                     F(D).c_str(), P(D).c_str(), upd ? (" " + R(A) + " = ea;").c_str() : ""));
        else
            line(fmt("{ u32 ea = %s; const auto q_ = %s(c, M, ea, %s, %s); %s = q_.a; %s = q_.b;%s }",  // RL-NX (Stage 1): gqrArg
                     ea.c_str(), isFloatFpr(D) ? "psqLdF" : "psqLd", W ? "true" : "false", gqrArg(I).c_str(), F(D).c_str(), P(D).c_str(),
                     upd ? (" " + R(A) + " = ea;").c_str() : ""));
        return true;
    }
    case 59:
        return fp59(w, o);
    case 63:
        return fp63(w, o);
    }
    return false;
}

bool Emitter::op31(u32 pc, u32 w, std::string& o) const {
    const u32 D = rD(w), A = rA(w), B = rB(w);
    auto line = [&](const std::string& s) { out(o, s); };  // RL-NX: spill/reload aware
    auto rec = [&](const std::string& r) {
        if (rc(w)) line(recStr(r));  // RL-NX (9.55)
    };
    const std::string rd = R(D), ra = R(A), rb = R(B);
    // XO-form arithmetic (OE in bit 10)
    const u32 x9 = xo9(w);
    const bool ovf = oe(w);
    auto arith = [&](const std::string& expr, bool ovAdd, const std::string& oa, const std::string& ob) {
        if (ovf && ovAdd) {
            line(fmt("{ u32 a_ = %s, b_ = %s; u32 r_ = %s; addOv(c, a_, b_, r_); %s = r_; }", oa.c_str(), ob.c_str(),
                     expr.c_str(), rd.c_str()));
        } else {
            line(rd + " = " + expr + ";");
        }
        rec(rd);
    };
    switch (x9) {
    case 266: arith(ovf ? "a_ + b_" : ra + " + " + rb, true, ra, rb); return true;           // add
    case 10:  line(fmt("%s = addCarry(c, %s, %s);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;  // addc
    case 138: line(fmt("%s = addeCarry(c, %s, %s);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true; // adde
    case 234: line(fmt("%s = addeCarry(c, %s, 0xFFFFFFFFu);", rd.c_str(), ra.c_str())); rec(rd); return true;   // addme
    case 202: line(fmt("%s = addeCarry(c, %s, 0);", rd.c_str(), ra.c_str())); rec(rd); return true;             // addze
    case 40:  arith(ovf ? "a_ + b_ + 1u" : rb + " - " + ra, true, "~" + ra, rb); return true;  // subf
    case 8:   line(fmt("{ u64 t = u64(~%s) + %s + 1; %s = u32(t); c.ca = u32(t >> 32); }", ra.c_str(), rb.c_str(), rd.c_str())); rec(rd); return true;  // subfc
    case 136: line(fmt("%s = addeCarry(c, ~%s, %s);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;  // subfe
    case 232: line(fmt("%s = addeCarry(c, ~%s, 0xFFFFFFFFu);", rd.c_str(), ra.c_str())); rec(rd); return true;    // subfme
    case 200: line(fmt("%s = addeCarry(c, ~%s, 0);", rd.c_str(), ra.c_str())); rec(rd); return true;              // subfze
    case 104:  // neg
        if (ovf) line(fmt("c.ov = %s == 0x80000000u;%s", ra.c_str(), ovTail().c_str()));  // RL-NX (Stage 11): ovTail
        line(fmt("%s = 0u - %s;", rd.c_str(), ra.c_str()));
        rec(rd);
        return true;
    case 235:  // mullw
        if (ovf) line(fmt("{ s64 p = s64(s32(%s)) * s32(%s); c.ov = p != s32(p);%s }", ra.c_str(), rb.c_str(), ovTail().c_str()));  // RL-NX (Stage 11)
        line(fmt("%s = u32(s32(%s) * s32(%s));", rd.c_str(), ra.c_str(), rb.c_str()));
        rec(rd);
        return true;
    case 75: line(fmt("%s = u32((s64(s32(%s)) * s32(%s)) >> 32);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;
    case 11: line(fmt("%s = u32((u64(%s) * %s) >> 32);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;
    case 491:
        if (ovf) line(fmt("c.ov = %s == 0 || (%s == 0x80000000u && %s == 0xFFFFFFFFu);%s", rb.c_str(), ra.c_str(), rb.c_str(), ovTail().c_str()));  // RL-NX (Stage 11)
        line(fmt("%s = divw(s32(%s), s32(%s));", rd.c_str(), ra.c_str(), rb.c_str()));
        rec(rd);
        return true;
    case 459:
        if (ovf) line(fmt("c.ov = %s == 0;%s", rb.c_str(), ovTail().c_str()));  // RL-NX (Stage 11)
        line(fmt("%s = divwu(%s, %s);", rd.c_str(), ra.c_str(), rb.c_str()));
        rec(rd);
        return true;
    }
    // X-form: rS = bits 21-25, rA = destination for logical ops
    const std::string rs = R(D);
    switch (xo10(w)) {
    // RL-NX (9.55): compares and the CR moves work on the CR field locals
    case 0: line(fmt("%s = crS(s32(%s), s32(%s), %s);", CR(crfD(w)).c_str(), ra.c_str(), rb.c_str(), SO().c_str())); return true;  // RL-NX (Stage 11): SO()
    case 32: line(fmt("%s = crU(%s, %s, %s);", CR(crfD(w)).c_str(), ra.c_str(), rb.c_str(), SO().c_str())); return true;
    case 4:
        if (D == 31) line(fmt("trap(c, %s);", hex(pc).c_str()));
        else line(fmt("if (%s) trap(c, %s);", trapCond(D, ra, rb).c_str(), hex(pc).c_str()));
        return true;
    case 19: {
        std::string s = rd + " = crPack(";
        for (u32 i = 0; i < 8; i++) s += CR(i) + (i < 7 ? ", " : ");");
        line(s);
        return true;
    }
    case 144: {
        const u32 fm = (w >> 12) & 0xFF;
        std::string s = "{ const u32 v_ = " + rs + ";";
        for (u32 i = 0; i < 8; i++)
            if (fm & (0x80u >> i)) s += fmt(" %s = u8((v_ >> %u) & 15);", CR(i).c_str(), 28 - 4 * i);
        line(s + " }");
        return true;
    }
    case 512: line(fmt("%s = u8((c.getxer() >> 28) & 15); c.so = c.ov = c.ca = 0;", CR(crfD(w)).c_str())); return true;
    case 20: line(fmt("{ u32 ea = %s; c.resvAddr = ea; %s = rd32(ea); }", eaX(w).c_str(), rd.c_str())); return true;
    case 150: line(fmt("{ u32 ea = %s; wr32(ea, %s); %s = u8(2 | %s); }", eaX(w).c_str(), rs.c_str(), CR(0).c_str(), SO().c_str())); return true;  // RL-NX (Stage 11): SO()
    // indexed loads/stores
    case 23: line(fmt("%s = rd32(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 87: line(fmt("%s = rd8(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 279: line(fmt("%s = rd16(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 343: line(fmt("%s = u32(s32(s16(rd16(%s))));", rd.c_str(), eaX(w).c_str())); return true;
    case 534: line(fmt("%s = rd32r(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 790: line(fmt("%s = rd16r(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 55: case 119: case 311: case 375: {
        const char* ld = xo10(w) == 55 ? "rd32(ea)" : xo10(w) == 119 ? "rd8(ea)" : xo10(w) == 311 ? "rd16(ea)" : "u32(s32(s16(rd16(ea))))";
        line(fmt("{ u32 ea = %s + %s; %s = %s; %s = ea; }", ra.c_str(), rb.c_str(), rd.c_str(), ld, ra.c_str()));
        return true;
    }
    case 151: line(fmt("wr32(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 215: line(fmt("wr8(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 407: line(fmt("wr16(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 662: line(fmt("wr32r(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 918: line(fmt("wr16r(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 183: case 247: case 439: {
        const char* st = xo10(w) == 183 ? "wr32" : xo10(w) == 247 ? "wr8" : "wr16";
        line(fmt("{ u32 ea = %s + %s; %s(ea, %s); %s = ea; }", ra.c_str(), rb.c_str(), st, rs.c_str(), ra.c_str()));
        return true;
    }
    case 535: line(fmt("%s = %s = %s(rd32(%s));", F(D).c_str(), P(D).c_str(), isFloatFpr(D) ? "bflt" : "s2d", eaX(w).c_str())); return true;
    case 567: line(fmt("{ u32 ea = %s + %s; %s = %s = %s(rd32(ea)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), P(D).c_str(), isFloatFpr(D) ? "bflt" : "s2d", ra.c_str())); return true;
    case 599: line(fmt("%s = bdbl(rd64(%s));", F(D).c_str(), eaX(w).c_str())); return true;
    case 631: line(fmt("{ u32 ea = %s + %s; %s = bdbl(rd64(ea)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), ra.c_str())); return true;
    case 663: line(fmt("wr32(%s, %s(%s));", eaX(w).c_str(), isFloatFpr(D) ? "fbits" : "d2s", F(D).c_str())); return true;
    case 695: line(fmt("{ u32 ea = %s + %s; wr32(ea, %s(%s)); %s = ea; }", ra.c_str(), rb.c_str(), isFloatFpr(D) ? "fbits" : "d2s", F(D).c_str(), ra.c_str())); return true;
    case 727: line(fmt("wr64(%s, dbits(%s));", eaX(w).c_str(), F(D).c_str())); return true;
    case 759: line(fmt("{ u32 ea = %s + %s; wr64(ea, dbits(%s)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), ra.c_str())); return true;
    case 983: line(fmt("wr32(%s, u32(dbits(%s)));", eaX(w).c_str(), F(D).c_str())); return true;
    case 597: line(fmt("lswi(c, %u, %s, %u);", D, A ? R(A).c_str() : "0u", B ? B : 32)); return true;
    case 725: line(fmt("stswi(c, %u, %s, %u);", D, A ? R(A).c_str() : "0u", B ? B : 32)); return true;
    case 533: line(fmt("lswi(c, %u, %s, c.xerBytes);", D, eaX(w).c_str())); return true;
    case 661: line(fmt("stswi(c, %u, %s, c.xerBytes);", D, eaX(w).c_str())); return true;
    // logical / shifts (dest rA)
    case 28: line(fmt("%s = %s & %s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 60: line(fmt("%s = %s & ~%s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 444:
        if (D == B) line(fmt("%s = %s;", ra.c_str(), rs.c_str()));
        else line(fmt("%s = %s | %s;", ra.c_str(), rs.c_str(), rb.c_str()));
        rec(ra);
        return true;
    case 412: line(fmt("%s = %s | ~%s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 316: line(fmt("%s = %s ^ %s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 476: line(fmt("%s = ~(%s & %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 124: line(fmt("%s = ~(%s | %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 284: line(fmt("%s = ~(%s ^ %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 24: line(fmt("%s = slw(%s, %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 536: line(fmt("%s = srw(%s, %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 792: line(fmt("%s = sraw(c, %s, %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 824: line(fmt("%s = srawi(c, %s, %u);", ra.c_str(), rs.c_str(), B)); rec(ra); return true;
    case 26: line(fmt("%s = cntlzw(%s);", ra.c_str(), rs.c_str())); rec(ra); return true;
    case 954: line(fmt("%s = u32(s32(s8(%s)));", ra.c_str(), rs.c_str())); rec(ra); return true;
    case 922: line(fmt("%s = u32(s32(s16(%s)));", ra.c_str(), rs.c_str())); rec(ra); return true;
    // system
    case 83: line(rd + " = c.msr;"); return true;
    case 146: line(fmt("mtmsr(c, %s, %s);", rs.c_str(), hex(pc).c_str())); return true;
    case 210: line(fmt("mtsr(c, %u, %s);", A & 15, rs.c_str())); return true;
    case 242: line(fmt("mtsr(c, %s >> 28, %s);", rb.c_str(), rs.c_str())); return true;
    case 595: line(fmt("%s = mfsr(c, %u);", rd.c_str(), A & 15)); return true;
    case 659: line(fmt("%s = mfsr(c, %s >> 28);", rd.c_str(), rb.c_str())); return true;
    case 339: {
        const u32 s = spr(w);
        if (s == 8) line(rd + " = c.lr;");
        else if (s == 9) line(rd + " = " + CTR() + ";");  // RL-NX (9.55)
        else if (s == 1) line(rd + " = c.getxer();");
        else if (s >= 912 && s <= 919) line(fmt("%s = c.gqr[%u];", rd.c_str(), s - 912));
        else line(fmt("%s = mfspr(c, %u);", rd.c_str(), s));
        return true;
    }
    case 467: {
        const u32 s = spr(w);
        if (s == 8) line("c.lr = " + rs + ";");
        else if (s == 9) line(CTR() + " = " + rs + ";");  // RL-NX (9.55)
        else if (s == 1) line(g_so0 ? "c.setxer(soCheck(" + rs + "));" : "c.setxer(" + rs + ");");  // RL-NX (Stage 11): see g_so0
        else if (s == 912 && g_gqr0Const) line("c.gqr[0] = gqr0Check(" + rs + ");");  // RL-NX (Stage 1): see g_gqr0Const
        else if (s >= 912 && s <= 919) line(fmt("c.gqr[%u] = %s;", s - 912, rs.c_str()));
        else line(fmt("mtspr(c, %u, %s);", s, rs.c_str()));
        return true;
    }
    case 371: line(fmt("%s = mftb(c, %u);", rd.c_str(), spr(w))); return true;
    case 54: case 86: case 278: case 246: case 566: case 598: case 854:
        return true;  // dcbst dcbf dcbt dcbtst tlbsync sync eieio
    case 306: line(fmt("tlbie(c, %s);", rb.c_str())); return true;
    case 470: line(fmt("dcbi(c, %s);", eaX(w).c_str())); return true;
    case 982: line(fmt("icbi(c, %s);", eaX(w).c_str())); return true;
    case 1014: line(fmt("dcbz(c, %s);", eaX(w).c_str())); return true;
    }
    return false;
}

bool Emitter::fp59(u32 w, std::string& o) const {
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { out(o, s); };  // RL-NX: spill/reload aware
    std::string e;
    switch (xo5(w)) {
    case 18: e = fmt("%s / %s", F(A).c_str(), F(B).c_str()); break;
    case 20: e = fmt("%s - %s", F(A).c_str(), F(B).c_str()); break;
    case 21: e = fmt("%s + %s", F(A).c_str(), F(B).c_str()); break;
    case 22: e = fmt("__builtin_sqrt(%s)", F(B).c_str()); break;
    case 24: line(fmt("%s = %s = fres(%s);", F(D).c_str(), P(D).c_str(), F(B).c_str())); goto done;
    case 25: e = fmt("%s * %s", F(A).c_str(), F(C).c_str()); break;
    case 28: e = fmt("fma_(%s, %s, -%s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    case 29: e = fmt("fma_(%s, %s, %s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    case 30: e = fmt("-fma_(%s, %s, -%s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    case 31: e = fmt("-fma_(%s, %s, %s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    default: return false;
    }
    {
        // RL-NX (Stage 2): every FPR the op names float-typed -> computed in float (see g_psFloat)
        const u32 x = xo5(w);
        const bool ua = x != 22, ub = x != 25, uc = x == 25 || x >= 28;
        if (g_psFloat && isFloatFpr(D) && (!ua || isFloatFpr(A)) && (!ub || isFloatFpr(B)) && (!uc || isFloatFpr(C))) {
            line(fmt("%s = %s = %s;", F(D).c_str(), P(D).c_str(), toFloatExpr(e).c_str()));
            goto done;
        }
    }
    line(fmt("%s = %s = rsingle(%s);", F(D).c_str(), P(D).c_str(), e.c_str()));
done:
    if (rc(w)) line(cr1Str());  // RL-NX (9.55)
    return true;
}

bool Emitter::fp63(u32 w, std::string& o) const {
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { out(o, s); };  // RL-NX: spill/reload aware
    auto fin = [&]() {
        if (rc(w)) line(cr1Str());  // RL-NX (9.55)
        return true;
    };
    switch (xo5(w)) {
    case 18: line(fmt("%s = %s / %s;", F(D).c_str(), F(A).c_str(), F(B).c_str())); return fin();
    case 20: line(fmt("%s = %s - %s;", F(D).c_str(), F(A).c_str(), F(B).c_str())); return fin();
    case 21: line(fmt("%s = %s + %s;", F(D).c_str(), F(A).c_str(), F(B).c_str())); return fin();
    case 22: line(fmt("%s = __builtin_sqrt(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 23: line(fmt("%s = fsel(%s, %s, %s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 25: line(fmt("%s = %s * %s;", F(D).c_str(), F(A).c_str(), F(C).c_str())); return fin();
    case 26: line(fmt("%s = frsqrte(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 28: line(fmt("%s = fma_(%s, %s, -%s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 29: line(fmt("%s = fma_(%s, %s, %s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 30: line(fmt("%s = -fma_(%s, %s, -%s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 31: line(fmt("%s = -fma_(%s, %s, %s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    }
    switch (xo10(w)) {
    case 0: case 32: line(fmt("%s = %s(c, %s, %s);", CR(crfD(w)).c_str(), cmpFn(A, B), F(A).c_str(), F(B).c_str())); return true;  // RL-NX (Stage 2)
    case 12: line(fmt("%s = %s = rsingle(%s);", F(D).c_str(), P(D).c_str(), F(B).c_str())); return fin();
    case 14: line(fmt("%s = ctiwBits(fctiw(c, %s, false));", F(D).c_str(), F(B).c_str())); return fin();
    case 15: line(fmt("%s = ctiwBits(fctiwz_(%s));", F(D).c_str(), F(B).c_str())); return fin();  // RL-NX (Stage 2): inline
    case 38: line(fmt("c.fpscr |= 0x%08Xu;", 0x80000000u >> D)); return fin();
    case 70: line(fmt("c.fpscr &= ~0x%08Xu;", 0x80000000u >> D)); return fin();
    case 134: {
        const u32 sh = 28 - 4 * crfD(w);
        line(fmt("c.fpscr = (c.fpscr & ~0x%08Xu) | 0x%08Xu;", 0xFu << sh, ((w >> 12) & 0xF) << sh));
        return fin();
    }
    case 711: {
        const u32 fm = (w >> 17) & 0xFF;
        u32 m = 0;
        for (int i = 0; i < 8; i++)
            if (fm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
        line(fmt("c.fpscr = (c.fpscr & ~0x%08Xu) | (u32(dbits(%s)) & 0x%08Xu);", m, F(B).c_str(), m));
        return fin();
    }
    case 583: line(fmt("%s = bdbl(0xFFF8000000000000ull | c.fpscr);", F(D).c_str())); return fin();
    case 64: line(fmt("%s = u8((c.fpscr >> %u) & 15);", CR(crfD(w)).c_str(), 28 - 4 * crfS(w))); return true;
    case 40: line(fmt("%s = fneg_(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 72: line(fmt("%s = %s;", F(D).c_str(), F(B).c_str())); return fin();
    case 136: line(fmt("%s = fnabs_(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 264: line(fmt("%s = fabs_(%s);", F(D).c_str(), F(B).c_str())); return fin();
    }
    return false;
}

bool Emitter::ps(const Function&, u32 pc, u32 w, std::string& o) const {
    (void)pc;
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { out(o, s); };  // RL-NX: spill/reload aware
    auto fin = [&]() {
        if (rc(w)) line(cr1Str());  // RL-NX (9.55)
        return true;
    };
    // two lanes computed from temporaries (operands may alias the destination)
    auto lanes = [&](const std::string& e0, const std::string& e1, bool round) {
        // RL-NX (Stage 2): all float-typed -> float temporaries, only the operands the lanes read (see g_psFloat)
        {
            const bool ua = usesTok(e0, e1, "a0") || usesTok(e0, e1, "a1"), ub = usesTok(e0, e1, "b0") || usesTok(e0, e1, "b1"),
                       uc = usesTok(e0, e1, "c0") || usesTok(e0, e1, "c1");
            if (g_psFloat && isFloatFpr(D) && (!ua || isFloatFpr(A)) && (!ub || isFloatFpr(B)) && (!uc || isFloatFpr(C))) {
                std::string tmp;
                auto decl = [&](bool u, const char* n, u32 r) {
                    if (!u) return;
                    tmp += fmt("%s%s0 = %s, %s1 = %s", tmp.empty() ? "float " : ", ", n, F(r).c_str(), n, P(r).c_str());
                };
                decl(ua, "a", A), decl(ub, "b", B), decl(uc, "c", C);
                line(fmt("{ %s; %s = %s; %s = %s; }", tmp.c_str(), F(D).c_str(), toFloatExpr(e0).c_str(), P(D).c_str(),
                         toFloatExpr(e1).c_str()));
                return fin();
            }
        }
        const char* rs = round ? "rsingle" : "";
        line(fmt("{ double a0 = %s, a1 = %s, b0 = %s, b1 = %s, c0 = %s, c1 = %s; (void)a0; (void)a1; (void)b0; (void)b1; (void)c0; (void)c1; "
                 "%s = %s(%s); %s = %s(%s); }",
                 F(A).c_str(), P(A).c_str(), F(B).c_str(), P(B).c_str(), F(C).c_str(), P(C).c_str(), F(D).c_str(), rs,
                 e0.c_str(), P(D).c_str(), rs, e1.c_str()));
        return fin();
    };
    switch (xo10(w)) {
    case 0: case 32: line(fmt("%s = %s(c, %s, %s);", CR(crfD(w)).c_str(), cmpFn(A, B), F(A).c_str(), F(B).c_str())); return true;  // RL-NX (Stage 2)
    case 64: case 96: line(fmt("%s = %s(c, %s, %s);", CR(crfD(w)).c_str(), cmpFn(A, B), P(A).c_str(), P(B).c_str())); return true;
    case 40: return lanes("fneg_(b0)", "fneg_(b1)", false);
    case 72: return lanes("b0", "b1", false);
    case 136: return lanes("fnabs_(b0)", "fnabs_(b1)", false);
    case 264: return lanes("fabs_(b0)", "fabs_(b1)", false);
    case 528: return lanes("a0", "b0", false);
    case 560: return lanes("a0", "b1", false);
    case 592: return lanes("a1", "b0", false);
    case 624: return lanes("a1", "b1", false);
    case 1014: line(fmt("dcbzl(c, %s);", eaX(w).c_str())); return true;
    }
    switch (xo5(w)) {
    case 10: return lanes("rsingle(a0 + b1)", "c1", false);
    case 11: return lanes("c0", "rsingle(a0 + b1)", false);
    case 12: return lanes("a0 * c0", "a1 * c0", true);
    case 13: return lanes("a0 * c1", "a1 * c1", true);
    case 14: return lanes("fma_(a0, c0, b0)", "fma_(a1, c0, b1)", true);
    case 15: return lanes("fma_(a0, c1, b0)", "fma_(a1, c1, b1)", true);
    case 18: return lanes("a0 / b0", "a1 / b1", true);
    case 20: return lanes("a0 - b0", "a1 - b1", true);
    case 21: return lanes("a0 + b0", "a1 + b1", true);
    case 23: return lanes("fsel(a0, c0, b0)", "fsel(a1, c1, b1)", false);
    case 24: return lanes("fres(b0)", "fres(b1)", false);
    case 25: return lanes("a0 * c0", "a1 * c1", true);
    case 26: return lanes("frsqrte(b0)", "frsqrte(b1)", true);
    case 28: return lanes("fma_(a0, c0, -b0)", "fma_(a1, c1, -b1)", true);
    case 29: return lanes("fma_(a0, c0, b0)", "fma_(a1, c1, b1)", true);
    case 30: return lanes("-fma_(a0, c0, -b0)", "-fma_(a1, c1, -b1)", true);
    case 31: return lanes("-fma_(a0, c0, b0)", "-fma_(a1, c1, b1)", true);
    }
    const u32 x6 = (w >> 1) & 0x3F;
    if (x6 == 6 || x6 == 7 || x6 == 38 || x6 == 39) {
        const bool st = x6 & 1, upd = x6 >= 38;
        const u32 W = (w >> 10) & 1, I = (w >> 7) & 7;
        std::string ea = upd ? fmt("%s + %s", R(A).c_str(), R(B).c_str()) : eaX(w);
        // RL-NX (9.55): psqLd/psqSt take the FPR locals by reference and the RAM base M
        if (st)
            line(fmt("{ u32 ea = %s; %s(c, M, ea, %s, %s, %s, %s);%s }", ea.c_str(), isFloatFpr(D) ? "psqStF" : "psqSt", W ? "true" : "false", gqrArg(I).c_str(),  // RL-NX (Stage 1): gqrArg
                     F(D).c_str(), P(D).c_str(), upd ? (" " + R(A) + " = ea;").c_str() : ""));
        else
            line(fmt("{ u32 ea = %s; const auto q_ = %s(c, M, ea, %s, %s); %s = q_.a; %s = q_.b;%s }",  // RL-NX (Stage 1): gqrArg
                     ea.c_str(), isFloatFpr(D) ? "psqLdF" : "psqLd", W ? "true" : "false", gqrArg(I).c_str(), F(D).c_str(), P(D).c_str(),
                     upd ? (" " + R(A) + " = ea;").c_str() : ""));
        return true;
    }
    return false;
}

// RL-NX (Stage 11): slot liveness and dirty tracking (R2RECOMP_LIVE=0 turns it off). Locals v2 loads every slot a
// function mentions at entry, stores every slot it writes at every spill and return, and reloads at a call every
// slot the callee may write; the summaries made each callee's read set "every slot it mentions" (the 17:09 audit:
// ABI boundary 19 % and host spills 13 % of game-code time). This pass works on the body's statements (one line of
// generated C each, with the spill/reload markers still in it):
//   * dirty (forward, may): a slot is dirty where some path since entry assigned its local and no full call or
//     reload has made Ctx the truth since. A spill or return stores only dirty slots: a clean local equals Ctx, so
//     its store rewrote the same value;
//   * live (backward): a local is live where some path reads it - a text read, or a store at a spill - before it
//     is assigned. Entry loads only the live-in slots; a reload after a call reloads only the slots live after it;
//   * need (backward): a slot whose Ctx copy must be exact - a direct Ctx read in the text, or a slot the callee
//     of a call reads from Ctx, not stored (dirty) at that call. A function's summary read set is what it loads at
//     entry plus what it needs at entry (less the slots its prologue saves, as before), found as a least fixpoint
//     over the call graph: a caller stores before a call only what the callee really reads.
// A text assignment kills only when the statement has no if/switch (a conditional write does not), and a reload
// kills only when it wraps the whole statement. Everything is per function and conservative; a body the parser
// does not understand (a goto to an unknown label) keeps the old behaviour (all loads, all stores).
namespace {
// RL-NX (Stage 11): small-data accesses unchecked (R2RECOMP_SDAFAST=0 turns it off). r13 and r2 hold the EABI small-data
// bases (_SDA_BASE_, _SDA2_BASE_, both in cached MEM1) from __start on, and every access through them with a 16-bit
// offset stays in MEM1, so in a function that never writes r13 (or r2) its accesses take the stack forms (ctx.h
// rd32S...: no arena test, no slow-path call), as r1's do; in VM-listed functions they also lose the VM TLB test,
// which they never needed. A function that writes the register (the OS's low-level handlers use r2 as a scratch
// register) keeps the ordinary forms for it. 17:09 audit: 10.6 % of guest memory samples were r13/r2 accesses.
bool g_sdaFast = true;
unsigned long long g_sdaSites = 0;  // statistics: accesses rewritten
// rewrite "NAME(M, (rN + " / "NAME(M, rN," / "NAME(M, rN)" for the plain, V and P accessor names to the S forms
unsigned sdaRewrite(std::string& body, u32 base) {
    static const char* kNames[] = {"rd8", "rd16", "rd32", "rd64", "wr8", "wr16", "wr32", "wr64"};
    unsigned n = 0;
    const std::string r = fmt("r%u", base);
    const std::string args[3] = {"(M, (" + r + " + ", "(M, " + r + ",", "(M, " + r + ")"};
    for (const char* nm : kNames)
        for (const char* suf : {"", "V", "P", "F"}) {  // RL-NX (Stage 11): + F (FASTMEM)
            const std::string from = std::string(nm) + suf;
            for (const std::string& a : args) {
                const std::string pat = from + a;
                for (size_t k = body.find(pat); k != std::string::npos; k = body.find(pat, k + 1)) {
                    if (k > 0 && (std::isalnum((unsigned char)body[k - 1]) || body[k - 1] == '_')) continue;
                    body.replace(k, from.size(), std::string(nm) + "S");
                    n++;
                }
            }
        }
    return n;
}
bool g_live = true;
unsigned long long g_lvEntrySkip = 0, g_lvStoreSkip = 0, g_lvReloadSkip = 0;  // statistics
SlotSet allSlots() {
    SlotSet s;
    s.set();
    return s;
}
Slot idxSlot(int i) {
    if (i < 32) return {'r', u32(i)};
    if (i < 64) return {'f', u32(i - 32)};
    if (i < 96) return {'p', u32(i - 64)};
    if (i < 104) return {'c', u32(i - 96)};
    return {'t', 0};
}
SlotSet toSet(const std::set<Slot>& s) {
    SlotSet m;
    for (const Slot& x : s) m.set(size_t(slotIndex(x)));
    return m;
}
struct LStmt {
    SlotSet use, defMust, defMay, ctxRd;  // text reads, unconditional writes, all writes, direct Ctx reads
    std::vector<int> succ;
    char spill = 0;        // 0, 'r' return, 'c' call site (summarised if its callee is), 'f' full
    char reload = 0;       // 0, 'c' (with a 'c' spill), 'f'
    bool reloadUncond = false;  // the markers wrap the whole statement
    u32 site = 0;          // 'c': the one direct callee
    SlotSet rsite;         // fixed part of what the call reads from Ctx ('f' sites)
    std::vector<u32> rsCallees;  // callees whose read sets a 'f' site also reads
};
struct FnIR {
    std::vector<LStmt> st;
    int entry = 0;
    bool bad = false;
    SlotSet touched, written, retSet;
};
std::map<u32, FnIR> g_ir;  // first pass, by entry
SlotSet g_abiArgs;          // what an indirect call reads (ABI2: r1 r2 r13 r3-r10 f1-f8 both halves, cr1)

// reads, writes and direct Ctx reads of one statement
void scanUD(const std::string& b, SlotSet& use, SlotSet& defMay, SlotSet& ctxRd) {
    const size_t n = b.size();
    for (size_t i = 0; i < n;) {
        const unsigned char ch = (unsigned char)b[i];
        if (std::isdigit(ch)) {
            while (i < n && (std::isalnum((unsigned char)b[i]) || b[i] == '_')) i++;
            continue;
        }
        if (!(std::isalpha(ch) || ch == '_')) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && (std::isalnum((unsigned char)b[j]) || b[j] == '_')) j++;
        const std::string id = b.substr(i, j - i);
        size_t k = i;
        while (k > 0 && b[k - 1] == ' ') k--;
        const bool member = (k > 0 && b[k - 1] == '.') || (k > 1 && b[k - 1] == '>' && b[k - 2] == '-');
        auto assignAt = [&](size_t q, bool& incdec) {
            while (q < n && b[q] == ' ') q++;
            incdec = q + 1 < n && ((b[q] == '-' && b[q + 1] == '-') || (b[q] == '+' && b[q + 1] == '+'));
            return q < n && b[q] == '=' && !(q + 1 < n && b[q + 1] == '=');
        };
        // "(void)x;" mentions a local without reading it
        const bool voidCast = i >= 6 && b.compare(i - 6, 6, "(void)") == 0;
        Slot s;
        if (!member && !voidCast && parseSlot(id, s)) {
            bool incdec = false;
            const bool asg = assignAt(j, incdec);
            const size_t x = size_t(slotIndex(s));
            if (asg) defMay.set(x);
            else if (incdec) defMay.set(x), use.set(x);
            else use.set(x);
        } else if (id == "c" && j < n && b[j] == '.' && !(i > 0 && (std::isalnum((unsigned char)b[i - 1]) || b[i - 1] == '_'))) {
            // a direct Ctx slot access: c.r[N] c.f[N] c.ps1[N] c.cr[N] c.ctr
            size_t q = j + 1, e = q;
            while (e < n && (std::isalnum((unsigned char)b[e]) || b[e] == '_')) e++;
            const std::string fld = b.substr(q, e - q);
            int base = -1;
            if (fld == "r") base = 0;
            else if (fld == "f") base = 32;
            else if (fld == "ps1") base = 64;
            else if (fld == "cr") base = 96;
            int idx = -1;
            size_t after = e;
            if (fld == "ctr") idx = 104;
            else if (base >= 0 && e < n && b[e] == '[') {
                size_t d = e + 1;
                u32 v = 0;
                bool num = false;
                while (d < n && std::isdigit((unsigned char)b[d])) v = v * 10 + u32(b[d] - '0'), d++, num = true;
                if (num && d < n && b[d] == ']') idx = base + int(v), after = d + 1;
            }
            if (idx >= 0 && idx <= 104) {
                bool incdec = false;
                if (!assignAt(after, incdec) || incdec) ctxRd.set(size_t(idx));
            }
            i = after;
            continue;
        }
        i = j;
    }
}
// does control leave the statement other than by falling through to the next one, on every path?
bool leaves(const std::string& t) {
    size_t a = t.find_first_not_of(std::string(" \t") + kSpillTok + kReloadTok);
    if (a == std::string::npos) return false;
    if (t.compare(a, 5, "goto ") == 0 || t.compare(a, 6, "return") == 0 || t.compare(a, 8, "switch (") == 0) return true;
    int depth = 0;
    for (size_t i = a; i < t.size(); i++) {
        if (t[i] == '{') depth++;
        else if (t[i] == '}') depth--;
        else if (depth == 0 && (t.compare(i, 7, "goto L_") == 0 || t.compare(i, 7, "return;") == 0) &&
                 (i == 0 || !(std::isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_')))
            return true;
    }
    return false;
}
struct LineRef {
    size_t at, len;
    int stmt;  // -1: not a statement
};
// split a finished body (markers in place) into statements; lines gets each line and its statement
void parseBody(const std::string& body, u32 entryPc, bool entryGoto, FnIR& ir, std::vector<LineRef>* lines) {
    std::map<u32, int> label;  // label pc -> first statement after it
    std::vector<u32> pending;
    std::vector<std::string> text;
    for (size_t p = 0; p < body.size();) {
        size_t e = body.find('\n', p);
        if (e == std::string::npos) e = body.size();
        const std::string ln = body.substr(p, e - p);
        int si = -1;
        if (ln.size() == 11 && ln.compare(0, 2, "L_") == 0 && ln[10] == ':' && isHex8(ln, 2)) {
            pending.push_back(hex8(ln, 2));
        } else if (!(ln.empty() || ln[0] == '#' || ln.compare(0, 6, "    //") == 0 || ln.compare(0, 14, "    icountAdd(") == 0)) {
            si = int(text.size());
            text.push_back(ln);
            for (u32 l : pending) label[l] = si;
            pending.clear();
        }
        if (lines) lines->push_back({p, e - p, si});
        p = e + 1;
    }
    ir.st.assign(text.size(), LStmt{});
    if (!pending.empty() || text.empty()) {  // a label with no statement after it
        ir.bad = true;
        return;
    }
    if (entryGoto) {
        auto it = label.find(entryPc);
        if (it == label.end()) {
            ir.bad = true;
            return;
        }
        ir.entry = it->second;
    }
    SlotSet abi = g_abiOn ? abiMask() : allSlots();
    (void)abi;
    for (size_t k = 0; k < text.size(); k++) {
        const std::string& t = text[k];
        LStmt& s = ir.st[k];
        scanUD(t, s.use, s.defMay, s.ctxRd);
        const bool cond = t.find("if (") != std::string::npos || t.find("switch") != std::string::npos ||
                          t.find("while") != std::string::npos || t.find("for (") != std::string::npos;
        if (!cond) s.defMust = s.defMay;
        for (size_t g = t.find("goto L_"); g != std::string::npos; g = t.find("goto L_", g + 7)) {
            auto it = isHex8(t, g + 7) ? label.find(hex8(t, g + 7)) : label.end();
            if (it == label.end()) {
                ir.bad = true;
                return;
            }
            s.succ.push_back(it->second);
        }
        if (!leaves(t) && k + 1 < text.size()) s.succ.push_back(int(k + 1));
        const size_t sp = t.find(kSpillTok), rl = t.find(kReloadTok);
        if (sp != std::string::npos) {
            size_t q = sp + 1;
            while (q < t.size() && (t[q] == ' ' || t[q] == '\t')) q++;
            if (q >= t.size() || t.compare(q, 7, "return;") == 0) s.spill = 'r';
            else {
                s.spill = 'f';
                if (rl != std::string::npos && rl > sp) {
                    std::string mid = t.substr(sp + 1, rl - sp - 1);
                    size_t a = mid.find_first_not_of(" \t"), z = mid.find_last_not_of(" \t");
                    if (a != std::string::npos) mid = mid.substr(a, z - a + 1);
                    if (mid.size() == 14 && mid.compare(0, 2, "f_") == 0 && isHex8(mid, 2) && mid.compare(10, 4, "(c);") == 0)
                        s.spill = 'c', s.site = hex8(mid, 2);
                }
            }
        }
        if (rl != std::string::npos) {
            s.reload = s.spill == 'c' ? 'c' : 'f';
            std::string rest = t.substr(rl + 1);
            size_t a = rest.find_first_not_of(" \t");
            rest = a == std::string::npos ? std::string() : rest.substr(a);
            const size_t lead = t.find_first_not_of(" \t");
            s.reloadUncond = sp == lead && (rest.empty() || rest == "c.r[1] = r1;");
        }
        // what a full site's call reads from Ctx
        if (s.spill == 'f') {
            if (t.find("dispatch(c") != std::string::npos) s.rsite |= g_abi2 ? g_abiArgs : allSlots();
            bool other = t.find("RT_SAVECTX") != std::string::npos || t.find("lswi(c") != std::string::npos ||
                         t.find("stswi(c") != std::string::npos || t.find("rfi(c") != std::string::npos ||
                         t.find("sc(c") != std::string::npos || t.find("trap(c") != std::string::npos ||
                         t.find("hle_") != std::string::npos;
            if (t.find("safepoint(c") != std::string::npos || t.find("idleWait(c") != std::string::npos ||
                t.find("mtmsr(c") != std::string::npos)
                other = other || g_summStrict;
            for (size_t g = t.find("f_"); g != std::string::npos; g = t.find("f_", g + 2))
                if ((g == 0 || !(std::isalnum((unsigned char)t[g - 1]) || t[g - 1] == '_')) && isHex8(t, g + 2) &&
                    t.compare(g + 10, 2, "(c") == 0)
                    s.rsCallees.push_back(hex8(t, g + 2));
            if (other) s.rsite = allSlots();
            // any other call with c that is not a known pure helper: be safe
            if (!other && s.rsCallees.empty() && t.find("dispatch(c") == std::string::npos &&
                t.find("safepoint(c") == std::string::npos && t.find("idleWait(c") == std::string::npos &&
                t.find("mtmsr(c") == std::string::npos)
                s.rsite = allSlots();
        }
    }
}
// the read set of a callee for its callers (all slots when unknown or opaque)
SlotSet calleeRead(u32 t) {
    auto it = g_summ.find(t);
    return it == g_summ.end() || it->second.opaque ? allSlots() : it->second.read;
}
const Summary* summarised(u32 t) {
    auto it = g_summ.find(t);
    return it == g_summ.end() || it->second.opaque ? nullptr : &it->second;
}
struct LvOut {
    std::vector<SlotSet> dirtyIn, liveOut;
    SlotSet liveEntry, needEntry;
};
// the planned stores of a statement's spill (before the dirty filter)
SlotSet plannedStores(const FnIR& ir, const LStmt& s) {
    if (s.spill == 'r') return ir.retSet;
    if (s.spill == 'c')
        if (const Summary* cs = summarised(s.site)) return ir.written & cs->read;
    return ir.written;
}
SlotSet plannedReload(const FnIR& ir, const LStmt& s) {
    if (s.reload == 'c')
        if (const Summary* cs = summarised(s.site)) return ir.touched & cs->write;
    return ir.touched;
}
void analyze(const FnIR& ir, LvOut& o) {
    const size_t n = ir.st.size();
    o.dirtyIn.assign(n, SlotSet());
    o.liveOut.assign(n, SlotSet());
    if (ir.bad) {
        for (auto& d : o.dirtyIn) d.set();
        for (auto& d : o.liveOut) d.set();
        o.liveEntry.set();
        o.needEntry.set();
        return;
    }
    // dirty, forward
    std::vector<SlotSet> dout(n);
    std::vector<char> seen(n, 0);
    std::vector<int> work{ir.entry};
    seen[size_t(ir.entry)] = 1;
    while (!work.empty()) {
        const int k = work.back();
        work.pop_back();
        const LStmt& s = ir.st[size_t(k)];
        SlotSet d = o.dirtyIn[size_t(k)];
        if (s.reload && s.reloadUncond) {
            const Summary* cs = s.reload == 'c' ? summarised(s.site) : nullptr;
            if (cs) d &= ~cs->write;
            else d.reset();
        }
        d |= s.defMay;
        dout[size_t(k)] = d;
        for (int t : s.succ) {
            const SlotSet m = o.dirtyIn[size_t(t)] | d;
            if (!seen[size_t(t)] || m != o.dirtyIn[size_t(t)]) {
                seen[size_t(t)] = 1;
                o.dirtyIn[size_t(t)] = m;
                work.push_back(t);
            }
        }
    }
    // live and need, backward (round robin to a fixpoint)
    std::vector<SlotSet> lin(n), nin(n);
    std::vector<SlotSet> stores(n), rsite(n), rkill(n);
    for (size_t k = 0; k < n; k++) {
        const LStmt& s = ir.st[k];
        if (s.spill) stores[k] = plannedStores(ir, s) & o.dirtyIn[k];
        if (s.spill == 'c') rsite[k] = calleeRead(s.site);
        else if (s.spill == 'f') {
            rsite[k] = s.rsite;
            for (u32 t : s.rsCallees) rsite[k] |= calleeRead(t);
        }
        if (s.reload && s.reloadUncond) rkill[k] = plannedReload(ir, s);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t kk = n; kk-- > 0;) {
            const LStmt& s = ir.st[kk];
            SlotSet L, N;
            for (int t : s.succ) L |= lin[size_t(t)], N |= nin[size_t(t)];
            o.liveOut[kk] = L;
            L &= ~rkill[kk];
            L = (L & ~s.defMust) | s.use | stores[kk];
            N = ((N & ~s.defMust) | s.ctxRd | rsite[kk]) & ~stores[kk];
            if (L != lin[kk] || N != nin[kk]) lin[kk] = L, nin[kk] = N, changed = true;
        }
    }
    o.liveEntry = lin[size_t(ir.entry)] & ir.touched;
    o.needEntry = nin[size_t(ir.entry)];
}
// least fixpoint of the summary read sets over the call graph (after the opaque/write closure)
void solveReads() {
    std::map<u32, std::vector<u32>> callers;
    for (auto& [e, ir] : g_ir) {
        std::set<u32> cs;
        for (const LStmt& s : ir.st) {
            if (s.site) cs.insert(s.site);
            for (u32 t : s.rsCallees) cs.insert(t);
        }
        for (u32 t : cs) callers[t].push_back(e);
    }
    std::vector<u32> work;
    for (auto& [e, sm] : g_summ)
        if (!sm.opaque && g_ir.count(e)) sm.read.reset(), work.push_back(e);
    std::set<u32> queued(work.begin(), work.end());
    size_t steps = 0;
    LvOut o;
    while (!work.empty()) {
        const u32 e = work.back();
        work.pop_back();
        queued.erase(e);
        Summary& sm = g_summ[e];
        analyze(g_ir[e], o);
        const SlotSet r = (o.liveEntry | o.needEntry) & ~sm.pres;
        steps++;
        if ((r & ~sm.read).none()) continue;
        sm.read |= r;
        for (u32 c : callers[e])
            if (!g_summ[c].opaque && queued.insert(c).second) work.push_back(c);
    }
    size_t bits = 0, fns = 0;
    for (auto& [e, sm] : g_summ)
        if (!sm.opaque) bits += sm.read.count(), fns++;
    fprintf(stderr, "r2recomp: liveness: read sets solved in %zu steps, %zu summarised functions read %.1f slots on average\n",
            steps, fns, fns ? double(bits) / double(fns) : 0.0);
}
}  // namespace

void Emitter::function(const Function& f, std::string& o, const std::string& name) const {
    typeFprs(img_, f);  // RL-NX (9.58): float FPRs, prologue saves, epilogue restores
    // RL-NX (Stage 4): does this function load the gather pipe's base (addis rD, 0, 0xCC01)?
    g_fnPipe = false;
    if (g_pipeFast)
        for (const u32 pc : f.insns) {
            const u32 w = img_.word(pc);
            if ((w >> 26) == 15 && ((w >> 16) & 31) == 0 && (w & 0xFFFF) == 0xCC01) {
                g_fnPipe = true;
                break;
            }
        }
    // RL-NX (Stage 11): FASTMEM - does this function build an IO, uncached or EFB address (addis rD, 0, imm)?
    g_fnIo = false;
    if (g_fastmem)
        for (const u32 pc : f.insns) {
            const u32 w = img_.word(pc), imm = w & 0xFFFF;
            if ((w >> 26) == 15 && ((w >> 16) & 31) == 0 &&
                ((imm >= 0xC000 && imm <= 0xC1FF) || (imm >= 0xC800 && imm <= 0xC8FF) || (imm >= 0xCC00 && imm <= 0xCDFF))) {
                g_fnIo = true;
                break;
            }
        }
    std::string body;
    bool usesJb = false;
    const bool needEntryGoto = !f.insns.empty() && f.insns.front() != f.entry;
    for (size_t i = 0; i < f.insns.size(); i++) {
        const u32 pc = f.insns[i];
        const u32 w = img_.word(pc);
        const bool blockStart = f.labels.count(pc) || (needEntryGoto && pc == f.entry);
        if (blockStart) body += fmt("L_%08X:\n", pc);
        // RL-NX: guest instruction metering (ctx.h icountAdd, R2_ICOUNT=1). One add per basic block
        // at its entry, so the executed guest instruction count comes out exactly; the block's size
        // is the run of instructions up to the next label. Compiled out of every ordinary build
        // (-DRL_NX_ICOUNT only), so the shipped code carries neither the counter nor the branch.
        if (blockStart) {
            size_t j = i + 1;
            while (j < f.insns.size() && !f.labels.count(f.insns[j]) && !(needEntryGoto && f.insns[j] == f.entry)) j++;
            body += fmt("#if defined(RL_NX_ICOUNT)\n    icountAdd(%zu);\n#endif\n", j - i);
        }
        body += fmt("    // %08X: %08X\n", pc, w);
        g_vmSite = g_vmInline && (cfg_.vmSites.count(pc) || (g_vmFns && cfg_.vmFns.count(f.entry)));  // RL-NX (Stage 9); VMFNS: audit 2026-11-27
        // RL-NX (Stage 11): only the game's paged modules (0x7E000000-0x7FFFFFFF) take F forms; the DOL holds the SDK, whose
        // GX/OS code reaches the CP/PE/PI registers through pointers kept in globals (__cpReg ...), which no lis shows
        g_fmChecked = g_fnIo || cfg_.fmSites.count(pc) || cfg_.fmFns.count(f.entry) || f.entry - 0x7E000000u >= 0x02000000u;
        const size_t insnAt = body.size();
        if (!insn(f, pc, w, body, usesJb)) body += fmt("    illegal(c, %s, %s);\n", hex(pc).c_str(), hex(w).c_str());
        if (g_vmSite) {  // RL-NX (Stage 9): the paired-single forms are not accessor names withBase sees
            g_vmSiteCount++;
            g_vmSite = false;
            for (const char* q : {"psqLdF(", "psqStF(", "psqLd(", "psqSt("}) {
                const std::string from = q, to = from.substr(0, from.size() - 1) + "V(";
                for (size_t k = body.find(from, insnAt); k != std::string::npos; k = body.find(from, k + to.size()))
                    body.replace(k, from.size(), to);
            }
        }
        // RL-NX (9.55): a tail call into the next entry is a call and a return: spill first
        if (f.tailEntries.count(pc))
            body += fmt("    %s%s return;\n", g_locals ? std::string(1, kSpillTok).c_str() : "",
                        callTo(pc + 4, pc, false).c_str());
    }
    // RL-NX (Stage 11): small-data accesses take the stack forms (see g_sdaFast)
    if (g_sdaFast && g_locals) {
        std::set<Slot> w;
        scanSlots(body, nullptr, &w);
        for (u32 base : {2u, 13u})
            if (!w.count(Slot{'r', base})) {
                const unsigned k = sdaRewrite(body, base);
                if (!g_summPass) g_sdaSites += k;
            }
    }
    o += "void " + name + "(Ctx& c) {\n";
    o += fmt("    if (g_trace) [[unlikely]] traceEnter(c, %s);\n", hex(f.entry).c_str());
    // RL-NX (PERF_PLAN 0b-ii): the oracle probe point - only in a RL_NX_ORACLE build, so shipped code carries
    // no call at all (oracle.cpp runs the interpreter and this body from identical bytes and compares them).
    o += fmt("#if defined(RL_NX_ORACLE)\n    oracleProbe(%s, c);\n#endif\n", hex(f.entry).c_str());
    if (usesJb) o += "    JmpBuf jb;\n";
    // RL-NX (9.55): the guest RAM base, read once (after the oracle probe, which may swap it while it runs)
    o += "    u8* const M = memBase(); (void)M;\n";
    if (g_locals) {
        // RL-NX (9.55, locals v2): every slot the body mentions becomes a local loaded at entry; the spill
        // marker stores every slot the body writes, the reload marker reloads every slot it mentions. A
        // function that falls off its end spills on the way out too.
        body += std::string("    ") + kSpillTok + "\n";
        std::set<Slot> touched, written;
        scanSlots(body, &touched, &written);
        // RL-NX (9.57): first pass of Emitter::write - record this function's own sets and calls, nothing more
        if (g_summPass) {
            Summary& sm = g_summ[g_summEntry];
            if (g_abi2) sm.pres = savedSlots(img_, f), g_presSlots += sm.pres.count();  // RL-NX (Stage 8)
            for (const Slot& sl : touched)
                if (!sm.pres.test(size_t(slotIndex(sl)))) sm.read.set(size_t(slotIndex(sl)));
            for (const Slot& sl : written)
                if (!sm.pres.test(size_t(slotIndex(sl)))) sm.write.set(size_t(slotIndex(sl)));
            scanCalls(body, sm);
            if (cfg_.saveCtx.count(g_summEntry)) sm.opaque = true;  // returns twice
            if (g_live) {  // RL-NX (Stage 11): the statements, for the read-set fixpoint (solveReads)
                FnIR& ir = g_ir[g_summEntry];
                ir = FnIR{};
                parseBody(body, f.entry, needEntryGoto, ir, nullptr);
                ir.touched = toSet(touched), ir.written = toSet(written);
                const bool abiRet1 = g_abiOn && !cfg_.saveCtx.count(f.entry) && body.find("rfi(c") == std::string::npos &&
                                     body.find("RT_SAVECTX") == std::string::npos;
                ir.retSet = abiRet1 ? ir.written & abiMask() : ir.written;
            }
            return;
        }
        // RL-NX (Stage 11): liveness of this body against the final summaries (see g_live)
        FnIR lir;
        LvOut lv;
        std::vector<LineRef> lines;
        if (g_live) {
            parseBody(body, f.entry, needEntryGoto, lir, &lines);
            lir.touched = toSet(touched), lir.written = toSet(written);
            const bool abiRet1 = g_abiOn && !cfg_.saveCtx.count(f.entry) && body.find("rfi(c") == std::string::npos &&
                                 body.find("RT_SAVECTX") == std::string::npos;
            lir.retSet = abiRet1 ? lir.written & abiMask() : lir.written;
            analyze(lir, lv);
        }
        std::string decl[6], spill, reload, retSpill;  // RL-NX (Stage 1): retSpill - the ABI-live part, for returns
        const char kinds[5] = {'r', 'f', 'p', 'c', 't'};
        bool anyFloat = false;
        for (const Slot& s : touched) {
            int k = int(std::find(kinds, kinds + 5, s.kind) - kinds);
            const bool fp = s.kind == 'f' || s.kind == 'p';
            if (fp && isFloatFpr(s.n)) k = 5, anyFloat = true;  // RL-NX (9.58): float FPRs get their own declaration
            // RL-NX (Stage 11): a slot that is not live at entry is declared without its load
            if (g_live && !lv.liveEntry.test(size_t(slotIndex(s)))) {
                decl[k] += fmt("%s%s", decl[k].empty() ? (std::string(s.type()) + " ").c_str() : ", ", s.local().c_str());
                g_lvEntrySkip++;
            } else
                decl[k] += fmt("%s%s = %s", decl[k].empty() ? (std::string(s.type()) + " ").c_str() : ", ", s.local().c_str(),
                               fp ? slotLoadExpr(s.kind, s.n).c_str() : s.field().c_str());
            reload += fp ? fmt(" %s = %s;", s.local().c_str(), slotLoadExpr(s.kind, s.n).c_str())
                         : fmt(" %s = %s;", s.local().c_str(), s.field().c_str());
        }
        if (anyFloat) g_floatFns++;
        for (u32 n = 0; n < 32; n++) g_floatRegs += (touched.count(Slot{'f', n}) && isFloatFpr(n)) ? 1 : 0;
        // RL-NX (Stage 1): a function that leaves through rfi or a saved context keeps the full store at its returns
        const bool abiRet = g_abiOn && !cfg_.saveCtx.count(f.entry) && body.find("rfi(c") == std::string::npos &&
                            body.find("RT_SAVECTX") == std::string::npos;
        for (const Slot& s : written) {
            const std::string st =
                (s.kind == 'f' || s.kind == 'p') ? slotStoreStmt(s.kind, s.n) : fmt(" %s = %s;", s.field().c_str(), s.local().c_str());
            spill += st;
            if (!abiRet || abiLive(s)) retSpill += st;
            else g_retSaved++;
        }
        for (auto& d : decl)
            if (!d.empty()) o += "    " + d + ";\n";
        for (u32 n = 0; n < 32; n++)  // RL-NX (9.58): epilogue lfd restores (slotStoreStmt)
            if ((g_fRestore >> n) & 1) o += fmt("    bool rs%u = false;\n", n);
        std::string out;
        out.reserve(body.size() + body.size() / 4);
        // RL-NX (Stage 11): with liveness each marker expands to the stores of its planned slots that are dirty there
        // and the reloads of its planned slots that are live after it (plannedStores/plannedReload, see g_live).
        if (g_live) {
            auto storeOf = [](const Slot& sl) {
                return (sl.kind == 'f' || sl.kind == 'p') ? slotStoreStmt(sl.kind, sl.n)
                                                          : fmt(" %s = %s;", sl.field().c_str(), sl.local().c_str());
            };
            auto loadOf = [](const Slot& sl) {
                return (sl.kind == 'f' || sl.kind == 'p') ? fmt(" %s = %s;", sl.local().c_str(), slotLoadExpr(sl.kind, sl.n).c_str())
                                                          : fmt(" %s = %s;", sl.local().c_str(), sl.field().c_str());
            };
            for (const LineRef& lr : lines) {
                const std::string ln = body.substr(lr.at, lr.len);
                if (lr.stmt < 0 || lir.bad) {
                    if (lir.bad) {  // the old expansion: every written slot at a spill, every touched slot at a reload
                        for (char ch : ln) out += ch == kSpillTok ? spill : ch == kReloadTok ? reload : std::string(1, ch);
                    } else out += ln;
                    out += '\n';
                    continue;
                }
                const LStmt& s = lir.st[size_t(lr.stmt)];
                const SlotSet st = s.spill ? plannedStores(lir, s) & lv.dirtyIn[size_t(lr.stmt)] : SlotSet();
                const SlotSet rl = s.reload ? plannedReload(lir, s) & (lv.liveOut[size_t(lr.stmt)] | s.use) : SlotSet();
                if (s.spill) g_lvStoreSkip += (plannedStores(lir, s) & ~st).count();
                if (s.reload) g_lvReloadSkip += (plannedReload(lir, s) & ~rl).count();
                if (s.spill == 'c' && summarised(s.site)) g_sitesSumm++;
                else if (s.reload) g_sitesFull++;
                for (char ch : ln) {
                    if (ch == kSpillTok) {
                        for (int x = 0; x < 105; x++)
                            if (st.test(size_t(x))) out += storeOf(idxSlot(x));
                    } else if (ch == kReloadTok) {
                        for (int x = 0; x < 105; x++)
                            if (rl.test(size_t(x))) out += loadOf(idxSlot(x));
                    } else out += ch;
                }
                out += '\n';
            }
            if (!body.empty() && body.back() != '\n') out.pop_back();
            body.swap(out);
            out.clear();
        }
        // RL-NX (9.57): a call site whose statement is one direct call to a summarised callee spills and reloads
        // only what that callee can read and write (see g_summ); every other marker expands as before.
        const Summary* site = nullptr;
        for (size_t i = 0; !g_live && i < body.size(); i++) {
            const char ch = body[i];
            if (ch == kSpillTok) {
                site = nullptr;
                // RL-NX (Stage 1): a spill in front of a return (or the one the function falls off its end with)
                // stores only the ABI-live slots; a tail call keeps the full spill, its callee reads the arguments
                {
                    size_t k = i + 1;
                    while (k < body.size() && (body[k] == ' ' || body[k] == '\t')) k++;
                    const bool ret = body.compare(k, 7, "return;") == 0 ||
                                     body.find_first_not_of(" \t\n", i + 1) == std::string::npos;
                    if (ret) {
                        out += retSpill;
                        continue;
                    }
                }
                if (g_summOn) {
                    const size_t j = body.find_first_of(std::string{kSpillTok, kReloadTok, '\n'}, i + 1);
                    if (j != std::string::npos && body[j] == kReloadTok) site = directCallee(body.substr(i + 1, j - i - 1));
                }
                if (!site) {
                    out += spill;
                    continue;
                }
                g_sitesSumm++;
                for (const Slot& sl : written) {
                    if (site->read.test(size_t(slotIndex(sl))))
                        out += (sl.kind == 'f' || sl.kind == 'p') ? slotStoreStmt(sl.kind, sl.n)
                                                                  : fmt(" %s = %s;", sl.field().c_str(), sl.local().c_str());
                    else g_spillSaved++;
                }
            } else if (ch == kReloadTok) {
                if (!site) {
                    out += reload;
                    g_sitesFull++;
                    continue;
                }
                for (const Slot& sl : touched) {
                    if (site->write.test(size_t(slotIndex(sl))))
                        out += (sl.kind == 'f' || sl.kind == 'p')
                                   ? fmt(" %s = %s;", sl.local().c_str(), slotLoadExpr(sl.kind, sl.n).c_str())
                                   : fmt(" %s = %s;", sl.local().c_str(), sl.field().c_str());
                    else g_reloadSaved++;
                }
                site = nullptr;
            } else {
                out += ch;
            }
        }
        if (!g_live) body.swap(out);  // RL-NX (Stage 11): the liveness expansion swapped already
    }
    if (needEntryGoto) o += fmt("    goto L_%08X;\n", f.entry);
    o += body;
    o += "}\n\n";
}

// RL-NX: statements that may change guest registers. The carry/divide/shift/PSQ/memory-maintenance
// helpers are listed as pure so the hot arithmetic does not spill around them.
bool Emitter::canClobber(const std::string& s) {
    // RL-NX: safepoint() DELIVERS guest exceptions and rfi() returns from one; both run guest code
    // that reads and writes c.r[], so neither is pure. Treating them as pure left dirty locals in
    // registers across the handler - the black screen the locals build had.
    // RL-NX (9.55): pure calls are cut out of the statement before the search, so a statement holding a pure
    // call AND a real one (a dispatch) still counts as clobbering. psqLd/psqSt/fcmpv take c only for the GQR
    // tables and FPSCR; their values travel through the locals.
    static const char* kPure[] = {"addCarry(c", "addeCarry(c", "addOv(c", "sraw(c", "srawi(c", "fctiw(c",
                                  "psqLd(c", "psqSt(c", "psqLdF(c", "psqStF(c", "fcmpv(c", "fcmpvF(c", "mfspr(c", "mtspr(c", "mftb(c", "mfsr(c",
                                  "mtsr(c", "dcbi(c", "icbi(c", "dcbz(c", "dcbzl(c", "tlbie(c",
                                  "traceEnter(c", "illegal(c", };
    std::string t = s;
    for (const char* p : kPure)
        for (size_t k = t.find(p); k != std::string::npos; k = t.find(p, k)) t.replace(k, std::strlen(p), "pure_(x");
    return t.find("(c,") != std::string::npos || t.find("(c)") != std::string::npos;
}

void Emitter::out(std::string& o, const std::string& s0) const {
    if (!g_locals) {
        o += "    " + s0 + "\n";
        return;
    }
    std::string s = withBase(s0);  // RL-NX (9.55): accessors take the RAM base M
    const bool marked = s.find(kSpillTok) != std::string::npos || s.find(kReloadTok) != std::string::npos;
    if (!marked) {
        // RL-NX (9.55): a statement the emitter did not place markers in itself: a call that may run guest code
        // gets spill + reload around it, a return gets a spill in front.
        if (canClobber(s)) s = std::string(1, kSpillTok) + s + std::string(1, kReloadTok);
        else if (s.find("return") != std::string::npos) s = std::string(1, kSpillTok) + s;
    }
    // RL-NX (9.55): r1 is written through to Ctx at once (see the note at g_locals)
    std::set<Slot> wr;
    scanSlots(s, nullptr, &wr);
    if (wr.count(Slot{'r', 1})) s += " c.r[1] = r1;";
    o += "    " + s + "\n";
}

bool Emitter::write(const std::string& outDir, int files) {
    g_locals = !getenv("R2RECOMP_NOLOCALS");  // RL-NX: R2RECOMP_NOLOCALS=1 regenerates the old c.r[] form
    std::vector<const Function*> list;
    for (auto& [e, f] : an_.funcs) list.push_back(&f);

    // RL-NX (9.57): call-site summaries - a first pass collects every function's own slot sets and calls, then
    // they are closed over the call graph (an unknown or opaque callee makes the caller opaque).
    {
        const char* e = getenv("R2RECOMP_SUMMARY");
        g_summOn = g_locals && !(e && *e == '0');
        g_summStrict = e && !strcmp(e, "strict");
        const char* fl = getenv("R2RECOMP_FLOATFPR");  // RL-NX (9.58)
        g_floatOn = !(fl && *fl == '0');
        const char* a2 = getenv("R2RECOMP_ABI2");  // RL-NX (Stage 8)
        g_abi2 = !(a2 && *a2 == '0');
        const char* ab = getenv("R2RECOMP_ABI");  // RL-NX (Stage 1)
        g_abiOn = g_locals && !(ab && *ab == '0');
        const char* sf = getenv("R2RECOMP_STACKFAST");  // RL-NX (Stage 3)
        g_stackFast = !(sf && *sf == '0');
        const char* vs = getenv("R2RECOMP_VMSITES");  // RL-NX (Stage 9)
        g_vmInline = !(vs && *vs == '0');
        const char* vf = getenv("R2RECOMP_VMFNS");  // RL-NX (audit 2026-11-27)
        g_vmFns = !(vf && *vf == '0');
        const char* pp = getenv("R2RECOMP_PIPEFAST");  // RL-NX (Stage 4)
        g_pipeFast = !(pp && *pp == '0');
        const char* pf = getenv("R2RECOMP_PSFLOAT");  // RL-NX (Stage 2)
        g_psFloat = !(pf && *pf == '0');
        const char* fm = getenv("R2RECOMP_FASTMEM");  // RL-NX (Stage 11): off unless =1
        g_fastmem = g_locals && fm && *fm == '1';
        const char* sd = getenv("R2RECOMP_SDAFAST");  // RL-NX (Stage 11)
        g_sdaFast = !(sd && *sd == '0');
        const char* lv = getenv("R2RECOMP_LIVE");  // RL-NX (Stage 11)
        g_live = g_locals && !(lv && *lv == '0');
        const char* so = getenv("R2RECOMP_SO0");  // RL-NX (Stage 11)
        g_so0 = !(so && *so == '0');
        const char* gq = getenv("R2RECOMP_GQR0");  // RL-NX (Stage 1)
        g_gqr0Const = !(gq && *gq == '0');
    }
    g_live = g_live && g_summOn;  // RL-NX (Stage 11): the read sets need the summaries
    for (u32 n : {1u, 2u, 13u}) g_abiArgs.set(n);  // RL-NX (Stage 11): the ABI2 indirect-call reads
    for (u32 n = 3; n <= 10; n++) g_abiArgs.set(n);
    for (u32 n = 1; n <= 8; n++) g_abiArgs.set(32 + n), g_abiArgs.set(64 + n);
    g_abiArgs.set(96 + 1);
    if (g_summOn) {
        g_summPass = true;
        std::string scratch;
        for (auto* f : list) {
            g_summEntry = f->entry;
            scratch.clear();
            function(*f, scratch, fn(f->entry));
            // RL-NX (Stage 9): an "abi" hook reads and writes no more than its recompiled body (its summary)
            if (cfg_.hooks.count(f->entry) && !cfg_.abiHooks.count(f->entry)) g_summ[f->entry].opaque = true;  // callers reach the native hook
        }
        g_summPass = false;
        for (auto& [a, sm] : g_summ)
            if (sm.opaque) sm.read.set(), sm.write.set();
        // RL-NX (Stage 8): an indirect call's ABI sets. Reads: r1 r2 r13, the argument GPRs r3-r10 and FPRs f1-f8 (both
        // halves), cr1 (the varargs float flag). Writes: every volatile slot (r0, r3-r12, f0-f13 both halves, cr0 cr1
        // cr5-cr7, ctr), less what the function saves itself.
        if (g_abi2) {
            SlotSet ar, vw;
            for (u32 n : {1u, 2u, 13u}) ar.set(n);
            for (u32 n = 3; n <= 10; n++) ar.set(n);
            for (u32 n = 1; n <= 8; n++) ar.set(32 + n), ar.set(64 + n);
            ar.set(96 + 1);
            vw.set(0);
            for (u32 n = 3; n <= 12; n++) vw.set(n);
            for (u32 n = 0; n <= 13; n++) vw.set(32 + n), vw.set(64 + n);
            for (u32 n : {0u, 1u, 5u, 6u, 7u}) vw.set(96 + n);
            vw.set(104);
            size_t ind = 0;
            for (auto& [a, sm] : g_summ)
                if (!sm.opaque && sm.indirect) sm.read |= ar, sm.write |= vw & ~sm.pres, ind++;
            fprintf(stderr, "r2recomp: ABI2: %zu functions with indirect calls summarised as ABI calls\n", ind);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [a, sm] : g_summ) {
                if (sm.opaque) continue;
                SlotSet r = sm.read, w = sm.write;
                bool op = false;
                for (u32 t : sm.callees) {
                    auto it = g_summ.find(t);
                    if (it == g_summ.end() || it->second.opaque) {
                        op = true;
                        break;
                    }
                    r |= it->second.read;
                    w |= it->second.write & ~sm.pres;  // RL-NX (Stage 8): what it saves, it restores
                }
                if (op) {
                    sm.opaque = true;
                    sm.read.set();
                    sm.write.set();
                    changed = true;
                } else if (r != sm.read || w != sm.write) {
                    sm.read = r;
                    sm.write = w;
                    changed = true;
                }
            }
        }
        // RL-NX (Stage 1): a caller reloads only the ABI-live part of what a summarised callee writes
        if (g_abiOn)
            for (auto& [a, sm] : g_summ) sm.write &= abiMask();
        // RL-NX (Stage 11): the read sets again, from liveness (the mentioned-slot closure above is replaced)
        if (g_live) solveReads();
        size_t opq = 0;
        for (auto& [a, sm] : g_summ) opq += sm.opaque;
        if (g_abi2) fprintf(stderr, "r2recomp: ABI2: %llu callee-saved slots saved by their functions\n", g_presSlots);  // RL-NX (Stage 8)
        fprintf(stderr, "r2recomp: call-site summaries%s: %zu functions, %zu opaque; made opaque by their own calls:",
                g_summStrict ? " (strict)" : "", g_summ.size(), opq);
        for (auto& [n, k] : g_opaqueWhy) fprintf(stderr, " %s %u", n.c_str(), k);
        fprintf(stderr, "\n");
    }

    // header
    {
        std::string h = "// Generated by r2recomp. Do not edit.\n#pragma once\n#include \"ctx.h\"\n\nnamespace rt {\n";
        for (auto* f : list) {
            h += "void " + fn(f->entry) + "(Ctx& c);";
            if (auto it = cfg_.names.find(f->entry); it != cfg_.names.end()) h += "  // " + it->second;
            h += "\n";
            if (cfg_.hooks.count(f->entry)) h += "void " + fn(f->entry) + "_orig(Ctx& c);\n";
        }
        for (auto& [a, n] : cfg_.hooks) h += "void hle_" + n + "(Ctx& c);\n";
        h += "}  // namespace rt\n";
        if (!writeFile(outDir + "/funcs.h", h)) return false;
    }
    // bodies, split by instruction count
    size_t total = 0;
    for (auto* f : list) total += f->insns.size();
    const size_t per = total / size_t(files) + 1;
    int file = 0;
    size_t acc = 0;
    std::string o;
    auto flush = [&]() -> bool {
        if (o.empty()) return true;
        std::string full = "// Generated by r2recomp. Do not edit.\n#include \"funcs.h\"\n\nnamespace rt {\n\n" + o + "}  // namespace rt\n";
        o.clear();
        return writeFile(outDir + fmt("/code_%03d.cpp", file++), full);
    };
    for (auto* f : list) {
        if (auto it = cfg_.hooks.find(f->entry); it != cfg_.hooks.end()) {
            o += "void " + fn(f->entry) + "(Ctx& c) { hle_" + it->second + "(c); }\n";
            function(*f, o, fn(f->entry) + "_orig");
        } else {
            function(*f, o, fn(f->entry));
        }
        acc += f->insns.size();
        if (acc >= per) {
            if (!flush()) return false;
            acc = 0;
        }
    }
    if (!flush()) return false;
    if (g_vmInline)  // RL-NX (Stage 9)
        fprintf(stderr, "r2recomp: VM sites: %zu instructions + %zu functions listed, %llu instructions given the V forms (both passes)\n", cfg_.vmSites.size(), cfg_.vmFns.size(), g_vmSiteCount);
    if (g_floatOn && g_locals)  // RL-NX (9.58)
        fprintf(stderr, "r2recomp: float FPRs: %llu functions, %llu registers\n", (unsigned long long)g_floatFns,
                (unsigned long long)g_floatRegs);
    if (g_summOn)  // RL-NX (9.57)
        fprintf(stderr, "r2recomp: %llu call sites summarised (%llu stores and %llu loads left out), %llu full\n",
                (unsigned long long)g_sitesSumm, (unsigned long long)g_spillSaved, (unsigned long long)g_reloadSaved,
                (unsigned long long)g_sitesFull);
    if (g_abiOn)  // RL-NX (Stage 1)
        fprintf(stderr, "r2recomp: ABI write-back: %llu return stores left out\n", (unsigned long long)g_retSaved);
    if (g_gqr0Const) fprintf(stderr, "r2recomp: GQR0 constant (plain floats)\n");  // RL-NX (Stage 1)
    if (g_so0) fprintf(stderr, "r2recomp: XER[SO] constant 0\n");  // RL-NX (Stage 11)
    if (g_sdaFast) fprintf(stderr, "r2recomp: small data: %llu r2/r13 accesses take the stack forms\n", g_sdaSites);  // RL-NX (Stage 11)
    if (g_fastmem)  // RL-NX (Stage 11)
        fprintf(stderr, "r2recomp: FASTMEM: %llu accesses given the F forms (both passes), %zu sites + %zu functions kept tested\n",
                g_fmCount, cfg_.fmSites.size(), cfg_.fmFns.size());
    if (g_live)  // RL-NX (Stage 11)
        fprintf(stderr, "r2recomp: liveness: %llu entry loads, %llu stores and %llu reloads left out\n",
                g_lvEntrySkip, g_lvStoreSkip, g_lvReloadSkip);
    for (; file < files; file++)  // keep the file set stable for the build system
        if (!writeFile(outDir + fmt("/code_%03d.cpp", file), "// empty\n")) return false;

    // function table + names
    {
        std::string t = "// Generated by r2recomp. Do not edit.\n#include \"funcs.h\"\n#include \"table.h\"\n\nnamespace rt {\n";
        t += "const FuncEntry g_funcTable[] = {\n";
        for (auto* f : list) t += fmt("    {%s, %s},\n", hex(f->entry).c_str(), fn(f->entry).c_str());
        t += "};\nconst u32 g_funcCount = sizeof(g_funcTable) / sizeof(g_funcTable[0]);\n";
        t += "const SymName g_symNames[] = {\n";
        for (auto& [a, n] : cfg_.names) t += fmt("    {%s, \"%s\"},\n", hex(a).c_str(), n.c_str());
        t += "    {0, nullptr},\n};\n";
        t += "const FuncEntry g_origTable[] = {\n";
        for (auto& [a, n] : cfg_.hooks)
            if (an_.funcs.count(a)) t += fmt("    {%s, %s_orig},\n", hex(a).c_str(), fn(a).c_str());
        t += "    {0, nullptr},\n};\n}  // namespace rt\n";
        if (!writeFile(outDir + "/table.cpp", t)) return false;
    }
    return true;
}

bool Emitter::writeFile(const std::string& path, const std::string& text) {
    // Only rewrite when the content changes (keeps incremental builds fast).
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (old == text) return true;
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    out << text;
    return bool(out);
}
