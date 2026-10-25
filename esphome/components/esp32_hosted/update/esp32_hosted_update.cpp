#if defined(USE_ESP32_VARIANT_ESP32H2) || defined(USE_ESP32_VARIANT_ESP32P4)
#include "esp32_hosted_update.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/components/sha256/sha256.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include <esp_image_format.h>
#include <esp_app_desc.h>
#include <esp_hosted.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_ota_ops.h>

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#endif

#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include <cstring>
#endif

extern "C" {
#include <esp_hosted_ota.h>
}

namespace esphome::esp32_hosted {

ESPHOME_LOG_TAG(TAG, "esp32_hosted.update");

// The host rejects writes above its per-RPC payload limit before they reach the coprocessor
constexpr size_t CHUNK_SIZE = EH_HOST_CP_OTA_CHUNK_MAX;
// OTA begin blocks while the coprocessor erases its partition, end while it validates the image
constexpr uint32_t OTA_WDT_TIMEOUT_MS = 60000;

static esp_err_t ota_end_guarded() {
  watchdog::WatchdogManager wdt(OTA_WDT_TIMEOUT_MS);
  return esp_hosted_slave_ota_end();  // NOLINT
}

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
// Interval/timeout IDs (uint32_t to avoid string comparison)
constexpr uint32_t INITIAL_CHECK_INTERVAL_ID = 0;
#endif

// Compile-time version string from esp_hosted_host_fw_ver.h macros
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)
static const char *const ESP_HOSTED_VERSION_STR = STRINGIFY(ESP_HOSTED_VERSION_MAJOR_1) "." STRINGIFY(
    ESP_HOSTED_VERSION_MINOR_1) "." STRINGIFY(ESP_HOSTED_VERSION_PATCH_1);

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
// Parse an integer from str, advancing ptr past the number
// Returns false if no digits were parsed
static bool parse_int(const char *&ptr, int &value) {
  char *end;
  value = static_cast<int>(strtol(ptr, &end, 10));
  if (end == ptr)
    return false;
  ptr = end;
  return true;
}

// Parse version string "major.minor.patch" into components
// Returns true if at least major.minor was parsed
static bool parse_version(const std::string &version_str, int &major, int &minor, int &patch) {
  major = minor = patch = 0;
  const char *ptr = version_str.c_str();

  if (!parse_int(ptr, major) || *ptr != '.')
    return false;
  ++ptr;
  if (!parse_int(ptr, minor))
    return false;
  if (*ptr == '.')
    parse_int(++ptr, patch);

  return true;
}

// Compare two versions, returns:
//  -1 if v1 < v2
//   0 if v1 == v2
//   1 if v1 > v2
static int compare_versions(int major1, int minor1, int patch1, int major2, int minor2, int patch2) {
  if (major1 != major2)
    return major1 < major2 ? -1 : 1;
  if (minor1 != minor2)
    return minor1 < minor2 ? -1 : 1;
  if (patch1 != patch2)
    return patch1 < patch2 ? -1 : 1;
  return 0;
}
#endif

void Esp32HostedUpdate::setup() {
  this->update_info_.title = "ESP32 Hosted Coprocessor";

#ifndef USE_WIFI
  // If WiFi is not present, connect to the coprocessor
  esp_hosted_connect_to_slave();  // NOLINT
#endif

  // Get coprocessor version
  esp_hosted_coprocessor_fwver_t ver_info;
  if (esp_hosted_get_coprocessor_fwversion(&ver_info) == ESP_OK) {
    // 16 bytes: "255.255.255" (11 chars) + null + safety margin
    char buf[16];
    snprintf(buf, sizeof(buf), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, ver_info.major1, ver_info.minor1, ver_info.patch1);
    this->update_info_.current_version = buf;
  } else {
    this->update_info_.current_version = "unknown";
  }
  ESP_LOGD(TAG, "Coprocessor version: %s", this->update_info_.current_version.c_str());

#if !defined(USE_ESP32_HOSTED_HTTP_UPDATE) && !defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  // Embedded mode: get image version from embedded firmware
  const int app_desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (this->firmware_size_ >= app_desc_offset + sizeof(esp_app_desc_t)) {
    esp_app_desc_t *app_desc = (esp_app_desc_t *) (this->firmware_data_ + app_desc_offset);
    if (app_desc->magic_word == ESP_APP_DESC_MAGIC_WORD) {
      ESP_LOGD(TAG,
               "ESP32 Hosted firmware:\n"
               "  Firmware version: %s\n"
               "  Project name: %s\n"
               "  Build date: %s\n"
               "  Build time: %s\n"
               "  IDF version: %s",
               app_desc->version, app_desc->project_name, app_desc->date, app_desc->time, app_desc->idf_ver);
      this->update_info_.latest_version = app_desc->version;
      if (this->update_info_.latest_version != this->update_info_.current_version) {
        this->state_ = update::UPDATE_STATE_AVAILABLE;
      } else {
        this->state_ = update::UPDATE_STATE_NO_UPDATE;
      }
    } else {
      ESP_LOGW(TAG, "Invalid app description magic word: 0x%08" PRIx32 " (expected 0x%08" PRIx32 ")",
               app_desc->magic_word, static_cast<uint32_t>(ESP_APP_DESC_MAGIC_WORD));
      this->state_ = update::UPDATE_STATE_NO_UPDATE;
    }
  } else {
    ESP_LOGW(TAG, "Firmware too small to contain app description");
    this->state_ = update::UPDATE_STATE_NO_UPDATE;
  }

  // Publish state
  this->status_clear_error();
  this->publish_state();
#elif !defined(USE_ESP32_HOSTED_HTTP_UPDATE)
  // Storage mode: no availability/version discovery — the firmware to install is whatever the
  // user placed on the configured storage path. Offer it as available; the install reads and
  // verifies (sha256) at perform() time.
  this->update_info_.latest_version = "storage";
  this->state_ = update::UPDATE_STATE_AVAILABLE;
  this->status_clear_error();
  this->publish_state();
  // Defer so the automation runs on the main loop after setup, not during App.setup()
  if (this->state_ == update::UPDATE_STATE_AVAILABLE && this->update_available_trigger_) {
    this->defer([this]() { this->update_available_trigger_->trigger(this->update_info_); });
  }
#else
  // HTTP mode: check every 10s until network is ready (max 6 attempts)
  // Only if update interval is > 1 minute to avoid redundant checks
  if (this->get_update_interval() > 60000) {
    this->initial_check_remaining_ = 6;
    this->set_interval(INITIAL_CHECK_INTERVAL_ID, 10000, [this]() {
      bool connected = network::is_connected();
      if (--this->initial_check_remaining_ == 0 || connected) {
        this->cancel_interval(INITIAL_CHECK_INTERVAL_ID);
        if (connected) {
          this->check();
        }
      }
    });
  }
#endif
}

void Esp32HostedUpdate::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP32 Hosted Update:\n"
                "  Host Library Version: %s\n"
                "  Coprocessor Version: %s\n"
                "  Latest Version: %s",
                ESP_HOSTED_VERSION_STR, this->update_info_.current_version.c_str(),
                this->update_info_.latest_version.c_str());
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  ESP_LOGCONFIG(TAG,
                "  Mode: HTTP\n"
                "  Source URL: %s",
                this->source_url_);
#elif defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  ESP_LOGCONFIG(TAG,
                "  Mode: Storage\n"
                "  Path: %s",
                this->storage_path_.c_str());
#else
  ESP_LOGCONFIG(TAG,
                "  Mode: Embedded\n"
                "  Firmware Size: %zu bytes",
                this->firmware_size_);
#endif
}

void Esp32HostedUpdate::check() {
#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
  if (this->source_url_[0] == '/') {
    this->storage_check_();
    return;
  }
#endif
  if (!network::is_connected()) {
    ESP_LOGD(TAG, "Network not connected, skipping update check");
    return;
  }

  if (!this->fetch_manifest_()) {
    return;
  }

  this->finish_check_();
#endif
}

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
void Esp32HostedUpdate::finish_check_() {
  const bool was_available = this->state_ == update::UPDATE_STATE_AVAILABLE;

  // Compare versions
  if (this->update_info_.latest_version.empty() ||
      this->update_info_.latest_version == this->update_info_.current_version) {
    this->state_ = update::UPDATE_STATE_NO_UPDATE;
  } else {
    this->state_ = update::UPDATE_STATE_AVAILABLE;
  }

  this->update_info_.has_progress = false;
  this->update_info_.progress = 0.0f;
  this->status_clear_error();
  this->publish_state();
  if (this->state_ == update::UPDATE_STATE_AVAILABLE && !was_available && this->update_available_trigger_) {
    this->update_available_trigger_->trigger(this->update_info_);
  }
}

bool Esp32HostedUpdate::fetch_manifest_() {
  ESP_LOGD(TAG, "Fetching manifest");

  auto container = this->http_request_parent_->get(this->source_url_);
  if (container == nullptr || container->status_code != 200) {
    ESP_LOGE(TAG, "Failed to fetch manifest from %s", this->source_url_);
    this->status_set_error(LOG_STR("Failed to fetch manifest"));
    return false;
  }

  // Read manifest JSON into string (manifest is small, ~1KB max)
  // NOTE: HttpContainer::read() has non-BSD socket semantics - see http_request.h
  // Use http_read_loop_result() helper instead of checking return values directly
  std::string json_str;
  json_str.reserve(container->content_length);
  uint8_t buf[256];
  uint32_t last_data_time = millis();
  const uint32_t read_timeout = this->http_request_parent_->get_timeout();
  while (container->get_bytes_read() < container->content_length) {
    int read_or_error = container->read(buf, sizeof(buf));
    App.feed_wdt();
    yield();
    auto result =
        http_request::http_read_loop_result(read_or_error, last_data_time, read_timeout, container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::RETRY)
      continue;
    // Note: COMPLETE is currently unreachable since the loop condition checks bytes_read < content_length,
    // but this is defensive code in case chunked transfer encoding support is added in the future.
    if (result != http_request::HttpReadLoopResult::DATA)
      break;  // COMPLETE, ERROR, or TIMEOUT
    json_str.append(reinterpret_cast<char *>(buf), read_or_error);
  }
  container->end();

  if (!this->parse_manifest_(json_str)) {
    ESP_LOGE(TAG, "Failed to parse manifest JSON");
    this->status_set_error(LOG_STR("Failed to parse manifest"));
    return false;
  }

  return true;
}

bool Esp32HostedUpdate::parse_manifest_(const std::string &json_str) {
  // Parse JSON manifest
  // Format: {"versions": [{"version": "2.7.0", "url": "...", "sha256": "..."}]}
  // Only consider versions <= host library version to avoid compatibility issues
  return json::parse_json(json_str, [this](JsonObject root) -> bool {
    if (!root["versions"].is<JsonArray>()) {
      ESP_LOGE(TAG, "Manifest does not contain 'versions' array");
      return false;
    }

    JsonArray versions = root["versions"].as<JsonArray>();
    if (versions.size() == 0) {
      ESP_LOGE(TAG, "Manifest 'versions' array is empty");
      return false;
    }

    // Find the highest version that is compatible with the host library
    // (version <= host version to avoid upgrading coprocessor ahead of host)
    int best_major = -1, best_minor = -1, best_patch = -1;
    std::string best_version, best_url, best_sha256;

    for (JsonObject entry : versions) {
      if (!entry["version"].is<const char *>() || !entry["url"].is<const char *>() ||
          !entry["sha256"].is<const char *>()) {
        continue;  // Skip malformed entries
      }

      std::string ver_str = entry["version"].as<std::string>();
      int major, minor, patch;
      if (!parse_version(ver_str, major, minor, patch)) {
        ESP_LOGW(TAG, "Failed to parse version: %s", ver_str.c_str());
        continue;
      }

      // Check if this version is compatible (not newer than host)
      if (compare_versions(major, minor, patch, ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
                           ESP_HOSTED_VERSION_PATCH_1) > 0) {
        continue;
      }

      // Check if this is better than our current best
      if (best_major < 0 || compare_versions(major, minor, patch, best_major, best_minor, best_patch) > 0) {
        best_major = major;
        best_minor = minor;
        best_patch = patch;
        best_version = ver_str;
        best_url = entry["url"].as<std::string>();
        best_sha256 = entry["sha256"].as<std::string>();
      }
    }

    if (best_major < 0) {
      ESP_LOGW(TAG, "No compatible firmware version found (host is %s)", ESP_HOSTED_VERSION_STR);
      return false;
    }

    this->update_info_.latest_version = best_version;
    this->firmware_url_ = best_url;
    if (best_url.rfind("http", 0) != 0 && best_url[0] != '/') {
      const char *slash = strrchr(this->source_url_, '/');
      if (slash != nullptr)
        this->firmware_url_.insert(0, this->source_url_, slash - this->source_url_ + 1);
    }

    // Parse SHA256 hex string to bytes
    if (!parse_hex(best_sha256, this->firmware_sha256_.data(), 32)) {
      ESP_LOGE(TAG, "Invalid SHA256: %s", best_sha256.c_str());
      return false;
    }

    ESP_LOGD(TAG, "Best compatible version: %s", this->update_info_.latest_version.c_str());

    return true;
  });
}

bool Esp32HostedUpdate::stream_firmware_to_coprocessor_() {
  ESP_LOGI(TAG, "Downloading firmware");

  auto container = this->http_request_parent_->get(this->firmware_url_);
  if (container == nullptr || container->status_code != 200) {
    ESP_LOGE(TAG, "Failed to fetch firmware");
    this->status_set_error(LOG_STR("Failed to fetch firmware"));
    return false;
  }

  size_t total_size = container->content_length;
  ESP_LOGI(TAG, "Firmware size: %zu bytes", total_size);

  // Begin OTA on coprocessor
  esp_err_t err;
  {
    watchdog::WatchdogManager wdt(OTA_WDT_TIMEOUT_MS);
    err = esp_hosted_slave_ota_begin();  // NOLINT
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
    container->end();
    this->status_set_error(LOG_STR("Failed to begin OTA"));
    return false;
  }

  // Stream firmware to coprocessor while computing SHA256
  // NOTE: HttpContainer::read() has non-BSD socket semantics - see http_request.h
  // Use http_read_loop_result() helper instead of checking return values directly
  sha256::SHA256 hasher;
  hasher.init();

  uint8_t buffer[CHUNK_SIZE];
  uint32_t last_data_time = millis();
  const uint32_t read_timeout = this->http_request_parent_->get_timeout();
  while (container->get_bytes_read() < total_size) {
    int read_or_error = container->read(buffer, sizeof(buffer));

    // Feed watchdog and give other tasks a chance to run
    App.feed_wdt();
    yield();

    auto result =
        http_request::http_read_loop_result(read_or_error, last_data_time, read_timeout, container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::RETRY)
      continue;
    // Note: COMPLETE is currently unreachable since the loop condition checks bytes_read < content_length,
    // but this is defensive code in case chunked transfer encoding support is added in the future.
    if (result == http_request::HttpReadLoopResult::COMPLETE)
      break;
    if (result != http_request::HttpReadLoopResult::DATA) {
      if (result == http_request::HttpReadLoopResult::TIMEOUT) {
        ESP_LOGE(TAG, "Timeout reading firmware data");
      } else {
        ESP_LOGE(TAG, "Error reading firmware data: %d", read_or_error);
      }
      ota_end_guarded();
      container->end();
      this->status_set_error(LOG_STR("Download failed"));
      return false;
    }

    hasher.add(buffer, read_or_error);
    err = esp_hosted_slave_ota_write(buffer, read_or_error);  // NOLINT
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
      ota_end_guarded();
      container->end();
      this->status_set_error(LOG_STR("Failed to write OTA data"));
      return false;
    }
  }
  container->end();

  // Verify SHA256
  hasher.calculate();
  if (!hasher.equals_bytes(this->firmware_sha256_.data())) {
    ESP_LOGE(TAG, "SHA256 mismatch");
    ota_end_guarded();
    this->status_set_error(LOG_STR("SHA256 verification failed"));
    return false;
  }

  ESP_LOGI(TAG, "SHA256 verified successfully");
  return true;
}
#endif
#ifdef USE_ESP32_HOSTED_STORAGE_UPDATE
storage::PathStorage *Esp32HostedUpdate::open_storage_(const char **rel, uint64_t *size) {
  storage::PathStorage *ps = storage::global_storage_registry == nullptr
                                 ? nullptr
                                 : storage::global_storage_registry->resolve_path(this->storage_path_.c_str(), rel);
  storage::FileStat st{};
  if (ps == nullptr || *rel == nullptr || storage::global_storage_worker == nullptr ||
      storage::global_storage_worker->has_active_task_io(ps) ||
      ps->stat(*rel, &st) != storage::StorageError::STORAGE_ERROR_OK || st.is_dir || st.size == 0) {
    ESP_LOGE(TAG, "'%s' is not a readable file on a mounted storage", this->storage_path_.c_str());
    return nullptr;
  }
  *size = st.size;
  return ps;
}

void Esp32HostedUpdate::close_stream_() {
  if (this->stream_open_) {
    storage::global_storage_worker->end_read(this->stream_, [](storage::StorageError) {});
    this->stream_open_ = false;
  }
}

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
void Esp32HostedUpdate::storage_check_() {
  static constexpr uint64_t MANIFEST_MAX = 16384;
  if (this->stream_open_)
    return;
  this->storage_path_ = this->source_url_;
  const char *rel = nullptr;
  uint64_t size = 0;
  storage::PathStorage *ps = this->open_storage_(&rel, &size);
  if (ps == nullptr || size > MANIFEST_MAX) {
    this->status_set_error(LOG_STR("Failed to read manifest"));
    return;
  }
  this->manifest_json_.assign(static_cast<size_t>(size), '\0');
  const storage::StorageError err =
      storage::global_storage_worker->begin_read(ps, rel, &this->stream_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          this->close_stream_();
          this->status_set_error(LOG_STR("Failed to read manifest"));
          return;
        }
        this->storage_check_read_(0);
      });
  this->stream_open_ = err == storage::StorageError::STORAGE_ERROR_OK;
  if (!this->stream_open_)
    this->status_set_error(LOG_STR("Failed to read manifest"));
}

void Esp32HostedUpdate::storage_check_read_(size_t at) {
  if (at >= this->manifest_json_.size()) {
    this->close_stream_();
    const bool valid = this->parse_manifest_(this->manifest_json_);
    this->manifest_json_.clear();
    this->manifest_json_.shrink_to_fit();
    if (!valid) {
      ESP_LOGE(TAG, "Failed to parse manifest JSON");
      this->status_set_error(LOG_STR("Failed to parse manifest"));
      return;
    }
    this->finish_check_();
    return;
  }
  this->got_ = 0;
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->stream_, reinterpret_cast<uint8_t *>(&this->manifest_json_[at]), this->manifest_json_.size() - at,
      &this->got_, [this, at](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->got_ == 0) {
          this->close_stream_();
          this->status_set_error(LOG_STR("Failed to read manifest"));
          return;
        }
        this->storage_check_read_(at + this->got_);
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->close_stream_();
    this->status_set_error(LOG_STR("Failed to read manifest"));
  }
}
#endif

void Esp32HostedUpdate::storage_flash_(update::UpdateState prev_state) {
  this->prev_state_ = prev_state;
  ESP_LOGI(TAG, "Reading firmware from '%s'", this->storage_path_.c_str());
  const char *rel = nullptr;
  storage::PathStorage *ps = this->stream_open_ ? nullptr : this->open_storage_(&rel, &this->total_);
  if (ps == nullptr) {
    this->status_set_error(LOG_STR("Firmware file not readable"));
    this->finish_install_(prev_state, false);
    return;
  }
  this->chunk_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(CHUNK_SIZE);
  if (this->chunk_ == nullptr) {
    this->status_set_error(LOG_STR("No memory"));
    this->finish_install_(prev_state, false);
    return;
  }
  esp_err_t err;
  {
    watchdog::WatchdogManager wdt(OTA_WDT_TIMEOUT_MS);
    err = esp_hosted_slave_ota_begin();  // NOLINT
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->chunk_, CHUNK_SIZE);
    this->chunk_ = nullptr;
    this->status_set_error(LOG_STR("Failed to begin OTA"));
    this->finish_install_(prev_state, false);
    return;
  }
  this->offset_ = 0;
  this->last_progress_ = millis();
  this->hasher_.init();
  const storage::StorageError serr =
      storage::global_storage_worker->begin_read(ps, rel, &this->stream_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGE(TAG, "Opening '%s' failed (%s)", this->storage_path_.c_str(), storage::error_to_string(e));
          this->storage_flash_done_(false);
          return;
        }
        this->storage_flash_read_();
      });
  this->stream_open_ = serr == storage::StorageError::STORAGE_ERROR_OK;
  if (!this->stream_open_)
    this->storage_flash_done_(false);
}

void Esp32HostedUpdate::storage_flash_read_() {
  if (this->offset_ >= this->total_) {
    this->storage_flash_done_(true);
    return;
  }
  const size_t want = static_cast<size_t>(std::min<uint64_t>(CHUNK_SIZE, this->total_ - this->offset_));
  this->got_ = 0;
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->stream_, this->chunk_, want, &this->got_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->got_ == 0) {
          ESP_LOGE(TAG, "Read failed at %llu", static_cast<unsigned long long>(this->offset_));
          this->storage_flash_done_(false);
          return;
        }
        this->hasher_.add(this->chunk_, this->got_);
        if (esp_hosted_slave_ota_write(this->chunk_, this->got_) != ESP_OK) {  // NOLINT
          ESP_LOGE(TAG, "Failed to write OTA data");
          this->storage_flash_done_(false);
          return;
        }
        this->offset_ += this->got_;
        const uint32_t now = millis();
        if (now - this->last_progress_ >= 500) {
          this->last_progress_ = now;
          this->update_info_.has_progress = true;
          this->update_info_.progress = this->offset_ * 100.0f / this->total_;
          this->publish_state();
        }
        this->storage_flash_read_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->storage_flash_done_(false);
}

void Esp32HostedUpdate::storage_flash_done_(bool ok) {
  this->close_stream_();
  if (this->chunk_ != nullptr) {
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->chunk_, CHUNK_SIZE);
    this->chunk_ = nullptr;
  }
  if (ok) {
    this->hasher_.calculate();
    ok = this->hasher_.equals_bytes(this->firmware_sha256_.data());
    if (!ok)
      ESP_LOGE(TAG, "SHA256 mismatch");
  }
  if (!ok) {
    esp_hosted_slave_ota_end();  // NOLINT
    this->status_set_error(LOG_STR("Firmware read failed or SHA256 mismatch"));
  } else {
    ESP_LOGI(TAG, "SHA256 verified successfully");
  }
  this->finish_install_(this->prev_state_, ok);
}
#endif
#if !defined(USE_ESP32_HOSTED_HTTP_UPDATE) && !defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
bool Esp32HostedUpdate::write_embedded_firmware_to_coprocessor_() {
  if (this->firmware_data_ == nullptr || this->firmware_size_ == 0) {
    ESP_LOGE(TAG, "No firmware data available");
    this->status_set_error(LOG_STR("No firmware data available"));
    return false;
  }

  // Verify SHA256 before writing
  sha256::SHA256 hasher;
  hasher.init();
  hasher.add(this->firmware_data_, this->firmware_size_);
  hasher.calculate();
  if (!hasher.equals_bytes(this->firmware_sha256_.data())) {
    ESP_LOGE(TAG, "SHA256 mismatch");
    this->status_set_error(LOG_STR("SHA256 verification failed"));
    return false;
  }

  ESP_LOGI(TAG, "Starting OTA update (%zu bytes)", this->firmware_size_);

  esp_err_t err;
  {
    watchdog::WatchdogManager wdt(OTA_WDT_TIMEOUT_MS);
    err = esp_hosted_slave_ota_begin();  // NOLINT
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(err));
    this->status_set_error(LOG_STR("Failed to begin OTA"));
    return false;
  }

  uint8_t chunk[CHUNK_SIZE];
  const uint8_t *data_ptr = this->firmware_data_;
  size_t remaining = this->firmware_size_;
  while (remaining > 0) {
    size_t chunk_size = std::min(remaining, static_cast<size_t>(CHUNK_SIZE));
    memcpy(chunk, data_ptr, chunk_size);
    err = esp_hosted_slave_ota_write(chunk, chunk_size);  // NOLINT
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
      ota_end_guarded();
      this->status_set_error(LOG_STR("Failed to write OTA data"));
      return false;
    }
    data_ptr += chunk_size;
    remaining -= chunk_size;
    App.feed_wdt();
  }

  return true;
}
#endif

void Esp32HostedUpdate::perform(bool force) {
  if (this->state_ != update::UPDATE_STATE_AVAILABLE && !force) {
    ESP_LOGW(TAG, "Update not available");
    return;
  }

#ifdef USE_ESP32_HOSTED_HTTP_UPDATE
  if (this->firmware_url_.empty()) {
    ESP_LOGW(TAG, "No firmware URL available, run check first");
    return;
  }
#endif

  update::UpdateState prev_state = this->state_;
  this->state_ = update::UPDATE_STATE_INSTALLING;
  this->update_info_.has_progress = false;
  this->publish_state();

#if defined(USE_ESP32_HOSTED_HTTP_UPDATE) && defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  if (this->firmware_url_[0] == '/') {
    this->storage_path_ = this->firmware_url_;
    this->storage_flash_(prev_state);
    return;
  }
  this->finish_install_(prev_state, this->stream_firmware_to_coprocessor_());
#elif defined(USE_ESP32_HOSTED_HTTP_UPDATE)
  this->finish_install_(prev_state, this->stream_firmware_to_coprocessor_());
#elif defined(USE_ESP32_HOSTED_STORAGE_UPDATE)
  this->storage_flash_(prev_state);
#else
  this->finish_install_(prev_state, this->write_embedded_firmware_to_coprocessor_());
#endif
}

void Esp32HostedUpdate::finish_install_(update::UpdateState prev_state, bool ok) {
  this->update_info_.has_progress = false;
  if (!ok) {
    this->state_ = prev_state;
    this->publish_state();
    return;
  }

  // End OTA and activate new firmware
  esp_err_t end_err = ota_end_guarded();
  if (end_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(end_err));
    this->state_ = prev_state;
    this->status_set_error(LOG_STR("Failed to end OTA"));
    this->publish_state();
    return;
  }

  esp_err_t activate_err = esp_hosted_slave_ota_activate();  // NOLINT
  if (activate_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to activate OTA: %s", esp_err_to_name(activate_err));
    this->state_ = prev_state;
    this->status_set_error(LOG_STR("Failed to activate OTA"));
    this->publish_state();
    return;
  }

  // Update state
  ESP_LOGI(TAG, "OTA update successful");
  this->state_ = update::UPDATE_STATE_NO_UPDATE;
  this->status_clear_error();
  this->publish_state();

#ifdef USE_OTA_ROLLBACK
  // Mark the host partition as valid before rebooting, in case the safe mode
  // timer hasn't expired yet.
  esp_ota_mark_app_valid_cancel_rollback();
#endif

  // Schedule a restart to ensure everything is in sync
  ESP_LOGI(TAG, "Restarting in 1 second");
  this->set_timeout(1000, []() { App.safe_reboot(); });
}

}  // namespace esphome::esp32_hosted
#endif
