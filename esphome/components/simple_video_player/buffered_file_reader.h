/**
 * @file buffered_file_reader.h
 * @brief Double-buffered, prefetching file reader backed by storage::StorageWorker
 */

#pragma once

#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace simple_video_player {

/**
 * @brief Double-buffered, prefetching file reader backed by storage::StorageWorker.
 *
 * StorageWorker's stream API (begin_read/read_chunk/seek/tell/end_read) is asynchronous, but
 * only ever allows ONE operation in flight per stream handle at a time (read_chunk()/seek()
 * return NOT_READY if the previous step's completion hasn't been consumed yet) -- so true
 * concurrent reads on a single handle aren't possible. What IS possible, and what this class
 * does, is not waiting for the *next* fetch until the caller has actually exhausted what's
 * already in hand: two READ_AHEAD_CAPACITY buffers are ping-ponged, and the fetch for the
 * "other" buffer is submitted (non-blockingly) the moment consumption switches onto the
 * "active" one, so that by the time the active buffer empties, the next one is often already
 * sitting there -- storage latency overlapping with consumption/decode time instead of
 * serializing "consume fully, then block-and-fetch, repeat" every single refill.
 *
 * The public interface stays synchronous (AVIParser and the playback task already expect
 * blocking calls) -- only read()'s *internal* refill is where the overlap actually happens.
 */
class BufferedFileReader {
 public:
  BufferedFileReader();
  ~BufferedFileReader();

  // Disable copy/move
  BufferedFileReader(const BufferedFileReader &) = delete;
  BufferedFileReader &operator=(const BufferedFileReader &) = delete;

  /**
   * @brief Point the reader at a stop flag owned by the caller (e.g. the loader task's
   * loader_task_stop_). While set, a pending wait_() returns STORAGE_ERROR_NOT_READY as soon as
   * the flag goes true instead of continuing to wait for the storage completion -- so the task
   * that owns this reader can be stopped promptly at end of playback.
   */
  void set_abort_flag(const volatile bool *flag) { this->abort_flag_ = flag; }

  /**
   * @brief Open a file for buffered reading
   * @param path VFS path (e.g. "/sdcard/video.mjpeg"), resolved against the storage registry
   * @return true if file opened successfully
   */
  bool open(const char *path);

  /**
   * @brief Close the file and release resources
   */
  void close();

  /**
   * @brief Check if file is currently open
   */
  bool is_open() const { return this->open_; }

  /**
   * @brief Read data from file into buffer
   * @return Number of bytes actually read (0 on EOF, -1 on error)
   */
  int read(uint8_t *buffer, size_t size);

  /**
   * @brief Seek to an absolute position in the file
   */
  bool seek(uint64_t position);

  /**
   * @brief Get the current logical file position
   */
  uint64_t tell() const { return this->current_position_; }

  /**
   * @brief Get the file size (via seek-to-end/tell/seek-back through the worker; no separate
   * stat call -- the worker's own SEEK_MODE_END already resolves file size where needed)
   */
  bool get_size(uint64_t *size);

  /**
   * @brief No-op kept for call-site compatibility: the worker has no separate cache to
   * pre-warm. Always returns true.
   */
  bool prefill_cache() { return true; }

 protected:
  // Async completion hand-off for every storage-worker call below. The worker delivers exactly
  // one completion per submitted request, on the main loop; the task that issued the request
  // waits for it via a task notification (the same primitive the playback pacing loop uses), not
  // a semaphore, and never blocks unbounded.
  //
  // Call once, immediately BEFORE submitting a worker op, on the task that will wait_() for it:
  // records that task as the one to wake and clears the completion flag. Same task-notification
  // hand-off the playback pacing loop uses -- no semaphore.
  void arm_wait_() {
#ifdef USE_ESP32
    this->waiting_task_ = xTaskGetCurrentTaskHandle();
#endif
    this->done_.store(false, std::memory_order_release);
  }

  // Storage-worker completion callback (runs on the main loop). Record the result, then wake the
  // armed task.
  void on_done_(storage::StorageError err) {
    this->last_result_ = err;
    this->done_.store(true, std::memory_order_release);
#ifdef USE_ESP32
    if (this->waiting_task_ != nullptr) {
      xTaskNotifyGive(this->waiting_task_);
    }
#endif
  }

  // Waits for on_done_() of the armed request. Woken by its task notification (precise, no polling
  // interval); the ulTaskNotifyTake timeout is only a slice for re-checking the abort flag, and
  // WAIT_CAP_MS bounds the whole wait so a lost completion can never hang the task. Returns the
  // request's StorageError, or NOT_READY if the caller's abort flag went true, or TIMEOUT.
  storage::StorageError wait_() {
#ifdef USE_ESP32
    uint32_t waited_ms = 0;
    while (!this->done_.load(std::memory_order_acquire)) {
      if (this->abort_flag_ != nullptr && *this->abort_flag_) {
        return storage::StorageError::STORAGE_ERROR_NOT_READY;
      }
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAIT_SLICE_MS)) == 0) {
        waited_ms += WAIT_SLICE_MS;
        if (waited_ms >= WAIT_CAP_MS) {
          return storage::StorageError::STORAGE_ERROR_TIMEOUT;
        }
      }
    }
#endif
    return this->last_result_;
  }

  static constexpr uint32_t WAIT_SLICE_MS = 20;
  static constexpr uint32_t WAIT_CAP_MS = 5000;

  // Issues one direct (uncached), BLOCKING worker read_chunk() into dest -- used for requests too
  // large to benefit from the read-ahead buffers (the per-frame JPEG payload path) and for the
  // very first fill on open()/seek(). Returns bytes read (0 = EOF, <0 = error).
  int read_chunk_(uint8_t *dest, size_t size);

  // Submits a NON-blocking read_chunk() to refill read_ahead_buf_[idx] and returns immediately
  // -- does not wait for completion. The stream must be IDLE (no other op in flight) when this
  // is called; the caller is responsible for that (only ever one prefetch in flight at a time,
  // tracked by prefetch_pending_).
  void start_prefetch_(int idx);

  // Blocks until the in-flight prefetch (if any) completes, recording its result into
  // read_ahead_len_[prefetch_idx_]. No-op if nothing is pending. Must be called before any other
  // stream op (seek, a direct read_chunk_, close) -- those all require the stream to be IDLE.
  void resolve_prefetch_();

  // Called whenever the active buffer is fully consumed: resolves any in-flight prefetch for the
  // *other* buffer, swaps active_idx_ to it, and (if that swap didn't land on EOF) kicks a new
  // prefetch for the buffer just vacated. Returns false at EOF (nothing more to read).
  bool swap_to_prefetched_();

  storage::StreamHandle handle_{};
  bool open_{false};
  uint64_t current_position_{0};

#ifdef USE_ESP32
  TaskHandle_t waiting_task_{nullptr};  // set by arm_wait_(); the task on_done_() notifies
#endif
  std::atomic<bool> done_{false};                     // false from arm_wait_() until on_done_()
  const volatile bool *abort_flag_{nullptr};          // caller's stop flag, see set_abort_flag()
  // Written by the completion callback before it sets done_; read by wait_() after it observes
  // done_ true -- the release/acquire on done_ orders it, no other lock needed.
  storage::StorageError last_result_{storage::StorageError::STORAGE_ERROR_OK};

  // Double-buffered read-ahead cache (PSRAM -- ESP32-P4 only, per project convention). AVIParser
  // issues many small sequential reads per chunk (4-byte fourcc, 4-byte size, 1-byte alignment
  // pad) on top of the one big per-frame JPEG payload read -- each call to the worker's
  // read_chunk() pays a real storage-bus round trip regardless of how few bytes are requested, so
  // without buffering, those small reads pay that latency individually instead of amortized. 1MB
  // per buffer is generous by design against the P4's 32MB PSRAM: at typical frame sizes (a few
  // hundred KB) this comfortably spans several whole frames' worth of interleaved video+audio
  // chunk data per single underlying read, not just the header bytes.
  static constexpr size_t READ_AHEAD_CAPACITY = 1 * 1024 * 1024;
  std::unique_ptr<uint8_t[]> read_ahead_buf_[2];
  size_t read_ahead_len_[2]{0, 0};  // valid bytes currently in each buffer (0 = empty/not filled)
  int active_idx_{0};               // which of [0]/[1] is currently being served from
  size_t read_ahead_pos_{0};        // consumed-so-far offset within read_ahead_buf_[active_idx_]

  // Prefetch (for read_ahead_buf_[1 - active_idx_]) state.
  bool prefetch_pending_{false};  // true from start_prefetch_() until resolve_prefetch_()
  int prefetch_idx_{-1};          // which buffer index the pending prefetch targets
  size_t prefetch_bytes_{0};      // bound to the worker call; valid only once resolved
};

}  // namespace simple_video_player
}  // namespace esphome
