#pragma once

#if defined(USE_ESP32_VARIANT_ESP32H2) || defined(USE_ESP32_VARIANT_ESP32P4)

#include "esphome/core/component.h"
#include "esphome/components/sha256/sha256.h"
#include "esphome/components/update/update_entity.h"
#include <array>
#include <memory>
#include <string>

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
#include "esphome/components/http_request/http_request.h"
#endif

#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#endif

namespace esphome::esp32_hosted {

enum class UpdateMethod : uint8_t { EMBEDDED, HTTP, STORAGE };

class Esp32HostedUpdate final : public update::UpdateEntity, public PollingComponent {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void update() override { this->check(); }
  void perform(bool force) override;
  void check() override;
  bool is_busy() const { return this->busy_; }

  void set_method(UpdateMethod method) { this->method_ = method; }
  void set_firmware_sha256(const std::array<uint8_t, 32> &sha256) { this->firmware_sha256_ = sha256; }
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  void set_source_url(const char *url) { this->source_url_ = url; }
  void set_http_request_parent(http_request::HttpRequestComponent *parent) { this->http_request_parent_ = parent; }
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  void set_storage_path(const std::string &path);
  const std::string &get_storage_path() const { return this->storage_path_; }
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  void set_firmware_data(const uint8_t *data) { this->firmware_data_ = data; }
  void set_firmware_size(size_t size) { this->firmware_size_ = size; }
#endif

 protected:
  void begin_install_();
  void next_chunk_();
  void on_chunk_();
  void install_done_(bool ok);
  void finish_install_(bool ok);

#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) || defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  bool parse_manifest_(const std::string &json_str);
  void finish_check_();
  std::string firmware_url_;
#endif
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  bool fetch_manifest_();
  bool open_download_();
  void read_download_();
  http_request::HttpRequestComponent *http_request_parent_{nullptr};
  const char *source_url_{nullptr};
  std::shared_ptr<http_request::HttpContainer> container_;
  uint32_t last_data_time_{0};
  uint8_t initial_check_remaining_{0};
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  bool is_manifest_() const;
  storage::PathStorage *open_storage_(const std::string &path, const char **rel, uint64_t *size);
  void close_stream_();
  void storage_check_();
  void storage_check_read_(size_t at);
  void storage_check_done_(bool ok);
  bool open_storage_read_();
  void read_storage_();
  std::string storage_path_;
  std::string manifest_json_;
  storage::StreamHandle stream_{};
  bool stream_open_{false};
#endif
#ifdef USE_ESP32_HOSTED_EMBEDDED_UPDATE
  const uint8_t *firmware_data_{nullptr};
  size_t firmware_size_{0};
#endif

  UpdateMethod method_{UpdateMethod::EMBEDDED};
  std::array<uint8_t, 32> firmware_sha256_{};
  sha256::SHA256 hasher_;
  uint8_t *chunk_{nullptr};
  uint64_t total_{0};
  uint64_t offset_{0};
  size_t got_{0};
  uint32_t last_progress_{0};
  update::UpdateState prev_state_{update::UPDATE_STATE_UNKNOWN};
  bool busy_{false};
};

}  // namespace esphome::esp32_hosted

#endif
