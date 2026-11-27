// Save states (savestate.h): slots on disk, taken and restored at a safepoint.
//
// A request (hotkey, menu) waits for the guest's next safepoint (state::service). Saving hands
// control to the root (the main thread's own stack, threads.cpp), which copies everything while
// no fiber runs; the file is compressed and written on a worker thread meanwhile. Loading reads
// and checks the file first, lets Aurora finish reading guest memory, and has the root put
// everything back; the code then continues inside the service call that saved the state, as
// it was then (so nothing in service may hold a heap object across the switch).
//
// File: a header (the build and host layout it needs, when it was saved) and the zstd-
// compressed snapshot stream.
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <xxhash.h>
#include <zstd.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#include "runtime.h"
#include "achievements.h"
#include "mods.h"
#include "savestate.h"
#include "settings.h"

namespace rt::state {

volatile bool g_pending = false;

namespace {

constexpr u32 kVersion = 4;  // 2: the port arena (memory.cpp); 3: RetroAchievements progress;
                             // 4: MEM1's heap extension (memory.cpp, hle_game.cpp)
struct Header {
    char magic[4];   // "R2ST"
    u32 version;
    char game[8];    // "GSWE64"
    u64 exeBase, exeHash, guestBase, stacksBase;
    u64 savedTb;     // host time base when saved (timers move by the difference)
    s64 savedUnix;   // wall clock (the menu shows it)
    u64 payloadSize, payloadHash, compressedSize;
};

Op s_op = Op::None;
int s_slot = 1;
// What the root just did. Globals survive a load (the stacks do not): after one, the code goes
// on in the service call that saved the state, and learns here what happened.
enum class Done { None, Saved, Loaded } s_done = Done::None;
int s_doneSlot = 0;
Writer s_writer;                // the snapshot the root writes
std::vector<u8> s_loadPayload;  // the snapshot the root reads
u64 s_loadSavedTb = 0;
std::string s_error;            // why a load did not happen
std::thread s_writeThread;
u64 s_savedTb = 0;

bool automated() {
    static const bool a = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    return a;
}

std::string stateDir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = getenv("R2_STATE_DIR"); e && *e) d = e;
        else if (automated()) d = "build/test_cache/states";
        else if (const std::string u = mods::userDir(); !u.empty()) {
            d = u + "states";
        } else {
            d = "states";
        }
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

void finishWrite() {
    if (s_writeThread.joinable()) s_writeThread.join();
}

// The root: the snapshot, in this order (loadState below reads it back the same way).
void rootSave() {
    Writer& w = s_writer;
    w.buf.clear();
    w.buf.reserve(48u << 20);
    s_savedTb = tbNow();
    memSaveState(w);
    threadsSaveState(w);
    cpuSaveState(w);
    eventsSaveState(w);
    hwSaveState(w);
    gxFifoSaveState(w);
    musyxSaveState(w);
    hleSaveState(w);
    gfx::bridgeSaveState(w);
    ra::saveState(w);
    w.section("END ");
    s_done = Done::Saved;
}

void rootLoad() {
    Reader r;
    r.p = s_loadPayload.data();
    r.end = r.p + s_loadPayload.size();
    r.tbDelta = s64(tbNow() - s_loadSavedTb);
    memLoadState(r);
    threadsLoadState(r);
    cpuLoadState(r);
    eventsLoadState(r);
    hwLoadState(r);
    gxFifoLoadState(r);
    musyxLoadState(r);
    hleLoadState(r);
    gfx::bridgeLoadState(r);
    ra::loadState(r);
    r.section("END ");
    // (the file's checksum matched: a mismatch here is a bug, and the machine is half restored)
    if (!r.ok) fatal("save state %d did not fit this build's layout", s_doneSlot);
    s_done = Done::Loaded;
}

// compress and write on a worker thread (the game goes on)
void startWrite(int slot) {
    std::vector<u8> data;
    data.swap(s_writer.buf);
    const HostLayout lay = hostLayout();
    Header h{};
    memcpy(h.magic, "R2ST", 4);
    h.version = kVersion;
    memcpy(h.game, "GSWE64", 6);
    h.exeBase = lay.exeBase;
    h.exeHash = lay.exeHash;
    h.guestBase = lay.guestBase;
    h.stacksBase = lay.stacksBase;
    h.savedTb = s_savedTb;
    h.savedUnix = s64(time(nullptr));
    const std::string path = slotPath(slot);
    s_writeThread = std::thread([h, data = std::move(data), path, slot]() mutable {
        const u64 t0 = tbNow();
        h.payloadSize = data.size();
        h.payloadHash = XXH3_64bits(data.data(), data.size());
        std::vector<u8> z(ZSTD_compressBound(data.size()));
        const size_t n = ZSTD_compress(z.data(), z.size(), data.data(), data.size(), 1);
        if (ZSTD_isError(n)) {
            logf("state", "slot %d: compression failed (%s)", slot, ZSTD_getErrorName(n));
            return;
        }
        h.compressedSize = n;
        const std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(z.data(), 1, n, f) == n;
        if (f) ok = fclose(f) == 0 && ok;
        std::error_code ec;
        if (ok) std::filesystem::rename(tmp, path, ec);
        if (!ok || ec) {
            logf("state", "slot %d: cannot write %s", slot, path.c_str());
            return;
        }
        logf("state", "slot %d written: %s (%.1f MiB, %.0f ms)", slot, path.c_str(), double(n) / (1 << 20),
             double(tbNow() - t0) * 1000.0 / double(kTbClock));
    });
}

// read and check a slot's file into s_loadPayload (false: s_error says why)
bool readState(int slot) {
    finishWrite();  // (a save to this slot may still be on its way)
    const std::string path = slotPath(slot);
    char buf[96];
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        snprintf(buf, sizeof buf, "NO STATE IN SLOT %d", slot);
        s_error = buf;
        return false;
    }
    Header h{};
    std::vector<u8> z;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "R2ST", 4) && h.compressedSize < (1ull << 31);
    if (ok) {
        z.resize(size_t(h.compressedSize));
        ok = fread(z.data(), 1, z.size(), f) == z.size();
    }
    fclose(f);
    const HostLayout lay = hostLayout();
    if (!ok || h.version != kVersion || memcmp(h.game, "GSWE64", 6) != 0) {
        snprintf(buf, sizeof buf, "STATE %d CANNOT BE READ", slot);
        s_error = buf;
        return false;
    }
    if (h.exeHash != lay.exeHash || h.exeBase != lay.exeBase) {
        snprintf(buf, sizeof buf, "STATE %d IS FROM ANOTHER VERSION OF RED TWO", slot);
        s_error = buf;
        return false;
    }
    if (h.guestBase != lay.guestBase || h.stacksBase != lay.stacksBase) {
        snprintf(buf, sizeof buf, "STATE %d NEEDS A RESTART (MEMORY LAYOUT)", slot);
        s_error = buf;
        return false;
    }
    s_loadPayload.resize(size_t(h.payloadSize));
    const size_t n = ZSTD_decompress(s_loadPayload.data(), s_loadPayload.size(), z.data(), z.size());
    if (ZSTD_isError(n) || n != h.payloadSize || XXH3_64bits(s_loadPayload.data(), n) != h.payloadHash) {
        s_loadPayload.clear();
        snprintf(buf, sizeof buf, "STATE %d IS DAMAGED", slot);
        s_error = buf;
        return false;
    }
    s_loadSavedTb = h.savedTb;
    return true;
}

void message(const char* fmt, int slot) {
    char buf[96];
    snprintf(buf, sizeof buf, fmt, slot);
    gfx::toast(buf);
}

}  // namespace

void finishWrites() { finishWrite(); }

std::string slotPath(int slot) {
    char name[32];
    snprintf(name, sizeof name, "/GSWE64_slot%d.r2s", slot);
    return stateDir() + name;
}

std::string slotInfo(int slot) {
    struct Cache {
        std::filesystem::file_time_type mtime{};
        bool exists = false;
        std::string text;
    };
    static Cache cache[kSlots + 1];
    if (slot < 1 || slot > kSlots) return {};
    std::error_code ec;
    const std::string path = slotPath(slot);
    const bool exists = std::filesystem::exists(path, ec);
    const auto mtime = exists ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
    Cache& c = cache[slot];
    if (c.exists == exists && c.mtime == mtime && (!exists || !c.text.empty())) return c.text;
    c.exists = exists;
    c.mtime = mtime;
    c.text.clear();
    if (!exists) return {};
    if (FILE* f = fopen(path.c_str(), "rb")) {
        Header h{};
        if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "R2ST", 4)) {
            const time_t t = time_t(h.savedUnix);
            struct tm lt{};
#ifdef _WIN32
            localtime_s(&lt, &t);
#else
            localtime_r(&t, &lt);
#endif
            char buf[32];
            strftime(buf, sizeof buf, "%m-%d %H:%M", &lt);
            c.text = buf;
            const HostLayout lay = hostLayout();
            if (h.version != kVersion || h.exeHash != lay.exeHash) c.text += " (OLD)";
        }
        fclose(f);
    }
    return c.text;
}

void request(Op op, int slot) {
    if (slot < 1 || slot > kSlots) return;
    s_op = op;
    s_slot = slot;
    g_pending = true;
    g_irqPending = 1;
}

// At a safepoint, on the guest's fiber. No heap objects may live here across the root switch:
// after a load the code continues in the call that saved, with the stack as it was then.
void service(Ctx&) {
    g_pending = false;
    const Op op = s_op;
    s_op = Op::None;
    s_doneSlot = s_slot;
    s_done = Done::None;
    if (op == Op::Save) {
        if (cinematicSkip()) {
            gfx::toast("NO SAVE STATE WHILE SKIPPING");
            return;
        }
        finishWrite();
        threadsYieldToRoot(rootSave);
    } else if (op == Op::Load) {
        if (!readState(s_doneSlot)) {
            logf("state", "%s", s_error.c_str());
            gfx::toast(s_error);
            return;
        }
        gfx::gpuDrain();  // Aurora reads guest memory (arrays, textures) until it has caught up
        threadsYieldToRoot(rootLoad);
    } else {
        return;
    }
    // here after the root's work (after a load: the save's own call, on the state's stack)
    if (s_done == Done::Saved) {
        startWrite(s_doneSlot);
        logf("state", "slot %d saved", s_doneSlot);
        message("STATE %d SAVED", s_doneSlot);
    } else if (s_done == Done::Loaded) {
        s_loadPayload.clear();
        s_loadPayload.shrink_to_fit();
        gfx::bridgeAfterLoad();
        audio::flush();
        hleAfterLoad();
        ra::afterLoad();
        g_irqPending = 1;
        logf("state", "slot %d loaded", s_doneSlot);
        message("STATE %d LOADED", s_doneSlot);
    }
    s_done = Done::None;
}

// R2_STATE_SCRIPT="<seconds>:save|load:<slot>,...": saves and loads at those times (tests)
void scriptTick() {
    struct Step {
        double at;
        Op op;
        int slot;
    };
    static std::vector<Step> steps = [] {
        std::vector<Step> v;
        if (const char* s = getenv("R2_STATE_SCRIPT")) {
            while (*s) {
                char* end;
                const double at = strtod(s, &end);
                if (*end != ':') break;
                s = end + 1;
                const Op op = !strncmp(s, "save", 4) ? Op::Save : !strncmp(s, "load", 4) ? Op::Load : Op::None;
                while (*s && *s != ':' && *s != ',') s++;
                const int slot = *s == ':' ? int(strtol(s + 1, &end, 10)) : 1;
                if (*s == ':') s = end;
                if (op != Op::None) v.push_back({at, op, slot});
                while (*s == ',') s++;
            }
        }
        return v;
    }();
    if (steps.empty()) return;
    static const u64 start = tbNow();
    const double t = double(tbNow() - start) / double(kTbClock);
    if (t < steps.front().at) return;
    logf("state", "script: %s slot %d", steps.front().op == Op::Save ? "save" : "load", steps.front().slot);
    request(steps.front().op, steps.front().slot);
    steps.erase(steps.begin());
}

}  // namespace rt::state
