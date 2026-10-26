/**
 * @file buffered_file_reader.h
 * @brief Async file reader: storage::StorageWorker streams the file (on its own task) straight
 *        into the free part of the PSRAM read-ahead ring; each read_chunk completion publishes
 *        what arrived and kicks the next read. read() just drains the ring, so playback and file
 *        I/O run in parallel and read() does not block while data is available.
 */

#pragma once

#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

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
  /// The read-ahead ring's memory (the player's, DMA-capable PSRAM), whole FILL_CHUNKs, at least two;
  /// only while closed.
  void set_ring(uint8_t *data, size_t size) {
    this->ring_.data = data;
    this->ring_.size = size;
    this->ring_.reset();
    this->fill_chunk_ = std::min(FILL_CHUNK, size / 2);
  }
  bool has_ring() const { return this->ring_.data != nullptr; }
  static constexpr size_t FILL_CHUNK = 256 * 1024;  // per read_chunk, straight into the ring

  bool open(const char *path);
  void close();
  bool is_open() const { return this->open_; }
  /// A read of the file failed; seek() starts over.
  bool failed() const { return this->fill_err_.load(std::memory_order_acquire); }

  /// Network source: the same read-ahead ring and the same read() path as a file, filled by push()
  /// from the network task instead of the storage worker. seek() only goes forward.
  bool open_stream();
  /// Bytes push() takes right now (0 when not open for a stream).
  size_t stream_room();
  /// Network task: append received bytes, at most stream_room() of them.
  void push(const uint8_t *data, size_t len);
  /// Network task: the sender is gone; read() reports the end once the ring is drained.
  void end_stream();
  /// Network task: room() is called once a read frees space in the ring after want_room().
  void set_room_callback(void (*room)(void *), void *arg) {
    this->room_cb_ = room;
    this->room_arg_ = arg;
  }
  void want_room() { this->room_wanted_.store(true); }

  /// `size` bytes from the ring, waiting for the fill chain as needed. Returns bytes read (fewer only
  /// at the end of the file; 0 = EOF, -1 = error or stopped).
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

  /// Main loop, every pass: the storage worker's stream calls are main-loop-only, so the calls the
  /// tasks ask for (open, seek, tell, close, the next fill) are made from here.
  void service();


 protected:
  // --- the one-shot stream calls (begin_read/seek/tell/end_read): made by service(), waited for --
  enum class Op : uint8_t { NONE, BEGIN_READ, END_READ, SEEK, TELL };
  // A task: has service() make the call and waits for its completion.
  storage::StorageError call_(Op op);
  // Main loop: false while the stream is still busy with a step (made again on the next pass).
  bool submit_(Op op, uint32_t seq);
  void on_sync_done_(uint32_t seq, storage::StorageError err) {
    if (seq != this->op_seq_.load(std::memory_order_acquire))
      return;  // of a call given up meanwhile
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
  // Any task: the next fill is wanted; service() submits it.
  void kick_fill_() { this->kick_wanted_.store(true, std::memory_order_release); }
  // Main loop: one non-blocking read_chunk() into the ring's free part if it has room and nothing
  // is in flight.
  void fill_now_();
  // read_chunk completion (main loop): publish what was read into the ring, then chain the next fill.
  void on_fill_done_(storage::StorageError err);
  // Wait (bounded, abortable) for any in-flight fill to finish, so a one-shot stream call can run.
  void quiesce_fill_();

  static constexpr uint32_t WAIT_SLICE_MS = 20;
  static constexpr uint32_t WAIT_CAP_MS = 5000;

  storage::StreamHandle handle_{};
  std::atomic<Op> op_{Op::NONE};
  std::atomic<uint32_t> op_seq_{0};
  std::string op_path_;
  int64_t op_offset_{0};
  storage::SeekMode op_mode_{storage::SeekMode::SEEK_MODE_SET};
  uint64_t op_tell_{0};
  TaskHandle_t loop_task_{nullptr};
  std::atomic<bool> kick_wanted_{false};
  bool open_{false};
  bool network_{false};                       // open_stream(): filled by push(), not the storage worker
  SemaphoreHandle_t stream_lock_{nullptr};    // push()/end_stream() vs close() of a network source
  uint64_t current_position_{0};

  TaskHandle_t waiting_task_{nullptr};
  std::atomic<TaskHandle_t> data_waiter_{nullptr};
  std::atomic<bool> sync_done_{false};
  storage::StorageError sync_result_{storage::StorageError::STORAGE_ERROR_OK};
  const volatile bool *abort_flag_{nullptr};

  // Byte ring with look-ahead: one writer (fill chain or push()), one reader task that read()s and
  // peek()s. Positions run over twice the size, so a full ring and an empty one differ; any size.
  struct LookaheadRing {
    uint8_t *data{nullptr};
    size_t size{0};
    std::atomic<uint32_t> head{0};  // write position
    std::atomic<uint32_t> tail{0};  // read position
    void reset() {
      this->head.store(0, std::memory_order_relaxed);
      this->tail.store(0, std::memory_order_release);
    }
    size_t at(uint32_t pos) const { return pos < this->size ? pos : pos - this->size; }
    uint32_t advance(uint32_t pos, size_t len) const {
      const size_t next = pos + len;
      return static_cast<uint32_t>(next < 2 * this->size ? next : next - 2 * this->size);
    }
    size_t used(uint32_t head_pos, uint32_t tail_pos) const {
      return head_pos >= tail_pos ? head_pos - tail_pos : head_pos + 2 * this->size - tail_pos;
    }
    size_t available() const {
      return this->used(this->head.load(std::memory_order_acquire), this->tail.load(std::memory_order_relaxed));
    }
    size_t free() const {
      return this->size -
             this->used(this->head.load(std::memory_order_acquire), this->tail.load(std::memory_order_acquire));
    }
    void write(const uint8_t *src, size_t len);  // len <= free()
    void commit(size_t len) {                    // len bytes were written at head (len <= free())
      this->head.store(this->advance(this->head.load(std::memory_order_relaxed), len), std::memory_order_release);
    }
    size_t read(uint8_t *dst, size_t len);
    bool peek(size_t offset, uint8_t *dst, size_t len) const;
    void copy_out_(uint32_t pos, uint8_t *dst, size_t len) const;
  };
  LookaheadRing ring_;
  size_t fill_chunk_{FILL_CHUNK};
  std::atomic<bool> fill_in_flight_{false};
  void (*room_cb_)(void *){nullptr};
  void *room_arg_{nullptr};
  std::atomic<bool> room_wanted_{false};
  std::atomic<bool> draining_{false};   // set during close()/seek(): no new fill may start
  size_t fill_got_{0};                  // bytes the in-flight read_chunk reports
  std::atomic<bool> eof_{false};
  std::atomic<bool> fill_err_{false};
};

}  // namespace simple_video_player
}  // namespace esphome
