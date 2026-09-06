/**
 * @file buffered_file_reader.h
 * @brief Async file reader: storage::StorageWorker streams the file (on its own task) into the
 *        shared storage::TransferBuffer arena; each read_chunk completion copies into a small
 *        ring_buffer::RingBuffer and kicks the next read. read() just drains the ring, so
 *        playback and file I/O run in parallel and read() does not block while data is available.
 */

#pragma once

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/storage/transfer_buffer.h"

#include <atomic>
#include <cstdint>
#include <memory>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

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

  /// Drain up to `size` bytes from the ring. Returns bytes read (0 = EOF, -1 = error).
  int read(uint8_t *buffer, size_t size);

  bool seek(uint64_t position);
  uint64_t tell() const { return this->current_position_; }
  bool get_size(uint64_t *size);

  bool prefill_cache() { return true; }

 protected:
  // --- blocking hand-off for the one-shot stream calls (begin_read/seek/tell/end_read) ----------
  void arm_wait_() {
#ifdef USE_ESP32
    this->waiting_task_ = xTaskGetCurrentTaskHandle();
#endif
    this->sync_done_.store(false, std::memory_order_release);
  }
  void on_sync_done_(storage::StorageError err) {
    this->sync_result_ = err;
    this->sync_done_.store(true, std::memory_order_release);
#ifdef USE_ESP32
    if (this->waiting_task_ != nullptr)
      xTaskNotifyGive(this->waiting_task_);
#endif
  }
  storage::StorageError wait_sync_() {
#ifdef USE_ESP32
    uint32_t waited = 0;
    while (!this->sync_done_.load(std::memory_order_acquire)) {
      if (this->abort_flag_ != nullptr && *this->abort_flag_)
        return storage::StorageError::STORAGE_ERROR_NOT_READY;
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WAIT_SLICE_MS)) == 0 && (waited += WAIT_SLICE_MS) >= WAIT_CAP_MS)
        return storage::StorageError::STORAGE_ERROR_TIMEOUT;
    }
#endif
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
  static constexpr size_t RING_BYTES = 512 * 1024;   // the small extra ring
  static constexpr size_t FILL_CHUNK = 128 * 1024;   // per read_chunk into the arena

  storage::StreamHandle handle_{};
  bool open_{false};
  uint64_t current_position_{0};

#ifdef USE_ESP32
  TaskHandle_t waiting_task_{nullptr};
#endif
  std::atomic<bool> sync_done_{false};
  storage::StorageError sync_result_{storage::StorageError::STORAGE_ERROR_OK};
  const volatile bool *abort_flag_{nullptr};

  uint8_t *arena_{nullptr};  // borrowed storage::TransferBuffer -- read_chunk destination
  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  std::atomic<bool> fill_in_flight_{false};
  std::atomic<bool> draining_{false};   // set during close()/seek(): no new fill may start
  size_t fill_got_{0};                  // bytes the in-flight read_chunk reports
  std::atomic<bool> eof_{false};
  std::atomic<bool> fill_err_{false};
};

}  // namespace simple_video_player
}  // namespace esphome
