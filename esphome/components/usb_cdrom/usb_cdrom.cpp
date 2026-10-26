#include "usb_cdrom.h"

#if defined(USE_USB_CDROM) && defined(USE_STORAGE_ISO9660) && defined(USE_STORAGE_WORKER) && \
    (defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
     defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4))

#include "esphome/components/storage/transfer_buffer.h"
#include "esphome/components/usb_storage/msc_bot.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <new>

namespace esphome::usb_cdrom {

static const char *const TAG = "usb_cdrom";

using storage::StorageError;
namespace iso = storage::iso9660;

namespace {

constexpr uint8_t CMD_TEST_UNIT_READY = 0x00;
constexpr uint8_t CMD_REQUEST_SENSE = 0x03;
constexpr uint8_t CMD_START_STOP_UNIT = 0x1B;
constexpr uint8_t CMD_PREVENT_ALLOW = 0x1E;
constexpr uint8_t CMD_READ_CAPACITY = 0x25;
constexpr uint8_t CMD_READ_TOC = 0x43;
constexpr uint8_t CMD_GET_CONFIGURATION = 0x46;
constexpr uint8_t CMD_GET_EVENT_STATUS = 0x4A;
constexpr uint8_t SENSE_NOT_READY = 0x2;
constexpr uint8_t SENSE_ILLEGAL_REQUEST = 0x5;
constexpr uint8_t SENSE_UNIT_ATTENTION = 0x6;
constexpr uint8_t ASC_MEDIUM_CHANGED = 0x28;
constexpr uint8_t ASC_NOT_PRESENT = 0x3A;
constexpr uint8_t EVENT_EJECT_REQUEST = 1;
constexpr uint8_t EVENT_NEW_MEDIA = 2;
constexpr uint8_t EVENT_MEDIA_REMOVAL = 3;
constexpr uint8_t EVENT_MEDIA_CHANGED = 4;

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

const char *profile_name(uint16_t profile) {
  if (profile >= 0x08 && profile <= 0x0A)
    return "CD";
  if (profile >= 0x10 && profile <= 0x2B)
    return "DVD";
  if (profile >= 0x40 && profile <= 0x43)
    return "BD";
  if (profile >= 0x50 && profile <= 0x5A)
    return "HD DVD";
  return "CD/DVD";
}

}

bool CdromDrive::start(const char *mount_path) {
  const size_t len = strlen(mount_path);
  if (len >= sizeof(this->mount_buf_) || storage::global_storage_registry == nullptr)
    return false;
  memcpy(this->mount_buf_, mount_path, len + 1);
  this->set_mount_path_(this->mount_buf_);
  this->dev_->sector_size_ = iso::SECTOR_SIZE;
  if (storage::global_storage_registry->register_storage(this) != StorageError::STORAGE_ERROR_OK)
    return false;
  this->registered_ = true;
  return true;
}

void CdromDrive::stop() {
  if (this->registered_) {
    storage::global_storage_registry->unregister_storage(this);
    this->registered_ = false;
  }
  this->mounted_.store(false, std::memory_order_release);
  this->close_all_files_();
  this->index_.clear();
  if (this->sector_buf_ != nullptr) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->sector_buf_, iso::SECTOR_SIZE);
    this->sector_buf_ = nullptr;
  }
}

bool CdromDrive::lock_(bool wait) {
  return xSemaphoreTake(this->dev_->io_lock_, wait ? portMAX_DELAY : 0) == pdTRUE;
}

void CdromDrive::unlock_() { xSemaphoreGive(this->dev_->io_lock_); }

bool CdromDrive::command_(const uint8_t *cdb, uint8_t cdb_len, uint8_t *in, uint16_t in_len) {
  usb_storage::USBStorageDevice *d = this->dev_;
  if (d->data_[0].xfer == nullptr)
    return false;
  const uint32_t tag = d->cbw_tag_++;
  if (!d->send_cbw_(tag, in_len, in_len > 0 ? usb_storage::MSC_BOT_CBW_FLAGS_IN : usb_storage::MSC_BOT_CBW_FLAGS_OUT,
                    cdb, cdb_len))
    return false;
  if (in_len > 0) {
    struct Rx {
      usb_storage::USBStorageDevice *dev;
      uint8_t *buf;
      uint16_t len;
    };
    Rx rx{d, in, in_len};
    auto cb = [&rx](const usb_host::TransferStatus &s) {
      if (s.success && s.data_len > 0)
        memcpy(rx.buf, s.data, std::min<size_t>(s.data_len, rx.len));
      usb_storage::USBStorageDevice::transfer_done_cb(s, rx.dev);
    };
    d->transfer_stalled_ = false;
    if (!d->client_->transfer_in(*d->dev_, d->bulk_in_ep_, cb, d->round_up_to_in_mps_(in_len)))
      return false;
    if (!d->wait_transfer_()) {
      if (d->transfer_stalled_ && d->clear_halt_(d->bulk_in_ep_))
        d->recv_csw_(tag, true);
      return false;
    }
  }
  return d->recv_csw_(tag, true);
}

bool CdromDrive::sense_(uint8_t &key, uint8_t &asc) {
  const uint8_t cdb[6] = {CMD_REQUEST_SENSE, 0, 0, 0, 18, 0};
  uint8_t resp[18]{};
  if (!this->command_(cdb, sizeof(cdb), resp, sizeof(resp)))
    return false;
  key = resp[2] & 0x0F;
  asc = resp[12];
  return true;
}

CdromDrive::Presence CdromDrive::unit_ready_() {
  const uint8_t cdb[6] = {CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
  if (this->command_(cdb, sizeof(cdb), nullptr, 0))
    return Presence::PRESENT;
  uint8_t key = 0;
  uint8_t asc = 0;
  if (!this->sense_(key, asc))
    return Presence::UNKNOWN;
  if (key == SENSE_UNIT_ATTENTION && asc == ASC_MEDIUM_CHANGED)
    return Presence::CHANGED;
  if (key == SENSE_NOT_READY && asc == ASC_NOT_PRESENT)
    return Presence::ABSENT;
  return Presence::UNKNOWN;
}

CdromDrive::Presence CdromDrive::presence_() {
  if (this->events_supported_) {
    const uint8_t cdb[10] = {CMD_GET_EVENT_STATUS, 0x01, 0, 0, 0x10, 0, 0, 0, 8, 0};
    uint8_t r[8]{};
    if (this->command_(cdb, sizeof(cdb), r, sizeof(r))) {
      if ((r[2] & 0x80) == 0 && (r[2] & 0x07) == 4 && be16(r) >= 6) {
        const uint8_t code = r[4] & 0x0F;
        if (code == EVENT_EJECT_REQUEST)
          return Presence::EJECT_REQUEST;
        if (code == EVENT_NEW_MEDIA || code == EVENT_MEDIA_CHANGED)
          return Presence::CHANGED;
        if (code == EVENT_MEDIA_REMOVAL)
          return Presence::ABSENT;
        return (r[5] & 0x02) != 0 ? Presence::PRESENT : Presence::ABSENT;
      }
    } else {
      uint8_t key = 0;
      uint8_t asc = 0;
      if (this->sense_(key, asc) && key == SENSE_ILLEGAL_REQUEST) {
        this->events_supported_ = false;
        ESP_LOGD(TAG, "'%s': no media events, TEST UNIT READY instead", this->mount_buf_);
      }
    }
  }
  return this->unit_ready_();
}

void CdromDrive::medium_lock_(bool prevent) {
  const uint8_t cdb[6] = {CMD_PREVENT_ALLOW, 0, 0, 0, static_cast<uint8_t>(prevent ? 1 : 0), 0};
  const bool ok = this->command_(cdb, sizeof(cdb), nullptr, 0);
  this->locked_ = prevent && ok;
}

void CdromDrive::start_stop_(bool load) {
  const uint8_t cdb[6] = {CMD_START_STOP_UNIT, 0x01, 0, 0, static_cast<uint8_t>(load ? 0x03 : 0x02), 0};
  this->command_(cdb, sizeof(cdb), nullptr, 0);
}

uint16_t CdromDrive::current_profile_() {
  const uint8_t cdb[10] = {CMD_GET_CONFIGURATION, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
  uint8_t r[8]{};
  if (!this->command_(cdb, sizeof(cdb), r, sizeof(r)))
    return 0;
  return be16(r + 6);
}

uint32_t CdromDrive::last_session_() {
  const uint8_t cdb[10] = {CMD_READ_TOC, 0, 0x01, 0, 0, 0, 0, 0, 12, 0};
  uint8_t r[12]{};
  if (!this->command_(cdb, sizeof(cdb), r, sizeof(r)) || be16(r) < 10)
    return 0;
  return be32(r + 8);
}

bool CdromDrive::read_sectors_(uint32_t lba, uint8_t *buf, uint32_t count) {
  return this->dev_->data_[0].xfer != nullptr && this->dev_->scsi_read_(lba, buf, count);
}

StorageError CdromDrive::build_index_(uint32_t session) {
  uint8_t *buf = nullptr;
  uint32_t sectors = 1;
  bool borrowed = false;
#if defined(USE_STORAGE_TRANSFER_BUFFER) && defined(USE_ESP32)
  if (storage::global_transfer_buffer != nullptr) {
    buf = storage::global_transfer_buffer->try_acquire(READ_SECTORS * iso::SECTOR_SIZE);
    if (buf != nullptr) {
      borrowed = true;
      sectors = READ_SECTORS;
    }
  }
#endif
  if (buf == nullptr)
    buf = this->sector_buf_;
  StorageError err = StorageError::STORAGE_ERROR_OK;
  const uint32_t bases[2] = {session, 0};
  for (uint32_t base : bases) {
    iso::Builder builder;
    builder.start(&this->index_, base, sectors);
    err = StorageError::STORAGE_ERROR_OK;
    for (;;) {
      uint32_t lba = 0;
      uint32_t count = 0;
      const iso::Builder::Step step = builder.next(&lba, &count);
      if (step == iso::Builder::Step::DONE)
        break;
      if (step == iso::Builder::Step::FAILED) {
        err = builder.error();
        break;
      }
      if (!this->read_sectors_(lba, buf, count)) {
        err = StorageError::STORAGE_ERROR_READ_ERROR;
        this->index_.clear();
        break;
      }
      builder.feed(buf, count);
    }
    if (err != StorageError::STORAGE_ERROR_CORRUPT || base == 0)
      break;
  }
#if defined(USE_STORAGE_TRANSFER_BUFFER) && defined(USE_ESP32)
  if (borrowed)
    storage::global_transfer_buffer->release();
#endif
  this->cached_lba_ = UINT32_MAX;
  if (err == StorageError::STORAGE_ERROR_OK && this->index_.empty())
    err = StorageError::STORAGE_ERROR_CORRUPT;
  return err;
}

StorageError CdromDrive::mount() {
  if (this->media_mounted())
    return StorageError::STORAGE_ERROR_OK;
  const bool from_check = this->check_mount_.exchange(false);
  if (!this->lock_(true))
    return StorageError::STORAGE_ERROR_NOT_READY;
  Presence p = this->unit_ready_();
  if (p == Presence::CHANGED)
    p = this->unit_ready_();
  if (p != Presence::PRESENT) {
    if (p == Presence::ABSENT && !from_check)
      this->start_stop_(true);
    this->unlock_();
    return StorageError::STORAGE_ERROR_NOT_READY;
  }
  this->profile_ = this->current_profile_();
  const uint32_t session = this->last_session_();
  const uint8_t cap_cdb[10] = {CMD_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t cap[8]{};
  if (this->command_(cap_cdb, sizeof(cap_cdb), cap, sizeof(cap)))
    this->dev_->sector_count_ = be32(cap) + 1;
  this->dev_->sector_size_ = iso::SECTOR_SIZE;
  if (this->sector_buf_ == nullptr)
    this->sector_buf_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(iso::SECTOR_SIZE);
  if (this->sector_buf_ == nullptr) {
    this->unlock_();
    return StorageError::STORAGE_ERROR_NO_SPACE;
  }
  const StorageError err = this->build_index_(session);
  if (err != StorageError::STORAGE_ERROR_OK) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->sector_buf_, iso::SECTOR_SIZE);
    this->sector_buf_ = nullptr;
    this->unlock_();
    return err;
  }
  if (this->events_supported_)
    this->medium_lock_(true);
  this->unlock_();
  this->mounted_.store(true, std::memory_order_release);
  this->owner_->note_media_event();
  return StorageError::STORAGE_ERROR_OK;
}

void CdromDrive::release_media_() {
  if (this->registered_)
    storage::global_storage_registry->quiesce_storage(this);
  if (!this->media_mounted())
    return;
  this->mounted_.store(false, std::memory_order_release);
  this->close_all_files_();
  this->index_.clear();
  this->cached_lba_ = UINT32_MAX;
  if (this->sector_buf_ != nullptr) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->sector_buf_, iso::SECTOR_SIZE);
    this->sector_buf_ = nullptr;
  }
  if (this->locked_ && this->lock_(true)) {
    this->medium_lock_(false);
    this->unlock_();
  }
#ifdef USE_STORAGE_CHANGE_FEED
  storage::global_storage_registry->note_dir_changed("");
#endif
  this->owner_->note_media_event();
}

StorageError CdromDrive::unmount() {
  if (storage::global_storage_worker->has_active_task_io(this))
    return StorageError::STORAGE_ERROR_NOT_READY;
  this->release_media_();
  if (!this->lock_(true))
    return StorageError::STORAGE_ERROR_NOT_READY;
  if (this->locked_)
    this->medium_lock_(false);
  this->start_stop_(false);
  this->unlock_();
  this->present_ = false;
  this->mount_failed_ = false;
  return StorageError::STORAGE_ERROR_OK;
}

StorageError CdromDrive::load() {
  if (storage::global_storage_worker->has_active_task_io(this))
    return StorageError::STORAGE_ERROR_NOT_READY;
  if (!this->lock_(true))
    return StorageError::STORAGE_ERROR_NOT_READY;
  this->start_stop_(true);
  this->unlock_();
  this->mount_failed_ = false;
  return StorageError::STORAGE_ERROR_OK;
}

void CdromDrive::request_mount_() {
  if (this->mount_pending_)
    return;
  this->mount_pending_ = true;
  this->check_mount_.store(true);
  USBCdrom *owner = this->owner_;
  CdromDrive *drive = this;
  const StorageError err = storage::global_storage_worker->async_mount(this, [owner, drive](StorageError e) {
    if (owner->has_drive(drive))
      drive->on_mount_done(e);
  });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->mount_pending_ = false;
    this->check_mount_.store(false);
  }
}

void CdromDrive::on_mount_done(StorageError err) {
  this->mount_pending_ = false;
  this->check_mount_.store(false);
  if (err == StorageError::STORAGE_ERROR_OK || err == StorageError::STORAGE_ERROR_NOT_READY)
    return;
  this->mount_failed_ = true;
  ESP_LOGW(TAG, "Disc in '%s' not mounted: %s", this->mount_buf_, storage::error_to_string(err));
}

void CdromDrive::check_media() {
  if (this->mount_pending_ || storage::global_storage_worker->is_busy_with(this))
    return;
  if (!this->lock_(false))
    return;
  const Presence p = this->presence_();
  this->unlock_();
  switch (p) {
    case Presence::EJECT_REQUEST:
      this->unmount();
      return;
    case Presence::ABSENT:
      this->present_ = false;
      this->mount_failed_ = false;
      if (this->media_mounted())
        this->release_media_();
      return;
    case Presence::CHANGED:
      this->mount_failed_ = false;
      if (this->media_mounted()) {
        this->release_media_();
        return;
      }
      this->present_ = true;
      this->request_mount_();
      return;
    case Presence::PRESENT:
      this->present_ = true;
      if (!this->media_mounted() && !this->mount_failed_)
        this->request_mount_();
      return;
    default:
      return;
  }
}

StorageError CdromDrive::get_info(storage::StorageInfo *info) {
  const bool mounted = this->media_mounted();
  info->id = this->mount_buf_;
  info->name = mounted && this->index_.label()[0] != '\0' ? this->index_.label() : profile_name(this->profile_);
  info->kind = "cdrom";
  info->total_bytes = mounted ? this->index_.volume_bytes() : 0;
  info->free_bytes = 0;
  info->block_size = iso::SECTOR_SIZE;
  info->is_mounted = mounted;
  info->is_removable = true;
  info->is_read_only = true;
  return StorageError::STORAGE_ERROR_OK;
}

StorageError CdromDrive::find_extent_(const char *path, uint64_t *offset, uint64_t *length) {
  if (!this->media_mounted())
    return StorageError::STORAGE_ERROR_NOT_READY;
  iso::Entry e{};
  const StorageError err = this->index_.lookup(path, &e);
  if (err != StorageError::STORAGE_ERROR_OK)
    return err;
  if (e.is_dir)
    return StorageError::STORAGE_ERROR_INVALID_ARGS;
  if (!e.contiguous)
    return StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  *offset = static_cast<uint64_t>(e.lba) * iso::SECTOR_SIZE;
  *length = e.size;
  return StorageError::STORAGE_ERROR_OK;
}

StorageError CdromDrive::read_medium_(storage::OpticalFileHandle *file, uint64_t offset, uint8_t *buf, size_t len,
                                      size_t *bytes_transferred) {
  if (!this->media_mounted() || this->sector_buf_ == nullptr)
    return StorageError::STORAGE_ERROR_NOT_READY;
  size_t done = 0;
  while (done < len) {
    const uint64_t at = offset + done;
    const uint32_t lba = static_cast<uint32_t>(at / iso::SECTOR_SIZE);
    const uint32_t off = static_cast<uint32_t>(at % iso::SECTOR_SIZE);
    const size_t rest = len - done;
    size_t n;
    if (off == 0 && rest >= iso::SECTOR_SIZE) {
      const uint32_t count = static_cast<uint32_t>(rest / iso::SECTOR_SIZE);
      if (!this->dev_->scsi_read(lba, buf + done, count))
        return StorageError::STORAGE_ERROR_READ_ERROR;
      n = static_cast<size_t>(count) * iso::SECTOR_SIZE;
    } else {
      if (this->cached_lba_ != lba) {
        if (!this->dev_->scsi_read(lba, this->sector_buf_, 1)) {
          this->cached_lba_ = UINT32_MAX;
          return StorageError::STORAGE_ERROR_READ_ERROR;
        }
        this->cached_lba_ = lba;
      }
      n = std::min<size_t>(iso::SECTOR_SIZE - off, rest);
      memcpy(buf + done, this->sector_buf_ + off, n);
    }
    done += n;
  }
  if (bytes_transferred != nullptr)
    *bytes_transferred = done;
  return StorageError::STORAGE_ERROR_OK;
}

StorageError CdromDrive::stat(const char *path, storage::FileStat *st) {
  if (!this->media_mounted())
    return StorageError::STORAGE_ERROR_NOT_READY;
  return this->index_.stat(path, st);
}

StorageError CdromDrive::list_dir(const char *path, bool (*callback)(const storage::FileStat *entry, void *ctx),
                                  void *ctx) {
  if (!this->media_mounted())
    return StorageError::STORAGE_ERROR_NOT_READY;
  return this->index_.list(path, callback, ctx);
}

void USBCdrom::setup() { this->disable_loop(); }

void USBCdrom::loop() {
  for (auto *drive : this->drives_) {
    if (drive == nullptr)
      continue;
    const bool mounted = drive->media_mounted();
    if (mounted == drive->reported_mounted())
      continue;
    drive->set_reported(mounted);
    if (mounted) {
      ESP_LOGI(TAG, "Disc '%s' mounted at '%s'", drive->label(), drive->get_mount_path());
      this->on_mounted_.call(drive->get_mount_path());
    } else {
      ESP_LOGI(TAG, "Disc removed from '%s'", drive->get_mount_path());
      this->on_removed_.call(drive->get_mount_path());
    }
  }
  this->disable_loop();
}

void USBCdrom::dump_config() {
  ESP_LOGCONFIG(TAG,
                "USB CD/DVD:\n"
                "  Mount path: %s\n"
                "  Drives: %u\n"
                "  Media check interval: %" PRIu32 " ms",
                this->base_, this->max_, this->interval_ms_);
}

bool USBCdrom::has_drive(const CdromDrive *drive) const {
  for (const auto *d : this->drives_) {
    if (d != nullptr && d == drive)
      return true;
  }
  return false;
}

CdromDrive *USBCdrom::attach(usb_storage::USBStorageDevice *dev) {
  int slot = -1;
  uint8_t used = 0;
  for (uint8_t i = 0; i < this->max_; i++) {
    if (this->drives_[i] != nullptr) {
      used++;
    } else if (slot < 0) {
      slot = i;
    }
  }
  if (slot < 0) {
    ESP_LOGW(TAG, "No room for another CD/DVD drive (max_drives %u)", this->max_);
    return nullptr;
  }
  char path[CdromDrive::MOUNT_PATH_SIZE];
  if (slot == 0) {
    snprintf(path, sizeof(path), "%s", this->base_);
  } else {
    snprintf(path, sizeof(path), "%s%d", this->base_, slot + 1);
  }
  RAMAllocator<CdromDrive> alloc(RAMAllocator<CdromDrive>::ALLOC_EXTERNAL);
  CdromDrive *raw = alloc.allocate(1);
  if (raw == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the drive at '%s'", path);
    return nullptr;
  }
  auto *drive = new (raw) CdromDrive(this, dev);
  if (!drive->start(path)) {
    ESP_LOGE(TAG, "Drive at '%s' not registered", path);
    drive->~CdromDrive();
    alloc.deallocate(drive, 1);
    return nullptr;
  }
  this->drives_[slot] = drive;
  ESP_LOGI(TAG, "CD/DVD drive at '%s'", path);
  if (used == 0 && this->interval_ms_ > 0)
    this->set_interval("media", this->interval_ms_, [this]() { this->check_all_(); });
  this->defer([this, drive]() {
    if (this->has_drive(drive))
      drive->check_media();
  });
  return drive;
}

void USBCdrom::detach(CdromDrive *drive) {
  bool any = false;
  for (auto *&d : this->drives_) {
    if (d == drive) {
      if (drive->reported_mounted()) {
        ESP_LOGI(TAG, "Disc removed from '%s'", drive->get_mount_path());
        this->on_removed_.call(drive->get_mount_path());
      }
      ESP_LOGI(TAG, "CD/DVD drive at '%s' removed", drive->get_mount_path());
      drive->stop();
      d = nullptr;
      drive->~CdromDrive();
      RAMAllocator<CdromDrive>(RAMAllocator<CdromDrive>::ALLOC_EXTERNAL).deallocate(drive, 1);
    } else if (d != nullptr) {
      any = true;
    }
  }
  if (!any)
    this->cancel_interval("media");
}

CdromDrive *USBCdrom::find_(const char *mount_path) const {
  for (auto *d : this->drives_) {
    if (d == nullptr)
      continue;
    if (mount_path == nullptr || mount_path[0] == '\0' || strcmp(d->get_mount_path(), mount_path) == 0)
      return d;
  }
  return nullptr;
}

StorageError USBCdrom::eject(const char *mount_path) {
  CdromDrive *drive = this->find_(mount_path);
  if (drive == nullptr)
    return StorageError::STORAGE_ERROR_NOT_FOUND;
  return drive->unmount();
}

StorageError USBCdrom::load(const char *mount_path) {
  CdromDrive *drive = this->find_(mount_path);
  if (drive == nullptr)
    return StorageError::STORAGE_ERROR_NOT_FOUND;
  return drive->load();
}

void USBCdrom::check_all_() {
  for (auto *d : this->drives_) {
    if (d != nullptr)
      d->check_media();
  }
}

}

#endif
