#include "spdif_speaker.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <cstring>

namespace esphome {
namespace spdif {

static const char *const TAG = "spdif.speaker";

// One output clock per biphase cell.
static const uint8_t CELLS_PER_SAMPLE_PERIOD = CELLS_PER_FRAME;

void SpdifSpeaker::setup() {
  size_t ring_bytes = (size_t) this->audio_stream_info_.ms_to_bytes(this->buffer_duration_ms_);
  this->ring_buffer_ = ring_buffer::RingBuffer::create(ring_bytes);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "could not allocate the %u byte input buffer", (unsigned) ring_bytes);
    this->mark_failed();
    return;
  }

  // DMA capable and internal: the descriptor chain is read by hardware and must not sit in
  // PSRAM, where a refresh stall would show up as a dropout on the wire.
  for (uint8_t i = 0; i < BUFFER_COUNT; i++) {
    this->buffers_[i] = (uint8_t *) heap_caps_calloc(1, BUFFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (this->buffers_[i] == nullptr) {
      ESP_LOGE(TAG, "could not allocate DMA buffer %u of %u bytes", i, (unsigned) BUFFER_BYTES);
      this->mark_failed();
      return;
    }
  }
}

bool SpdifSpeaker::start_hardware_() {
  parlio_tx_unit_config_t config = {};
  config.clk_src = PARLIO_CLK_SRC_DEFAULT;
  config.data_width = 1;  // one cell per clock on one pin
  config.clk_in_gpio_num = GPIO_NUM_NC;
  config.valid_gpio_num = GPIO_NUM_NC;
  // The bit clock stays inside the chip: S/PDIF carries its clock in the transitions, so
  // there is nothing to route out alongside the data.
  config.clk_out_gpio_num = GPIO_NUM_NC;
  // Zero is a valid GPIO number, so every unused lane has to be marked, not left at the
  // value a plain initialiser gives it.
  for (size_t i = 0; i < PARLIO_TX_UNIT_MAX_DATA_WIDTH; i++) {
    config.data_gpio_nums[i] = GPIO_NUM_NC;
  }
  config.data_gpio_nums[0] = (gpio_num_t) this->pin_->get_pin();
  config.output_clk_freq_hz = this->audio_stream_info_.get_sample_rate() * CELLS_PER_SAMPLE_PERIOD;
  config.trans_queue_depth = BUFFER_COUNT;
  config.max_transfer_size = BUFFER_BYTES;
  // The shift edge is left at its default. It decides when data changes relative to the
  // output clock, and that clock is not routed anywhere: a receiver recovers its own from
  // the transitions in the stream.
  //
  // Cells were packed least significant bit first by the encoder, so they have to leave the
  // shift register in that order too. With a one bit bus, LSB order puts bit 0 of a byte on
  // the pin first and bit 7 last.
  config.bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB;

  esp_err_t err = parlio_new_tx_unit(&config, &this->tx_unit_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "parlio_new_tx_unit failed: %s", esp_err_to_name(err));
    return false;
  }
  err = parlio_tx_unit_enable(this->tx_unit_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "parlio_tx_unit_enable failed: %s", esp_err_to_name(err));
    parlio_del_tx_unit(this->tx_unit_);
    this->tx_unit_ = nullptr;
    return false;
  }
  return true;
}

void SpdifSpeaker::stop_hardware_() {
  if (this->tx_unit_ == nullptr) {
    return;
  }
  // No wait_all_done here: a looping transmission has no end to wait for, and disabling is
  // what ends it.
  parlio_tx_unit_disable(this->tx_unit_);
  parlio_del_tx_unit(this->tx_unit_);
  this->tx_unit_ = nullptr;
}

void SpdifSpeaker::start() {
  if (this->is_failed() || this->hardware_failed_.load()) {
    return;
  }
  if (this->task_running_.load()) {
    this->state_ = speaker::STATE_RUNNING;
    return;
  }
  this->should_run_.store(true);
  this->state_ = speaker::STATE_STARTING;

  BaseType_t created = (this->task_core_ < 0)
                           ? xTaskCreate(SpdifSpeaker::encoder_task_entry_, "spdif_enc", TASK_STACK_WORDS, this,
                                         TASK_PRIORITY, &this->task_)
                           : xTaskCreatePinnedToCore(SpdifSpeaker::encoder_task_entry_, "spdif_enc", TASK_STACK_WORDS,
                                                     this, TASK_PRIORITY, &this->task_, this->task_core_);
  if (created != pdPASS) {
    ESP_LOGE(TAG, "could not create the encoder task");
    this->should_run_.store(false);
    this->state_ = speaker::STATE_STOPPED;
  }
}

void SpdifSpeaker::stop() {
  this->should_run_.store(false);
  this->state_ = speaker::STATE_STOPPING;
}

void SpdifSpeaker::finish() {
  // Let what is already buffered reach the wire before the carrier goes away.
  this->stop();
}

bool SpdifSpeaker::has_buffered_data() const {
  return this->ring_buffer_ != nullptr && this->ring_buffer_->available() > 0;
}

size_t SpdifSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed() || this->ring_buffer_ == nullptr) {
    return 0;
  }
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
  }
  return this->ring_buffer_->write_without_replacement((const void *) data, length, ticks_to_wait);
}

void SpdifSpeaker::encoder_task_entry_(void *param) { static_cast<SpdifSpeaker *>(param)->encoder_task_loop_(); }

void SpdifSpeaker::encoder_task_loop_() {
  uint8_t channel_status[CHANNEL_STATUS_BYTES];
  build_channel_status(this->audio_stream_info_.get_sample_rate(), false, channel_status);
  this->encoder_.set_channel_status(channel_status);
  this->encoder_.reset();

  if (!this->start_hardware_()) {
    this->hardware_failed_.store(true);
    this->task_running_.store(false);
    this->task_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  this->task_running_.store(true);

  parlio_transmit_config_t transmit_config = {};
  // The descriptor chain never goes idle. A submitted buffer replaces the looping one at
  // its next wrap, so the line keeps transitioning across the seam and the receiver holds
  // its lock. Stopping between buffers would be a dropout, not silence.
  transmit_config.flags.loop_transmission = true;

  while (this->should_run_.load()) {
    uint8_t *buffer = this->buffers_[this->next_buffer_];
    this->fill_buffer_(buffer);

    esp_err_t err = parlio_tx_unit_transmit(this->tx_unit_, buffer, BUFFER_BYTES * 8, &transmit_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "parlio_tx_unit_transmit failed: %s", esp_err_to_name(err));
      this->hardware_failed_.store(true);
      break;
    }
    this->next_buffer_ = (uint8_t) ((this->next_buffer_ + 1) % BUFFER_COUNT);
  }

  this->stop_hardware_();
  this->task_running_.store(false);
  this->task_ = nullptr;
  vTaskDelete(nullptr);
}

void SpdifSpeaker::fill_buffer_(uint8_t *buffer) {
  static const uint8_t BYTES_PER_FRAME = 4;  // two 16 bit samples
  uint8_t pcm[FRAMES_PER_BUFFER * BYTES_PER_FRAME];

  // A buffer is 4 ms of audio at 48 kHz. Waiting most of that for the ring buffer keeps the
  // task off the CPU while the previous buffer plays, and still leaves room to encode.
  size_t wanted = sizeof(pcm);
  size_t got = this->ring_buffer_->read((void *) pcm, wanted, pdMS_TO_TICKS(3));
  if (got < wanted) {
    // Short read is not an error. The carrier has to keep running whatever the source does,
    // so the remainder of the buffer is silence rather than a gap.
    memset(pcm + got, 0, wanted - got);
  }
  this->underrun_.store(got == 0);

  const int16_t *samples = reinterpret_cast<const int16_t *>(pcm);
  for (uint16_t frame = 0; frame < FRAMES_PER_BUFFER; frame++) {
    // A 16 bit sample occupies time slots 12 to 27, so it is the high end of the 24 bit
    // field the subframe carries.
    int32_t left = ((int32_t) samples[frame * 2]) << 8;
    int32_t right = ((int32_t) samples[frame * 2 + 1]) << 8;
    this->encoder_.encode_frame(left, right, buffer + (size_t) frame * (CELLS_PER_FRAME / 8));
  }
}

void SpdifSpeaker::loop() {
  if (this->hardware_failed_.load()) {
    this->hardware_failed_.store(false);
    this->should_run_.store(false);
    this->state_ = speaker::STATE_STOPPED;
    this->status_set_error("S/PDIF output failed to start");
    return;
  }
  if (this->task_running_.load() && this->state_ == speaker::STATE_STARTING) {
    this->state_ = speaker::STATE_RUNNING;
    this->status_clear_error();
  }
  if (!this->task_running_.load() && this->state_ == speaker::STATE_STOPPING) {
    this->state_ = speaker::STATE_STOPPED;
  }
}

void SpdifSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "S/PDIF speaker:");
  LOG_PIN("  Pin: ", this->pin_);
  ESP_LOGCONFIG(TAG, "  Sample rate: %" PRIu32 " Hz", this->audio_stream_info_.get_sample_rate());
  ESP_LOGCONFIG(TAG, "  Cell rate: %" PRIu32 " Hz",
                this->audio_stream_info_.get_sample_rate() * CELLS_PER_SAMPLE_PERIOD);
  ESP_LOGCONFIG(TAG, "  Input buffer: %" PRIu32 " ms", this->buffer_duration_ms_);
  if (this->task_core_ < 0) {
    ESP_LOGCONFIG(TAG, "  Encoder task core: any");
  } else {
    ESP_LOGCONFIG(TAG, "  Encoder task core: %d", this->task_core_);
  }
}

}  // namespace spdif
}  // namespace esphome

#endif  // USE_ESP32
