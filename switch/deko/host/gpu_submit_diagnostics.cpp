// The release deko3d library turns a failed NVIDIA submission into a generic 2359-0001 abort.
// Preserve the driver result before that path loses it. (From KartPadNX; linked with
// --wrap=nvGpuChannelAppendEntry and --wrap=nvGpuChannelKickoff, switch/CMakeLists.txt.)
#include <switch/types.h>
#include <switch/result.h>
extern "C" {
#include <switch/nvidia/gpu_channel.h>
}
#include <cstdio>

extern "C" Result __real_nvGpuChannelAppendEntry(NvGpuChannel*, iova_t, size_t, u32, u32);
extern "C" Result __real_nvGpuChannelKickoff(NvGpuChannel*);
extern "C" Result __real_nvGpuChannelGetErrorNotification(NvGpuChannel*, NvNotification*);
extern "C" void kartpad_deko_log_failure_context();
extern "C" void kartpad_log_flush_sync(void);

namespace {
// Logs everything known about a failed GPFIFO append or kickoff: the result, the channel's entry count
// and fence, the renderer's own context and breadcrumbs, and the channel's error info from the driver.
void report_failure(const char* operation, NvGpuChannel* channel, Result result,
                    iova_t address = 0, size_t words = 0, u32 flags = 0, u32 threshold = 0) {
    // Do not query/consume the error event here: deko3d's checkError still owns it. Likewise, leave
    // the return value and submission/retry policy intact.
    std::fprintf(stderr,
        "[gpu-submit] %s failed: rc=0x%08x (%04u-%04u), fd=%u, entries=%u/%u, "
        "syncpoint=%u value=%u increment=%u, iova=0x%llx words=%zu flags=0x%x threshold=%u\n",
        operation, result, 2000 + R_MODULE(result), R_DESCRIPTION(result), channel->base.fd,
        channel->num_entries, GPFIFO_QUEUE_SIZE, channel->fence.id, channel->fence.value,
        channel->fence_incr, static_cast<unsigned long long>(address), words, flags, threshold);
    // Save the original result before any optional follow-up diagnostic call.
    std::fflush(stderr);
    kartpad_log_flush_sync();
    // GPU progress first: the ioctl below is not implemented everywhere (Ryujinx stops on it).
    kartpad_deko_log_failure_context();
    std::fflush(stderr);
    kartpad_log_flush_sync();
    // This ioctl reads the channel's error details without waiting on or clearing the notification
    // event. It can tell a GPU fault from a submission error even before the notifier is signalled.
    NvError error{};
    const Result infoResult = nvGpuChannelGetErrorInfo(channel, &error);
    std::fprintf(stderr, "[gpu-submit] error_info rc=0x%08x type=0x%x", infoResult, error.type);
    if (R_SUCCEEDED(infoResult))
        for (u32 word : error.info) std::fprintf(stderr, " %08x", word);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    kartpad_log_flush_sync();
}
}  // namespace

namespace {
NvGpuChannel* g_channel = nullptr;  // the queue's channel (one queue in this port)
u32 g_maxKickoffEntries = 0;
}  // namespace

// GPFIFO entries appended since the last kickoff. deko3d appends at every dkQueueSubmitCommands and kicks
// off only at a flush, and libnx refuses a kickoff carrying too many entries (2345-0002, seen with 1482).
extern "C" u32 kartpad_gpu_pending_entries(void) { return g_channel ? g_channel->num_entries : 0; }
extern "C" u32 kartpad_gpu_max_kickoff_entries(void) {
    const u32 v = g_maxKickoffEntries;
    g_maxKickoffEntries = 0;
    return v;
}

// The wrappers call libnx's real functions unchanged; they only remember the channel, track the largest
// kickoff, and report failures.
extern "C" Result __wrap_nvGpuChannelAppendEntry(NvGpuChannel* channel, iova_t address,
                                                  size_t words, u32 flags, u32 threshold) {
    g_channel = channel;
    const Result result = __real_nvGpuChannelAppendEntry(channel, address, words, flags, threshold);
    if (R_FAILED(result)) report_failure("append", channel, result, address, words, flags, threshold);
    return result;
}

extern "C" Result __wrap_nvGpuChannelKickoff(NvGpuChannel* channel) {
    if (channel->num_entries > g_maxKickoffEntries) g_maxKickoffEntries = channel->num_entries;
    const Result result = __real_nvGpuChannelKickoff(channel);
    if (R_FAILED(result)) report_failure("kickoff", channel, result);
    return result;
}

// RL-NX: the fault itself. deko3d's release build compiles its error report out (DK_WARNING is a no-op
// there), so a GPU fault reaches the log only as the generic 2359-0001 abort that the *next* submit or
// flush raises from "queue in error state", with nothing about what failed. This wrapper prints the
// notification and the driver's error info the moment the queue learns of it, then hands it back
// untouched: deko3d's checkError still owns the event and sets its own error state from it. Silent unless
// the driver actually reports an error.
extern "C" Result __wrap_nvGpuChannelGetErrorNotification(NvGpuChannel* channel, NvNotification* notif) {
    const Result result = __real_nvGpuChannelGetErrorNotification(channel, notif);
    if (R_SUCCEEDED(result) && notif->status) {
        std::fprintf(stderr,
            "[gpu-submit] GPU error notification: status=0x%x info16=0x%x info32=0x%x timestamp=%llu, "
            "fd=%u entries=%u/%u\n",
            notif->status, notif->info16, notif->info32,
            static_cast<unsigned long long>(notif->timestamp), channel->base.fd, channel->num_entries,
            GPFIFO_QUEUE_SIZE);
        NvError error{};
        const Result infoResult = nvGpuChannelGetErrorInfo(channel, &error);
        if (R_SUCCEEDED(infoResult)) {
            std::fprintf(stderr, "[gpu-submit]   type=0x%x info: %08x %08x %08x %08x %08x\n", error.type,
                         error.info[0], error.info[1], error.info[2], error.info[3], error.info[4]);
            if (error.type == 1)
                std::fprintf(stderr, "[gpu-submit]   GPU page fault: address 0x%02x%08x, %s\n", error.info[1],
                             error.info[2], error.info[3] == 2 ? "write" : "read");
        } else {
            std::fprintf(stderr, "[gpu-submit]   error info unavailable, rc=0x%08x\n", infoResult);
        }
        std::fflush(stderr);
        kartpad_log_flush_sync();
        kartpad_deko_log_failure_context();  // names the last GPU operation that finished
        std::fflush(stderr);
        kartpad_log_flush_sync();
    }
    return result;
}
