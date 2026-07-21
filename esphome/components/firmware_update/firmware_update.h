#pragma once

#include "esphome/components/ota/ota_backend.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include <string>

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome::firmware_update {

// Flash the device's own firmware from a local storage path (a mounted filesystem or a network
// share), instead of over the air. The image is streamed straight into the shared OTA flash
// backend: a plain app image goes to the next app slot, and a combined pre-fill image (app +
// named data-partition, self-described by its EPF2 header) is routed to both partitions by the
// backend — the same code the network/HTTP OTA paths use, so nothing is compiled twice.
class FirmwareUpdateComponent final : public ota::OTAComponent {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_path(const std::string &path) { this->path_ = path; }

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

  std::string path_{};
  bool update_started_{false};
};

}  // namespace esphome::firmware_update
