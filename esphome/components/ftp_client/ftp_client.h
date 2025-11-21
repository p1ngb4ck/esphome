#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/network/ip_address.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

#if defined(USE_STORAGE)
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/network_storage.h"
#endif

namespace esphome {
namespace ftp_client {

static const char *const TAG = "ftp_client";

static constexpr size_t FTP_DATA_SIZE = 512;

static constexpr size_t FTP_MAX_PACKET_SIZE = 516;
static constexpr uint32_t FTP_TIMEOUT_MS = 5000;
static constexpr uint8_t FTP_MAX_RETRIES = 3;
static constexpr uint16_t FTP_DEFAULT_PORT = 21;

enum FTPMode { FTP_MODE_OCTET, FTP_MODE_NETASCII };

class FTPClient : public Component {
 public:
  FTPClient() = default;
  ~FTPClient();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_server(const std::string &server) { this->server_ = server; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_username(const std::string &username) { this->username_ = username; }
  void set_password(const std::string &password) { this->password_ = password; }
  void set_mount_path(const std::string &path) { this->mount_path_ = path; }
  const std::string &get_mount_path() const { return this->mount_path_; }
  void register_with_storage();

 protected:
  std::string server_;
  uint16_t port_{FTP_DEFAULT_PORT};
  std::string mount_path_;
  std::string username_;
  std::string password_;

#ifdef USE_ESP_IDF
  int socket_{-1};
#else
  std::unique_ptr<WiFiUDP> udp_;
#endif

  bool initialized_{false};
  bool init_socket_();
  void close_socket_();
};

}  // namespace ftp_client
}  // namespace esphome
