#include "screen_capture_component.h"

#ifdef USE_SCREEN_CAPTURE_COMPONENT

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <new>

namespace esphome::screen_capture {

static const char *const TAG = "screen_capture";
static constexpr size_t PIECE_ALIGN = 128;

std::shared_ptr<ring_buffer::RingBuffer> StorageRingBuffer::create(uint8_t *storage, size_t len) {
  if (storage == nullptr)
    return nullptr;
  auto rb = std::allocate_shared<StorageRingBuffer>(
      RAMAllocator<StorageRingBuffer>(RAMAllocator<StorageRingBuffer>::ALLOC_EXTERNAL));
  if (xRingbufferCreateStatic(len, RINGBUF_TYPE_BYTEBUF, storage, &rb->structure_) != rb->handle_())
    return nullptr;
  rb->created_ = true;
  return rb;
}

ScreenCaptureComponent *ScreenCaptureComponent::create() {
  void *mem = RAMAllocator<ScreenCaptureComponent>(RAMAllocator<ScreenCaptureComponent>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) ScreenCaptureComponent();
  return new ScreenCaptureComponent();
}

void ScreenCaptureComponent::setup() {
  this->capture_.setup();
  this->disable_loop();
}

void ScreenCaptureComponent::loop() {
  this->capture_.loop();
  if (this->capture_.idle())
    this->disable_loop();
}

void ScreenCaptureComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Screen capture:\n"
                "  Screenshots: %s\n"
                "  Screencasts: %s",
                this->capture_.get_screenshot_directory().c_str(), this->capture_.get_screencast_directory().c_str());
}

uint8_t *ScreenCaptureComponent::capture_alloc(size_t size) {
  return static_cast<uint8_t *>(
      heap_caps_aligned_calloc(PIECE_ALIGN, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void ScreenCaptureComponent::capture_free(uint8_t *piece) { heap_caps_free(piece); }

const char *ScreenCaptureComponent::capture_relay_url() {
#ifdef USE_SCREEN_CAPTURE_RELAY
  if (this->relay_ != nullptr)
    return this->relay_->get_url();
#endif
  return "";
}

bool ScreenCaptureComponent::capture_relay_ask(bool start) {
#ifdef USE_SCREEN_CAPTURE_RELAY
  if (this->relay_ != nullptr)
    return this->relay_->submit(&ScreenCaptureComponent::cast_job_, this, start ? 1 : 0);
#endif
  return false;
}

uint16_t ScreenCaptureComponent::capture_stream_port() {
#ifdef USE_SCREEN_CAPTURE_RELAY
  if (this->relay_ != nullptr)
    return this->relay_->get_stream_port();
#endif
  return 0;
}

bool ScreenCaptureComponent::capture_http(const char *path, const char *payload, char **body, size_t *len) {
#ifdef USE_SCREEN_CAPTURE_RELAY
  if (this->relay_ != nullptr)
    return this->relay_->http(path, payload, body, len);
#endif
  return false;
}

#ifdef USE_SCREEN_CAPTURE_RELAY
void ScreenCaptureComponent::cast_job_(void *ctx, uint16_t index) {
  static_cast<ScreenCaptureComponent *>(ctx)->capture_.relay_exchange(index != 0);
}
#endif

}

#endif
