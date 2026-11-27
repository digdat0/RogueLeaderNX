// Guest time base, decrementer and timed device events.
//
// Device state is only touched on the guest thread. A host timer thread sleeps (on a
// high-resolution waitable timer) until the earliest scheduled event and then raises
// g_irqPending; the guest polls it at loop
// back-edges (safepoint), runs due events there and takes interrupts if enabled.
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <sys/prctl.h>
#include <time.h>
#include <condition_variable>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>  // RL-NX: getenv for R2_DETERMINISTIC (phase 0b)
#include <mutex>
#include <thread>

#include "perfmon.h"  // RL-NX: PERF LOG idle timer
#include "runtime.h"

namespace rt {

void (*g_eventHandlers[EV_COUNT])() = {};
u64 g_timerFlags = 0;  // times the timer thread raised g_irqPending (R2_GX_LOG)
volatile u32 g_irqLine = 0;

namespace {
#ifdef _WIN32
LARGE_INTEGER s_qpcFreq, s_qpcStart;
#else
timespec s_start{};
#endif
u64 s_due[EV_COUNT];  // 0 = not scheduled
std::mutex s_mu;
std::atomic<u64> s_earliest{~0ull};

u64 s_decBaseTb = 0;
u32 s_decValue = 0;
bool s_decPending = false;

void recomputeEarliest() {
    u64 e = ~0ull;
    for (u64 d : s_due)
        if (d && d < e) e = d;
    s_earliest = e;
}

// The timer thread sleeps on a high-resolution waitable timer. Ordinary sleeps (condition
// variables, Sleep) follow the system tick, and Windows 11 ignores timeBeginPeriod(1) for
// processes without a visible window (headless runs, windows behind others): retraces then
// fired a 15.6 ms tick late while the game idled waiting for them (the intro ran at ~1/3 speed).
#ifdef _WIN32
HANDLE s_timer = nullptr;   // high-resolution waitable timer
HANDLE s_wakeEv = nullptr;  // schedule() changed the earliest event
HANDLE s_guestEv = nullptr; // g_irqPending was raised (wakes an idle guest)

void wakeTimer() {
    if (s_wakeEv) SetEvent(s_wakeEv);
}
void signalGuest() { SetEvent(s_guestEv); }
void waitGuest(u32 ms) { WaitForSingleObject(s_guestEv, ms); }
void cpuRelax() { YieldProcessor(); }

void sleepUs(u64 us, bool wakeable) {
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(us * 10);  // relative, 100 ns units
    SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE);
    HANDLE hs[2] = {s_timer, s_wakeEv};
    WaitForMultipleObjects(wakeable ? 2 : 1, hs, FALSE, INFINITE);
}
#else
// Linux: auto-reset events from a mutex and a condition variable; the timer thread asks for a
// 1 us timer slack (the default 50 us would make every retrace late by that much).
struct Event {
    std::mutex m;
    std::condition_variable cv;
    bool set = false;
    void signal() {
        {
            std::lock_guard lk(m);
            set = true;
        }
        cv.notify_one();
    }
    void wait(u64 us) {
        std::unique_lock lk(m);
        cv.wait_for(lk, std::chrono::microseconds(us), [this] { return set; });
        set = false;
    }
};
Event s_wakeEv, s_guestEv;
void wakeTimer() { s_wakeEv.signal(); }
void signalGuest() { s_guestEv.signal(); }
void waitGuest(u32 ms) { s_guestEv.wait(u64(ms) * 1000); }
#if defined(__aarch64__)
void cpuRelax() { asm volatile("yield"); }
#else
void cpuRelax() { __builtin_ia32_pause(); }
#endif

void sleepUs(u64 us, bool wakeable) {
    if (wakeable) {
        s_wakeEv.wait(us);
        return;
    }
    timespec ts{time_t(us / 1000000), long(us % 1000000) * 1000};
    while (nanosleep(&ts, &ts) != 0) {
    }
}
#endif

void raisePending() {
    g_irqPending = 1;
    signalGuest();
}

// RL-NX (PERF_PLAN phase 0b): R2_DETERMINISTIC=1 - the guest's own clock.
//
// Two runs of one build at the same frame index reach different game states because the guest's timeline
// is the wall clock: interrupts land where the host scheduler puts them, the decrementer and every timing
// loop move with it, and a state hash taken at "frame 300" therefore reports a difference between a build
// and itself. With this on, tbNow() - which is the timebase register, the decrementer, the event queue and
// the audio pacing, all through one function - becomes a virtual counter that advances by exactly one
// frame period per presented frame and stands still inside one. The guest then sees the same 60 Hz
// timeline on every run, which is what makes the state hash comparable and a performance A/B fair.
//
// It is a boot.cfg switch, not a build switch, so the same NRO can be measured either way.
namespace {
bool s_det = false;      // RL-NX: R2_DETERMINISTIC - the guest's clock is virtual (detInit)
u64 s_detTb = 0;
u64 s_detPeriod = 0;
}  // namespace

void timerThread() {
    for (;;) {
        const u64 e = s_earliest.load();
        const u64 now = tbNow();
        if (e <= now) {
            raisePending();
            g_timerFlags++;
            // the guest runs the event at its next safepoint (and reschedules); check again soon
            sleepUs(200, false);
            continue;
        }
        if (e == ~0ull) {
            sleepUs(50000, true);
            continue;
        }
        const u64 waitUs = (e - now) * 1000000 / kTbClock;
        if (waitUs > 300) sleepUs(waitUs - 150, true);  // wake a little early, then spin
        else
            while (tbNow() < e && s_earliest.load() == e) cpuRelax();
    }
}
}  // namespace

// RL-NX (PERF_PLAN phase 0b): R2_DETERMINISTIC=1 - the guest's own clock. Defined outside the anonymous
// namespace above because the frame hook in switch/src/host_switch.cpp calls detAdvanceFrame.
bool deterministicClock() { return s_det; }

void detInit() {
    const char* v = getenv("R2_DETERMINISTIC");
    const bool want = v && *v && *v != '0';
    // Measured dead end, PERF_HANDOFF 9.47: with the clock standing still between presented frames the
    // guest stops making progress during boot, where it waits on a DSP interrupt that only a moving
    // timebase can raise (the run ended at 4.18 s, last line a DSP register write). The mechanism stays
    // here for the next attempt - an instruction-driven clock - but it is not enabled, so that setting the
    // switch cannot turn a working build into a hang.
    if (want) {
        logf("det", "R2_DETERMINISTIC is disabled: a frame-driven guest clock deadlocks the boot (PERF_HANDOFF 9.47)");
    }
    s_det = false;
    if (!s_det) return;
    const double rate = viRate() > 0.0 ? viRate() : 60.0;
    s_detPeriod = u64(double(kTbClock) / rate);
    s_detTb = 0;
    logf("det", "deterministic clock: the guest's timebase advances %llu ticks per presented frame (%.2f Hz), nothing in between",
         (unsigned long long)s_detPeriod, rate);
}

// One presented frame: the only place the guest's clock moves when determinism is on. Raising the pending
// flag makes the guest re-evaluate the event queue at its next safepoint, so events due at the new time fire
// on this frame rather than whenever the timer thread next wakes on the host's clock.
void detAdvanceFrame() {
    if (!s_det) return;
    s_detTb += s_detPeriod;
    raisePending();
}

#ifdef _WIN32
u64 tbNow() {
    if (s_det) return s_detTb;  // RL-NX: R2_DETERMINISTIC (phase 0b)
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const u64 d = u64(t.QuadPart - s_qpcStart.QuadPart);
    const u64 f = u64(s_qpcFreq.QuadPart);
    return (d / f) * kTbClock + (d % f) * kTbClock / f;
}

void eventsInit() {
    QueryPerformanceFrequency(&s_qpcFreq);
    QueryPerformanceCounter(&s_qpcStart);
    timeBeginPeriod(1);
    s_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_timer) s_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // before Windows 10 1803
    s_wakeEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_guestEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(timerThread).detach();
}
#else
u64 tbNow() {
    if (s_det) return s_detTb;  // RL-NX: R2_DETERMINISTIC (phase 0b)
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const u64 ns = u64(t.tv_sec - s_start.tv_sec) * 1000000000ull + u64(t.tv_nsec) - u64(s_start.tv_nsec);
    return (ns / 1000000000ull) * kTbClock + (ns % 1000000000ull) * kTbClock / 1000000000ull;
}

void eventsInit() {
    clock_gettime(CLOCK_MONOTONIC, &s_start);
    std::thread([] {
        prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
        timerThread();
    }).detach();
}
#endif

void schedule(int id, u64 at) {
    if (!at) at = 1;
    std::lock_guard lk(s_mu);
    s_due[id] = at;
    const u64 before = s_earliest.load();
    recomputeEarliest();
    if (s_earliest.load() < before) wakeTimer();
}

void unschedule(int id) {
    std::lock_guard lk(s_mu);
    s_due[id] = 0;
    recomputeEarliest();
}

bool scheduled(int id) { return s_due[id] != 0; }

void wakeGuest() { raisePending(); }

u64 g_idleWaits = 0;  // R2_GX_LOG statistics

void idleWait(Ctx& c, u32 pc) {
    if (g_oracleIo) [[unlikely]] oracleAbandon();  // RL-NX (9.55): an idle loop cannot end inside a probe
    gxGpKick();  // RL-NX (9.57): the GP thread sees everything queued before the guest sleeps on its interrupt
    if (!g_irqPending && !g_irqLine) {
        g_idleWaits++;
        // The timer thread signals when an event is due; the timeout only bounds a missed wake.
        const u64 t0 = tbNow();
        const uint64_t pm0 = rlPmTick();  // RL-NX: PERF LOG - the game thread's idle time per frame
        waitGuest(4);
        rlPmAddTimer(RLPM_IDLE, pm0);
        g_idleTb += tbNow() - t0;  // (the session log's "game thread busy")
    }
    poll(c, pc);
}

void runDueEvents() {
    const u64 now = tbNow();
    for (int id = 0; id < EV_COUNT; id++) {
        bool fire = false;
        {
            std::lock_guard lk(s_mu);
            if (s_due[id] && s_due[id] <= now) {
                s_due[id] = 0;
                recomputeEarliest();
                fire = true;
            }
        }
        if (fire && g_eventHandlers[id]) g_eventHandlers[id]();
    }
}

// --- decrementer ----------------------------------------------------------------------
static void onDec() {
    s_decPending = true;
    g_irqLine |= 2;
    wakeGuest();
}

void decWrite(u32 v) {
    s_decBaseTb = tbNow();
    s_decValue = v;
    if (!g_eventHandlers[EV_DEC]) g_eventHandlers[EV_DEC] = onDec;
    if (v & 0x80000000u) onDec();
    else schedule(EV_DEC, s_decBaseTb + v + 1);
}

u32 decRead() { return s_decValue - u32(tbNow() - s_decBaseTb); }
bool decPending() { return s_decPending; }
void decAck() {
    s_decPending = false;
    g_irqLine &= ~2u;
}

}  // namespace rt

// --- save states ----------------------------------------------------------------------------
#include "savestate.h"
namespace rt {
void eventsSaveState(state::Writer& w) {
    w.section("EVNT");
    {
        std::lock_guard lk(s_mu);
        w.pod(s_due);
    }
    w.pod(s_decBaseTb);
    w.pod(s_decValue);
    w.pod(s_decPending);
}

void eventsLoadState(state::Reader& r) {
    r.section("EVNT");
    u64 due[EV_COUNT];
    r.pod(due);
    r.pod(s_decBaseTb);
    r.pod(s_decValue);
    r.pod(s_decPending);
    s_decBaseTb = r.time(s_decBaseTb);
    {
        std::lock_guard lk(s_mu);
        for (int i = 0; i < EV_COUNT; i++) s_due[i] = r.time(due[i]);
        recomputeEarliest();
    }
    wakeTimer();
    if (!g_eventHandlers[EV_DEC]) g_eventHandlers[EV_DEC] = onDec;
    if (s_decPending) g_irqLine |= 2;
    else g_irqLine &= ~2u;
    g_irqPending = 1;
}
}  // namespace rt
