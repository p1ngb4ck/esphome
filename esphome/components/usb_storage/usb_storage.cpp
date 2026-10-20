#include "esphome/components/storage/storage.h"
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "usb_storage.h"
#include "esphome/components/storage/fatfs_select.h"
#include "usb_storage_diskio.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <cinttypes>
#include <cerrno>

namespace esphome::usb_storage {

namespace {

// Maps a FATFS FRESULT to the closest StorageError. `for_rmdir` selects FR_DENIED's mapping:
// f_unlink() (used for rmdir -- see rmdir() below, FATFS has no dedicated f_rmdir) returns
// FR_DENIED both for "directory not empty" and for genuine permission/read-only failures, so the
// caller must tell us which context applies.
storage::StorageError fresult_to_storage_error(FRESULT res, bool for_rmdir, bool is_write) {
  switch (res) {
    case FR_OK:
      return storage::StorageError::STORAGE_ERROR_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:
      return storage::StorageError::STORAGE_ERROR_NOT_FOUND;
    case FR_EXIST:
      return storage::StorageError::STORAGE_ERROR_ALREADY_EXISTS;
    case FR_DENIED:
      return for_rmdir ? storage::StorageError::STORAGE_ERROR_NOT_EMPTY
                       : storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED;
    case FR_INVALID_NAME:
      return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
    case FR_NOT_READY:
      return storage::StorageError::STORAGE_ERROR_NOT_READY;
    case FR_WRITE_PROTECTED:
      return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED;
    default:
      return is_write ? storage::StorageError::STORAGE_ERROR_WRITE_ERROR
                      : storage::StorageError::STORAGE_ERROR_READ_ERROR;
  }
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Transfer semaphore helpers
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageDevice::transfer_done_cb(const usb_host::TransferStatus &status, USBStorageDevice *device) {
  device->transfer_ok_ = status.success;
  device->transfer_stalled_ = status.error_code == USB_TRANSFER_STATUS_STALL;
  device->transfer_len_ = static_cast<uint16_t>(status.data_len);
  xSemaphoreGive(device->transfer_sem_);
}

bool USBStorageDevice::wait_transfer_(uint32_t timeout_ms) {
  return xSemaphoreTake(this->transfer_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && this->transfer_ok_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Endpoint parsing
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageDevice::parse_msc_endpoints_() {
  const usb_config_desc_t *cfg = this->dev_->config;
  if (cfg == nullptr)
    return false;

  this->bulk_in_ep_ = 0;
  this->bulk_out_ep_ = 0;
  this->bulk_in_mps_ = 0;
  this->bulk_out_mps_ = 0;

  int offset = 0;
  const usb_standard_desc_t *next = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_msc_intf = false;

  while ((next = usb_parse_next_descriptor(next, cfg->wTotalLength, &offset)) != nullptr) {
    if (next->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(next);
      in_msc_intf = (intf->bInterfaceClass == USB_CLASS_MASS_STORAGE);
      if (in_msc_intf)
        this->msc_interface_ = intf->bInterfaceNumber;
    } else if (in_msc_intf && next->bDescriptorType == USB_W_VALUE_DT_ENDPOINT) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(next);
      if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
        continue;
      if (USB_EP_DESC_GET_EP_DIR(ep)) {
        this->bulk_in_ep_ = ep->bEndpointAddress;
        this->bulk_in_mps_ = USB_EP_DESC_GET_MPS(ep);
      } else {
        this->bulk_out_ep_ = ep->bEndpointAddress;
        this->bulk_out_mps_ = USB_EP_DESC_GET_MPS(ep);
      }
    }
    if (this->bulk_in_ep_ && this->bulk_out_ep_)
      break;
  }

  if (!this->bulk_in_ep_ || !this->bulk_out_ep_) {
    ESP_LOGE(TAG, "Failed to find MSC bulk endpoints (in=0x%02X out=0x%02X)", this->bulk_in_ep_, this->bulk_out_ep_);
    return false;
  }
  if (this->bulk_in_mps_ == 0 || this->bulk_out_mps_ == 0) {
    ESP_LOGE(TAG, "MSC bulk endpoint reports a zero wMaxPacketSize (in %u out %u)", this->bulk_in_mps_,
             this->bulk_out_mps_);
    return false;
  }
  // The client's transfer buffers are sized from this device's own descriptors, so one whole
  // packet of these endpoints always fits, at full speed and at high speed alike.
  ESP_LOGD(TAG, "MSC endpoints: bulk_in=0x%02X (mps %u) bulk_out=0x%02X (mps %u) intf=%d", this->bulk_in_ep_,
           this->bulk_in_mps_, this->bulk_out_ep_, this->bulk_out_mps_, this->msc_interface_);
  return true;
}

uint16_t USBStorageDevice::round_up_to_in_mps_(uint16_t len) const {
  const uint16_t mps = this->bulk_in_mps_;
  if (mps == 0)
    return len;
  return static_cast<uint16_t>(((len + mps - 1) / mps) * mps);
}

// ─────────────────────────────────────────────────────────────────────────────
// BOT primitives
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageDevice::send_cbw_(uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t *cdb, uint8_t cdb_len) {
  MscCbw cbw{};
  cbw.tag = tag;
  cbw.data_transfer_length = data_len;
  cbw.flags = flags;
  cbw.lun = 0;
  cbw.cb_length = cdb_len;
  memcpy(cbw.cb, cdb, cdb_len);

  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  if (!this->client_->transfer_out(*this->dev_, this->bulk_out_ep_, cb, reinterpret_cast<const uint8_t *>(&cbw),
                                   sizeof(MscCbw)))
    return false;
  return this->wait_transfer_();
}

// Endpoint halted by a STALL: cleared on the host side, then on the device (CLEAR_FEATURE ENDPOINT_HALT),
// as the IDF's MSC host driver does.
bool USBStorageDevice::clear_halt_(uint8_t ep) {
  static constexpr uint16_t FEATURE_ENDPOINT_HALT = 0;
  if (usb_host_endpoint_halt(this->dev_->handle, ep) != ESP_OK)
    return false;
  usb_host_endpoint_flush(this->dev_->handle, ep);  // nothing queued: refused, nothing to retire
  if (usb_host_endpoint_clear(this->dev_->handle, ep) != ESP_OK)
    return false;
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  if (!this->client_->control_transfer(
          *this->dev_, usb_host::USB_DIR_OUT | usb_host::USB_TYPE_STANDARD | usb_host::USB_RECIP_ENDPOINT,
          USB_B_REQUEST_CLEAR_FEATURE, FEATURE_ENDPOINT_HALT, ep, cb))
    return false;
  return this->wait_transfer_();
}

bool USBStorageDevice::recv_csw_(uint32_t expected_tag, bool check_condition_expected) {
  MscCsw csw{};
  auto cb = [this, &csw](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(MscCsw))
      memcpy(&csw, s.data, sizeof(MscCsw));
    transfer_done_cb(s, this);
  };
  this->transfer_stalled_ = false;
  if (!this->client_->transfer_in(*this->dev_, this->bulk_in_ep_, cb, this->round_up_to_in_mps_(sizeof(MscCsw))))
    return false;
  if (!this->wait_transfer_()) {
    // A stalled status phase: read the status once more after clearing the halt (BOT 5.3.3)
    if (!this->transfer_stalled_ || !this->clear_halt_(this->bulk_in_ep_))
      return false;
    if (!this->client_->transfer_in(*this->dev_, this->bulk_in_ep_, cb,
                                    this->round_up_to_in_mps_(sizeof(MscCsw))) ||
        !this->wait_transfer_())
      return false;
  }
  if (csw.signature != MSC_BOT_CSW_SIGNATURE) {
    ESP_LOGE(TAG, "Invalid CSW signature: 0x%08" PRIX32, csw.signature);
    return false;
  }
  if (csw.tag != expected_tag) {
    ESP_LOGE(TAG, "CSW tag mismatch: expected %" PRIu32 " got %" PRIu32, expected_tag, csw.tag);
    return false;
  }
  if (csw.status != MSC_BOT_CSW_STATUS_GOOD) {
    if (!check_condition_expected || csw.status != MSC_BOT_CSW_STATUS_FAILED)
      ESP_LOGW(TAG, "CSW status: %d", csw.status);
    return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SCSI commands
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageDevice::scsi_test_unit_ready_() {
  uint8_t cdb[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  uint32_t tag = this->cbw_tag_++;
  return this->send_cbw_(tag, 0, MSC_BOT_CBW_FLAGS_OUT, cdb, sizeof(cdb)) && this->recv_csw_(tag, true);
}

bool USBStorageDevice::scsi_request_sense_(uint8_t &key, uint8_t &asc) {
  static constexpr uint8_t SENSE_LEN = 18;  // fixed format sense data
  uint8_t cdb[6] = {SCSI_CMD_REQUEST_SENSE, 0, 0, 0, SENSE_LEN, 0};
  uint32_t tag = this->cbw_tag_++;
  uint8_t resp[SENSE_LEN]{};

  auto cb = [this, &resp](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len > 0)
      memcpy(resp, s.data, std::min<size_t>(s.data_len, SENSE_LEN));
    transfer_done_cb(s, this);
  };

  if (!this->send_cbw_(tag, SENSE_LEN, MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)))
    return false;
  if (!this->client_->transfer_in(*this->dev_, this->bulk_in_ep_, cb, this->round_up_to_in_mps_(SENSE_LEN)))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (!this->recv_csw_(tag))
    return false;
  key = resp[2] & 0x0F;
  asc = resp[12];
  return true;
}

bool USBStorageDevice::scsi_inquiry_() {
  uint8_t cdb[6] = {SCSI_CMD_INQUIRY, 0, 0, 0, sizeof(ScsiInquiryResponse), 0};
  uint32_t tag = this->cbw_tag_++;
  ScsiInquiryResponse resp{};

  auto cb = [this, &resp](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(ScsiInquiryResponse))
      memcpy(&resp, s.data, sizeof(ScsiInquiryResponse));
    transfer_done_cb(s, this);
  };

  if (!this->send_cbw_(tag, sizeof(ScsiInquiryResponse), MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)))
    return false;
  if (!this->client_->transfer_in(*this->dev_, this->bulk_in_ep_, cb,
                                  this->round_up_to_in_mps_(sizeof(ScsiInquiryResponse))))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (!this->recv_csw_(tag))
    return false;

  char vendor[9]{}, product[17]{};
  memcpy(vendor, resp.vendor_id, 8);
  memcpy(product, resp.product_id, 16);
  ESP_LOGD(TAG, "SCSI INQUIRY: vendor='%s' product='%s' type=0x%02X", vendor, product,
           resp.peripheral_qualifier_type & 0x1F);
  return (resp.peripheral_qualifier_type & 0x1F) == 0x00;
}

bool USBStorageDevice::scsi_read_capacity_() {
  uint8_t cdb[10] = {SCSI_CMD_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t tag = this->cbw_tag_++;
  ScsiReadCapacity10Response resp{};

  auto cb = [this, &resp](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(ScsiReadCapacity10Response))
      memcpy(&resp, s.data, sizeof(ScsiReadCapacity10Response));
    transfer_done_cb(s, this);
  };

  if (!this->send_cbw_(tag, sizeof(ScsiReadCapacity10Response), MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)))
    return false;
  if (!this->client_->transfer_in(*this->dev_, this->bulk_in_ep_, cb,
                                  this->round_up_to_in_mps_(sizeof(ScsiReadCapacity10Response))))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (!this->recv_csw_(tag))
    return false;

  this->sector_count_ = __builtin_bswap32(resp.last_lba) + 1;
  this->sector_size_ = __builtin_bswap32(resp.block_size);
  ESP_LOGD(TAG, "SCSI READ CAPACITY: sectors=%" PRIu32 " sector_size=%" PRIu32 " (%.1f MB)", this->sector_count_,
           this->sector_size_, static_cast<float>(this->sector_count_) * this->sector_size_ / (1024.0f * 1024.0f));
  return this->sector_size_ > 0 && this->sector_count_ > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public DISKIO interface (called from FATFS context -- blocking)
// ─────────────────────────────────────────────────────────────────────────────

// Data phase on the stick's own two transfers (see DataXfer): submitted ahead, completed in
// order, so the next piece is already on the bus while the last one is copied.
void USBStorageDevice::data_done_cb(usb_transfer_t *xfer) { xSemaphoreGive(static_cast<DataXfer *>(xfer->context)->done); }

// Bytes per data transfer: whole packets, at most 127 of them per transfer (the DWC controller's
// limit, see usb_host_transfer_alloc() in usb_host.h), at most the transfer's buffer.
uint32_t USBStorageDevice::data_piece_(uint16_t mps) const {
  if (mps == 0)
    return 0;
  uint32_t piece = std::min<uint32_t>(DATA_XFER_SIZE, 127u * mps);
  return piece - piece % mps;
}

bool USBStorageDevice::data_submit_(DataXfer &d, uint8_t ep, uint32_t len) {
  usb_transfer_t *t = d.xfer;
  xSemaphoreTake(d.done, 0);
  t->device_handle = this->dev_->handle;
  t->bEndpointAddress = ep;
  t->num_bytes = static_cast<int>(len);
  t->flags = 0;
  if (usb_host_transfer_submit(t) != ESP_OK)
    return false;
  d.busy = true;
  return true;
}

// Retires what is still on the bus: a halted and flushed endpoint hands its transfers back.
void USBStorageDevice::data_cancel_(uint8_t ep) {
  bool busy = false;
  for (auto &d : this->data_)
    busy |= d.busy;
  if (!busy)
    return;
  usb_host_endpoint_halt(this->dev_->handle, ep);
  usb_host_endpoint_flush(this->dev_->handle, ep);
  for (auto &d : this->data_) {
    if (d.busy) {
      xSemaphoreTake(d.done, portMAX_DELAY);
      d.busy = false;
    }
  }
  usb_host_endpoint_clear(this->dev_->handle, ep);
}

USBStorageDevice::DataResult USBStorageDevice::data_phase_(bool in, uint8_t *in_buf, const uint8_t *out_buf,
                                                           uint32_t len) {
  const uint8_t ep = in ? this->bulk_in_ep_ : this->bulk_out_ep_;
  const uint32_t piece = this->data_piece_(in ? this->bulk_in_mps_ : this->bulk_out_mps_);
  if (piece == 0)
    return DataResult::FAIL;
  uint32_t submitted = 0;
  uint32_t done = 0;
  uint8_t next = 0;
  uint8_t head = 0;
  DataResult result = DataResult::OK;
  while (done < len) {
    while (submitted < len && !this->data_[next].busy) {
      DataXfer &d = this->data_[next];
      d.len = std::min(piece, len - submitted);
      if (!in)
        memcpy(d.xfer->data_buffer, out_buf + submitted, d.len);
      const uint32_t wire = in ? this->round_up_to_in_mps_(static_cast<uint16_t>(d.len)) : d.len;
      if (!this->data_submit_(d, ep, wire)) {
        result = DataResult::FAIL;
        break;
      }
      submitted += d.len;
      next = (next + 1) % DATA_XFERS;
    }
    if (result != DataResult::OK)
      break;
    DataXfer &d = this->data_[head];
    if (xSemaphoreTake(d.done, pdMS_TO_TICKS(5000)) != pdTRUE) {
      result = DataResult::FAIL;
      break;
    }
    d.busy = false;
    const usb_transfer_t *t = d.xfer;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED || static_cast<uint32_t>(t->actual_num_bytes) != d.len) {
      result = t->status == USB_TRANSFER_STATUS_STALL ? DataResult::STALL : DataResult::FAIL;
      break;
    }
    if (in)
      memcpy(in_buf + done, t->data_buffer, d.len);
    done += d.len;
    head = (head + 1) % DATA_XFERS;
  }
  if (result != DataResult::OK)
    this->data_cancel_(ep);
  return result;
}

// BOT 5.3.4 reset recovery: Bulk-Only Mass Storage Reset, then the halt of both bulk endpoints
// cleared.
bool USBStorageDevice::reset_recovery_() {
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  const bool ok =
      this->client_->control_transfer(
          *this->dev_, usb_host::USB_DIR_OUT | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE,
          MSC_REQUEST_RESET, 0, this->msc_interface_, cb) &&
      this->wait_transfer_();
  this->clear_halt_(this->bulk_in_ep_);
  this->clear_halt_(this->bulk_out_ep_);
  return ok;
}

bool USBStorageDevice::data_command_(bool in, uint8_t *in_buf, const uint8_t *out_buf, uint32_t len, uint32_t tag) {
  const DataResult result = this->data_phase_(in, in_buf, out_buf, len);
  if (result == DataResult::STALL) {
    // BOT 6.7.2 / 6.7.3: the device ended the data phase with a STALL, its status still follows
    ESP_LOGE(TAG, "Data phase stalled");
    if (this->clear_halt_(in ? this->bulk_in_ep_ : this->bulk_out_ep_))
      this->recv_csw_(tag);
    return false;
  }
  if (result != DataResult::OK) {
    ESP_LOGE(TAG, "Data phase failed, reset recovery");
    this->reset_recovery_();
    return false;
  }
  return this->recv_csw_(tag);
}

// The data transfers exist while a stick is connected: allocated on connect, freed on
// disconnect. On the P4 they are DMA buffers in PSRAM, never internal RAM; without room
// there the stick stays unmounted.
bool USBStorageDevice::alloc_data_() {
  if (this->data_[0].xfer != nullptr)
    return true;
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < DATA_XFER_SIZE + 64) {
    ESP_LOGE(TAG, "No %u bytes of PSRAM for a data transfer", static_cast<unsigned>(DATA_XFER_SIZE));
    return false;
  }
#endif
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  bool ok = true;
  for (auto &d : this->data_) {
    if (usb_host_transfer_alloc(DATA_XFER_SIZE, 0, &d.xfer) != ESP_OK) {
      d.xfer = nullptr;
      ok = false;
      break;
    }
    d.xfer->callback = data_done_cb;
    d.xfer->context = &d;
    d.busy = false;
  }
  xSemaphoreGive(this->io_lock_);
  if (!ok) {
    ESP_LOGE(TAG, "Failed to allocate the %u byte data transfers", static_cast<unsigned>(DATA_XFER_SIZE));
    this->free_data_();
  }
  return ok;
}

// Waits for a SCSI command of the storage worker in progress; the device is gone, so its
// transfers come back at once.
void USBStorageDevice::free_data_() {
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  for (auto &d : this->data_) {
    if (d.xfer != nullptr)
      usb_host_transfer_free(d.xfer);
    d.xfer = nullptr;
    d.busy = false;
  }
  xSemaphoreGive(this->io_lock_);
}

bool USBStorageDevice::scsi_read(uint32_t lba, uint8_t *buf, uint32_t count) {
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  const bool ok = this->data_[0].xfer != nullptr && this->scsi_read_(lba, buf, count);
  xSemaphoreGive(this->io_lock_);
  return ok;
}

bool USBStorageDevice::scsi_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  const bool ok = this->data_[0].xfer != nullptr && this->scsi_write_(lba, buf, count);
  xSemaphoreGive(this->io_lock_);
  return ok;
}

// One READ(10) / WRITE(10) for all sectors FATFS asks for at once (up to 65535 per command).
bool USBStorageDevice::scsi_read_(uint32_t lba, uint8_t *buf, uint32_t count) {
  while (count > 0) {
    const uint32_t n = std::min<uint32_t>(count, 0xFFFF);
    uint8_t cdb[10] = {
        SCSI_CMD_READ_10,
        0,
        static_cast<uint8_t>(lba >> 24),
        static_cast<uint8_t>(lba >> 16),
        static_cast<uint8_t>(lba >> 8),
        static_cast<uint8_t>(lba & 0xFF),
        0,
        static_cast<uint8_t>(n >> 8),
        static_cast<uint8_t>(n & 0xFF),
        0,
    };
    const uint32_t bytes = n * this->sector_size_;
    const uint32_t tag = this->cbw_tag_++;
    if (!this->send_cbw_(tag, bytes, MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)) ||
        !this->data_command_(true, buf, nullptr, bytes, tag))
      return false;
    lba += n;
    buf += bytes;
    count -= n;
  }
  return true;
}

bool USBStorageDevice::scsi_write_(uint32_t lba, const uint8_t *buf, uint32_t count) {
  while (count > 0) {
    const uint32_t n = std::min<uint32_t>(count, 0xFFFF);
    uint8_t cdb[10] = {
        SCSI_CMD_WRITE_10,
        0,
        static_cast<uint8_t>(lba >> 24),
        static_cast<uint8_t>(lba >> 16),
        static_cast<uint8_t>(lba >> 8),
        static_cast<uint8_t>(lba & 0xFF),
        0,
        static_cast<uint8_t>(n >> 8),
        static_cast<uint8_t>(n & 0xFF),
        0,
    };
    const uint32_t bytes = n * this->sector_size_;
    const uint32_t tag = this->cbw_tag_++;
    if (!this->send_cbw_(tag, bytes, MSC_BOT_CBW_FLAGS_OUT, cdb, sizeof(cdb)) ||
        !this->data_command_(false, nullptr, buf, bytes, tag))
      return false;
    lba += n;
    buf += bytes;
    count -= n;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageClient -- the sticks
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageClient::loop() {
  // A safe eject waits until nothing references its stick
  bool waiting = false;
  for (auto *stick : this->sticks_) {
    if (stick->unmount_pending_.load(std::memory_order_acquire) && !stick->try_unmount_())
      waiting = true;
  }
  if (!this->process_usb_events_() && !waiting)
    this->disable_loop();
}

USBStorageDevice *USBStorageClient::find_mounted(const char *mount_path) const {
  for (auto *stick : this->sticks_) {
    if (stick->fs_mounted_ && (mount_path == nullptr || mount_path[0] == '\0' ||
                               strcmp(stick->mount_buf_, mount_path) == 0))
      return stick;
  }
  return nullptr;
}

bool USBStorageClient::path_in_use_(const char *mount_path) const {
  for (auto *stick : this->sticks_) {
    if (stick->mount_buf_[0] != '\0' && strcmp(stick->mount_buf_, mount_path) == 0)
      return true;
  }
  return false;
}

// The entry naming this stick, else one without vid/pid: an entry that names it wins over any
// wildcard listed before it. An entry whose mount path another stick has is skipped.
int USBStorageClient::find_entry_(uint16_t vid, uint16_t pid) const {
  if constexpr (usb_host::USB_WHITELIST_USED) {
    for (uint8_t i = 0; i < this->entry_count_; i++) {
      const USBStorageEntry &e = this->entries_[i];
      if (e.vid == vid && e.pid == pid && (vid != 0 || pid != 0) && !this->path_in_use_(e.mount_path))
        return i;
    }
  }
  for (uint8_t i = 0; i < this->entry_count_; i++) {
    const USBStorageEntry &e = this->entries_[i];
    if (e.vid == 0 && e.pid == 0 && !this->path_in_use_(e.mount_path))
      return i;
  }
  return -1;
}

void USBStorageClient::on_device_connected(usb_host::UsbDevice &dev) {
  auto *stick = new USBStorageDevice(this, &dev);  // NOLINT: deleted in free_device_context
  dev.context = stick;
  this->sticks_.push_back(stick);
  stick->start_();  // on failure the stick is closed again (stop_() and free_device_context() follow)
}

void USBStorageClient::on_device_disconnected(usb_host::UsbDevice &dev) {
  auto *stick = static_cast<USBStorageDevice *>(dev.context);
  if (stick == nullptr)
    return;
  stick->stop_();
  this->sticks_.erase(std::remove(this->sticks_.begin(), this->sticks_.end(), stick), this->sticks_.end());
}

void USBStorageClient::free_device_context(usb_host::UsbDevice &dev) {
  delete static_cast<USBStorageDevice *>(dev.context);  // NOLINT
  dev.context = nullptr;
}

// As many sticks as the build has room for (max_devices); entries naming sticks only, without one
// for any stick, keep the others away.
usb_host::MatchTier USBStorageClient::refine_match_(const usb_host::RoutedDevice &dev, usb_host::MatchTier tier) const {
  if (this->devices().size() >= this->max_devices_)
    return usb_host::MatchTier::NONE;
  if (this->entry_count_ == 0)
    return tier;
  bool any = false;
  for (uint8_t i = 0; i < this->entry_count_; i++) {
    const USBStorageEntry &e = this->entries_[i];
    if (e.vid == 0 && e.pid == 0) {
      any = true;
    } else if constexpr (usb_host::USB_WHITELIST_USED) {
      if (e.vid == dev.desc->idVendor && e.pid == dev.desc->idProduct)
        return usb_host::MatchTier::NAMED;
    }
  }
  return any ? tier : usb_host::MatchTier::NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageDevice -- connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────

USBStorageDevice::~USBStorageDevice() {
  if (this->fatfs_drive_ >= 0)
    usb_diskio_unregister(this->fatfs_drive_);
  for (auto &d : this->data_) {
    if (d.done != nullptr)
      vSemaphoreDelete(d.done);
  }
  if (this->io_lock_ != nullptr)
    vSemaphoreDelete(this->io_lock_);
  if (this->transfer_sem_ != nullptr)
    vSemaphoreDelete(this->transfer_sem_);
}

void USBStorageDevice::start_() {
  const uint16_t vid = this->dev_->desc->idVendor;
  const uint16_t pid = this->dev_->desc->idProduct;
  ESP_LOGI(TAG, "MSC device connected VID=0x%04X PID=0x%04X", vid, pid);

  this->transfer_sem_ = xSemaphoreCreateBinary();
  this->io_lock_ = xSemaphoreCreateMutex();
  bool ok = this->transfer_sem_ != nullptr && this->io_lock_ != nullptr;
  for (auto &d : this->data_) {
    d.done = xSemaphoreCreateBinary();
    ok = ok && d.done != nullptr;
  }
  if (!ok) {
    ESP_LOGE(TAG, "Failed to create the transfer semaphores");
    this->client_->close_device(*this->dev_);
    return;
  }
  this->fatfs_drive_ = usb_diskio_register(this);
  if (this->fatfs_drive_ < 0) {
    ESP_LOGE(TAG, "Failed to register FATFS DISKIO drive");
    this->client_->close_device(*this->dev_);
    return;
  }
  snprintf(this->fatfs_drive_str_, sizeof(this->fatfs_drive_str_), "%d:", this->fatfs_drive_);

  if (!this->alloc_data_() || !this->parse_msc_endpoints_()) {
    this->client_->close_device(*this->dev_);
    return;
  }

  if (!this->client_->claim_interface(*this->dev_, this->msc_interface_, 0)) {
    ESP_LOGE(TAG, "Failed to claim MSC interface %d", this->msc_interface_);
    this->client_->close_device(*this->dev_);
    return;
  }

  if (!this->scsi_inquiry_()) {
    ESP_LOGE(TAG, "SCSI INQUIRY failed");
    this->client_->close_device(*this->dev_);
    return;
  }

  this->ready_tries_ = 0;
  this->check_ready_();
}

// Card readers (and some sticks) answer CHECK CONDITION until the medium is ready -- UNIT ATTENTION
// after power-up or a media change, NOT READY while it comes up -- and hold that condition until
// REQUEST SENSE has read it. As the IDF's MSC host driver does: TEST UNIT READY, REQUEST SENSE on
// failure, every 100 ms for up to 5 s; here from the scheduler, so the main loop never waits for it.
void USBStorageDevice::check_ready_() {
  static constexpr uint8_t SENSE_NO_SENSE = 0x0;
  static constexpr uint8_t SENSE_NOT_READY = 0x2;
  static constexpr uint8_t SENSE_UNIT_ATTENTION = 0x6;
  static constexpr uint32_t READY_INTERVAL_MS = 100;
  static constexpr uint8_t READY_TRIES = 50;

  if (this->scsi_test_unit_ready_()) {
    this->finish_connect_();
    return;
  }
  uint8_t key = 0xFF;
  uint8_t asc = 0;
  if (!this->scsi_request_sense_(key, asc) ||
      (key != SENSE_NO_SENSE && key != SENSE_NOT_READY && key != SENSE_UNIT_ATTENTION)) {
    ESP_LOGE(TAG, "Unit not ready (sense key 0x%X, ASC 0x%02X)", key, asc);
    this->client_->close_device(*this->dev_);
    return;
  }
  if (++this->ready_tries_ >= READY_TRIES) {
    ESP_LOGE(TAG, "Unit not ready after %" PRIu32 " ms (sense key 0x%X, ASC 0x%02X)", READY_INTERVAL_MS * READY_TRIES,
             key, asc);
    this->client_->close_device(*this->dev_);
    return;
  }
  App.scheduler.set_timeout(this, READY_INTERVAL_MS, [this]() { this->check_ready_(); });
}

void USBStorageDevice::finish_connect_() {
  if (!this->scsi_read_capacity_()) {
    ESP_LOGE(TAG, "SCSI READ CAPACITY failed");
    this->client_->close_device(*this->dev_);
    return;
  }

  this->disk_ready_ = true;

  // The mount path of the entry for this stick, else /usb; a path another stick has gets a number
  this->entry_ = this->client_->find_entry_(this->dev_->desc->idVendor, this->dev_->desc->idProduct);
  const char *base = "/usb";
  if (this->entry_ >= 0) {
    base = this->client_->entries_[this->entry_].mount_path;
    this->format_on_mismatch_ = this->client_->entries_[this->entry_].format_on_mismatch;
  }
  // Chosen in a local buffer: this stick's own mount_buf_ stays empty until then, so it never counts as taken
  char path[sizeof(this->mount_buf_)];
  snprintf(path, sizeof(path), "%s", base);
  for (int n = 2; this->client_->path_in_use_(path) && n <= 9; n++)
    snprintf(path, sizeof(path), "%s%d", base, n);
  memcpy(this->mount_buf_, path, sizeof(this->mount_buf_));
  this->set_mount_path_(this->mount_buf_);

#ifdef USE_STORAGE_FILE_SYSTEM_SELECT
  // Before the one and only f_mount below: probe the first sectors and, if a manually
  // requested filesystem mismatches what is on the medium, reformat first -- the mount then
  // happens on the correct filesystem from the start.
  if (!storage::ensure_requested_filesystem(TAG, this->fatfs_drive_, this->fatfs_drive_str_,
                                            this->client_->requested_file_system_, this->format_on_mismatch_)) {
    this->disk_ready_ = false;
    this->client_->close_device(*this->dev_);
    return;
  }
#endif

  FATFS *fs = nullptr;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  esp_vfs_fat_conf_t vfs_conf = {
      .base_path = this->mount_buf_,
      .fat_drive = this->fatfs_drive_str_,
      .max_files = 5,
  };
  esp_err_t err = esp_vfs_fat_register_cfg(&vfs_conf, &fs);
#else
  esp_err_t err = esp_vfs_fat_register(this->mount_buf_, this->fatfs_drive_str_, 5, &fs);
#endif
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(err));
    this->disk_ready_ = false;
    this->client_->close_device(*this->dev_);
    return;
  }

  FRESULT res = f_mount(fs, this->fatfs_drive_str_, 1);
  if (res != FR_OK) {
    ESP_LOGE(TAG, "f_mount failed: %d", res);
    esp_vfs_fat_unregister_path(this->mount_buf_);
    this->disk_ready_ = false;
    this->client_->close_device(*this->dev_);
    return;
  }

  this->mounted_ = true;
#ifdef USE_STORAGE_CHANGE_FEED
  // Mount state is part of the roots listing: whoever flipped it (CD pin, hotplug, HTTP,
  // automation), the browser's change poll must see it -- including recovery after an error.
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->note_dir_changed("");
#endif

  ESP_LOGI(TAG, "FAT filesystem mounted at '%s'", this->mount_buf_);

  this->fs_mounted_ = true;
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->register_storage(this);
  if (this->entry_ >= 0)
    this->client_->on_mounted_.call(this->entry_, this->mount_buf_);
}

void USBStorageDevice::unmount_filesystem_() {
  if (!this->mounted_)
    return;
  f_mount(nullptr, this->fatfs_drive_str_, 0);
  esp_vfs_fat_unregister_path(this->mount_buf_);
  this->mounted_ = false;
#ifdef USE_STORAGE_CHANGE_FEED
  // Mount state is part of the roots listing: whoever flipped it (CD pin, hotplug, HTTP,
  // automation), the browser's change poll must see it -- including recovery after an error.
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->note_dir_changed("");
#endif

  ESP_LOGI(TAG, "FAT filesystem unmounted from '%s'", this->mount_buf_);
}

void USBStorageDevice::stop_() {
  // Transfers still out on the bulk endpoints come back now, so a SCSI command of the storage
  // worker ends at once instead of on its timeouts.
  if (this->dev_->handle != nullptr) {
    for (const uint8_t ep : {this->bulk_in_ep_, this->bulk_out_ep_}) {
      if (ep != 0 && usb_host_endpoint_halt(this->dev_->handle, ep) == ESP_OK)
        usb_host_endpoint_flush(this->dev_->handle, ep);
    }
  }
  App.scheduler.cancel_timeout(this);
  this->unmount_pending_.store(false, std::memory_order_release);
  if (this->fs_mounted_) {
    ESP_LOGI(TAG, "USB Storage Device disconnected (mount_path='%s')", this->mount_buf_);
    this->fs_mounted_ = false;
    if (storage::global_storage_registry != nullptr)
      storage::global_storage_registry->unregister_storage(this);
  }
  this->disk_ready_ = false;

  this->unmount_filesystem_();
  // Waits for a command still on the pipes: the IDF releases an interface only with none of its
  // transfers in flight, and the device close after it needs that.
  if (this->io_lock_ != nullptr)
    this->free_data_();

  if (this->msc_interface_ != 0xFF) {
    this->client_->release_interface(*this->dev_, this->msc_interface_);
    this->msc_interface_ = 0xFF;
  }
}

bool USBStorageDevice::remount_device() {
  if (!this->fs_mounted_) {
    ESP_LOGW(TAG, "Device not mounted, cannot remount");
    return false;
  }
  ESP_LOGI(TAG, "Device remounted successfully");
  if (this->entry_ >= 0)
    this->client_->on_mounted_.call(this->entry_, this->mount_buf_);
  return true;
}

void USBStorageDevice::unmount_device() {
  if (!this->fs_mounted_)
    return;
  // Safe-eject semantics: unregister from the storage registry first (drains worker jobs,
  // see the registry contract), then tear the FAT mount + VFS registration down completely.
  // The device intentionally "disappears" -- get_mount_caps() is UNMOUNT-only, so there is no
  // mount button; the next mount happens when the stick is physically (re)inserted, which
  // re-runs the client's connect flow.
  ESP_LOGI(TAG, "Device unmounted");
  this->fs_mounted_ = false;
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->unregister_storage(this);
  this->unmount_filesystem_();
}

void USBStorageDevice::log_list_dir_start_(const char *path) const { ESP_LOGD(TAG, "Listing files in: %s", path); }

bool USBStorageDevice::log_list_dir_entry(const storage::FileStat *entry, void *ctx) {
  if (entry->is_dir) {
    ESP_LOGD(TAG, "  [DIR]  %s", entry->name);
  } else {
    ESP_LOGD(TAG, "  [FILE] %s (%llu bytes)", entry->name, static_cast<unsigned long long>(entry->size));
  }
  return true;  // keep enumerating -- this is a "list everything" action
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle pool helpers
// ─────────────────────────────────────────────────────────────────────────────

USBFileHandle *USBStorageDevice::alloc_handle_() {
  for (auto &h : this->handle_pool_) {
    if (!h.in_use) {
      h.in_use = true;
      h.storage = this;
      h.file = nullptr;
      return &h;
    }
  }
  return nullptr;
}

void USBStorageDevice::free_handle_(USBFileHandle *handle) {
  handle->in_use = false;
  handle->file = nullptr;
  handle->path = nullptr;
}

void USBStorageDevice::build_path_(char *out, size_t out_size, const char *path) const {
  if (path[0] == '/') {
    snprintf(out, out_size, "%s%s", this->mount_path_, path);
  } else {
    snprintf(out, out_size, "%s/%s", this->mount_path_, path);
  }
}

bool USBStorageDevice::build_fatfs_path_(const char *rel_path, char *out, size_t out_size) const {
  int written;
  if (rel_path[0] == '/') {
    written = snprintf(out, out_size, "%s%s", this->fatfs_drive_str_, rel_path);
  } else {
    written = snprintf(out, out_size, "%s/%s", this->fatfs_drive_str_, rel_path);
  }
  return written > 0 && static_cast<size_t>(written) < out_size;
}

// ─────────────────────────────────────────────────────────────────────────────
// FilesystemStorage interface
// ─────────────────────────────────────────────────────────────────────────────

storage::StorageError USBStorageDevice::get_info(storage::StorageInfo *info) {
  info->id = this->mount_path_;
  info->name = "USB Storage";
  info->kind = "usb";
  info->is_mounted = this->fs_mounted_;
  info->is_removable = true;
  info->is_read_only = false;
  info->block_size = this->sector_size_;
  info->total_bytes = 0;
  info->free_bytes = 0;

  if (this->fs_mounted_) {
    FATFS *fs;
    DWORD fre_clust;
    char path[8];
    snprintf(path, sizeof(path), "%s/", this->mount_path_);
    if (f_getfree(path, &fre_clust, &fs) == FR_OK) {
      DWORD tot_sect = (fs->n_fatent - 2) * fs->csize;
      DWORD fre_sect = fre_clust * fs->csize;
      info->total_bytes = static_cast<uint64_t>(tot_sect) * fs->ssize;
      info->free_bytes = static_cast<uint64_t>(fre_sect) * fs->ssize;
    }
  }
  return storage::StorageError::STORAGE_ERROR_OK;
}

// mount/unmount are driven by USBStorageClient events -- these are no-ops
// when called directly (the client owns the FAT mount lifecycle).
storage::StorageError USBStorageDevice::mount() {
  return this->fs_mounted_ ? storage::StorageError::STORAGE_ERROR_OK : storage::StorageError::STORAGE_ERROR_NOT_READY;
}

bool USBStorageDevice::has_open_handles_() const {
  for (const auto &h : this->handle_pool_) {
    if (h.in_use)
      return true;
  }
  return false;
}

storage::StorageError USBStorageDevice::unmount() {
  // Already unmounted -> target state reached, no error (see design notes).
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_OK;
  // An unmount is already being carried out asynchronously.
  if (this->unmount_pending_.load(std::memory_order_acquire))
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  // Request the unmount; the client's loop completes it once the device is quiescent (no open
  // handle, no worker job referencing this device). Typical USB "safe eject" behaviour: in-flight
  // transfers finish, then we sync and unmount. Returns OK = unmount accepted/initiated.
  this->unmount_pending_.store(true, std::memory_order_release);
  this->client_->enable_loop_soon_any_context();
  ESP_LOGD(TAG, "Unmount requested; will complete once active transfers finish");
  return storage::StorageError::STORAGE_ERROR_OK;
}

bool USBStorageDevice::try_unmount_() {
  // Wait until nothing references the device: no open file handle, and no worker job whose
  // src/dst is this device. The worker keeps a handle open for a transfer's whole duration,
  // so has_open_handles_() already covers active copies; the extra registry check guards the
  // brief open()/close() edges where a job exists but its handle is momentarily not counted.
  if (this->has_open_handles_())
    return false;
#ifdef USE_STORAGE_WORKER
  if (storage::global_storage_worker != nullptr && storage::global_storage_worker->is_busy_with(this))
    return false;
#endif
  // Quiescent -- sync then physically unmount.
  this->sync();
  this->unmount_pending_.store(false, std::memory_order_release);
  this->unmount_device();
  return true;
}

storage::StorageError USBStorageDevice::format() {
  ESP_LOGW(TAG, "Format not implemented for USB storage");
  return storage::StorageError::STORAGE_ERROR_NOT_SUPPORTED;
}

storage::StorageError USBStorageDevice::sync() {
  // Flush any open handle's buffered data to the medium (same as SdStorageBase::sync()).
  // handle->file is a POSIX FILE* on the FAT VFS mount. In the deferred-unmount path there
  // are usually no open handles left (unmount waits for that), but sync() is also callable
  // on its own, and flushing here still commits FATFS-internal buffers on the next write.
  storage::StorageError err = storage::StorageError::STORAGE_ERROR_OK;
  for (auto &h : this->handle_pool_) {
    if (!h.in_use || h.file == nullptr)
      continue;
    if (fflush(h.file) != 0 || fsync(fileno(h.file)) != 0)
      err = storage::StorageError::STORAGE_ERROR_WRITE_ERROR;
  }
  return err;
}

storage::StorageError USBStorageDevice::open(const char *path, storage::FileHandle *&handle, storage::OpenMode mode) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;

  USBFileHandle *h = this->alloc_handle_();
  if (h == nullptr)
    return storage::StorageError::STORAGE_ERROR_TOO_MANY_OPEN_FILES;

  this->build_path_(h->path_buf, sizeof(h->path_buf), path);
  h->path = h->path_buf;

  const char *fmode = "rb";
  switch (mode) {
    case storage::OpenMode::OPEN_MODE_WRITE:
      fmode = "wb";
      break;
    case storage::OpenMode::OPEN_MODE_APPEND:
      fmode = "ab";
      break;
    case storage::OpenMode::OPEN_MODE_READ_WRITE:
      fmode = "r+b";
      break;
    default:
      break;
  }

  h->file = fopen(h->path_buf, fmode);
  if (h->file == nullptr) {
    this->free_handle_(h);
    switch (errno) {
      case ENOENT:
        return storage::StorageError::STORAGE_ERROR_NOT_FOUND;
      case ENOSPC:
        return storage::StorageError::STORAGE_ERROR_NO_SPACE;
      case EACCES:
      case EROFS:
        return storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED;
      case EMFILE:
      case ENFILE:
        return storage::StorageError::STORAGE_ERROR_TOO_MANY_OPEN_FILES;
      default:
        return mode == storage::OpenMode::OPEN_MODE_READ ? storage::StorageError::STORAGE_ERROR_READ_ERROR
                                                         : storage::StorageError::STORAGE_ERROR_WRITE_ERROR;
    }
  }

  handle = h;
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError USBStorageDevice::close(storage::FileHandle *handle) {
  if (handle == nullptr || !handle->in_use)
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  auto *h = static_cast<USBFileHandle *>(handle);
  storage::StorageError err = storage::StorageError::STORAGE_ERROR_OK;
  if (h->file != nullptr) {
    if (fclose(h->file) != 0)
      err = storage::StorageError::STORAGE_ERROR_WRITE_ERROR;
  }
  this->free_handle_(h);
  return err;
}

storage::StorageError USBStorageDevice::read(storage::FileHandle *handle, uint8_t *buf, size_t len,
                                             size_t *bytes_transferred) {
  if (handle == nullptr || !handle->in_use || handle->file == nullptr)
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  auto *h = static_cast<USBFileHandle *>(handle);
  size_t n = fread(buf, 1, len, h->file);
  if (bytes_transferred != nullptr)
    *bytes_transferred = n;
  // fread() returning less than requested means either EOF (not an error, per the
  // partial-read contract in storage.h) or a real I/O error -- ferror() disambiguates.
  if (n < len && ferror(h->file)) {
    clearerr(h->file);
    return storage::StorageError::STORAGE_ERROR_READ_ERROR;
  }
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError USBStorageDevice::write(storage::FileHandle *handle, const uint8_t *buf, size_t len,
                                              size_t *bytes_transferred) {
  if (handle == nullptr || !handle->in_use || handle->file == nullptr)
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  auto *h = static_cast<USBFileHandle *>(handle);
  size_t n = fwrite(buf, 1, len, h->file);
  if (bytes_transferred != nullptr)
    *bytes_transferred = n;
  return n == len ? storage::StorageError::STORAGE_ERROR_OK : storage::StorageError::STORAGE_ERROR_WRITE_ERROR;
}

storage::StorageError USBStorageDevice::seek(storage::FileHandle *handle, int64_t offset, storage::SeekMode mode) {
  if (handle == nullptr || !handle->in_use || handle->file == nullptr)
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  // ESP-IDF's newlib fseek() takes a 32-bit `long` offset -- FATFS/POSIX on this platform can't
  // address beyond that anyway (files >4GB aren't representable here), so reject rather than
  // silently truncating a caller-supplied 64-bit offset (the interface allows >4GB elsewhere,
  // e.g. NetworkStorage).
  if (offset > INT32_MAX || offset < INT32_MIN)
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  int whence;
  switch (mode) {
    case storage::SeekMode::SEEK_MODE_SET:
      whence = SEEK_SET;
      break;
    case storage::SeekMode::SEEK_MODE_CUR:
      whence = SEEK_CUR;
      break;
    case storage::SeekMode::SEEK_MODE_END:
      whence = SEEK_END;
      break;
    default:
      return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  }
  auto *h = static_cast<USBFileHandle *>(handle);
  return fseek(h->file, static_cast<int32_t>(offset), whence) == 0 ? storage::StorageError::STORAGE_ERROR_OK
                                                                   : storage::StorageError::STORAGE_ERROR_READ_ERROR;
}

storage::StorageError USBStorageDevice::tell(storage::FileHandle *handle, uint64_t *position) {
  if (handle == nullptr || !handle->in_use || handle->file == nullptr)
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;
  auto *h = static_cast<USBFileHandle *>(handle);
  int32_t pos = static_cast<int32_t>(ftell(h->file));
  if (pos < 0)
    return storage::StorageError::STORAGE_ERROR_READ_ERROR;
  *position = static_cast<uint64_t>(pos);
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError USBStorageDevice::stat(const char *path, storage::FileStat *st) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;

  // f_stat() on the drive root ("N:/") fails by FATFS design -- synthesize the result instead of
  // calling it. path is "" or "/" exactly when the caller asked to stat the mount point itself.
  if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
    st->name[0] = '\0';
    st->size = 0;
    st->is_dir = true;
    st->mtime = 0;
    return storage::StorageError::STORAGE_ERROR_OK;
  }

  char full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  if (!this->build_fatfs_path_(path, full, sizeof(full)))
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;

  FILINFO fno;
  FRESULT res = f_stat(full, &fno);
  if (res != FR_OK)
    return fresult_to_storage_error(res, /*for_rmdir=*/false, /*is_write=*/false);

  // FileStat::name is the basename only (see the contract on the struct in storage.h) --
  // consistent with what list_dir() puts there, regardless of how many path segments the
  // caller passed in.
  StringRef path_ref(path);
  size_t base_pos = 0;
  for (size_t i = path_ref.size(); i > 0; i--) {
    if (path_ref[i - 1] == '/') {
      base_pos = i;
      break;
    }
  }
  StringRef base_ref(path + base_pos, path_ref.size() - base_pos);
  size_t name_len = base_ref.copy(st->name, sizeof(st->name) - 1);
  st->name[name_len] = '\0';
  st->is_dir = (fno.fattrib & AM_DIR) != 0;
  st->size = st->is_dir ? 0 : static_cast<uint64_t>(fno.fsize);
  st->mtime = 0;  // FatFs FILINFO exposes fdate/ftime (DOS format), not a Unix timestamp.
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError USBStorageDevice::list_dir(const char *path,
                                                 bool (*callback)(const storage::FileStat *entry, void *ctx),
                                                 void *ctx) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  char full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  if (!this->build_fatfs_path_(path, full, sizeof(full)))
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;

  FF_DIR fat_dir;
  FRESULT res = f_opendir(&fat_dir, full);
  if (res != FR_OK)
    return fresult_to_storage_error(res, /*for_rmdir=*/false, /*is_write=*/false);

  FILINFO fno;
  for (;;) {
    if (f_readdir(&fat_dir, &fno) != FR_OK || fno.fname[0] == '\0')
      break;
    StringRef fname_ref(fno.fname);
    if (fname_ref == "." || fname_ref == "..")
      continue;

    storage::FileStat st{};
    size_t name_len = fname_ref.copy(st.name, sizeof(st.name) - 1);
    st.name[name_len] = '\0';
    st.is_dir = (fno.fattrib & AM_DIR) != 0;
    st.size = st.is_dir ? 0 : static_cast<uint64_t>(fno.fsize);
    st.mtime = 0;  // FatFs FILINFO exposes fdate/ftime (DOS format), not a Unix timestamp.

    // callback returns false to stop enumeration early -- that is not an error, so we still
    // return OK below regardless of how the loop exits.
    if (!callback(&st, ctx))
      break;
  }
  f_closedir(&fat_dir);
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError USBStorageDevice::mkdir(const char *path) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  char full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  if (!this->build_fatfs_path_(path, full, sizeof(full)))
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;

  FRESULT res = f_mkdir(full);
  return fresult_to_storage_error(res, /*for_rmdir=*/false, /*is_write=*/true);
}

storage::StorageError USBStorageDevice::rmdir(const char *path) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  char full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  if (!this->build_fatfs_path_(path, full, sizeof(full)))
    return storage::StorageError::STORAGE_ERROR_INVALID_ARGS;

  // Non-recursive per the storage:: contract: must fail with NOT_EMPTY if the directory has
  // contents. Recursive delete is the free storage::remove_recursive() helper, built on top
  // of list_dir()/remove()/this rmdir() -- no need to duplicate that tree-walk here.
  FF_DIR fat_dir;
  FRESULT res = f_opendir(&fat_dir, full);
  if (res != FR_OK)
    return fresult_to_storage_error(res, /*for_rmdir=*/true, /*is_write=*/false);

  bool has_entries = false;
  FILINFO fno;
  while (f_readdir(&fat_dir, &fno) == FR_OK && fno.fname[0] != '\0') {
    StringRef fname_ref(fno.fname);
    if (fname_ref == "." || fname_ref == "..")
      continue;
    has_entries = true;
    break;
  }
  f_closedir(&fat_dir);
  if (has_entries)
    return storage::StorageError::STORAGE_ERROR_NOT_EMPTY;

  // FATFS removes empty directories via f_unlink() -- there is no dedicated f_rmdir().
  res = f_unlink(full);
  return fresult_to_storage_error(res, /*for_rmdir=*/true, /*is_write=*/true);
}

storage::StorageError USBStorageDevice::remove(const char *path) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  char full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  this->build_path_(full, sizeof(full), path);
  return ::remove(full) == 0 ? storage::StorageError::STORAGE_ERROR_OK : storage::StorageError::STORAGE_ERROR_NOT_FOUND;
}

storage::StorageError USBStorageDevice::rename(const char *old_path, const char *new_path) {
  if (!this->fs_mounted_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  char old_full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  char new_full[(ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 1)];
  this->build_path_(old_full, sizeof(old_full), old_path);
  this->build_path_(new_full, sizeof(new_full), new_path);
  // Report why it failed: FatFs refuses an existing destination (FR_EXIST -> EEXIST), and a
  // caller that cannot tell that apart from an I/O error cannot offer to overwrite.
  errno = 0;
  if (::rename(old_full, new_full) != 0)
    return storage::error_from_errno(errno, true);
  return storage::StorageError::STORAGE_ERROR_OK;
}

}  // namespace esphome::usb_storage

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
