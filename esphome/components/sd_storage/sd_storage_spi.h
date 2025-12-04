#pragma once

#include "esphome/core/defines.h"

#ifdef USE_SD_STORAGE_SPI

#include "sd_storage_base.h"
#include "esphome/core/gpio.h"
#include "esphome/components/spi/spi.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage_device.h"
#endif

#ifdef USE_ESP_IDF
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#endif

namespace esphome {
namespace sd_storage {

static const char *const TAG_SPI = "sd_storage.spi";

#ifdef USE_STORAGE
class SdSpi : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                    spi::DATA_RATE_10MHZ>,
              public SdStorageBase,
              public storage::StorageDevice {
#else
class SdSpi : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                    spi::DATA_RATE_10MHZ>,
              public SdStorageBase {
#endif
 public:
  enum ErrorCode {
    ERR_MOUNT,
    ERR_NO_CARD,
  };

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Run after storage component (DATA=600) to ensure global_storage is initialized
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Pin configuration
  void set_mode_1bit(bool mode_1bit) { this->mode_1bit_ = mode_1bit; }
  void set_mount_path(const std::string &path) { this->mount_path_ = path; }
  void set_cs_pin(GPIOPin *pin) { this->cs_pin_ = pin; }
  void set_cs_pin_number(uint8_t *pin) { this->cs_pin_number_ = pin; }
  void set_slot(uint8_t slot) { this->slot_ = slot; }
  void set_id(const std::string &id) { this->id_ = id; }

  // File operations
  bool write_file(const std::string &path, const std::string &data);
  bool append_file(const std::string &path, const std::string &data);
  std::string read_file(const std::string &path);
  bool delete_file(const std::string &path);

  // Directory operations
  bool create_directory(const std::string &path);
  bool remove_directory(const std::string &path);
  std::vector<FileInfo> list_directory(const std::string &path);

  // Utility functions
  bool is_directory(const std::string &path);
  uint32_t file_size(const std::string &path);

  // Card info
  CardType get_card_type() const { return this->card_type_; }
  bool is_mounted() const { return this->is_mounted_; }
  uint64_t get_total_bytes() const { return this->total_bytes_; }
  uint64_t get_used_bytes() const { return this->used_bytes_; }
  uint64_t get_free_bytes() const;

  // Public mount/unmount methods for external control
  bool mount_card();
  void unmount_card();

  // Mount notification system for storage consumers
  void add_mount_ready_callback(const mount_ready_callback_t &callback) {
    this->mount_ready_callbacks_.push_back(callback);
  }
  const std::string &get_mount_path() const { return this->mount_path_; }

#ifdef USE_STORAGE
  //========================================================================
  // StorageDevice Interface Implementation
  //========================================================================

  // Device Information (required)
  storage::StorageInfo get_info() override;
  bool is_available() override { return this->is_mounted_; }

  // Filesystem Access
  bool supports_filesystem() override { return true; }
  std::string get_mount_path() override { return this->mount_path_; }

  // File Operations
  bool file_exists(const char *path) override;
  bool get_file_size(const char *path, size_t *size) override;
  bool read_file(const char *path, uint8_t *data, size_t *length) override;
  bool write_file(const char *path, const uint8_t *data, size_t length) override;
  bool append_file(const char *path, const uint8_t *data, size_t length) override;
  bool delete_file(const char *path) override;
  bool rename_file(const char *old_path, const char *new_path) override;
  bool copy_file(const char *src_path, const char *dst_path) override;

  // Directory Operations
  bool dir_exists(const char *path) override;
  bool create_dir(const char *path) override;
  bool delete_dir(const char *path, bool recursive = false) override;
  bool list_dir(const char *path, std::vector<storage::StorageFileInfo> *entries) override;

  // Space Information
  bool get_space_info(uint64_t *total, uint64_t *free) override;
  bool can_write_file(const char *path, size_t size) override;

  // Streaming File Access
  void *open_file(const char *path, const char *mode) override;
  size_t read_file_chunk(void *handle, uint8_t *buffer, size_t size) override;
  size_t write_file_chunk(void *handle, const uint8_t *data, size_t size) override;
  bool seek_file(void *handle, size_t offset) override;
  size_t tell_file(void *handle) override;
  bool close_file(void *handle) override;

  // Maintenance
  bool format() override;
  bool sync() override;
#endif

 protected:
  // Configuration
  GPIOPin *cs_pin_{nullptr};
  uint8_t cs_pin_number_{255};
  bool mode_1bit_{true};  // SPI mode is always 1-bit
  uint8_t slot_{0};

  // Card state
  CardType card_type_{CardType::UNKNOWN};
  bool is_mounted_{false};
  uint64_t total_bytes_{0};
  uint64_t used_bytes_{0};
  ErrorCode init_error_;

  // Mount configuration and callbacks
  std::string mount_path_{"/sdcard"};                          // Default mount path
  std::string id_;                                             // Unique identifier for storage registry
  std::vector<mount_ready_callback_t> mount_ready_callbacks_;  // Callbacks to notify when mount is ready

#ifdef USE_ESP_IDF
  sdmmc_card_t *card_{nullptr};
#endif

  // Mount management (internal)
  bool update_card_info();

  // Helper to build full path
  std::string build_full_path(const char *path);

  // Error code conversion
  static std::string error_code_to_string(ErrorCode code);
};

}  // namespace sd_storage
}  // namespace esphome

#endif  // USE_SD_STORAGE_SPI
