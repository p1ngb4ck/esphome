#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/network/ip_address.h"
// Optional storage integration (soft dependency)
#if defined(USE_STORAGE)
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/network_storage.h"
#endif

#ifdef USE_ESP_IDF
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#else
#include <WiFiUdp.h>
#endif

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace esphome {
namespace ftp_client {

static const char *const TAG = "ftp_client";

static constexpr size_t FTP_DATA_SIZE = 512;

/// Maximum FTP packet size (2 byte opcode + 2 byte block + 512 data)
static constexpr size_t FTP_MAX_PACKET_SIZE = 516;

/// FTP timeout in milliseconds
static constexpr uint32_t FTP_TIMEOUT_MS = 5000;

/// Maximum retries for FTP operations
static constexpr uint8_t FTP_MAX_RETRIES = 3;

/// FTP default port
static constexpr uint16_t FTP_DEFAULT_PORT = 21;

/// FTP Opcodes
enum FTPOpcode : uint16_t {
  FTP_OPCODE_RRQ = 1,    ///< Read request
  FTP_OPCODE_WRQ = 2,    ///< Write request
  FTP_OPCODE_DATA = 3,   ///< Data packet
  FTP_OPCODE_ACK = 4,    ///< Acknowledgment
  FTP_OPCODE_ERROR = 5,  ///< Error packet
};

/// FTP Error Codes
enum FTPErrorCode : uint16_t {
  FTP_ERROR_NOT_DEFINED = 0,          ///< Not defined
  FTP_ERROR_FILE_NOT_FOUND = 1,       ///< File not found
  FTP_ERROR_ACCESS_VIOLATION = 2,     ///< Access violation
  FTP_ERROR_DISK_FULL = 3,            ///< Disk full
  FTP_ERROR_ILLEGAL_OPERATION = 4,    ///< Illegal FTP operation
  FTP_ERROR_UNKNOWN_TRANSFER_ID = 5,  ///< Unknown transfer ID
  FTP_ERROR_FILE_EXISTS = 6,          ///< File already exists
  FTP_ERROR_NO_SUCH_USER = 7,         ///< No such user
};

/// FTP Transfer Mode
enum FTPMode {
  FTP_MODE_OCTET,    ///< Binary mode (octet)
  FTP_MODE_NETASCII  ///< ASCII mode (netascii)
};

//========================================================================
// FTP Packet Structures
//========================================================================

/**
 * @brief FTP packet structure
 *
 * Handles encoding/decoding of FTP protocol packets according to RFC 1350
 */
struct FTPPacket {
  uint16_t opcode;
  std::vector<uint8_t> data;

  /**
   * @brief Create RRQ (Read Request) packet
   */
  static FTPPacket create_rrq(const std::string &filename, FTPMode mode = FTP_MODE_OCTET);

  /**
   * @brief Create WRQ (Write Request) packet
   */
  static FTPPacket create_wrq(const std::string &filename, FTPMode mode = FTP_MODE_OCTET);

  /**
   * @brief Create DATA packet
   */
  static FTPPacket create_data(uint16_t block_number, const uint8_t *data, size_t length);

  /**
   * @brief Create ACK packet
   */
  static FTPPacket create_ack(uint16_t block_number);

  /**
   * @brief Create ERROR packet
   */
  static FTPPacket create_error(FTPErrorCode error_code, const std::string &error_msg);

  /**
   * @brief Serialize packet to bytes
   */
  std::vector<uint8_t> serialize() const;

  /**
   * @brief Parse packet from bytes
   */
  static FTPPacket parse(const uint8_t *buffer, size_t length);

  /**
   * @brief Get block number from DATA or ACK packet
   */
  uint16_t get_block_number() const;

  /**
   * @brief Get error code from ERROR packet
   */
  FTPErrorCode get_error_code() const;

  /**
   * @brief Get error message from ERROR packet
   */
  std::string get_error_message() const;
};

//========================================================================
// FTP File Operations
//========================================================================

/**
 * @brief FTP file operation result
 */
struct FTPResult {
  bool success;
  std::string error_message;
  std::vector<uint8_t> data;  ///< For read operations
  size_t bytes_transferred;

  FTPResult() : success(false), bytes_transferred(0) {}
  static FTPResult ok(size_t bytes = 0) {
    FTPResult result;
    result.success = true;
    result.bytes_transferred = bytes;
    return result;
  }
  static FTPResult error(const std::string &msg) {
    FTPResult result;
    result.success = false;
    result.error_message = msg;
    return result;
  }
};

//========================================================================
// FTP Client Component
//========================================================================

/**
 * @brief FTP client for network file access
 *
 * Implements FTP protocol (RFC 959) for reading and writing files
 * to/from a remote FTP server. Integrates with storage to provide
 * a virtual mount point for FTP file access.
 *
 * Features:
 * - Read files from FTP server
 * - Write files to FTP server
 * - Configurable server address and port
 * - Automatic retry on timeout
 * - Storage host integration (soft dependency)
 *
 * Example configuration:
 * @code
 * ftp_client:
 *   - id: my_ftp
 *     server: 192.168.1.100
 *     port: 21  # Optional, default 21
 *     mount_path: /ftp  # Optional, for storage integration
 * @endcode
 */
class FTPClient : public Component {
 public:
  FTPClient() = default;
  ~FTPClient();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  //========================================================================
  // Configuration
  //========================================================================

  /**
   * @brief Set FTP server address
   */
  void set_server(const std::string &server) { this->server_ = server; }

  /**
   * @brief Set FTP server port
   */
  void set_port(uint16_t port) { this->port_ = port; }

  /**
   * @brief Set FTP username
   */
  void set_username(const std::string &username) { this->username_ = username; }

  /**
   * @brief Set FTP password
   */
  void set_password(const std::string &password) { this->password_ = password; }

  /**
   * @brief Set mount path for storage integration
   */
  void set_mount_path(const std::string &path) { this->mount_path_ = path; }

  /**
   * @brief Get mount path
   */
  const std::string &get_mount_path() const { return this->mount_path_; }

  //========================================================================
  // Storage Host Integration (soft dependency)
  //========================================================================

  /**
   * @brief Register this FTP client with storage
   *
   * Creates a virtual mount point for FTP file access.
   * Only works if storage component is present (soft dependency).
   */
  void register_with_storage();

  //========================================================================
  // FTP File Operations
  //========================================================================

  /**
   * @brief Read file from FTP server
   *
   * @param filename Remote filename
   * @return FTPResult with file data or error
   */
  FTPResult read_file(const std::string &filename);

  /**
   * @brief Write file to FTP server
   *
   * @param filename Remote filename
   * @param data File data to write
   * @param length Length of data
   * @return FTPResult with success status
   */
  FTPResult write_file(const std::string &filename, const uint8_t *data, size_t length);

  /**
   * @brief Check if server is reachable
   *
   * @return true if server responds to FTP requests
   */
  bool is_server_reachable();

 protected:
  //========================================================================
  // Configuration
  //========================================================================

  std::string server_;
  uint16_t port_{FTP_DEFAULT_PORT};
  std::string mount_path_;
  std::string username_;
  std::string password_;

  //========================================================================
  // Network State
  //========================================================================

#ifdef USE_ESP_IDF
  int socket_{-1};
#else
  std::unique_ptr<WiFiUDP> udp_;
#endif

  bool initialized_{false};

  //========================================================================
  // Internal FTP Operations
  //========================================================================

  /**
   * @brief Initialize UDP socket
   */
  bool init_socket_();

  /**
   * @brief Close UDP socket
   */
  void close_socket_();

  /**
   * @brief Send FTP packet
   */
  bool send_packet_(const FTPPacket &packet, const std::string &server, uint16_t port);

  /**
   * @brief Receive FTP packet with timeout
   */
  bool receive_packet_(FTPPacket &packet, std::string &server, uint16_t &port, uint32_t timeout_ms = FTP_TIMEOUT_MS);

  /**
   * @brief Perform FTP read operation
   */
  FTPResult read_file_internal_(const std::string &filename);

  /**
   * @brief Perform FTP write operation
   */
  FTPResult write_file_internal_(const std::string &filename, const uint8_t *data, size_t length);
};

}  // namespace ftp_client
}  // namespace esphome
