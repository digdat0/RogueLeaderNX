// Glue for the deko3d renderer's kit hooks (switch/deko and switch/deko/host).
//
// switch/deko came from SFA-NX, where the Switch layer provided a handful of globals and functions by
// those names. Red Two's Switch layer names them differently (its log flag is rl_log_extra_on, its
// profiler is perf_switch.cpp), so the renderer gets the four names it links against here, defined in
// terms of ours - nothing in switch/deko is edited.
#include <malloc.h>   // mallinfo(): the dlmalloc statistics from malloc_switch.c
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "log_switch.h"

extern "C" {

// switch/deko: "the - menu's Extra logging" - the same flag as rl_log_extra_on, which rl_log_set_extra()
// keeps in step (log_switch.cpp). A plain int because backend_commands.cpp loads it atomically.
int sfa_log_extra_on = 0;

// gpu_timing.cpp: the GPU timing sampler. RL-NX: switched by the - menu's GPU TIMING row (perfmon_switch.cpp);
// it was hard-wired off.
int rlnxGpuTimingEnabled(void);
int sfaProfEnabled(void) { return rlnxGpuTimingEnabled(); }

// host/deko_host_switch.cpp and perf_switch.cpp: one formatted line into the buffered log.
void pc_log_line(const char* fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
  va_end(args);
  if (n <= 0) {
    return;
  }
  const size_t length = static_cast<size_t>(n) < sizeof(line) - 2 ? static_cast<size_t>(n) : sizeof(line) - 3;
  line[length] = '\n';
  melee_nx_log_write(line, length + 1);
}

// device.cpp: the heap statistics it logs every 30 s. The layout must match the struct it declares
// there (it is filled through this pointer), so the fields are assigned in its order.
struct SfaHeapStats {
  uint64_t mallocCalls, freeCalls;
  size_t footprint, inUse, freeInside, freeChunks, topFree;
  uintptr_t topCaller[8];
  uint32_t topCount[8];
  uint32_t sampleEvery;
  uintptr_t growCaller[8];
  int64_t growBytes[8];
  uint64_t trackedLive;
  uint32_t trackDropped;
};

void sfa_heap_stats(SfaHeapStats* out) {
  if (!out) {
    return;
  }
  *out = SfaHeapStats{};
  const struct mallinfo info = mallinfo();  // malloc_switch.c: dlmalloc's own accounting
  out->footprint = static_cast<size_t>(info.arena);
  out->inUse = static_cast<size_t>(info.uordblks);
  out->freeInside = static_cast<size_t>(info.fordblks);
  out->freeChunks = static_cast<size_t>(info.ordblks);
  // topCaller/topCount (the sampled allocation sites) belong to the kit's leak finder, which Red Two
  // does not have; leaving them zero means the statistics line has no hot sites, not a wrong one.
}

}  // extern "C"
