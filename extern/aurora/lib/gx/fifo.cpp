#include "fifo.hpp"

#include "../thread.hpp"
#include "command_processor.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>       // RL-NX (Stage 7): frame boundaries
#include <functional>  // RL-NX (Stage 7)
#include <limits>
#include <mutex>

#include <tracy/Tracy.hpp>

namespace aurora::gx::fifo {
namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

namespace {
constexpr Module Log{"aurora::gx::fifo"};
ProcessingMode sProcessingMode = ProcessingMode::Thread;
// Draws handed to the worker at once: waking it is a system call, too costly for every draw
// of a heavy frame (drain() still hands over everything at frame ends and sync points)
constexpr uint32_t kDrawBatchSize = 16;

bool sFrameActive = false;
uint32_t sPendingDraws = 0;
std::atomic<uint64_t> sPublished{0};
std::atomic<uint64_t> sProcessed{0};
uint64_t sStreamBase = 0;
std::mutex sBufferMutex;
std::atomic<uint32_t> sWorkerWake{0};
thread::Thread sWorkerThread;
std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};

// RL-NX (Stage 7, from SFA-NX): notify only a side that is actually waiting. libstdc++'s atomic wait/notify is a
// shared mutex + condition variable on the Switch (no futex), so every unconditional notify_all was a lock the two
// cores fought over (SFA-NX build 2046: 3.3 ms a frame of svcArbitrateLock on the FIFO thread). The flag and the
// value are both seq_cst, so either the waiter sees the new value or the writer sees the flag.
std::atomic<bool> sWorkerSleeping{false};
std::atomic<uint32_t> sProcessedWaiters{0};

// RL-NX (Stage 7): frame overlap, ported from SFA-NX's Aurora (foxhollow extern/aurora/lib/gx/fifo.cpp). Without it
// the producer (RL-NX's GX bridge worker) waited in end_frame until this thread had decoded the whole frame
// (hardware build 14:45, level 22: 5-9 ms a frame, and the game thread then waited on the bridge worker). Now each
// frame's commands live in their own buffer: end_frame_async hands the finished buffer over with a frame boundary
// (a stream position plus the work that ends the frame's recording and opens the next one), and the producer goes
// straight on writing the next frame into the other buffer. At most one boundary is pending, so a frame can be at
// most one ahead, as the GameCube's GPU ran a frame behind its CPU. drain() still waits for everything, boundaries
// included, so the bridge's sync points (CP reads, PE interrupts) keep their guarantee that the renderer has read
// all guest memory behind the commands so far.
uint8_t* sPrevData = nullptr; // previous frame's commands (guarded by sBufferMutex)
uint32_t sPrevCapacity = 0;
uint64_t sPrevBase = 0;
uint32_t sPrevSize = 0;
struct Boundary {
  uint64_t pos;
  std::function<void()> work;
};
std::deque<Boundary> sBoundaries; // guarded by sBufferMutex
// RL-NX (Stage 7, from SFA-NX): this thread processes outside sBufferMutex (holding it made the producer's frame
// hand-over and every buffer growth wait for the whole pass in progress). The buffer it reads is named here;
// growing the current buffer while it is being read copies it and leaves the old copy here for this thread to free.
const uint8_t* sProcessingData = nullptr; // guarded by sBufferMutex
uint8_t* sRetiredData = nullptr;          // guarded by sBufferMutex
std::atomic<uint64_t> sBoundariesQueued{0};
std::atomic<uint64_t> sBoundariesDone{0};

void dispatch_draw_done() noexcept {
  if (const auto callback = sDrawDoneCallback.load(std::memory_order_acquire); callback != nullptr) {
    callback();
  }
}

void wake_worker() noexcept {
  sWorkerWake.fetch_add(1, std::memory_order_seq_cst);
  if (sWorkerSleeping.load(std::memory_order_seq_cst)) { // RL-NX (Stage 7)
    sWorkerWake.notify_all();
  }
}

// RL-NX (Stage 7): processes up to target, running any frame boundary the stream reaches on the way. Thread mode
// processes outside the lock (sProcessingData); Drain/Inline run on the producer, which nothing else touches.
void process_to(uint64_t target, std::memory_order order) noexcept {
  const bool threaded = sProcessingMode == ProcessingMode::Thread;
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (true) {
    ProcessResult result{};
    std::function<void()> boundary;
    uint64_t limit = target;
    {
      std::unique_lock lock{sBufferMutex, std::defer_lock};
      if (threaded) {
        lock.lock();
      }
      if (!sBoundaries.empty() && sBoundaries.front().pos <= processed) {
        boundary = std::move(sBoundaries.front().work);
        sBoundaries.pop_front();
      } else if (processed >= target) {
        break;
      } else {
        if (!sBoundaries.empty() && sBoundaries.front().pos < limit) {
          limit = sBoundaries.front().pos;
        }
        const uint8_t* data;
        uint64_t base;
        if (sPrevSize != 0 && processed < sPrevBase + sPrevSize) {
          data = sPrevData;
          base = sPrevBase;
          limit = std::min<uint64_t>(limit, sPrevBase + sPrevSize);
        } else {
          AURORA_ASSERT(processed >= sStreamBase && limit <= sStreamBase + detail::sBufferSize,
                        "FIFO processing range [{}, {}) is outside buffered range [{}, {})", processed, limit,
                        sStreamBase, sStreamBase + detail::sBufferSize);
          data = detail::sBufferData;
          base = sStreamBase;
        }
        if (threaded) {
          sProcessingData = data;
          lock.unlock();
        }
        result = process(data + static_cast<uint32_t>(processed - base), static_cast<uint32_t>(limit - processed));
        if (threaded) {
          lock.lock();
          sProcessingData = nullptr;
          if (sRetiredData != nullptr) {
            free(sRetiredData);
            sRetiredData = nullptr;
          }
        }
      }
    }
    if (boundary) {
      boundary();
      sBoundariesDone.fetch_add(1, std::memory_order_seq_cst);
      if (threaded) {
        sBoundariesDone.notify_all();
      }
      continue;
    }
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= limit - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  limit - processed);
    if (result.drawDone) {
      dispatch_draw_done();
    }
    processed += result.bytesProcessed;
    sProcessed.store(processed, order == std::memory_order_relaxed ? order : std::memory_order_seq_cst);
    if (threaded && sProcessedWaiters.load(std::memory_order_seq_cst) != 0) { // RL-NX (Stage 7)
      sProcessed.notify_all();
    }
  }
}

// RL-NX (Stage 7): true while a frame boundary queued by end_frame_async has not run yet on the FIFO thread.
bool boundaries_pending() noexcept {
  return sBoundariesDone.load(std::memory_order_acquire) != sBoundariesQueued.load(std::memory_order_relaxed);
}

// RL-NX (Stage 7): waits until every queued frame boundary has run (the previous frame is recorded and submitted).
void wait_boundaries() noexcept {
  const uint64_t queued = sBoundariesQueued.load(std::memory_order_relaxed);
  uint64_t done = sBoundariesDone.load(std::memory_order_acquire);
  while (done != queued) {
    sBoundariesDone.wait(done, std::memory_order_acquire);
    done = sBoundariesDone.load(std::memory_order_acquire);
  }
}

void worker_main(std::stop_token token) noexcept {
  std::stop_callback wakeOnStop{token, [] {
                                  sWorkerWake.fetch_add(1, std::memory_order_seq_cst);
                                  sWorkerWake.notify_all();
                                }};
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    if (published != processed || boundaries_pending()) {
      process_to(published, std::memory_order_release);
      continue;
    }

    if (token.stop_requested()) {
      break;
    }
    // RL-NX (Stage 7): announce the sleep, then check once more for work published meanwhile (sWorkerSleeping)
    sWorkerSleeping.store(true, std::memory_order_seq_cst);
    if (sPublished.load(std::memory_order_seq_cst) == sProcessed.load(std::memory_order_relaxed) &&
        !boundaries_pending() && !token.stop_requested()) {
      sWorkerWake.wait(event, std::memory_order_acquire);
    }
    sWorkerSleeping.store(false, std::memory_order_relaxed);
  }
}

void start_worker() {
  if (sProcessingMode != ProcessingMode::Thread || sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread = thread::Thread{{
                                     .name = "Aurora FIFO processor",
                                     .affinity = thread::Affinity::SharedCache,
                                 },
                                 worker_main};
}

void stop_worker() {
  if (!sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread.request_stop();
  sWorkerThread.join();
}
} // namespace

ProcessingMode processing_mode() noexcept { return sProcessingMode; }

void init(ProcessingMode mode) {
  stop_worker();
  sProcessingMode = mode;

  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  AURORA_ASSERT(detail::sBufferData != nullptr, "fifo::init: failed to allocate {} bytes", initialCapacity);
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;

  sFrameActive = false;
  sPendingDraws = 0;
  sStreamBase = 0;
  sPublished.store(0, std::memory_order_relaxed);
  sProcessed.store(0, std::memory_order_relaxed);
  sWorkerWake.store(0, std::memory_order_relaxed);
  // RL-NX (Stage 7): the second buffer and the boundary queue
  free(sPrevData);
  sPrevData = nullptr;
  free(sRetiredData);
  sRetiredData = nullptr;
  sProcessingData = nullptr;
  sPrevCapacity = 0;
  sPrevBase = 0;
  sPrevSize = 0;
  sBoundaries.clear();
  sBoundariesQueued.store(0, std::memory_order_relaxed);
  sBoundariesDone.store(0, std::memory_order_relaxed);

  start_worker();
}

void shutdown() { stop_worker(); }

void begin_frame() noexcept { sFrameActive = true; }

void end_frame() noexcept {
  sFrameActive = false;
  clear_draw_cache(); // command_processor
}

// RL-NX (Stage 7): the producer's frame is over but its end runs later, on the FIFO thread (end_frame_async)
void set_frame_inactive() noexcept { sFrameActive = false; }

// RL-NX (Stage 7, from SFA-NX): hands the frame written so far to the FIFO thread together with `work`, which runs
// there once every command before it has been processed, and gives the producer the other buffer for the next frame.
void end_frame_async(std::function<void()> work) {
  // One frame in flight: the previous frame's boundary must have run, so its buffer is free again.
  wait_boundaries();
  const uint64_t target = sStreamBase + detail::sBufferSize;
  {
    std::lock_guard lock{sBufferMutex};
    sBoundaries.push_back({target, std::move(work)});
    uint8_t* spare = sPrevData;
    uint32_t spareCapacity = sPrevCapacity;
    if (spare == nullptr) {
      spareCapacity = detail::sBufferCapacity;
      spare = static_cast<uint8_t*>(malloc(spareCapacity));
      AURORA_ASSERT(spare != nullptr, "fifo::end_frame_async: failed to allocate {} bytes", spareCapacity);
    }
    sPrevData = detail::sBufferData;
    sPrevCapacity = detail::sBufferCapacity;
    sPrevBase = sStreamBase;
    sPrevSize = detail::sBufferSize;
    detail::sBufferData = spare;
    detail::sBufferCapacity = spareCapacity;
    detail::sBufferSize = 0;
    sStreamBase = target;
  }
  sBoundariesQueued.fetch_add(1, std::memory_order_seq_cst);
  sPendingDraws = 0;
  sPublished.store(target, std::memory_order_seq_cst);
  wake_worker();
}

void write_data_grow(const void* data, uint32_t length) {
  const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
  AURORA_ASSERT(needed64 <= std::numeric_limits<uint32_t>::max(), "fifo::write_data: buffer size overflow");
  const auto needed = static_cast<uint32_t>(needed64);
  const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
  const auto newCapacity = static_cast<uint32_t>(
      std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
  const auto grow = [newCapacity] {
    // RL-NX (Stage 7): the FIFO thread reads this buffer outside the lock: copy, and let it free the old one
    if (detail::sBufferData != nullptr && detail::sBufferData == sProcessingData) {
      auto* fresh = static_cast<uint8_t*>(malloc(newCapacity));
      AURORA_ASSERT(fresh != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
      AURORA_ASSERT(sRetiredData == nullptr, "fifo::write_data: a retired buffer is still pending");
      std::memcpy(fresh, detail::sBufferData, detail::sBufferSize);
      sRetiredData = detail::sBufferData;
      detail::sBufferData = fresh;
      return;
    }
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
    detail::sBufferData = resized;
  };
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    grow();
  } else {
    grow();
  }
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCapacity;
}

void publish() noexcept {
  if (!sFrameActive || sProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }

  const uint64_t target = sStreamBase + detail::sBufferSize;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_seq_cst); // RL-NX (Stage 7): seq_cst for the sleep handshake
    if (sProcessingMode == ProcessingMode::Thread) {
      wake_worker();
    } else {
      process_to(target, std::memory_order_relaxed);
    }
  }
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {
  return sDrawDoneCallback.exchange(callback, std::memory_order_acq_rel);
}

void finish_draw() noexcept {
  if (!sFrameActive || sProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }
  if (++sPendingDraws >= kDrawBatchSize) {
    publish();
  }
}

void patch_u32(uint32_t offset, uint32_t val) {
  AURORA_ASSERT(!detail::sInDisplayList && offset <= detail::sBufferSize &&
                    sizeof(uint32_t) <= detail::sBufferSize - offset,
                "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  AURORA_ASSERT(sStreamBase + offset >= sPublished.load(std::memory_order_relaxed),
                "fifo::patch_u32: offset {} is below the published watermark", offset);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  const uint32_t bytesWritten = detail::sDlWritePos;
  const uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

void drain() {
  if (detail::sBufferSize == 0) {
    // RL-NX (Stage 7): nothing new, but a frame handed over by end_frame_async may still be running
    if (sProcessingMode == ProcessingMode::Thread) {
      wait_boundaries();
    }
    return;
  }

  ZoneScoped;
  const uint64_t target = sStreamBase + detail::sBufferSize;

  switch (sProcessingMode) {
  case ProcessingMode::Drain:
  case ProcessingMode::Inline:
    sPublished.store(target, std::memory_order_relaxed);
    process_to(target, std::memory_order_relaxed);
    break;
  case ProcessingMode::Thread: {
    sPublished.store(target, std::memory_order_seq_cst);
    wake_worker();

    // RL-NX (Stage 7): registered as a waiter so the FIFO thread notifies (sProcessedWaiters)
    sProcessedWaiters.fetch_add(1, std::memory_order_seq_cst);
    uint64_t processed = sProcessed.load(std::memory_order_seq_cst);
    while (processed < target) {
      sProcessed.wait(processed, std::memory_order_acquire);
      processed = sProcessed.load(std::memory_order_acquire);
    }
    sProcessedWaiters.fetch_sub(1, std::memory_order_relaxed);
    wait_boundaries();
    break;
  }
  }

  {
    std::lock_guard lock{sBufferMutex};
    sStreamBase = target;
    detail::sBufferSize = 0;
    sPrevSize = 0; // RL-NX (Stage 7): the previous frame's buffer is processed too
  }
  sPendingDraws = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }

void clear_buffer() {
  const uint64_t processed = sProcessed.load(std::memory_order_acquire);
  AURORA_ASSERT(sPublished.load(std::memory_order_acquire) == processed,
                "fifo::clear_buffer: published commands are still pending");
  if (sProcessingMode == ProcessingMode::Thread) {
    wait_boundaries(); // RL-NX (Stage 7)
  }
  std::lock_guard lock{sBufferMutex};
  sStreamBase = processed;
  detail::sBufferSize = 0;
  sPrevSize = 0; // RL-NX (Stage 7)
  sPendingDraws = 0;
}

} // namespace aurora::gx::fifo
