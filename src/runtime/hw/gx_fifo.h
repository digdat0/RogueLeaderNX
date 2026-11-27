#pragma once
#include "../ctx.h"

namespace rt {

struct PadState {
    bool connected = false;
    u16 buttons = 0x0080;  // PAD_USE_ORIGIN is always set by the controller
    u8 stickX = 0x80, stickY = 0x80, substickX = 0x80, substickY = 0x80, triggerL = 0, triggerR = 0;
};
PadState& padState(int port);

// Gather pipe (0xCC008000) -> GX command stream
void gxFifoWrite(u32 v, int size);
struct GxStats {
    u64 bytes = 0, draws = 0, vertices = 0, bp = 0, xf = 0, cp = 0, dl = 0, unknown = 0;
};
GxStats& gxStats();
// Receives every complete GX command (raw bytes, big-endian), e.g. for a renderer.
using GxSink = void (*)(const u8* cmd, u32 len);
void gxSetSink(GxSink sink);
void gxSetSyncHook(void (*hook)());  // called when the CPU reads the GP's status or pointers
// RL-NX (9.53): PE events and PE register access, for the bridge worker's barriers (gx_bridge.cpp peBarrier):
// 0 token without interrupt, 1 event that raises an interrupt, 2 the CPU touches PE registers.
void gxSetPeBarrierHook(void (*hook)(int kind));
void gxPeBarrier(int kind);

// FIFO registers: CP (0xCC000000) and the PI's CPU FIFO (0xCC00300C-0x14)
void gxInit();
u16 gxCpRead(u32 off);
void gxCpWrite(u32 off, u16 v);
u32 gxPiRead(u32 off);
void gxPiWrite(u32 off, u32 v);
void gxResetGatherPipe();  // WPAR write
void gxFlushGatherPipe();  // PPCSync: drain a partly filled gather buffer
void gxGatherRefresh();    // RL-NX (9.53): re-decide whether ctx.h's inline gather pipe may run (oracle, R2_DRAW_CALLER)
void gxSetFastPipe(bool on);  // RL-NX (Stage 4): the - menu's FAST PIPE row (ctx.h wr32P, g_gpLimitP)
bool gxFastPipe();
void gxDrawCallerLog(u64 frame);  // RL-NX: R2_DRAW_CALLER, who wrote the draw commands since the last call

// RL-NX (9.57): the GP thread. With it on, the game thread only moves the CP pointers (the GP is still
// "infinitely fast" as far as the game can see: the read pointer reaches the write pointer or the breakpoint at
// once) and hands the raw bytes it passed to the bridge worker, which splits and runs the commands. The PE
// events the stream raises (draw done, tokens) reach the game asynchronously, as on the console: the worker
// posts them and wakes the guest; its next safepoint (or PE register access) applies them.
using GxChunkSink = void (*)(const u8* p, u32 n);  // whole CP commands in order (p == nullptr: unused)
// sink: queues commands (the game thread walks the stream so each chunk ends on a command); dlSink: queues a copy
// of a display list the stream calls, in place of the call; kick: make everything queued visible to the worker (the game is about to wait);
// drainAll: wait until the worker and the renderer have run everything; fence (worker): the renderer has read
// all guest memory behind the commands so far (before a PE event lets the game reuse it).
void gxSetGpThread(GxChunkSink sink, GxChunkSink dlSink, void (*kick)(), void (*drainAll)(), void (*fence)());
void gxGpSetAsync(bool on);       // game thread, with the worker drained
bool gxGpAsync();
// RL-NX (Stage 6): raw mode - the worker splits the CP bytes itself (no walker on the game thread); CP register reads
// wait for the worker through the drain hook. gxGpSetRaw: game thread, with the worker drained.
void gxSetGpDrain(void (*drain)());
void gxGpSetRaw(bool on);
bool gxGpRaw();
// RL-NX (Stage 11): CP LAG - with GP RAW on, CP register reads report the worker's progress instead of waiting for it
// (gx_fifo.cpp s_cpLag). The bridge supplies its ring positions; gxSetCpLag: game thread, with the worker drained.
void gxSetGpProgress(u64 (*writePos)(), u64 (*readPos)());
void gxSetCpLag(bool on);
bool gxCpLag();
void gxGpParse(const u8* p, u32 n);  // bridge worker: run whole CP commands (nullptr: drop a partial command)
void gxGpParseDl(const u8* p, u32 n);  // bridge worker: run a copied display list
void gxGpPoll();                  // game thread: apply the PE events the worker posted
void gxGpKick();                  // game thread: about to wait for the GP (idle loop, status spin)

// PE events raised by the stream
void peToken(u16 token, bool irq);
void peFinish();

}  // namespace rt
