// RL-NX: audio output on Horizon.
//
// hle_musyx.cpp mixes the game's MusyX voices into AI DMA blocks - big-endian 16-bit stereo at the
// game's 32 kHz - and hands each finished block to rt::audio::push (hw/musyx_dsp.h). On the desktop
// audio.cpp plays those through SDL; here they go out through libnx's audout, which is 48 kHz stereo
// s16 little-endian, so every block is endian-swapped and resampled 3:2 on the way.
//
// No audio thread: push() is called from the DSP's block thread, buffer reclamation is non-blocking
// (audoutGetReleasedAudioOutBuffer), and when all four buffers are in flight the excess is dropped and
// counted. Dropping beats blocking - the game thread must never wait on the audio device, and an
// audible gap from a full queue is a signal in the log rather than a stalled frame.
//
// RL-NX (9.57): the audio-out thread. The audout calls are IPC (svcSendSyncRequest), ~0.75 ms a frame on the game
// thread in the 00:56 gameplay profile. push() now copies the finished block into a small queue and a thread of
// its own does the resample and the audout calls. The DSP (hle_musyx.cpp) still runs on the guest thread; only
// the device side moved. R2_AUDIO_THREAD=0 or the - menu's AUDIO THREAD row puts it back inline. Either way the
// work runs under s_mx, so switching mid-stream is safe and blocks keep their order (inline only once the queue
// is empty).
#include <atomic>  // RL-NX (9.57): the audio-out thread
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>  // memalign: audout's buffers are aligned to 0x1000
#include <mutex>     // RL-NX (9.57)
#include <vector>

#include <switch.h>

#include "hw/musyx_dsp.h"  // rt::audio::* (the interface this file implements)
#include "log_switch.h"
#include "perfmon.h"  // RL-NX: PERF LOG audio submit timer
#include "runtime.h"

extern "C" void melee_nx_perf_background_thread(const char* name);  // RL-NX (9.57): perf_switch.cpp

namespace rt::audio {
namespace {

constexpr int kBuffers = 4;
// 6144 bytes = 3072 s16 samples = 1536 stereo frames at 48 kHz (~32 ms). audout wants the buffer and
// its size aligned to 0x1000. (s_pending counts samples, not frames - the earlier code mixed the two
// and copied twice as many samples as it erased.)
constexpr u32 kBufferBytes = 0x1800;
constexpr size_t kBufferSamples = kBufferBytes / 2;

bool s_up = false;
u32 s_outRate = 48000;
float s_gain = 1.0f;

AudioOutBuffer s_bufs[kBuffers] = {};
void* s_mem[kBuffers] = {};
bool s_free[kBuffers] = {true, true, true, true};

std::vector<s16> s_pending;  // output samples (interleaved) waiting for a free buffer
double s_phase = 0.0;        // resampler position, in input frames, inside the current block
s16 s_prev[2] = {0, 0};      // the last input frame (the sample left of position 0)

// The rate the blocks actually arrive at, not the rate they claim. The guest's AI clock wants a
// block every 5 ms, but it is serviced at the guest's safepoints: measured with R2_AI_LOG, every
// boundary lands 5-30 ms late and the stream arrives at ~84% of 32 kHz. Resampling at the nominal
// rate then produces more audio than arrives and the device starves (gaps); resampling at the
// measured rate keeps the output continuous and, just as importantly, in step with the visuals,
// which run on the same slipping clock. Clamped so a stall cannot bend the pitch.
double s_inputRate = 0.0;     // measured input frames per second (0: not measured yet)
u64 s_measureStart = 0;
u64 s_measureFrames = 0;

u64 s_blocks = 0, s_submitted = 0, s_dropped = 0, s_frames = 0;

// Reclaims every buffer the device has finished with. Non-blocking.
void reclaim() {
    RlPmScope pmTimer(RLPM_AUDIO);  // RL-NX: PERF LOG - audout calls on the game thread
    for (;;) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || !count || !released) return;
        for (int i = 0; i < kBuffers; i++)
            if (&s_bufs[i] == released) s_free[i] = true;
    }
}

// Hands the oldest complete buffer to the device, if one is free.
void flushOne() {
    if (s_pending.size() < kBufferSamples) return;
    RlPmScope pmTimer(RLPM_AUDIO);  // RL-NX: PERF LOG - audout calls on the game thread
    int slot = -1;
    for (int i = 0; i < kBuffers; i++)
        if (s_free[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {  // all four in flight: the device is behind by ~128 ms, drop rather than stall
        s_pending.erase(s_pending.begin(), s_pending.begin() + kBufferSamples);
        s_dropped++;
        return;
    }
    std::memcpy(s_mem[slot], s_pending.data(), kBufferBytes);
    s_pending.erase(s_pending.begin(), s_pending.begin() + kBufferSamples);
    AudioOutBuffer& b = s_bufs[slot];
    b.next = nullptr;
    b.buffer = s_mem[slot];
    b.buffer_size = kBufferBytes;
    b.data_size = kBufferBytes;
    b.data_offset = 0;
    s_free[slot] = false;
    if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&b))) s_submitted++;
    else s_free[slot] = true;
}


// RL-NX (9.57): the audio-out thread's queue - one producer (the guest thread's push), one consumer.
constexpr int kSlots = 32;                 // ~160 ms of 5 ms blocks
constexpr u32 kSlotBytes = 4096;           // 1024 stereo frames; a longer block is queued in pieces
struct Slot {
    u32 frames, rate;
    u8 data[kSlotBytes];
};
Slot* s_slots = nullptr;
std::atomic<u32> s_qHead{0}, s_qTail{0};   // written by the producer / the consumer
std::atomic<bool> s_threaded{false};
std::atomic<int> s_threadWant{-1};
std::mutex s_mx;                           // the resampler state and the device calls
bool s_threadUp = false;
Thread s_thread;
u64 s_qFull = 0;

void processBlock(const u8* data, u32 frames, u32 rate);  // below

void drainQueue() {
    for (;;) {
        const u32 t = s_qTail.load(std::memory_order_relaxed);
        if (t == s_qHead.load(std::memory_order_acquire)) return;
        Slot& sl = s_slots[t % kSlots];
        {
            std::lock_guard<std::mutex> lk(s_mx);
            processBlock(sl.data, sl.frames, sl.rate);
        }
        s_qTail.store(t + 1, std::memory_order_release);
    }
}

void threadMain(void*) {
    melee_nx_perf_background_thread("audio out");  // perf_switch.cpp: lowest core, listed in [pm-thr]
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);  // short work that must be on time (as the DSP thread)
    for (;;) {
        drainQueue();
        svcSleepThread(2000000);  // 2 ms: blocks arrive every ~5 ms, the device holds ~128 ms
    }
}

}  // namespace

void init() {
    if (R_FAILED(audoutInitialize())) {
        logf("audio", "audoutInitialize failed; the game runs silently");
        return;
    }
    s_outRate = audoutGetSampleRate() ? audoutGetSampleRate() : 48000;
    const u32 channels = audoutGetChannelCount() ? audoutGetChannelCount() : 2;
    for (int i = 0; i < kBuffers; i++) {
        s_mem[i] = memalign(0x1000, kBufferBytes);
        if (!s_mem[i]) {
            logf("audio", "no memory for the output buffers; running silently");
            audoutExit();
            return;
        }
        std::memset(s_mem[i], 0, kBufferBytes);
        s_bufs[i] = {};
    }
    if (R_FAILED(audoutStartAudioOut())) {
        logf("audio", "audoutStartAudioOut failed; running silently");
        for (int i = 0; i < kBuffers; i++) free(s_mem[i]);
        audoutExit();
        return;
    }
    s_up = true;
    logf("audio", "audout up: %u Hz, %u channels, %d buffers of %u bytes", s_outRate, channels, kBuffers, kBufferBytes);
    // RL-NX (9.57): the audio-out thread (on unless R2_AUDIO_THREAD=0)
    s_slots = static_cast<Slot*>(malloc(sizeof(Slot) * kSlots));
    if (s_slots && R_SUCCEEDED(threadCreate(&s_thread, threadMain, nullptr, nullptr, 0x10000, 0x2B, -2)) &&
        R_SUCCEEDED(threadStart(&s_thread))) {
        s_threadUp = true;
        const char* e = getenv("R2_AUDIO_THREAD");
        s_threaded.store(!(e && *e == '0'), std::memory_order_relaxed);
    }
    logf("audio", "audio-out thread: %s", !s_threadUp ? "could not start, inline" : s_threaded.load() ? "on" : "off (R2_AUDIO_THREAD=0)");
}

namespace {
// RL-NX (9.57): what push() did, now run by the audio-out thread (or inline, under s_mx either way).
void processBlock(const u8* data, u32 frames, u32 rate) {
    s_blocks++;
    if (!s_up) {  // still count, so "the mixer runs" stays visible in the log with no device
        s_frames += frames;
        if (s_blocks % 500 == 0)
            logf("audio", "%llu blocks, %llu frames, %u Hz (no output device)", (unsigned long long)s_blocks,
                 (unsigned long long)s_frames, rate);
        return;
    }
    reclaim();
    s_frames += frames;

    // Re-measure the arrival rate about once a second, smoothed, and clamped to +/-25% of the rate
    // the DMA registers claim (so a pause in the stream cannot slide the pitch).
    const u64 now = svcGetSystemTick();
    if (!s_measureStart) {
        s_measureStart = now;
        s_measureFrames = 0;
    }
    s_measureFrames += frames;
    const u64 window = 19200000;  // one second in ticks
    if (now - s_measureStart >= window) {
        const double measured = double(s_measureFrames) * 19200000.0 / double(now - s_measureStart);
        s_measureStart = now;
        s_measureFrames = 0;
        if (measured > rate * 0.75 && measured < rate * 1.25)
            s_inputRate = s_inputRate > 0.0 ? s_inputRate * 0.75 + measured * 0.25 : measured;
    }
    const double sourceRate = s_inputRate > 0.0 ? s_inputRate : double(rate ? rate : 32000);
    const double step = sourceRate / double(s_outRate);
    const auto be16 = [](const u8* p) { return s16((u16(p[0]) << 8) | p[1]); };  // MusyX blocks are BE

    // Resample with a two-point lerp, carrying the phase and one sample across blocks so the seam
    // between two blocks is continuous.
    while (s_phase < double(frames)) {
        const int i = int(std::floor(s_phase));
        const double frac = s_phase - double(i);
        for (int ch = 0; ch < 2; ch++) {
            const s16 a = i < 0 ? s_prev[ch] : be16(data + u32(i) * 4 + ch * 2);
            const s16 b = i + 1 < int(frames) ? be16(data + u32(i + 1) * 4 + ch * 2) : a;
            float v = float(a) + (float(b) - float(a)) * float(frac);
            v *= s_gain;
            if (v > 32767.f) v = 32767.f;
            if (v < -32768.f) v = -32768.f;
            s_pending.push_back(s16(v));
        }
        s_phase += step;
    }
    s_phase -= double(frames);
    s_prev[0] = frames ? be16(data + (frames - 1) * 4) : s_prev[0];
    s_prev[1] = frames ? be16(data + (frames - 1) * 4 + 2) : s_prev[1];

    while (s_pending.size() >= kBufferSamples) flushOne();
    // The staging vector only ever holds less than one buffer plus one block's worth.
    if (s_pending.size() > 8 * kBufferSamples) s_pending.clear();

    if (s_blocks % 500 == 0)
        logf("audio", "%llu blocks, %llu frames, %u Hz (arriving at %.0f Hz): %llu buffers out, %llu dropped, %zu queued, %llu thread-queue drops",
             (unsigned long long)s_blocks, (unsigned long long)s_frames, rate, sourceRate,
             (unsigned long long)s_submitted, (unsigned long long)s_dropped, s_pending.size(), (unsigned long long)s_qFull);
}
}  // namespace

// RL-NX (9.57): on the guest thread. Queues the block for the audio-out thread, or runs it here.
void push(const u8* data, u32 frames, u32 rate) {
    if (!data || !frames) return;
    if (const int want = s_threadWant.exchange(-1, std::memory_order_relaxed); want >= 0 && s_threadUp) {
        s_threaded.store(want != 0, std::memory_order_relaxed);
        logf("audio", "audio-out thread %s", want ? "on" : "off");
    }
    const bool queued = s_qHead.load(std::memory_order_relaxed) != s_qTail.load(std::memory_order_acquire);
    if (!s_threaded.load(std::memory_order_relaxed) && !queued) {
        std::lock_guard<std::mutex> lk(s_mx);
        processBlock(data, frames, rate);
        return;
    }
    while (frames) {
        const u32 n = frames < kSlotBytes / 4 ? frames : kSlotBytes / 4;
        const u32 h = s_qHead.load(std::memory_order_relaxed);
        if (h - s_qTail.load(std::memory_order_acquire) >= u32(kSlots)) {  // the thread is ~160 ms behind: drop
            s_qFull++;
            return;
        }
        Slot& sl = s_slots[h % kSlots];
        sl.frames = n;
        sl.rate = rate;
        std::memcpy(sl.data, data, n * 4);
        s_qHead.store(h + 1, std::memory_order_release);
        data += n * 4;
        frames -= n;
    }
}

// RL-NX (9.57): the - menu's AUDIO THREAD row (applied at the next block)
void setThreaded(bool on) { s_threadWant.store(on ? 1 : 0, std::memory_order_relaxed); }
bool threaded() {
    const int w = s_threadWant.load(std::memory_order_relaxed);
    return w >= 0 ? w != 0 : s_threaded.load(std::memory_order_relaxed);
}

void setGain(float gain) { s_gain = gain < 0.f ? 0.f : gain; }
void setSpeakers(int) {}  // the game's stereo mix goes out as stereo
int outputChannels() { return s_up ? 2 : 0; }
void deviceEvent(const void*) {}
std::string perfText() { return {}; }

}  // namespace rt::audio
