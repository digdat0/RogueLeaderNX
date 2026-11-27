// Save states: the whole emulated machine in a file, restored later (also in a later session).
//
// A snapshot is taken at a safepoint (an interrupt check in recompiled code): the guest's
// threads are host fibers whose stacks hold recompiled frames, so the stacks themselves are
// part of the state (threads.cpp), which is why they, guest memory and the exe sit at fixed
// host addresses (memory.cpp). Every device model writes its registers and timers into the
// stream, in a fixed order; the renderer's register state is replayed into Aurora after a load
// (gfx/gx_bridge.cpp). The memory card is not part of a state: its file keeps the game's saves.
#pragma once
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "ctx.h"

namespace rt::state {

constexpr u32 tag(const char (&s)[5]) { return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24; }

struct Writer {
    std::vector<u8> buf;
    void bytes(const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    template <typename T>
    void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof v);
    }
    template <typename T>
    void vec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        pod(u64(v.size()));
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
    void section(const char (&s)[5]) { pod(tag(s)); }
};

struct Reader {
    const u8* p = nullptr;
    const u8* end = nullptr;
    bool ok = true;
    s64 tbDelta = 0;  // host time base now minus at the save: saved timestamps move by it
    void bytes(void* dst, size_t n) {
        if (!ok || size_t(end - p) < n) {
            ok = false;
            memset(dst, 0, n);
            return;
        }
        memcpy(dst, p, n);
        p += n;
    }
    template <typename T>
    void pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof v);
    }
    template <typename T>
    T get() {
        T v{};
        pod(v);
        return v;
    }
    template <typename T>
    void vec(std::vector<T>& v) {
        const u64 n = get<u64>();
        if (!ok || n > u64(end - p) / sizeof(T)) {
            ok = false;
            return;
        }
        v.resize(size_t(n));
        if (n) bytes(v.data(), size_t(n) * sizeof(T));
    }
    bool section(const char (&s)[5]) {
        if (get<u32>() != tag(s)) ok = false;
        return ok;
    }
    // a host time base value from the state, moved to now (0 stays: "not scheduled")
    u64 time(u64 tb) const { return tb ? u64(s64(tb) + tbDelta) : 0; }
};

// Requests (hotkeys, the menu, R2_STATE_SCRIPT): carried out at the guest's next safepoint.
enum class Op { None, Save, Load };
void request(Op op, int slot);
extern volatile bool g_pending;  // a request waits (checked by safepoint)
void service(Ctx& c);            // safepoint: carry out the request

constexpr int kSlots = 8;
std::string slotPath(int slot);
// "11-09 14:32" when the slot holds a state, "" when empty
std::string slotInfo(int slot);
void scriptTick();  // R2_STATE_SCRIPT="<seconds>:save|load:<slot>,...": automated tests (once per retrace)
void finishWrites();  // wait for a state still being written (before quitting)

}  // namespace rt::state

namespace rt {
// Each subsystem's part of a snapshot (in this order; loads run on the main thread's own stack).
void memSaveState(state::Writer& w);
void memLoadState(state::Reader& r);
void threadsSaveState(state::Writer& w);
void threadsLoadState(state::Reader& r);
void cpuSaveState(state::Writer& w);  // SPRs, time base offset, VM segment registers
void cpuLoadState(state::Reader& r);
void eventsSaveState(state::Writer& w);
void eventsLoadState(state::Reader& r);
void hwSaveState(state::Writer& w);  // devices and ARAM
void hwLoadState(state::Reader& r);
void gxFifoSaveState(state::Writer& w);
void gxFifoLoadState(state::Reader& r);
void musyxSaveState(state::Writer& w);
void musyxLoadState(state::Reader& r);
void hleSaveState(state::Writer& w);  // hle_game.cpp (cheats' records, skip)
void hleLoadState(state::Reader& r);
void hleAfterLoad();                  // caches of guest objects start over
namespace gfx {
void bridgeSaveState(state::Writer& w);
void bridgeLoadState(state::Reader& r);
void bridgeAfterLoad();  // on the guest's fiber: the register state goes to Aurora again
void gpuDrain();         // Aurora has read everything sent (before guest memory changes under it)
void toast(const std::string& text);  // a short message on screen (menu.cpp)
}  // namespace gfx
namespace audio {
void flush();  // drop queued output (a state was loaded)
}

// threads.cpp: run fn on the main thread's own stack (not a guest fiber's), then continue.
// After a load the code continues in the fiber and at the place the state was saved.
void threadsYieldToRoot(void (*fn)());
[[noreturn]] void threadsRun(u32 entry);  // main: start the game's boot thread
// memory.cpp: host addresses the state depends on (identical in every session when fixed)
struct HostLayout {
    u64 exeBase, exeHash, guestBase, stacksBase;
};
HostLayout hostLayout();
u8* fiberStackRegion();
constexpr u32 kMaxFibers = 16;
#if defined(__SWITCH__)
// RL-NX: 1 MiB per fiber. A GameCube guest thread's own stack is 8-64 KiB and the host frames that
// mirror them are a few times larger; 16 x 16 MiB would not fit beside the 256 MiB guest arena.
constexpr u64 kFiberStackSize = 1ull << 20;
#else
constexpr u64 kFiberStackSize = 16ull << 20;  // as the main thread's (/STACK)
#endif
}  // namespace rt
