#include "ftp_client.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cstring>
#include <algorithm>

// Forward declare storage for soft dependency
#if defined(USE_STORAGE)
namespace storage {
extern class StorageHost *global_storage;
}
#endif  // USE_STORAGE

namespace esphome {
namespace ftp_client {

FTPClient::~FTPClient() { this->close_socket_(); }

void FTPClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up FTP Client...");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);

  if (!this->mount_path_.empty()) {
    ESP_LOGCONFIG(TAG, "  Mount Path: %s", this->mount_path_.c_str());
  }

  // Initialize socket
  if (!this->init_socket_()) {
    ESP_LOGE(TAG, "Failed to initialize UDP socket");
    this->mark_failed();
    return;
  }

  // Register with storage if configured
  if (!this->mount_path_.empty()) {
    this->register_with_storage();
  }
}

void FTPClient::loop() {
  // Nothing to do in loop for now
}

void FTPClient::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP Client:");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  if (!this->mount_path_.empty()) {
    ESP_LOGCONFIG(TAG, "  Mount Path: %s", this->mount_path_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Status: %s", this->initialized_ ? "Ready" : "Failed");
}

void FTPClient::register_with_storage() {
#if defined(USE_STORAGE)
  // Check if storage is available (soft dependency)
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_mount(this->mount_path_, "ftp");
    ESP_LOGI(TAG, "Registered FTP mount with storage: %s", this->mount_path_.c_str());
  } else {
    ESP_LOGD(TAG, "storage not available, skipping mount registration");
  }
#else
  ESP_LOGD(TAG, "storage component not compiled, mount registration disabled");
#endif  // USE_STORAGE
}

bool FTPClient::init_socket_() {
#ifdef USE_ESP_IDF
  // Create UDP socket
  this->socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
    return false;
  }

  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = FTP_TIMEOUT_MS / 1000;
  timeout.tv_usec = (FTP_TIMEOUT_MS % 1000) * 1000;
  if (setsockopt(this->socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    ESP_LOGW(TAG, "Failed to set socket timeout: errno %d", errno);
  }

  ESP_LOGD(TAG, "UDP socket initialized (fd=%d)", this->socket_);
  return true;
#else
  // Arduino WiFiUDP
  this->udp_ = std::make_unique<WiFiUDP>();
  if (!this->udp_->begin(0)) {  // Bind to random port
    ESP_LOGE(TAG, "Failed to initialize WiFiUDP");
    return false;
  }

  ESP_LOGD(TAG, "WiFiUDP initialized");
  return true;
#endif
}

void FTPClient::close_socket_() {
#ifdef USE_ESP_IDF
  if (this->socket_ >= 0) {
    close(this->socket_);
    this->socket_ = -1;
  }
#else
  if (this->udp_) {
    this->udp_->stop();
    this->udp_ = nullptr;
  }
#endif
}

}  // namespace ftp_client
}  // namespace esphome
