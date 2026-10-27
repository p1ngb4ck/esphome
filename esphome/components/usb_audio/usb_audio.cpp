#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_audio.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "usb/usb_helpers.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace esphome {
namespace usb_audio {

static const char *const TAG = "usb_audio";

// Timeout for each blocking control transfer during connection setup.
static constexpr uint32_t CTRL_TIMEOUT_MS = 1000;

static constexpr uint32_t CTRL_TASK_STACK_SIZE = 4096;
// Below the USB host task, so its transfers are not held up by class requests.
static constexpr UBaseType_t CTRL_TASK_PRIORITY = 5;

// Largest data stage any control request in this component uses.
static constexpr size_t UAC_CTRL_MAX_DATA_LEN = 8;

// Audio devices in the field reject a class request now and then and answer the very same
// request a moment later, so one refusal does not mean the control is not there. The Linux
// USB audio mixer allows ten attempts before it gives up on a request; the same count is
// used here. A request the host never completed is not retried: that is a host or link
// problem and repeating it only spends the timeout again.
static constexpr uint8_t UAC_CTRL_ATTEMPTS = 10;

// URBs per stream (triple-buffering).
static constexpr uint8_t ISOC_NUM_URBS = 3;

// Packets per URB - small for audio to minimise latency.
static constexpr uint8_t ISOC_PACKETS_PER_URB = 4;

// Service intervals per second for an isochronous endpoint. At full speed the endpoint is
// serviced every bInterval frames (1 ms each), at high speed every 2^(bInterval-1)
// microframes (125 us each). bInterval 0 is illegal for isochronous endpoints; treat it as
// "every interval" so a sloppy descriptor still yields the common 1 ms / 125 us case.
static uint32_t uac_service_intervals_per_second(bool high_speed, uint8_t b_interval) {
  if (high_speed) {
    const uint8_t exponent = (b_interval >= 1 && b_interval <= 16) ? static_cast<uint8_t>(b_interval - 1) : 0;
    return std::max<uint32_t>(1, 8000u >> exponent);
  }
  const uint32_t period = (b_interval >= 1 && b_interval <= 16) ? b_interval : 1;
  return std::max<uint32_t>(1, 1000u / period);
}


static constexpr uint32_t CAPTURE_TASK_STACK_SIZE = 4096;
static constexpr UBaseType_t CAPTURE_TASK_PRIORITY = 18;
static constexpr uint32_t SINK_TIMER_BASE = 0x1000;
static constexpr uint32_t CAPTURE_TIMER_BASE = 0x2000;
// Upper bound on the time one play() call may spend pushing data into the USB ring buffer.
static constexpr uint32_t MAX_WORK_TIME_MS = 20;
// Share of the work budget a single blocking write may consume. The isochronous sink drains
// at exactly real time, so a chunk worth more playback time than this can only time out.
static constexpr uint32_t WRITE_BUDGET_DIVISOR = 2;
// Window over which the accepted byte rate is measured before it is judged.
static constexpr uint32_t RATE_WINDOW_MS = 1000;
// Fraction of the nominal sample rate the sink must keep up with, in percent. A short write
// is normal backpressure: the sink drains at exactly real time and the caller keeps the
// remainder. Only a sustained shortfall below this means playback is actually breaking up.
static constexpr uint32_t RATE_WARN_PERCENT = 80;
// How long finish() waits for the queue to drain before stopping anyway.
static constexpr uint32_t FINISH_TIMEOUT_MS = 1000;

static std::unique_ptr<ring_buffer::RingBuffer> make_psram_ring(size_t size) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < size)
    return nullptr;
  return ring_buffer::RingBuffer::create(size);
}

static bool same_format(const EndpointConfig &a, const EndpointConfig &b) {
  return a.sample_rate == b.sample_rate && a.bits_per_sample == b.bits_per_sample && a.channels == b.channels;
}

template<typename T> static T *make_in_psram(USBAudioClient *client) {
  RAMAllocator<T> allocator(RAMAllocator<T>::ALLOC_EXTERNAL);
  T *mem = allocator.allocate(1);
  if (mem == nullptr)
    return nullptr;
  return new (mem) T(client);
}

// The pipes (host channels) of what this configuration plays or records only: the streaming interface
// of a direction this block does not drive, the speaker's feedback endpoint while feedback is off,
// and the AudioControl interface (never claimed, its requests go over EP0) get none.
uint32_t USBAudioClient::skipped_endpoints_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                                            const usb_intf_desc_t *alt) const {
  static constexpr uint8_t SUBCLASS_AUDIOSTREAMING = 2;
  static constexpr uint8_t USAGE_FEEDBACK = 1;  // bmAttributes bits 5..4
  uint32_t all = 0;
  bool data_out = false;
  bool data_in = false;
  uint32_t feedback = 0;
  usb_host::for_each_alt_endpoint(config, alt, [&](const usb_ep_desc_t *ep) {
    all |= usb_host::endpoint_bit(ep);
    if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_ISOC)
      return;
    if (((ep->bmAttributes >> 4) & 0x03) == USAGE_FEEDBACK) {
      feedback |= usb_host::endpoint_bit(ep);
    } else if (ep->bEndpointAddress & 0x80) {
      data_in = true;
    } else {
      data_out = true;
    }
  });
  if (alt->bInterfaceClass != USB_CLASS_AUDIO || alt->bInterfaceSubClass != SUBCLASS_AUDIOSTREAMING)
    return all;
  if (data_out) {
    if (!this->spk_cfg_.configured)
      return all;
    // UAC1 asynchronous OUT: the feedback endpoint may be marked as data (bSynchAddress names it)
    if (!this->feedback_enabled_)
      return feedback | (all & 0xFFFF0000u);
    return 0;
  }
  if (data_in && !this->mic_cfg_.configured)
    return all;
  return 0;
}

void USBAudioClient::set_microphone_params(uint8_t channels, uint16_t bits, uint32_t sample_rate) {
  this->mic_cfg_.channels = channels;
  this->mic_cfg_.bits_per_sample = bits;
  this->mic_cfg_.sample_rate = sample_rate;
  this->mic_cfg_.configured = true;
}

void USBAudioClient::set_speaker_params(uint8_t channels, uint16_t bits, uint32_t sample_rate) {
  this->spk_cfg_.channels = channels;
  this->spk_cfg_.bits_per_sample = bits;
  this->spk_cfg_.sample_rate = sample_rate;
  this->spk_cfg_.configured = true;
}

void USBAudioClient::setup() {
  if (this->spk_buf_size_ == 0)
    this->spk_buf_size_ = USB_AUDIO_DEFAULT_BUFFER_SIZE;
  if (this->mic_buf_size_ == 0)
    this->mic_buf_size_ = USB_AUDIO_DEFAULT_BUFFER_SIZE;

  // Remember which task runs the component loop; a blocking control transfer issued from it
  // has to keep the host library's event handler running, see uac_control_transfer_.
  this->loop_task_ = xTaskGetCurrentTaskHandle();

  this->ctrl_queue_ = xQueueCreateStatic(CTRL_QUEUE_LENGTH, sizeof(CtrlRequest), this->ctrl_queue_storage_.data(),
                                         &this->ctrl_queue_struct_);
  if (this->ctrl_queue_ == nullptr ||
      !this->ctrl_task_.create(&USBAudioClient::ctrl_task, "usb_audio_ctl", CTRL_TASK_STACK_SIZE, this,
                               CTRL_TASK_PRIORITY, true, LOOP_TASK_CORE)) {
    this->mark_failed(LOG_STR("Failed to create control task"));
    return;
  }

  if (this->speaker_ != nullptr)
    this->speaker_->set_timer_id(SINK_TIMER_BASE - 1);
  if (this->mic_cfg_.configured) {
    if (this->microphone_ != nullptr)
      this->microphone_->set_timer_id(CAPTURE_TIMER_BASE - 1);
    this->capture_lock_ = xSemaphoreCreateMutexStatic(&this->capture_lock_struct_);
    this->capture_buffer_.reserve(this->mic_buf_size_);
    if (this->capture_lock_ == nullptr ||
        !this->capture_task_.create(&USBAudioClient::capture_task, "usb_mic", CAPTURE_TASK_STACK_SIZE, this,
                                    CAPTURE_TASK_PRIORITY, true, LOOP_TASK_CORE)) {
      this->mark_failed(LOG_STR("Failed to create microphone task"));
      return;
    }
  }

  usb_host::USBClient::setup();
}

void USBAudioClient::loop() {
  // Event-driven, no polling: woken by the client's USB events (USBClient::client_event_cb), the
  // control task's completions, a sink or capture request, usb_host when a stream died or
  // finished closing, and the reopen backoff's timer. Idle it is disabled.
  const bool usb_work = this->process_usb_events_();
  if (const uint32_t errors = this->transfer_errors_.exchange(0, std::memory_order_relaxed); errors != 0)
    ESP_LOGW(TAG, "%" PRIu32 " USB transfer errors", errors);

  bool pending = false;
  uint32_t due_in = UINT32_MAX;
  const uint32_t now = millis();
  for (const auto &dev : this->devices()) {
    auto *device = static_cast<USBAudioDevice *>(dev->context);
    if (device == nullptr)
      continue;
    if (device->loop_())
      pending = true;
    // A wanted stream whose reopen waits for its backoff: woken when it is due.
    if (device->spk_wanted_ && !device->spk_stream_open_ && device->spk_open_fails_ != 0)
      due_in = std::min(due_in, static_cast<uint32_t>(std::max<int32_t>(0, (int32_t) (device->spk_reopen_at_ms_ - now))));
    if (device->mic_wanted_ && !device->mic_stream_open_ && device->mic_open_fails_ != 0)
      due_in = std::min(due_in, static_cast<uint32_t>(std::max<int32_t>(0, (int32_t) (device->mic_reopen_at_ms_ - now))));
  }
  for (USBAudioSink *sink : this->sinks_)
    sink->check_finish();
  if (this->speaker_ != nullptr)
    this->speaker_->check_finish();
  for (USBAudioCapture *capture : this->captures_)
    capture->process();
  if (this->microphone_ != nullptr)
    this->microphone_->process();

  if (due_in != UINT32_MAX)
    this->set_timeout(REOPEN_TIMEOUT_ID, std::max<uint32_t>(1, due_in), [this]() { this->enable_loop(); });
  if (!usb_work && !pending)
    this->disable_loop();
}

bool USBAudioClient::teardown() {
  bool done = true;
  for (const auto &dev : this->devices()) {
    auto *device = static_cast<USBAudioDevice *>(dev->context);
    if (device == nullptr)
      continue;
    device->close_speaker_stream_();
    device->close_microphone_stream_();
    if (device->spk_stream_.pending_urbs.load(std::memory_order_acquire) != 0 ||
        device->spk_fb_stream_.pending_urbs.load(std::memory_order_acquire) != 0 ||
        device->mic_stream_.pending_urbs.load(std::memory_order_acquire) != 0 || device->ctrl_submitting_.load())
      done = false;
  }
  return done;
}

bool USBAudioClient::device_connected() const {
  for (const auto &dev : this->devices()) {
    auto *device = static_cast<USBAudioDevice *>(dev->context);
    if (device != nullptr && device->device_connected())
      return true;
  }
  return false;
}

void USBAudioClient::set_speaker_volume_level(float volume) {
  this->spk_volume_.store(std::clamp(volume, 0.0f, 1.0f));
  for (const auto &dev : this->devices()) {
    if (dev->context != nullptr)
      static_cast<USBAudioDevice *>(dev->context)->set_speaker_volume_level(volume);
  }
}

void USBAudioClient::set_speaker_mute_state(bool mute_state) {
  this->spk_mute_.store(mute_state);
  for (const auto &dev : this->devices()) {
    if (dev->context != nullptr)
      static_cast<USBAudioDevice *>(dev->context)->set_speaker_mute_state(mute_state);
  }
}

USBAudioSink *USBAudioClient::take_sink_() {
  for (USBAudioSink *sink : this->sinks_) {
    if (sink->device() == nullptr && sink->idle())
      return sink;
  }
  auto *sink = make_in_psram<USBAudioSink>(this);
  if (sink == nullptr)
    return nullptr;
  sink->set_timer_id(SINK_TIMER_BASE + this->sinks_.size());
  this->sinks_.push_back(sink);
  return sink;
}

USBAudioCapture *USBAudioClient::take_capture_() {
  for (USBAudioCapture *capture : this->captures_) {
    if (capture->device() == nullptr && capture->idle())
      return capture;
  }
  auto *capture = make_in_psram<USBAudioCapture>(this);
  if (capture == nullptr)
    return nullptr;
  capture->set_timer_id(CAPTURE_TIMER_BASE + this->captures_.size());
  xSemaphoreTake(this->capture_lock_, portMAX_DELAY);
  this->captures_.push_back(capture);
  xSemaphoreGive(this->capture_lock_);
  return capture;
}

void USBAudioClient::make_name_unique_(USBAudioDevice *device) {
  char base[sizeof(device->name_)];
  memcpy(base, device->name_, sizeof(base));
  for (uint8_t n = 2; n < 100; n++) {
    bool taken = false;
    for (const auto &dev : this->devices()) {
      auto *other = static_cast<USBAudioDevice *>(dev->context);
      if (other != nullptr && other != device && strcmp(other->name_, device->name_) == 0)
        taken = true;
    }
    if (!taken)
      return;
    snprintf(device->name_, sizeof(device->name_), "%.40s (%u)", base, n);
  }
}

void USBAudioClient::attach_routes_(USBAudioDevice *device) {
  if (device->spk_format_ok_) {
    for (size_t route = 0; route < this->speaker_routes_.size() && route < USBAudioDevice::MAX_SINKS; route++) {
      auto ring = make_psram_ring(this->spk_buf_size_);
      USBAudioSink *sink = ring != nullptr ? this->take_sink_() : nullptr;
      if (sink == nullptr) {
        ESP_LOGE(TAG, "%s: no PSRAM for its speaker buffer", device->name());
        break;
      }
      device->slots_[route].ring = std::move(ring);
      sink->attach(device, device->slots_[route].ring.get());
      device->slots_[route].sink.store(sink);
      const EndpointConfig &fmt = device->spk_fmt_;
      this->speaker_routes_[route](sink, device->name(), device->key(), fmt.sample_rate,
                                   static_cast<uint8_t>(fmt.bits_per_sample), fmt.channels, true);
    }
  }
  if (device->mic_format_ok_ && !this->microphone_routes_.empty()) {
    auto ring = make_psram_ring(this->mic_buf_size_);
    USBAudioCapture *capture = ring != nullptr ? this->take_capture_() : nullptr;
    if (capture == nullptr) {
      ESP_LOGE(TAG, "%s: no PSRAM for its microphone buffer", device->name());
      return;
    }
    device->mic_rb_ = std::move(ring);
    device->capture_ = capture;
    capture->attach(device, device->mic_rb_.get());
    for (auto &route : this->microphone_routes_)
      route(capture, device->name(), device->key(), true);
  }
}

void USBAudioClient::attach_fixed_() {
  const size_t slot = this->speaker_routes_.size();
  if (this->speaker_ != nullptr && this->speaker_->device() == nullptr && slot < USBAudioDevice::MAX_SINKS) {
    for (const auto &dev : this->devices()) {
      auto *device = static_cast<USBAudioDevice *>(dev->context);
      if (device == nullptr || device->gone_ || !device->spk_format_ok_ || device->slots_[slot].ring != nullptr ||
          !same_format(device->spk_fmt_, this->spk_cfg_))
        continue;
      device->slots_[slot].ring = make_psram_ring(this->spk_buf_size_);
      if (device->slots_[slot].ring == nullptr) {
        ESP_LOGE(TAG, "%s: no PSRAM for its speaker buffer", device->name());
        break;
      }
      this->speaker_->attach(device, device->slots_[slot].ring.get());
      device->slots_[slot].sink.store(this->speaker_);
      break;
    }
  }
  if (this->microphone_ != nullptr && this->microphone_->device() == nullptr) {
    for (const auto &dev : this->devices()) {
      auto *device = static_cast<USBAudioDevice *>(dev->context);
      if (device == nullptr || device->gone_ || !device->mic_format_ok_ || device->capture_ != nullptr ||
          !same_format(device->mic_fmt_, this->mic_cfg_))
        continue;
      device->mic_rb_ = make_psram_ring(this->mic_buf_size_);
      if (device->mic_rb_ == nullptr) {
        ESP_LOGE(TAG, "%s: no PSRAM for its microphone buffer", device->name());
        break;
      }
      device->capture_ = this->microphone_;
      this->microphone_->attach(device, device->mic_rb_.get());
      break;
    }
  }
}

void USBAudioClient::release_device_(USBAudioDevice *device) {
  for (size_t slot = 0; slot < USBAudioDevice::MAX_SINKS; slot++) {
    USBAudioSink *sink = device->slots_[slot].sink.load();
    if (sink == nullptr)
      continue;
    if (slot < this->speaker_routes_.size())
      this->speaker_routes_[slot](sink, device->name(), device->key(), 0, 0, 0, false);
    sink->detach();
  }
  USBAudioCapture *capture = device->capture_;
  if (capture != nullptr) {
    if (capture != this->microphone_) {
      for (auto &route : this->microphone_routes_)
        route(capture, device->name(), device->key(), false);
    }
    capture->detach();
    device->capture_ = nullptr;
  }
}

void USBAudioClient::on_device_connected(usb_host::UsbDevice &dev) {
  RAMAllocator<USBAudioDevice> allocator(RAMAllocator<USBAudioDevice>::ALLOC_EXTERNAL);
  USBAudioDevice *mem = allocator.allocate(1);
  if (mem == nullptr) {
    ESP_LOGE(TAG, "Device %d: no PSRAM for its context", dev.addr);
    this->close_device(dev);
    return;
  }
  auto *device = new (mem) USBAudioDevice(this, &dev);
  dev.context = device;
  device->on_connected();
  this->make_name_unique_(device);
  this->attach_routes_(device);
  this->attach_fixed_();
  this->enable_loop();
}

void USBAudioClient::on_device_disconnected(usb_host::UsbDevice &dev) {
  auto *device = static_cast<USBAudioDevice *>(dev.context);
  if (device == nullptr)
    return;
  device->on_disconnected();
  this->release_device_(device);
  this->attach_fixed_();
}

void USBAudioClient::free_device_context(usb_host::UsbDevice &dev) {
  auto *device = static_cast<USBAudioDevice *>(dev.context);
  if (device == nullptr)
    return;
  dev.context = nullptr;
  device->~USBAudioDevice();
  RAMAllocator<USBAudioDevice> allocator(RAMAllocator<USBAudioDevice>::ALLOC_EXTERNAL);
  allocator.deallocate(device, 1);
}

bool USBAudioClient::device_busy_(const usb_host::UsbDevice &dev) const {
  const auto *device = static_cast<const USBAudioDevice *>(dev.context);
  return device != nullptr && device->busy_();
}

void USBAudioClient::on_stream_packet(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error,
                                      bool skipped) {
  if (error && this->transfer_errors_.fetch_add(1, std::memory_order_relaxed) == 0)
    this->enable_loop_soon_any_context();
  USBAudioDevice *device = device_of_(stream);
  if (device != nullptr)
    device->on_stream_packet_(stream, data, len, error);
}

size_t USBAudioClient::on_stream_fill(usb_host::IsocStream &stream, uint8_t *data, size_t max_len) {
  USBAudioDevice *device = device_of_(stream);
  return device != nullptr ? device->fill_(data, max_len) : 0;
}

void USBAudioClient::on_stream_out_error(usb_host::IsocStream &stream) {
  if (this->transfer_errors_.fetch_add(1, std::memory_order_relaxed) == 0)
    this->enable_loop_soon_any_context();
}

void USBAudioClient::on_stream_open(usb_host::IsocStream &stream, bool ok) {
  USBAudioDevice *device = device_of_(stream);
  if (device != nullptr)
    device->on_stream_open_(stream, ok);
}

bool USBAudioClient::post_ctrl_(CtrlOp op, USBAudioDevice *device, uint32_t seq) {
  const CtrlRequest request{op, device, device->ctrl_gen_.load(), seq};
  return xQueueSend(this->ctrl_queue_, &request, 0) == pdTRUE;
}

void USBAudioClient::ctrl_task(void *param) {
  auto *self = static_cast<USBAudioClient *>(param);
  CtrlRequest request;
  while (true) {
    if (xQueueReceive(self->ctrl_queue_, &request, portMAX_DELAY) == pdTRUE)
      request.device->run_ctrl_(request);
  }
}

void USBAudioClient::capture_task(void *param) { static_cast<USBAudioClient *>(param)->capture_task_loop_(); }

void USBAudioClient::capture_task_loop_() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool more = true;
    while (more) {
      more = false;
      for (size_t i = 0;; i++) {
        xSemaphoreTake(this->capture_lock_, portMAX_DELAY);
        USBAudioCapture *capture = nullptr;
        if (i < this->captures_.size()) {
          capture = this->captures_[i];
        } else if (i == this->captures_.size()) {
          capture = this->microphone_;
        }
        if (capture == nullptr) {
          xSemaphoreGive(this->capture_lock_);
          if (i >= this->captures_.size())
            break;
          continue;
        }
        if (!capture->running()) {
          xSemaphoreGive(this->capture_lock_);
          continue;
        }
        capture->set_delivering(true);
        const size_t received = capture->read(this->capture_buffer_);
        xSemaphoreGive(this->capture_lock_);
        if (received > 0) {
          capture->deliver(this->capture_buffer_);
          more = true;
        }
        capture->set_delivering(false);
        if (capture->stopping())
          this->enable_loop_soon_any_context();
      }
    }
  }
}

void USBAudioClient::wake_capture_task_() {
  TaskHandle_t task = this->capture_task_.get_handle();
  if (task != nullptr)
    xTaskNotifyGive(task);
}

void USBAudioSink::attach(USBAudioDevice *device, ring_buffer::RingBuffer *ring) {
  this->ring_.store(ring);
  this->device_.store(device);
  this->warned_unavailable_ = false;
}

void USBAudioSink::detach() {
  this->device_.store(nullptr);
  this->ring_.store(nullptr);
  this->client_->enable_loop();
}

void USBAudioSink::start() {
  if (this->state_ == speaker::STATE_RUNNING)
    return;
  this->last_play_ms_.store(millis(), std::memory_order_relaxed);
  USBAudioDevice *device = this->device_.load();
  if (device == nullptr || !device->speaker_available()) {
    // No device, or an unusable format: a state to report once, not an error to repeat.
    if (!this->warned_unavailable_)
      ESP_LOGW(TAG, "USB audio stream not available yet");
    this->warned_unavailable_ = true;
    return;
  }
  this->warned_unavailable_ = false;
  this->active_.store(true);
  this->state_ = speaker::STATE_RUNNING;
  // The device opens the stream on the client's main loop; writes wait for it.
  this->client_->enable_loop_soon_any_context();
}

void USBAudioSink::stop() {
  if (this->state_ == speaker::STATE_STOPPED)
    return;
  this->active_.store(false);
  this->state_ = speaker::STATE_STOPPED;
  this->finish_requested_.store(false);
  if (this->finish_timer_set_) {
    this->finish_timer_set_ = false;
    this->client_->cancel_timeout(this->timer_id_);
  }
  this->paused_.store(false);
  this->rate_window_start_ms_ = 0;
  this->client_->enable_loop_soon_any_context();
}

void USBAudioSink::set_pause_state(bool pause_state) {
  if (this->paused_.load() == pause_state)
    return;
  this->paused_.store(pause_state);
  if (!pause_state)
    this->rate_window_start_ms_ = 0;
}

void USBAudioSink::finish() {
  if (!this->is_running()) {
    this->stop();
    return;
  }
  this->finish_requested_.store(true);
  this->client_->enable_loop_soon_any_context();
}

void USBAudioSink::check_finish() {
  if (!this->finish_requested_.load())
    return;
  if (!this->finish_timer_set_) {
    this->finish_timer_set_ = true;
    this->finish_deadline_ms_ = millis() + FINISH_TIMEOUT_MS;
    this->client_->set_timeout(this->timer_id_, FINISH_TIMEOUT_MS, [this]() { this->client_->enable_loop(); });
  }
  if (!this->has_buffered_data() || (int32_t) (millis() - this->finish_deadline_ms_) >= 0)
    this->stop();
}

size_t USBAudioSink::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  // The caller's block time is the budget; MAX_WORK_TIME_MS only caps how long one call may
  // hold the audio task.
  uint32_t time_budget_ms = MAX_WORK_TIME_MS;
  if (ticks_to_wait != portMAX_DELAY) {
    const uint32_t ticks_ms = static_cast<uint32_t>(ticks_to_wait) * portTICK_PERIOD_MS;
    if (ticks_ms > 0) {
      time_budget_ms = std::min(time_budget_ms, ticks_ms);
    }
  }
  return this->play_internal_(data, length, time_budget_ms);
}

size_t USBAudioSink::play(const uint8_t *data, size_t length) {
  return this->play_internal_(data, length, MAX_WORK_TIME_MS);
}

size_t USBAudioSink::play_internal_(const uint8_t *data, size_t length, uint32_t time_budget_ms) {
  this->last_play_ms_.store(millis(), std::memory_order_relaxed);
  this->writers_.fetch_add(1);
  USBAudioDevice *device = this->device_.load();
  ring_buffer::RingBuffer *ring = this->ring_.load();
  if (!this->is_running() && device != nullptr)
    this->start();
  if (!this->is_running() || device == nullptr || ring == nullptr) {
    this->writers_.fetch_sub(1);
    if (device == nullptr)
      this->client_->enable_loop_soon_any_context();
    // No device: the caller waits its budget, as on a full buffer, instead of spinning.
    vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(std::max<uint32_t>(1, time_budget_ms))));
    return 0;
  }
  if (data == nullptr || length == 0) {
    this->writers_.fetch_sub(1);
    return 0;
  }

  const EndpointConfig &cfg = device->speaker_format();
  const uint32_t work_budget_ms =
      (time_budget_ms > 0) ? std::min<uint32_t>(time_budget_ms, MAX_WORK_TIME_MS) : MAX_WORK_TIME_MS;
  const uint32_t start_ms = millis();

  const size_t bytes_per_frame =
      std::max<size_t>(1, static_cast<size_t>(cfg.channels) * std::max<size_t>(1, cfg.bits_per_sample / 8));

  // Write granularity is the isochronous packet the sink consumes per service interval, so a
  // write never leaves a partial packet behind. Fall back to a single audio frame while the
  // stream is not open yet and the packet size is therefore unknown.
  size_t packet_bytes = device->get_speaker_packet_bytes();
  if (packet_bytes < bytes_per_frame) {
    packet_bytes = bytes_per_frame;
  }

  // The sink drains at real time and never faster: sample_rate frames per second. Asking a
  // blocking write for more playback time than it may block for can only ever time out, so
  // cap one chunk at what drains within a fraction of the budget. Deriving this from the
  // stream format keeps it correct for any rate the endpoint was opened with.
  const size_t bytes_per_ms = std::max<size_t>(1, (static_cast<size_t>(cfg.sample_rate) * bytes_per_frame) / 1000);
  size_t max_chunk = bytes_per_ms * std::max<uint32_t>(1, work_budget_ms / WRITE_BUDGET_DIVISOR);
  max_chunk = (max_chunk / packet_bytes) * packet_bytes;
  if (max_chunk == 0) {
    max_chunk = packet_bytes;
  }

  size_t total_written = 0;
  size_t remaining = length;
  const uint8_t *current = data;

  while (remaining >= bytes_per_frame) {
    const uint32_t elapsed_ms = millis() - start_ms;
    if (elapsed_ms >= work_budget_ms) {
      break;
    }

    // Whole packets while there is enough left for one, whole frames for the tail.
    size_t chunk = std::min(remaining, max_chunk);
    size_t aligned = (chunk / packet_bytes) * packet_bytes;
    if (aligned == 0) {
      aligned = (chunk / bytes_per_frame) * bytes_per_frame;
    }
    chunk = aligned;
    if (chunk == 0) {
      break;
    }

    uint32_t call_timeout_ms = work_budget_ms - elapsed_ms;
    if (call_timeout_ms == 0) {
      call_timeout_ms = 1;
    }

    // A short write is a normal outcome: the sink drains at exactly real time and the
    // caller keeps the remainder. Retrying with a smaller chunk cannot help, because the
    // drain rate is fixed by the sample clock. Also while the stream still opens: the audio
    // goes into the ring (the stream starts prefilled), and a full ring blocks the caller on
    // its space - the drain is the event, nothing sleeps blind.
    const TickType_t ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(call_timeout_ms));
    if (ring->write_without_replacement(current, chunk, ticks, false) != chunk)
      break;

    total_written += chunk;
    current += chunk;
    remaining -= chunk;

    const size_t frames_written = chunk / bytes_per_frame;
    if (frames_written > 0) {
      const int64_t timestamp_us = esp_timer_get_time();
      this->audio_output_callback_(static_cast<uint32_t>(frames_written), timestamp_us);
    }
  }

  // Only a stream that runs can fall behind; one still opening (or without a free host channel)
  // takes nothing, which is not an underrun.
  if (device->get_speaker_packet_bytes() != 0) {
    this->check_throughput_(total_written, bytes_per_ms);
  } else {
    this->rate_window_start_ms_ = 0;
  }
  this->writers_.fetch_sub(1);
  return total_written;
}

// Judge the sink by throughput, not by whether a single write was short. Over a one second
// window the accepted bytes have to track the sample clock; a sustained shortfall is the one
// symptom that actually means the audio is breaking up (a stream that stopped draining, or a
// device that fell behind), and it is worth one warning per window.
void USBAudioSink::check_throughput_(size_t accepted, size_t bytes_per_ms) {
  const uint32_t now = millis();
  if (this->rate_window_start_ms_ == 0) {
    this->rate_window_start_ms_ = now;
    this->rate_window_bytes_ = accepted;
    return;
  }
  this->rate_window_bytes_ += accepted;
  const uint32_t elapsed_ms = now - this->rate_window_start_ms_;
  if (elapsed_ms < RATE_WINDOW_MS)
    return;

  const size_t expected = bytes_per_ms * elapsed_ms;
  const size_t floor_bytes = expected / 100 * RATE_WARN_PERCENT;
  if (this->rate_window_bytes_ < floor_bytes) {
    ESP_LOGW(TAG, "Speaker underrunning: accepted %u of %u bytes over %" PRIu32 " ms",
             (unsigned) this->rate_window_bytes_, (unsigned) expected, elapsed_ms);
  }
  this->rate_window_start_ms_ = now;
  this->rate_window_bytes_ = 0;
}

bool USBAudioSink::has_buffered_data() const {
  if (!this->is_running())
    return false;
  // Ask the queue rather than inferring it from the time of the last write, which reports
  // "buffered" for a stream that has long since drained and "empty" for a paused one.
  auto *self = const_cast<USBAudioSink *>(this);
  self->writers_.fetch_add(1);
  ring_buffer::RingBuffer *ring = this->ring_.load();
  const bool buffered = ring != nullptr && ring->available() > 0;
  self->writers_.fetch_sub(1);
  return buffered;
}

// The base class already forwards to a configured audio_dac. When that dac is the
// usb_audio platform it drives the same Feature Unit, so forwarding here as well would
// send every request twice.
void USBAudioSink::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);
#ifdef USE_AUDIO_DAC
  if (this->has_audio_dac_())
    return;
#endif
  this->client_->spk_volume_.store(std::clamp(volume, 0.0f, 1.0f));
  USBAudioDevice *device = this->device_.load();
  if (device != nullptr)
    device->set_speaker_volume_level(volume);
}

void USBAudioSink::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);
#ifdef USE_AUDIO_DAC
  if (this->has_audio_dac_())
    return;
#endif
  this->client_->spk_mute_.store(mute_state);
  USBAudioDevice *device = this->device_.load();
  if (device != nullptr)
    device->set_speaker_mute_state(mute_state);
}

void USBAudioCapture::attach(USBAudioDevice *device, ring_buffer::RingBuffer *ring) {
  const EndpointConfig &cfg = device->microphone_format();
  this->audio_stream_info_ = audio::AudioStreamInfo(cfg.bits_per_sample, cfg.channels, cfg.sample_rate);
  xSemaphoreTake(this->client_->capture_lock_, portMAX_DELAY);
  this->ring_ = ring;
  xSemaphoreGive(this->client_->capture_lock_);
  this->device_ = device;
  if (this->state_ == microphone::STATE_STARTING && this->listeners_.load() > 0)
    this->open_stream_();
}

void USBAudioCapture::detach() {
  xSemaphoreTake(this->client_->capture_lock_, portMAX_DELAY);
  this->ring_ = nullptr;
  xSemaphoreGive(this->client_->capture_lock_);
  this->device_ = nullptr;
  this->client_->cancel_timeout(this->timer_id_);
  if (this->state_ == microphone::STATE_RUNNING)
    this->state_ = microphone::STATE_STARTING;
}

void USBAudioCapture::request_(uint8_t set, uint8_t clear) {
  this->commands_.fetch_and(static_cast<uint8_t>(~clear));
  this->commands_.fetch_or(set);
  this->client_->enable_loop_soon_any_context();
}

// Every MicrophoneSource forwards its own start() and stop(), so capture runs while any of
// them is listening, like i2s_audio's listener count.
void USBAudioCapture::start() {
  uint8_t listeners = this->listeners_.load();
  do {
    if (listeners >= MAX_LISTENERS) {
      return;
    }
  } while (!this->listeners_.compare_exchange_weak(listeners, listeners + 1));
  if (listeners == 0) {
    this->request_(COMMAND_START, COMMAND_STOP);
  }
}

void USBAudioCapture::stop() {
  uint8_t listeners = this->listeners_.load();
  do {
    if (listeners == 0) {
      return;
    }
  } while (!this->listeners_.compare_exchange_weak(listeners, listeners - 1));
  if (listeners == 1) {
    this->request_(COMMAND_STOP, COMMAND_START);
  }
}

void USBAudioCapture::process() {
  const uint8_t commands = this->commands_.exchange(0);
  if (commands & COMMAND_STOP) {
    this->client_->cancel_timeout(this->timer_id_);
    this->retry_delay_ms_ = RETRY_MIN_MS;
    if (this->state_ == microphone::STATE_STARTING || this->state_ == microphone::STATE_RUNNING) {
      if (this->device_ != nullptr)
        this->device_->suspend_microphone();
      xSemaphoreTake(this->client_->capture_lock_, portMAX_DELAY);
      this->running_ = false;
      xSemaphoreGive(this->client_->capture_lock_);
      this->state_ = microphone::STATE_STOPPING;
    }
  }
  if (this->state_ == microphone::STATE_STOPPING && !this->delivering_.load()) {
    this->state_ = microphone::STATE_STOPPED;
  }
  if ((commands & COMMAND_START) && this->listeners_ > 0) {
    if (this->state_ == microphone::STATE_STOPPED) {
      this->state_ = microphone::STATE_STARTING;
      this->open_stream_();
    } else if (this->state_ == microphone::STATE_STOPPING) {
      // Started again once the task is idle.
      this->commands_.fetch_or(COMMAND_START);
    }
  }
}

// Answered by on_stream_event() when the open is still under way; a device that is not there
// yet opens the stream itself once it connects.
void USBAudioCapture::open_stream_() {
  if (this->device_ == nullptr)
    return;
  if (!this->device_->resume_microphone()) {
    if (this->device_->device_connected()) {
      this->on_stream_event(MicStreamEvent::FAILED);
    }
    return;
  }
  if (this->device_->microphone_stream_open()) {
    this->set_running_();
  }
}

void USBAudioCapture::set_running_() {
  this->state_ = microphone::STATE_RUNNING;
  this->retry_delay_ms_ = RETRY_MIN_MS;
  this->running_ = true;
  this->client_->wake_capture_task_();
}

void USBAudioCapture::on_stream_event(MicStreamEvent event) {
  switch (event) {
    case MicStreamEvent::OPEN:
      if (this->state_ == microphone::STATE_STARTING) {
        this->set_running_();
      }
      break;
    case MicStreamEvent::LOST:
      if (this->state_ == microphone::STATE_RUNNING) {
        ESP_LOGW(TAG, "Microphone stream lost, waiting for it to reopen");
        this->state_ = microphone::STATE_STARTING;
        // A stream that died with the device still there is not reopened by a reconnect.
        if (this->device_ != nullptr && this->device_->device_connected()) {
          this->on_stream_event(MicStreamEvent::FAILED);
        }
      }
      break;
    case MicStreamEvent::FAILED:
      if (this->state_ == microphone::STATE_STARTING && this->listeners_ > 0) {
        this->client_->set_timeout(this->timer_id_, this->retry_delay_ms_, [this]() {
          if (this->listeners_ > 0 && this->state_ == microphone::STATE_STARTING) {
            this->open_stream_();
          }
        });
        this->retry_delay_ms_ = std::min(this->retry_delay_ms_ * 2, RETRY_MAX_MS);
      }
      break;
  }
}

size_t USBAudioCapture::read(std::vector<uint8_t> &buffer) {
  if (this->ring_ == nullptr)
    return 0;
  buffer.resize(buffer.capacity());
  const size_t received = this->ring_->read(buffer.data(), buffer.size(), 0);
  buffer.resize(received);
  return received;
}

void USBAudioCapture::deliver(std::vector<uint8_t> &buffer) {
  this->apply_channel_selection_(buffer.data(), buffer.size());
  this->data_callbacks_.call(buffer);
}

void USBAudioCapture::apply_channel_selection_(uint8_t *data, size_t length) {
  if (this->enabled_mask_ == 0) {
    return;
  }
  const uint8_t channels = this->audio_stream_info_.get_channels();
  const size_t bytes_per_sample = this->audio_stream_info_.get_bits_per_sample() / 8U;
  const size_t channel_stride = static_cast<size_t>(channels) * bytes_per_sample;
  if (bytes_per_sample == 0 || channel_stride == 0 || (length % channel_stride) != 0) {
    if (!this->logged_invalid_frame_) {
      ESP_LOGW(TAG, "Unable to apply channel selection: unexpected frame size (%u bytes)",
               static_cast<unsigned int>(length));
      this->logged_invalid_frame_ = true;
    }
    return;
  }
  const uint8_t primary_channel = static_cast<uint8_t>(__builtin_ctz(this->enabled_mask_));
  const size_t sample_count = length / channel_stride;
  for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
    uint8_t *sample_base = data + sample_index * channel_stride;
    const uint8_t *primary_source = sample_base + primary_channel * bytes_per_sample;
    for (size_t channel_index = 0; channel_index < channels; ++channel_index) {
      if (!(this->enabled_mask_ & (1u << channel_index))) {
        std::memcpy(sample_base + channel_index * bytes_per_sample, primary_source, bytes_per_sample);
      }
    }
  }
}

USBAudioDevice::USBAudioDevice(USBAudioClient *client, usb_host::UsbDevice *dev) : client_(client), dev_(dev) {
  this->spk_vol_req_.store(client->spk_volume_.load());
  this->spk_mute_req_.store(client->spk_mute_.load());
}

void USBAudioDevice::read_identity_() {
  const usb_device_desc_t *desc = this->dev_->desc;
  char id[12];
  snprintf(id, sizeof(id), "%04X:%04X", desc != nullptr ? desc->idVendor : 0, desc != nullptr ? desc->idProduct : 0);
  std::string identity = id;
  RAMAllocator<usb_host::UsbDeviceInfo> allocator(RAMAllocator<usb_host::UsbDeviceInfo>::ALLOC_EXTERNAL);
  usb_host::UsbDeviceInfo *info = allocator.allocate(1);
  if (info != nullptr && this->client_->get_device_info(*this->dev_, *info)) {
    snprintf(this->name_, sizeof(this->name_), "USB: %.40s", info->product[0] != '\0' ? info->product : id);
    identity += ':';
    identity += info->serial_number;
  } else {
    snprintf(this->name_, sizeof(this->name_), "USB: %s", id);
  }
  if (info != nullptr)
    allocator.deallocate(info, 1);
  this->key_ = fnv1_hash(identity);
}

void USBAudioDevice::wake_loop() { this->client_->enable_loop_soon_any_context(); }

bool USBAudioDevice::has_speaker_rings_() const {
  for (const auto &slot : this->slots_) {
    if (slot.ring != nullptr)
      return true;
  }
  return false;
}

void USBAudioDevice::flush_speaker_buffer_() {
  for (auto &slot : this->slots_) {
    if (slot.ring != nullptr)
      slot.ring->reset();
  }
}

bool USBAudioDevice::busy_() const {
  if (this->ctrl_refs_.load() != 0 || this->ctrl_submitting_.load())
    return true;
  if (!this->spk_stream_.is_closed() || !this->spk_fb_stream_.is_closed() || !this->mic_stream_.is_closed())
    return true;
  for (const auto &slot : this->slots_) {
    const USBAudioSink *sink = slot.sink.load();
    if (sink != nullptr && sink->writing())
      return true;
  }
  return false;
}

bool USBAudioDevice::loop_() {
  if (this->setup_pending_ && this->setup_done_seq_.load() == this->setup_seq_)
    this->finish_connect_();
  if (this->spk_rate_pending_ && this->spk_rate_done_seq_.load() == this->spk_rate_seq_) {
    this->spk_rate_pending_ = false;
    this->finish_speaker_open_();
  }
  if (this->mic_rate_pending_ && this->mic_rate_done_seq_.load() == this->mic_rate_seq_) {
    this->mic_rate_pending_ = false;
    this->finish_microphone_open_();
  }

  // A stream can stop on its own: the device went away mid-transfer, or the periodic
  // scheduler rejected every resubmission until no URB was left. Nothing is drained after
  // that, so every write would block until timeout forever. Tear it down here so the state
  // reflects reality and the normal reopen path (with backoff) can take over.
  if (this->spk_stream_open_ && this->spk_stream_.died.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Speaker stream stopped unexpectedly; closing");
    this->close_speaker_stream_();
    this->note_open_result_(false, this->spk_reopen_at_ms_, this->spk_open_fails_);
  }
  if (this->mic_stream_open_ && this->mic_stream_.died.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Microphone stream stopped unexpectedly; closing");
    this->close_microphone_stream_();
    this->note_open_result_(false, this->mic_reopen_at_ms_, this->mic_open_fails_);
  }

  if (!this->gone_) {
    bool wanted = false;
    for (const auto &slot : this->slots_) {
      const USBAudioSink *sink = slot.sink.load();
      if (sink != nullptr && sink->device() == this && sink->wanted())
        wanted = true;
    }
    if (wanted != this->spk_wanted_) {
      this->spk_wanted_ = wanted;
      if (wanted) {
        this->client_->cancel_timeout(this->timer_id_(0));
      } else if (this->client_->stream_idle_timeout_ms_ != 0) {
        this->client_->set_timeout(this->timer_id_(0), this->client_->stream_idle_timeout_ms_,
                                   [this]() { this->close_idle_speaker_(); });
      }
    }
  }

  // A stream in use but not open: its open failed (no free host channel, a died stream) or
  // it was started again while an idle close still drained. Opened again, paced by the
  // backoff, once the previous one is fully closed.
  if (this->spk_wanted_ && !this->spk_stream_open_ && this->device_connected_ && this->client_->spk_cfg_.configured &&
      this->spk_format_ok_ && this->spk_alt_.valid && this->spk_stream_.is_closed() &&
      this->spk_fb_stream_.is_closed() && this->reopen_due_(this->spk_reopen_at_ms_, this->spk_open_fails_))
    this->ensure_started_speaker_();
  if (this->mic_wanted_ && !this->mic_stream_open_ && this->device_connected_ && this->client_->mic_cfg_.configured &&
      this->mic_format_ok_ && this->mic_alt_.valid && this->mic_stream_.is_closed() &&
      this->reopen_due_(this->mic_reopen_at_ms_, this->mic_open_fails_))
    this->ensure_started_microphone_();

  return this->setup_pending_ || this->spk_rate_pending_ || this->mic_rate_pending_;
}

void USBAudioDevice::on_stream_packet_(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error) {
  // Async OUT feedback endpoint: the device reports its desired rate (samples per service
  // interval, Q10.14 at full speed / Q16.16 at high speed). Feed it into the OUT pacing.
  if (&stream == &this->spk_fb_stream_) {
    const size_t need = this->device_is_high_speed_ ? 4u : 3u;
    if (!error && len >= need) {
      uint32_t value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                       (static_cast<uint32_t>(data[2]) << 16);
      if (this->device_is_high_speed_)
        value |= static_cast<uint32_t>(data[3]) << 24;
      if (value != 0)
        this->spk_stream_.fb_value.store(value, std::memory_order_relaxed);
    }
    return;
  }

  if (error || len == 0 || &stream != &this->mic_stream_)
    return;

  // -- Microphone IN: write PCM into the ring buffer --------------------------
  if (this->mic_rb_ == nullptr || this->mic_suspended_)
    return;
  // Non-blocking send; if full, drop oldest data (overwrite) is not possible with
  // RINGBUF_TYPE_BYTEBUF, so we just skip if full to avoid blocking.
  if (this->mic_alt_.channel_map_active) {
    this->push_mapped_capture_(data, len);
  } else {
    this->mic_rb_->write_without_replacement(data, len, 0, false);
  }
  this->client_->wake_capture_task_();
}

// -- Speaker OUT: drain the ring buffer into this packet's payload area ------
// max_len is the sample-clock packet size the pacing layer chose for this packet. What is
// not written is zero filled by the caller, so a short read is silence.
size_t USBAudioDevice::fill_(uint8_t *data, size_t max_len) {
  if (this->spk_alt_.channel_map_active)
    return this->fill_mapped_(data, max_len);
  return this->read_mixed_(data, max_len);
}

static void mix_add(uint8_t *dst, const uint8_t *src, size_t len, size_t sample) {
  switch (sample) {
    case 1:
      for (size_t i = 0; i < len; i++) {
        const int32_t sum = static_cast<int8_t>(dst[i]) + static_cast<int8_t>(src[i]);
        dst[i] = static_cast<uint8_t>(static_cast<int8_t>(std::clamp<int32_t>(sum, INT8_MIN, INT8_MAX)));
      }
      break;
    case 2:
      for (size_t i = 0; i + 2 <= len; i += 2) {
        int16_t a;
        int16_t b;
        memcpy(&a, dst + i, 2);
        memcpy(&b, src + i, 2);
        const int16_t sum = static_cast<int16_t>(std::clamp<int32_t>(int32_t(a) + b, INT16_MIN, INT16_MAX));
        memcpy(dst + i, &sum, 2);
      }
      break;
    case 3:
      for (size_t i = 0; i + 3 <= len; i += 3) {
        const int32_t a = static_cast<int32_t>((uint32_t(dst[i]) << 8) | (uint32_t(dst[i + 1]) << 16) |
                                               (uint32_t(dst[i + 2]) << 24)) >> 8;
        const int32_t b = static_cast<int32_t>((uint32_t(src[i]) << 8) | (uint32_t(src[i + 1]) << 16) |
                                               (uint32_t(src[i + 2]) << 24)) >> 8;
        const int32_t sum = std::clamp<int32_t>(a + b, -8388608, 8388607);
        dst[i] = static_cast<uint8_t>(sum);
        dst[i + 1] = static_cast<uint8_t>(sum >> 8);
        dst[i + 2] = static_cast<uint8_t>(sum >> 16);
      }
      break;
    case 4:
      for (size_t i = 0; i + 4 <= len; i += 4) {
        int32_t a;
        int32_t b;
        memcpy(&a, dst + i, 4);
        memcpy(&b, src + i, 4);
        const int32_t sum = static_cast<int32_t>(std::clamp<int64_t>(int64_t(a) + b, INT32_MIN, INT32_MAX));
        memcpy(dst + i, &sum, 4);
      }
      break;
    default:
      break;
  }
}

size_t USBAudioDevice::read_mixed_(uint8_t *dst, size_t len) {
  const size_t sample = this->spk_alt_.sub_frame_size;
  size_t produced = 0;
  bool first = true;
  for (auto &slot : this->slots_) {
    USBAudioSink *sink = slot.sink.load(std::memory_order_acquire);
    ring_buffer::RingBuffer *ring = slot.ring.get();
    if (sink == nullptr || ring == nullptr || !sink->playing())
      continue;
    if (first) {
      produced = ring->read(dst, len, 0);
      first = false;
    } else {
      uint8_t scratch[96];
      size_t offset = 0;
      while (offset < len) {
        const size_t want = std::min(sizeof(scratch), len - offset);
        const size_t got = ring->read(scratch, want, 0);
        if (got == 0)
          break;
        if (offset + got > produced) {
          memset(dst + produced, 0, offset + got - produced);
          produced = offset + got;
        }
        mix_add(dst + offset, scratch, got, sample);
        offset += got;
        if (got < want)
          break;
      }
    }
    if (sink->finishing() && ring->available() == 0)
      this->client_->enable_loop_soon_any_context();
  }
  return produced;
}

// Backoff helpers. next_attempt_ms is an absolute millis() deadline; fail_count only sets
// how far it is pushed out.
bool USBAudioDevice::reopen_due_(const uint32_t &next_attempt_ms, uint8_t fail_count) const {
  if (fail_count == 0)
    return true;
  return (int32_t) (millis() - next_attempt_ms) >= 0;
}

void USBAudioDevice::note_open_result_(bool ok, uint32_t &next_attempt_ms, uint8_t &fail_count) {
  if (ok) {
    fail_count = 0;
    next_attempt_ms = millis();
    return;
  }
  uint32_t delay_ms = STREAM_REOPEN_BASE_MS;
  for (uint8_t i = 0; i < fail_count && delay_ms < STREAM_REOPEN_MAX_MS; i++)
    delay_ms *= 2;
  if (delay_ms > STREAM_REOPEN_MAX_MS)
    delay_ms = STREAM_REOPEN_MAX_MS;
  if (fail_count < 255)
    fail_count++;
  next_attempt_ms = millis() + delay_ms;
  ESP_LOGD(TAG, "Stream open failed (%u in a row); next attempt in %" PRIu32 " ms", fail_count, delay_ms);
}

void USBAudioDevice::set_stream_error_(const char *what) {
  ESP_LOGE(TAG, "%s", what);
  this->client_->status_set_error();
}

// -----------------------------------------------------------------------------
// Descriptor parsing
// -----------------------------------------------------------------------------

void USBAudioDevice::detect_uac_version_() {
  // Assume the older version until the device says otherwise: a device that does not
  // describe itself is far more likely to be a 1.0 one, and the 1.0 layouts are the ones a
  // 2.0 device would never be parsed with by accident.
  this->uac_version_ = UAC_VERSION_1;

  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr)
    return;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0);
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 5)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_HEADER)
      continue;

    // Class-specific AC interface header: bcdADC at [3..4]. This is read before the
    // streaming interfaces are picked, so it is the first audio function's header. A device
    // that describes two functions of different class versions is not something the
    // specification provides for.
    const uint16_t bcd = static_cast<uint16_t>(d[3] | (d[4] << 8));
    this->uac_version_ = (bcd >= UAC_VERSION_2) ? UAC_VERSION_2 : UAC_VERSION_1;
    ESP_LOGD(TAG, "Device speaks audio class %u.%u", static_cast<unsigned>(bcd >> 8),
             static_cast<unsigned>((bcd >> 4) & 0x0F));
    return;
  }

  ESP_LOGD(TAG, "No AudioControl header found; assuming audio class 1.0");
}

bool USBAudioDevice::find_ac_interface_() {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr)
    return false;

  // The AudioStreaming interfaces this component is going to use, when they are known.
  uint8_t wanted[2];
  uint8_t wanted_count = 0;
  if (this->client_->spk_cfg_.configured && this->spk_format_ok_)
    wanted[wanted_count++] = this->spk_as_intf_;
  if (this->client_->mic_cfg_.configured && this->mic_format_ok_)
    wanted[wanted_count++] = this->mic_as_intf_;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  bool found_any = false;
  uint8_t current_ac = 0;
  uint8_t first_ac = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0);
      if (in_ac) {
        current_ac = id->bInterfaceNumber;
        if (!found_any) {
          first_ac = current_ac;
          found_any = true;
        }
      }
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 8)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_HEADER)
      continue;

    // Class-specific AC interface header: bInCollection at [7], then one interface number
    // per entry. The Collection is what ties an AudioControl interface to the
    // AudioStreaming interfaces of the same audio function, and a device may hold several
    // such functions.
    const uint8_t in_collection = d[7];
    for (uint8_t i = 0; i < in_collection && (8 + i) < desc->bLength; i++) {
      for (uint8_t w = 0; w < wanted_count; w++) {
        if (d[8 + i] != wanted[w])
          continue;
        this->ac_intf_ = current_ac;
        ESP_LOGD(TAG, "AudioControl interface %u owns streaming interface %u", current_ac, wanted[w]);
        return true;
      }
    }
  }

  if (!found_any) {
    ESP_LOGE(TAG, "No AudioControl interface found");
    return false;
  }

  this->ac_intf_ = first_ac;
  if (wanted_count != 0) {
    ESP_LOGW(TAG, "No AudioControl Collection lists streaming interface %u; addressing interface %u", wanted[0],
             first_ac);
  }
  return true;
}

// Offset of the first upstream source ID inside an AudioControl unit or terminal
// descriptor. The leading fields of these descriptors are the same in UAC 1.0 and 2.0, so
// a walk built on them does not depend on the audio class version. Returns 0 for
// descriptors that have no upstream connection.
static uint8_t uac_first_source_offset(uint8_t subtype) {
  switch (subtype) {
    case UAC_AC_OUTPUT_TERMINAL:
      return 7;  // bSourceID
    case UAC_AC_FEATURE_UNIT:
      return 4;  // bSourceID
    case UAC_AC_MIXER_UNIT:
    case UAC_AC_SELECTOR_UNIT:
      return 5;  // baSourceID[0], preceded by bNrInPins
    case UAC_AC_PROCESSING_UNIT:
    case UAC_AC_EXTENSION_UNIT:
      return 7;  // baSourceID[0], preceded by wProcessType and bNrInPins
    default:
      return 0;
  }
}

static const UacTopologyNode *uac_find_node(const UacTopologyNode *nodes, uint8_t count, uint8_t id) {
  for (uint8_t i = 0; i < count; i++) {
    if (nodes[i].id == id)
      return &nodes[i];
  }
  return nullptr;
}

// Walk upstream from an Output Terminal and report the first Feature Unit on the path, plus
// whether the path reaches `want_source`. A unit with several inputs is followed on its
// first pin only, which is where a device that offers one volume control puts it.
//
// Upstream is the right direction for both directions of audio: playback runs Input
// Terminal (USB streaming) -> Feature Unit -> Output Terminal (speaker), capture runs Input
// Terminal (microphone) -> Feature Unit -> Output Terminal (USB streaming). Either way the
// Feature Unit sits between the two, which is also how sound/usb/mixer.c builds its mixer.
static uint8_t uac_walk_from_output_terminal(const UacTopologyNode *nodes, uint8_t count,
                                             const UacTopologyNode &output_terminal, uint8_t want_source,
                                             bool &reaches_source) {
  reaches_source = false;
  uint8_t feature_unit = 0;
  uint8_t next = output_terminal.first_source;
  // A malformed descriptor can describe a cycle, so bound the walk by the node count.
  for (uint8_t hop = 0; hop < count && next != 0; hop++) {
    if (next == want_source) {
      reaches_source = true;
      break;
    }
    const UacTopologyNode *node = uac_find_node(nodes, count, next);
    if (node == nullptr)
      break;
    if (feature_unit == 0 && node->subtype == UAC_AC_FEATURE_UNIT)
      feature_unit = node->id;
    next = node->first_source;
  }
  return feature_unit;
}

// Feature Unit belonging to one specific stream, identified by the Terminal its AudioStreaming
// interface links to (bTerminalLink).
//
// Selecting the Output Terminal by type alone is wrong on any device with more than one of
// them: a card with a speaker jack, a headphone jack and an S/PDIF output describes three
// playback Output Terminals, and the first one in descriptor order need not be the one the
// chosen stream feeds. Volume and mute would then be applied to a Feature Unit that is not in
// the path being played, which is silent from the user's side and reports no error.
//
// For capture, bTerminalLink names the Output Terminal itself, so the walk starts there.
// For playback it names the Input Terminal, so each candidate Output Terminal is walked and
// the one whose path reaches that Input Terminal is the stream's own.
static uint8_t uac_find_feature_unit(const UacTopologyNode *nodes, uint8_t count, uint8_t terminal_link,
                                     bool capture) {
  if (terminal_link == 0)
    return 0;

  if (capture) {
    const UacTopologyNode *terminal = uac_find_node(nodes, count, terminal_link);
    if (terminal == nullptr || terminal->subtype != UAC_AC_OUTPUT_TERMINAL)
      return 0;
    bool unused = false;
    return uac_walk_from_output_terminal(nodes, count, *terminal, 0, unused);
  }

  for (uint8_t i = 0; i < count; i++) {
    if (nodes[i].subtype != UAC_AC_OUTPUT_TERMINAL)
      continue;
    // A playback path ends at a physical sink, never back at the host.
    if (nodes[i].terminal_type == UAC_TERMINAL_TYPE_USB_STREAMING)
      continue;
    bool reaches = false;
    const uint8_t feature_unit = uac_walk_from_output_terminal(nodes, count, nodes[i], terminal_link, reaches);
    if (reaches)
      return feature_unit;
  }
  return 0;
}

uint8_t USBAudioDevice::collect_topology_(UacTopologyNode *nodes) {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr)
    return 0;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  uint8_t count = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 4)
      continue;
    if (count >= UAC_MAX_TOPOLOGY_NODES) {
      ESP_LOGW(TAG, "Audio function has more than %u units; topology is incomplete", UAC_MAX_TOPOLOGY_NODES);
      break;
    }

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    const uint8_t subtype = d[2];
    const bool is_terminal = (subtype == UAC_AC_INPUT_TERMINAL || subtype == UAC_AC_OUTPUT_TERMINAL);
    const uint8_t src_off = uac_first_source_offset(subtype);
    if (src_off == 0 && !is_terminal)
      continue;  // header or an unknown descriptor: not part of the signal path

    UacTopologyNode node{};
    node.subtype = subtype;
    node.id = d[3];
    if (is_terminal) {
      if (desc->bLength < 6)
        continue;
      node.terminal_type = static_cast<uint16_t>(d[4] | (d[5] << 8));
    }
    if (src_off != 0) {
      if (desc->bLength <= src_off)
        continue;
      node.first_source = d[src_off];
    }
    nodes[count++] = node;
  }
  return count;
}

uint8_t USBAudioDevice::collect_feature_units_(UacFeatureUnit *units, uint8_t max_count) {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr)
    return 0;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  uint8_t count = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr && count < max_count) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 7)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_FEATURE_UNIT)
      continue;

    // Feature Unit layout:
    //   UAC 1.0 (section 4.3.2.5): [3] bUnitID [4] bSourceID [5] bControlSize
    //                              [6..] bmaControls[0] (master), then one per channel,
    //                              bControlSize bytes each, one bit per control.
    //   UAC 2.0 (section 4.7.2.8): [3] bUnitID [4] bSourceID
    //                              [5..] bmaControls[0] (master), then one per channel,
    //                              always four bytes each, two bits per control.
    // Both end with iFeature, which is why the trailing byte is not counted as an entry.
    const bool v2 = (this->uac_version_ >= UAC_VERSION_2);
    UacFeatureUnit fu{};
    fu.unit_id      = d[3];
    fu.source_id    = d[4];
    fu.control_size = v2 ? UAC2_FU_CONTROL_SIZE : d[5];
    const uint8_t controls_off = v2 ? 5 : 6;
    if (fu.control_size == 0 || (controls_off + fu.control_size + 1) > desc->bLength)
      continue;
    fu.control_entries = (desc->bLength - controls_off - 1) / fu.control_size;
    if (fu.control_entries == 0)
      continue;

    // What one bmaControls entry says about mute and volume, in whichever encoding the
    // class version uses. A UAC 2.0 control that can only be read is of no use here, so it
    // counts as not being there.
    auto entry_controls = [&](uint8_t entry, bool &mute, bool &volume) {
      const uint16_t off = controls_off + static_cast<uint16_t>(entry) * fu.control_size;
      if (off + fu.control_size > desc->bLength) {
        mute = volume = false;
        return;
      }
      if (!v2) {
        mute = (d[off] & UAC_FU_CTL_MUTE) != 0;
        volume = (d[off] & UAC_FU_CTL_VOLUME) != 0;
        return;
      }
      const uint32_t bm = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
                          (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
      mute = (bm & UAC2_FU_CTL_MUTE_MASK) == UAC2_FU_CTL_MUTE_RW;
      volume = (bm & UAC2_FU_CTL_VOLUME_MASK) == UAC2_FU_CTL_VOLUME_RW;
    };

    entry_controls(0, fu.has_mute, fu.has_volume);
    fu.master_controls = d[controls_off];

    // Not every device puts its controls on the master entry; some describe them per
    // channel only. Record those so a control that exists can still be reached.
    for (uint8_t entry = 1; entry < fu.control_entries && entry <= UAC_FU_MAX_CHANNELS; entry++) {
      bool mute = false;
      bool volume = false;
      entry_controls(entry, mute, volume);
      if (mute)
        fu.mute_channels |= static_cast<uint8_t>(1 << (entry - 1));
      if (volume)
        fu.volume_channels |= static_cast<uint8_t>(1 << (entry - 1));
    }

    units[count++] = fu;
  }
  return count;
}

bool USBAudioDevice::parse_feature_units_() {
  // Another device may have been attached before, so start from nothing.
  this->spk_ctl_.fu = {};
  this->mic_ctl_.fu = {};
  this->spk_ctl_.clock_id = 0;
  this->mic_ctl_.clock_id = 0;

  UacTopologyNode nodes[UAC_MAX_TOPOLOGY_NODES];
  const uint8_t node_count = this->collect_topology_(nodes);

  UacFeatureUnit units[UAC_MAX_TOPOLOGY_NODES];
  const uint8_t unit_count = this->collect_feature_units_(units, UAC_MAX_TOPOLOGY_NODES);

  // Tie each direction to the Terminal its own stream links to, so a device with several
  // outputs does not get its volume applied to a path it is not playing on.
  uint8_t spk_fu_id = uac_find_feature_unit(nodes, node_count, this->spk_alt_.terminal_link, false);
  uint8_t mic_fu_id = uac_find_feature_unit(nodes, node_count, this->mic_alt_.terminal_link, true);

  // A device may describe a topology this walk cannot follow. Picking a Feature Unit by
  // descriptor order would then be a guess, and sending a control request to the wrong unit
  // changes the wrong thing, so only fall back when there is nothing to guess between.
  if (spk_fu_id == 0 && mic_fu_id == 0 && unit_count == 1) {
    ESP_LOGD(TAG, "Could not follow the audio topology; using the only Feature Unit found");
    spk_fu_id = units[0].unit_id;
    mic_fu_id = units[0].unit_id;
  }

  for (uint8_t i = 0; i < unit_count; i++) {
    if (this->client_->spk_cfg_.configured && units[i].unit_id == spk_fu_id)
      this->spk_ctl_.fu = units[i];
    if (this->client_->mic_cfg_.configured && units[i].unit_id == mic_fu_id)
      this->mic_ctl_.fu = units[i];
  }

  // Unit ID 0 is reserved by the specification, so it marks "nothing found" here.
  if (this->client_->spk_cfg_.configured) {
    if (this->spk_ctl_.fu.unit_id != 0) {
      ESP_LOGD(TAG, "Speaker Feature Unit: id=%u mute=%s vol=%s", this->spk_ctl_.fu.unit_id,
               YESNO(this->spk_ctl_.fu.mute_available()), YESNO(this->spk_ctl_.fu.volume_available()));
    } else {
      ESP_LOGW(TAG, "No Feature Unit in the playback path; volume and mute are unavailable");
    }
  }
  if (this->client_->mic_cfg_.configured) {
    if (this->mic_ctl_.fu.unit_id != 0) {
      ESP_LOGD(TAG, "Mic Feature Unit: id=%u mute=%s vol=%s", this->mic_ctl_.fu.unit_id,
               YESNO(this->mic_ctl_.fu.mute_available()), YESNO(this->mic_ctl_.fu.volume_available()));
    } else {
      ESP_LOGW(TAG, "No Feature Unit in the capture path; volume and mute are unavailable");
    }
  }

  return true;  // non-fatal if not found - volume/mute will be silently skipped
}

namespace {

// Frame position of one spatial channel: the number of positions below it that the device
// declares, since channels appear in a frame in bit order.
uint8_t uac_position_offset(uint32_t channel_config, uint32_t position_bit) {
  uint8_t offset = 0;
  for (uint32_t bit = 1; bit < position_bit; bit <<= 1) {
    if ((channel_config & bit) != 0)
      offset++;
  }
  return offset;
}

// Resolve where a pair sits in a frame. A device that declares no bitmap leaves the layout
// undefined, in which case the conventional order is assumed, which is what the Linux driver
// does as well. Returns false when the pair is not carried by this alt-setting.
bool uac_resolve_pair(const UacAltInfo &alt, UacChannelPair pair, uint8_t &left, uint8_t &right) {
  const uint32_t mask = uac_pair_mask(pair);
  const uint32_t left_bit = mask & (~mask + 1);  // lowest set bit of the pair
  const uint32_t right_bit = mask & ~left_bit;

  if (alt.channel_config == 0) {
    uac_pair_default_offsets(pair, left, right);
    return alt.channels > left && alt.channels > right;
  }
  if ((alt.channel_config & mask) != mask)
    return false;
  left = uac_position_offset(alt.channel_config, left_bit);
  right = uac_position_offset(alt.channel_config, right_bit);
  return alt.channels > left && alt.channels > right;
}

const char *uac_pair_name(UacChannelPair pair) {
  switch (pair) {
    case UacChannelPair::CENTER_LFE: return "center_lfe";
    case UacChannelPair::SIDE: return "side";
    case UacChannelPair::BACK: return "back";
    case UacChannelPair::FRONT:
    default: return "front";
  }
}

}  // namespace

// Channel layout a Terminal declares. UAC 1.0 keeps it on the Terminal rather than on the
// streaming interface, so a stream's layout is only reachable through its bTerminalLink.
uint32_t USBAudioDevice::terminal_channel_config_(uint8_t terminal_id) {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr || terminal_id == 0)
    return 0;

  const bool uac2 = (this->uac_version_ >= UAC_VERSION_2);
  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 4)
      continue;
    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    // Only an Input Terminal carries the layout; an Output Terminal takes whatever its
    // source hands it.
    if (d[2] != UAC_AC_INPUT_TERMINAL || d[3] != terminal_id)
      continue;
    if (uac2) {
      // [8] bNrChannels [9..12] bmChannelConfig
      if (desc->bLength < 13)
        return 0;
      return static_cast<uint32_t>(d[9]) | (static_cast<uint32_t>(d[10]) << 8) |
             (static_cast<uint32_t>(d[11]) << 16) | (static_cast<uint32_t>(d[12]) << 24);
    }
    // UAC 1.0: [7] bNrChannels [8..9] wChannelConfig
    if (desc->bLength < 10)
      return 0;
    return static_cast<uint32_t>(d[8]) | (static_cast<uint32_t>(d[9]) << 8);
  }
  return 0;
}

static uint32_t uac_offered_rate(const UacAltInfo &alt, uint32_t wanted) {
  if (alt.freq_from_clock)
    return wanted;
  if (alt.sample_freq_type == 0)
    return std::clamp(wanted, alt.sample_freq_lower, alt.sample_freq_upper);
  uint32_t best = 0;
  for (uint8_t i = 0; i < alt.sample_freq_type && i < UAC_MAX_SAMPLE_FREQS; i++) {
    const uint32_t rate = alt.sample_freq[i];
    if (rate == 0)
      continue;
    const uint32_t diff = rate > wanted ? rate - wanted : wanted - rate;
    const uint32_t best_diff = best > wanted ? best - wanted : wanted - best;
    if (best == 0 || diff < best_diff)
      best = rate;
  }
  return best;
}

bool USBAudioDevice::parse_as_interface_(bool want_out, uint8_t channels, uint8_t bits, uint32_t sample_rate,
                                          uint8_t &intf_out, UacAltInfo &alt_out, EndpointConfig &fmt_out) {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr)
    return false;

  // alt_out is a member that outlives the device it was filled for. Clear it before the
  // search, otherwise the first-match rule below would keep a previous device's setting.
  intf_out = 0;
  alt_out = {};
  fmt_out = {};

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);

  // Isochronous high-bandwidth transactions (the wMaxPacketSize MULT field) exist only at
  // high speed. Resolve the device speed once so a full-speed UAC device does not get its
  // mps inflated below (see the endpoint parse). ESP32-P4 is the first HS-capable host, so
  // this only diverges there; older FS-only hosts always take the clamped path anyway.
  usb_device_info_t dev_info;
  const bool is_high_speed =
      usb_host_device_info(this->dev_->handle, &dev_info) == ESP_OK && dev_info.speed == USB_SPEED_HIGH;
  this->device_is_high_speed_ = is_high_speed;

  // The two class versions describe a streaming format in different descriptors, so which
  // one the device speaks has to be settled before its descriptors are read.
  const bool uac2 = (this->uac_version_ >= UAC_VERSION_2);

  uint8_t cur_intf = 0;
  uint8_t cur_alt  = 0;
  bool    cur_is_as = false;
  UacAltInfo cur_alt_info{};
  bool format_seen = false;

  // An alt-setting carrying exactly the requested channel count is always preferred: it puts
  // no mapping in the data path and moves the least over the wire. A multichannel setting is
  // only taken when the device offers no such setting at all, and then one pair is lifted out
  // of the frame. Both are remembered so the choice does not depend on descriptor order.
  UacAltInfo mapped_alt{};
  uint8_t mapped_intf = 0;
  UacAltInfo offered_alt{};
  uint8_t offered_intf = 0;
  uint32_t offered_rate = 0;
  uint8_t offered_penalty = UINT8_MAX;

  // What the device offers, once per device (the first of the two searches): what a configuration can ask for
  const bool log_formats = want_out == this->client_->spk_cfg_.configured;
  auto try_commit = [&]() {
    if (!format_seen || !cur_alt_info.valid)
      return;
    if (log_formats) {
      char rates[64] = "";
      if (cur_alt_info.freq_from_clock) {
        snprintf(rates, sizeof(rates), "clock");
      } else if (cur_alt_info.sample_freq_type == 0) {
        snprintf(rates, sizeof(rates), "%" PRIu32 "-%" PRIu32, cur_alt_info.sample_freq_lower,
                 cur_alt_info.sample_freq_upper);
      } else {
        size_t used = 0;
        for (uint8_t i = 0; i < cur_alt_info.sample_freq_type && i < UAC_MAX_SAMPLE_FREQS && used < sizeof(rates);
             i++)
          used += snprintf(rates + used, sizeof(rates) - used, "%s%" PRIu32, i == 0 ? "" : " ",
                           cur_alt_info.sample_freq[i]);
      }
      ESP_LOGD(TAG, "Offers %s: intf=%u alt=%u channels=%u bits=%u rate=%s Hz",
               (cur_alt_info.ep_addr & 0x80) == 0 ? "speaker" : "microphone", cur_intf, cur_alt_info.alt_setting,
               cur_alt_info.channels, cur_alt_info.bit_resolution, rates);
    }
    if (((cur_alt_info.ep_addr & 0x80) == 0) == want_out && cur_alt_info.sub_frame_size >= 2 &&
        cur_alt_info.sub_frame_size <= 4 && cur_alt_info.channels >= 1 && cur_alt_info.channels <= 8) {
      const uint32_t rate = uac_offered_rate(cur_alt_info, sample_rate);
      const uint8_t penalty = (rate != sample_rate ? 4 : 0) + (cur_alt_info.channels != channels ? 2 : 0) +
                              (cur_alt_info.sub_frame_size * 8u != bits ? 1 : 0);
      if (rate != 0 && penalty < offered_penalty) {
        offered_alt = cur_alt_info;
        offered_intf = cur_intf;
        offered_rate = rate;
        offered_penalty = penalty;
      }
    }
    if (cur_alt_info.bit_resolution != bits)
      return;
    // A multichannel alt-setting is usable when it carries the requested pair, but only for a
    // stereo request; there is nothing sensible to lift out for any other channel count.
    const bool exact = (cur_alt_info.channels == channels);
    if (!exact && (channels != 2 || cur_alt_info.channels < 2))
      return;
    // Check sample rate. An alt-setting whose rate is a property of a Clock Source has
    // nothing in its descriptors to check against; the clock is asked when the stream
    // opens.
    bool freq_ok = cur_alt_info.freq_from_clock;
    if (freq_ok) {
      // nothing to check here
    } else if (cur_alt_info.sample_freq_type == 0) {
      freq_ok = (sample_rate >= cur_alt_info.sample_freq_lower && sample_rate <= cur_alt_info.sample_freq_upper);
    } else {
      for (uint8_t i = 0; i < cur_alt_info.sample_freq_type && i < UAC_MAX_SAMPLE_FREQS; i++) {
        if (cur_alt_info.sample_freq[i] == sample_rate) {
          freq_ok = true;
          break;
        }
      }
    }
    if (!freq_ok)
      return;
    // Check endpoint direction.
    bool ep_is_out = (cur_alt_info.ep_addr & 0x80) == 0;
    if (ep_is_out != want_out)
      return;

    // The first match of each kind wins. A device may describe the same format on several
    // Terminals, and overwriting would hand the stream to whichever comes last in the
    // descriptor, which is not something a user can predict or influence.
    if (exact) {
      if (alt_out.valid)
        return;
      intf_out = cur_intf;
      alt_out = cur_alt_info;
      return;
    }

    if (mapped_alt.valid)
      return;
    // UAC 1.0 keeps the layout on the Terminal, so it is only reachable now that
    // bTerminalLink is known.
    if (cur_alt_info.channel_config == 0)
      cur_alt_info.channel_config = this->terminal_channel_config_(cur_alt_info.terminal_link);
    uint8_t left = 0;
    uint8_t right = 1;
    if (!uac_resolve_pair(cur_alt_info, this->client_->channel_pair_, left, right))
      return;
    cur_alt_info.channel_map_active = true;
    cur_alt_info.map_left = left;
    cur_alt_info.map_right = right;
    mapped_alt = cur_alt_info;
    mapped_intf = cur_intf;
  };

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    const uint8_t dtype = desc->bDescriptorType;
    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);

    if (dtype == USB_W_VALUE_DT_INTERFACE) {
      try_commit();
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      cur_intf   = id->bInterfaceNumber;
      cur_alt    = id->bAlternateSetting;
      cur_is_as  = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOSTREAMING);
      cur_alt_info = {};
      format_seen  = false;
      // alt 0 is zero-bandwidth - skip but stay on interface
      continue;
    }

    if (!cur_is_as || cur_alt == 0)
      continue;

    if (dtype == UAC_CS_INTERFACE && desc->bLength >= 3) {
      uint8_t sub = d[2];

      if (sub == UAC_AS_GENERAL && desc->bLength >= 4) {
        // Class-specific AS interface descriptor. Both versions start with bTerminalLink,
        // which is the Terminal of the AudioControl interface this stream belongs to and
        // therefore the way to the Clock Source in UAC 2.0.
        cur_alt_info.terminal_link = d[3];
        // UAC 2.0 (section 4.9.2) moved the channel count here:
        // [3] bTerminalLink [4] bmControls [5] bFormatType [6..9] bmFormats
        // [10] bNrChannels [11..14] bmChannelConfig [15] iChannelNames
        if (uac2 && desc->bLength >= 11) {
          cur_alt_info.channels = d[10];
          // [11..14] bmChannelConfig; UAC 2.0 puts the layout on the streaming interface.
          if (desc->bLength >= 15) {
            cur_alt_info.channel_config = static_cast<uint32_t>(d[11]) | (static_cast<uint32_t>(d[12]) << 8) |
                                          (static_cast<uint32_t>(d[13]) << 16) | (static_cast<uint32_t>(d[14]) << 24);
          }
          format_seen = true;
        }
        continue;
      }

      if (sub == UAC_AS_FORMAT_TYPE && uac2 && desc->bLength >= 6) {
        // Type I Format Type descriptor (UAC 2.0 table 2-2). The sample frequencies are
        // not here: in this version the Clock Source entity answers for them.
        // [3] bFormatType [4] bSubslotSize [5] bBitResolution
        if (d[3] != 0x01)  // only Type I
          continue;
        cur_alt_info.sub_frame_size = d[4];
        cur_alt_info.bit_resolution = d[5];
        cur_alt_info.freq_from_clock = true;
        continue;
      }

      if (sub == UAC_AS_FORMAT_TYPE && desc->bLength >= 8) {
        // Type I Format Type descriptor (UAC 1.0 table 4-21):
        // [0] bLength [1] bDescriptorType [2] bDescriptorSubType
        // [3] bFormatType (must be 0x01 for Type I)
        // [4] bNrChannels [5] bSubFrameSize [6] bBitResolution
        // [7] bSamFreqType  then: continuous=(lower 3B, upper 3B); discrete=Nx3B each
        if (d[3] != 0x01)  // only Type I
          continue;
        cur_alt_info.channels       = d[4];
        cur_alt_info.sub_frame_size = d[5];
        cur_alt_info.bit_resolution = d[6];
        cur_alt_info.sample_freq_type = d[7];
        if (cur_alt_info.sample_freq_type == 0 && desc->bLength >= 14) {
          // Continuous: tLowerSamFreq at bytes 8-10, tUpperSamFreq at 11-13
          cur_alt_info.sample_freq_lower = static_cast<uint32_t>(d[8]) |
                                           (static_cast<uint32_t>(d[9]) << 8) |
                                           (static_cast<uint32_t>(d[10]) << 16);
          cur_alt_info.sample_freq_upper = static_cast<uint32_t>(d[11]) |
                                           (static_cast<uint32_t>(d[12]) << 8) |
                                           (static_cast<uint32_t>(d[13]) << 16);
        } else {
          uint8_t n = std::min<uint8_t>(cur_alt_info.sample_freq_type, UAC_MAX_SAMPLE_FREQS);
          for (uint8_t i = 0; i < n; i++) {
            uint16_t base = 8 + static_cast<uint16_t>(i) * 3;
            if (base + 2 >= desc->bLength)
              break;
            cur_alt_info.sample_freq[i] = static_cast<uint32_t>(d[base]) |
                                           (static_cast<uint32_t>(d[base + 1]) << 8) |
                                           (static_cast<uint32_t>(d[base + 2]) << 16);
          }
        }
        format_seen = true;
        continue;
      }
    }

    if (dtype == USB_W_VALUE_DT_ENDPOINT && cur_is_as && cur_alt > 0 && desc->bLength >= sizeof(usb_ep_desc_t)) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_ISOC)
        continue;
      const bool ep_is_in = (ep->bEndpointAddress & 0x80) != 0;
      const bool want_in = !want_out;
      if (ep_is_in == want_in) {
        // Data endpoint for this stream's direction.
        cur_alt_info.ep_addr = ep->bEndpointAddress;
        cur_alt_info.ep_attr = ep->bmAttributes;
        // MULT (wMaxPacketSize bits 12:11) is a high-speed-only high-bandwidth multiplier; on
        // full speed those bits are reserved and an isochronous packet is capped at 1023 bytes.
        // Applying MULT to a full-speed device inflates the ISO DMA buffer past what the link
        // moves per frame and crashes the HS host controller (ESP32-P4 with a full-speed UAC
        // device), so honour MULT only at high speed and clamp otherwise.
        cur_alt_info.mps = is_high_speed
                               ? static_cast<uint16_t>(USB_EP_DESC_GET_MPS(ep) * (USB_EP_DESC_GET_MULT(ep) + 1))
                               : std::min<uint16_t>(USB_EP_DESC_GET_MPS(ep), 1023);
        // bInterval selects the service interval and therefore how many bytes per interval
        // the sample clock has to produce; without it the pacing can only guess.
        cur_alt_info.b_interval = ep->bInterval;
        // UAC1 audio endpoint descriptor (bLength 9) carries bSynchAddress at offset 8: the
        // companion feedback IN endpoint address for an asynchronous OUT endpoint.
        if (desc->bLength >= 9 && reinterpret_cast<const uint8_t *>(ep)[8] != 0)
          cur_alt_info.feedback_ep_addr = reinterpret_cast<const uint8_t *>(ep)[8];
        cur_alt_info.alt_setting = cur_alt;
        cur_alt_info.valid = true;
      } else {
        // Opposite-direction isochronous endpoint in the same AS interface: the explicit
        // feedback endpoint (UAC2, or UAC1 where bSynchAddress was not used).
        cur_alt_info.feedback_ep_addr = ep->bEndpointAddress;
        cur_alt_info.feedback_mps = USB_EP_DESC_GET_MPS(ep);
      }
      continue;
    }
  }

  // Commit the final accumulated alt-setting.
  try_commit();

  // No plain stereo setting on this device: fall back to lifting a pair out of a
  // multichannel frame.
  if (!alt_out.valid && mapped_alt.valid) {
    intf_out = mapped_intf;
    alt_out = mapped_alt;
    ESP_LOGI(TAG, "%s: no %u-channel setting; taking the %s pair from %u channels (frame slots %u and %u)",
             want_out ? "Speaker" : "Mic", channels, uac_pair_name(this->client_->channel_pair_), alt_out.channels,
             alt_out.map_left, alt_out.map_right);
  }

  uint32_t rate = sample_rate;
  if (!alt_out.valid && offered_alt.valid) {
    intf_out = offered_intf;
    alt_out = offered_alt;
    rate = offered_rate;
    ESP_LOGI(TAG, "%s: %u channels, %u bits, %" PRIu32 " Hz not offered; taking %u channels, %u bits, %" PRIu32 " Hz",
             want_out ? "Speaker" : "Mic", channels, bits, sample_rate, alt_out.channels, alt_out.sub_frame_size * 8u,
             rate);
  }

  if (!alt_out.valid) {
    ESP_LOGD(TAG, "No %s AS alt-setting found for channels=%u bits=%u rate=%" PRIu32,
             want_out ? "speaker" : "microphone", channels, bits, sample_rate);
    return false;
  }
  fmt_out.channels = alt_out.channel_map_active ? 2 : alt_out.channels;
  fmt_out.bits_per_sample = alt_out.sub_frame_size * 8u;
  fmt_out.sample_rate = rate;
  fmt_out.configured = true;

  ESP_LOGI(TAG, "%s: intf=%u alt=%u ep=0x%02X mps=%u channels=%u bits=%u",
           want_out ? "Speaker" : "Mic", intf_out, alt_out.alt_setting, alt_out.ep_addr,
           alt_out.mps, alt_out.channels, alt_out.bit_resolution);
  return true;
}

// -----------------------------------------------------------------------------
// Control transfer helpers
// -----------------------------------------------------------------------------

namespace {
// Result of a control transfer. The caller stops waiting once the timeout expires, but the
// transfer stays submitted and its callback runs later on the USB task, so the callback
// must not write to anything owned by the caller's stack frame. Both sides hold a shared
// pointer to this block instead, and it lives until the last of them is gone.
struct UacControlResult {
  std::atomic<bool> done{false};
  bool success{false};
  uint16_t status{0};  // usb_transfer_status_t as the host reported it
  uint8_t data[UAC_CTRL_MAX_DATA_LEN]{};
  size_t data_len{0};
};

// What the host said about a finished transfer. A request the device rejected, one that
// never reached it and one that was cut short are three different answers, and which one
// came back decides where to look next.
const char *uac_transfer_status_str(uint16_t status) {
  switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
      return "completed";
    case USB_TRANSFER_STATUS_ERROR:
      return "bus error";
    case USB_TRANSFER_STATUS_TIMED_OUT:
      return "timed out";
    case USB_TRANSFER_STATUS_CANCELED:
      return "canceled";
    case USB_TRANSFER_STATUS_STALL:
      return "stalled by device";
    case USB_TRANSFER_STATUS_OVERFLOW:
      return "overflow";
    case USB_TRANSFER_STATUS_SKIPPED:
      return "skipped";
    case USB_TRANSFER_STATUS_NO_DEVICE:
      return "no device";
    default:
      return "unknown";
  }
}
}  // namespace

bool USBAudioDevice::uac_control_transfer_(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                                            const uint8_t *out_data, size_t out_len, uint8_t *in_data,
                                            size_t in_len, bool *rejected) {
  if (out_len > UAC_CTRL_MAX_DATA_LEN || in_len > UAC_CTRL_MAX_DATA_LEN) {
    ESP_LOGE(TAG, "Control request 0x%02X wants more than %u bytes of data", request,
             static_cast<unsigned>(UAC_CTRL_MAX_DATA_LEN));
    return false;
  }

  // wLength is the length the class definition gives the request: two bytes for a volume,
  // one for a mute, three for a sampling frequency. It is part of what identifies the
  // request to the device, so it is sent as defined and never rounded.
  //
  // The buffer behind it is a separate matter. The host controller sizes an IN data stage
  // from the buffer rather than from wLength, and it needs whole endpoint 0 packets, so a
  // read rounds its buffer up to one. Anything the device does not send simply stays
  // untouched in it.
  size_t buffer_len = out_len;
  const size_t w_length = (in_data != nullptr) ? in_len : out_len;
  if (in_data != nullptr) {
    const usb_device_desc_t *dev_desc = this->dev_->desc;
    if (dev_desc == nullptr || dev_desc->bMaxPacketSize0 == 0) {
      ESP_LOGW(TAG, "Control request 0x%02X skipped: endpoint 0 packet size is unknown", request);
      return false;
    }
    const size_t mps0 = dev_desc->bMaxPacketSize0;
    buffer_len = ((in_len + mps0 - 1) / mps0) * mps0;
  }

  // A control transfer only makes progress while usb_host_lib_handle_events() is being
  // called, and the only caller of it is USBHost::loop(), which runs on the component loop
  // task. Waiting on that task without driving it means waiting for something this wait is
  // itself preventing, which is why these requests never completed while a request issued
  // from the stream task did. Drive it here for as long as the wait lasts. Requests from
  // other tasks must not touch it, since it belongs to the loop task.
  const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  const bool on_loop_task = (this->client_->loop_task_ != nullptr && current_task == this->client_->loop_task_);
  const bool on_ctrl_task = (current_task == this->client_->ctrl_task_.get_handle());

  uint16_t last_status = USB_TRANSFER_STATUS_COMPLETED;
  size_t last_len = 0;
  bool short_answer = false;
  for (uint8_t attempt = 0; attempt < UAC_CTRL_ATTEMPTS; attempt++) {
    auto result = std::make_shared<UacControlResult>();
    std::vector<uint8_t> payload(buffer_len);
    if (out_data != nullptr && out_len != 0)
      memcpy(payload.data(), out_data, out_len);

    // The device and its transfer buffers go away on the main loop. A disconnect bumps the
    // generation and then waits for this flag, so a request is either handed over before the
    // close or not at all.
    if (on_ctrl_task) {
      this->ctrl_submitting_.store(true);
      if (this->ctrl_gen_.load() != this->ctrl_op_gen_) {
        this->ctrl_submitting_.store(false);
        return false;
      }
    }
    const bool submitted =
        this->client_->control_transfer(*this->dev_, req_type, request, value, index,
                               [result, current_task, on_ctrl_task](const usb_host::TransferStatus &s) {
                                 result->success = s.success;
                                 result->status = s.error_code;
                                 if (s.success && s.data != nullptr) {
                                   result->data_len = std::min<size_t>(s.data_len, UAC_CTRL_MAX_DATA_LEN);
                                   memcpy(result->data, s.data, result->data_len);
                                 }
                                 result->done.store(true, std::memory_order_release);
                                 if (on_ctrl_task)
                                   xTaskNotifyGive(current_task);
                               },
                               payload, static_cast<int32_t>(w_length));
    if (on_ctrl_task)
      this->ctrl_submitting_.store(false);
    if (!submitted) {
      // Nothing was handed to the host, so no callback is coming. Waiting for one would
      // spend the full timeout and then report a device that did not answer, which is not
      // what happened.
      ESP_LOGW(TAG, "Control request 0x%02X was not submitted", request);
      return false;
    }

    const uint32_t started = millis();
    if (on_ctrl_task) {
      // The completion wakes this task. A wake left over from an earlier request that timed
      // out only costs one more look at the flag.
      while (!result->done.load(std::memory_order_acquire)) {
        const uint32_t elapsed = millis() - started;
        if (elapsed >= CTRL_TIMEOUT_MS)
          break;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CTRL_TIMEOUT_MS - elapsed));
      }
    } else {
      while (!result->done.load(std::memory_order_acquire) && (millis() - started) < CTRL_TIMEOUT_MS) {
        if (on_loop_task) {
          uint32_t event_flags = 0;
          usb_host_lib_handle_events(0, &event_flags);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }

    if (!result->done.load(std::memory_order_acquire)) {
      // Submitted, but the host never reported it finishing either way. That is not the
      // device turning the request down, so repeating it would only spend the timeout
      // again.
      ESP_LOGW(TAG, "Control request 0x%02X to %s 0x%04X: no completion within %" PRIu32 " ms",
               request, (req_type & usb_host::USB_DIR_IN) != 0 ? "get" : "set", index, CTRL_TIMEOUT_MS);
      return false;
    }
    if (result->success) {
      if (in_data == nullptr)
        return true;
      if (result->data_len >= in_len) {
        memcpy(in_data, result->data, in_len);
        return true;
      }
      // A short answer is the device declining to give what the request asked for; it can
      // answer the next attempt in full.
      short_answer = true;
      last_len = result->data_len;
      continue;
    }
    short_answer = false;
    last_status = result->status;
  }

  if (short_answer) {
    ESP_LOGW(TAG, "Control request 0x%02X to 0x%04X after %u attempts: returned %u of %u bytes", request, index,
             static_cast<unsigned>(UAC_CTRL_ATTEMPTS), static_cast<unsigned>(last_len),
             static_cast<unsigned>(in_len));
  } else {
    ESP_LOGW(TAG, "Control request 0x%02X to 0x%04X after %u attempts: %s", request, index,
             static_cast<unsigned>(UAC_CTRL_ATTEMPTS), uac_transfer_status_str(last_status));
  }
  if (rejected != nullptr)
    *rejected = true;
  return false;
}

bool USBAudioDevice::uac_set_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                                             const uint8_t *data, size_t len, bool *rejected) {
  // wValue: (selector << 8) | channel
  // wIndex: (unit_id << 8) | ac_intf_num
  const uint16_t wValue = static_cast<uint16_t>((selector << 8) | channel);
  const uint16_t wIndex = static_cast<uint16_t>((unit_id << 8) | this->ac_intf_);
  return this->uac_control_transfer_(UAC_REQ_TYPE_INTF_SET, UAC_SET_CUR, wValue, wIndex, data, len, nullptr, 0,
                                     rejected);
}

bool USBAudioDevice::uac_get_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                                             uint8_t *data, size_t len) {
  const uint16_t wValue = static_cast<uint16_t>((selector << 8) | channel);
  const uint16_t wIndex = static_cast<uint16_t>((unit_id << 8) | this->ac_intf_);
  // UAC 1.0 has one request code per attribute, so reading the current value has its own.
  // UAC 2.0 has one code for the value in either direction, and bmRequestType says which.
  const uint8_t request = (this->uac_version_ >= UAC_VERSION_2) ? UAC2_CS_CUR : UAC_GET_CUR;
  return this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, request, wValue, wIndex, nullptr, 0, data, len);
}

bool USBAudioDevice::uac_set_cur_endpoint_(uint8_t ep_addr, uint8_t selector,
                                            const uint8_t *data, size_t len) {
  const uint16_t wValue = static_cast<uint16_t>(selector << 8);
  return this->uac_control_transfer_(UAC_REQ_TYPE_EP_SET, UAC_SET_CUR, wValue, ep_addr, data, len, nullptr, 0);
}

uint8_t USBAudioDevice::find_clock_source_(uint8_t terminal_id) {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr || terminal_id == 0)
    return 0;

  // Two passes over the AudioControl interface. The first asks the Terminal which clock it
  // is fed from, the second follows a Clock Selector to the entity behind it, because a
  // Selector is not something a frequency can be set on.
  auto scan = [&](uint8_t want_id, bool follow_selector) -> uint8_t {
    uint16_t total = cfg->wTotalLength;
    int offset = 0;
    const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
    bool in_ac = false;
    while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
      if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
        const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
        in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
                 id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
        continue;
      }
      if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 4)
        continue;
      const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
      if (d[3] != want_id)
        continue;
      const uint8_t subtype = d[2];
      if (!follow_selector) {
        // UAC 2.0 Terminal descriptors: bCSourceID sits after bAssocTerminal on an Input
        // Terminal and after bSourceID as well on an Output Terminal.
        if (subtype == UAC_AC_INPUT_TERMINAL && desc->bLength > 7)
          return d[7];
        if (subtype == UAC_AC_OUTPUT_TERMINAL && desc->bLength > 8)
          return d[8];
        return 0;
      }
      if (subtype == UAC2_AC_CLOCK_SOURCE)
        return want_id;
      // Clock Selector: baCSourceID[0] follows bNrInPins.
      if (subtype == UAC2_AC_CLOCK_SELECTOR && desc->bLength > 5)
        return d[5];
      // Clock Multiplier: bCSourceID follows bClockID.
      if (subtype == UAC2_AC_CLOCK_MULTIPLIER && desc->bLength > 4)
        return d[4];
      return 0;
    }
    return 0;
  };

  const uint8_t source = scan(terminal_id, false);
  if (source == 0)
    return 0;
  const uint8_t clock = scan(source, true);
  return (clock != 0) ? clock : source;
}

bool USBAudioDevice::set_sample_rate_(const UacAltInfo &alt, const UacControlState &ctl, uint32_t freq) {
  if (this->uac_version_ < UAC_VERSION_2) {
    // UAC 1.0: a three byte control on the isochronous endpoint itself. A device is allowed
    // not to have it when it runs at one fixed rate, so a refusal is not fatal.
    uint8_t buf[3] = {
        static_cast<uint8_t>(freq & 0xFF),
        static_cast<uint8_t>((freq >> 8) & 0xFF),
        static_cast<uint8_t>((freq >> 16) & 0xFF),
    };
    const bool ok = this->uac_set_cur_endpoint_(alt.ep_addr, UAC_EP_SAMPLING_FREQ_CONTROL, buf, sizeof(buf));
    if (!ok) {
      ESP_LOGW(TAG, "SET_CUR sampling freq ep=0x%02X freq=%" PRIu32 " failed (may not be supported)", alt.ep_addr,
               freq);
    }
    return ok;
  }

  // UAC 2.0: a four byte control on the Clock Source entity that drives the terminal the
  // stream is attached to. Nothing about it is addressed through the endpoint any more.
  if (ctl.clock_id == 0) {
    ESP_LOGW(TAG, "No Clock Source behind terminal %u; leaving the rate as the device has it", alt.terminal_link);
    return false;
  }
  uint8_t buf[4] = {
      static_cast<uint8_t>(freq & 0xFF),
      static_cast<uint8_t>((freq >> 8) & 0xFF),
      static_cast<uint8_t>((freq >> 16) & 0xFF),
      static_cast<uint8_t>((freq >> 24) & 0xFF),
  };
  const uint16_t wValue = static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL << 8);
  const uint16_t wIndex = static_cast<uint16_t>((ctl.clock_id << 8) | this->ac_intf_);
  if (!this->uac_control_transfer_(UAC_REQ_TYPE_INTF_SET, UAC2_CS_CUR, wValue, wIndex, buf, sizeof(buf), nullptr,
                                   0)) {
    ESP_LOGW(TAG, "SET_CUR sample rate on clock %u to %" PRIu32 " Hz failed", ctl.clock_id, freq);
    return false;
  }

  // The clock is what the stream is then timed by, so what it ended up at decides whether
  // the audio comes out at the right pitch. Saying what we asked for would say nothing.
  uint8_t rb[4] = {0, 0, 0, 0};
  if (this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, UAC2_CS_CUR, wValue, wIndex, nullptr, 0, rb, sizeof(rb))) {
    const uint32_t actual = static_cast<uint32_t>(rb[0]) | (static_cast<uint32_t>(rb[1]) << 8) |
                            (static_cast<uint32_t>(rb[2]) << 16) | (static_cast<uint32_t>(rb[3]) << 24);
    if (actual != freq) {
      ESP_LOGW(TAG, "Clock %u is at %" PRIu32 " Hz, not the requested %" PRIu32 " Hz", ctl.clock_id, actual, freq);
    }
  }
  return true;
}

// -- Volume / mute -------------------------------------------------------------

// Channels a Feature Unit control has to be written to. A device that describes the
// control on the master entry takes a single request for all channels; one that describes
// it per channel needs a request per channel. Returns the number written to channels.
// Every entry of a Feature Unit that carries a control, in the order they should be written.
//
// A unit may carry the same control on its master entry and on its per-channel entries at
// the same time, and the two are separate settings: writing the master leaves whatever the
// channels powered up with in place. Linux builds a control element for each in
// parse_audio_feature_unit() and initialises them all, with no else between the two cases.
// Returning at the master, as this did, meant a device whose channels come up silent stayed
// silent however loud the master was set.
static uint8_t uac_control_channels(bool on_master, uint8_t channel_mask, uint8_t *channels) {
  uint8_t count = 0;
  if (on_master)
    channels[count++] = UAC_FU_MASTER_CHANNEL;
  for (uint8_t i = 0; i < UAC_FU_MAX_CHANNELS; i++) {
    if (channel_mask & (1 << i))
      channels[count++] = static_cast<uint8_t>(i + 1);
  }
  return count;
}

void USBAudioDevice::probe_volume_range_(UacControlState &ctl, const char *what) {
  ctl.range_known = false;
  if (ctl.fu.unit_id == 0 || !ctl.fu.volume_available())
    return;

  // Ask on the same channel the value will be written to.
  uint8_t vol_channels[UAC_FU_MAX_TARGETS];
  const uint8_t vol_count = uac_control_channels(ctl.fu.has_volume, ctl.fu.volume_channels, vol_channels);
  const uint8_t vol_channel = (vol_count > 0) ? vol_channels[0] : UAC_FU_MASTER_CHANNEL;
  const uint16_t wValue = static_cast<uint16_t>((UAC_FU_VOLUME_CONTROL << 8) | vol_channel);
  const uint16_t wIndex = static_cast<uint16_t>((ctl.fu.unit_id << 8) | this->ac_intf_);

  int16_t vol_min = 0;
  int16_t vol_max = 0;
  int16_t vol_res = 0;
  bool range_ok = false;

  if (this->uac_version_ >= UAC_VERSION_2) {
    // One RANGE request answers with the number of sub-ranges followed by a (MIN, MAX, RES)
    // triplet for each, two bytes per value for a volume. A control split into several
    // sub-ranges has gaps in it that cannot be expressed as one scale, so only the first is
    // taken and the rest is left to the device to clamp.
    uint8_t buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, UAC2_CS_RANGE, wValue, wIndex, nullptr, 0, buf,
                                    sizeof(buf))) {
      const uint16_t subranges = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
      if (subranges >= 1) {
        vol_min = static_cast<int16_t>(buf[2] | (buf[3] << 8));
        vol_max = static_cast<int16_t>(buf[4] | (buf[5] << 8));
        vol_res = static_cast<int16_t>(buf[6] | (buf[7] << 8));
        range_ok = true;
        if (subranges > 1) {
          ESP_LOGD(TAG, "%s volume has %u sub-ranges; using the first", what,
                   static_cast<unsigned>(subranges));
        }
      }
    }
  } else {
    auto get_vol_range = [this, wValue, wIndex](uint8_t bRequest, int16_t &out) {
      uint8_t buf[2] = {0, 0};
      if (!this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, bRequest, wValue, wIndex, nullptr, 0, buf,
                                       sizeof(buf)))
        return false;
      ESP_LOGD(TAG,
         "VOLUME RANGE req=0x%02X raw=[%02X %02X]",
         bRequest, buf[0], buf[1]);
      out = static_cast<int16_t>(buf[0] | (buf[1] << 8));
      return true;
    };
    // MAX before MIN, so a device that only answers the first request leaves the more
    // informative of the two behind.
    const bool max_ok = get_vol_range(UAC_GET_MAX, vol_max);
    const bool min_ok = get_vol_range(UAC_GET_MIN, vol_min);
    range_ok = max_ok && min_ok;
    // The resolution says which settings between the endpoints the device can actually
    // take. A device that does not answer is treated as taking every step.
    if (range_ok && !get_vol_range(UAC_GET_RES, vol_res))
      vol_res = 0;
  }

  if (!range_ok) {
    ESP_LOGW(TAG, "%s reported no volume range; falling back to the absolute dB scale", what);
    return;
  }
  // 0x8000 is the reserved "silence" code and never a range endpoint, and a range that is
  // not strictly increasing means the answer is not a scale.
  if (vol_min == UAC_VOLUME_SILENCE || vol_max <= vol_min) {
    ESP_LOGW(TAG, "%s reported an unusable volume range (%d..%d); falling back to the absolute dB scale", what,
             static_cast<int>(vol_min), static_cast<int>(vol_max));
    return;
  }
  // A scale whose loudest setting is still that far down is not a scale a device is
  // playing on; it is a stand-in a device returns when it has nothing to report. Mapping
  // onto it would leave every setting inaudible.
  if (vol_max <= UAC_VOLUME_BOGUS_MAX) {
    ESP_LOGW(TAG, "%s reported a volume range topping out at %.2f dB; treating it as bogus", what,
             static_cast<float>(vol_max) / 256.0f);
    return;
  }
  // A step that is not positive, or one no smaller than the whole scale, describes nothing
  // that can be landed on. Take every step instead.
  if (vol_res <= 0 || vol_res >= static_cast<int32_t>(vol_max) - vol_min)
    vol_res = 1;

  ctl.vol_min = vol_min;
  ctl.vol_max = vol_max;
  ctl.vol_res = vol_res;
  ctl.range_known = true;
  ESP_LOGD(TAG, "%s volume range: %.2f dB to %.2f dB in steps of %.2f dB", what,
           static_cast<float>(ctl.vol_min) / 256.0f, static_cast<float>(ctl.vol_max) / 256.0f,
           static_cast<float>(ctl.vol_res) / 256.0f);

  this->probe_sticky_volume_(ctl, what, vol_channel);
}

// Find out whether GET_CUR tells us anything.
//
// Some devices answer GET_CUR with the same value no matter what was last set. That is
// either a SET_CUR that does nothing or a GET_CUR that reports nothing, and the two cannot
// be told apart from the outside -- so treating a constant answer as proof that the volume
// did not take effect is wrong, and it is what produced "volume not applied as requested"
// on devices whose volume works. Linux reaches the same conclusion in
// check_sticky_volume_control() and keeps the control usable, serving the requested value
// rather than the device's.
//
// Set both endpoints of the device's own range and read back after each. If the answer
// never moves, the answers are worthless and nothing is read back from this device again.
// Whatever the volume was is restored before returning, so probing does not leave the
// device somewhere it was not.
void USBAudioDevice::probe_sticky_volume_(UacControlState &ctl, const char *what, uint8_t channel) {
  ctl.get_cur_broken = false;
  // The probe writes dB values and judges the answers on that scale. A device on the BYTE
  // quirk is on neither, so probing it would set it to two arbitrary levels for nothing.
  if (this->volume_encoding_ != VolumeEncoding::DB)
    return;

  auto get_cur = [this, &ctl, channel](int16_t &out) {
    uint8_t buf[2] = {0, 0};
    if (!this->uac_get_cur_interface_(ctl.fu.unit_id, UAC_FU_VOLUME_CONTROL, channel, buf, sizeof(buf)))
      return false;
    out = static_cast<int16_t>(buf[0] | (buf[1] << 8));
    return true;
  };
  auto set_cur = [this, &ctl, channel](int16_t value) {
    const uint8_t buf[2] = {
      static_cast<uint8_t>(static_cast<uint16_t>(value) & 0xFF),
      static_cast<uint8_t>((static_cast<uint16_t>(value) >> 8) & 0xFF),
    };
    return this->uac_set_cur_interface_(ctl.fu.unit_id, UAC_FU_VOLUME_CONTROL, channel, buf, sizeof(buf));
  };

  int16_t saved = 0;
  if (!get_cur(saved))
    return;  // Nothing answers; there is nothing to distrust.

  const int16_t probes[2] = {ctl.vol_min, ctl.vol_max};
  for (int16_t probe : probes) {
    if (probe == saved)
      continue;
    int16_t seen = 0;
    // A refused write or an unreadable answer says nothing either way, so assume the
    // device is fine and leave the readback in use.
    if (!set_cur(probe) || !get_cur(seen) || seen != saved)
      return;
  }

  ctl.get_cur_broken = true;
  ESP_LOGD(TAG, "%s reports a constant %.2f dB whatever is set; its readback will be ignored", what,
           static_cast<float>(saved) / 256.0f);
  set_cur(saved);
}

// Settle the wire encoding for the device that just connected.
//
// A device that takes the volume as a plain level rather than as dB cannot be recognised by
// asking it: it answers MIN, MAX and RES like any other, and its GET_CUR says nothing
// reliable either. Linux carries the equivalent as a per-device entry in its quirk table
// rather than probing for it, and this does the same from the configuration. Without a
// matching entry the class definition applies.
void USBAudioDevice::resolve_device_quirks_() {
  this->volume_encoding_ = VolumeEncoding::DB;
  const usb_device_desc_t *dev = this->dev_->desc;
  if (dev == nullptr)
    return;
  for (const auto &quirk : this->client_->quirks_) {
    if (quirk.vid != dev->idVendor || quirk.pid != dev->idProduct)
      continue;
    this->volume_encoding_ = quirk.volume_encoding;
    if (quirk.volume_encoding == VolumeEncoding::BYTE) {
      ESP_LOGI(TAG, "Device quirk for %04X:%04X: volume is sent as a level, not as dB", dev->idVendor,
               dev->idProduct);
    }
    return;
  }
}

bool USBAudioDevice::apply_volume_(UacControlState &ctl, const char *what) {
  uint8_t channels[UAC_FU_MAX_TARGETS];
  const uint8_t channel_count = uac_control_channels(ctl.fu.has_volume, ctl.fu.volume_channels, channels);
  if (ctl.fu.unit_id == 0 || channel_count == 0) {
    ESP_LOGD(TAG, "%s volume %.0f%% ignored: device has no volume control", what, ctl.volume * 100.0f);
    return false;
  }
  if (ctl.volume_rejected) {
    ESP_LOGD(TAG, "%s volume %.0f%% ignored: the device rejected its volume control", what, ctl.volume * 100.0f);
    return false;
  }

  // A Feature Unit volume is a dB scale, and MIN, MAX and RES are what the device says
  // about that scale. How a 0..1 setting is placed on it is the configured curve: as a
  // position on the scale, or as an amplitude converted onto it.
  //
  // The reserved 0x8000 silence code is not sent for any of this. It is not part of the
  // scale, a device is free to treat it as an out of range value, and the bottom of the
  // range is the quietest setting the device itself offers anyway.
  const float clamped = std::clamp(ctl.volume, 0.0f, 1.0f);
  int16_t vol;
  if (ctl.range_known) {
    int32_t raw;
    if (this->client_->volume_curve_ == VolumeCurve::LOGARITHMIC && clamped > 0.0f) {
      // Full scale is the top of what the device offers, and each halving of the amplitude
      // is 6.02 dB below that.
      raw = static_cast<int32_t>(ctl.vol_max) + std::lround(20.0 * std::log10(static_cast<double>(clamped)) * 256.0);
    } else if (this->client_->volume_curve_ == VolumeCurve::LOGARITHMIC) {
      raw = ctl.vol_min;
    } else {
      const int32_t span = static_cast<int32_t>(ctl.vol_max) - ctl.vol_min;
      raw = ctl.vol_min + std::lround(static_cast<double>(span) * clamped);
    }
    raw = std::clamp<int32_t>(raw, ctl.vol_min, ctl.vol_max);
    // Settings between two steps are not addressable, so land on the nearest one rather
    // than always on the one below.
    const int32_t res = ctl.vol_res;
    const int32_t stepped = ctl.vol_min + std::lround(static_cast<double>(raw - ctl.vol_min) / res) * res;
    vol = static_cast<int16_t>(std::clamp<int32_t>(stepped, ctl.vol_min, ctl.vol_max));
  } else if (clamped <= 0.0f) {
    vol = UAC_VOLUME_MIN_GAIN;
  } else {
    // With no endpoints from the device the absolute definition is all that is left: the
    // value is gain in dB, so full volume is unity gain and each halving of the amplitude
    // is 6.02 dB below it. There is no scale to place a position on, so the curve setting
    // has nothing to choose between here.
    const int32_t raw = std::lround(20.0 * std::log10(static_cast<double>(clamped)) * 256.0);
    vol = static_cast<int16_t>(std::clamp<int32_t>(raw, UAC_VOLUME_MIN_GAIN, 0));
  }

  // A Feature Unit volume is a signed 16-bit value in 1/256 dB, sent little-endian. That is
  // the whole encoding; there is no percentage anywhere in the class definition. A device
  // carrying the BYTE quirk does not follow it and takes the setting as a plain 0..255
  // level in the low byte instead.
  const bool as_level = this->volume_encoding_ == VolumeEncoding::BYTE;
  uint8_t buf[2];
  if (as_level) {
    buf[0] = static_cast<uint8_t>(std::lround(clamped * 255.0f));
    buf[1] = 0x80;
    ESP_LOGD(TAG, "%s SET volume %.0f%%: level %u (bytes %02X %02X) to %u Feature Unit entries", what,
             clamped * 100.0f, buf[0], buf[0], buf[1], channel_count);
  } else {
    buf[0] = static_cast<uint8_t>(static_cast<uint16_t>(vol) & 0xFF);
    buf[1] = static_cast<uint8_t>((static_cast<uint16_t>(vol) >> 8) & 0xFF);
    ESP_LOGD(TAG, "%s SET volume %.0f%%: %.2f dB (raw %d, bytes %02X %02X) to %u Feature Unit entries", what,
             clamped * 100.0f, static_cast<float>(vol) / 256.0f, static_cast<int>(vol), buf[0], buf[1], channel_count);
  }
  uint8_t written = 0;
  uint8_t refused = 0;
  for (uint8_t i = 0; i < channel_count; i++) {
    bool rejected = false;
    if (this->uac_set_cur_interface_(ctl.fu.unit_id, UAC_FU_VOLUME_CONTROL, channels[i], buf, sizeof(buf),
                                     &rejected)) {
      written++;
    } else if (rejected) {
      refused++;
    }
  }
  const bool ok = written > 0;
  if (written > 0 && written < channel_count)
    ESP_LOGD(TAG, "%s volume reached %u of %u Feature Unit entries", what, written, channel_count);
  const float sent_db = static_cast<float>(vol) / 256.0f;
  if (!ok) {
    ESP_LOGW(TAG, "%s volume %.0f%% was not accepted by the device", what, clamped * 100.0f);
    if (refused == channel_count) {
      ctl.volume_rejected = true;
      ESP_LOGW(TAG, "%s volume is not requested again until the device reconnects", what);
    }
    return false;
  }

  // Read the value back, unless the device has already been found to answer GET_CUR with a
  // constant. A device may clamp to its own range or quantise to a coarser step, so
  // reporting what we sent would be reporting an assumption. A device on the BYTE quirk is
  // not answering on the dB scale in the first place, so there is nothing to compare.
  uint8_t rb[2] = {0, 0};
  if (!as_level && !ctl.get_cur_broken &&
      this->uac_get_cur_interface_(ctl.fu.unit_id, UAC_FU_VOLUME_CONTROL, channels[0], rb, sizeof(rb))) {
    const int16_t actual = static_cast<int16_t>(rb[0] | (rb[1] << 8));
    ESP_LOGD(TAG, "%s volume %.0f%% -> %.2f dB (device reports %.2f dB, raw %02X %02X)", what, clamped * 100.0f,
             sent_db, static_cast<float>(actual) / 256.0f, rb[0], rb[1]);
    // Compare on the device's own scale. Landing within one step of its reported resolution
    // is the device rounding; anything past that is the device overriding what was asked
    // for. Both are its prerogative and neither is an error, so this stays a debug line.
    const int32_t step = ctl.range_known ? ctl.vol_res : 256;
    if (std::abs(static_cast<int32_t>(actual) - vol) > step) {
      ESP_LOGD(TAG, "%s volume settled at %.2f dB instead of %.2f dB", what, static_cast<float>(actual) / 256.0f,
               sent_db);
    }
  } else {
    ESP_LOGD(TAG, "%s volume %.0f%% -> %.2f dB (readback unavailable)", what, clamped * 100.0f, sent_db);
  }
  return true;
}

bool USBAudioDevice::apply_mute_(UacControlState &ctl, const char *what) {
  uint8_t channels[UAC_FU_MAX_TARGETS];
  const uint8_t channel_count = uac_control_channels(ctl.fu.has_mute, ctl.fu.mute_channels, channels);
  if (ctl.fu.unit_id == 0 || channel_count == 0) {
    ESP_LOGD(TAG, "%s mute %s ignored: device has no mute control", what, ONOFF(ctl.muted));
    return false;
  }
  if (ctl.mute_rejected) {
    ESP_LOGD(TAG, "%s mute %s ignored: the device rejected its mute control", what, ONOFF(ctl.muted));
    return false;
  }

  uint8_t val = ctl.muted ? 1 : 0;
  uint8_t written = 0;
  uint8_t refused = 0;
  for (uint8_t i = 0; i < channel_count; i++) {
    bool rejected = false;
    if (this->uac_set_cur_interface_(ctl.fu.unit_id, UAC_FU_MUTE_CONTROL, channels[i], &val, 1, &rejected)) {
      written++;
    } else if (rejected) {
      refused++;
    }
  }
  // A unit can carry the control on the master and on the channels at once and refuse some
  // of those entries, so a refusal is only a failure when nothing at all was written.
  if (written == 0) {
    ESP_LOGW(TAG, "%s mute %s was not accepted by the device", what, ONOFF(ctl.muted));
    if (refused == channel_count) {
      ctl.mute_rejected = true;
      ESP_LOGW(TAG, "%s mute is not requested again until the device reconnects", what);
    }
    return false;
  }
  if (written < channel_count) {
    ESP_LOGD(TAG, "%s mute %s applied to %u of %u Feature Unit entries", what, ONOFF(ctl.muted), written,
             channel_count);
  } else {
    ESP_LOGD(TAG, "%s mute %s (%u entries)", what, ONOFF(ctl.muted), channel_count);
  }
  return true;
}

bool USBAudioDevice::open_speaker_stream_() {
  if (this->spk_stream_open_)
    return true;
  if (!this->client_->spk_cfg_.configured || !this->spk_alt_.valid)
    return false;
  // An open already under way, or a previous one still draining. Either way, starting a
  // second sequence on the same stream would trample the first.
  if (!this->spk_stream_.is_closed())
    return true;

  this->spk_stream_.ep_addr        = this->spk_alt_.ep_addr;
  this->spk_stream_.mps            = this->spk_alt_.mps;
  this->spk_stream_.packets_per_urb = ISOC_PACKETS_PER_URB;
  this->spk_stream_.num_urbs       = ISOC_NUM_URBS;
  this->spk_stream_.interface_num  = this->spk_as_intf_;
  this->spk_stream_.alt_setting    = this->spk_alt_.alt_setting;

  // Sample-clock pacing for the OUT stream. frame_size = channels * subframe; the service
  // interval follows from the device speed and the endpoint's bInterval. packet_size is the
  // floor bytes per interval, packet_size_frac the per-interval frame remainder that the
  // accumulator folds back in (e.g. 44.1 kHz).
  {
    // The wire carries the device's frame, which is wider than the host's whenever a pair is
    // being lifted out of a multichannel stream. Pacing off the host frame would ask the
    // device for a fraction of the bytes it expects per interval.
    const uint32_t frame_size =
    static_cast<uint32_t>(this->spk_alt_.channels) * this->spk_alt_.sub_frame_size;
    const uint32_t derived_ips =
        uac_service_intervals_per_second(this->device_is_high_speed_, this->spk_alt_.b_interval);
    uint32_t frac_div = derived_ips;

    // A UAC endpoint sizes its mps to carry exactly one service interval of audio (plus at
    // most one extra frame for fractional rates and async slack). If the derived interval
    // rate asks for no more than half of mps, the speed or bInterval it came from does not
    // describe this endpoint - most UAC devices are full-speed and are serviced once per
    // 1 ms frame, so an inflated interval rate starves the device by that same factor.
    // Halve back down to what mps actually describes, never below the 1 ms frame rate.
    while (frac_div > 1000u && this->spk_alt_.mps != 0 &&
           ((this->spk_fmt_.sample_rate / frac_div) * frame_size) * 2 <= static_cast<uint32_t>(this->spk_alt_.mps)) {
      frac_div /= 2;
    }
    if (frac_div != derived_ips) {
      ESP_LOGW(TAG, "Speaker pacing: %" PRIu32 " intervals/s does not fit mps=%u, using %" PRIu32 "/s",
               derived_ips, this->spk_alt_.mps, frac_div);
    }

    this->spk_stream_.is_output       = true;
    this->spk_stream_.frame_size      = frame_size;
    this->spk_stream_.frac_div        = frac_div;
    this->spk_stream_.packet_size      = (this->spk_fmt_.sample_rate / frac_div) * frame_size;
    this->spk_stream_.packet_size_frac = this->spk_fmt_.sample_rate % frac_div;
    this->spk_stream_.frac_accum      = 0;
    this->spk_stream_.fb_value.store(0, std::memory_order_relaxed);
    this->spk_stream_.fb_accum        = 0;

    ESP_LOGI(TAG, "Speaker pacing: %s bInterval=%u %" PRIu32 " intervals/s, %" PRIu32
                  " bytes/packet (frac %" PRIu32 "/%" PRIu32 "), frame=%" PRIu32 " bytes, mps=%u",
             this->device_is_high_speed_ ? "HS" : "FS", this->spk_alt_.b_interval, frac_div,
             this->spk_stream_.packet_size, this->spk_stream_.packet_size_frac, frac_div, frame_size,
             this->spk_alt_.mps);
  }

  // Opening continues in on_stream_open() once the device has taken the alt-setting.
  return this->client_->stream_open(*this->dev_, this->spk_stream_, this->client_);
}

bool USBAudioDevice::open_microphone_stream_() {
  if (this->mic_stream_open_)
    return true;
  if (!this->client_->mic_cfg_.configured || !this->mic_alt_.valid)
    return false;
  if (!this->mic_stream_.is_closed())
    return true;

  this->mic_stream_.ep_addr        = this->mic_alt_.ep_addr;
  this->mic_stream_.mps            = this->mic_alt_.mps;
  this->mic_stream_.packets_per_urb = ISOC_PACKETS_PER_URB;
  this->mic_stream_.num_urbs       = ISOC_NUM_URBS;
  this->mic_stream_.interface_num  = this->mic_as_intf_;
  this->mic_stream_.alt_setting    = this->mic_alt_.alt_setting;

  // Opening continues in on_stream_open() once the device has taken the alt-setting.
  return this->client_->stream_open(*this->dev_, this->mic_stream_, this->client_);
}

void USBAudioDevice::close_speaker_stream_() {
  // Unconditional: an open that is still selecting its alt-setting has to be cancelled too,
  // and it has not set spk_stream_open_ yet.
  if (this->spk_fb_open_ || !this->spk_fb_stream_.is_closed()) {
    this->client_->stream_close(this->spk_fb_stream_);
    this->spk_fb_open_ = false;
  }
  this->client_->stream_close(this->spk_stream_);
  this->spk_rate_pending_ = false;
  if (!this->spk_stream_open_)
    return;
  this->spk_stream_open_ = false;
  this->spk_stream_.fb_value.store(0, std::memory_order_relaxed);
  this->flush_speaker_buffer_();
  ESP_LOGI(TAG, "Speaker stream closed");
}

void USBAudioDevice::close_microphone_stream_() {
  this->client_->stream_close(this->mic_stream_);
  this->mic_rate_pending_ = false;
  if (!this->mic_stream_open_)
    return;
  this->mic_stream_open_ = false;
  ESP_LOGI(TAG, "Microphone stream closed");
  if (this->capture_ != nullptr)
    this->capture_->on_stream_event(MicStreamEvent::LOST);
}

// -----------------------------------------------------------------------------
// Connection lifecycle
// -----------------------------------------------------------------------------

void USBAudioDevice::on_connected() {
  const usb_device_desc_t *dev = this->dev_->desc;
  if (dev != nullptr)
    ESP_LOGI(TAG, "USB Audio device connected: VID=0x%04X PID=0x%04X", dev->idVendor, dev->idProduct);
  this->read_identity_();

  // Fresh device: forget everything the previous one taught us.
  this->resolve_device_quirks_();
  this->spk_reopen_at_ms_ = millis();
  this->mic_reopen_at_ms_ = millis();
  this->spk_open_fails_ = 0;
  this->mic_open_fails_ = 0;
  this->spk_format_ok_ = false;
  this->mic_format_ok_ = false;
  this->client_->status_clear_error();

  // Which version of the class the device speaks decides how its descriptors are laid out,
  // so it is settled before any of them is read.
  this->detect_uac_version_();

  if (this->client_->spk_cfg_.configured) {
    this->spk_format_ok_ = this->parse_as_interface_(true, this->client_->spk_cfg_.channels,
                                                     static_cast<uint8_t>(this->client_->spk_cfg_.bits_per_sample),
                                                     this->client_->spk_cfg_.sample_rate,
                                                     this->spk_as_intf_, this->spk_alt_, this->spk_fmt_);
    if (!this->spk_format_ok_) {
      // The device cannot do the configured format (its formats: "Offers" above). Retrying will not change that:
      // said once, the speaker stays off for this device; its microphone (if configured) is served on
      ESP_LOGW(TAG, "Device offers no usable speaker format: no speaker on it");
    }
  }

  if (this->client_->mic_cfg_.configured) {
    this->mic_format_ok_ = this->parse_as_interface_(false, this->client_->mic_cfg_.channels,
                                                     static_cast<uint8_t>(this->client_->mic_cfg_.bits_per_sample),
                                                     this->client_->mic_cfg_.sample_rate,
                                                     this->mic_as_intf_, this->mic_alt_, this->mic_fmt_);
    if (!this->mic_format_ok_) {
      ESP_LOGW(TAG, "Device offers no usable microphone format: no microphone on it");
    }
  }

  // Which AudioControl interface to address depends on which streaming interfaces are in
  // use, so this has to come after they have been picked.
  if (!this->find_ac_interface_()) {
    this->set_stream_error_("Device has no AudioControl interface; not usable as USB audio");
    return;
  }
  this->parse_feature_units_();

  // In UAC 2.0 the sample rate is a property of a Clock Source entity rather than of the
  // endpoint, so the clock behind each stream's terminal is looked up once here.
  if (this->uac_version_ >= UAC_VERSION_2) {
    if (this->spk_format_ok_)
      this->spk_ctl_.clock_id = this->find_clock_source_(this->spk_alt_.terminal_link);
    if (this->mic_format_ok_)
      this->mic_ctl_.clock_id = this->find_clock_source_(this->mic_alt_.terminal_link);
    ESP_LOGD(TAG, "Clock sources: speaker=%u mic=%u", this->spk_ctl_.clock_id, this->mic_ctl_.clock_id);
  }

  // The class requests (rates, volume ranges, volume and mute) run on the control task; the
  // streams open once it is done, see finish_connect_().
  this->setup_seq_ = this->ctrl_gen_.fetch_add(1) + 1;
  this->setup_pending_ = this->post_ctrl_(CtrlOp::SETUP, this->setup_seq_);
  if (!this->setup_pending_)
    this->set_stream_error_("Control task queue full; device setup not started");
}

// Control task: everything a fresh device has to be told before its streams open.
void USBAudioDevice::connect_setup_() {
  // Set the rate now, while every AudioStreaming interface is still on its zero-bandwidth
  // alt-setting. A Clock Source is addressed through the AudioControl interface and does
  // not need the stream, and a device is entitled to refuse or quietly ignore a clock
  // change once an alt-setting is active -- it would then run at its own rate while the
  // host paced at the configured one, which is silence with nothing logged. Linux sets it
  // in the same order, before the interface is selected.
  if (this->uac_version_ >= UAC_VERSION_2) {
    if (this->spk_format_ok_)
      this->set_sample_rate_(this->spk_alt_, this->spk_ctl_, this->spk_fmt_.sample_rate);
    if (this->mic_format_ok_)
      this->set_sample_rate_(this->mic_alt_, this->mic_ctl_, this->mic_fmt_.sample_rate);
  }

  // Feature Unit requests go to the AudioControl interface, so the saved volume and mute
  // state can be sent as soon as the device is there. Nothing about them depends on an
  // AudioStreaming alt-setting being selected or a stream being open.
  this->ctrl_pending_.fetch_and(
      static_cast<uint8_t>(~(PENDING_SPK_VOLUME | PENDING_SPK_MUTE | PENDING_MIC_VOLUME | PENDING_MIC_MUTE)));
  this->spk_ctl_.volume = this->spk_vol_req_.load();
  this->spk_ctl_.muted = this->spk_mute_req_.load();
  this->mic_ctl_.volume = this->mic_vol_req_.load();
  this->mic_ctl_.muted = this->mic_mute_req_.load();
  this->probe_volume_range_(this->spk_ctl_, "Speaker");
  this->apply_volume_(this->spk_ctl_, "Speaker");
  this->apply_mute_(this->spk_ctl_, "Speaker");
  this->spk_ctl_.volume_sent = true;
  this->spk_ctl_.mute_sent = true;
  this->probe_volume_range_(this->mic_ctl_, "Mic");
  this->apply_volume_(this->mic_ctl_, "Mic");
  this->apply_mute_(this->mic_ctl_, "Mic");
  this->mic_ctl_.volume_sent = true;
  this->mic_ctl_.mute_sent = true;
}

// Main loop: the control task has told the device everything; open the streams.
void USBAudioDevice::finish_connect_() {
  this->setup_pending_ = false;
  this->device_connected_ = true;
  // Volume or mute set while the setup ran.
  if (this->ctrl_pending_.load() != 0)
    this->post_controls_();

  // Open whichever streams are needed immediately.
  // With an idle timeout only a stream already in use; the others open when started.
  const bool idle_close = this->client_->stream_idle_timeout_ms_ != 0;
  if (this->client_->spk_cfg_.configured && this->spk_format_ok_ && this->has_speaker_rings_() &&
      (!idle_close || this->spk_wanted_))
    this->note_open_result_(this->open_speaker_stream_(), this->spk_reopen_at_ms_, this->spk_open_fails_);
  if (this->client_->mic_cfg_.configured && this->mic_format_ok_ && this->mic_rb_ != nullptr &&
      !this->mic_suspended_ && (!idle_close || this->mic_wanted_))
    this->note_open_result_(this->open_microphone_stream_(), this->mic_reopen_at_ms_, this->mic_open_fails_);
}

void USBAudioDevice::on_disconnected() {
  // Requests of this connection are not sent any more. One being handed to the host right
  // now finishes first, before the device and its transfer buffers are released.
  this->gone_ = true;
  this->device_connected_ = false;
  this->ctrl_gen_.fetch_add(1);
  while (this->ctrl_submitting_.load())
    vTaskDelay(1);
  this->setup_pending_ = false;
  this->close_speaker_stream_();
  this->close_microphone_stream_();
  this->client_->cancel_timeout(this->timer_id_(0));
  this->client_->cancel_timeout(this->timer_id_(1));
  // The next device has to be told the state from scratch.
  this->spk_ctl_.volume_sent = false;
  this->spk_ctl_.mute_sent = false;
  this->spk_ctl_.range_known = false;
  this->spk_ctl_.volume_rejected = false;
  this->spk_ctl_.mute_rejected = false;
  this->mic_ctl_.volume_sent = false;
  this->mic_ctl_.mute_sent = false;
  this->mic_ctl_.range_known = false;
  this->mic_ctl_.volume_rejected = false;
  this->mic_ctl_.mute_rejected = false;
  this->spk_format_ok_ = false;
  this->mic_format_ok_ = false;
  this->spk_open_fails_ = 0;
  this->mic_open_fails_ = 0;
  // The device is gone, so nothing will ever drain the queued audio. Drop it rather than
  // playing it into the next device that shows up.
  this->flush_speaker_buffer_();
  this->client_->status_clear_error();
  ESP_LOGI(TAG, "USB Audio device disconnected");
}

// Expand host stereo frames into the device's wider frame, writing the pair into its two
// slots and leaving every other slot silent. Returns whole device frames only, so a packet
// never ends mid-frame.
size_t USBAudioDevice::fill_mapped_(uint8_t *data, size_t max_len) {
  const size_t sample = this->spk_alt_.sub_frame_size;
  const size_t dev_frame = sample * this->spk_alt_.channels;
  const size_t host_frame = sample * 2;
  if (sample == 0 || dev_frame == 0 || max_len < dev_frame)
    return 0;

  const size_t frames = max_len / dev_frame;
  // Slots outside the pair are not touched below, so the packet is cleared first.
  memset(data, 0, frames * dev_frame);

  uint8_t host[96];
  const size_t chunk_frames = sizeof(host) / host_frame;
  size_t produced = 0;
  while (produced < frames) {
    const size_t want = std::min(chunk_frames, frames - produced) * host_frame;
    const size_t got = this->read_mixed_(host, want) / host_frame;
    for (size_t i = 0; i < got; i++) {
      uint8_t *dst = data + (produced + i) * dev_frame;
      memcpy(dst + this->spk_alt_.map_left * sample, host + i * host_frame, sample);
      memcpy(dst + this->spk_alt_.map_right * sample, host + i * host_frame + sample, sample);
    }
    produced += got;
    if (got * host_frame < want)
      break;
  }
  return produced * dev_frame;
}

// The capture counterpart: keep the pair's two slots out of each device frame and drop the
// rest, so the ring buffer only ever holds host stereo frames.
void USBAudioDevice::push_mapped_capture_(const uint8_t *data, size_t len) {
  const size_t sample = this->mic_alt_.sub_frame_size;
  const size_t dev_frame = sample * this->mic_alt_.channels;
  const size_t host_frame = sample * 2;
  if (sample == 0 || dev_frame == 0 || len < dev_frame)
    return;

  // Small fixed staging block so a large packet does not need a buffer of its own.
  uint8_t chunk[64];
  size_t chunk_len = 0;
  for (size_t off = 0; off + dev_frame <= len; off += dev_frame) {
    if (chunk_len + host_frame > sizeof(chunk)) {
      this->mic_rb_->write_without_replacement(chunk, chunk_len, 0, false);
      chunk_len = 0;
    }
    memcpy(chunk + chunk_len, data + off + this->mic_alt_.map_left * sample, sample);
    memcpy(chunk + chunk_len + sample, data + off + this->mic_alt_.map_right * sample, sample);
    chunk_len += host_frame;
  }
  if (chunk_len != 0)
    this->mic_rb_->write_without_replacement(chunk, chunk_len, 0, false);
}

// Second half of opening a stream. The endpoint only exists once the device has switched to
// the alt-setting, which is a control transfer, so everything that talks to the running
// stream waits until usb_host reports the outcome here.
void USBAudioDevice::on_stream_open_(usb_host::IsocStream &stream, bool ok) {
  if (&stream == &this->spk_fb_stream_) {
    this->spk_fb_open_ = ok;
    if (ok) {
      ESP_LOGI(TAG, "Speaker async feedback stream open (ep=0x%02X)", this->spk_alt_.feedback_ep_addr);
    } else {
      ESP_LOGW(TAG, "Speaker feedback stream open failed; using nominal pacing");
    }
    return;
  }

  if (&stream == &this->spk_stream_) {
    if (!ok) {
      ESP_LOGE(TAG, "stream_open_ failed for speaker");
      this->note_open_result_(false, this->spk_reopen_at_ms_, this->spk_open_fails_);
      return;
    }
    // Asynchronous OUT endpoint (bmAttributes sync type 0b01) with a companion feedback IN
    // endpoint: open it on the already-claimed AS interface so the device's own clock paces
    // playback. Adaptive/synchronous endpoints (or when disabled) stay on nominal pacing.
    const uint8_t sync_type = (this->spk_alt_.ep_attr >> 2) & 0x03;
    if (this->client_->feedback_enabled_ && sync_type == 0x01 && this->spk_alt_.feedback_ep_addr != 0) {
      this->spk_stream_.fb_shift = this->device_is_high_speed_ ? 16 : 14;
      this->spk_fb_stream_.ep_addr = this->spk_alt_.feedback_ep_addr;
      this->spk_fb_stream_.mps =
          this->spk_alt_.feedback_mps != 0 ? this->spk_alt_.feedback_mps : (this->device_is_high_speed_ ? 4 : 3);
      this->spk_fb_stream_.packets_per_urb = 1;
      this->spk_fb_stream_.num_urbs = 2;
      this->spk_fb_stream_.interface_num = this->spk_as_intf_;
      this->spk_fb_stream_.alt_setting = this->spk_alt_.alt_setting;
      this->spk_fb_stream_.is_output = false;
      this->spk_fb_stream_.owns_interface = false;  // shares the speaker OUT interface
      this->client_->stream_open(*this->dev_, this->spk_fb_stream_, this->client_);
    } else {
      ESP_LOGD(TAG, "Speaker OUT sync type %u, feedback ep 0x%02X (feedback %s)", sync_type,
               this->spk_alt_.feedback_ep_addr, this->client_->feedback_enabled_ ? "enabled" : "disabled");
    }

    // UAC 1.0 only: the sampling frequency is a control on the isochronous endpoint, which
    // exists only now that the alt-setting is active. UAC 2.0 was handled at connect time,
    // before the interface was selected. The stream counts as open once the rate is set.
    if (this->uac_version_ < UAC_VERSION_2) {
      this->spk_rate_seq_ = ++this->ctrl_seq_;
      if (this->post_ctrl_(CtrlOp::SPK_RATE, this->spk_rate_seq_)) {
        this->spk_rate_pending_ = true;
        return;
      }
      ESP_LOGW(TAG, "Control task queue full; speaker sample rate not set");
    }
    this->finish_speaker_open_();
    return;
  }

  if (&stream == &this->mic_stream_) {
    if (!ok) {
      ESP_LOGE(TAG, "stream_open_ failed for microphone");
      this->note_open_result_(false, this->mic_reopen_at_ms_, this->mic_open_fails_);
      if (this->capture_ != nullptr)
        this->capture_->on_stream_event(MicStreamEvent::FAILED);
      return;
    }
    if (this->uac_version_ < UAC_VERSION_2) {
      this->mic_rate_seq_ = ++this->ctrl_seq_;
      if (this->post_ctrl_(CtrlOp::MIC_RATE, this->mic_rate_seq_)) {
        this->mic_rate_pending_ = true;
        return;
      }
      ESP_LOGW(TAG, "Control task queue full; microphone sample rate not set");
    }
    this->finish_microphone_open_();
  }
}

void USBAudioDevice::finish_speaker_open_() {
  this->spk_stream_open_ = true;
  this->note_open_result_(true, this->spk_reopen_at_ms_, this->spk_open_fails_);
  ESP_LOGI(TAG, "Speaker stream open");
  this->ctrl_pending_.fetch_or(PENDING_SPK_VOLUME | PENDING_SPK_MUTE | PENDING_MIC_VOLUME | PENDING_MIC_MUTE);
  this->post_controls_();
}

void USBAudioDevice::finish_microphone_open_() {
  this->mic_stream_open_ = true;
  this->note_open_result_(true, this->mic_reopen_at_ms_, this->mic_open_fails_);
  ESP_LOGI(TAG, "Microphone stream open");
  this->ctrl_pending_.fetch_or(PENDING_SPK_VOLUME | PENDING_SPK_MUTE | PENDING_MIC_VOLUME | PENDING_MIC_MUTE);
  this->post_controls_();
  if (this->capture_ != nullptr)
    this->capture_->on_stream_event(MicStreamEvent::OPEN);
}

bool USBAudioDevice::ensure_started_speaker_() {
  if (!this->device_connected_ || !this->spk_format_ok_)
    return false;
  if (this->spk_stream_open_)
    return true;
  if (!this->reopen_due_(this->spk_reopen_at_ms_, this->spk_open_fails_))
    return false;
  const bool ok = this->open_speaker_stream_();
  this->note_open_result_(ok, this->spk_reopen_at_ms_, this->spk_open_fails_);
  return ok;
}

bool USBAudioDevice::ensure_started_microphone_() {
  if (!this->device_connected_ || !this->mic_format_ok_)
    return false;
  if (this->mic_stream_open_)
    return true;
  if (!this->reopen_due_(this->mic_reopen_at_ms_, this->mic_open_fails_))
    return false;
  const bool ok = this->open_microphone_stream_();
  this->note_open_result_(ok, this->mic_reopen_at_ms_, this->mic_open_fails_);
  return ok;
}

// Main loop, the speaker stopped for the idle timeout: its pipes go back to the host.
void USBAudioDevice::close_idle_speaker_() {
  if (this->spk_wanted_ || this->spk_stream_.is_closed())
    return;
  this->close_speaker_stream_();
  // What the stopped speaker left queued would otherwise play at the next start. Stopped,
  // the USB task reads nothing more from it.
  this->flush_speaker_buffer_();
}

void USBAudioDevice::close_idle_microphone_() {
  if (this->mic_wanted_ || this->mic_stream_.is_closed())
    return;
  this->close_microphone_stream_();
}

bool USBAudioDevice::resume_microphone() {
  this->mic_suspended_ = false;
  this->mic_wanted_ = true;
  this->client_->cancel_timeout(this->timer_id_(1));
  this->client_->enable_loop_soon_any_context();  // a failed open is retried by loop()
  return this->ensure_started_microphone_();
}

void USBAudioDevice::suspend_microphone() {
  this->mic_suspended_ = true;
  this->mic_wanted_ = false;
  if (this->client_->stream_idle_timeout_ms_ != 0)
    this->client_->set_timeout(this->timer_id_(1), this->client_->stream_idle_timeout_ms_,
                               [this]() { this->close_idle_microphone_(); });
}

// Take a volume or mute request for one direction. A request that cannot change what the
// device was last told is dropped: a media player drives several speakers that all end up
// here, so the same setting arrives more than once, and asking the device again for what
// it was just asked is a control request that cannot change anything.
// The control task sends the value; a device that is not set up yet gets the latest one
// with its setup.
void USBAudioDevice::set_volume_level_(std::atomic<float> &request, uint8_t pending_bit, float volume) {
  const float clamped = std::clamp(volume, 0.0f, 1.0f);
  if (request.exchange(clamped) == clamped)
    return;
  this->ctrl_pending_.fetch_or(pending_bit);
  if (this->device_connected_.load())
    this->post_controls_();
}

void USBAudioDevice::set_mute_state_(std::atomic<bool> &request, uint8_t pending_bit, bool mute_state) {
  if (request.exchange(mute_state) == mute_state)
    return;
  this->ctrl_pending_.fetch_or(pending_bit);
  if (this->device_connected_.load())
    this->post_controls_();
}

void USBAudioDevice::set_speaker_volume_level(float volume) {
  this->set_volume_level_(this->spk_vol_req_, PENDING_SPK_VOLUME, volume);
}

void USBAudioDevice::set_speaker_mute_state(bool mute_state) {
  this->set_mute_state_(this->spk_mute_req_, PENDING_SPK_MUTE, mute_state);
}

void USBAudioDevice::set_microphone_volume_level(float volume) {
  this->set_volume_level_(this->mic_vol_req_, PENDING_MIC_VOLUME, volume);
}

void USBAudioDevice::set_microphone_mute_state(bool mute_state) {
  this->set_mute_state_(this->mic_mute_req_, PENDING_MIC_MUTE, mute_state);
}

// -----------------------------------------------------------------------------
// Control task
// -----------------------------------------------------------------------------

bool USBAudioDevice::post_ctrl_(CtrlOp op, uint32_t seq) {
  this->ctrl_refs_.fetch_add(1);
  if (this->client_->post_ctrl_(op, this, seq))
    return true;
  this->ctrl_refs_.fetch_sub(1);
  return false;
}

// One CONTROLS request carries every pending change, so a burst of volume steps queues one.
void USBAudioDevice::post_controls_() {
  if (this->ctrl_controls_queued_.exchange(true))
    return;
  if (!this->post_ctrl_(CtrlOp::CONTROLS, 0)) {
    this->ctrl_controls_queued_.store(false);
    ESP_LOGW(TAG, "Control task queue full; volume/mute change not sent");
  }
}

void USBAudioDevice::run_ctrl_(const CtrlRequest &request) {
  if (request.op == CtrlOp::CONTROLS)
    this->ctrl_controls_queued_.store(false);
  // A request of a connection that is gone; the next device gets its own.
  if (!this->gone_.load() && request.gen == this->ctrl_gen_.load()) {
    this->ctrl_op_gen_ = request.gen;
    switch (request.op) {
      case CtrlOp::SETUP:
        this->connect_setup_();
        this->setup_done_seq_.store(request.seq);
        break;
      case CtrlOp::CONTROLS:
        this->apply_pending_controls_();
        break;
      case CtrlOp::SPK_RATE:
        this->set_sample_rate_(this->spk_alt_, this->spk_ctl_, this->spk_fmt_.sample_rate);
        this->spk_rate_done_seq_.store(request.seq);
        break;
      case CtrlOp::MIC_RATE:
        this->set_sample_rate_(this->mic_alt_, this->mic_ctl_, this->mic_fmt_.sample_rate);
        this->mic_rate_done_seq_.store(request.seq);
        break;
    }
  }
  USBAudioClient *client = this->client_;
  const bool wake = request.op != CtrlOp::CONTROLS || this->gone_.load();
  this->ctrl_refs_.fetch_sub(1);
  if (wake)
    client->enable_loop_soon_any_context();
}

void USBAudioDevice::apply_pending_controls_() {
  const uint8_t pending = this->ctrl_pending_.exchange(0);
  if (pending & PENDING_SPK_VOLUME) {
    this->spk_ctl_.volume = this->spk_vol_req_.load();
    this->apply_volume_(this->spk_ctl_, "Speaker");
    this->spk_ctl_.volume_sent = true;
  }
  if (pending & PENDING_SPK_MUTE) {
    this->spk_ctl_.muted = this->spk_mute_req_.load();
    this->apply_mute_(this->spk_ctl_, "Speaker");
    this->spk_ctl_.mute_sent = true;
  }
  if (pending & PENDING_MIC_VOLUME) {
    this->mic_ctl_.volume = this->mic_vol_req_.load();
    this->apply_volume_(this->mic_ctl_, "Mic");
    this->mic_ctl_.volume_sent = true;
  }
  if (pending & PENDING_MIC_MUTE) {
    this->mic_ctl_.muted = this->mic_mute_req_.load();
    this->apply_mute_(this->mic_ctl_, "Mic");
    this->mic_ctl_.mute_sent = true;
  }
}

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
