/**
 * @file buffered_file_reader.h
 * @brief Prefetching file reader over storage::StorageWorker. Read-ahead lives in the shared
 *        storage::TransferBuffer arena -- this class allocates nothing.
 */

#pragma once

#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/storage/transfer_buffer.h"

#include <atomic>
#include <cstdint>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome {
namespace simple_video_player {

/**
 * @brief Prefetching file reader backed by storage::StorageWorker.
 *
 * The read-ahead window is the shared storage::TransferBuffer arena (storage's own PSRAM
 * addition), borrowed for the whole open()..close() session -- this class allocates no memory of
 * its own. The worker's stream API (begin_read/read_chunk/seek/tell/end_read) is async and
 * delivers completions on the main loop; the issuing task waits via a bounded, abortable task
 * notification. read() serves from the arena window and refills it (one worker read_chunk) when
 * it drains.
 *
 * The public interface is synchronous (AVIParser / the playback task expect blocking calls).
 */
class BufferedFileReader {
 public:
  BufferedFileReader() = default;
  ~BufferedFileReader() { this->close(); }

  BufferedFileReader(const BufferedFileReader &) = delete;
  BufferedFileReader &operator=(const BufferedFileReader &) = delete;

  /**
   * @brief Point the reader at a caller-owned stop flag. While set, a pending wait_() returns
   * STORAGE_ERROR_NOT_READY as soon as the flag goes true instead of waiting out the storage
   * completion -- so playback can be stopped promptly.
   */
  void set_abort_flag(const volatile bool *flag) { this->abort_flag_ = flag; }

  bool open(const char *path);
  void close();
  bool is_open() const { return this->open_; }

  /// Read into buffer; returns bytes read (0 on EOF, -1 on error).
  int read(uint8_t *buffer, size_t size);

  bool seek(uint64_t position);
  uint64_t tell() const { return this->current_position_; }

  /// File size via seek(END)/tell()/seek(back) on the stream (no separate stat call).
  bool get_size(uint64_t *size);

  /// No-op kept for call-site compatibility.
  bool prefill_cache() { return true; }

 protected:
  // --- async completion hand-off (worker delivers on the main loop; the issuing task waits via a
  // bounded, abortable task notification) --------------------------------------------------------
  void arm_wait_() {
#ifdef USE_ESP32
    this->waiting_task_ = xTaskGetCurrentTaskHandle();
#endif
    this->done_.store(false, std::memory_order_release);
  }
  void on_done_(storage::StorageError err) {
    this->last_result_ = err;
    this->done_.store(true, std::memory_order_release);
#ifdef USE_ESP32
    if (this->waiting_task_ != nullptr) {
      xTaskNotifyGive(this->waiting_task_);
    }
#endif
  }
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
  // Smallest arena that is worth using as the read-ahead window; open() fails below this.
  static constexpr size_t MIN_WINDOW = 256 * 1024;

  // Refill the arena window from file_pos_ with one blocking worker read_chunk(). Returns bytes
  // read (0 = EOF, <0 = error). Stream must be IDLE.
  int refill_();

  storage::StreamHandle handle_{};
  bool open_{false};
  uint64_t current_position_{0};  // file offset of window_[win_pos_]

#ifdef USE_ESP32
  TaskHandle_t waiting_task_{nullptr};
#endif
  std::atomic<bool> done_{false};
  const volatile bool *abort_flag_{nullptr};
  storage::StorageError last_result_{storage::StorageError::STORAGE_ERROR_OK};

  // Read-ahead window == the borrowed storage::TransferBuffer arena. Not owned, not freed.
  uint8_t *window_{nullptr};
  size_t window_cap_{0};        // arena capacity
  size_t win_len_{0};           // valid bytes currently in the window
  size_t win_pos_{0};           // consumed offset within the window
  uint64_t file_pos_{0};        // file offset the next refill() reads from
};

}  // namespace simple_video_player
}  // namespace esphome
