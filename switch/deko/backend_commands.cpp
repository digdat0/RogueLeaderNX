// Native deko3d adapter, part 3: command recording and replay. Encoders and render passes
// record closures; queue submission replays them into the frame's deko3d command buffer, where
// the draw state (bindings, pipeline, split RGB/alpha blending) is applied. See
// backend_internal.h for the whole adapter.
#include "backend_internal.h"
#include "copy_fsh_dksh.h"
#include "present_vsh_dksh.h"
#ifdef COS_DK_DEBUG_LIB
#include "probe_fsh_dksh.h"
#include <sys/stat.h>
#endif
// Hooks from KartPad-NX: the transition guard (a racing-game notion of "course loading") does not exist in
// SFA-NX, so kartpad_gpu_transition_guard is null and the "synchronous course transition" paths below
// never run here. kartpad_opt is provided by host/deko_host_switch.cpp (perf.cfg values).
extern "C" void kartpad_deko_log_failure_context();
extern "C" int sfa_log_extra_on;  // switch/src/log_switch.cpp: the - menu's Extra logging
// switch/src/transition_guard.cpp; weak, so builds without the game (renderer tests) link.
extern "C" int kartpad_gpu_transition_guard(void) __attribute__((weak));
extern "C" int kartpad_opt(const char *key, int fallback) __attribute__((weak));

namespace kd {
std::unordered_map<WGPUCommandEncoder, std::shared_ptr<Commands>> encoders;
std::unordered_map<WGPUCommandEncoder, int> encoderRefs;
std::unordered_map<WGPUCommandBuffer, std::pair<std::shared_ptr<Commands>, int>> commands;
std::unordered_map<WGPURenderPassEncoder, Pass> passes;
State state;
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
namespace {
// GPU diagnostics, compiled in only with the COS_DK_GPU_* macros (switch/cmake/Deko3D.cmake options). SFA-NX builds with
// breadcrumbs: every pass, copy, upload and present writes its serial number from the GPU into memory the
// CPU can read, so after a GPU timeout (2348-0006) the log names the last operation the GPU finished and
// the first one it did not.
const char *gpuOperation = "<none>";
uint64_t gpuArgs[3]{}, gpuOperationSerial = 0, gpuProgressSerial = 0;
struct OperationRecord {
  const char *name = "<none>";
  uint64_t serial = 0, args[3]{};
};
std::array<OperationRecord, 32> gpuRecent{};
struct ImageCopyRecord {
  uint64_t serial = 0, srcAddress = 0, dstAddress = 0;
  uint32_t srcSize = 0, dstSize = 0, srcFormat = 0, dstFormat = 0;
  uint32_t srcMip = 0, dstMip = 0, srcOrigin[3]{}, dstOrigin[3]{};
  uint32_t extent[3]{};  // RL-NX: the copied extent (an upload's record would otherwise miss it)
  uint32_t srcPitch = 0, srcRows = 0;  // RL-NX: an upload's staging row pitch and row count
  char source[96]{}, destination[96]{};
} gpuImageCopy;
// The bytes snprintf(out, N, "%s", label) writes, without the printf machinery: breadcrumbs
// copy two labels twice for every GPU operation on the frame worker.
template <size_t N> void copy_label(char (&out)[N], const char *label) {
  const size_t n = strnlen(label, N - 1);
  std::memcpy(out, label, n);
  out[n] = '\0';
}
#ifdef COS_DK_GPU_BREADCRUMBS
// Reports write a value and timestamp into permanent, CPU-uncached memory.
// Neither this storage nor its CPU history moves while commands are in flight.
// Reuse requires the old GPU-written serial, rather than a CPU submit count.
constexpr size_t kGpuBreadcrumbs = 16384;
struct Breadcrumb {
  OperationRecord operation{nullptr, 0, {}};
  uint64_t frame = 0, vs = 0, fs = 0, targetAddress = 0, depthAddress = 0;
  uint64_t indexAddress = 0, indexBytes = 0;
  uint32_t width = 0, height = 0;
  char pipeline[96]{}, target[96]{};
  ImageCopyRecord copy;
  bool recorded = false;
};
std::array<Breadcrumb, kGpuBreadcrumbs> gpuBreadcrumbs;
Block gpuReports;
void log_gpu_breadcrumbs();
uint64_t breadcrumb_value(uint64_t serial) {
  if (!gpuReports) return 0;
  return reinterpret_cast<volatile const uint64_t *>(gpuReports.cpu)[(serial % kGpuBreadcrumbs) * 2];
}
void breadcrumb_context(Breadcrumb &record) {
  const auto &p = state.pipeline;
  const auto *v = state.target ? find_view(state.target.Get()) : nullptr;
  const auto *d = state.depthTarget ? find_view(state.depthTarget.Get()) : nullptr;
  const auto *t = v ? v->tex : nullptr;
  record.vs = p && p->vs.shader ? p->vs.shader->glslHash : 0;
  record.fs = p && p->fs.shader ? p->fs.shader->glslHash : 0;
  record.targetAddress = t ? dkImageGetGpuAddr(&t->image) : 0;
  record.depthAddress = d ? dkImageGetGpuAddr(&d->tex->image) : 0;
  record.width = t ? t->width : 0; record.height = t ? t->height : 0;
  record.indexAddress = state.index ? state.index->mem.gpu + state.indexOffset : 0;
  record.indexBytes = state.index ? state.index->size - state.indexOffset : 0;
  copy_label(record.pipeline, p ? p->label.c_str() : "<none>");
  copy_label(record.target, t ? t->label.c_str() : "<none>");
}
// SFA-NX: whether the GPU has written this operation's breadcrumb. ReportValue writes a 32-bit payload, so
// only the low 32 bits are compared. The serial counts every operation, draws included (about 16k a
// second), and so passes 2^32 after about 74 hours of running. The old check stopped the game there
// ("GPU breadcrumb serial exhausted"). Wrapping is safe. Slot k only ever holds serials congruent to k
// mod kGpuBreadcrumbs, and 2^32 is a multiple of that, so an older value matches the low bits only if it
// was written a whole 2^32 serials (about 74 hours) earlier. Even then, the only effect is a skipped
// history check.
bool breadcrumb_reported(uint64_t serial) {
  return uint32_t(breadcrumb_value(serial)) == uint32_t(serial);
}
void breadcrumb_begin() {
  auto &record = gpuBreadcrumbs[gpuOperationSerial % kGpuBreadcrumbs];
  if (record.operation.serial && (!record.recorded || !breadcrumb_reported(record.operation.serial))) {
    log_gpu_breadcrumbs();
    fatal("GPU breadcrumb history full: refusing to overwrite unfinished operation %llu",
          (unsigned long long)record.operation.serial);
  }
  record = {};
  record.operation = {gpuOperation, gpuOperationSerial, {gpuArgs[0], gpuArgs[1], gpuArgs[2]}};
  record.frame = R.frame + 1;
  breadcrumb_context(record);
}
void breadcrumb_end() {
  if (!gpuReports) {
    gpuReports = block_create(kGpuBreadcrumbs * 16, "GPU completion breadcrumbs");
    if (!gpuReports) fatal("Cannot allocate GPU completion breadcrumbs");
    std::memset(gpuReports.cpu, 0, gpuReports.size);
  }
  auto &record = gpuBreadcrumbs[gpuOperationSerial % kGpuBreadcrumbs];
  breadcrumb_context(record);
  if (gpuImageCopy.serial == gpuOperationSerial) record.copy = gpuImageCopy;
  dkCmdBufReportValue(R.cmd, uint32_t(gpuOperationSerial),
                      gpuReports.gpu + (gpuOperationSerial % kGpuBreadcrumbs) * 16);
  record.recorded = true;
  static uint64_t nextSampleFrame = 0;
  // A progress sample every 300 frames, with Extra logging only: a GPU hang dumps the breadcrumbs anyway.
  if (record.frame >= nextSampleFrame && __atomic_load_n(&sfa_log_extra_on, __ATOMIC_RELAXED)) {
    const uint64_t sample = gpuOperationSerial > 2048 ? gpuOperationSerial - 2048 : 1;
    dklog("GPU progress sample: CPU=%llu sample_serial=%llu GPU_value=%llu",
          (unsigned long long)gpuOperationSerial, (unsigned long long)sample,
          (unsigned long long)breadcrumb_value(sample));
    nextSampleFrame = record.frame + 300;
  }
}
void log_gpu_breadcrumbs() {
  const uint64_t oldest = gpuOperationSerial >= kGpuBreadcrumbs ? gpuOperationSerial - kGpuBreadcrumbs + 1 : 1;
  uint64_t firstPending = 0, lastReported = 0;
  for (uint64_t serial = oldest; serial <= gpuOperationSerial; ++serial) {
    const auto &record = gpuBreadcrumbs[serial % kGpuBreadcrumbs];
    if (record.operation.serial != serial) continue;
    if (record.recorded && breadcrumb_reported(serial))
      lastReported = serial;
    else if (!firstPending)
      firstPending = serial;
  }
  std::fprintf(stderr, "[gpu-progress] CPU=%llu retained_from=%llu last_GPU_report=%llu first_missing=%llu report_addr=0x%llx\n",
      (unsigned long long)gpuOperationSerial, (unsigned long long)oldest,
      (unsigned long long)lastReported, (unsigned long long)firstPending,
      (unsigned long long)gpuReports.gpu);
  const uint64_t focus = firstPending ? firstPending : gpuOperationSerial;
  for (uint64_t serial = std::max(oldest, focus > 8 ? focus - 8 : 1);
       serial <= std::min(gpuOperationSerial, focus + 24); ++serial) {
    const auto &r = gpuBreadcrumbs[serial % kGpuBreadcrumbs];
    const auto &op = r.operation;
    if (op.serial != serial) continue;
    std::fprintf(stderr, "[gpu-breadcrumb] serial=%llu frame=%llu recorded=%u GPU_value=%llu op=%s args=%llu/%llu/%llu pipeline=%s vs=%016llx fs=%016llx target=%s %ux%u addr=0x%llx depth=0x%llx index=0x%llx bytes=%llu\n",
        (unsigned long long)serial, (unsigned long long)r.frame, unsigned(r.recorded),
        (unsigned long long)breadcrumb_value(serial), op.name,
        (unsigned long long)op.args[0], (unsigned long long)op.args[1], (unsigned long long)op.args[2],
        r.pipeline, (unsigned long long)r.vs, (unsigned long long)r.fs, r.target, r.width, r.height,
        (unsigned long long)r.targetAddress, (unsigned long long)r.depthAddress,
        (unsigned long long)r.indexAddress, (unsigned long long)r.indexBytes);
    if (r.copy.serial == serial)
      std::fprintf(stderr, "[gpu-breadcrumb-copy] serial=%llu source=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u destination=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u\n",
          (unsigned long long)serial, r.copy.source, (unsigned long long)r.copy.srcAddress, r.copy.srcSize,
          r.copy.srcFormat, r.copy.srcMip, r.copy.srcOrigin[0], r.copy.srcOrigin[1], r.copy.srcOrigin[2],
          r.copy.destination, (unsigned long long)r.copy.dstAddress, r.copy.dstSize,
          r.copy.dstFormat, r.copy.dstMip, r.copy.dstOrigin[0], r.copy.dstOrigin[1], r.copy.dstOrigin[2]);
    if (r.copy.extent[0])  // RL-NX: an upload's copied extent and the staging layout it came from
      std::fprintf(stderr, "[gpu-breadcrumb-copy] serial=%llu extent=%ux%ux%u pitch=%u rows=%u\n",
          (unsigned long long)serial, r.copy.extent[0], r.copy.extent[1], r.copy.extent[2],
          r.copy.srcPitch, r.copy.srcRows);
  }
}
#endif
#ifdef COS_DK_GPU_FLUSHES
uint32_t gpuOperationsPending = 0;
// Outside course transitions, native submission works as without these diagnostics (candidate C,
// docs/RR-PERFORMANCE-2026-10.md): the frame's own submits, no per-operation report and no pass,
// copy or interval flush. Every recorded 2348-0006 timeout came while a course loaded (the first one
// after the menus, or the next one after a race), where the guard below makes each operation
// complete; v9's policy in steady state cost ~80 submits
// and +3-6 ms of recording per present, and the game thread's GX wait rose from 6 to 45-230 ms/s.
// It did not prevent the timeout either (test 6). perf.cfg `gpu_steady_v9 1` restores it everywhere.
bool steady_v9() {
  static const bool on = kartpad_opt && kartpad_opt("gpu_steady_v9", 0) != 0;
  return on;
}
#ifdef COS_DK_GPU_BREADCRUMBS
bool gpuOperationTracked = false;  // this operation's breadcrumb was begun (gpu_operation_begin)
#endif
// Course loads: the guard (switch/src/transition_guard.cpp) is asked at most every 5 ms;
// R.synchronous follows it.
bool synchronous_operation() {
  if (!kartpad_gpu_transition_guard) return false;  // no scene guard in this build: never synchronous
  static uint64_t nextCheck = 0;
  const uint64_t now = now_ns();
  if (now >= nextCheck) {
    nextCheck = now + 5'000'000;
    const bool synchronous = kartpad_gpu_transition_guard && kartpad_gpu_transition_guard() != 0;
    if (synchronous != R.synchronous)
      dklog(synchronous ? "course transition: each GPU operation completes before the next until a race runs"
                        : "race running: GPU operations asynchronous again");
    R.synchronous = synchronous;
  }
  return R.synchronous;
}
#endif
}
void gpu_operation_begin(const char *name, uint64_t a, uint64_t b, uint64_t c) {
  static const bool announced = [] {
#ifdef COS_DK_GPU_CHECKPOINTS
    swdk::dklog("GPU checkpoint test v3: native API validation and timed per-operation fences enabled");
#else
#ifdef COS_DK_GPU_BREADCRUMBS
    if (steady_v9())
      swdk::dklog("GPU progress test v9: GPU-written completion history; v8 pass/transfer flush policy; no CPU waits");
    else
      swdk::dklog("GPU submission: native outside course transitions; in them each operation completes, with "
                  "GPU-written completion history (perf.cfg gpu_steady_v9 1: v9 everywhere)");
#else
    swdk::dklog("GPU transition test v8: pass/transfer flushes, at most %u draw operations; CPU waits disabled",
               unsigned(COS_DK_GPU_FLUSH_INTERVAL));
#endif
#endif
    return true;
  }();
  (void)announced;
  gpuOperation = name;
  gpuArgs[0] = a; gpuArgs[1] = b; gpuArgs[2] = c;
  ++gpuOperationSerial;
  gpuRecent[gpuOperationSerial % gpuRecent.size()] = {name, gpuOperationSerial, {a, b, c}};
#ifdef COS_DK_GPU_BREADCRUMBS
  // Steady state records no breadcrumb (its report is a GPU write and flush per operation); slots
  // only ever hold completed transition operations, so breadcrumb_begin's reuse check still holds.
  // SFA-NX (2026-11-18): every operation but single draws leaves a GPU-written marker in steady state
  // too, without v9's flushes (a report per pass, copy, upload and present: a few dozen per frame), so a
  // 2348-0006 names the operation the GPU stopped in. Draws are covered by their pass's begin/end.
  gpuOperationTracked = steady_v9() || synchronous_operation() ||
                        (std::strcmp(name, "draw") != 0 && std::strcmp(name, "indexed draw") != 0);
  if (gpuOperationTracked) breadcrumb_begin();
#endif
}
void gpu_operation_end(bool flushBoundary) {
#ifdef COS_DK_GPU_BREADCRUMBS
  if (gpuOperationTracked) breadcrumb_end();
#endif
#ifdef COS_DK_GPU_CHECKPOINTS
  (void)flushBoundary;
  DkFence fence{};
  dkCmdBufSignalFence(R.cmd, &fence, true);
  submit_readback("GPU operation checkpoint");
  const DkResult result = dkFenceWait(&fence, 2'000'000'000ll);
  if (result != DkResult_Success) {
    kartpad_deko_log_failure_context();
    fatal("GPU checkpoint stopped after %s: result=%d frame=%llu", gpuOperation, int(result),
          static_cast<unsigned long long>(R.frame + 1));
  }
#else
  if (synchronous_operation()) {
    // v3's checkpoint without its validation: the operation and everything before it finish now.
    DkFence fence{};
    dkCmdBufSignalFence(R.cmd, &fence, true);
    submit_readback("course transition operation");
    gpuOperationsPending = 0;
    const DkResult result = dkFenceWait(&fence, 2'000'000'000ll);
    if (result != DkResult_Success) {
      kartpad_deko_log_failure_context();
      fatal("GPU operation did not complete during a course transition: %s, result=%d frame=%llu", gpuOperation,
            int(result), static_cast<unsigned long long>(R.frame + 1));
    }
    gpuProgressSerial = gpuOperationSerial;
    return;
  }
  if (!steady_v9()) return;  // native: the frame's submits carry it
  // Diagnostic flush policy only; a small submitted batch can still time out.
  if (++gpuOperationsPending < COS_DK_GPU_FLUSH_INTERVAL && !flushBoundary)
    return;
  submit_readback("asynchronous operation flush");
  gpuOperationsPending = 0;
#endif
  gpuProgressSerial = gpuOperationSerial;
}
void gpu_operation_image_copy(const Tex &source, const Tex &destination,
                              const WGPUTexelCopyTextureInfo &src,
                              const WGPUTexelCopyTextureInfo &dst) {
  gpuImageCopy.serial = gpuOperationSerial;
  gpuImageCopy.srcAddress = dkImageGetGpuAddr(&source.image);
  gpuImageCopy.dstAddress = dkImageGetGpuAddr(&destination.image);
  gpuImageCopy.srcSize = source.mem.size; gpuImageCopy.dstSize = destination.mem.size;
  gpuImageCopy.srcFormat = unsigned(source.format); gpuImageCopy.dstFormat = unsigned(destination.format);
  gpuImageCopy.srcMip = src.mipLevel; gpuImageCopy.dstMip = dst.mipLevel;
  gpuImageCopy.srcOrigin[0] = src.origin.x; gpuImageCopy.srcOrigin[1] = src.origin.y; gpuImageCopy.srcOrigin[2] = src.origin.z;
  gpuImageCopy.dstOrigin[0] = dst.origin.x; gpuImageCopy.dstOrigin[1] = dst.origin.y; gpuImageCopy.dstOrigin[2] = dst.origin.z;
  copy_label(gpuImageCopy.source, source.label.c_str());
  copy_label(gpuImageCopy.destination, destination.label.c_str());
  gpuImageCopy.extent[0] = gpuImageCopy.extent[1] = gpuImageCopy.extent[2] = 0;
  gpuImageCopy.srcPitch = gpuImageCopy.srcRows = 0;  // image copies carry no staging layout
}
// RL-NX: a buffer-to-image upload. Same record as the copy above, from a staging buffer instead of an
// image: a level load sends these by the hundred, and the hang that ends a menu-chosen mission lands in
// that burst (docs/PERF_HANDOFF.md 9.22).
void gpu_operation_buffer_image_copy(const Tex &destination, uint64_t sourceAddress, uint32_t sourceBytes,
                                     uint32_t rowPitch, uint32_t rows, const WGPUTexelCopyTextureInfo &dst,
                                     const WGPUExtent3D &extent) {
  gpuImageCopy = {};
  gpuImageCopy.serial = gpuOperationSerial;
  gpuImageCopy.srcAddress = sourceAddress;
  gpuImageCopy.srcSize = sourceBytes;
  gpuImageCopy.srcFormat = 0;  // a staging buffer has no format or mip: the layout is printed instead
  gpuImageCopy.srcMip = 0;
  gpuImageCopy.srcPitch = rowPitch;
  gpuImageCopy.srcRows = rows;
  gpuImageCopy.dstAddress = dkImageGetGpuAddr(&destination.image);
  gpuImageCopy.dstSize = destination.mem.size;
  gpuImageCopy.dstFormat = unsigned(destination.format);
  gpuImageCopy.dstMip = dst.mipLevel;
  gpuImageCopy.dstOrigin[0] = dst.origin.x;
  gpuImageCopy.dstOrigin[1] = dst.origin.y;
  gpuImageCopy.dstOrigin[2] = dst.origin.z;
  gpuImageCopy.extent[0] = extent.width;
  gpuImageCopy.extent[1] = extent.height;
  gpuImageCopy.extent[2] = extent.depthOrArrayLayers;
  copy_label(gpuImageCopy.source, "upload buffer");
  copy_label(gpuImageCopy.destination, destination.label.c_str());
}
#ifdef COS_DK_GPU_FLUSHES
void gpu_operation_queue_submitted() {
  // Submit the short tail too: a caller can enqueue fewer than the limit and
  // then wait for a map/readback before producing any further GPU operations.
  if (!steady_v9() && !R.synchronous) return;  // native: no tail to flush (see steady_v9)
  std::lock_guard<std::mutex> lock(fence_mutex());
  dkQueueFlush(R.queue);
  gpuOperationsPending = 0;
  gpuProgressSerial = gpuOperationSerial;
}
#endif
void log_gpu_operation_context() {
#ifdef COS_DK_GPU_BREADCRUMBS
  log_gpu_breadcrumbs();
#endif
#ifdef COS_DK_GPU_CHECKPOINTS
  const char *progress = "completed";
#else
  const char *progress = "flushed";
#endif
  std::fprintf(stderr, "[gpu-operation] serial=%llu %s=%llu op=%s args=%llu/%llu/%llu\n",
      (unsigned long long)gpuOperationSerial, progress, (unsigned long long)gpuProgressSerial, gpuOperation,
      (unsigned long long)gpuArgs[0], (unsigned long long)gpuArgs[1], (unsigned long long)gpuArgs[2]);
  const auto &copy = gpuImageCopy;
  std::fprintf(stderr, "[gpu-image-copy] serial=%llu source=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u destination=%s addr=0x%llx bytes=%u format=%u mip=%u origin=%u/%u/%u\n",
      (unsigned long long)copy.serial, copy.source, (unsigned long long)copy.srcAddress, copy.srcSize,
      copy.srcFormat, copy.srcMip, copy.srcOrigin[0], copy.srcOrigin[1], copy.srcOrigin[2], copy.destination,
      (unsigned long long)copy.dstAddress, copy.dstSize, copy.dstFormat, copy.dstMip,
      copy.dstOrigin[0], copy.dstOrigin[1], copy.dstOrigin[2]);
  for (uint64_t serial = gpuOperationSerial > gpuRecent.size() ? gpuOperationSerial - gpuRecent.size() + 1 : 1;
       serial <= gpuOperationSerial; ++serial) {
    const auto &op = gpuRecent[serial % gpuRecent.size()];
    std::fprintf(stderr, "[gpu-operation-recent] serial=%llu op=%s args=%llu/%llu/%llu\n",
        (unsigned long long)op.serial, op.name, (unsigned long long)op.args[0],
        (unsigned long long)op.args[1], (unsigned long long)op.args[2]);
  }
}
#endif
// The entry with binding number `b` in a bound group, or null.
Binding *binding(BoundGroup &g, uint32_t b) {
  if (g.group)
    for (auto &e : g.group->entries)
      if (e.binding == b)
        return &e;
  return nullptr;
}
// A buffer binding's offset plus its dynamic offset, if it has one.
uint64_t offset(BoundGroup &g, Binding &b) {
  return b.offset +
         (b.dynamic >= 0 && size_t(b.dynamic) < g.offsets.size() ? g.offsets[b.dynamic] : 0);
}
namespace {
Block immediateFallback;
} // namespace
// Address of the uniform block holding the current immediates. A new value gets its own slot of the
// frame's stream, so a draw can never read a later draw's bytes. If the stream is full, one
// persistent block is rewritten with dkCmdBufPushConstants instead, which the GPU applies in
// command order. Called from bind_stage, during replay.
DkGpuAddr current_immediates() {
  if (state.immediateGpu && !state.immediatesDirty)
    return state.immediateGpu;
  if (auto s = stream_alloc(kImmediateSlotBytes, DK_UNIFORM_BUF_ALIGNMENT)) {
    std::memcpy(s.cpu, state.immediates.data(), kImmediateBytes);
    state.immediateGpu = s.gpu;
  } else {
    if (!immediateFallback) {
      immediateFallback = block_create(kImmediateSlotBytes, "draw immediates");
      if (!immediateFallback)
        fatal("Cannot allocate the draw immediates block");
    }
    dkCmdBufPushConstants(R.cmd, immediateFallback.gpu, kImmediateSlotBytes, 0, kImmediateBytes,
                          state.immediates.data());
    state.immediateGpu = immediateFallback.gpu;
  }
  state.immediatesDirty = false;
  return state.immediateGpu;
}
// Binds everything one shader stage reads (uniform and storage buffers, textures with their samplers,
// immediates) to its native slots. BindingCache skips binds whose value did not change since the last draw.
void bind_stage(Stage &s, DkStage stage) {
  const uint32_t cacheStage = stage == DkStage_Vertex ? 0 : 1;
  for (auto &slot : s.slots) {
    if (slot.kind == SlotBinding::Kind::Immediates) {
      const DkGpuAddr address = current_immediates();
      if (state.bindings.uniform(cacheStage, slot.slot, address, kImmediateSlotBytes))
        dkCmdBufBindUniformBuffer(R.cmd, stage, slot.slot, address, kImmediateSlotBytes);
      continue;
    }
    if (slot.group >= state.groups.size())
      fatal("Invalid bind group");
    auto &g = state.groups[slot.group];
    auto *b = binding(g, slot.binding);
    if (slot.kind == SlotBinding::Kind::Sampler) {
      auto *sampler = slot.samplerGroup < state.groups.size()
                          ? binding(state.groups[slot.samplerGroup], slot.samplerBinding)
                          : nullptr;
      const DkResHandle h =
          texture_handle(b ? find_view(b->view.Get()) : nullptr,
                         sampler ? find_sampler(sampler->sampler.Get()) : nullptr);
      if (state.bindings.texture(cacheStage, slot.slot, h))
        dkCmdBufBindTextures(R.cmd, stage, slot.slot, &h, 1);
    } else if (b && b->buffer) {
      uint64_t off = offset(g, *b);
      if (off > b->buffer->size)
        fatal("Buffer binding offset out of bounds");
      uint64_t n = std::min(b->size, b->buffer->size - off);
      if (slot.kind == SlotBinding::Kind::Uniform &&
          state.bindings.uniform(cacheStage, slot.slot, b->buffer->mem.gpu + off, n))
        dkCmdBufBindUniformBuffer(R.cmd, stage, slot.slot, b->buffer->mem.gpu + off, n);
      else if (slot.kind == SlotBinding::Kind::Storage &&
               state.bindings.storage(cacheStage, slot.slot, b->buffer->mem.gpu + off, n))
        dkCmdBufBindStorageBuffer(R.cmd, stage, slot.slot, b->buffer->mem.gpu + off, n);
    } else if (slot.kind != SlotBinding::Kind::Immediates)
      fatal("Missing buffer binding %u:%u", slot.group, slot.binding);
  }
}
// Applies the current pipeline's shaders and fixed-function state when it changed, then binds both
// stages' resources. Returns false when a shader is not compiled yet (the draw is then skipped).
bool bind_pipeline() {
  auto p = state.pipeline;
  if (!p)
    fatal("Draw without pipeline");
  // SFA-NX: a stage still in uam's queue (or one uam refused) skips the draw, as the async compile
  // in backend_pipelines.cpp expects. Both stages are asked so each loads as soon as it is ready and
  // counts its skipped draws for the worker's queue order.
  const bool vsReady = shader_usable(p->vs.shader);
  const bool fsReady = shader_usable(p->fs.shader);
  if (!vsReady || !fsReady)
    return false;
  p->vs.shader->usedDraws.fetch_add(1, std::memory_order_relaxed);
  p->fs.shader->usedDraws.fetch_add(1, std::memory_order_relaxed);
  code_make_visible();
  if (state.boundPipeline != p.get()) {
    // Pipeline binds and any fixed/native helper may change driver-side state.
    // A pass reset clears these too; never carry bindings across render passes.
    state.bindings.invalidate();
    const DkShader *shaders[] = {&p->vs.shader->shader, &p->fs.shader->shader};
    dkCmdBufBindShaders(R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindRasterizerState(R.cmd, &p->raster);
    dkCmdBufBindDepthStencilState(R.cmd, &p->depth);
    dkCmdBufBindColorState(R.cmd, &p->color);
    dkCmdBufBindColorWriteState(R.cmd, &p->write);
    // The vendored deko3d (switch/third_party/deko3d, patch 1) writes the alpha destination factor
    // correctly, so the blend state binds as it is.
    dkCmdBufBindBlendStates(R.cmd, 0, &p->blend, 1);
    dkCmdBufSetDepthBias(R.cmd, p->bias[0], p->bias[1], p->bias[2]);
    dkCmdBufBindVtxAttribState(R.cmd, p->attributes.empty() ? nullptr : p->attributes.data(),
                               p->attributes.size());
    dkCmdBufBindVtxBufferState(R.cmd, p->vertexStates.empty() ? nullptr : p->vertexStates.data(),
                               p->vertexStates.size());
    state.boundPipeline = p.get();
  }
  bind_stage(p->vs, DkStage_Vertex);
  bind_stage(p->fs, DkStage_Fragment);
  return true;
}
#ifdef COS_DK_DEBUG_LIB
#include "gx_probe.inc"
#endif
// RL-NX: did the current pass open with a depth clear? Set in begin_pass_native, read when the game's own depth
// clear quad is seen, so a repair can tell whether the buffer was already empty.
static bool s_passClearedDepth = false;
// RL-NX (R2_CLEAR_LOG): the z range of the viewport the pass was last given, kept for the depth-clear quad's log
// below. The pass-level clear value is logged once per distinct value and is always 0.000. The clear *quad* writes
// the viewport's z range (clear::render sets min == max == clear_depth_value()), so this is the number that names
// it - and deko passes the range straight into DkViewport, so a collapsed range really does write that value.
static float s_viewportDepthNear = 0.f, s_viewportDepthFar = 1.f;
// Issues one draw during replay: binds the pipeline (or skips the draw), then draws once.
void draw(bool indexed, uint32_t count, uint32_t instances, uint32_t first, int32_t base,
          uint32_t firstInstance) {
  GpuCheckpoint checkpoint(indexed ? "indexed draw" : "draw", count, instances, first);
  // RL-NX (R2_SKIP_DRAW_RANGE=a:b): drop draws a..b of every pass - a bisection tool for "which draw leaves the
  // depth buffer holding one value". The readback names a frame, not a draw, and run 3 (9.30) showed the writer
  // is one draw inside the biggest pass that puts every pixel of the 640x450 picture at 1.0. Halving the range
  // over a few launches names it with no rebuild, because boot.cfg is read at launch. Diagnostic only.
  {
    static const char *range = getenv("R2_SKIP_DRAW_RANGE");
    if (range) {
      static const unsigned long long from = std::strtoull(range, nullptr, 10);
      static const char *colon = std::strchr(range, ':');
      static const unsigned long long to = colon ? std::strtoull(colon + 1, nullptr, 10) : ~0ull;
      const unsigned long long index = kd::state.draws;
      if (index >= from && index < to) {
        diag_draw(false, 0, false, 0);
        return;
      }
    }
  }
  if (!bind_pipeline()) {
    diag_draw(false, 0, false, 0);  // RL-NX: a draw the conversion skipped, not one the game failed to send
    return;
  }
  // RL-NX (R2_SKIP_CLEAR_DRAW=1): the game clears the EFB with a *quad drawn by the "EFB Clear Pipeline (...)"*
  // rather than with a pass-level clear, so this draw is the only place a frame's depth clear can be dropped.
  // Dropping it leaves the previous frame's depth, which in this port's reversed depth (GreaterEqual, 0 =
  // nearest) rejects everything the camera has moved past - the flicker's recorded picture. It models the
  // hardware fault so a deliberate frame can be matched against the videos and a fix tested against it.
  // Diagnostic only: the colour clear the same draw may carry is dropped with it, which costs the frame its
  // previous colour but not the geometry that matters.
  {
    static const bool skipClearDraw = getenv("R2_SKIP_CLEAR_DRAW") != nullptr;
    // RL-NX (R2_DEPTH_POISON_AFTER_CLEAR=1): the game's depth clear is a quad drawn inside the pass, so a
    // poison written at pass begin is overwritten by it - which is why the first poison attempt changed
    // nothing. This writes far depth (1.0) at the first draw *after* the clear quad, which is what a stale
    // buffer looks like to the world's draws, and should remove exactly what the depth test rejects while the
    // undepth-tested HUD, sprites and starfield stay. Diagnostic only.
    // The value is a stride: 1 (or any non-number) poisons every frame, n poisons every n-th frame. A stride of
    // 2 alternates broken and good frames *inside one run*, which is the only comparison that survives the demo
    // advancing between runs - and the break trigger then dumps exactly such a pair.
    static const char *poisonEnv = getenv("R2_DEPTH_POISON_AFTER_CLEAR");
    static const uint64_t poisonStride = poisonEnv ? std::strtoull(poisonEnv, nullptr, 10) : 0;
    static const bool poisonAfterClear = poisonEnv != nullptr;
    // RL-NX (R2_DEPTH_CLEAR_AFTER_CLEAR=1): the same point, the other way - clear the depth buffer right after
    // the game's own depth clear quad instead of trusting it. If that quad is the step a frame loses (a draw
    // that never rasterises, a state it inherits wrong), the buffer stays stale and the world's depth-tested
    // geometry is rejected; this re-establishes the empty buffer at exactly the moment the world needs it.
    static const bool clearAfterClear = getenv("R2_DEPTH_CLEAR_AFTER_CLEAR") != nullptr;
    static bool pendingPoison = false;
    static bool fullTargetClear = false;  // did the game's clear quad cover the whole target?
    static DkScissor scissor{};  // and the rect it used, for the log
    const bool isDepthClear = state.pipeline &&
                              state.pipeline->label.find("EFB Clear Pipeline") != std::string::npos &&
                              state.pipeline->label.find("depth true") != std::string::npos;
    const uint64_t poisonFrame = swdk::R.frame + 1;
    const bool poisonThis = poisonAfterClear && (poisonStride <= 1 || poisonFrame % poisonStride == 0);
    if (isDepthClear) {
      // RL-NX (R2_CLEAR_LOG=1): every frame's depth-clear quad, with the depth it writes and the rect it covers.
      // Run 2 (11252026-0924) dumped the world's depth on frames whose world was missing: one constant over
      // exactly the world's rect. Run 3 (9.30) then decoded that constant properly - 1.0, not 0.502 - and showed
      // the quad is not its source: the quad writes 0.0 and the repair re-clears the image to 0.0 after it. The
      // line stays because it is the only record of what the game asks a clear to write, and a run where the
      // quad starts writing 1.0 would be a different fault with the same picture.
      static const bool clearLog = getenv("R2_CLEAR_LOG") != nullptr;
      if (clearLog) {
        // RL-NX: every quad of the frame, not only the first one. Run 4 (9.31) showed a level frame carrying
        // several depth-clear quads - the one that got logged was `0,32 256x256`, a shadow target's - so the
        // world's own quad was never printed and a quad writing 0.5 over the picture's rect was invisible.
        static uint64_t loggedFrame = ~0ull;
        static unsigned loggedInFrame = 0;
        if (loggedFrame != swdk::R.frame) {
          loggedFrame = swdk::R.frame;
          loggedInFrame = 0;
        }
        if (loggedInFrame < 8) {
          ++loggedInFrame;
          auto *tv = kd::find_view(state.target.Get());
          swdk::dklog("clear: frame %llu quad %u depth z %.4f (viewport z %.4f..%.4f), %u,%u %ux%u of %ux%u, "
                      "into %s %ux%u#%u, pass cleared depth %d",
                      (unsigned long long)(swdk::R.frame + 1), loggedInFrame, s_viewportDepthNear,
                      s_viewportDepthNear, s_viewportDepthFar, state.scissor.x, state.scissor.y,
                      state.scissor.width, state.scissor.height, uint32_t(state.viewport.width),
                      uint32_t(state.viewport.height), tv ? tv->tex->label.c_str() : "?",
                      tv ? tv->tex->width : 0u, tv ? tv->tex->height : 0u, tv ? tv->tex->id : 0u,
                      int(s_passClearedDepth));
        }
      }
      // deko3d has only a whole-image clear, so the repair must know whether the quad it stands in for
      // covered the whole target: re-clearing a scissored clear would erase depth the game meant to keep.
      const auto &s = state.scissor;
      scissor = s;
      // The game's picture is 640x450 inside the 640x480 EFB, so its clear quad is `0,16 640x450` against a
      // 640x480 viewport - a full-picture clear with the 30-row overscan left out. Full width and no more than
      // the overscan short of the height is therefore what "covers the target" means here; anything smaller is
      // a deliberately partial clear that a whole-image clear must not widen.
      fullTargetClear = s.x == 0 && s.width == uint32_t(state.viewport.width) &&
                        s.height + 32 >= uint32_t(state.viewport.height);
      pendingPoison = poisonThis || clearAfterClear;
    } else if (pendingPoison) {
      pendingPoison = false;
      // 0 is the value the game's own clear uses and the empty buffer in this port's reversed depth, so the
      // clear arm restores what the quad was supposed to write; the poison arm writes the opposite (far). With
      // both set the frame is poisoned first and then repaired, which shows the repair works on a broken one.
      if (poisonThis)
        dkCmdBufClearDepthStencil(R.cmd, true, 1.0f, 0, 0);
      if (clearAfterClear && fullTargetClear)
        dkCmdBufClearDepthStencil(R.cmd, true, 0.0f, 0, 0);
      static unsigned done = 0;
      if (++done <= 6)
        swdk::dklog("depth: %s%s%s after the clear quad (%u so far; scissor %u,%u %ux%u, viewport %ux%u, pass "
                    "cleared depth %d)",
                    poisonAfterClear ? "poisoned to 1.0" : "",
                    poisonAfterClear && clearAfterClear ? " then " : "",
                    clearAfterClear ? (fullTargetClear ? "re-cleared to 0.0"
                                                       : "NOT re-cleared: the quad was scissored")
                                    : "",
                    done, scissor.x, scissor.y, scissor.width, scissor.height,
                    uint32_t(state.viewport.width), uint32_t(state.viewport.height), int(s_passClearedDepth));
    }
    if (skipClearDraw && isDepthClear) {
      diag_draw(false, 0, false, 0);
      static unsigned dropped = 0;
      if (++dropped <= 3) {
        // The target is named: a drop only matters if it is the world pass's own depth (the pass whose
        // colour target the frame line lists as EFB 2/Render texture), not an offscreen one.
        auto *tv = kd::find_view(state.target.Get());
        swdk::dklog("clear: dropped the depth clear draw %u (%s %ux%u#%u, R2_SKIP_CLEAR_DRAW)", dropped,
                    tv ? tv->tex->label.c_str() : "?", tv ? tv->tex->width : 0, tv ? tv->tex->height : 0,
                    tv ? tv->tex->id : 0u);
      }
      return;
    }
  }
  // RL-NX diagnostic (R2_DRAW_LOG): the uniform data the GPU is given for a few draws, after the game
  // has been running long enough to be past the fade-in. What a 3D draw's model-view and projection
  // matrices actually are: a picture 28 rows tall in a 640x480 frame is a vertical scale near 1/16, and
  // this says whether that scale is in the data bound here or is a shader's reading of it.
  {
    static const bool dumpUniforms = getenv("R2_DRAW_LOG") != nullptr;
    if (dumpUniforms) {
      static int logged = 0;
      static bool late = false;
      static const uint64_t start = now_ns();
      if (!late && now_ns() - start > 25'000'000'000ull)
        late = true;
      if (late && logged < 12) {
        for (size_t i = 0; i < state.groups.size(); ++i) {
          auto &[group, dyn] = state.groups[i];
          if (!group)
            continue;
          for (const auto &e : group->entries) {
            if (!e.buffer || e.buffer->size < 64 || !e.buffer->mem.cpu)
              continue;
            const uint64_t dynOff = e.dynamic >= 0 && size_t(e.dynamic) < dyn.size() ? dyn[e.dynamic] : 0;
            const uint64_t at = e.offset + dynOff;
            if (at + 32 > e.buffer->size)
              continue;
            const float *f = reinterpret_cast<const float *>(static_cast<const uint8_t *>(e.buffer->mem.cpu) + at);
            const uint64_t room = (e.buffer->size - at) / 4;
            ++logged;
            dklog("draw %llu uniforms b%u size %llu at %llu: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                  (unsigned long long)state.draws, e.binding, (unsigned long long)e.buffer->size,
                  (unsigned long long)at, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            // RL-NX: the whole first half kilobyte of the big uniform pool, eight floats a line: the
            // matrices a 3D draw is given. A vertical scale near 1/16 shows up as a row of a matrix here
            // if the data is what is wrong, and its absence says the vertex stage is reading it wrong.
            if (logged == 1 && room >= 128) {
              for (unsigned r = 0; r < 16; ++r) {
                const float *row = f + r * 8;
                dklog("  m%02u: %8.4f %8.4f %8.4f %8.4f | %8.4f %8.4f %8.4f %8.4f", r, row[0], row[1], row[2],
                      row[3], row[4], row[5], row[6], row[7]);
              }
            }
            if (logged >= 12)
              return;
          }
        }
      }
    }
  }
#ifdef COS_DK_DEBUG_LIB
  // Probe before the real draw: the depth comparison must see the same depth
  // image the real draw will see, rather than its own already-written depth.
  probe_draw(indexed, count, instances, first, base, firstInstance);
  bind_pipeline();
#endif
  ++state.draws;
  // RL-NX (R2_DEKO_FRAME_LOG): the two states that decide whether a fragment can paint anything at all - the
  // depth test, and target 0's blend mode as a tag. A pass whose tag changes between frames is a leaked blend
  // mode, and subtract-with-zero-alpha (op 2, factors 01/01) paints nothing while still writing depth.
  unsigned blendTag = 0;
  if (state.pipeline && (state.pipeline->color.blendEnableMask & 1)) {
    const DkBlendState &bs = state.pipeline->blend;
    blendTag = ((unsigned(bs.colorBlendOp) & 7u) << 12) | ((unsigned(bs.srcColorBlendFactor) & 0x3fu) << 6) |
               (unsigned(bs.dstColorBlendFactor) & 0x3fu);
    if (!blendTag)
      blendTag = 1;  // blending on, all-zero code: still distinct from off
    // The colour write mask of target 0, in the tag's top nibble: a "blended" draw that writes no colour is a
    // depth or alpha-only pass and cannot paint anything either way, so the mask decides how to read the mode.
    blendTag |= (unsigned(state.pipeline->write.masks) & 0xFu) << 16;
  }
  diag_draw(true, blendTag, state.pipeline ? bool(state.pipeline->depth.depthTestEnable) : false,
            state.pipeline ? unsigned(state.pipeline->depth.depthCompareOp) : 0u);
  auto p = state.pipeline;
  // SFA-NX: per-draw GPU cost probe (gpu_timing.cpp): on a probe frame, a stamp before the draw (after its
  // binds, so state changes are not counted) and one after it below
  const bool probe = gpu_timing_draw_sampling();
  if (probe)
    gpu_timing_draw_begin();
  auto issue = [&] {
    if (indexed) {
      if (!state.index)
        fatal("Missing index buffer");
      if (state.bindings.index(state.index->mem.gpu + state.indexOffset, state.indexFormat))
        dkCmdBufBindIdxBuffer(R.cmd, state.indexFormat, state.index->mem.gpu + state.indexOffset);
      dkCmdBufDrawIndexed(R.cmd, p->topology, count, instances, first, base, firstInstance);
    } else
      dkCmdBufDraw(R.cmd, p->topology, count, instances, first, firstInstance);
  };
  // RL-NX (R2_DRAW_REPEAT=<n>): issues every draw n times, which makes the GPU - not the CPU - the bottleneck
  // in the emulator. The console runs this level at 13.7 fps with the GPU behind, and every timing theory here
  // (a frame's depth or colour still in use while the next frame writes it, a fence taken too early) is
  // invisible while the emulator's GPU is idle: this is the only way to put the port in the console's
  // condition without a console. The picture must be identical at any repeat factor (the same draw, same
  // state, overwritten), so a frame that changes with it is a frame that depends on GPU timing.
  {
    static const char *repeatEnv = getenv("R2_DRAW_REPEAT");
    static const uint64_t repeat = repeatEnv ? std::max<uint64_t>(1, std::strtoull(repeatEnv, nullptr, 10)) : 1;
    for (uint64_t i = 0; i < repeat; ++i)
      issue();
  }
  // SFA-NX: the probe's second stamp, with what identifies the draw: shaders, size, blend and depth state,
  // viewport, and the first texture the fragment stage samples
  if (probe) {
    GpuDrawInfo info;
    info.vsHash = p->vs.shader ? p->vs.shader->glslHash : 0;
    info.fsHash = p->fs.shader ? p->fs.shader->glslHash : 0;
    info.count = count;
    info.instances = instances;
    info.indexed = indexed;
    info.blend = (p->color.blendEnableMask & 1) != 0;
    info.depthTest = p->depth.depthTestEnable;
    info.depthWrite = p->depth.depthWriteEnable;
    info.viewportW = uint32_t(state.viewport.width);
    info.viewportH = uint32_t(state.viewport.height);
    for (auto &slot : p->fs.slots) {
      if (slot.kind != SlotBinding::Kind::Sampler || slot.group >= state.groups.size())
        continue;
      auto *b = binding(state.groups[slot.group], slot.binding);
      auto *v = b ? find_view(b->view.Get()) : nullptr;
      info.tex = v ? v->tex : nullptr;
      break;
    }
    gpu_timing_draw_end(info);
  }
#ifdef COS_DK_DEBUG_LIB
  probe_after_draw();
#endif
  checkpoint.finish();
}
} // namespace kd

// Called when a submit fails or the GPU times out: logs the diagnostics above plus the draw state of the
// command being replayed. Never blocks on the locks (the failure may come from inside them).
extern "C" void kartpad_deko_log_failure_context() {
  // A driver failure may arrive while the fence mutex is held. Avoid waiting
  // for the object mutex in the reverse order on this diagnostic path.
  std::unique_lock queueLock(kd::queueMutex, std::try_to_lock);
  std::unique_lock lock(kd::mutex, std::try_to_lock);
  if (!queueLock.owns_lock() || !lock.owns_lock()) {
    std::fprintf(stderr, "[gpu-submit] replay context busy\n");
    return;
  }
  const auto stats = swdk::submit_stats();
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
  kd::log_gpu_operation_context();
#endif
  const auto pipeline = kd::state.pipeline;
  const auto *view = kd::state.target ? kd::find_view(kd::state.target.Get()) : nullptr;
  const auto *texture = view ? view->tex : nullptr;
  std::fprintf(stderr,
      "[gpu-submit] frame=%llu submits=%llu pass_draws=%llu pipeline=%s "
      "vs=%016llx fs=%016llx target=%s (%ux%u) buffers=%zu pipelines=%zu\n",
      static_cast<unsigned long long>(swdk::R.frame + 1),
      static_cast<unsigned long long>(stats.submits),
      static_cast<unsigned long long>(kd::state.draws), pipeline ? pipeline->label.c_str() : "<none>",
      static_cast<unsigned long long>(pipeline && pipeline->vs.shader ? pipeline->vs.shader->glslHash : 0),
      static_cast<unsigned long long>(pipeline && pipeline->fs.shader ? pipeline->fs.shader->glslHash : 0),
      texture ? texture->label.c_str() : "<none>", texture ? texture->width : 0,
      texture ? texture->height : 0, kd::buffers.size(), kd::pipelines.size());
}

// RL-NX: called by the release deko3d library just before it aborts (switch/third_party/deko3d dk_device.cpp).
// The hardware crashes of 2026-11-25 were all "queue in error state" raised inside a mid-frame submit, i.e. an
// earlier command faulted the GPU, and the release library wrote nothing. This logs which GPU operations had
// and had not completed (the breadcrumbs read GPU-written memory and take no lock) and flushes the log to the
// card synchronously. It must not take kd::mutex or the queue lock: the failing submit already holds them.
extern "C" void kartpad_log_flush_sync(void);
extern "C" void rlnx_deko_release_error(int result) {
  static bool entered = false;  // a second error while logging the first: let the abort happen
  if (entered) return;
  entered = true;
  std::fprintf(stderr, "[dk] FATAL: deko3d release error %d (queue %s) in frame %llu\n", result,
               swdk::R.queue && dkQueueIsInErrorState(swdk::R.queue) ? "in error state: a GPU fault"
                                                                      : "not in error state",
               static_cast<unsigned long long>(swdk::R.frame + 1));
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
  kd::log_gpu_operation_context();
#endif
  swdk::log_flush();
  kartpad_log_flush_sync();
}

extern "C" {
// Encoders and command buffers: each Dawn handle gets a native command list (CommandArena of closures),
// reference-counted alongside the Dawn object. Finish hands the encoder's list to the command buffer.
WGPUCommandEncoder __wrap_wgpuDeviceCreateCommandEncoder(WGPUDevice d,
                                                         const WGPUCommandEncoderDescriptor *desc) {
  auto h = __real_wgpuDeviceCreateCommandEncoder(d, desc);
  kd::Lock l(kd::mutex);
  kd::encoders[h] = std::make_shared<kd::Commands>();
  kd::encoderRefs[h] = 1;
  return h;
}
void __wrap_wgpuCommandEncoderAddRef(WGPUCommandEncoder h) {
  kd::Lock l(kd::mutex);
  ++kd::encoderRefs[h];
  __real_wgpuCommandEncoderAddRef(h);
}
void __wrap_wgpuCommandEncoderRelease(WGPUCommandEncoder h) {
  kd::Lock l(kd::mutex);
  if (!--kd::encoderRefs[h]) {
    kd::encoders.erase(h);
    kd::encoderRefs.erase(h);
  }
  __real_wgpuCommandEncoderRelease(h);
}
WGPUCommandBuffer __wrap_wgpuCommandEncoderFinish(WGPUCommandEncoder h,
                                                  const WGPUCommandBufferDescriptor *desc) {
  auto b = __real_wgpuCommandEncoderFinish(h, desc);
  kd::Lock l(kd::mutex);
  kd::commands[b] = {kd::required(kd::encoders, h, __func__), 1};
  return b;
}
void __wrap_wgpuCommandBufferAddRef(WGPUCommandBuffer h) {
  kd::Lock l(kd::mutex);
  ++kd::required(kd::commands, h, __func__).second;
  __real_wgpuCommandBufferAddRef(h);
}
void __wrap_wgpuCommandBufferRelease(WGPUCommandBuffer h) {
  kd::Lock l(kd::mutex);
  auto it = kd::commands.find(h);
  if (it != kd::commands.end() && !--it->second.second)
    kd::commands.erase(it);
  __real_wgpuCommandBufferRelease(h);
}
WGPURenderPassEncoder
// A render pass records into its encoder's list; begin and end become replay closures.
__wrap_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e,
                                         const WGPURenderPassDescriptor *desc) {
  auto p = __real_wgpuCommandEncoderBeginRenderPass(e, desc);
  kd::Lock l(kd::mutex);
  auto view = kd::keep(desc->colorAttachments[0].view);
  kd::passes[p] = {kd::required(kd::encoders, e, __func__), view, kd::str(desc->label)};
  auto depth = desc->depthStencilAttachment ? kd::keep(desc->depthStencilAttachment->view)
                                            : wgpu::TextureView{};
  auto color = desc->colorAttachments[0].clearValue;
  bool clear = desc->colorAttachments[0].loadOp == WGPULoadOp_Clear;
  bool clearDepth =
      desc->depthStencilAttachment && desc->depthStencilAttachment->depthLoadOp == WGPULoadOp_Clear;
  float z = desc->depthStencilAttachment ? desc->depthStencilAttachment->depthClearValue : 1;
  const std::array<float, 4> c{float(color.r), float(color.g), float(color.b), float(color.a)};
  kd::record(e, [view, depth, c, clear, clearDepth, z] {
    kd::begin_pass_native(view, depth, clear, c, clearDepth, z);
  });
  return p;
}
void __wrap_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder p) {
  kd::Lock l(kd::mutex);
  const auto &pass = kd::required(kd::passes, p, __func__);
  auto target = pass.target;
  auto label = pass.label;
  kd::record_pass(p, [target, label] { kd::end_pass_native(target, label); });
  __real_wgpuRenderPassEncoderEnd(p);
  kd::passes.erase(p);
}
} // extern "C"

namespace kd {
// Replay of a pass start: barrier against the previous pass, bind the targets, reset the draw state to
// full-target viewport and scissor, and clear what the pass asked to clear.
void begin_pass_native(const wgpu::TextureView &view, const wgpu::TextureView &depth, bool clear,
                       const std::array<float, 4> &color, bool clearDepth, float z) {
  GpuCheckpoint checkpoint("begin render pass");
  open();
  barrier();
  gpu_timing_pass_begin();
  state = {};
  auto *v = find_view(view.Get());
  auto *d = depth ? find_view(depth.Get()) : nullptr;
  if (!v || !v->tex->valid)
    swdk::fatal("Invalid native render target");
  const DkImageView *targets[] = {&v->view};
  dkCmdBufBindRenderTargets(swdk::R.cmd, targets, 1, d ? &d->view : nullptr);
  swdk::set_view(0, 0, v->tex->width >> v->baseMip, v->tex->height >> v->baseMip);
  // RL-NX diagnostic (R2_DRAW_LOG): every pass of the frame with the texture it draws into. The presented
  // image holding 30 rows of 480 means either the game draws the frame into a target other than the one
  // that is presented, or the presented target only receives part of it - this says which.
  if (getenv("R2_DRAW_LOG")) {
    static int passesLogged = 0;
    if (passesLogged < 40) {
      ++passesLogged;
      swdk::dklog("pass into %s %ux%u (mip %u), clear %d", v->tex->label.c_str(), v->tex->width, v->tex->height,
                  v->baseMip, int(clear));
    }
  }
  state.target = view;
  state.depthTarget = depth;
  state.viewport = {0, 0, float(v->tex->width >> v->baseMip), float(v->tex->height >> v->baseMip), 0, 1};
  state.scissor = {0, 0, v->tex->width >> v->baseMip, v->tex->height >> v->baseMip};
  if (clear)
    dkCmdBufClearColor(swdk::R.cmd, 0, DkColorMask_RGBA, color.data());
  // RL-NX (R2_SKIP_DEPTH_CLEAR): the flicker's picture is what a frame draws when its depth buffer was never
  // cleared - the world (depth-tested) fails against the previous frame's depth while the starfield, sprites
  // and HUD (no depth test) still draw. This switch reproduces exactly that, to match a flicker frame against
  // a deliberate one (docs/PERF_HANDOFF.md 9.27). Never set in a normal run.
  // RL-NX (R2_FRAME_DEPTH_CLEAR): the same reasoning as a candidate fix. If the frame's first pass into a
  // depth target loads instead of clearing, the target still holds the previous frame's depth and every
  // depth-tested draw of this frame can fail against it - one frame of world gone, HUD, sprites, starfield and
  // the radar's own render texture untouched, which is what the flicker recordings show. The scene target's
  // depth is therefore cleared once per frame here whatever the pass asked for (its own clear still runs and
  // is then redundant). Behind a switch until a console boot proves it (9.27).
  static const bool frameDepthClear = getenv("R2_FRAME_DEPTH_CLEAR") != nullptr;
  // RL-NX: `R2_FRAME_DEPTH_CLEAR=all` clears at *every* pass into a depth target, not once per frame. The frame
  // draws its world twice (`EFB 2` and `EFB 4`, 632 draws each, same depth image), so a frame where only the second
  // pass's clear was lost is uncovered by the once-per-frame arm and covered by this one. The game re-clears for
  // each of those passes anyway, which is why it is safe to mirror it.
  static const bool frameDepthClearAll =
      getenv("R2_FRAME_DEPTH_CLEAR") && !strcmp(getenv("R2_FRAME_DEPTH_CLEAR"), "all");
  static const bool skipDepthClear = getenv("R2_SKIP_DEPTH_CLEAR") != nullptr;
  // RL-NX (R2_DEPTH_CLEAR_ZERO): this port's depth is reversed - the world's draws compare GreaterEqual and
  // the game clears to 0.000, so 0 is the nearest, not the far plane. A pass that asked for anything else
  // would reject every depth-tested draw of the frame while the undepth-tested stars, sprites and HUD still
  // paint, which is the flicker's picture. This forces the clear to 0 whatever the pass asked for; the
  // distinct requested values are logged once each either way.
  static const bool depthClearZero = getenv("R2_DEPTH_CLEAR_ZERO") != nullptr;
  static const DkImage *depthCleared = nullptr;
  static uint64_t depthClearedFrame = 0;
  s_passClearedDepth = false;
  if (d) {
    const uint64_t frame = swdk::R.frame + 1;
    if (depthClearedFrame != frame) {
      depthClearedFrame = frame;
      depthCleared = nullptr;
    }
    if (clearDepth && !skipDepthClear) {
      static float seenZ[4] = {};
      static int seenCount = 0;
      bool known = false;
      for (int i = 0; i < seenCount; ++i)
        known |= seenZ[i] == z;
      if (!known && seenCount < 4) {
        seenZ[seenCount++] = z;
        swdk::dklog("depth: clear value %.3f into %s (%d distinct so far; reverse depth, 0 = nearest)", z,
                    d->tex->label.c_str(), seenCount);
      }
      dkCmdBufClearDepthStencil(swdk::R.cmd, true, depthClearZero ? 0.0f : z, 0, 0);
      depthCleared = &d->tex->image;
      s_passClearedDepth = true;
    } else if (frameDepthClear && (frameDepthClearAll || depthCleared != &d->tex->image)) {
      // 0 is the *nearest* here (reverse depth: the world's draws compare GreaterEqual and the game's own
      // clear value is 0.000), so 0 is the value that leaves every draw of the frame able to pass. Clearing
      // to 1.0 would put the whole buffer at the far plane and reject all of it - the opposite of the fix.
      dkCmdBufClearDepthStencil(swdk::R.cmd, true, 0.0f, 0, 0);
      depthCleared = &d->tex->image;
      s_passClearedDepth = true;
      static unsigned added = 0;
      if (++added == 1 || added % 600 == 0)
        swdk::dklog("depth: frame %llu's first pass into %s loaded depth instead of clearing it; cleared it "
                    "here (%u times, R2_FRAME_DEPTH_CLEAR)",
                    (unsigned long long)frame, d->tex->label.c_str(), added);
    }
  }
  // RL-NX (R2_DEPTH_POISON=<n>): writes 1.0 - the far plane in this port's reversed depth - into the depth
  // buffer right after the pass cleared it, on every n-th frame. Every depth-tested draw of that frame then
  // fails while the undepth-tested stars, sprites, HUD and engine glows still paint, which is the flicker's
  // recorded picture; so a deliberately broken frame can be matched against the videos and a fix against it.
  // Diagnostic only: never set in a normal run.
  static const char *poison = getenv("R2_DEPTH_POISON");
  static const uint64_t poisonEvery = poison ? std::strtoull(poison, nullptr, 10) : 0;
  if (poisonEvery && d && clearDepth) {
    const uint64_t frame = swdk::R.frame + 1;
    if (frame % poisonEvery == 0) {
      dkCmdBufClearDepthStencil(swdk::R.cmd, true, 1.0f, 0, 0);
      depthCleared = &d->tex->image;
      static unsigned logged = 0;
      if (++logged <= 3)
        swdk::dklog("depth: poisoned frame %llu, colour %s %ux%u#%u, depth %s %ux%u#%u (forced to 1.0)",
                    (unsigned long long)frame, v->tex->label.c_str(), v->tex->width, v->tex->height,
                    v->tex->id, d->tex->label.c_str(), d->tex->width, d->tex->height, d->tex->id);
    }
  }
  // RL-NX (R2_DEKO_FRAME_LOG): the pass is open from its target bind and clears on, so a cut taken at
  // this pass's own barrier (above) reads as "between passes", which is what it is.
  diag_pass_begin();
  checkpoint.finish(true);
}
// Replay of a pass end: GPU timing stamp, and in a capture frame (deko-capture.flag) a dump of the target
// and every texture the last pipeline sampled.
void end_pass_native(const wgpu::TextureView &target, const std::string &label) {
    // RL-NX (R2_DEKO_FRAME_LOG): the pass's draw count and its target, kept for the frame's line. The
    // target is named by label, size and creation id: Aurora labels every render target "Render texture",
    // so without the other two the field cannot say which target a frame's world geometry went into.
    if (auto *tv = kd::find_view(target.Get())) {
      // RL-NX (R2_DEKO_FRAME_LOG): the depth image the pass bound, beside the colour target. A frame whose world
      // pass binds a depth image the frame's clear did not touch would be depth-rejected with nothing else
      // wrong, and until now the line could not show it: the id says whether that buffer changes between frames.
      char id[128];
      if (auto *dv = kd::state.depthTarget ? kd::find_view(kd::state.depthTarget.Get()) : nullptr)
        std::snprintf(id, sizeof id, "%s %ux%u#%u/z %s %ux%u#%u", tv->tex->label.c_str(), tv->tex->width,
                      tv->tex->height, tv->tex->id, dv->tex->label.c_str(), dv->tex->width, dv->tex->height,
                      dv->tex->id);
      else
        std::snprintf(id, sizeof id, "%s %ux%u#%u/z none", tv->tex->label.c_str(), tv->tex->width,
                      tv->tex->height, tv->tex->id);
      kd::diag_pass_end(label.c_str(), id, kd::state.draws);
      // RL-NX (R2_DEPTH_SHOT): the frame's biggest pass's depth, copied for the break trigger - the
      // recordings leave one question open and it is whether those draws ever wrote depth.
      if (kd::state.depthTarget) {
        if (auto *dv = kd::find_view(kd::state.depthTarget.Get())) {
          // RL-NX: the target's own format, so the readback decodes floats as floats. Compared against
          // deko3d's own formats, not WGPU's: the two enums share no values, so the old test against
          // WGPUTextureFormat_Depth32Float was always false and every float target was decoded as Z24 (the
          // compiler's -Wenum-compare was pointing at it all along).
          const DkImageFormat df = dv->tex->format;
          swdk::depth_shot_pass(dv->view, std::max(1u, dv->tex->width >> dv->baseMip),
                                std::max(1u, dv->tex->height >> dv->baseMip), kd::state.draws,
                                df == DkImageFormat_ZF32 || df == DkImageFormat_ZF32_X24S8);
        }
      }
    }
    kd::GpuCheckpoint checkpoint("end render pass");
    if (kd::gpu_timing_sampling() || swdk::capture_frame())
      kd::barrier();
    {
      auto *tv = kd::find_view(target.Get());
      kd::gpu_timing_pass_end(label.c_str(), tv ? tv->tex->label.c_str() : "?",
                              tv ? tv->tex->width >> tv->baseMip : 0, tv ? tv->tex->height >> tv->baseMip : 0,
                              kd::state.draws);
    }
    if (swdk::capture_frame()) {
      auto *v = kd::find_view(target.Get());
      if (v && v->tex->valid) {
        swdk::dklog("pixel pass: %s target=%s draws=%llu", label.c_str(), v->tex->label.c_str(),
                    (unsigned long long)kd::state.draws);
        swdk::capture_image(v->view, std::max(1u, v->tex->width >> v->baseMip),
                            std::max(1u, v->tex->height >> v->baseMip), v->tex->format,
                            label.c_str());
      }
      if (auto pipeline = kd::state.pipeline) {
        for (const auto *stage : {&pipeline->vs, &pipeline->fs}) {
          for (const auto &slot : stage->slots) {
            if (slot.kind != swdk::SlotBinding::Kind::Sampler)
              continue;
            auto *b = kd::binding(kd::state.groups[slot.group], slot.binding);
            auto *source = b ? kd::find_view(b->view.Get()) : nullptr;
            swdk::dklog("pixel input: %s binding=%u:%u native_slot=%u texture=%s valid=%d",
                        label.c_str(), slot.group, slot.binding, slot.slot,
                        source ? source->tex->label.c_str() : "MISSING",
                        source && source->tex->valid);
            if (source && source->tex->valid) {
              auto sourceLabel = std::string("input ") + source->tex->label;
              swdk::capture_image(source->view, std::max(1u, source->tex->width >> source->baseMip),
                                  std::max(1u, source->tex->height >> source->baseMip),
                                  source->tex->format, sourceLabel.c_str());
            }
          }
        }
      }
    }
    checkpoint.finish(true);
}
} // namespace kd

extern "C" {
// Pass state setters: each records a closure that updates `state` (or the command buffer) at replay.
void __wrap_wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline h) {
  kd::Lock l(kd::mutex);
  auto pipe = kd::required(kd::pipelines, h, __func__);
  kd::record_pass(p, [pipe] { kd::state.pipeline = pipe; });
}
void __wrap_wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, uint32_t i, WGPUBindGroup h,
                                              size_t n, const uint32_t *offsets) {
  kd::Lock l(kd::mutex);
  auto g = h ? kd::required(kd::groups, h, __func__) : nullptr;
  std::array<uint32_t, 8> o{};
  if (n > o.size())
    swdk::fatal("Too many dynamic offsets");
  if (n)
    std::copy_n(offsets, n, o.begin());
  kd::record_pass(p, [i, g, o] { kd::state.groups.at(i) = {g, o}; });
}
void __wrap_wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float w,
                                             float h, float near, float far) {
  // RL-NX: what the renderer is actually asked for, not what the guest's registers say. A picture
  // squeezed into a band is either a viewport this small or one that does not fit the target (a
  // viewport at x 660 in a 640-wide target is outside it and deko clips it away silently).
  static const bool logViewport = getenv("R2_DRAW_LOG") != nullptr;
  if (logViewport) {
    static int logs = 0;
    if (logs < 16) {
      ++logs;
      swdk::dklog("viewport %.1f %.1f %.1fx%.1f (depth %.3f..%.3f)", x, y, w, h, near, far);
    }
  }
  kd::record_pass(p, [=] {
    DkViewport v{x, y, w, h, near, far};
    kd::state.viewport = v;
    kd::s_viewportDepthNear = near;  // RL-NX (R2_CLEAR_LOG): what the depth-clear quad will write
    kd::s_viewportDepthFar = far;
    dkCmdBufSetViewports(swdk::R.cmd, 0, &v, 1);
  });
}
void __wrap_wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, uint32_t x, uint32_t y,
                                                uint32_t w, uint32_t h) {
  // RL-NX diagnostic (R2_DRAW_LOG): the clip the renderer is really given. The draws' vertices cover the
  // whole 640x480 target (R2_DRAW_BOUNDS) and the viewport is the whole target as well, so a band-shaped
  // picture can only come from the scissor (or from outside this pass).
  static const bool logScissor = getenv("R2_DRAW_LOG") != nullptr;
  if (logScissor) {
    // every distinct rectangle of the run, not just the first few: a picture that survives in one horizontal
    // band is a clip, and the clip that matters is whichever one the last draws of the frame were given.
    static std::vector<std::array<uint32_t, 4>> seen;
    const std::array<uint32_t, 4> r{x, y, w, h};
    if (std::find(seen.begin(), seen.end(), r) == seen.end()) {
      if (seen.size() < 48) {
        seen.push_back(r);
        swdk::dklog("scissor %u,%u %ux%u (distinct %zu)", x, y, w, h, seen.size());
      }
    }
  }
  kd::record_pass(p, [=] {
    DkScissor s{x, y, w, h};
    kd::state.scissor = s;
    dkCmdBufSetScissors(swdk::R.cmd, 0, &s, 1);
  });
}
void __wrap_wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, const WGPUColor *color) {
  auto c = *color;
  kd::record_pass(p, [=] { dkCmdBufSetBlendConst(swdk::R.cmd, c.r, c.g, c.b, c.a); });
}
// Aurora sets its per-draw DrawImmediateData here. Dawn's Null pass never sees the draws (they are
// recorded natively), so the call is not forwarded.
void __wrap_wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder p, uint32_t offset,
                                               const void *data, size_t size) {
  if (offset > kd::kImmediateBytes || size > kd::kImmediateBytes - offset)
    swdk::fatal("Immediate data out of range: offset %u size %zu (limit %u)", offset, size,
                kd::kImmediateBytes);
  std::array<uint8_t, kd::kImmediateBytes> bytes{};
  if (size)
    std::memcpy(bytes.data(), data, size);
  kd::record_pass(p, [bytes, offset, size] {
    std::memcpy(kd::state.immediates.data() + offset, bytes.data(), size);
    kd::state.immediatesDirty = true;
  });
}
void __wrap_wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder p, WGPUBuffer h,
                                                WGPUIndexFormat fmt, uint64_t off, uint64_t size) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  kd::record_pass(p, [=] {
    kd::state.index = b;
    kd::state.indexOffset = off;
    kd::state.indexFormat = fmt == WGPUIndexFormat_Uint32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
  });
}
void __wrap_wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder p, uint32_t slot,
                                                 WGPUBuffer h, uint64_t off, uint64_t size) {
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  if (off > b->size)
    swdk::fatal("Vertex buffer offset out of bounds");
  if (size == WGPU_WHOLE_SIZE)
    size = b->size - off;
  if (size > UINT32_MAX || size > b->size - off)
    swdk::fatal("Invalid vertex buffer size");
  kd::record_pass(p, [=] {
    DkBufExtents e{b->mem.gpu + off, uint32_t(size)};
    dkCmdBufBindVtxBuffers(swdk::R.cmd, slot, &e, 1);
  });
}
// Draws replay through kd::draw, shared with the direct renderer.
void __wrap_wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, uint32_t count, uint32_t instances,
                                      uint32_t first, uint32_t firstInstance) {
  kd::record_pass(p, [=] { kd::draw(false, count, instances, first, 0, firstInstance); });
}
void __wrap_wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder p, uint32_t count,
                                             uint32_t instances, uint32_t first, int32_t base,
                                             uint32_t firstInstance) {
  kd::record_pass(p, [=] { kd::draw(true, count, instances, first, base, firstInstance); });
}
// Replays every submitted command list into the frame's command buffer, submits it to the GPU, and
// recycles upload memory whose fences have passed. Dawn's own submit is then called for bookkeeping only.
void __wrap_wgpuQueueSubmit(WGPUQueue q, size_t n, const WGPUCommandBuffer *b) {
  // SFA-NX: the replay holds the queue lock only (backend_internal.h). The command lists are
  // looked up under the object lock and kept alive by these references meanwhile.
  std::vector<std::shared_ptr<kd::Commands>> lists;
  lists.reserve(n);
  {
    kd::QueueLock queueLock;
    {
      kd::Lock l(kd::mutex);
      for (size_t i = 0; i < n; ++i)
        lists.push_back(kd::required(kd::commands, b[i], __func__).first);
    }
    kd::open();
    for (auto &list : lists)
      list->run();
    swdk::submit("Aurora queue submit");
#ifdef COS_DK_GPU_FLUSHES
    kd::gpu_operation_queue_submitted();
#endif
    kd::retire_uploads();
  }
  kd::Lock l(kd::mutex);
  __real_wgpuQueueSubmit(q, n, b);
}

// Copies, recorded and replayed in order with the passes. Each is wrapped in barriers so it sees the
// passes before it finished and the passes after it see its result.
void __wrap_wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer src, uint64_t so,
                                                 WGPUBuffer dst, uint64_t off, uint64_t n) {
  kd::Lock l(kd::mutex);
  auto s = kd::required(kd::buffers, src, __func__), d = kd::required(kd::buffers, dst, __func__);
  // Pin the mapped generation, not a CPU copy. prepare_map() cannot hand this
  // memory to the producer again before this command and its GPU fence finish.
  auto source = s->active;
  kd::record(e, [=] {
    kd::GpuCheckpoint checkpoint("buffer copy", so, off, n);
    kd::open();
    kd::barrier();
    dkCmdBufCopyBuffer(swdk::R.cmd, source->mem.gpu + so, d->mem.gpu + off, n);
    // DMA writes can replace uniforms/storage at an address the shader has
    // already cached. L2 invalidation alone does not invalidate that cache.
    kd::barrier(DkInvalidateFlags_Shader);
    source->used();
    checkpoint.finish(true);
  });
}
} // extern "C"
namespace kd {
// Colour formats the copy shader (shaders/copy_fsh.glsl) can read and write unchanged.
static bool copy_format_ok(WGPUTextureFormat f) {
  switch (f) {
  case WGPUTextureFormat_RGBA8Unorm:
  case WGPUTextureFormat_BGRA8Unorm:
  case WGPUTextureFormat_RGB10A2Unorm:
  case WGPUTextureFormat_RG11B10Ufloat:
  case WGPUTextureFormat_RGBA16Float:
  case WGPUTextureFormat_RG16Float:
  case WGPUTextureFormat_R16Float:
  case WGPUTextureFormat_RGBA32Float:
  case WGPUTextureFormat_RG32Float:
  case WGPUTextureFormat_R32Float:
  case WGPUTextureFormat_RG8Unorm:
  case WGPUTextureFormat_R8Unorm:
    return true;
  default:
    return false;
  }
}
// SFA-NX: a copy from a render target with hardware compression (the - menu's Render compression) is done
// as a full-screen draw instead of dkCmdBufCopyImage. The image copy engine reads compressed memory
// slowly (a ~2 ms gap in the 2026-11-19 GPU timings); the 3D engine decompresses as it samples. Only
// same-size, same-format, same-position 2D colour copies qualify; everything else uses the copy engine.
static bool copy_by_draw(Tex *st, Tex *dt, const WGPUTexelCopyTextureInfo &s,
                         const WGPUTexelCopyTextureInfo &d, const WGPUExtent3D &n,
                         const DkImageView &sv, const DkImageView &dv) {
  if (st && dt && st->hwCompression) {
    static int seen = 0;
    if (seen++ < 6)
      swdk::dklog("image copy from compressed %s (fmt 0x%X %ux%u mip %u) to %s (fmt 0x%X %ux%u mip %u render %d) "
                  "at %u,%u -> %u,%u size %ux%ux%u",
                  st->label.c_str(), unsigned(st->wformat), st->width, st->height, s.mipLevel, dt->label.c_str(),
                  unsigned(dt->wformat), dt->width, dt->height, d.mipLevel, int(dt->renderTarget), s.origin.x,
                  s.origin.y, d.origin.x, d.origin.y, n.width, n.height, n.depthOrArrayLayers);
  }
  if (!st || !dt || !st->hwCompression || !dt->renderTarget || !dt->valid || st->depth || dt->depth ||
      st->compressed || st->samples != 1 || dt->samples != 1 || st->layers != 1 || dt->layers != 1 ||
      st->format != dt->format || !copy_format_ok(st->wformat) || n.depthOrArrayLayers != 1 ||
      s.origin.x != d.origin.x || s.origin.y != d.origin.y || s.origin.z != 0 || d.origin.z != 0)
    return false;
  const uint32_t w = std::max(1u, st->width >> s.mipLevel), h = std::max(1u, st->height >> s.mipLevel);
  if (w != std::max(1u, dt->width >> d.mipLevel) || h != std::max(1u, dt->height >> d.mipLevel) ||
      d.origin.x + n.width > w || d.origin.y + n.height > h)
    return false;
  return draw_copy(sv, dv, w, h, DkScissor{d.origin.x, d.origin.y, n.width, n.height});
}
// Draws `sv` into `dv` 1:1 (texelFetch, no filtering), limited to the scissor rectangle. Also used by the
// depth peek in backend_present.cpp.
bool draw_copy(const DkImageView &sv, const DkImageView &dv, uint32_t w, uint32_t h, const DkScissor &sc) {
  static DkShader vs{}, fs{};
  static int loaded = 0;
  if (loaded == 0) {
    loaded = swdk::code_load(vs, present_vsh_dksh, present_vsh_dksh_size, "copy vertex") &&
                     swdk::code_load(fs, copy_fsh_dksh, copy_fsh_dksh_size, "copy fragment")
                 ? 1
                 : -1;
    swdk::dklog("image copy: compressed sources copied by a draw %s", loaded > 0 ? "on" : "off (shaders failed)");
  }
  if (loaded < 0)
    return false;
  const uint32_t slot = swdk::image_slot_alloc();
  if (slot == 0)
    return false;
  const DkImageView *targets[] = {&dv};
  dkCmdBufBindRenderTargets(swdk::R.cmd, targets, 1, nullptr);
  swdk::bind_2d_state(false);
  const DkViewport vp = {0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  dkCmdBufSetViewports(swdk::R.cmd, 0, &vp, 1);
  dkCmdBufSetScissors(swdk::R.cmd, 0, &sc, 1);
  swdk::write_image_view_descriptor(slot, sv);
  DkResHandle handle = dkMakeTextureHandle(slot, swdk::kSamplerNearestClamp);
  const DkShader *sh[] = {&vs, &fs};
  swdk::code_make_visible();
  dkCmdBufBindVtxAttribState(swdk::R.cmd, nullptr, 0);
  dkCmdBufBindVtxBufferState(swdk::R.cmd, nullptr, 0);
  dkCmdBufBindShaders(swdk::R.cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, sh, 2);
  dkCmdBufBindTextures(swdk::R.cmd, DkStage_Fragment, 0, &handle, 1);
  dkCmdBufDraw(swdk::R.cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
  swdk::image_slot_free(slot);
  return true;
}
} // namespace kd
extern "C" {
void __wrap_wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder e,
                                                   const WGPUTexelCopyTextureInfo *src,
                                                   const WGPUTexelCopyTextureInfo *dst,
                                                   const WGPUExtent3D *size) {
  auto sh = kd::keep(src->texture), dh = kd::keep(dst->texture);
  auto s = *src, d = *dst;
  auto n = *size;
  kd::record(e, [=] {
    kd::GpuCheckpoint checkpoint("image copy", n.width, n.height, n.depthOrArrayLayers);
    kd::open();
    kd::barrier();
    auto *st = kd::find_texture(sh.Get());
    auto *dt = kd::find_texture(dh.Get());
    // RL-NX (R2_DEKO_FRAME_LOG): this frame's copies in bytes, and the XFB's share - the display copy is
    // the one that carries the picture out of the frame.
    if (dt) {
      const uint32_t bw = (n.width + dt->blockW - 1) / dt->blockW;
      const uint32_t bh = (n.height + dt->blockH - 1) / dt->blockH;
      // RL-NX: destination as label, size and id (see end_pass_native); "XFB" in the label is what
      // diag_copy_bytes counts as the display copy. The source goes along: it is the target the frame's
      // picture actually came from, and the frame line prints it beside the passes' targets.
      char dst[64], src[64];
      std::snprintf(dst, sizeof dst, "%s %ux%u#%u", dt->label.c_str(), dt->width, dt->height, dt->id);
      if (st)
        std::snprintf(src, sizeof src, "%s %ux%u#%u", st->label.c_str(), st->width, st->height, st->id);
      else
        std::snprintf(src, sizeof src, "?");
      kd::diag_copy_bytes(uint32_t(uint64_t(bw) * bh * n.depthOrArrayLayers * dt->blockBytes), dst, src);
    }
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
    kd::gpu_operation_image_copy(*st, *dt, s, d);
#endif
    auto sv = kd::level_view(st, s.mipLevel), dv = kd::level_view(dt, d.mipLevel);
    if (kd::copy_by_draw(st, dt, s, d, n, sv, dv)) {
      kd::barrier();
      checkpoint.finish(true);
      return;
    }
    auto sx = s.origin.x, sy = s.origin.y, dx = d.origin.x, dy = d.origin.y, w = n.width,
         h = n.height;
    if (st->compressed) {
      w = (w + st->blockW - 1) / st->blockW;
      h = (h + st->blockH - 1) / st->blockH;
      sx /= st->blockW;
      sy /= st->blockH;
      dx /= dt->blockW;
      dy /= dt->blockH;
    }
    DkImageRect sr{sx, sy, s.origin.z, w, h, n.depthOrArrayLayers},
        dr{dx, dy, d.origin.z, w, h, n.depthOrArrayLayers};
    dkCmdBufCopyImage(swdk::R.cmd, &sv, &sr, &dv, &dr, 0);
    kd::barrier();
    checkpoint.finish(true);
  });
}
// Uploads and readbacks through buffers: rows default to tightly packed when the layout leaves them open.
void __wrap_wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder e,
                                                  const WGPUTexelCopyBufferInfo *src,
                                                  const WGPUTexelCopyTextureInfo *dst,
                                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  auto s = kd::required(kd::buffers, src->buffer, __func__)->active;
  auto t = kd::keep(dst->texture);
  auto layout = src->layout;
  auto d = *dst;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *dt = kd::find_texture(t.Get());
    kd::GpuCheckpoint checkpoint("buffer to image", n.width, n.height, n.depthOrArrayLayers);
    auto dv = kd::level_view(dt, d.mipLevel);
    uint32_t row = layout.bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED
                       ? ((n.width + dt->blockW - 1) / dt->blockW) * dt->blockBytes
                       : layout.bytesPerRow;
    uint32_t rows = layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED
                        ? (n.height + dt->blockH - 1) / dt->blockH
                        : layout.rowsPerImage;
    DkCopyBuf b{s->mem.gpu + layout.offset, row, row * rows};
    DkImageRect r{d.origin.x, d.origin.y, d.origin.z, n.width, n.height, n.depthOrArrayLayers};
#if defined(COS_DK_GPU_CHECKPOINTS) || defined(COS_DK_GPU_FLUSHES)
    // RL-NX: record this upload before it is replayed, so the breadcrumb dump names it if it hangs.
    kd::gpu_operation_buffer_image_copy(*dt, b.addr, s->mem.size > layout.offset ? s->mem.size - layout.offset : 0,
                                        row, row * rows, d, n);
#endif
    dkCmdBufCopyBufferToImage(swdk::R.cmd, &b, &dv, &r, 0);
    kd::barrier();
    s->used();
    checkpoint.finish(true);
  });
}
void __wrap_wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder e,
                                                  const WGPUTexelCopyTextureInfo *src,
                                                  const WGPUTexelCopyBufferInfo *dst,
                                                  const WGPUExtent3D *size) {
  kd::Lock l(kd::mutex);
  auto d = kd::required(kd::buffers, dst->buffer, __func__);
  auto t = kd::keep(src->texture);
  auto layout = dst->layout;
  auto s = *src;
  auto n = *size;
  kd::record(e, [=] {
    kd::open();
    kd::barrier();
    auto *st = kd::find_texture(t.Get());
    kd::GpuCheckpoint checkpoint("image to buffer", n.width, n.height, n.depthOrArrayLayers);
    auto sv = kd::level_view(st, s.mipLevel);
    uint32_t row = layout.bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? n.width * st->blockBytes
                                                                    : layout.bytesPerRow;
    uint32_t rows =
        layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? n.height : layout.rowsPerImage;
    DkCopyBuf b{d->mem.gpu + layout.offset, row, row * rows};
    DkImageRect r{s.origin.x, s.origin.y, s.origin.z, n.width, n.height, n.depthOrArrayLayers};
    dkCmdBufCopyImageToBuffer(swdk::R.cmd, &sv, &r, &b, 0);
    kd::barrier(DkInvalidateFlags_Shader);
    checkpoint.finish(true);
  });
}
// Clears a buffer range by copying zeros from a temporary block (freed once the GPU is past it).
void __wrap_wgpuCommandEncoderClearBuffer(WGPUCommandEncoder e, WGPUBuffer b, uint64_t off,
                                          uint64_t n) {
  kd::Lock l(kd::mutex);
  auto d = kd::required(kd::buffers, b, __func__);
  if (n == WGPU_WHOLE_SIZE)
    n = d->size - off;
  kd::record(e, [=] {
    kd::GpuCheckpoint checkpoint("buffer clear", off, n);
    kd::open();
    kd::barrier();
    std::vector<uint8_t> zero(n);
    auto temp = swdk::block_create(n, "buffer clear");
    memcpy(temp.cpu, zero.data(), n);
    dkCmdBufCopyBuffer(swdk::R.cmd, temp.gpu, d->mem.gpu + off, n);
    swdk::block_free_deferred(temp.block);
    kd::barrier(DkInvalidateFlags_Shader);
    checkpoint.finish(true);
  });
}

// Queue writes execute before subsequent command buffers, under the same lock.
void __wrap_wgpuQueueWriteBuffer(WGPUQueue q, WGPUBuffer h, uint64_t off, const void *data,
                                 size_t n) {
  kd::QueueLock queueLock;
  kd::Lock l(kd::mutex);
  auto b = kd::required(kd::buffers, h, __func__);
  kd::GpuCheckpoint checkpoint("buffer upload", off, n);
  kd::open();
  kd::barrier();
  auto temp = kd::upload_allocation(n);
  memcpy(temp->mem.cpu, data, n);
  dkCmdBufCopyBuffer(swdk::R.cmd, temp->mem.gpu, b->mem.gpu + off, n);
  temp->used();
  kd::barrier(DkInvalidateFlags_Shader);
  checkpoint.finish(true);
}
// Texture uploads are staged with rows padded to 256 bytes, the copy engine's pitch alignment.
void __wrap_wgpuQueueWriteTexture(WGPUQueue q, const WGPUTexelCopyTextureInfo *dst,
                                  const void *data, size_t bytes,
                                  const WGPUTexelCopyBufferLayout *layout,
                                  const WGPUExtent3D *size) {
  kd::QueueLock queueLock;
  kd::Lock l(kd::mutex);
  kd::open();
  kd::barrier();
  auto *t = kd::find_texture(dst->texture);
  kd::GpuCheckpoint checkpoint("texture upload", size->width, size->height, size->depthOrArrayLayers);
  auto v = kd::level_view(t, dst->mipLevel);
  uint32_t row = ((size->width + t->blockW - 1) / t->blockW) * t->blockBytes,
           rows = (size->height + t->blockH - 1) / t->blockH, pitch = (row + 255) & ~255u;
  uint32_t inPitch = layout->bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? row : layout->bytesPerRow;
  uint32_t inRows =
      layout->rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? rows : layout->rowsPerImage;
  auto temp = kd::upload_allocation(uint64_t(pitch) * rows * size->depthOrArrayLayers);
  for (uint32_t z = 0; z < size->depthOrArrayLayers; ++z)
    for (uint32_t y = 0; y < rows; ++y)
      memcpy(temp->mem.cpu + (z * rows + y) * pitch,
             static_cast<const uint8_t *>(data) + layout->offset + (z * inRows + y) * inPitch, row);
  DkCopyBuf b{temp->mem.gpu, pitch, pitch * rows};
  DkImageRect r{dst->origin.x, dst->origin.y, dst->origin.z,
                size->width,   size->height,  size->depthOrArrayLayers};
  dkCmdBufCopyBufferToImage(swdk::R.cmd, &b, &v, &r, 0);
  temp->used();
  kd::barrier();
  checkpoint.finish(true);
}
}
