// Should not be needed, but it's required to pass CI clang-tidy checks
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "usb_uart.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::usb_uart {

/**
 *
 * Given a configuration, look for the required interfaces defining a CDC-ACM device
 * @param config_desc The configuration descriptor
 * @param intf_idx The index of the interface to be examined
 * @return
 */
static optional<CdcEps> get_cdc(const usb_config_desc_t *config_desc, uint8_t intf_idx) {
  int conf_offset, ep_offset;
  // look for an interface with an interrupt endpoint (notify), and one with two bulk endpoints (data in/out)
  CdcEps eps{};
  for (;;) {
    const auto *intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx++, 0, &conf_offset);
    if (!intf_desc) {
      ESP_LOGE(TAG, "usb_parse_interface_descriptor failed");
      return nullopt;
    }
    ESP_LOGD(TAG, "intf_desc: bInterfaceClass=%02X, bInterfaceSubClass=%02X, bInterfaceProtocol=%02X, bNumEndpoints=%d",
             intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass, intf_desc->bInterfaceProtocol,
             intf_desc->bNumEndpoints);
    for (uint8_t i = 0; i != intf_desc->bNumEndpoints; i++) {
      ep_offset = conf_offset;
      const auto *ep = usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &ep_offset);
      if (!ep) {
        ESP_LOGE(TAG, "Ran out of interfaces at %d before finding all endpoints", i);
        return nullopt;
      }
      ESP_LOGD(TAG, "ep: bEndpointAddress=%02X, bmAttributes=%02X", ep->bEndpointAddress, ep->bmAttributes);
      if (ep->bmAttributes == USB_BM_ATTRIBUTES_XFER_INT) {
        eps.notify_ep = ep;
        eps.interrupt_interface_number = intf_desc->bInterfaceNumber;
        eps.interrupt_interface_class = intf_desc->bInterfaceClass;
      } else if (ep->bmAttributes == USB_BM_ATTRIBUTES_XFER_BULK && ep->bEndpointAddress & usb_host::USB_DIR_IN &&
                 (eps.bulk_interface_number == 0xFF || eps.bulk_interface_number == intf_desc->bInterfaceNumber)) {
        eps.in_ep = ep;
        eps.bulk_interface_number = intf_desc->bInterfaceNumber;
        eps.bulk_interface_class = intf_desc->bInterfaceClass;
      } else if (ep->bmAttributes == USB_BM_ATTRIBUTES_XFER_BULK && !(ep->bEndpointAddress & usb_host::USB_DIR_IN) &&
                 (eps.bulk_interface_number == 0xFF || eps.bulk_interface_number == intf_desc->bInterfaceNumber)) {
        eps.out_ep = ep;
        eps.bulk_interface_number = intf_desc->bInterfaceNumber;
        eps.bulk_interface_class = intf_desc->bInterfaceClass;
      } else {
        ESP_LOGE(TAG, "Unexpected endpoint attributes: %02X", ep->bmAttributes);
        continue;
      }
    }
    if (eps.in_ep != nullptr && eps.out_ep != nullptr && eps.notify_ep != nullptr)
      return eps;
  }
}

std::vector<CdcEps> USBUartTypeCdcAcm::parse_descriptors(usb_device_handle_t dev_hdl) {
  const usb_config_desc_t *config_desc;
  const usb_device_desc_t *device_desc;
  int desc_offset = 0;
  std::vector<CdcEps> cdc_devs{};

  // Get required descriptors
  if (usb_host_get_device_descriptor(dev_hdl, &device_desc) != ESP_OK) {
    ESP_LOGE(TAG, "get_device_descriptor failed");
    return {};
  }
  if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) != ESP_OK) {
    ESP_LOGE(TAG, "get_active_config_descriptor failed");
    return {};
  }
  if (device_desc->bDeviceClass == USB_CLASS_COMM || device_desc->bDeviceClass == USB_CLASS_VENDOR_SPEC) {
    // single CDC-ACM device
    if (auto eps = get_cdc(config_desc, 0)) {
      ESP_LOGV(TAG, "Found CDC-ACM device");
      cdc_devs.push_back(*eps);
    }
    return cdc_devs;
  }
  if (((device_desc->bDeviceClass == USB_CLASS_MISC) && (device_desc->bDeviceSubClass == USB_SUBCLASS_COMMON) &&
       (device_desc->bDeviceProtocol == USB_DEVICE_PROTOCOL_IAD)) ||
      ((device_desc->bDeviceClass == USB_CLASS_PER_INTERFACE) && (device_desc->bDeviceSubClass == USB_SUBCLASS_NULL) &&
       (device_desc->bDeviceProtocol == USB_PROTOCOL_NULL))) {
    // This is a composite device, that uses Interface Association Descriptor
    const auto *this_desc = reinterpret_cast<const usb_standard_desc_t *>(config_desc);
    for (;;) {
      this_desc = usb_parse_next_descriptor_of_type(this_desc, config_desc->wTotalLength,
                                                    USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION, &desc_offset);
      if (!this_desc)
        break;
      const auto *iad_desc = reinterpret_cast<const usb_iad_desc_t *>(this_desc);

      if (iad_desc->bFunctionClass == USB_CLASS_COMM && iad_desc->bFunctionSubClass == USB_CDC_SUBCLASS_ACM) {
        ESP_LOGV(TAG, "Found CDC-ACM device in composite device");
        if (auto eps = get_cdc(config_desc, iad_desc->bFirstInterface))
          cdc_devs.push_back(*eps);
      }
    }
  }
  return cdc_devs;
}

void RingBuffer::push(uint8_t item) {
  if (this->buffer_ == nullptr || this->get_free_space() == 0)
    return;
  this->buffer_[this->insert_pos_] = item;
  this->insert_pos_ = (this->insert_pos_ + 1) % this->buffer_size_;
}
void RingBuffer::push(const uint8_t *data, size_t len) {
  if (this->buffer_ == nullptr)
    return;
  size_t free = this->get_free_space();
  if (len > free)
    len = free;
  for (size_t i = 0; i != len; i++) {
    this->buffer_[this->insert_pos_] = *data++;
    this->insert_pos_ = (this->insert_pos_ + 1) % this->buffer_size_;
  }
}

uint8_t RingBuffer::pop() {
  uint8_t item = this->buffer_[this->read_pos_];
  this->read_pos_ = (this->read_pos_ + 1) % this->buffer_size_;
  return item;
}
size_t RingBuffer::pop(uint8_t *data, size_t len) {
  len = std::min(len, this->get_available());
  for (size_t i = 0; i != len; i++) {
    *data++ = this->buffer_[this->read_pos_];
    this->read_pos_ = (this->read_pos_ + 1) % this->buffer_size_;
  }
  return len;
}

void USBUartChannelBase::write_array(const uint8_t *data, size_t len) {
  if (!this->initialised_.load()) {
    ESP_LOGD(TAG, "Channel not initialised - write ignored");
    return;
  }
#ifdef USE_UART_DEBUGGER
  if (this->debug_) {
    constexpr size_t batch = 16;
    char buf[format_hex_pretty_size(batch)];
    for (size_t off = 0; off < len; off += batch) {
      size_t n = std::min(len - off, batch);
      format_hex_pretty_to(buf, data + off, n, ',');
#ifdef UART_DEBUGGER_ADD_SETTINGS
      if (this->debug_add_settings_) {
        char settings[32];
        snprintf(settings, sizeof(settings), "|%" PRIu32 ":%u:%s:%s|", this->baud_rate_, this->data_bits_,
                 PARITY_NAMES[static_cast<uint8_t>(this->line_parity_())],
                 STOP_BITS_NAMES[static_cast<uint8_t>(this->line_stop_bits_())]);
        ESP_LOGD(TAG, "%s%s>>> %s", settings, this->debug_prefix_.c_str(), buf);
      } else
#endif
        ESP_LOGD(TAG, "%s>>> %s", this->debug_prefix_.c_str(), buf);
    }
  }
#endif
  while (len > 0) {
    UsbOutputChunk *chunk = this->output_pool_.allocate();
    if (chunk == nullptr) {
      ESP_LOGE(TAG, "Output pool full - lost %zu bytes", len);
      break;
    }
    uint16_t chunk_len = std::min(len, UsbOutputChunk::MAX_CHUNK_SIZE);
    memcpy(chunk->data, data, chunk_len);
    chunk->length = chunk_len;
    // Push always succeeds: pool is sized to queue capacity (SIZE-1), so if
    // allocate() returned non-null, the queue cannot be full.
    this->output_queue_.push(chunk);
    data += chunk_len;
    len -= chunk_len;
  }
  if (this->parent_ != nullptr)
    this->parent_->start_output(this);
}

uart::UARTFlushResult USBUartChannelBase::flush() {
  // Spin until the output queue is drained and the last USB transfer completes.
  // Safe to call from the main loop only.
  // The flush_timeout_ms_ timeout guards against a device that stops responding mid-flush;
  // in that case the main loop is blocked for the full duration.
  if (this->parent_ == nullptr)
    return uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  uint32_t start = millis();
  while ((!this->output_queue_.empty() || this->output_started_.load()) && millis() - start < this->flush_timeout_ms_) {
    // Kick start_output() in case data arrived but no transfer is in flight yet.
    this->parent_->start_output(this);
    yield();
  }
  if (!this->output_queue_.empty() || this->output_started_.load())
    return uart::UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
  return uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
}

bool USBUartChannelBase::peek_byte(uint8_t *data) {
  if (this->input_buffer_.is_empty()) {
    return false;
  }
  *data = this->input_buffer_.peek();
  return true;
}
bool USBUartChannelBase::read_array(uint8_t *data, size_t len) {
  if (!this->initialised_.load() && !this->destroying_.load()) {
    ESP_LOGV(TAG, "Channel not initialised - read ignored");
    return false;
  }
  auto available = this->available();
  bool status = true;
  if (len > available) {
    ESP_LOGV(TAG, "underflow: requested %zu but returned %d, bytes", len, available);
    len = available;
    status = false;
  }
  for (size_t i = 0; i != len; i++) {
    *data++ = this->input_buffer_.pop();
  }
  if (this->parent_ != nullptr)
    this->parent_->start_input(this);
  return status;
}
bool USBUartComponent::loop() {
  bool had_work = this->run_config_machine_();

  // Process USB data from the lock-free queue
  UsbDataChunk *chunk;
  while ((chunk = this->usb_data_queue_.pop()) != nullptr) {
    had_work = true;
    auto *channel = chunk->channel;

#ifdef USE_UART_DEBUGGER
    if (channel->debug_) {
      // The hex buffer is a build-time constant while the packet size comes from the device,
      // so a device whose packets are larger than the configured max_packet_size has its
      // debug line truncated. Only the log line is affected, never the data itself.
      char buf[format_hex_pretty_size(usb_host::USB_MAX_PACKET_SIZE)];
      format_hex_pretty_to(buf, chunk->data, std::min<uint16_t>(chunk->length, usb_host::USB_MAX_PACKET_SIZE), ',');
#ifdef UART_DEBUGGER_ADD_SETTINGS
      if (channel->debug_add_settings_) {
        char settings[32];
        snprintf(settings, sizeof(settings), "|%" PRIu32 ":%u:%s:%s|", channel->baud_rate_, channel->data_bits_,
                 PARITY_NAMES[static_cast<uint8_t>(channel->line_parity_())],
                 STOP_BITS_NAMES[static_cast<uint8_t>(channel->line_stop_bits_())]);
        ESP_LOGD(TAG, "%s%s<<< %s", settings, channel->debug_prefix_.c_str(), buf);
      } else
#endif
        ESP_LOGD(TAG, "%s<<< %s", channel->debug_prefix_.c_str(), buf);
    }
#endif

    // If there is not enough space for the full chunk, let the device subclass
    // handle it (e.g. FTDI clears the buffer to prevent mid-telegram corruption).
    if (channel->input_buffer_.get_free_space() < chunk->length) {
      this->on_rx_overflow(channel);
    }
    // Push data to ring buffer (now safe in main loop)
    channel->input_buffer_.push(chunk->data, chunk->length);

    // Return chunk to pool for reuse
    this->chunk_pool_.release(chunk);

    // Invoke the RX callback (if registered) immediately after data lands in the
    // ring buffer.  This lets consumers such as ZigbeeProxy process incoming bytes
    // in the same loop iteration they are delivered, avoiding an extra wakeup cycle.
    if (channel->rx_callback_) {
      channel->rx_callback_();
    }
  }

  // Log dropped USB data periodically
  uint16_t dropped = this->usb_data_queue_.get_and_reset_dropped_count();
  if (dropped > 0) {
    ESP_LOGW(TAG, "Dropped %u USB data chunks due to buffer overflow", dropped);
  }

  // Deferred RX buffer free: once the shared queue is empty and a disconnecting channel's
  // input_buffer_ has been fully consumed by the application, release its backing memory.
  if (this->usb_data_queue_.empty()) {
    for (auto *channel : this->channels_) {
      if (channel->destroying_.load() && channel->input_buffer_.is_empty()) {
        channel->input_buffer_.free_buffer();
        channel->destroying_.store(false);
      }
    }
    // Same deferral for the receive chunk storage: chunks stay valid until the queue that
    // carries them is empty. The next device gets storage sized for its own endpoints.
    if (!this->connected_ && this->chunk_pool_.has_storage())
      this->chunk_pool_.free_storage();
  }
  return had_work;
}
void USBUartComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB UART adapter %04X:%04X", this->vid_, this->pid_);
  for (auto &channel : this->channels_) {
    ESP_LOGCONFIG(TAG,
                  "  UART Channel %d\n"
                  "    Baud Rate: %" PRIu32 " baud\n"
                  "    Data Bits: %u\n"
                  "    Parity: %s\n"
                  "    Stop bits: %s\n"
                  "    Flush Timeout: %" PRIu32 " ms\n"
                  "    Debug: %s\n"
                  "    Dummy receiver: %s",
                  channel->index_, channel->baud_rate_, channel->data_bits_,
                  PARITY_NAMES[static_cast<uint8_t>(channel->line_parity_())],
                  STOP_BITS_NAMES[static_cast<uint8_t>(channel->line_stop_bits_())], channel->flush_timeout_ms_,
                  YESNO(channel->debug_), YESNO(channel->dummy_receiver_));
  }
}
void USBUartComponent::start_input(USBUartChannelBase *channel) {
  if (!channel->initialised_.load())
    return;
  // THREAD CONTEXT: Called from both USB task and main loop threads
  // - USB task: Immediate restart after successful transfer for continuous data flow
  // - Main loop: Controlled restart after consuming data (backpressure mechanism)
  //
  // This dual-thread access is intentional for performance:
  // - USB task restarts avoid context switch delays for high-speed data
  // - Main loop restarts provide flow control when buffers are full
  //
  // The underlying transfer_in() uses lock-free atomic allocation from the
  // TransferRequest pool, making this multi-threaded access safe

  // Use compare_exchange_strong to avoid spurious failures: a missed submit here is
  // never retried by read_array() because no data will ever arrive to trigger it.
  auto started = false;
  if (!channel->input_started_.compare_exchange_strong(started, true))
    return;
  const auto *ep = channel->cdc_dev_.in_ep;
  // CALLBACK CONTEXT: This lambda is executed in USB task via transfer_callback
  auto callback = [this, channel](const usb_host::TransferStatus &status) {
    ESP_LOGV(TAG, "Transfer result: length: %u; status %X", status.data_len, status.error_code);
    if (!status.success) {
      ESP_LOGE(TAG, "Input transfer failed, status=%s", esp_err_to_name(status.error_code));
      // On failure, don't restart - let next read_array() trigger it
      channel->input_started_.store(false);
      return;
    }

    if (!channel->dummy_receiver_ && status.data_len > 0) {
      // Allocate a chunk from the pool
      UsbDataChunk *chunk = this->chunk_pool_.allocate();
      if (chunk == nullptr) {
        // No chunks available - queue is full or we're out of memory
        this->usb_data_queue_.increment_dropped_count();
        // Mark input as not started so we can retry
        channel->input_started_.store(false);
        return;
      }

      // Copy data to chunk (this is fast, happens in USB task)
      memcpy(chunk->data, status.data, status.data_len);
      chunk->length = status.data_len;
      chunk->channel = channel;

      // Push to lock-free queue for main loop processing
      // Push always succeeds: pool is sized to queue capacity (SIZE-1), so if
      // allocate() returned non-null, the queue cannot be full.
      this->usb_data_queue_.push(chunk);

      // Re-enable component loop to process the queued data
      this->enable_loop_soon_any_context();

      // Wake main loop immediately to process USB data
      App.wake_loop_threadsafe();
    }

    // On success, restart input immediately from USB task for performance
    // The lock-free queue will handle backpressure
    channel->input_started_.store(false);
    this->start_input(channel);
  };
  if (!this->transfer_in(ep->bEndpointAddress, callback, ep->wMaxPacketSize)) {
    ESP_LOGE(TAG, "IN transfer submission failed for ep=0x%02X", ep->bEndpointAddress);
    channel->input_started_.store(false);
  }
}

void USBUartComponent::start_output(USBUartChannelBase *channel) {
  // THREAD CONTEXT: Called from both main loop and USB task threads.
  // The output_queue_ is a lock-free SPSC queue, so pop() is safe from either thread.
  // The output_started_ atomic flag is claimed via compare_exchange to guarantee that
  // only one thread starts a transfer at a time.

  // Atomically claim the "output in progress" flag. If already set, another thread
  // is handling the transfer; return immediately.
  bool expected = false;
  if (!channel->output_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  UsbOutputChunk *chunk = channel->output_queue_.pop();
  if (chunk == nullptr) {
    // Nothing to send — release the flag and return.
    channel->output_started_.store(false, std::memory_order_release);
    return;
  }

  const auto *ep = channel->cdc_dev_.out_ep;
  // CALLBACK CONTEXT: This lambda is executed in the USB task via transfer_callback.
  // It releases the chunk, clears the flag, and directly restarts output without
  // going through defer() — eliminating one full main-loop-wakeup cycle of latency.
  auto callback = [this, channel, chunk](const usb_host::TransferStatus &status) {
    if (!status.success) {
      ESP_LOGW(TAG, "Output transfer failed: status %X", status.error_code);
    } else {
      ESP_LOGV(TAG, "Output Transfer result: length: %u; status %X", status.data_len, status.error_code);
    }
    channel->output_pool_.release(chunk);
    channel->output_started_.store(false, std::memory_order_release);
    // Restart directly from USB task — safe because output_queue_ is lock-free
    // and transfer_out() uses thread-safe atomic slot allocation.
    this->start_output(channel);
  };

  const auto len = chunk->length;
  if (!this->transfer_out(ep->bEndpointAddress, callback, chunk->data, len)) {
    // Transfer submission failed — return chunk and release flag so callers can retry.
    channel->output_pool_.release(chunk);
    channel->output_started_.store(false, std::memory_order_release);
    return;
  }
  ESP_LOGV(TAG, "Output %u bytes started", len);
}

void USBUartTypeCdcAcm::on_connected() {
  auto cdc_devs = this->parse_descriptors(this->device_handle_);
  if (cdc_devs.empty()) {
    this->status_set_error(LOG_STR("No CDC-ACM device found"));
    this->disconnect();
    return;
  }
  ESP_LOGD(TAG, "Found %zu CDC-ACM devices", cdc_devs.size());
  if (this->channels_.empty())
    this->add_default_channels_(static_cast<uint8_t>(cdc_devs.size()));
  size_t i = 0;
  for (auto *channel : this->channels_) {
    if (i == cdc_devs.size()) {
      ESP_LOGE(TAG, "No configuration found for channel %d", channel->index_);
      this->status_set_warning(LOG_STR("No configuration found for channel"));
      break;
    }
    if (!channel->input_buffer_.has_buffer()) {
      if (!channel->input_buffer_.allocate()) {
        ESP_LOGE(TAG, "Channel %d: out of memory for RX buffer", channel->index_);
        this->status_set_error(LOG_STR("Out of memory for RX buffer"));
        this->disconnect();
        return;
      }
    }
    channel->destroying_.store(false);
    channel->cdc_dev_ = cdc_devs[i++];
    channel->initialised_.store(true);
    // Claim the communication (interrupt) interface so CDC class requests are accepted
    // by the device. Some CDC ACM implementations (e.g. EFR32 NCP) require this before
    // they enable data flow on the bulk endpoints.
    // Skipped when claim_notification_ep is false to conserve HCD channels.
    if (channel->claim_notification_ep_ && channel->cdc_dev_.interrupt_interface_number != 0xFF &&
        channel->cdc_dev_.interrupt_interface_number != channel->cdc_dev_.bulk_interface_number) {
      if (!this->claim_comm_interface_) {
        ESP_LOGD(TAG, "Comm interface %d left unclaimed", channel->cdc_dev_.interrupt_interface_number);
      } else {
        auto err_comm = usb_host_interface_claim(this->handle_, this->device_handle_,
                                                 channel->cdc_dev_.interrupt_interface_number, 0);
        if (err_comm != ESP_OK) {
          // Continue anyway: the interface number stays valid for CDC request addressing
          ESP_LOGW(TAG, "Could not claim comm interface %d: %s", channel->cdc_dev_.interrupt_interface_number,
                   esp_err_to_name(err_comm));
        } else {
          ESP_LOGD(TAG, "Claimed comm interface %d", channel->cdc_dev_.interrupt_interface_number);
          channel->cdc_dev_.interrupt_interface_claimed = true;
        }
      }
    } else if (!channel->claim_notification_ep_) {
      channel->cdc_dev_.interrupt_interface_number = 0xFF;
    }
    auto err =
        usb_host_interface_claim(this->handle_, this->device_handle_, channel->cdc_dev_.bulk_interface_number, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "usb_host_interface_claim failed: %s, channel=%d, intf=%d", esp_err_to_name(err), channel->index_,
               channel->cdc_dev_.bulk_interface_number);
      this->status_set_error(LOG_STR("usb_host_interface_claim failed"));
      this->disconnect();
      return;
    }
  }
  // One receive chunk has to hold one whole packet of the IN endpoint, so the storage is
  // sized from the endpoints of the device that is actually attached. Taking the widest of
  // them covers every channel out of the one shared pool.
  uint16_t rx_packet_size = 0;
  for (auto *channel : this->channels_) {
    if (channel->cdc_dev_.in_ep != nullptr)
      rx_packet_size = std::max<uint16_t>(rx_packet_size, USB_EP_DESC_GET_MPS(channel->cdc_dev_.in_ep));
  }
  if (!this->chunk_pool_.allocate_storage(rx_packet_size)) {
    ESP_LOGE(TAG, "Out of memory for %u byte receive chunks", rx_packet_size);
    this->status_set_error(LOG_STR("Out of memory for receive chunks"));
    this->disconnect();
    return;
  }
  this->status_clear_error();
  this->enable_channels();
}

void USBUartTypeCdcAcm::on_disconnected() {
  for (auto *channel : this->channels_) {
    // Not set up for this device: it was rejected before on_connected() ran, or it has
    // fewer ports than there are channels. Nothing was claimed for it.
    if (channel->cdc_dev_.in_ep == nullptr)
      continue;
    usb_host_endpoint_halt(this->device_handle_, channel->cdc_dev_.in_ep->bEndpointAddress);
    usb_host_endpoint_flush(this->device_handle_, channel->cdc_dev_.in_ep->bEndpointAddress);
    if (channel->cdc_dev_.out_ep != nullptr) {
      usb_host_endpoint_halt(this->device_handle_, channel->cdc_dev_.out_ep->bEndpointAddress);
      usb_host_endpoint_flush(this->device_handle_, channel->cdc_dev_.out_ep->bEndpointAddress);
    }
    // Only tear down the notify pipe when we claimed its interface ourselves;
    // no transfer is ever submitted on it, so there is nothing else to cancel.
    if (channel->cdc_dev_.notify_ep != nullptr && channel->cdc_dev_.interrupt_interface_claimed) {
      usb_host_endpoint_halt(this->device_handle_, channel->cdc_dev_.notify_ep->bEndpointAddress);
      usb_host_endpoint_flush(this->device_handle_, channel->cdc_dev_.notify_ep->bEndpointAddress);
    }
    if (channel->cdc_dev_.interrupt_interface_claimed) {
      usb_host_interface_release(this->handle_, this->device_handle_, channel->cdc_dev_.interrupt_interface_number);
      channel->cdc_dev_.interrupt_interface_claimed = false;
    }
    usb_host_interface_release(this->handle_, this->device_handle_, channel->cdc_dev_.bulk_interface_number);
    // Reset the input and output started flags to their initial state to avoid the possibility of spurious restarts
    channel->input_started_.store(true);
    channel->output_started_.store(true);
    // Drain any pending output chunks and return them to the pool
    {
      UsbOutputChunk *chunk;
      while ((chunk = channel->output_queue_.pop()) != nullptr) {
        channel->output_pool_.release(chunk);
      }
    }
    channel->initialised_.store(false);
    // Signal loop() to free the RX buffer once usb_data_queue_ and input_buffer_ are drained.
    channel->destroying_.store(true);
    // The descriptors these point into are freed with the device
    channel->cdc_dev_ = {};
    channel->dtr_ = true;
    channel->rts_ = true;
  }
}

static constexpr uint8_t CDC_REQUEST_TYPE = usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE;

bool USBUartTypeCdcAcm::config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok,
                                    const uint8_t *response) {
  static constexpr uint8_t CDC_SET_LINE_CODING = 0x20;

  switch (step) {
    case 0: {
      // Configure the bridge's UART parameters. A USB-UART bridge will not forward data
      // at the correct speed until SET_LINE_CODING is sent; without it the UART may run
      // at an indeterminate default rate so the NCP receives garbled bytes and never
      // sends RSTACK.
      uint32_t baud = channel->baud_rate_;
      std::vector<uint8_t> line_coding = {
          static_cast<uint8_t>(baud & 0xFF),
          static_cast<uint8_t>((baud >> 8) & 0xFF),
          static_cast<uint8_t>((baud >> 16) & 0xFF),
          static_cast<uint8_t>((baud >> 24) & 0xFF),
          static_cast<uint8_t>(channel->line_stop_bits_()),  // bCharFormat
          static_cast<uint8_t>(channel->line_parity_()),     // bParityType
          static_cast<uint8_t>(channel->data_bits_),         // bDataBits
      };
      ESP_LOGD(TAG, "SET_LINE_CODING: baud=%u stop=%s parity=%s data=%u", (unsigned) baud,
               STOP_BITS_NAMES[static_cast<uint8_t>(channel->line_stop_bits_())],
               PARITY_NAMES[static_cast<uint8_t>(channel->line_parity_())], channel->data_bits_);
      this->config_transfer_(CDC_REQUEST_TYPE, CDC_SET_LINE_CODING, 0, channel->cdc_dev_.interrupt_interface_number,
                             line_coding);
      return true;
    }
    case 1:
      // Assert DTR+RTS to signal DTE is present (init only).
      if (reload)
        return false;
      this->modem_control_transfer(channel);
      return true;
    default:
      return false;
  }
}

void USBUartTypeCdcAcm::modem_control_transfer(USBUartChannelBase *channel) {
  static constexpr uint8_t CDC_SET_CONTROL_LINE_STATE = 0x22;
  static constexpr uint16_t CDC_CONTROL_LINE_DTR = 1 << 0;
  static constexpr uint16_t CDC_CONTROL_LINE_RTS = 1 << 1;
  const uint16_t value = (channel->dtr_ ? CDC_CONTROL_LINE_DTR : 0) | (channel->rts_ ? CDC_CONTROL_LINE_RTS : 0);
  ESP_LOGD(TAG, "SET_CONTROL_LINE_STATE: DTR=%s RTS=%s", ONOFF(channel->dtr_), ONOFF(channel->rts_));
  this->config_transfer_(CDC_REQUEST_TYPE, CDC_SET_CONTROL_LINE_STATE, value,
                         channel->cdc_dev_.interrupt_interface_number);
}

void USBUartComponent::enable_channels() {
  this->cfg_single_ = nullptr;
  // Init sends every channel's current line settings and DTR/RTS
  for (auto *channel : this->channels_) {
    channel->reload_pending_ = false;
    channel->modem_pending_ = false;
  }
  this->cfg_channel_idx_ = 0;
  this->start_config_(ConfigMode::CONFIG_MODE_INIT);
}

void USBUartComponent::apply_channel_settings(USBUartChannelBase *channel) {
  if (this->cfg_active_) {
    // A config sequence is already running. Defer this reload until it finishes to preserve
    // the one-control-transfer-at-a-time guarantee (restarting mid-flight would let an
    // in-flight callback complete against fresh state). The per-channel flag coalesces multiple
    // requests; the channel's live settings are read when the reload eventually runs.
    channel->reload_pending_ = true;
    return;
  }
  this->cfg_single_ = channel;
  this->start_config_(ConfigMode::CONFIG_MODE_RELOAD);
}

void USBUartComponent::apply_modem_control(USBUartChannelBase *channel) {
  if (this->cfg_active_) {
    // Deferred like a reload. The transfer sends whatever dtr_/rts_ hold when it runs, so a
    // channel that is set several times before then only sends its latest state.
    channel->modem_pending_ = true;
    return;
  }
  this->cfg_single_ = channel;
  this->start_config_(ConfigMode::CONFIG_MODE_MODEM);
}

void USBUartComponent::start_config_(ConfigMode mode) {
  this->cfg_mode_ = mode;
  this->cfg_device_phase_ = mode == ConfigMode::CONFIG_MODE_INIT;
  this->cfg_step_ = 0;
  this->cfg_ok_ = true;
  this->cfg_in_flight_ = false;
  this->cfg_done_.store(false);
  this->cfg_active_ = true;
  this->enable_loop();
}

void USBUartComponent::config_transfer_(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                                        const std::vector<uint8_t> &data) {
  this->cfg_done_.store(false);
  // The completion callback runs in the USB-task context: it only records the result and
  // wakes the loop. The next transfer is issued from run_config_machine_() on the loop thread.
  bool submitted = this->control_transfer(
      type, request, value, index,
      [this](const usb_host::TransferStatus &status) {
        this->cfg_ok_ = status.success;
        if (!status.success) {
          ESP_LOGW(TAG, "Config control transfer failed: %s", esp_err_to_name(status.error_code));
        } else if (status.data_len > 0) {
          memcpy(this->cfg_response_, status.data, std::min<size_t>(status.data_len, sizeof(this->cfg_response_)));
        }
        // Release: publishes cfg_ok_/cfg_response_ before the loop observes cfg_done_.
        this->cfg_done_.store(true, std::memory_order_release);
        this->enable_loop_soon_any_context();
        App.wake_loop_threadsafe();
      },
      data);
  if (!submitted) {
    // Submission failed (e.g. no free transfer request). No callback will fire, so synthesize
    // a failed completion here so the state machine advances/aborts instead of hanging.
    ESP_LOGW(TAG, "Config control transfer submit failed");
    this->cfg_ok_ = false;
    this->cfg_done_.store(true, std::memory_order_release);
  }
}

bool USBUartComponent::run_config_machine_() {
  if (!this->cfg_active_)
    return false;

  if (this->cfg_in_flight_) {
    // Acquire: pairs with the release in config_transfer_'s callback.
    if (!this->cfg_done_.load(std::memory_order_acquire))
      return false;  // still waiting; the callback will re-wake the loop (no busy spin)
    this->cfg_in_flight_ = false;
    this->cfg_done_.store(false);
    this->cfg_step_++;
  }

  // cfg_ok_ is now synchronized (we only get here on the initial entry or after observing
  // cfg_done_ with acquire ordering), so it is safe to read.
  const bool init = this->cfg_mode_ == ConfigMode::CONFIG_MODE_INIT;
  ESP_LOGV(TAG, "Config machine: device_phase=%d channel_idx=%d step=%d mode=%d ok=%d", this->cfg_device_phase_,
           this->cfg_channel_idx_, this->cfg_step_, static_cast<int>(this->cfg_mode_), this->cfg_ok_);

  // One-time device-level phase (init only). config_device_step() inspects cfg_ok_ itself.
  if (this->cfg_device_phase_) {
    if (this->config_device_step(this->cfg_step_, this->cfg_ok_, this->cfg_response_)) {
      this->cfg_in_flight_ = true;
      return true;
    }
    this->cfg_device_phase_ = false;
    this->cfg_step_ = 0;
    this->cfg_ok_ = true;
  }

  USBUartChannelBase *channel =
      this->cfg_single_ != nullptr
          ? this->cfg_single_
          : (this->cfg_channel_idx_ < this->channels_.size() ? this->channels_[this->cfg_channel_idx_] : nullptr);

  if (channel != nullptr && channel->initialised_.load()) {
    if (!this->cfg_ok_) {
      // A previous step in this channel's sequence failed. Abort the rest. On a full init,
      // mark the channel uninitialised so data flow isn't started on a misconfigured channel;
      // otherwise, leave the already-working channel as it was.
      if (init)
        channel->initialised_.store(false);
    } else if (this->cfg_mode_ == ConfigMode::CONFIG_MODE_MODEM) {
      // A single transfer, so only the first step issues one
      if (this->cfg_step_ == 0) {
        this->modem_control_transfer(channel);
        this->cfg_in_flight_ = true;
        return true;
      }
    } else if (this->config_step(channel, this->cfg_step_, !init, this->cfg_ok_, this->cfg_response_)) {
      this->cfg_in_flight_ = true;
      return true;
    }
  }

  // Channel finished (or aborted). On full init, kick off data flow if still initialised.
  if (channel != nullptr && init && channel->initialised_.load()) {
    channel->input_started_.store(false);
    channel->output_started_.store(false);
    this->start_input(channel);
  }

  // Advance to the next channel (or finish).
  this->cfg_step_ = 0;
  this->cfg_ok_ = true;
  if (this->cfg_single_ != nullptr) {
    this->cfg_active_ = false;
    this->cfg_single_ = nullptr;
  } else if (++this->cfg_channel_idx_ >= this->channels_.size()) {
    this->cfg_active_ = false;
  }

  // If the machine just went idle and a run was requested while it was busy, start it now.
  // A channel's pending modem change waits for its pending reload: the reload's init-only
  // steps are skipped, so it cannot undo the modem lines.
  if (!this->cfg_active_) {
    for (auto *pending : this->channels_) {
      if (pending->reload_pending_) {
        pending->reload_pending_ = false;
        this->cfg_single_ = pending;
        this->start_config_(ConfigMode::CONFIG_MODE_RELOAD);
        break;
      }
      if (pending->modem_pending_) {
        pending->modem_pending_ = false;
        this->cfg_single_ = pending;
        this->start_config_(ConfigMode::CONFIG_MODE_MODEM);
        break;
      }
    }
  }
  return true;
}

LineStopBits USBUartChannelBase::line_stop_bits_() const {
  switch (this->stop_bits_) {
    case UART_CONFIG_STOP_BITS_1_5:
      return LineStopBits::LINE_STOP_BITS_1_5;
    case UART_CONFIG_STOP_BITS_2:
      return LineStopBits::LINE_STOP_BITS_2;
    default:
      return LineStopBits::LINE_STOP_BITS_1;
  }
}

LineParity USBUartChannelBase::line_parity_() const {
  switch (this->parity_) {
    case uart::UART_CONFIG_PARITY_ODD:
      return LineParity::LINE_PARITY_ODD;
    case uart::UART_CONFIG_PARITY_EVEN:
      return LineParity::LINE_PARITY_EVEN;
    case uart::UART_CONFIG_PARITY_MARK:
      return LineParity::LINE_PARITY_MARK;
    case uart::UART_CONFIG_PARITY_SPACE:
      return LineParity::LINE_PARITY_SPACE;
    default:
      return LineParity::LINE_PARITY_NONE;
  }
}

void USBUartChannelBase::load_settings(bool /*dump_config*/) {
  // The per-channel control transfers already log their values at debug level.
  if (this->parent_ != nullptr)
    this->parent_->apply_channel_settings(this);
}

bool USBUartChannelBase::set_modem_control(bool dtr, bool rts) {
  if (!this->initialised_.load() || this->parent_ == nullptr)
    return false;
  const bool changed = dtr != this->dtr_ || rts != this->rts_;
  this->dtr_ = dtr;
  this->rts_ = rts;
  // Nothing to send when the request matches the previous one
  if (changed)
    this->parent_->apply_modem_control(this);
  return true;
}


// ─────────────────────────────────────────────────────────────────────────────
// USBUartComponent: the client's calls for this adapter's device
// ─────────────────────────────────────────────────────────────────────────────

USBUartComponent::USBUartComponent(USBUartClient *client, usb_host::UsbDevice *dev)
    : client_(client),
      dev_(dev),
      handle_(client->handle_),
      device_handle_(dev->handle),
      vid_(dev->desc->idVendor),
      pid_(dev->desc->idProduct) {}

USBUartComponent::~USBUartComponent() {
  this->chunk_pool_.free_storage();
  for (auto *channel : this->channels_) {
    if (this->own_channels_) {
      channel->input_buffer_.free_buffer();
      delete channel;  // NOLINT: made by add_default_channels_()
    } else {
      // A YAML entry's channel stays for its consumers; its next adapter sets the parent again
      channel->set_parent(nullptr);
    }
  }
}

USBUartChannelBase *USBUartComponent::make_channel_(uint8_t index, uint16_t buffer_size) {
  return new USBUartChannel(index, buffer_size);  // NOLINT: deleted with the adapter
}

void USBUartComponent::add_default_channels_(uint8_t count) {
  static constexpr uint16_t DEFAULT_BUFFER_SIZE = 256;
  static constexpr uint32_t DEFAULT_BAUD_RATE = 115200;
  this->own_channels_ = true;
  for (uint8_t i = 0; i < count; i++) {
    USBUartChannelBase *channel = this->make_channel_(i, DEFAULT_BUFFER_SIZE);
    channel->set_parent(this);
    channel->set_baud_rate(DEFAULT_BAUD_RATE);
    channel->set_data_bits(8);
    channel->set_stop_bits(UART_CONFIG_STOP_BITS_1);
    channel->set_parity(uart::UART_CONFIG_PARITY_NONE);
    this->channels_.push_back(channel);
  }
  ESP_LOGI(TAG, "Adapter %04X:%04X: %u channels, 115200 8N1", this->vid_, this->pid_, count);
}

#ifdef USE_USB_BULK_TRANSFERS
bool USBUartComponent::transfer_in(uint8_t ep_address, const usb_host::transfer_cb_t &callback, uint16_t length) {
  return this->client_->transfer_in(*this->dev_, ep_address, callback, length);
}

bool USBUartComponent::transfer_out(uint8_t ep_address, const usb_host::transfer_cb_t &callback, const uint8_t *data,
                                    uint16_t length) {
  return this->client_->transfer_out(*this->dev_, ep_address, callback, data, length);
}
#endif

bool USBUartComponent::control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                                        const usb_host::transfer_cb_t &callback, const std::vector<uint8_t> &data) {
  return this->client_->control_transfer(*this->dev_, type, request, value, index, callback, data);
}

void USBUartComponent::enable_loop() { this->client_->enable_loop(); }

void USBUartComponent::enable_loop_soon_any_context() { this->client_->enable_loop_soon_any_context(); }

void USBUartComponent::disconnect() { this->client_->close_device(*this->dev_); }

void USBUartComponent::status_set_error(const LogString *message) { this->client_->status_set_error(message); }

void USBUartComponent::status_set_warning(const LogString *message) { this->client_->status_set_warning(message); }

void USBUartComponent::status_clear_error() { this->client_->status_clear_error(); }

// ─────────────────────────────────────────────────────────────────────────────
// USBUartClient: every adapter
// ─────────────────────────────────────────────────────────────────────────────

namespace {
struct DefaultType {
  uint16_t vid;
  uint16_t pid;
  UartType type;
};
// An adapter without a YAML entry: its type by vid:pid (the types of __init__.py), else CDC ACM
constexpr DefaultType DEFAULT_TYPES[] = {
    {0x1A86, 0xE018, UartType::CH934X}, {0x1A86, 0x55D9, UartType::CH934X}, {0x1A86, 0x55D5, UartType::CH34X},
    {0x1A86, 0x7523, UartType::CH34X},  {0x10C4, 0xEA60, UartType::CP210X}, {0x0403, 0x6001, UartType::FT23XX},
    {0x0403, 0x6010, UartType::FT23XX}, {0x0403, 0x6011, UartType::FT23XX}, {0x067B, 0x2303, UartType::PL2303},
    {0x067B, 0x23B3, UartType::PL2303}, {0x067B, 0x23A3, UartType::PL2303}, {0x067B, 0x23E3, UartType::PL2303},
    {0x067B, 0x23D3, UartType::PL2303}, {0x067B, 0x23F3, UartType::PL2303}, {0x067B, 0x23C3, UartType::PL2303},
};
}  // namespace

USBUartClient::USBUartClient(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {
  // Any device with a CDC ACM communication interface (class 02/02), and the vendor adapters
  this->add_class_match(0x02, 0x02);
  for (const auto &t : DEFAULT_TYPES)
    this->add_vendor_match(t.vid, t.pid);
}

void USBUartClient::loop() {
  bool had_work = this->process_usb_events_();
  for (auto *adapter : this->adapters_)
    had_work |= adapter->loop();
  // A YAML entry's channel of a gone adapter: its RX buffer goes once the application read it
  for (auto &entry : this->entries_) {
    for (auto *channel : entry.channels) {
      if (channel->destroying_.load() && channel->get_parent() == nullptr && channel->input_buffer_.is_empty()) {
        channel->input_buffer_.free_buffer();
        channel->destroying_.store(false);
      }
    }
  }
  // Disable loop when idle. Callbacks re-enable via enable_loop_soon_any_context().
  if (!had_work)
    this->disable_loop();
}

void USBUartClient::dump_config() {
  USBClient::dump_config();
  for (const auto &entry : this->entries_) {
    ESP_LOGCONFIG(TAG, "  Entry %04X:%04X, %u channels", entry.vid, entry.pid,
                  static_cast<unsigned>(entry.channels.size()));
  }
}

USBUartEntry *USBUartClient::find_entry_(uint16_t vid, uint16_t pid) {
  for (auto &entry : this->entries_) {
    if ((entry.vid != 0 && entry.vid != vid) || (entry.pid != 0 && entry.pid != pid))
      continue;
    bool free = !entry.channels.empty();
    for (auto *adapter : this->adapters_) {
      if (!adapter->own_channels_ && !adapter->channels_.empty() && adapter->channels_[0] == entry.channels[0])
        free = false;
    }
    if (free)
      return &entry;
  }
  return nullptr;
}

void USBUartClient::on_device_connected(usb_host::UsbDevice &dev) {
  const uint16_t vid = dev.desc->idVendor;
  const uint16_t pid = dev.desc->idProduct;
  USBUartEntry *entry = this->find_entry_(vid, pid);
  UartType type = UartType::CDC_ACM;
  if (entry != nullptr) {
    type = entry->type;
  } else {
    for (const auto &t : DEFAULT_TYPES) {
      if (t.vid == vid && t.pid == pid)
        type = t.type;
    }
  }
  USBUartComponent *adapter = nullptr;
  // NOLINTBEGIN: deleted in free_device_context
  switch (type) {
    case UartType::CH34X:
      adapter = new USBUartTypeCH34X(this, &dev);
      break;
    case UartType::CH934X:
      adapter = new USBUartTypeCH934X(this, &dev);
      break;
    case UartType::CP210X:
      adapter = new USBUartTypeCP210X(this, &dev);
      break;
    case UartType::FT23XX:
      adapter = new USBUartTypeFT23XX(this, &dev);
      break;
    case UartType::PL2303:
      adapter = new USBUartTypePL2303(this, &dev);
      break;
    default:
      adapter = new USBUartTypeCdcAcm(this, &dev);
      break;
  }
  // NOLINTEND
  if (entry != nullptr) {
    for (auto *channel : entry->channels) {
      channel->set_parent(adapter);
      adapter->channels_.push_back(channel);
    }
    if (type == UartType::CH934X) {
      static_cast<USBUartTypeCH934X *>(adapter)->set_init_lanes(entry->init_lanes);
    } else if (type != UartType::CH34X && !entry->claim_comm_interface) {
      static_cast<USBUartTypeCdcAcm *>(adapter)->set_claim_comm_interface(false);
    }
  }
  dev.context = adapter;
  this->adapters_.push_back(adapter);
  adapter->on_connected();  // on failure the adapter is closed again (on_device_disconnected follows)
}

void USBUartClient::on_device_disconnected(usb_host::UsbDevice &dev) {
  auto *adapter = static_cast<USBUartComponent *>(dev.context);
  if (adapter == nullptr)
    return;
  adapter->connected_ = false;
  adapter->on_disconnected();
  this->adapters_.erase(std::remove(this->adapters_.begin(), this->adapters_.end(), adapter), this->adapters_.end());
}

void USBUartClient::free_device_context(usb_host::UsbDevice &dev) {
  delete static_cast<USBUartComponent *>(dev.context);  // NOLINT
  dev.context = nullptr;
}

}  // namespace esphome::usb_uart

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31 || USE_ESP32_VARIANT_ESP32H4
