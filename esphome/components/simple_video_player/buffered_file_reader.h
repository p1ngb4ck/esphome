/**
 * @file buffered_file_reader.h
 * @brief Async file reader: storage::StorageWorker streams the file (on its own task) into the
 *        shared storage::TransferBuffer arena; each read_chunk completion copies into the PSRAM
 *        read-ahead ring and kicks the next read. read() just drains the ring, so
 *        playback and file I/O run in parallel and read() does not block while data is available.
 */

#pragma once

#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/storage/transfer_buffer.h"

#include <atomic>
#include <cstdint>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esphome {
namespace simple_video_player {

class BufferedFileReader {
 public:
  BufferedFileReader() = default;
  ~BufferedFileReader() { this->close(); }

  BufferedFileReader(const BufferedFileReader &) = delete;
  BufferedFileReader &operator=(const BufferedFileReader &) = delete;

  /// While *flag is true, a pending wait returns immediately instead of waiting out a storage
  /// completion -- so playback can be stopped promptly.
  void set_abort_flag(const volatile bool *flag) { this->abort_flag_ = flag; }

  bool open(const char *path);
  void close();
  bool is_open() const { return this->open_; }

  /// Network source: the same read-ahead ring and the same read() path as a file, filled by push()
  /// from the network task instead of the storage worker. seek() only goes forward.
  bool open_stream();
  /// Bytes push() takes right now (0 when not open for a stream).
  size_t stream_room();
  /// Network task: append received bytes, at most stream_room() of them.
  void push(const uint8_t *data, size_t len);
  /// Network task: the sender is gone; read() reports the end once the ring is drained.
  void end_stream();

  /// LOAD uses the default (blocking) read -- header/dimension probe needs the bytes now, before
  /// the async stream has spun up. The play loop calls set_streaming(true) once, right before its
  /// frame loop, so from then on read() is a single non-blocking ring drain (no 20ms park).
  void set_streaming(bool streaming) { this->streaming_ = streaming; }

  /// Drain up to `size` bytes from the ring. Returns bytes read (0 = EOF, -1 = error).
  int read(uint8_t *buffer, size_t size);
  /// Copies `size` bytes found `offset` bytes past the read position, without taking them; only from
  /// the task that read()s. False while those bytes are not in the ring yet.
  bool peek(size_t offset, uint8_t *buffer, size_t size) const;

  bool seek(uint64_t position);
  uint64_t tell() const { return this->current_position_; }
  bool get_size(uint64_t *size);

  /// LOAD-time: block (bounded) until the read-ahead ring is full, so the first frame reads in
  /// streaming mode already have their bytes. Uses the existing kick_fill_ -> on_fill_done_ chain.
  bool prefill_cache();

  /// Fill-path counters for the end-of-playback stats (plain counters, reset per open()).
  struct FillStats {
    uint32_t underruns{0};      // streaming reads that found the ring short of the request
    size_t min_avail{0};        // lowest ring fill seen by a streaming read
    bool min_set{false};
    uint32_t chunks{0};         // read_chunk completions delivered into the ring
    uint64_t chunk_us{0};       // sum of submit -> delivered time
    uint32_t chunk_us_max{0};
    uint64_t bytes_filled{0};
  };
  const FillStats &fill_stats() const { return this->stats_; }

 protected:
  // --- blocking hand-off for the one-shot stream calls (begin_read/seek/tell/end_read) ----------
  void arm_wait_() {
    this->waiting_task_ = xTaskGetCurrentTaskHandle();
    this->sync_done_.store(false, std::memory_order_release);
  }
  void on_sync_done_(storage::StorageError err) {
    this->sync_result_ = err;
    this->sync_done_.store(true, std::memory_order_release);
    if (this->waiting_task_ != nullptr)
      xTaskNotifyGive(this->waiting_task_);
  }
  storage::StorageError wait_sync_() {
    uint32_t waited = 0;
    while (!this->sync_done_.load(std::memory_order_acquire)) {
      if (this->abort_flag_ != nullptr && *this->abort_flag_)
        return storage::StorageError::STORAGE_ERROR_NOT_READY;
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAIT_SLICE_MS)) == 0 && (waited += WAIT_SLICE_MS) >= WAIT_CAP_MS)
        return storage::StorageError::STORAGE_ERROR_TIMEOUT;
    }
    return this->sync_result_;
  }

  // --- async fill chain -------------------------------------------------------------------------
  // Submit one non-blocking read_chunk() into the arena if the ring has room and nothing is in
  // flight. Safe to call from the playback task (after read() drains the ring) and from the
  // completion callback on the main loop -- the atomic exchange makes it single-entry.
  void kick_fill_();
  // read_chunk completion (main loop): copy what was read into the ring, then chain the next fill.
  void on_fill_done_(storage::StorageError err);
  // Wait (bounded, abortable) for any in-flight fill to finish, so a one-shot stream call can run.
  void quiesce_fill_();

  static constexpr uint32_t WAIT_SLICE_MS = 20;
  static constexpr uint32_t WAIT_CAP_MS = 5000;
  static constexpr size_t RING_BYTES = 4 * 1024 * 1024;  // PSRAM read-ahead ring (~4 MB)
  static constexpr size_t FILL_CHUNK = 256 * 1024;   // per read_chunk into the arena

  storage::StreamHandle handle_{};
  bool open_{false};
  bool network_{false};                       // open_stream(): filled by push(), not the storage worker
  SemaphoreHandle_t stream_lock_{nullptr};    // push()/end_stream() vs close() of a network source
  bool streaming_{false};  // false = blocking read (LOAD); true = non-blocking single drain (PLAY)
  uint64_t current_position_{0};

  TaskHandle_t waiting_task_{nullptr};
  std::atomic<bool> sync_done_{false};
  storage::StorageError sync_result_{storage::StorageError::STORAGE_ERROR_OK};
  const volatile bool *abort_flag_{nullptr};

  uint8_t *arena_{nullptr};  // borrowed storage::TransferBuffer -- read_chunk destination
  // Byte ring with look-ahead: one writer (fill chain or push()), one reader task that read()s and
  // peek()s. Positions count bytes and wrap at 2^32; RING_BYTES is a power of two, so they index it.
  struct LookaheadRing {
    uint8_t *data{nullptr};
    std::atomic<uint32_t> head{0};  // bytes written
    std::atomic<uint32_t> tail{0};  // bytes read
    bool allocate();
    void release();
    void reset() {
      this->head.store(0, std::memory_order_relaxed);
      this->tail.store(0, std::memory_order_release);
    }
    size_t available() const {
      return this->head.load(std::memory_order_acquire) - this->tail.load(std::memory_order_relaxed);
    }
    size_t free() const {
      return RING_BYTES - (this->head.load(std::memory_order_acquire) - this->tail.load(std::memory_order_acquire));
    }
    void write(const uint8_t *src, size_t len);  // len <= free()
    size_t read(uint8_t *dst, size_t len);
    bool peek(size_t offset, uint8_t *dst, size_t len) const;
    void copy_out_(uint32_t pos, uint8_t *dst, size_t len) const;
  };
  static_assert((RING_BYTES & (RING_BYTES - 1)) == 0, "RING_BYTES must be a power of two");
  LookaheadRing ring_;
  std::atomic<bool> fill_in_flight_{false};
  std::atomic<bool> draining_{false};   // set during close()/seek(): no new fill may start
  size_t fill_got_{0};                  // bytes the in-flight read_chunk reports
  std::atomic<bool> eof_{false};
  std::atomic<bool> fill_err_{false};
  uint32_t fill_submit_us_{0};
  FillStats stats_{};
};

}  // namespace simple_video_player
}  // namespace esphome
