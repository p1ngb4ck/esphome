#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/components/network/ip_address.h"
#include <string>
#include <vector>
#include <memory>
#include <ctime>

#ifdef USE_ESP_IDF
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#endif

#if defined(USE_STORAGE)
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/network_storage.h"
#endif

namespace esphome {
namespace ftp_client {

static const char *const TAG = "ftp_client";

static constexpr size_t FTP_BUF_SIZE = 2048;       // Buffer for FTP responses
static constexpr size_t FTP_DATA_BUF_SIZE = 8192;  // Buffer for data transfer
static constexpr uint32_t FTP_TIMEOUT_MS = 10000;  // 10 second timeout
static constexpr uint16_t FTP_DEFAULT_PORT = 21;

// FTP response codes
static constexpr int FTP_CODE_SERVICE_READY = 220;
static constexpr int FTP_CODE_USER_OK = 331;
static constexpr int FTP_CODE_LOGIN_OK = 230;
static constexpr int FTP_CODE_PASSIVE_MODE = 227;
static constexpr int FTP_CODE_FILE_OK = 150;
static constexpr int FTP_CODE_TRANSFER_OK = 226;
static constexpr int FTP_CODE_FILE_ACTION_OK = 250;
static constexpr int FTP_CODE_PATHNAME_CREATED = 257;

struct FileEntry {
  std::string name;
  bool is_directory;
  size_t size;
  time_t modified;
};

class FTPClient : public Component
#if defined(USE_STORAGE)
    ,
                  public storage::NetworkStorage
#endif
{
 public:
  FTPClient() = default;
  ~FTPClient() override;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Configuration
  void set_server(const std::string &server) { this->server_ = server; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_username(const std::string &username) { this->username_ = username; }
  void set_password(const std::string &password) { this->password_ = password; }
  void set_mount_path(const std::string &path) { this->mount_path_ = path; }
  const std::string &get_mount_path() const { return this->mount_path_; }

#if defined(USE_STORAGE)
  // NetworkStorage interface implementation
  const char *get_protocol() const override { return "ftp"; }
  bool is_mounted() const override { return this->connected_; }
  bool list_directory(const std::string &path, std::vector<storage::DirectoryEntry> &entries) override;
  bool read_file(const std::string &path, std::vector<uint8_t> &data) override;
  bool write_file(const std::string &path, const uint8_t *data, size_t length) override;
  bool delete_file(const std::string &path) override;
  bool delete_directory(const std::string &path) override;
  bool create_directory(const std::string &path) override;
  bool file_exists(const std::string &path) override;
  bool get_file_info(const std::string &path, size_t &size, time_t &modified) override;
#endif

 protected:
  std::string server_;
  uint16_t port_{FTP_DEFAULT_PORT};
  std::string mount_path_;
  std::string username_;
  std::string password_;

#ifdef USE_ESP_IDF
  int control_socket_{-1};  // FTP control connection
  int data_socket_{-1};     // FTP data connection
#endif

  bool connected_{false};
  bool initialized_{false};
  std::vector<uint8_t> response_buffer_;
  std::vector<uint8_t> data_buffer_;

  // Connection management
  bool connect_();
  void disconnect_();
  bool send_command_(const std::string &command);
  int read_response_(std::string &response);
  bool enter_passive_mode_(std::string &ip, uint16_t &port);
  bool open_data_connection_(const std::string &ip, uint16_t port);
  void close_data_connection_();

  // Helper methods
  bool read_data_until_close_(std::vector<uint8_t> &data);
  bool write_data_(const uint8_t *data, size_t length);
  bool parse_list_line_(const std::string &line, FileEntry &entry);
};

}  // namespace ftp_client
}  // namespace esphome
