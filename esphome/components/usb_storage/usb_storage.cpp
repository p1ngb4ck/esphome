#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32P4)

#include "usb_storage.h"
#include "usb_storage_diskio.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_ch9.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

#include <cstring>
#include <cinttypes>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

namespace esphome {
namespace usb_storage {

// ─────────────────────────────────────────────────────────────────────────────
// Transfer semaphore helpers
// ─────────────────────────────────────────────────────────────────────────────

// USB-task callback: post result to semaphore so the blocking caller can proceed
void USBStorageClient::transfer_done_cb_(const usb_host::TransferStatus &status, USBStorageClient *client) {
  client->transfer_ok_ = status.success;
  client->transfer_len_ = static_cast<uint16_t>(status.data_len);
  xSemaphoreGive(client->transfer_sem_);
}

bool USBStorageClient::wait_transfer_(uint32_t timeout_ms) {
  return xSemaphoreTake(this->transfer_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && this->transfer_ok_;
}

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageClient — setup / loop
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageClient::setup() {
  this->transfer_sem_ = xSemaphoreCreateBinary();
  if (this->transfer_sem_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create transfer semaphore");
    this->mark_failed();
    return;
  }

  this->fatfs_drive_ = usb_diskio_register(this);
  if (this->fatfs_drive_ < 0) {
    ESP_LOGE(TAG, "Failed to register FATFS DISKIO drive");
    this->mark_failed();
    return;
  }

  // Call USBClient::setup() to register as USB host client and start USB task
  USBClient::setup();
}

void USBStorageClient::loop() {
  // USBClient::loop() processes the event queue; disable loop when idle
  USBClient::loop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Endpoint parsing
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageClient::parse_msc_endpoints_() {
  const usb_config_desc_t *cfg = this->get_config_desc();
  if (cfg == nullptr)
    return false;

  this->bulk_in_ep_ = 0;
  this->bulk_out_ep_ = 0;

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
      // Only bulk endpoints
      if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
        continue;
      if (USB_EP_DESC_GET_EP_DIR(ep)) {
        this->bulk_in_ep_ = ep->bEndpointAddress;
      } else {
        this->bulk_out_ep_ = ep->bEndpointAddress;
      }
    }
    if (this->bulk_in_ep_ && this->bulk_out_ep_)
      break;
  }

  if (!this->bulk_in_ep_ || !this->bulk_out_ep_) {
    ESP_LOGE(TAG, "Failed to find MSC bulk endpoints (in=0x%02X out=0x%02X)", this->bulk_in_ep_,
             this->bulk_out_ep_);
    return false;
  }
  ESP_LOGD(TAG, "MSC endpoints: bulk_in=0x%02X bulk_out=0x%02X intf=%d", this->bulk_in_ep_, this->bulk_out_ep_,
           this->msc_interface_);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BOT primitives
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageClient::send_cbw_(uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t *cdb,
                                 uint8_t cdb_len) {
  MscCbw cbw{};
  cbw.tag = tag;
  cbw.data_transfer_length = data_len;
  cbw.flags = flags;
  cbw.lun = 0;
  cbw.cb_length = cdb_len;
  memcpy(cbw.cb, cdb, cdb_len);

  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb_(s, this); };
  if (!this->transfer_out(this->bulk_out_ep_, cb, reinterpret_cast<const uint8_t *>(&cbw), sizeof(MscCbw)))
    return false;
  return this->wait_transfer_();
}

bool USBStorageClient::recv_data_(uint8_t *buf, uint16_t len) {
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb_(s, this); };
  if (!this->transfer_in(this->bulk_in_ep_, cb, len))
    return false;
  if (!this->wait_transfer_())
    return false;
  // Copy from the transfer buffer — USBClient stores received data in the transfer buffer
  // which is accessible via the TransferStatus::data pointer captured in the callback.
  // Since we need the data here, we use a stored pointer set by our callback.
  return true;
}

bool USBStorageClient::send_data_(const uint8_t *buf, uint16_t len) {
  auto cb = [this](const usb_host::TransferStatus &s) { transfer_done_cb_(s, this); };
  return this->transfer_out(this->bulk_out_ep_, cb, buf, len) && this->wait_transfer_();
}

bool USBStorageClient::recv_csw_(uint32_t expected_tag) {
  MscCsw csw{};
  auto cb = [this, &csw](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(MscCsw))
      memcpy(&csw, s.data, sizeof(MscCsw));
    transfer_done_cb_(s, this);
  };
  if (!this->transfer_in(this->bulk_in_ep_, cb, sizeof(MscCsw)))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (csw.signature != MSC_BOT_CSW_SIGNATURE) {
    ESP_LOGE(TAG, "Invalid CSW signature: 0x%08" PRIX32, csw.signature);
    return false;
  }
  if (csw.tag != expected_tag) {
    ESP_LOGE(TAG, "CSW tag mismatch: expected %" PRIu32 " got %" PRIu32, expected_tag, csw.tag);
    return false;
  }
  if (csw.status != MSC_BOT_CSW_STATUS_GOOD) {
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
  return this->send_cbw_(tag, 0, MSC_BOT_CBW_FLAGS_OUT, cdb, sizeof(cdb)) && this->recv_csw_(tag);
}

bool USBStorageClient::scsi_inquiry_() {
  uint8_t cdb[6] = {SCSI_CMD_INQUIRY, 0, 0, 0, sizeof(ScsiInquiryResponse), 0};
  uint32_t tag = this->cbw_tag_++;
  ScsiInquiryResponse resp{};

  auto cb = [this, &resp](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(ScsiInquiryResponse))
      memcpy(&resp, s.data, sizeof(ScsiInquiryResponse));
    transfer_done_cb_(s, this);
  };

  if (!this->send_cbw_(tag, sizeof(ScsiInquiryResponse), MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)))
    return false;
  if (!this->transfer_in(this->bulk_in_ep_, cb, sizeof(ScsiInquiryResponse)))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (!this->recv_csw_(tag))
    return false;

  char vendor[9]{}, product[17]{};
  memcpy(vendor, resp.vendor_id, 8);
  memcpy(product, resp.product_id, 16);
  ESP_LOGI(TAG, "SCSI INQUIRY: vendor='%s' product='%s' type=0x%02X", vendor, product,
           resp.peripheral_qualifier_type & 0x1F);
  return (resp.peripheral_qualifier_type & 0x1F) == 0x00;  // direct-access block device
}

bool USBStorageClient::scsi_read_capacity_() {
  uint8_t cdb[10] = {SCSI_CMD_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint32_t tag = this->cbw_tag_++;
  ScsiReadCapacity10Response resp{};

  auto cb = [this, &resp](const usb_host::TransferStatus &s) {
    if (s.success && s.data_len >= sizeof(ScsiReadCapacity10Response))
      memcpy(&resp, s.data, sizeof(ScsiReadCapacity10Response));
    transfer_done_cb_(s, this);
  };

  if (!this->send_cbw_(tag, sizeof(ScsiReadCapacity10Response), MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)))
    return false;
  if (!this->transfer_in(this->bulk_in_ep_, cb, sizeof(ScsiReadCapacity10Response)))
    return false;
  if (!this->wait_transfer_())
    return false;
  if (!this->recv_csw_(tag))
    return false;

  // Fields are big-endian
  this->sector_count_ = __builtin_bswap32(resp.last_lba) + 1;
  this->sector_size_ = __builtin_bswap32(resp.block_size);
  ESP_LOGI(TAG, "SCSI READ CAPACITY: sectors=%" PRIu32 " sector_size=%" PRIu32 " (%.1f MB)", this->sector_count_,
           this->sector_size_,
           static_cast<float>(this->sector_count_) * this->sector_size_ / (1024.0f * 1024.0f));
  return this->sector_size_ > 0 && this->sector_count_ > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public DISKIO interface (called from FATFS context — blocking)
// ─────────────────────────────────────────────────────────────────────────────

bool USBStorageClient::scsi_read(uint32_t lba, uint8_t *buf, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint8_t cdb[10] = {
        SCSI_CMD_READ_10,
        0,
        static_cast<uint8_t>((lba + i) >> 24),
        static_cast<uint8_t>((lba + i) >> 16),
        static_cast<uint8_t>((lba + i) >> 8),
        static_cast<uint8_t>((lba + i) & 0xFF),
        0,
        0,
        1,  // transfer length = 1 sector
        0,
    };
    uint32_t tag = this->cbw_tag_++;
    uint8_t *sector_buf = buf + i * this->sector_size_;

    auto cb = [this, sector_buf, sz = this->sector_size_](const usb_host::TransferStatus &s) {
      if (s.success && s.data_len >= sz)
        memcpy(sector_buf, s.data, sz);
      transfer_done_cb_(s, this);
    };

    if (!this->send_cbw_(tag, this->sector_size_, MSC_BOT_CBW_FLAGS_IN, cdb, sizeof(cdb)))
      return false;
    if (!this->transfer_in(this->bulk_in_ep_, cb, static_cast<uint16_t>(this->sector_size_)))
      return false;
    if (!this->wait_transfer_())
      return false;
    if (!this->recv_csw_(tag))
      return false;
  }
  return true;
}

bool USBStorageClient::scsi_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    uint8_t cdb[10] = {
        SCSI_CMD_WRITE_10,
        0,
        static_cast<uint8_t>((lba + i) >> 24),
        static_cast<uint8_t>((lba + i) >> 16),
        static_cast<uint8_t>((lba + i) >> 8),
        static_cast<uint8_t>((lba + i) & 0xFF),
        0,
        0,
        1,  // transfer length = 1 sector
        0,
    };
    uint32_t tag = this->cbw_tag_++;
    const uint8_t *sector_buf = buf + i * this->sector_size_;

    if (!this->send_cbw_(tag, this->sector_size_, MSC_BOT_CBW_FLAGS_OUT, cdb, sizeof(cdb)))
      return false;
    if (!this->send_data_(sector_buf, static_cast<uint16_t>(this->sector_size_)))
      return false;
    if (!this->recv_csw_(tag))
      return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageClient::notify_connected_(uint16_t vid, uint16_t pid) {
  for (auto *device : this->devices_) {
    if ((device->vid_ == 0 && device->pid_ == 0) || (device->vid_ == vid && device->pid_ == pid)) {
      if (!device->is_mounted()) {
        device->on_device_connected(this->mount_path_);
        return;
      }
    }
  }
  ESP_LOGW(TAG, "No storage device configured for VID=0x%04X PID=0x%04X", vid, pid);
}

void USBStorageClient::notify_disconnected_() {
  for (auto *device : this->devices_) {
    if (device->is_mounted()) {
      device->on_device_disconnected();
    }
  }
}

void USBStorageClient::on_connected() {
  const usb_device_desc_t *dev = this->get_device_desc();
  uint16_t vid = dev ? dev->idVendor : 0;
  uint16_t pid = dev ? dev->idProduct : 0;

  ESP_LOGI(TAG, "MSC device connected VID=0x%04X PID=0x%04X", vid, pid);

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

  if (!this->scsi_read_capacity_()) {
    ESP_LOGE(TAG, "SCSI READ CAPACITY failed");
    this->disconnect();
    return;
  }

  this->disk_ready_ = true;

  // Build a mount path from the first matching device, or use a default
  this->mount_path_ = "/usb";
  for (auto *device : this->devices_) {
    if ((device->vid_ == 0 && device->pid_ == 0) || (device->vid_ == vid && device->pid_ == pid)) {
      this->mount_path_ = device->mount_path_;
      break;
    }
  }

  // Mount FAT filesystem via esp_vfs_fat using our DISKIO driver
  char drive_path[8];
  snprintf(drive_path, sizeof(drive_path), "%d:", this->fatfs_drive_);

  FATFS *fs = nullptr;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  esp_vfs_fat_conf_t vfs_conf = {
      .base_path = this->mount_path_.c_str(),
      .fat_drive = drive_path,
      .max_files = 5,
  };
  esp_err_t err = esp_vfs_fat_register_cfg(&vfs_conf, &fs);
#else
  esp_err_t err = esp_vfs_fat_register(this->mount_path_.c_str(), drive_path, 5, &fs);
#endif
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(err));
    this->disk_ready_ = false;
    this->disconnect();
    return;
  }

  FRESULT res = f_mount(fs, drive_path, 1);
  if (res != FR_OK) {
    ESP_LOGE(TAG, "f_mount failed: %d", res);
    esp_vfs_fat_unregister_path(this->mount_path_.c_str());
    this->disk_ready_ = false;
    this->disconnect();
    return;
  }

  this->mounted_ = true;
  ESP_LOGI(TAG, "FAT filesystem mounted at '%s'", this->mount_path_.c_str());

  this->notify_connected_(vid, pid);
}

void USBStorageClient::on_disconnected() {
  this->notify_disconnected_();
  this->disk_ready_ = false;

  if (this->mounted_) {
    char drive_path[8];
    snprintf(drive_path, sizeof(drive_path), "%d:", this->fatfs_drive_);
    f_mount(nullptr, drive_path, 0);
    esp_vfs_fat_unregister_path(this->mount_path_.c_str());
    this->mounted_ = false;
    ESP_LOGI(TAG, "FAT filesystem unmounted from '%s'", this->mount_path_.c_str());
  }

  if (this->msc_interface_ != 0xFF) {
    this->release_interface(this->msc_interface_);
    this->msc_interface_ = 0xFF;
  }

  this->sector_count_ = 0;
  this->bulk_in_ep_ = 0;
  this->bulk_out_ep_ = 0;
}

void USBStorageClient::on_removed(usb_device_handle_t handle) {
  USBClient::on_removed(handle);
}

// ─────────────────────────────────────────────────────────────────────────────
// USBStorageDevice
// ─────────────────────────────────────────────────────────────────────────────

void USBStorageDevice::setup() { ESP_LOGCONFIG(TAG, "Setting up USB Storage Device"); }

void USBStorageDevice::dump_config() {
  ESP_LOGCONFIG(TAG, "USB Storage Device:");
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_.c_str());
  ESP_LOGCONFIG(TAG, "  VID: 0x%04X  PID: 0x%04X", this->vid_, this->pid_);
}

void USBStorageDevice::on_device_connected(const std::string &mount_path) {
  ESP_LOGI(TAG, "USB Storage Device connected (mount_path='%s')", mount_path.c_str());
  this->mount_path_ = mount_path;
  this->mounted_ = true;

  ESP_LOGI(TAG, "Notifying %zu mount ready callbacks", this->mount_ready_callbacks_.size());
  for (const auto &callback : this->mount_ready_callbacks_) {
    callback(this->mount_path_);
  }

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_device(this);
    ESP_LOGD(TAG, "Registered USB storage device with storage registry");
  }
#endif
}

void USBStorageDevice::on_device_disconnected() {
  ESP_LOGI(TAG, "USB Storage Device disconnected (mount_path='%s')", this->mount_path_.c_str());
  this->mounted_ = false;

#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->unregister_device(this);
    ESP_LOGD(TAG, "Unregistered USB storage device from storage registry");
  }
#endif
}

bool USBStorageDevice::remount_device() {
  if (!this->mounted_) {
    ESP_LOGW(TAG, "Device not mounted, cannot remount");
    return false;
  }
  // Re-trigger mount callbacks and storage notification
  for (const auto &callback : this->mount_ready_callbacks_) {
    callback(this->mount_path_);
  }
#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->notify_device_changed(this);
  }
#endif
  return true;
}

void USBStorageDevice::unmount_device() {
  // Physical unmount is managed by USBStorageClient; signal to upper layers only
  if (!this->mounted_)
    return;
  this->on_device_disconnected();
}

void USBStorageDevice::list_files() {
  ESP_LOGI(TAG, "Listing contents of '%s'", this->mount_path_.c_str());
  DIR *dh = opendir(this->mount_path_.c_str());
  if (!dh) {
    ESP_LOGE(TAG, "Failed to open directory: %s", this->mount_path_.c_str());
    return;
  }
  struct dirent *d;
  while ((d = readdir(dh)) != nullptr) {
    ESP_LOGI(TAG, "  %s/%s", this->mount_path_.c_str(), d->d_name);
  }
  closedir(dh);
}

std::string USBStorageDevice::build_full_path(const char *path) {
  std::string full_path = this->mount_path_;
  if (path[0] != '/')
    full_path += "/";
  full_path += path;
  return full_path;
}

#ifdef USE_STORAGE
//========================================================================
// StorageDevice Interface Implementation
//========================================================================

storage::StorageInfo USBStorageDevice::get_info() {
  storage::StorageInfo info;
  info.id = this->id_.empty() ? "usb_storage" : this->id_;
  info.name = "USB Storage";
  info.type = storage::StorageType::USB_MSC;
  info.filesystem = storage::FilesystemType::FAT;
  info.mount_path = this->mount_path_;
  info.is_mounted = this->mounted_;
  info.is_removable = true;
  info.is_read_only = false;
  info.supports_raw_access = false;
  info.supports_filesystem = true;

  if (this->mounted_) {
    uint64_t total, free_bytes;
    if (this->get_space_info(&total, &free_bytes)) {
      info.total_bytes = total;
      info.free_bytes = free_bytes;
      info.block_size = 512;
    } else {
      info.total_bytes = 0;
      info.free_bytes = 0;
      info.block_size = 512;
    }
  } else {
    info.total_bytes = 0;
    info.free_bytes = 0;
    info.block_size = 512;
  }
  return info;
}

bool USBStorageDevice::file_exists(const char *path) {
  if (!this->mounted_)
    return false;
  struct stat path_stat;
  return stat(this->build_full_path(path).c_str(), &path_stat) == 0 && S_ISREG(path_stat.st_mode);
}

bool USBStorageDevice::get_file_size(const char *path, size_t *size) {
  if (!this->mounted_)
    return false;
  struct stat path_stat;
  if (stat(this->build_full_path(path).c_str(), &path_stat) != 0)
    return false;
  *size = path_stat.st_size;
  return true;
}

bool USBStorageDevice::read_file(const char *path, uint8_t *data, size_t *length) {
  if (!this->mounted_)
    return false;
  FILE *f = fopen(this->build_full_path(path).c_str(), "rb");
  if (f == nullptr)
    return false;
  *length = fread(data, 1, *length, f);
  fclose(f);
  return true;
}

bool USBStorageDevice::write_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->mounted_)
    return false;
  FILE *f = fopen(this->build_full_path(path).c_str(), "wb");
  if (f == nullptr)
    return false;
  bool ok = fwrite(data, 1, length, f) == length;
  fclose(f);
  return ok;
}

bool USBStorageDevice::append_file(const char *path, const uint8_t *data, size_t length) {
  if (!this->mounted_)
    return false;
  FILE *f = fopen(this->build_full_path(path).c_str(), "ab");
  if (f == nullptr)
    return false;
  bool ok = fwrite(data, 1, length, f) == length;
  fclose(f);
  return ok;
}

bool USBStorageDevice::delete_file(const char *path) {
  if (!this->mounted_)
    return false;
  return remove(this->build_full_path(path).c_str()) == 0;
}

bool USBStorageDevice::rename_file(const char *old_path, const char *new_path) {
  if (!this->mounted_)
    return false;
  return rename(this->build_full_path(old_path).c_str(), this->build_full_path(new_path).c_str()) == 0;
}

bool USBStorageDevice::copy_file(const char *src_path, const char *dst_path) {
  if (!this->mounted_)
    return false;
  FILE *src = fopen(this->build_full_path(src_path).c_str(), "rb");
  if (src == nullptr)
    return false;
  FILE *dst = fopen(this->build_full_path(dst_path).c_str(), "wb");
  if (dst == nullptr) {
    fclose(src);
    return false;
  }
  uint8_t buf[512];
  size_t n;
  bool ok = true;
  while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
    if (fwrite(buf, 1, n, dst) != n) {
      ok = false;
      break;
    }
  }
  fclose(src);
  fclose(dst);
  return ok;
}

bool USBStorageDevice::dir_exists(const char *path) {
  if (!this->mounted_)
    return false;
  struct stat path_stat;
  return stat(this->build_full_path(path).c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode);
}

bool USBStorageDevice::create_dir(const char *path) {
  if (!this->mounted_)
    return false;
  return mkdir(this->build_full_path(path).c_str(), 0755) == 0;
}

bool USBStorageDevice::delete_dir(const char *path, bool recursive) {
  if (!this->mounted_)
    return false;
  std::string full_path = this->build_full_path(path);
  if (recursive) {
    DIR *dir = opendir(full_path.c_str());
    if (dir == nullptr)
      return false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        continue;
      std::string entry_path = std::string(path) + "/" + entry->d_name;
      if (entry->d_type == DT_DIR) {
        if (!this->delete_dir(entry_path.c_str(), true)) {
          closedir(dir);
          return false;
        }
      } else {
        if (!this->delete_file(entry_path.c_str())) {
          closedir(dir);
          return false;
        }
      }
    }
    closedir(dir);
  }
  return rmdir(full_path.c_str()) == 0;
}

bool USBStorageDevice::list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) {
  if (!this->mounted_)
    return false;
  std::string full_path = this->build_full_path(path);
  DIR *dir = opendir(full_path.c_str());
  if (dir == nullptr)
    return false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    storage::StorageFileInfo info;
    info.name = entry->d_name;
    info.path = std::string(path) + "/" + entry->d_name;
    info.is_directory = entry->d_type == DT_DIR;
    struct stat st;
    if (stat((full_path + "/" + entry->d_name).c_str(), &st) == 0) {
      info.size = info.is_directory ? 0 : st.st_size;
      info.modified_time = st.st_mtime;
    } else {
      info.size = 0;
      info.modified_time = 0;
    }
    entries->push_back(info);
  }
  closedir(dir);
  return true;
}

bool USBStorageDevice::get_space_info(uint64_t *total, uint64_t *free) {
  if (!this->mounted_)
    return false;
  FATFS *fs;
  DWORD fre_clust;
  std::string path = this->mount_path_;
  if (path.back() != '/')
    path += '/';
  if (f_getfree(path.c_str(), &fre_clust, &fs) != FR_OK)
    return false;
  DWORD tot_sect = (fs->n_fatent - 2) * fs->csize;
  DWORD fre_sect = fre_clust * fs->csize;
  *total = static_cast<uint64_t>(tot_sect) * fs->ssize;
  *free = static_cast<uint64_t>(fre_sect) * fs->ssize;
  return true;
}

bool USBStorageDevice::can_write_file(const char *path, size_t size) {
  uint64_t total, free_space;
  return this->mounted_ && this->get_space_info(&total, &free_space) && free_space >= size;
}

void *USBStorageDevice::open_file(const char *path, const char *mode) {
  if (!this->mounted_)
    return nullptr;
  return fopen(this->build_full_path(path).c_str(), mode);
}

size_t USBStorageDevice::read_file_chunk(void *handle, uint8_t *buffer, size_t size) {
  return handle ? fread(buffer, 1, size, static_cast<FILE *>(handle)) : 0;
}

size_t USBStorageDevice::write_file_chunk(void *handle, const uint8_t *data, size_t size) {
  return handle ? fwrite(data, 1, size, static_cast<FILE *>(handle)) : 0;
}

bool USBStorageDevice::seek_file(void *handle, size_t offset) {
  return handle && fseek(static_cast<FILE *>(handle), offset, SEEK_SET) == 0;
}

size_t USBStorageDevice::tell_file(void *handle) {
  return handle ? ftell(static_cast<FILE *>(handle)) : 0;
}

bool USBStorageDevice::close_file(void *handle) {
  return handle && fclose(static_cast<FILE *>(handle)) == 0;
}

bool USBStorageDevice::format() {
  ESP_LOGW(TAG, "Format not implemented for USB storage");
  return false;
}

bool USBStorageDevice::sync() { return true; }

#endif  // USE_STORAGE

}  // namespace usb_storage
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4
