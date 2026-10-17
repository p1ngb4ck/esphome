#include "esphome/components/storage/storage.h"
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "usb_storage.h"
#include "esphome/components/storage/fatfs_select.h"
#include "usb_storage_diskio.h"
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

void USBStorageClient::transfer_done_cb(const usb_host::TransferStatus &status, USBStorageClient *client) {
  client->transfer_ok_ = status.success;
  client->transfer_stalled_ = status.error_code == USB_TRANSFER_STATUS_STALL;
  client->transfer_len_ = static_cast<uint16_t>(status.data_len);
  xSemaphoreGive(client->transfer_sem_);
}

bool USBStorageClient::wait_transfer_(uint32_t timeout_ms) {
  return xSemaphoreTake(this->transfer_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && this->transfer_ok_;
}

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageClient -- setup / loop
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageClient::setup() {
  this->transfer_sem_ = xSemaphoreCreateBinary();
  if (this->transfer_sem_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create transfer semaphore");
    this->mark_failed();
    return;
  }
  this->io_lock_ = xSemaphoreCreateBinary();
  if (this->io_lock_ == nullptr) {
    this->mark_failed();
    return;
  }
  xSemaphoreGive(this->io_lock_);
  for (auto &d : this->data_) {
    d.done = xSemaphoreCreateBinary();
    if (d.done == nullptr) {
      this->mark_failed();
      return;
    }
  }

  // LUN 0 is the entry's own mount, the reader's other slots are the luns entries.
  Lun first;
  first.device = this->devices_.empty() ? nullptr : this->devices_[0];
  first.mount_path = first.device != nullptr ? first.device->get_mount_path() : "/usb";
  this->luns_.push_back(first);
  for (const auto &extra : this->extra_luns_) {
    Lun lun;
    lun.number = extra.number;
    lun.device = extra.device;
    lun.mount_path = extra.device->get_mount_path();
    this->luns_.push_back(lun);
  }
  for (size_t i = 0; i < this->luns_.size(); i++) {
    this->luns_[i].drive = usb_diskio_register(this, static_cast<uint8_t>(i));
    if (this->luns_[i].drive < 0) {
      ESP_LOGE(TAG, "Failed to register FATFS DISKIO drive");
      this->mark_failed();
      return;
    }
    if (this->luns_[i].device != nullptr)
      this->luns_[i].device->set_lun_index(static_cast<uint8_t>(i));
  }

  USBClient::setup();
}

void USBStorageClient::loop() { USBClient::loop(); }

// ─────────────────────────────────────────────────────────────────────────────
// Endpoint parsing
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageClient::parse_msc_endpoints_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
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

uint16_t USBStorageClient::max_chunk_for_mps_(uint16_t mps) const {
  if (mps == 0)
    return 0;
  return static_cast<uint16_t>((this->transfer_capacity() / mps) * mps);
}

uint16_t USBStorageClient::round_up_to_in_mps_(uint16_t len) const {
  const uint16_t mps = this->bulk_in_mps_;
  if (mps == 0)
    return len;
  return static_cast<uint16_t>(((len + mps - 1) / mps) * mps);
}

// ─────────────────────────────────────────────────────────────────────────────
// BOT primitives
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageClient::send_cbw_(uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t *cdb, uint8_t cdb_len,
                                 uint8_t lun) {
  MscCbw cbw{};
  cbw.tag = tag;
  cbw.data_transfer_length = data_len;
  cbw.flags = flags;
  cbw.lun = lun;
  cbw.cb_length = cdb_len;
  memcpy(cbw.cb, cdb, cdb_len);

  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  if (!this->transfer_out(this->bulk_out_ep_, cb, reinterpret_cast<const uint8_t *>(&cbw), sizeof(MscCbw)))
    return false;
  return this->wait_transfer_();
}

bool USBStorageClient::recv_data_(uint8_t *buf, uint16_t len) {
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  if (!this->transfer_in(this->bulk_in_ep_, cb, this->round_up_to_in_mps_(len)))
    return false;
  return this->wait_transfer_();
}

bool USBStorageClient::send_data_(const uint8_t *buf, uint16_t len) {
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  return this->transfer_out(this->bulk_out_ep_, cb, buf, len) && this->wait_transfer_();
}

bool USBStorageClient::recv_chunked_(uint8_t *buf, uint32_t len) {
  const uint16_t chunk = max_chunk_for_mps_(this->bulk_in_mps_);
  if (chunk == 0)
    return false;

  uint32_t done = 0;
  while (done < len) {
    const uint32_t want = std::min<uint32_t>(chunk, len - done);
    uint8_t *dest = buf + done;
    auto cb = [this, dest, want](const usb_host::TransferStatus &s) {
      if (s.success && s.data_len > 0)
        memcpy(dest, s.data, std::min<size_t>(s.data_len, want));
      transfer_done_cb(s, this);
    };
    if (!this->transfer_in(this->bulk_in_ep_, cb, this->round_up_to_in_mps_(static_cast<uint16_t>(want))))
      return false;
    if (!this->wait_transfer_())
      return false;
    if (this->transfer_len_ < want) {
      // The device ended the data phase early, so nothing useful follows. The caller
      // treats this like any other transfer failure.
      ESP_LOGE(TAG, "Short data-in: wanted %" PRIu32 " got %u at offset %" PRIu32, want, this->transfer_len_, done);
      return false;
    }
    done += want;
  }
  return true;
}

bool USBStorageClient::send_chunked_(const uint8_t *buf, uint32_t len) {
  const uint16_t chunk = max_chunk_for_mps_(this->bulk_out_mps_);
  if (chunk == 0)
    return false;

  uint32_t done = 0;
  while (done < len) {
    // chunk is a whole number of packets, so every piece but the last is full length and
    // the device does not see a short packet mid-phase and end the transfer early.
    const uint32_t want = std::min<uint32_t>(chunk, len - done);
    if (!this->send_data_(buf + done, static_cast<uint16_t>(want)))
      return false;
    done += want;
  }
  return true;
}

// Endpoint halted by a STALL: cleared on the host side, then on the device (CLEAR_FEATURE ENDPOINT_HALT),
// as the IDF's MSC host driver does.
bool USBStorageClient::clear_halt_(uint8_t ep) {
  static constexpr uint16_t FEATURE_ENDPOINT_HALT = 0;
  if (usb_host_endpoint_halt(this->device_handle_, ep) != ESP_OK)
    return false;
  usb_host_endpoint_flush(this->device_handle_, ep);  // nothing queued: refused, nothing to retire
  if (usb_host_endpoint_clear(this->device_handle_, ep) != ESP_OK)
    return false;
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  if (!this->control_transfer(usb_host::USB_DIR_OUT | usb_host::USB_TYPE_STANDARD | usb_host::USB_RECIP_ENDPOINT,
                              USB_B_REQUEST_CLEAR_FEATURE, FEATURE_ENDPOINT_HALT, ep, cb))
    return false;
  return this->wait_transfer_();
}

bool USBStorageClient::recv_csw_(uint32_t expected_tag, bool check_condition_expected) {
  MscCsw csw{};
  auto cb = [this, &csw](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(MscCsw))
      memcpy(&csw, s.data, sizeof(MscCsw));
    transfer_done_cb(s, this);
  };
  this->transfer_stalled_ = false;
  if (!this->transfer_in(this->bulk_in_ep_, cb, this->round_up_to_in_mps_(sizeof(MscCsw))))
    return false;
  if (!this->wait_transfer_()) {
    // A stalled status phase: read the status once more after clearing the halt (BOT 5.3.3)
    if (!this->transfer_stalled_ || !this->clear_halt_(this->bulk_in_ep_))
      return false;
    if (!this->transfer_in(this->bulk_in_ep_, cb, this->round_up_to_in_mps_(sizeof(MscCsw))) ||
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

bool USBStorageClient::scsi_test_unit_ready_() {
  uint8_t cdb[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  uint32_t tag = this->cbw_tag_++;
  return this->send_cbw_(tag, 0, MSC_BOT_CBW_FLAGS_OUT, cdb, sizeof(cdb)) && this->recv_csw_(tag, true);
}

bool USBStorageClient::scsi_request_sense_(uint8_t &key, uint8_t &asc) {
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
  if (!this->transfer_in(this->bulk_in_ep_, cb, this->round_up_to_in_mps_(SENSE_LEN)))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (!this->recv_csw_(tag))
    return false;
  key = resp[2] & 0x0F;
  asc = resp[12];
  return true;
}

bool USBStorageClient::scsi_inquiry_() {
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
  if (!this->transfer_in(this->bulk_in_ep_, cb, this->round_up_to_in_mps_(sizeof(ScsiInquiryResponse))))
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

// ─────────────────────────────────────────────────────────────────────────────
// Public DISKIO interface (called from FATFS context -- blocking)
// ─────────────────────────────────────────────────────────────────────────────

// Data phase on the client's own two transfers (see DataXfer): submitted ahead, completed in
// order, so the next piece is already on the bus while the last one is copied.
void USBStorageClient::data_done_cb(usb_transfer_t *xfer) { xSemaphoreGive(static_cast<DataXfer *>(xfer->context)->done); }

// Bytes per data transfer: whole packets, at most 127 of them per transfer (the DWC controller's
// limit, see usb_host_transfer_alloc() in usb_host.h), at most the transfer's buffer.
uint32_t USBStorageClient::data_piece_(uint16_t mps) const {
  if (mps == 0)
    return 0;
  uint32_t piece = std::min<uint32_t>(DATA_XFER_SIZE, 127u * mps);
  return piece - piece % mps;
}

bool USBStorageClient::data_submit_(DataXfer &d, uint8_t ep, uint32_t len) {
  usb_transfer_t *t = d.xfer;
  xSemaphoreTake(d.done, 0);
  t->device_handle = this->device_handle_;
  t->bEndpointAddress = ep;
  t->num_bytes = static_cast<int>(len);
  t->flags = 0;
  if (usb_host_transfer_submit(t) != ESP_OK)
    return false;
  d.busy = true;
  return true;
}

// Retires what is still on the bus: a halted and flushed endpoint hands its transfers back.
void USBStorageClient::data_cancel_(uint8_t ep) {
  bool busy = false;
  for (auto &d : this->data_)
    busy |= d.busy;
  if (!busy)
    return;
  usb_host_endpoint_halt(this->device_handle_, ep);
  usb_host_endpoint_flush(this->device_handle_, ep);
  for (auto &d : this->data_) {
    if (d.busy) {
      xSemaphoreTake(d.done, portMAX_DELAY);
      d.busy = false;
    }
  }
  usb_host_endpoint_clear(this->device_handle_, ep);
}

USBStorageClient::DataResult USBStorageClient::data_phase_(bool in, uint8_t *in_buf, const uint8_t *out_buf,
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
bool USBStorageClient::reset_recovery_() {
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb(s, this); };
  const bool ok =
      this->control_transfer(usb_host::USB_DIR_OUT | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE,
                             MSC_REQUEST_RESET, 0, this->msc_interface_, cb) &&
      this->wait_transfer_();
  this->clear_halt_(this->bulk_in_ep_);
  this->clear_halt_(this->bulk_out_ep_);
  return ok;
}

bool USBStorageClient::data_command_(bool in, uint8_t *in_buf, const uint8_t *out_buf, uint32_t len, uint32_t tag) {
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
bool USBStorageClient::alloc_data_() {
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
void USBStorageClient::free_data_() {
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  for (auto &d : this->data_) {
    if (d.xfer != nullptr)
      usb_host_transfer_free(d.xfer);
    d.xfer = nullptr;
    d.busy = false;
  }
  xSemaphoreGive(this->io_lock_);
}

bool USBStorageClient::scsi_read(uint8_t index, uint32_t lba, uint8_t *buf, uint32_t count) {
  if (index >= this->luns_.size())
    return false;
  const Lun &lun = this->luns_[index];
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  const bool ok = this->data_[0].xfer != nullptr && lun.ready &&
                  this->scsi_read_(lun.number, lba, buf, count, lun.sector_size);
  xSemaphoreGive(this->io_lock_);
  return ok;
}

bool USBStorageClient::scsi_write(uint8_t index, uint32_t lba, const uint8_t *buf, uint32_t count) {
  if (index >= this->luns_.size())
    return false;
  const Lun &lun = this->luns_[index];
  xSemaphoreTake(this->io_lock_, portMAX_DELAY);
  const bool ok = this->data_[0].xfer != nullptr && lun.ready &&
                  this->scsi_write_(lun.number, lba, buf, count, lun.sector_size);
  xSemaphoreGive(this->io_lock_);
  return ok;
}

// One READ(10) / WRITE(10) for all sectors FATFS asks for at once (up to 65535 per command).
bool USBStorageClient::scsi_read_(uint8_t lun, uint32_t lba, uint8_t *buf, uint32_t count, uint32_t sector_size) {
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
    const uint32_t bytes = n * sector_size;
    const uint32_t tag = this->cbw_tag_++;
    if (!this->send_cbw_(tag, bytes, MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb), lun) ||
        !this->data_command_(true, buf, nullptr, bytes, tag))
      return false;
    lba += n;
    buf += bytes;
    count -= n;
  }
  return true;
}

bool USBStorageClient::scsi_write_(uint8_t lun, uint32_t lba, const uint8_t *buf, uint32_t count,
                                   uint32_t sector_size) {
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
    const uint32_t bytes = n * sector_size;
    const uint32_t tag = this->cbw_tag_++;
    if (!this->send_cbw_(tag, bytes, MSC_BOT_CBW_FLAGS_OUT, cdb, sizeof(cdb), lun) ||
        !this->data_command_(false, nullptr, buf, bytes, tag))
      return false;
    lba += n;
    buf += bytes;
    count -= n;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageClient::on_connected() {
  const usb_device_desc_t *dev = this->get_device_desc_();
  uint16_t vid = dev ? dev->idVendor : 0;
  uint16_t pid = dev ? dev->idProduct : 0;

  ESP_LOGI(TAG, "MSC device connected VID=0x%04X PID=0x%04X", vid, pid);
  this->connection_++;
  if (!this->alloc_data_()) {
    this->disconnect();
    return;
  }

  if (!this->parse_msc_endpoints_()) {
    this->disconnect();
    return;
  }

  if (!this->claim_interface(this->msc_interface_, 0)) {
    ESP_LOGE(TAG, "Failed to claim MSC interface %d", this->msc_interface_);
    this->disconnect();
    return;
  }

  if (!this->scsi_inquiry_()) {
    ESP_LOGE(TAG, "SCSI INQUIRY failed");
    this->disconnect();
    return;
  }

  if (!this->get_max_lun_(this->max_lun_))
    this->max_lun_ = 0;
  for (const auto &lun : this->luns_) {
    if (lun.number > this->max_lun_)
      ESP_LOGW(TAG, "LUN %u configured, the device has LUNs 0..%u", lun.number, this->max_lun_);
  }
  this->report_connected_();
  // Every slot looked at now; a medium coming up after power-on is waited for (async_done_()).
  this->check_card();
}

// GET MAX LUN (BOT 3.2); a device with one LUN may stall it.
bool USBStorageClient::get_max_lun_(uint8_t &max_lun) {
  const uint16_t mps0 = this->get_device_desc_() != nullptr ? this->get_device_desc_()->bMaxPacketSize0 : 64;
  uint8_t value = 0;
  auto cb = [this, &value](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= 1)
      value = s.data[0];
    transfer_done_cb(s, this);
  };
  if (!this->control_transfer(usb_host::USB_DIR_IN | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE,
                              MSC_REQUEST_GET_MAX_LUN, 0, this->msc_interface_, cb, std::vector<uint8_t>(mps0), 1))
    return false;
  if (!this->wait_transfer_())
    return false;
  max_lun = value;
  return true;
}

// -- Medium check --------------------------------------------------------------------------

void USBStorageClient::check_card() {
  for (auto &lun : this->luns_)
    lun.ejected = false;
  this->start_check_();
}

// One round over the slots; an ejected slot stays unmounted.
void USBStorageClient::start_check_() {
  if (!this->is_connected() || this->luns_.empty())
    return;
  if (this->checking_) {
    this->check_again_ = true;
    return;
  }
  this->cancel_timeout("card_check");
  this->checking_ = true;
  this->check_again_ = false;
  this->check_index_ = 0;
  this->luns_[0].tries = 0;
  this->check_lun_(0);
}

void USBStorageClient::check_lun_(uint8_t index) {
  Lun &lun = this->luns_[index];
  if (lun.number > this->max_lun_) {
    this->next_lun_();
    return;
  }
  const uint8_t cdb[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  // The storage worker has the pipes: the medium is in use, so it is there.
  if (!this->async_command_(Step::TUR, cdb, sizeof(cdb), 0))
    this->next_lun_();
}

void USBStorageClient::next_lun_() {
  if (++this->check_index_ < this->luns_.size()) {
    this->luns_[this->check_index_].tries = 0;
    this->check_lun_(this->check_index_);
    return;
  }
  this->checking_ = false;
  if (this->check_again_) {
    this->check_again_ = false;
    this->start_check_();
    return;
  }
  if (this->card_check_interval_ != SCHEDULER_DONT_RUN)
    this->set_timeout("card_check", this->card_check_interval_, [this]() { this->start_check_(); });
}

bool USBStorageClient::async_command_(Step step, const uint8_t *cdb, uint8_t cdb_len, uint8_t data_len) {
  if (xSemaphoreTake(this->io_lock_, 0) != pdTRUE)
    return false;
  MscCbw cbw{};
  this->async_step_ = step;
  this->async_tag_ = this->cbw_tag_++;
  this->async_len_ = data_len;
  memset(this->async_buf_, 0, sizeof(this->async_buf_));
  cbw.tag = this->async_tag_;
  cbw.data_transfer_length = data_len;
  cbw.flags = data_len != 0 ? MSC_BOT_CBW_FLAGS_IN : MSC_BOT_CBW_FLAGS_OUT;
  cbw.lun = this->luns_[this->check_index_].number;
  cbw.cb_length = cdb_len;
  memcpy(cbw.cb, cdb, cdb_len);
  const bool sent = this->transfer_out(
      this->bulk_out_ep_,
      [this](const usb_host::TransferStatus &s) {
        if (!s.success) {
          this->async_finish_(0xFF);
          return;
        }
        if (this->async_len_ == 0) {
          this->async_csw_();
          return;
        }
        const bool reading = this->transfer_in(
            this->bulk_in_ep_,
            [this](const usb_host::TransferStatus &d) {
              if (!d.success) {
                this->async_finish_(0xFF);
                return;
              }
              memcpy(this->async_buf_, d.data, std::min<size_t>(d.data_len, sizeof(this->async_buf_)));
              this->async_csw_();
            },
            this->round_up_to_in_mps_(this->async_len_));
        if (!reading)
          this->async_finish_(0xFF);
      },
      reinterpret_cast<const uint8_t *>(&cbw), sizeof(MscCbw));
  if (!sent) {
    xSemaphoreGive(this->io_lock_);
    this->set_timeout("lun_retry", 0, [this]() { this->async_done_(this->async_step_, 0xFF); });
  }
  return true;
}

// USB task.
void USBStorageClient::async_csw_() {
  const bool reading = this->transfer_in(
      this->bulk_in_ep_,
      [this](const usb_host::TransferStatus &s) {
        MscCsw csw{};
        if (s.success && s.data_len >= sizeof(MscCsw))
          memcpy(&csw, s.data, sizeof(MscCsw));
        const bool valid = s.success && csw.signature == MSC_BOT_CSW_SIGNATURE && csw.tag == this->async_tag_;
        this->async_finish_(valid ? csw.status : 0xFF);
      },
      this->round_up_to_in_mps_(sizeof(MscCsw)));
  if (!reading)
    this->async_finish_(0xFF);
}

// USB task: the pipes are free again; the answer goes to the main loop. 0xFF: the transport failed.
void USBStorageClient::async_finish_(uint8_t status) {
  xSemaphoreGive(this->io_lock_);
  const uint32_t connection = this->connection_;
  const Step step = this->async_step_;
  this->defer([this, connection, step, status]() {
    if (connection == this->connection_)
      this->async_done_(step, status);
  });
}

void USBStorageClient::async_done_(Step step, uint8_t status) {
  static constexpr uint8_t SENSE_NO_SENSE = 0x0;
  static constexpr uint8_t SENSE_NOT_READY = 0x2;
  static constexpr uint8_t SENSE_UNIT_ATTENTION = 0x6;
  static constexpr uint8_t ASC_BECOMING_READY = 0x04;
  static constexpr uint8_t ASC_MEDIUM_CHANGED = 0x28;
  static constexpr uint8_t ASC_MEDIUM_NOT_PRESENT = 0x3A;
  static constexpr uint32_t RETRY_MS = 100;
  static constexpr uint8_t RETRIES = 50;

  const uint8_t index = this->check_index_;
  Lun &lun = this->luns_[index];
  if (status == 0xFF || status == MSC_BOT_CSW_STATUS_PHASE_ERROR) {
    this->async_reset_();
    this->next_lun_();
    return;
  }
  switch (step) {
    case Step::TUR: {
      if (status == MSC_BOT_CSW_STATUS_GOOD) {
        if (lun.mounted || lun.ejected) {
          this->next_lun_();
          return;
        }
        const uint8_t cdb[10] = {SCSI_CMD_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (!this->async_command_(Step::CAPACITY, cdb, sizeof(cdb), sizeof(ScsiReadCapacity10Response)))
          this->next_lun_();
        return;
      }
      // CHECK CONDITION: the reason is held until REQUEST SENSE has read it.
      const uint8_t cdb[6] = {SCSI_CMD_REQUEST_SENSE, 0, 0, 0, 18, 0};
      if (!this->async_command_(Step::SENSE, cdb, sizeof(cdb), 18))
        this->next_lun_();
      return;
    }
    case Step::SENSE: {
      const uint8_t key = this->async_buf_[2] & 0x0F;
      const uint8_t asc = this->async_buf_[12];
      if (status != MSC_BOT_CSW_STATUS_GOOD || asc == ASC_MEDIUM_NOT_PRESENT) {
        if (lun.mounted) {
          ESP_LOGI(TAG, "Medium removed from '%s'", lun.mount_path);
          this->unmount_lun_(index);
        }
        lun.ejected = false;
        this->next_lun_();
        return;
      }
      if (asc == ASC_MEDIUM_CHANGED && lun.mounted) {
        ESP_LOGI(TAG, "Medium changed in '%s'", lun.mount_path);
        this->unmount_lun_(index);
      }
      // A medium coming up (after power-on, after it was inserted): asked again shortly.
      const bool coming = key == SENSE_UNIT_ATTENTION || key == SENSE_NO_SENSE ||
                          (key == SENSE_NOT_READY && asc == ASC_BECOMING_READY);
      if (coming && ++lun.tries < RETRIES) {
        this->set_timeout("lun_retry", RETRY_MS, [this, index]() { this->check_lun_(index); });
        return;
      }
      if (coming)
        ESP_LOGW(TAG, "LUN %u not ready after %" PRIu32 " ms (sense key 0x%X, ASC 0x%02X)", lun.number,
                 RETRY_MS * RETRIES, key, asc);
      this->next_lun_();
      return;
    }
    case Step::CAPACITY: {
      ScsiReadCapacity10Response resp{};
      memcpy(&resp, this->async_buf_, sizeof(resp));
      if (status == MSC_BOT_CSW_STATUS_GOOD) {
        lun.sector_count = __builtin_bswap32(resp.last_lba) + 1;
        lun.sector_size = __builtin_bswap32(resp.block_size);
        ESP_LOGD(TAG, "LUN %u: %" PRIu32 " sectors of %" PRIu32 " bytes", lun.number, lun.sector_count,
                 lun.sector_size);
        if (lun.sector_size != 0 && lun.sector_count != 0)
          this->mount_lun_(index);
      }
      this->next_lun_();
      return;
    }
  }
}

// BOT 5.3.4 reset recovery without waiting: Bulk-Only Mass Storage Reset, then both bulk
// endpoints cleared, each request answered in its callback.
void USBStorageClient::async_reset_() {
  if (xSemaphoreTake(this->io_lock_, 0) != pdTRUE)
    return;
  static constexpr uint16_t FEATURE_ENDPOINT_HALT = 0;
  for (const uint8_t ep : {this->bulk_in_ep_, this->bulk_out_ep_}) {
    if (usb_host_endpoint_halt(this->device_handle_, ep) == ESP_OK) {
      usb_host_endpoint_flush(this->device_handle_, ep);
      usb_host_endpoint_clear(this->device_handle_, ep);
    }
  }
  const uint8_t in_ep = this->bulk_in_ep_;
  const uint8_t out_ep = this->bulk_out_ep_;
  static constexpr uint8_t ENDPOINT = usb_host::USB_DIR_OUT | usb_host::USB_TYPE_STANDARD | usb_host::USB_RECIP_ENDPOINT;
  const bool sent = this->control_transfer(
      usb_host::USB_DIR_OUT | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE, MSC_REQUEST_RESET, 0,
      this->msc_interface_, [this, in_ep, out_ep](const usb_host::TransferStatus &) {
        const bool in_sent = this->control_transfer(
            ENDPOINT, USB_B_REQUEST_CLEAR_FEATURE, FEATURE_ENDPOINT_HALT, in_ep,
            [this, out_ep](const usb_host::TransferStatus &) {
              const bool out_sent =
                  this->control_transfer(ENDPOINT, USB_B_REQUEST_CLEAR_FEATURE, FEATURE_ENDPOINT_HALT, out_ep,
                                         [this](const usb_host::TransferStatus &) { xSemaphoreGive(this->io_lock_); });
              if (!out_sent)
                xSemaphoreGive(this->io_lock_);
            });
        if (!in_sent)
          xSemaphoreGive(this->io_lock_);
      });
  if (!sent)
    xSemaphoreGive(this->io_lock_);
}

// Main loop: FATFS mounts the slot's volume, reading its first sectors through the storage
// I/O path (scsi_read()).
bool USBStorageClient::mount_lun_(uint8_t index) {
  Lun &lun = this->luns_[index];
  char drive_path[8];
  snprintf(drive_path, sizeof(drive_path), "%d:", lun.drive);
  lun.ready = true;

#ifdef USE_STORAGE_FILE_SYSTEM_SELECT
  // Before the one and only f_mount below: probe the first sectors and, if a manually
  // requested filesystem mismatches what is on the medium, reformat first -- the mount then
  // happens on the correct filesystem from the start.
  if (!storage::ensure_requested_filesystem(TAG, lun.drive, drive_path, this->requested_file_system_,
                                            this->format_on_mismatch_)) {
    lun.ready = false;
    return false;
  }
#endif

  FATFS *fs = nullptr;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  esp_vfs_fat_conf_t vfs_conf = {
      .base_path = lun.mount_path,
      .fat_drive = drive_path,
      .max_files = 5,
  };
  esp_err_t err = esp_vfs_fat_register_cfg(&vfs_conf, &fs);
#else
  esp_err_t err = esp_vfs_fat_register(lun.mount_path, drive_path, 5, &fs);
#endif
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(err));
    lun.ready = false;
    return false;
  }

  FRESULT res = f_mount(fs, drive_path, 1);
  if (res != FR_OK) {
    ESP_LOGE(TAG, "f_mount failed: %d", res);
    esp_vfs_fat_unregister_path(lun.mount_path);
    lun.ready = false;
    return false;
  }

  lun.mounted = true;
#ifdef USE_STORAGE_CHANGE_FEED
  // Mount state is part of the roots listing: whoever flipped it (CD pin, hotplug, HTTP,
  // automation), the browser's change poll must see it -- including recovery after an error.
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->note_dir_changed("");
#endif

  ESP_LOGI(TAG, "FAT filesystem mounted at '%s'", lun.mount_path);
  if (lun.device != nullptr) {
    lun.device->on_device_connected(lun.mount_path, lun.drive);
  } else if (index == 0) {
    ESP_LOGW(TAG, "No storage device configured for this stick, mounted at '%s' only in the VFS", lun.mount_path);
  }
  return true;
}

void USBStorageClient::unmount_lun_(uint8_t index) {
  Lun &lun = this->luns_[index];
  if (lun.device != nullptr && lun.device->is_mounted())
    lun.device->on_device_disconnected();
  lun.ready = false;
  if (!lun.mounted)
    return;
  char drive_path[8];
  snprintf(drive_path, sizeof(drive_path), "%d:", lun.drive);
  f_mount(nullptr, drive_path, 0);
  esp_vfs_fat_unregister_path(lun.mount_path);
  lun.mounted = false;
#ifdef USE_STORAGE_CHANGE_FEED
  // Mount state is part of the roots listing: whoever flipped it (CD pin, hotplug, HTTP,
  // automation), the browser's change poll must see it -- including recovery after an error.
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->note_dir_changed("");
#endif

  ESP_LOGI(TAG, "FAT filesystem unmounted from '%s'", lun.mount_path);
}

void USBStorageClient::unmount_filesystem(uint8_t index) {
  if (index >= this->luns_.size())
    return;
  this->unmount_lun_(index);
  this->luns_[index].ejected = true;
}

void USBStorageClient::on_disconnected() {
  this->connection_++;
  // Transfers still out on the bulk endpoints come back now, so a SCSI command of the storage
  // worker or the medium check ends at once instead of on its timeouts (free_data_() waits for it).
  if (this->device_handle_ != nullptr) {
    for (const uint8_t ep : {this->bulk_in_ep_, this->bulk_out_ep_}) {
      if (ep != 0 && usb_host_endpoint_halt(this->device_handle_, ep) == ESP_OK)
        usb_host_endpoint_flush(this->device_handle_, ep);
    }
  }
  this->cancel_timeout("card_check");
  this->cancel_timeout("lun_retry");
  this->checking_ = false;
  this->check_again_ = false;
  for (size_t i = 0; i < this->luns_.size(); i++) {
    this->unmount_lun_(static_cast<uint8_t>(i));
    this->luns_[i].sector_count = 0;
    this->luns_[i].ejected = false;
  }

  if (this->msc_interface_ != 0xFF) {
    this->release_interface(this->msc_interface_);
    this->msc_interface_ = 0xFF;
  }

  this->bulk_in_ep_ = 0;
  this->bulk_out_ep_ = 0;
  this->bulk_in_mps_ = 0;
  this->bulk_out_mps_ = 0;
  this->free_data_();
}

void USBStorageClient::on_removed(usb_device_handle_t handle) { USBClient::on_removed(handle); }

// A mount naming the stick makes it this instance's; a mount without vid/pid takes any stick.
usb_host::MatchTier USBStorageClient::refine_match_(const usb_host::RoutedDevice &dev, usb_host::MatchTier tier) const {
  if (this->devices_.empty())
    return tier;
  bool any = false;
  for (const auto *device : this->devices_) {
    if (device->vid_ == 0 && device->pid_ == 0) {
      any = true;
    } else if (device->vid_ == dev.desc->idVendor && device->pid_ == dev.desc->idProduct) {
      return usb_host::MatchTier::NAMED;
    }
  }
  return any ? tier : usb_host::MatchTier::NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageDevice -- lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageDevice::setup() { ESP_LOGCONFIG(TAG, "Setting up USB Storage Device"); }

void USBStorageDevice::dump_config() {
  ESP_LOGCONFIG(TAG, "USB Storage Device:");
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_ ? this->mount_path_ : "(none)");
  ESP_LOGCONFIG(TAG, "  VID: 0x%04X  PID: 0x%04X", this->vid_, this->pid_);
}

void USBStorageDevice::on_device_connected(const char *mount_path, int drive) {
  ESP_LOGI(TAG, "USB Storage Device connected (mount_path='%s')", mount_path);
  this->mount_path_ = mount_path;
  this->fs_mounted_ = true;
  snprintf(this->fatfs_drive_, sizeof(this->fatfs_drive_), "%d:", drive);

  this->on_mounted_.call(mount_path);

  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->register_storage(this);
}

void USBStorageDevice::on_device_disconnected() {
  ESP_LOGI(TAG, "USB Storage Device disconnected (mount_path='%s')", this->mount_path_ ? this->mount_path_ : "(none)");
  this->fs_mounted_ = false;

  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->unregister_storage(this);
}

bool USBStorageDevice::remount_device() {
  if (!this->fs_mounted_) {
    ESP_LOGW(TAG, "Device not mounted, cannot remount");
    return false;
  }
  ESP_LOGI(TAG, "Device remounted successfully");
  this->on_mounted_.call(this->mount_path_);
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
  this->on_device_disconnected();
  if (this->client_ != nullptr)
    this->client_->unmount_filesystem(this->lun_index_);
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
    written = snprintf(out, out_size, "%s%s", this->fatfs_drive_, rel_path);
  } else {
    written = snprintf(out, out_size, "%s/%s", this->fatfs_drive_, rel_path);
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
  info->block_size = this->client_->get_sector_size(this->lun_index_);
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
  if (this->unmount_pending_)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  // Request the unmount; loop() completes it once the device is quiescent (no open handle,
  // no worker job referencing this device). Typical USB "safe eject" behaviour: in-flight
  // transfers finish, then we sync and unmount. Returns OK = unmount accepted/initiated.
  this->unmount_pending_ = true;
  ESP_LOGD(TAG, "Unmount requested; will complete once active transfers finish");
  return storage::StorageError::STORAGE_ERROR_OK;
}

void USBStorageDevice::loop() {
  if (!this->unmount_pending_)
    return;
  // Wait until nothing references the device: no open file handle, and no worker job whose
  // src/dst is this device. The worker keeps a handle open for a transfer's whole duration,
  // so has_open_handles_() already covers active copies; the extra registry check guards the
  // brief open()/close() edges where a job exists but its handle is momentarily not counted.
  if (this->has_open_handles_())
    return;
#ifdef USE_STORAGE_WORKER
  if (storage::global_storage_worker != nullptr && storage::global_storage_worker->is_busy_with(this))
    return;
#endif
  // Quiescent -- sync then physically unmount.
  this->sync();
  this->unmount_pending_ = false;
  this->unmount_device();
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
