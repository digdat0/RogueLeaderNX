// GX FIFOs: write-gather pipe -> PI FIFO in memory -> command processor.
//
// As on the console, everything the CPU writes to the gather pipe (0xCC008000) lands in the
// CPU FIFO in main memory at the PI write pointer. The command processor reads its own FIFO
// (CP base/end/read/write pointers) when reading is enabled; while the two are linked (CP_CR
// GP link, the normal immediate mode) its write pointer follows the PI's. Display lists are
// recorded by pointing the PI FIFO at a buffer with the link off, and executed later through
// CALL_DL. The GPU is infinitely fast here: commands run as soon as they are complete.
#include "gx_fifo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>  // RL-NX (9.57): the GP thread's posted PE events
#include <string>
#include <unordered_map>
#include <vector>

#include "../runtime.h"

namespace rt {
namespace {
u8 s_gpBuf[64];  // RL-NX (9.53): the gather buffer (s_gp below names it)
}  // namespace
// RL-NX (9.53): ctx.h's inline gather pipe (gpPut) - next free byte, and the burst limit (null: slow path).
u8* g_gpCur RT_HIDDEN = s_gpBuf;
u8* g_gpLimit RT_HIDDEN = s_gpBuf + 32;
// RL-NX (Stage 4): the limit ctx.h's wr8P/wr16P/wr32P and psqStoreQv's pipe case test: g_gpLimit while the - menu's
// FAST PIPE row is on (R2_FAST_PIPE, default on), null when it is off, so those writes go back to wrSlow's pipe case.
u8* g_gpLimitP RT_HIDDEN = s_gpBuf + 32;
bool s_fastPipe = true;
namespace {

GxStats s_stats;
GxSink s_sink = nullptr;
void (*s_syncHook)() = nullptr;  // the renderer catches up (the CPU looks at the GP's progress)
bool s_log = false;

// --- PI (CPU side) FIFO ---
u32 s_piBase = 0, s_piEnd = 0, s_piWp = 0;  // physical; wp bit 29 = wrap flag
// --- CP (GPU side) FIFO ---
u16 s_cpReg[0x40] = {};  // raw 16-bit registers 0x00..0x7E
u32 s_cpBase = 0, s_cpEnd = 0, s_cpWp = 0, s_cpRp = 0, s_cpHi = 0, s_cpLo = 0, s_cpBp = 0;
u16 s_cpCr = 0;
std::vector<u8> s_cmd;  // bytes of an incomplete command read from the CP FIFO

// RL-NX (9.57): the vertex formats command lengths depend on, as a struct: with the GP thread on, the game thread
// walks the stream with its own copy (s_vtxWalk) while the worker runs it with this one.
struct VtxState {
    u32 vcdLo = 0, vcdHi = 0;
    u32 vatA[8] = {}, vatB[8] = {}, vatC[8] = {};
    // RL-NX (Stage 5): vertexSize per format, cached (bit f of vsOk set = vs[f] is current). cpVtxWrite and the
    // save state load clear vsOk; struct copies carry a cache that matches the registers they copy.
    mutable u32 vs[8] = {};
    mutable u8 vsOk = 0;
};
VtxState s_vtx;

u32 compSize(u32 fmt) {
    switch (fmt) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    case 4: return 4;
    default: return 0;
    }
}
u32 colSize(u32 fmt) {
    switch (fmt) {
    case 0: case 3: return 2;
    case 1: case 4: return 3;
    case 2: case 5: return 4;
    default: return 0;
    }
}
u32 idxSize(u32 type) { return type == 2 ? 1 : type == 3 ? 2 : 0; }

u32 vertexSize(u32 fmt, const VtxState& v) {  // RL-NX (9.57): of the given state
    const u32 lo = v.vcdLo, hi = v.vcdHi, A = v.vatA[fmt], B = v.vatB[fmt], C = v.vatC[fmt];
    u32 size = 0;
    for (int i = 0; i < 9; i++)
        if (lo & (1u << i)) size += 1;  // PNMTXIDX, TEX0..7MTXIDX
    const u32 pos = (lo >> 9) & 3;
    if (pos == 1) size += compSize((A >> 1) & 7) * ((A & 1) ? 3 : 2);
    else size += idxSize(pos);
    const u32 nrm = (lo >> 11) & 3;
    const bool nbt = (A >> 9) & 1;
    if (nrm == 1) size += compSize((A >> 10) & 7) * (nbt ? 9 : 3);
    else if (nrm) size += idxSize(nrm) * ((nbt && (A >> 31)) ? 3 : 1);
    const u32 c0 = (lo >> 13) & 3, c1 = (lo >> 15) & 3;
    if (c0 == 1) size += colSize((A >> 14) & 7);
    else size += idxSize(c0);
    if (c1 == 1) size += colSize((A >> 18) & 7);
    else size += idxSize(c1);
    struct Tex {
        u32 cnt, fmt;
    } tex[8] = {
        {(A >> 21) & 1, (A >> 22) & 7}, {B & 1, (B >> 1) & 7},         {(B >> 9) & 1, (B >> 10) & 7},
        {(B >> 18) & 1, (B >> 19) & 7}, {(B >> 27) & 1, (B >> 28) & 7}, {(C >> 5) & 1, (C >> 6) & 7},
        {(C >> 14) & 1, (C >> 15) & 7}, {(C >> 23) & 1, (C >> 24) & 7},
    };
    for (int i = 0; i < 8; i++) {
        const u32 t = (hi >> (2 * i)) & 3;
        if (t == 1) size += compSize(tex[i].fmt) * (tex[i].cnt ? 2 : 1);
        else size += idxSize(t);
    }
    return size;
}

inline u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
inline u32 be16(const u8* p) { return (u32(p[0]) << 8) | p[1]; }

// Length of the command at p, or 0 if more bytes are needed. RL-NX (9.57): `v` is the vertex state to size draws
// with (s_vtx unless the GP thread's walker asks).
size_t cmdLength(const u8* p, size_t avail, const VtxState& v = s_vtx) {
    const u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        if (avail < 3) return 0;
        // RL-NX (Stage 5): cmdLength was 2.35% of the CPU profile (every draw recomputed the size from VCD/VAT)
        const u32 f = op & 7;
        if (!(v.vsOk & (1u << f))) {
            v.vs[f] = vertexSize(f, v);
            v.vsOk |= u8(1u << f);
        }
        const u32 vs = v.vs[f];
        if (!vs && be16(p + 1)) {
            static int warned = 0;
            if (warned++ < 8)
                logf("gx", "draw %02X with %u vertices of size 0: VCD %08X/%08X VAT %08X %08X %08X", op, be16(p + 1), v.vcdLo,
                     v.vcdHi, v.vatA[op & 7], v.vatB[op & 7], v.vatC[op & 7]);
        }
        return 3 + size_t(be16(p + 1)) * vs;
    }
    switch (op) {
    case 0x00: case 0x48: return 1;
    case 0x08: return 6;
    case 0x10:
        if (avail < 5) return 0;
        return 5 + 4 * size_t((be32(p + 1) >> 16) + 1);
    case 0x20: case 0x28: case 0x30: case 0x38: return 5;
    case 0x40: return 9;
    case 0x61: return 5;
    default: return 1;  // unknown: skip a byte and resynchronize
    }
}

void runDisplayList(u32 addr, u32 size, int depth);

// RL-NX (9.57): a CP register write (opcode 0x08) as it affects command lengths
void cpVtxWrite(VtxState& s, u8 reg, u32 v) {
    s.vsOk = 0;  // RL-NX (Stage 5): the cached vertex sizes are stale
    if (reg == 0x50) s.vcdLo = v;
    else if (reg == 0x60) s.vcdHi = v;
    else if ((reg & 0xF8) == 0x70) s.vatA[reg & 7] = v;
    else if ((reg & 0xF8) == 0x80) s.vatB[reg & 7] = v;
    else if ((reg & 0xF8) == 0x90) s.vatC[reg & 7] = v;
}

struct Recent {  // last commands, for desync reports
    u32 len = 0;
    bool dl = false;
    u8 bytes[12] = {};
};
Recent s_recent[48];
u32 s_recentPos = 0;

int s_cmdLog = 0;     // R2_FIFO_LOG=2: every command (first 16 bytes) once the GP FIFO has moved
int s_gpFifoSets = 0;  // GP FIFO base writes (GXSetGPFifo)

// RL-NX (9.57): the GP thread (gx_fifo.h). s_gpAsync is switched by the game thread with the worker drained, so
// whichever thread parses owns s_cmd and the VCD/VAT state at the time. s_gpParsing is set while gxGpParse runs
// (on the worker): the PE events execute() finds are then posted instead of applied.
bool s_gpAsync = false;
bool s_gpParsing = false;
// RL-NX (Stage 6): raw mode of the GP thread. The game thread's walker (walkBuf/cmdLength/walkDl, ~3-5 ms a frame
// in the 13:54 explosion profile at stock clocks) is skipped: the CP bytes go to the worker as they are and its
// parseChunk splits them with s_cmd/s_vtx, running called display lists from guest memory (runDisplayList). The walker
// copied each list at once because the game may rewrite it as soon as the read pointer has passed the call; here
// every CP register read waits for the worker first (s_gpDrainBridge), so the game can never see the read pointer
// past a list the worker has not run. PE events were already ordered (the worker posts them). Switched only with
// the worker drained (gxGpSetRaw); R2_GP_RAW=0 or the - menu's GP RAW row goes back to the walker.
bool s_gpRaw = true;
void (*s_gpDrainBridge)() = nullptr;  // the bridge worker has run everything queued (not the renderer)
// RL-NX (Stage 11): CP LAG (- menu row, R2_CP_LAG, default on). Raw mode drained the whole bridge backlog at every CP
// register read so the game never saw the read pointer past a display list the worker had not run - +3 ms of gxsync
// a frame on hardware (1532 vs 1537), which ate the walker's 2.5-4 ms and made GP RAW neutral. With CP LAG the
// reads do not wait: they report the read pointer the worker has really reached, as a real GP that lags behind
// does. cpRun records, for each chunk it hands over, the ring position after it (s_gpWritePos) and the read pointer
// after it; a CP read pops the chunks the worker has consumed (s_gpReadPos) and reports the last one's pointer
// (s_cpRpVis) - all of them consumed: the true one. A list is run with its chunk, so the game still sees the pointer
// past a call only once the worker has run the list. The internal pointer (watermark interrupts, breakpoint) moves
// at once as before. Off: the drain.
bool s_cpLag = true;
u64 (*s_gpWritePos)() = nullptr;  // bridge: ring position after everything queued so far
u64 (*s_gpReadPos)() = nullptr;   // bridge: ring position the worker has consumed
struct LagEntry {
    u64 pos;
    u32 rp;
};
constexpr u32 kLagCap = 512;
LagEntry s_lag[kLagCap];
u32 s_lagHead = 0, s_lagCount = 0;  // a ring of pending chunks, oldest at s_lagHead
u32 s_cpRpVis = 0;                  // the read pointer the game sees while chunks are pending
bool cpLagActive() { return s_cpLag && s_gpAsync && s_gpRaw && s_gpWritePos && s_gpReadPos; }
void cpLagClear() {
    s_lagHead = s_lagCount = 0;
    s_cpRpVis = s_cpRp;
}
// the chunk just handed over ends at the current write position, with the read pointer at rp
void cpLagPush(u32 rp) {
    const u64 pos = s_gpWritePos();
    if (s_lagCount && s_lag[(s_lagHead + s_lagCount - 1) % kLagCap].pos == pos) {  // it joined the open record
        s_lag[(s_lagHead + s_lagCount - 1) % kLagCap].rp = rp;
        return;
    }
    if (s_lagCount == kLagCap) {  // full: the newest entry stands for both (the pointer then lags a little more)
        s_lag[(s_lagHead + s_lagCount - 1) % kLagCap] = {pos, rp};
        return;
    }
    s_lag[(s_lagHead + s_lagCount) % kLagCap] = {pos, rp};
    s_lagCount++;
}
// the read pointer the game may see now
u32 cpLagRp() {
    const u64 done = s_gpReadPos();
    while (s_lagCount && s_lag[s_lagHead].pos <= done) {
        s_cpRpVis = s_lag[s_lagHead].rp;
        s_lagHead = (s_lagHead + 1) % kLagCap;
        s_lagCount--;
    }
    if (!s_lagCount) s_cpRpVis = s_cpRp;
    return s_cpRpVis;
}
GxChunkSink s_chunkSink = nullptr;
GxChunkSink s_dlSink = nullptr;  // a called display list's bytes, copied when the game thread walks the call
VtxState s_vtxWalk;              // game thread: the walker's vertex state
std::vector<u8> s_walkCmd;       // game thread: a command cut by the end of a chunk (the walker's s_cmd)
void (*s_gpKickHook)() = nullptr;
void (*s_gpDrainAll)() = nullptr;
void (*s_gpFence)() = nullptr;
// Posted by the worker: the latest token value and how many tokens (without / with interrupt) and draw dones
// it has run. The game thread applies what changed since it last looked (gxGpPoll).
std::atomic<u32> s_gpTokVal{0}, s_gpTokSeq{0}, s_gpTokIrqSeq{0}, s_gpFinSeq{0};
u32 s_gpTokSeen = 0, s_gpTokIrqSeen = 0, s_gpFinSeen = 0;  // game thread

void gpToken(u16 token, bool irq) {
    if (!s_gpParsing) return peToken(token, irq);
    if (s_gpFence) s_gpFence();  // the game may reuse what earlier draws read once it sees the token
    s_gpTokVal.store(token, std::memory_order_relaxed);
    (irq ? s_gpTokIrqSeq : s_gpTokSeq).fetch_add(1, std::memory_order_release);
    wakeGuest();
}
void gpFinish() {
    if (!s_gpParsing) return peFinish();
    if (s_gpFence) s_gpFence();
    s_gpFinSeq.fetch_add(1, std::memory_order_release);
    wakeGuest();
}

void execute(const u8* p, size_t len, int depth) {
    const u8 op = p[0];
    bool known = true;
    if (s_cmdLog >= 2 && s_gpFifoSets >= 2 && op != 0x00) {
        char hexb[64] = {};
        for (size_t k = 0; k < len && k < 16; k++) snprintf(hexb + 3 * k, 4, "%02X ", p[k]);
        logf("cmd", "%s%s(len %zu)", depth ? "DL " : "", hexb, len);
    }
    if (op >= 0x80 && op < 0xC0) {
        s_stats.draws++;
        s_stats.vertices += be16(p + 1);
    } else {
        switch (op) {
        case 0x00: case 0x48: break;
        case 0x08: {
            s_stats.cp++;
            const u8 reg = p[1];
            const u32 v = be32(p + 2);
            cpVtxWrite(s_vtx, reg, v);  // RL-NX (9.57)
            break;
        }
        case 0x10: s_stats.xf++; break;
        case 0x20: case 0x28: case 0x30: case 0x38: s_stats.xf++; break;
        case 0x40: {
            s_stats.dl++;
            const u32 a = be32(p + 1), n = be32(p + 5);
            if (s_cmdLog >= 2 && s_gpFifoSets >= 2) logf("gx", "call display list %08X size %X", a, n);
            if (depth == 0) runDisplayList(a, n, depth + 1);
            break;
        }
        case 0x61: {
            s_stats.bp++;
            const u32 v = be32(p + 1);
            const u8 reg = u8(v >> 24);
            if (s_log && (reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52))
                logf("gx", "BP %02X = %06X%s", reg, v & 0xFFFFFF, depth ? " (display list)" : "");
            // RL-NX (9.57): posted to the game thread when the GP thread runs this (gpToken / gpFinish)
            if (reg == 0x45 && (v & 2)) gpFinish();
            else if (reg == 0x47) gpToken(u16(v), true);
            else if (reg == 0x48) gpToken(u16(v), false);
            break;
        }
        default:
            known = false;
            if (s_stats.unknown++ < 16) {
                logf("gx", "unknown FIFO opcode %02X (%s); last commands, oldest first:", op, depth ? "display list" : "FIFO");
                for (u32 i = 0; i < 48; i++) {
                    const Recent& r = s_recent[(s_recentPos + i) % 48];
                    if (!r.len) continue;
                    char hexb[64] = {};
                    for (u32 k = 0; k < r.len && k < 12; k++) snprintf(hexb + 3 * k, 4, "%02X ", r.bytes[k]);
                    logf("gx", "  %s len %5u: %s", r.dl ? "DL  " : "FIFO", r.len, hexb);
                }
            }
            break;
        }
    }
    {
        Recent& r = s_recent[s_recentPos++ % 48];
        r.len = u32(len);
        r.dl = depth != 0;
        memcpy(r.bytes, p, len < 12 ? len : 12);
    }
    // The renderer gets every command, display list contents inline (not the CALL_DL).
    if (s_sink && op != 0x40 && known) s_sink(p, u32(len));
}

// Commands in a display list run with the current CP state.
void runDisplayList(u32 addr, u32 size, int depth) {
    const u8* base = physPtr(addr, size);
    if (!base) {
        logf("gx", "display list outside memory: %08X size %X", addr, size);
        return;
    }
    size_t pos = 0;
    while (pos < size) {
        const size_t len = cmdLength(base + pos, size - pos);
        if (!len || pos + len > size) break;
        execute(base + pos, len, depth);
        pos += len;
    }
}

// CP_CR bits
constexpr u16 CR_READ = 1 << 0, CR_BP = 1 << 1, CR_OVFINT = 1 << 2, CR_UNFINT = 1 << 3, CR_LINK = 1 << 4,
              CR_BPINT = 1 << 5;
bool s_bpHit = false;  // CP_SR breakpoint: the GP stopped at the breakpoint

// Bytes written but not yet read by the GP (it only lags behind a breakpoint).
u32 cpDistance() {
    const u32 size = s_cpEnd + 32 - s_cpBase;
    if (!s_cpEnd || s_cpEnd <= s_cpBase) return 0;
    return s_cpWp >= s_cpRp ? s_cpWp - s_cpRp : s_cpWp + size - s_cpRp;
}
// The watermark flags are live, as the hardware evaluates them: the SDK suspends the writing
// thread on overflow and resumes it on underflow.
bool cpOverflow() { return cpDistance() > s_cpHi; }
bool cpUnderflow() { return cpDistance() < s_cpLo; }

void cpUpdateIrq() {
    const bool on = (s_bpHit && (s_cpCr & CR_BPINT)) || ((s_cpCr & CR_OVFINT) && cpOverflow()) ||
                    ((s_cpCr & CR_UNFINT) && cpUnderflow());
    piSetIrq(PI_CP, on);
}

// RL-NX (9.53): commands run straight from the FIFO in guest memory. Every 32-byte burst used to be
// appended to s_cmd, split there and erased from its front (a vector insert and a memmove per burst,
// ~0.7 ms a frame in the 20:14 profile); now only a command cut by the end of a chunk is carried over
// in s_cmd, and the chunks after it join it there until it completes. The read pointer moves before
// the commands run, as it did when they ran after cpRun's loop.
// RL-NX (9.57): moved out of cpRun so the GP thread (gxGpParse) runs the same splitter.
void parseChunk(const u8* src, size_t n) {
    size_t pos = 0;
    if (s_cmd.empty()) {
        while (pos < n) {
            const size_t len = cmdLength(src + pos, n - pos);
            if (!len || pos + len > n) break;
            execute(src + pos, len, 0);
            pos += len;
        }
        if (pos < n) s_cmd.assign(src + pos, src + n);
    } else {
        s_cmd.insert(s_cmd.end(), src, src + n);
        while (pos < s_cmd.size()) {
            const size_t len = cmdLength(&s_cmd[pos], s_cmd.size() - pos);
            if (!len || pos + len > s_cmd.size()) break;
            execute(&s_cmd[pos], len, 0);
            pos += len;
        }
        if (pos) s_cmd.erase(s_cmd.begin(), s_cmd.begin() + ptrdiff_t(pos));
    }
}

// RL-NX (9.57): the GP thread's walker, on the game thread. It finds where each command ends (which needs the
// vertex formats, so it follows the CP writes, those inside display lists included) and hands the worker whole
// commands only. A display-list call is replaced by a copy of the list taken now: the game may rewrite the list
// as soon as the GP's read pointer has passed the call, which it does here at once, long before the worker runs it
// (the first GP-thread build read the lists late and lost the stream: a rewritten list's vertex formats sized the
// draws after it wrongly, "unknown FIFO opcode E3" at 92 s in the Ryujinx attract).
void walkDl(u32 addr, u32 size) {
    const u8* base = physPtr(addr, size);
    if (!base) {
        logf("gx", "display list outside memory: %08X size %X", addr, size);
        return;
    }
    size_t pos = 0;
    while (pos < size) {
        const size_t len = cmdLength(base + pos, size - pos, s_vtxWalk);
        if (!len || pos + len > size) break;
        if (base[pos] == 0x08) cpVtxWrite(s_vtxWalk, base[pos + 1], be32(base + pos + 2));
        pos += len;
    }
    s_dlSink(base, size);
}
size_t walkBuf(const u8* p, size_t n) {
    size_t pos = 0, start = 0;
    while (pos < n) {
        const size_t len = cmdLength(p + pos, n - pos, s_vtxWalk);
        if (!len || pos + len > n) break;
        const u8 op = p[pos];
        if (op == 0x08) {
            cpVtxWrite(s_vtxWalk, p[pos + 1], be32(p + pos + 2));
        } else if (op == 0x40) {
            if (pos > start) s_chunkSink(p + start, u32(pos - start));
            walkDl(be32(p + pos + 1), be32(p + pos + 5));
            start = pos + len;
        }
        pos += len;
    }
    if (pos > start) s_chunkSink(p + start, u32(pos - start));
    return pos;
}
void gpWalk(const u8* src, size_t n) {
    if (s_walkCmd.empty()) {
        const size_t pos = walkBuf(src, n);
        if (pos < n) s_walkCmd.assign(src + pos, src + n);
    } else {
        s_walkCmd.insert(s_walkCmd.end(), src, src + n);
        const size_t pos = walkBuf(s_walkCmd.data(), s_walkCmd.size());
        if (pos) s_walkCmd.erase(s_walkCmd.begin(), s_walkCmd.begin() + ptrdiff_t(pos));
    }
}

// Feed the bytes between the CP read and write pointers to the command splitter. With the
// breakpoint enabled the GP stops when its read pointer reaches it (the game keeps one trailing
// its own write pointer and moves it from the breakpoint interrupt).
void cpRun() {
    if (!(s_cpCr & CR_READ)) return;  // GP FIFO reading disabled
    if (!s_cpEnd || s_cpEnd <= s_cpBase) return;
    int guard = 0;
    while (s_cpRp != s_cpWp && guard++ < 1000000) {
        if ((s_cpCr & CR_BP) && s_cpRp == s_cpBp) {
            if (!s_bpHit) {
                s_bpHit = true;
                cpUpdateIrq();
            }
            break;
        }
        // contiguous run up to the write pointer or the end of the buffer
        u32 stop = s_cpWp > s_cpRp ? s_cpWp : s_cpEnd + 32;
        if ((s_cpCr & CR_BP) && s_cpBp > s_cpRp && s_cpBp < stop) stop = s_cpBp;
        const u32 n = stop - s_cpRp;
        const u8* src = physPtr(s_cpRp, n);
        if (!src) {
            logf("gx", "CP FIFO outside memory: rp %08X wp %08X", s_cpRp, s_cpWp);
            s_cpRp = s_cpWp;
            break;
        }
        s_cpRp += n;
        if (s_cpRp >= s_cpEnd + 32) s_cpRp = s_cpBase;
        // RL-NX (9.57): with the GP thread on, the walker hands the worker the commands (gxGpParse runs them); the
        // read pointer has moved already, as it has for parseChunk.
        if (s_gpAsync) {
            if (s_gpRaw) {
                s_chunkSink(src, n);  // RL-NX (Stage 6): the worker splits (gxGpParse -> parseChunk)
                if (cpLagActive()) cpLagPush(s_cpRp);  // RL-NX (Stage 11): CP LAG
            } else gpWalk(src, n);
            continue;
        }
        parseChunk(src, n);
    }
    cpUpdateIrq();
}

u32 regPair(u32 lo, u32 hi) { return (u32(s_cpReg[hi / 2] & 0x3FF) << 16) | s_cpReg[lo / 2]; }

// RL-NX: R2_DRAW_CALLER - which guest function writes the stream's draw commands (a draw opcode 0x80..0xBF
// or a display-list call 0x40), counted and logged once per window by gxDrawCallerLog. The writes happen
// inside recompiled guest code, so curCtx().curFn is the game function that submits the picture: at the
// title the image layer is missing while the text/icons are submitted, and this names the function that
// submits each half.
bool s_callerOn = false;
std::unordered_map<u32, u32> s_callerCount;

}  // namespace

GxStats& gxStats() { return s_stats; }
void gxSetSink(GxSink sink) { s_sink = sink; }
void gxSetSyncHook(void (*hook)()) { s_syncHook = hook; }
// RL-NX (9.53): see gx_fifo.h
void (*s_peBarrierHook)(int) = nullptr;
void gxSetPeBarrierHook(void (*hook)(int kind)) { s_peBarrierHook = hook; }
void gxPeBarrier(int kind) {
    // RL-NX (9.57): with the GP thread on, the worker has run everything before a PE event it posted (and
    // fenced the renderer), so nothing waits here; a PE register access only looks for posted events.
    if (s_gpAsync) {
        if (kind == 2) {
            gxGpKick();
            gxGpPoll();
        }
        return;
    }
    if (s_peBarrierHook) s_peBarrierHook(kind);
}

// RL-NX (9.57): the GP thread (gx_fifo.h)
void gxSetGpThread(GxChunkSink sink, GxChunkSink dlSink, void (*kick)(), void (*drainAll)(), void (*fence)()) {
    s_chunkSink = sink;
    s_dlSink = dlSink;
    s_gpKickHook = kick;
    s_gpDrainAll = drainAll;
    s_gpFence = fence;
}
void gxGpSetAsync(bool on) {
    if (on && !s_chunkSink) return;
    gxGpPoll();  // events the worker posted before it went idle
    if (on == s_gpAsync) return;
    // The worker is idle: hand the partial command and the vertex state over to the thread that splits next.
    // RL-NX (Stage 6): in raw mode the worker splits with s_cmd/s_vtx, the same state the game thread uses with the
    // GP thread off, so nothing moves.
    cpLagClear();  // RL-NX (Stage 11): the worker is idle, nothing pending
    if (s_gpRaw) {
    } else if (on) {
        s_walkCmd.swap(s_cmd);
        s_cmd.clear();
        s_vtxWalk = s_vtx;
    } else {
        s_cmd.swap(s_walkCmd);
        s_walkCmd.clear();
    }
    s_gpAsync = on;
}
bool gxGpAsync() { return s_gpAsync; }
// RL-NX (Stage 6): raw mode (see s_gpRaw). Game thread, with the worker drained: the splitting state moves between
// the walker (s_walkCmd/s_vtxWalk) and the worker's parseChunk (s_cmd/s_vtx) only while the GP thread is on.
void gxSetGpDrain(void (*drain)()) { s_gpDrainBridge = drain; }
void gxGpSetRaw(bool on) {
    if (on == s_gpRaw) return;
    cpLagClear();  // RL-NX (Stage 11): switched with the worker drained: nothing pending
    if (s_gpAsync) {
        gxGpPoll();
        if (on) {  // walker -> worker: the worker's s_vtx already follows every CP write it ran
            s_cmd.swap(s_walkCmd);
            s_walkCmd.clear();
        } else {   // worker -> walker
            s_walkCmd.swap(s_cmd);
            s_cmd.clear();
            s_vtxWalk = s_vtx;
        }
    }
    s_gpRaw = on;
}
bool gxGpRaw() { return s_gpRaw; }
// RL-NX (Stage 11): CP LAG (see s_cpLag). gxSetCpLag: game thread, with the worker drained (bridgeApplyWanted).
void gxSetGpProgress(u64 (*writePos)(), u64 (*readPos)()) {
    s_gpWritePos = writePos;
    s_gpReadPos = readPos;
}
void gxSetCpLag(bool on) {
    cpLagClear();
    s_cpLag = on;
}
bool gxCpLag() { return s_cpLag; }
void gxGpParse(const u8* p, u32 n) {
    if (!p) {
        s_cmd.clear();
        return;
    }
    s_gpParsing = true;
    parseChunk(p, n);  // whole commands only (the walker), so s_cmd stays empty
    s_gpParsing = false;
}
// RL-NX (9.57): a display list the walker copied (execute's CALL_DL case, on the copy)
void gxGpParseDl(const u8* p, u32 n) {
    s_gpParsing = true;
    s_stats.dl++;
    size_t pos = 0;
    while (pos < n) {
        const size_t len = cmdLength(p + pos, n - pos);
        if (!len || pos + len > n) break;
        execute(p + pos, len, 1);
        pos += len;
    }
    s_gpParsing = false;
}
void gxGpPoll() {
    const u32 t = s_gpTokSeq.load(std::memory_order_acquire);
    const u32 ti = s_gpTokIrqSeq.load(std::memory_order_acquire);
    const u32 f = s_gpFinSeq.load(std::memory_order_acquire);
    if (t != s_gpTokSeen || ti != s_gpTokIrqSeen) {
        const bool irq = ti != s_gpTokIrqSeen;
        s_gpTokSeen = t;
        s_gpTokIrqSeen = ti;
        peToken(u16(s_gpTokVal.load(std::memory_order_relaxed)), irq);  // the latest value, as the register holds
    }
    if (f != s_gpFinSeen) {
        s_gpFinSeen = f;
        peFinish();
    }
}
void gxGpKick() {
    if (s_gpAsync && s_gpKickHook) s_gpKickHook();
}
void gxInit() {
    s_log = envFlag("R2_GX_LOG");
    s_callerOn = envFlag("R2_DRAW_CALLER");  // RL-NX: the guest functions that submit draws
    if (const char* v = getenv("R2_FAST_PIPE")) s_fastPipe = *v != '0';  // RL-NX (Stage 4): menu.cfg / boot.cfg
    // RL-NX (Stage 6): menu.cfg / boot.cfg. RL-NX (Stage 11): this runs after the bridge logged the GP raw state at its
    // start (hwInit), so a saved GP RAW = 0 used to switch the walker back on silently: say so here.
    if (const char* v = getenv("R2_GP_RAW")) {
        s_gpRaw = *v != '0';
        logf("gfx", "GP raw split: %s from R2_GP_RAW (menu.cfg / boot.cfg)", s_gpRaw ? "on" : "off, the game thread walks");
    }
    if (const char* v = getenv("R2_CP_LAG")) s_cpLag = *v != '0';  // RL-NX (Stage 11)
    logf("gfx", "CP lag: %s (R2_CP_LAG / - menu CP LAG; only with GP RAW on)", s_cpLag ? "on, CP reads report the worker's progress" : "off, CP reads drain the worker");
    if (const char* v = getenv("R2_FIFO_LOG")) s_cmdLog = atoi(v);
    gxGatherRefresh();  // RL-NX (9.53): R2_DRAW_CALLER needs every pipe write, so it keeps the slow path
}

// --- gather pipe ------------------------------------------------------------------------
// The CPU's write-gather buffer: memory sees 32-byte bursts (the SDK pads with NOPs to flush).
namespace {
u8 (&s_gp)[64] = s_gpBuf;
// RL-NX (9.53): the fill level is ctx.h's g_gpCur (the inline gather pipe bumps it), not a length; these keep
// the old name for the code below and the save state.
inline u32 gpLen() { return u32(g_gpCur - s_gp); }
inline void gpSetLen(u32 n) { g_gpCur = s_gp + n; }

void burst() {
    const u32 wp = s_piWp & 0x03FFFFE0;
    u8* dst = physPtr(wp, 32);
    if (!dst || !s_piEnd) {
        static int n = 0;
        if (n++ < 8) logf("gx", "gather pipe burst lost: no CPU FIFO (base %08X end %08X wp %08X)", s_piBase, s_piEnd, s_piWp);
        return;
    }
    memcpy(dst, s_gp, 32);
    u32 nwp = wp + 32;
    u32 wrap = s_piWp & 0x20000000u;
    if (nwp >= s_piEnd + 32) {  // past the end of the buffer: wrap
        nwp = s_piBase;
        wrap ^= 0x20000000u;
    }
    s_piWp = nwp | wrap;
    if (s_cpCr & CR_LINK) {  // linked: each burst also advances the GP's write pointer
        s_cpWp += 32;
        if (s_cpWp >= s_cpEnd + 32) s_cpWp = s_cpBase;
        cpRun();
        cpUpdateIrq();  // watermarks (cpRun returns early while reading is off)
    }
}
}  // namespace

void gxFifoWrite(u32 v, int size) {
    // RL-NX: oracle test (PERF_PLAN 0b-ii) - a probe runs a function twice; letting either pass push bytes
    // into the gather pipe would submit its draws for real. Both engines see the same silent pipe instead.
    if (g_oracleIo) return;
    if (s_callerOn && size == 1 && ((v & 0xF8) == 0x80 || v == 0x40)) {  // RL-NX: R2_DRAW_CALLER
        // curFn was 0 for every draw in the first run (the GX writes happen on a fiber whose context has
        // no function recorded), so fall back to the return address and print the ring once to see which
        // it is.
        const rt::Ctx& c = rt::curCtx();
        s_callerCount[c.curFn ? c.curFn : c.lr]++;
        static bool once = false;
        if (!once) {
            once = true;
            logf("call", "draw command %02X on fiber: curFn %08X lr %08X, ring %s, %s", u8(v), c.curFn, c.lr,
                 traceRing(6).c_str(), guestBacktrace().c_str());
        }
    }
    u8* d = g_gpCur;  // big-endian into the pipe (s_gp has room past 32 bytes)
    if (size == 4) {
        const u32 b = __builtin_bswap32(v);
        memcpy(d, &b, 4);
    } else if (size == 2) {
        const u16 b = __builtin_bswap16(u16(v));
        memcpy(d, &b, 2);
    } else {
        *d = u8(v);
    }
    g_gpCur = d + size;
    if (gpLen() >= 32) gxGatherBurst();
}

// RL-NX (9.53): one full 32-byte burst (from gxFifoWrite or ctx.h's inline gpPut). s_stats.bytes is counted
// here, per burst, since the inline path no longer passes through gxFifoWrite.
void gxGatherBurst() {
    s_stats.bytes += 32;
    burst();
    const u32 rest = gpLen() - 32;
    if (rest) memcpy(s_gp, s_gp + 32, rest);
    gpSetLen(rest);
}

void gxGatherRefresh() {
    g_gpLimit = (g_oracleIo || s_callerOn) ? nullptr : s_gp + 32;
    g_gpLimitP = s_fastPipe ? g_gpLimit : nullptr;  // RL-NX (Stage 4)
}

// RL-NX (Stage 4): the - menu's FAST PIPE row (menu_switch.cpp), on the game thread like every pipe write
void gxSetFastPipe(bool on) {
    s_fastPipe = on;
    gxGatherRefresh();
}
bool gxFastPipe() { return s_fastPipe; }

void gxFlushGatherPipe() {
    if (gpLen()) {
        memset(s_gp + gpLen(), 0, 32 - gpLen());  // NOPs
        gpSetLen(0);
        s_stats.bytes += 32;  // RL-NX (9.53): counted per burst now
        burst();
    }
    gxGpKick();  // RL-NX (9.57): PPCSync follows GXFlush / GXSetDrawDone: the game is about to wait for the GP
}

// RL-NX: R2_DRAW_CALLER - the guest functions that wrote draw commands since the last call, most first
// (the window is whatever the caller chooses, e.g. 30 frames).
void gxDrawCallerLog(u64 frame) {
    if (!s_callerOn || s_callerCount.empty()) return;
    std::vector<std::pair<u32, u32>> v(s_callerCount.begin(), s_callerCount.end());
    std::sort(v.begin(), v.end(), [](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) {
        return a.second > b.second;
    });
    std::string line;
    for (size_t i = 0; i < v.size() && i < 5; i++) {
        char b[160];
        snprintf(b, sizeof b, "%s%s x%u", i ? ", " : "", describeAddr(v[i].first).c_str(), v[i].second);
        line += b;
    }
    logf("call", "draw commands up to frame %llu written by: %s", (unsigned long long)frame, line.c_str());
    s_callerCount.clear();
}

void gxResetGatherPipe() {
    static int n = 0;
    if (gpLen() && n++ < 16) {
        char b[100] = {};
        for (u32 i = 0; i < gpLen() && i < 32; i++) snprintf(b + 3 * i, 4, "%02X ", s_gp[i]);
        logf("gx", "gather pipe reset with %u bytes pending: %s", gpLen(), b);
    }
    gpSetLen(0);
}

// --- PI FIFO registers (0xCC00300C..0x14) ---------------------------------------------------
u32 gxPiRead(u32 off) {
    switch (off) {
    case 0x0C: return s_piBase;
    case 0x10: return s_piEnd;
    case 0x14: return s_piWp;
    default: return 0;
    }
}
bool fifoLog() {
    static const bool on = envFlag("R2_FIFO_LOG");
    return on;
}

void gxPiWrite(u32 off, u32 v) {
    if (fifoLog()) logf("fifo", "PI %02X = %08X (gp pending %u)", off, v, gpLen());
    switch (off) {
    case 0x0C: s_piBase = v & 0x03FFFFE0; break;
    // Battle of Hoth records display lists with base 0, end 0x04000000 (past all memory: no wrap)
    // and the write pointer at the list; masked to 26 bits that end read as 0, "no FIFO", and
    // every burst of the list was dropped. (Dolphin keeps all bits of these registers.)
    case 0x10: s_piEnd = v & 0x07FFFFE0; break;
    case 0x14: s_piWp = v & 0x23FFFFE0; break;
    case 0x18:  // FIFO reset: the write pointer goes back to the base
        if (v & 1) s_piWp = s_piBase;
        break;
    }
}

// --- CP registers (0xCC000000) ------------------------------------------------------------
u16 gxCpRead(u32 off) {
    off &= 0x7E;
    // RL-NX (9.57): every answer below comes from the game thread's own pointers; the sync only kept the renderer
    // from reading memory late. With the GP thread on, that is left to the PE events (gpToken / gpFinish fence),
    // and a read only makes the queued bytes visible, in case the game spins here until something happens.
    if (s_gpAsync) {
        gxGpKick();
        // RL-NX (Stage 6): raw mode - the read pointer below is past lists only the worker has read; let it run them
        // before the game can conclude it may rewrite them (~6 CP reads a second in level, 13:54 [pm-mem] cp_rd)
        if (cpLagActive()) {
            gxGpPoll();  // RL-NX (Stage 11): CP LAG - no wait, the pointers below are the worker's progress
        } else if (s_gpRaw && s_gpDrainBridge) {
            s_gpDrainBridge();
            gxGpPoll();
        }
    } else if (s_syncHook) s_syncHook();
    const u32 rpSeen = cpLagActive() ? cpLagRp() : s_cpRp;  // RL-NX (Stage 11): the read pointer the game sees
    switch (off) {
    case 0x00: {  // status: overflow, underflow, read idle, command idle, breakpoint
        // A GP halted at a breakpoint with commands still behind it is not idle: before moving
        // the GP FIFO the game spins on GXGetGPStatus until read+command idle, while its
        // interrupt callback releases the queued frames one breakpoint at a time. Reporting
        // idle at the breakpoint made it switch FIFOs with two frames (and their draw-dones)
        // still unread, so its frames-in-flight counter never drained.
        const bool readIdle = rpSeen == s_cpWp;  // RL-NX (Stage 11): rpSeen
        const bool cmdIdle = readIdle || !(s_cpCr & CR_READ);
        return u16((cpOverflow() ? 1 : 0) | (cpUnderflow() ? 2 : 0) | (readIdle ? 4 : 0) | (cmdIdle ? 8 : 0) |
                   (s_bpHit ? 0x10 : 0));
    }
    case 0x02: return s_cpCr;
    case 0x30: case 0x32: {  // read-write distance: what is still unread (behind a breakpoint)
        const u32 size = s_cpEnd + 32 - s_cpBase;
        const u32 d = s_cpWp >= rpSeen ? s_cpWp - rpSeen : s_cpWp + size - rpSeen;  // RL-NX (Stage 11): rpSeen
        return off == 0x30 ? u16(d) : u16(d >> 16);
    }
    case 0x34: return u16(s_cpWp);
    case 0x36: return u16(s_cpWp >> 16);
    case 0x38: return u16(rpSeen);  // RL-NX (Stage 11): rpSeen
    case 0x3A: return u16(rpSeen >> 16);
    default: return s_cpReg[off / 2];
    }
}
void gxCpWrite(u32 off, u16 v) {
    off &= 0x7E;
    if (fifoLog() && off >= 0x02) {
        logf("fifo", "CP %02X = %04X (rp %08X wp %08X pi wp %08X)", off, v, s_cpRp, s_cpWp, s_piWp);
        if (s_cmdLog >= 3 && (off == 0x02 || off == 0x20 || off == 0x3C)) logf("fifo", "  from %s", traceRing(10).c_str());
    }
    s_cpReg[off / 2] = v;
    switch (off) {
    case 0x02: {
        const u16 old = s_cpCr;
        s_cpCr = v;
        if (s_log && (old ^ v)) logf("gx", "CP_CR %04X (read %d, link %d)", v, v & 1, (v >> 4) & 1);
        if (!(v & CR_BP)) s_bpHit = false;  // disabling the breakpoint releases the GP
        cpUpdateIrq();
        cpRun();
        break;
    }
    case 0x04:  // CP_CLEAR: the watermark flags are live; re-evaluate
        cpUpdateIrq();
        break;
    case 0x20: case 0x22: s_cpBase = regPair(0x20, 0x22) & 0x03FFFFE0; if (off == 0x20) s_gpFifoSets++; break;
    case 0x24: case 0x26: s_cpEnd = regPair(0x24, 0x26) & 0x03FFFFE0; break;
    case 0x28: case 0x2A: s_cpHi = regPair(0x28, 0x2A); break;
    case 0x2C: case 0x2E: s_cpLo = regPair(0x2C, 0x2E); break;
    case 0x30: case 0x32: break;  // read-write distance: derived from the pointers
    case 0x34: case 0x36:
        s_cpWp = regPair(0x34, 0x36) & 0x03FFFFE0;
        if (off == 0x36) cpRun();
        break;
    case 0x38: case 0x3A:
        s_cpRp = regPair(0x38, 0x3A) & 0x03FFFFE0;
        cpLagClear();  // RL-NX (Stage 11): a new read pointer; earlier chunks no longer say where the GP is
        if (s_gpAsync && s_gpRaw) s_chunkSink(nullptr, 0);  // RL-NX (Stage 6): the worker drops its partial command
        else if (s_gpAsync) s_walkCmd.clear();  // RL-NX (9.57): the walker holds the partial command
        else s_cmd.clear();
        break;
    case 0x3C: case 0x3E:
        s_cpBp = regPair(0x3C, 0x3E) & 0x03FFFFE0;
        if (s_bpHit && s_cpRp != s_cpBp) {  // moved: the GP may go on
            s_bpHit = false;
            cpUpdateIrq();
        }
        if (off == 0x3E) cpRun();
        break;
    }
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "../savestate.h"
namespace rt {
void gxFifoSaveState(state::Writer& w) {
    // RL-NX (9.57): s_cmd and the VCD/VAT state are the GP thread's while it runs
    if (s_gpAsync && s_gpDrainAll) s_gpDrainAll();
    gxGpPoll();
    const bool walk = s_gpAsync && !s_gpRaw;  // the walker holds the partial command: save it where the old layout has it (RL-NX Stage 6: not in raw mode)
    if (walk) s_cmd.swap(s_walkCmd);
    w.section("GXFF");
    w.pod(s_piBase);
    w.pod(s_piEnd);
    w.pod(s_piWp);
    w.pod(s_cpReg);
    w.pod(s_cpBase);
    w.pod(s_cpEnd);
    w.pod(s_cpWp);
    w.pod(s_cpRp);
    w.pod(s_cpHi);
    w.pod(s_cpLo);
    w.pod(s_cpBp);
    w.pod(s_cpCr);
    w.vec(s_cmd);
    w.pod(s_vtx.vcdLo);  // RL-NX (9.57): the vertex state is a struct now (same bytes)
    w.pod(s_vtx.vcdHi);
    w.pod(s_vtx.vatA);
    w.pod(s_vtx.vatB);
    w.pod(s_vtx.vatC);
    w.pod(s_bpHit);
    w.pod(s_gp);
    {
        const u32 len = gpLen();  // RL-NX (9.53): the fill level is a pointer now
        w.pod(len);
    }
    w.pod(s_gpFifoSets);
    if (walk) s_cmd.swap(s_walkCmd);  // RL-NX (9.57)
}

void gxFifoLoadState(state::Reader& r) {
    if (s_gpAsync && s_gpDrainAll) s_gpDrainAll();  // RL-NX (9.57): as gxFifoSaveState
    gxGpPoll();
    r.section("GXFF");
    r.pod(s_piBase);
    r.pod(s_piEnd);
    r.pod(s_piWp);
    r.pod(s_cpReg);
    r.pod(s_cpBase);
    r.pod(s_cpEnd);
    r.pod(s_cpWp);
    r.pod(s_cpRp);
    r.pod(s_cpHi);
    r.pod(s_cpLo);
    r.pod(s_cpBp);
    r.pod(s_cpCr);
    r.vec(s_cmd);
    r.pod(s_vtx.vcdLo);  // RL-NX (9.57): the vertex state is a struct now (same bytes)
    r.pod(s_vtx.vcdHi);
    r.pod(s_vtx.vatA);
    r.pod(s_vtx.vatB);
    r.pod(s_vtx.vatC);
    s_vtx.vsOk = 0;  // RL-NX (Stage 5): the cached vertex sizes belong to the old registers
    r.pod(s_bpHit);
    r.pod(s_gp);
    {
        u32 len = 0;  // RL-NX (9.53)
        r.pod(len);
        gpSetLen(len < 32 ? len : 0);
    }
    r.pod(s_gpFifoSets);
    if (s_gpAsync && !s_gpRaw) {  // RL-NX (9.57): the walker splits from here on (RL-NX Stage 6: the worker does in raw mode)
        s_walkCmd.swap(s_cmd);
        s_cmd.clear();
        s_vtxWalk = s_vtx;
    }
    cpUpdateIrq();
}
}  // namespace rt
