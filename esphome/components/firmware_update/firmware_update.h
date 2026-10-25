#pragma once

#include "esphome/components/ota/ota_backend.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include <functional>
#include <string>

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif
#if defined(USE_STORAGE_WORKER) && defined(USE_STORAGE_TRANSFER_BUFFER) && defined(USE_ESP32)
#define USE_FIRMWARE_UPDATE_ASYNC
#include "esphome/components/md5/md5.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/storage/transfer_buffer.h"
#endif

namespace esphome::firmware_update {

// Flash the device's own firmware from a local storage path (a mounted filesystem or a network
// share), instead of over the air. The image is streamed straight into the shared OTA flash
// backend: a plain app image goes to the next app slot, and a combined pre-fill image (app +
// named data-partition, self-described by its EPF2 header) is routed to both partitions by the
// backend -- the same code the network/HTTP OTA paths use, so nothing is compiled twice.
class FirmwareUpdateComponent final : public ota::OTAComponent {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_path(const std::string &path) { this->path_ = path; }
  void set_md5(const std::string &md5) { this->md5_expected_ = md5; }
  void set_project_name(const std::string &name) { this->project_name_ = name; }
  void load_manifest(const std::string &path, std::function<void(bool)> &&on_done);
  const std::string &get_path() const { return this->path_; }
  const std::string &get_manifest_version() const { return this->manifest_version_; }
  const std::string &get_manifest_name() const { return this->manifest_name_; }
  const std::string &get_manifest_summary() const { return this->manifest_summary_; }
  const char *get_last_error() const { return this->last_error_; }
  std::string installed_version() const;
  bool is_running() const;

  // Read the firmware image from storage and flash it. Reboots on success; on any failure the
  // OTA backend is aborted and the boot slot is left untouched (the running app keeps booting).
  void flash();

 protected:
#ifdef USE_STORAGE
  // Streams the image from the resolved storage into `backend`, hashing as it goes and setting
  // the computed MD5 on the backend for its end-of-stream check. Returns an OTA response code;
  // OTA_RESPONSE_OK means the backend was ended (activated) successfully.
  uint8_t stream_from_storage_(ota::OTABackendPtr &backend);
#endif
#ifdef USE_FIRMWARE_UPDATE_ASYNC
  bool start_async_();
  void read_next_();
  void on_chunk_(storage::StorageError err);
  void finish_async_(uint8_t status);

  ota::OTABackendPtr backend_;
  storage::StreamHandle stream_{};
  md5::MD5Digest md5_{};
  uint8_t *buf_{nullptr};
  uint64_t total_{0};
  uint64_t offset_{0};
  size_t got_{0};
  uint32_t last_progress_{0};
  bool running_{false};
  bool stream_open_{false};
  void manifest_read_(size_t at);
  void manifest_finish_(bool read_ok);
  storage::StreamHandle manifest_stream_{};
  std::string manifest_path_{};
  std::string manifest_json_{};
  std::function<void(bool)> manifest_done_{};
  bool manifest_open_{false};
#endif

  bool check_head_(const uint8_t *data, size_t len);
  void fail_(const char *error);

  std::string path_{};
  std::string md5_expected_{};
  std::string project_name_{};
  std::string manifest_version_{};
  std::string manifest_name_{};
  std::string manifest_summary_{};
  const char *last_error_{""};
  bool update_started_{false};
};

}  // namespace esphome::firmware_update
