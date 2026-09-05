/**
 * @file buffered_file_reader.h
 * @brief Blocking file reader backed by storage::StorageWorker
 */

#pragma once

#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"

#include <cstdint>
#include <cstdio>
#include <memory>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace simple_video_player {

/**
 * @brief Blocking file reader backed by storage::StorageWorker.
 *
 * StorageWorker's stream API (begin_read/read_chunk/seek/tell/end_read) is asynchronous --
 * completions fire later, on the main loop. This class turns it back into the synchronous,
 * blocking interface AVIParser and the playback task already expect: each call submits the
 * matching worker request and blocks the calling task on a semaphore until the completion
 * callback signals it. All actual file-ops (buffering, seek/tell, EOF, local-vs-network
 * dispatch) belong to the worker/driver -- this class reinvents none of it.
 */
class BufferedFileReader {
 public:
  BufferedFileReader();
  ~BufferedFileReader();

  // Disable copy/move
  BufferedFileReader(const BufferedFileReader &) = delete;
  BufferedFileReader &operator=(const BufferedFileReader &) = delete;

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
  // Completion callback shared by every worker call below: records the result and wakes the
  // blocked caller. The worker guarantees exactly one completion per submitted request, so
  // done_sem_ never accumulates more than one pending "give".
  void on_done_(storage::StorageError err) {
    this->last_result_ = err;
#ifdef USE_ESP32
    xSemaphoreGive(this->done_sem_);
#endif
  }

  // Blocks until on_done_() fires for the just-submitted request; returns its StorageError.
  storage::StorageError wait_() {
#ifdef USE_ESP32
    xSemaphoreTake(this->done_sem_, portMAX_DELAY);
#endif
    return this->last_result_;
  }

  // Issues one direct (uncached) worker read_chunk() into dest -- shared by read()'s large-request
  // path and the read-ahead refill path. Returns bytes read (0 = EOF, <0 = error), same contract
  // as read().
  int read_chunk_(uint8_t *dest, size_t size);

  storage::StreamHandle handle_{};
  bool open_{false};
  uint64_t current_position_{0};

#ifdef USE_ESP32
  SemaphoreHandle_t done_sem_{nullptr};
#endif
  // Set by the completion callback just before it gives done_sem_; read by wait_() right after
  // taking it -- safe with no extra lock since the semaphore itself is the handoff point (the
  // callback always finishes its write before giving, wait_() always reads after taking).
  storage::StorageError last_result_{storage::StorageError::STORAGE_ERROR_OK};

  // Read-ahead cache (PSRAM -- ESP32-P4 only, per project convention: everything but small
  // DMA-aligned scratch buffers lives in PSRAM). AVIParser issues many small sequential reads per
  // chunk (4-byte fourcc, 4-byte size, 1-byte alignment pad) on top of the one big per-frame JPEG
  // payload read -- each call to the worker's read_chunk() pays a full async submit/block round
  // trip regardless of how few bytes are requested, so without buffering, those small reads pay
  // that latency individually instead of amortized. 1MB is generous by design against the P4's
  // 32MB PSRAM: at typical frame sizes (a few hundred KB) this comfortably spans several whole
  // frames' worth of interleaved video+audio chunk data per single underlying read, not just the
  // header bytes -- a request that doesn't fit even this (extremely large frame) falls back to
  // its own direct read_chunk_() call.
  static constexpr size_t READ_AHEAD_CAPACITY = 1 * 1024 * 1024;
  std::unique_ptr<uint8_t[]> read_ahead_buf_;
  size_t read_ahead_len_{0};  // valid bytes currently in read_ahead_buf_
  size_t read_ahead_pos_{0};  // consumed-so-far offset within read_ahead_buf_
};

}  // namespace simple_video_player
}  // namespace esphome
