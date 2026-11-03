#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

#include <driver/parlio_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

#include "iec60958.h"

namespace esphome {
namespace spdif {

/// @brief Drives an IEC 60958 stream out of a single GPIO.
///
/// The pin carries biphase-mark cells at 128 * Fs, produced by the parallel output in one
/// bit wide mode: one cell per output clock. At 48 kHz that is 6.144 MHz, well inside what
/// the peripheral does, and the DMA means the CPU only ever fills buffers.
///
/// Continuity is the whole problem. A receiver locks to the transitions in the stream, so a
/// gap between two DMA descriptors is a dropout, not a pause. Loop transmission solves it:
/// the descriptor chain never stops, and a newly submitted buffer is chained in without the
/// engine going idle. The encoder task keeps that chain fed and writes silence when the
/// ring buffer runs dry, because silence is still a carrier and stopping is not.
class SpdifSpeaker : public speaker::Speaker, public Component {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_buffer_duration_ms(uint32_t ms) { this->buffer_duration_ms_ = ms; }
  /// @brief Core the encoder task runs on, or -1 for no affinity.
  void set_task_core(int8_t core) { this->task_core_ = core; }

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;

 protected:
  static void encoder_task_entry_(void *param);
  void encoder_task_loop_();
  /// @brief Fills one DMA buffer with encoded frames, reading PCM or emitting silence.
  void fill_buffer_(uint8_t *buffer);
  bool start_hardware_();
  void stop_hardware_();

  // One buffer holds this many IEC 60958 frames. 192 is one channel status block, which
  // keeps a buffer boundary from ever falling inside a block and makes the arithmetic below
  // exact: 192 frames is 4 ms at 48 kHz and 3072 bytes of cells.
  static const uint16_t FRAMES_PER_BUFFER = FRAMES_PER_BLOCK;
  static const size_t BUFFER_BYTES = (size_t) FRAMES_PER_BUFFER * CELLS_PER_FRAME / 8;
  static const uint8_t BUFFER_COUNT = 3;
  static const int TASK_STACK_WORDS = 4096;
  static const int TASK_PRIORITY = 10;

  InternalGPIOPin *pin_{nullptr};
  uint32_t buffer_duration_ms_{500};
  int8_t task_core_{-1};

  parlio_tx_unit_handle_t tx_unit_{nullptr};
  uint8_t *buffers_[BUFFER_COUNT]{};
  uint8_t next_buffer_{0};

  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;
  Iec60958Encoder encoder_;
  TaskHandle_t task_{nullptr};

  std::atomic<bool> task_running_{false};
  std::atomic<bool> should_run_{false};
  std::atomic<bool> hardware_failed_{false};
  // Set by the task when it has emitted silence for longer than the receiver needs to stay
  // locked, so loop() can report the speaker as idle without tearing the carrier down.
  std::atomic<bool> underrun_{false};
};

}  // namespace spdif
}  // namespace esphome

#endif  // USE_ESP32
