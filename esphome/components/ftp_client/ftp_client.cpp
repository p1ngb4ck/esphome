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

//========================================================================
// FTP Packet Implementation
//========================================================================

FTPPacket FTPPacket::create_rrq(const std::string &filename, FTPMode mode) {
  FTPPacket packet;
  packet.opcode = FTP_OPCODE_RRQ;

  // Filename (null-terminated)
  packet.data.insert(packet.data.end(), filename.begin(), filename.end());
  packet.data.push_back(0);

  // Mode string (null-terminated)
  const char *mode_str = (mode == FTP_MODE_OCTET) ? "octet" : "netascii";
  packet.data.insert(packet.data.end(), mode_str, mode_str + strlen(mode_str));
  packet.data.push_back(0);

  return packet;
}

FTPPacket FTPPacket::create_wrq(const std::string &filename, FTPMode mode) {
  FTPPacket packet;
  packet.opcode = FTP_OPCODE_WRQ;

  // Filename (null-terminated)
  packet.data.insert(packet.data.end(), filename.begin(), filename.end());
  packet.data.push_back(0);

  // Mode string (null-terminated)
  const char *mode_str = (mode == FTP_MODE_OCTET) ? "octet" : "netascii";
  packet.data.insert(packet.data.end(), mode_str, mode_str + strlen(mode_str));
  packet.data.push_back(0);

  return packet;
}

FTPPacket FTPPacket::create_data(uint16_t block_number, const uint8_t *data, size_t length) {
  FTPPacket packet;
  packet.opcode = FTP_OPCODE_DATA;

  // Block number (big-endian)
  packet.data.push_back((block_number >> 8) & 0xFF);
  packet.data.push_back(block_number & 0xFF);

  // Data
  packet.data.insert(packet.data.end(), data, data + length);

  return packet;
}

FTPPacket FTPPacket::create_ack(uint16_t block_number) {
  FTPPacket packet;
  packet.opcode = FTP_OPCODE_ACK;

  // Block number (big-endian)
  packet.data.push_back((block_number >> 8) & 0xFF);
  packet.data.push_back(block_number & 0xFF);

  return packet;
}

FTPPacket FTPPacket::create_error(FTPErrorCode error_code, const std::string &error_msg) {
  FTPPacket packet;
  packet.opcode = FTP_OPCODE_ERROR;

  // Error code (big-endian)
  packet.data.push_back((error_code >> 8) & 0xFF);
  packet.data.push_back(error_code & 0xFF);

  // Error message (null-terminated)
  packet.data.insert(packet.data.end(), error_msg.begin(), error_msg.end());
  packet.data.push_back(0);

  return packet;
}

std::vector<uint8_t> FTPPacket::serialize() const {
  std::vector<uint8_t> buffer;

  // Opcode (big-endian)
  buffer.push_back((this->opcode >> 8) & 0xFF);
  buffer.push_back(this->opcode & 0xFF);

  // Data
  buffer.insert(buffer.end(), this->data.begin(), this->data.end());

  return buffer;
}

FTPPacket FTPPacket::parse(const uint8_t *buffer, size_t length) {
  FTPPacket packet;

  if (length < 2) {
    ESP_LOGE(TAG, "FTP packet too short: %zu bytes", length);
    packet.opcode = 0;
    return packet;
  }

  // Parse opcode (big-endian)
  packet.opcode = (buffer[0] << 8) | buffer[1];

  // Parse data
  if (length > 2) {
    packet.data.assign(buffer + 2, buffer + length);
  }

  return packet;
}

uint16_t FTPPacket::get_block_number() const {
  if (this->data.size() < 2) {
    return 0;
  }
  return (this->data[0] << 8) | this->data[1];
}

FTPErrorCode FTPPacket::get_error_code() const {
  if (this->data.size() < 2) {
    return FTP_ERROR_NOT_DEFINED;
  }
  return static_cast<FTPErrorCode>((this->data[0] << 8) | this->data[1]);
}

std::string FTPPacket::get_error_message() const {
  if (this->data.size() < 3) {
    return "Unknown error";
  }
  // Error message starts at offset 2, null-terminated
  return std::string(reinterpret_cast<const char *>(&this->data[2]));
}

//========================================================================
// FTPClient Implementation
//========================================================================

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

  this->initialized_ = true;
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

//========================================================================
// Socket Operations
//========================================================================

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

bool FTPClient::send_packet_(const FTPPacket &packet, const std::string &server, uint16_t port) {
  std::vector<uint8_t> buffer = packet.serialize();

  ESP_LOGVV(TAG, "Sending FTP packet: opcode=%u, size=%zu bytes to %s:%u", packet.opcode, buffer.size(), server.c_str(),
            port);

#ifdef USE_ESP_IDF
  // Resolve server address
  struct sockaddr_in dest_addr;
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(port);

  struct hostent *host = gethostbyname(server.c_str());
  if (host == nullptr) {
    ESP_LOGE(TAG, "Failed to resolve host: %s", server.c_str());
    return false;
  }
  memcpy(&dest_addr.sin_addr, host->h_addr, sizeof(dest_addr.sin_addr));

  // Send packet
  int sent = sendto(this->socket_, buffer.data(), buffer.size(), 0, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
  if (sent < 0) {
    ESP_LOGE(TAG, "Failed to send packet: errno %d", errno);
    return false;
  }

  return true;
#else
  // Arduino WiFiUDP
  if (!this->udp_->beginPacket(server.c_str(), port)) {
    ESP_LOGE(TAG, "Failed to begin UDP packet");
    return false;
  }

  this->udp_->write(buffer.data(), buffer.size());

  if (!this->udp_->endPacket()) {
    ESP_LOGE(TAG, "Failed to send UDP packet");
    return false;
  }

  return true;
#endif
}

bool FTPClient::receive_packet_(FTPPacket &packet, std::string &server, uint16_t &port, uint32_t timeout_ms) {
#ifdef USE_ESP_IDF
  uint8_t buffer[FTP_MAX_PACKET_SIZE];
  struct sockaddr_in src_addr;
  socklen_t src_addr_len = sizeof(src_addr);

  // Set socket timeout
  struct timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(this->socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  // Receive packet
  int received = recvfrom(this->socket_, buffer, sizeof(buffer), 0, (struct sockaddr *) &src_addr, &src_addr_len);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ESP_LOGV(TAG, "Receive timeout");
    } else {
      ESP_LOGW(TAG, "Failed to receive packet: errno %d", errno);
    }
    return false;
  }

  // Parse packet
  packet = FTPPacket::parse(buffer, received);

  // Get source address and port
  char addr_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &src_addr.sin_addr, addr_str, sizeof(addr_str));
  server = addr_str;
  port = ntohs(src_addr.sin_port);

  ESP_LOGVV(TAG, "Received FTP packet: opcode=%u, size=%d bytes from %s:%u", packet.opcode, received, server.c_str(),
            port);

  return true;
#else
  // Arduino WiFiUDP
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    int packet_size = this->udp_->parsePacket();
    if (packet_size > 0) {
      uint8_t buffer[FTP_MAX_PACKET_SIZE];
      int received = this->udp_->read(buffer, sizeof(buffer));

      if (received > 0) {
        packet = FTPPacket::parse(buffer, received);
        server = this->udp_->remoteIP().toString().c_str();
        port = this->udp_->remotePort();

        ESP_LOGVV(TAG, "Received FTP packet: opcode=%u, size=%d bytes from %s:%u", packet.opcode, received,
                  server.c_str(), port);

        return true;
      }
    }
    delay(10);
  }

  ESP_LOGV(TAG, "Receive timeout");
  return false;
#endif
}

//========================================================================
// FTP File Operations
//========================================================================

FTPResult FTPClient::read_file(const std::string &filename) {
  if (!this->initialized_) {
    return FTPResult::error("FTP client not initialized");
  }

  ESP_LOGI(TAG, "Reading file from FTP server: %s", filename.c_str());

  // Perform read with retries
  for (uint8_t retry = 0; retry < FTP_MAX_RETRIES; retry++) {
    if (retry > 0) {
      ESP_LOGW(TAG, "Retry %u/%u for file: %s", retry, FTP_MAX_RETRIES, filename.c_str());
    }

    FTPResult result = this->read_file_internal_(filename);
    if (result.success) {
      ESP_LOGI(TAG, "Successfully read file: %s (%zu bytes)", filename.c_str(), result.bytes_transferred);
      return result;
    }

    if (retry < FTP_MAX_RETRIES - 1) {
      delay(500);  // Wait before retry
    }
  }

  return FTPResult::error("Failed after " + std::to_string(FTP_MAX_RETRIES) + " retries");
}

FTPResult FTPClient::write_file(const std::string &filename, const uint8_t *data, size_t length) {
  if (!this->initialized_) {
    return FTPResult::error("FTP client not initialized");
  }

  ESP_LOGI(TAG, "Writing file to FTP server: %s (%zu bytes)", filename.c_str(), length);

  // Perform write with retries
  for (uint8_t retry = 0; retry < FTP_MAX_RETRIES; retry++) {
    if (retry > 0) {
      ESP_LOGW(TAG, "Retry %u/%u for file: %s", retry, FTP_MAX_RETRIES, filename.c_str());
    }

    FTPResult result = this->write_file_internal_(filename, data, length);
    if (result.success) {
      ESP_LOGI(TAG, "Successfully wrote file: %s (%zu bytes)", filename.c_str(), result.bytes_transferred);
      return result;
    }

    if (retry < FTP_MAX_RETRIES - 1) {
      delay(500);  // Wait before retry
    }
  }

  return FTPResult::error("Failed after " + std::to_string(FTP_MAX_RETRIES) + " retries");
}

FTPResult FTPClient::read_file_internal_(const std::string &filename) {
  // Send RRQ (Read Request)
  FTPPacket rrq = FTPPacket::create_rrq(filename, FTP_MODE_OCTET);
  if (!this->send_packet_(rrq, this->server_, this->port_)) {
    return FTPResult::error("Failed to send RRQ");
  }

  FTPResult result = FTPResult::ok();
  uint16_t expected_block = 1;
  std::string server_addr;
  uint16_t server_port;

  // Receive data blocks
  while (true) {
    FTPPacket response;
    if (!this->receive_packet_(response, server_addr, server_port, FTP_TIMEOUT_MS)) {
      return FTPResult::error("Timeout waiting for DATA packet");
    }

    // Handle ERROR packet
    if (response.opcode == FTP_OPCODE_ERROR) {
      std::string error_msg = response.get_error_message();
      ESP_LOGE(TAG, "FTP error: %s", error_msg.c_str());
      return FTPResult::error("Server error: " + error_msg);
    }

    // Validate DATA packet
    if (response.opcode != FTP_OPCODE_DATA) {
      return FTPResult::error("Unexpected opcode: " + std::to_string(response.opcode));
    }

    // Get block number
    uint16_t block_number = response.get_block_number();
    if (block_number != expected_block) {
      ESP_LOGW(TAG, "Unexpected block number: got %u, expected %u", block_number, expected_block);
    }

    // Extract data (skip 2-byte block number)
    if (response.data.size() > 2) {
      const uint8_t *block_data = response.data.data() + 2;
      size_t block_size = response.data.size() - 2;
      result.data.insert(result.data.end(), block_data, block_data + block_size);
      result.bytes_transferred += block_size;
    }

    // Send ACK
    FTPPacket ack = FTPPacket::create_ack(block_number);
    if (!this->send_packet_(ack, server_addr, server_port)) {
      return FTPResult::error("Failed to send ACK");
    }

    // Check if this is the last block (less than 512 bytes)
    size_t data_size = response.data.size() - 2;
    if (data_size < FTP_DATA_SIZE) {
      ESP_LOGD(TAG, "Received last block (%zu bytes)", data_size);
      break;
    }

    expected_block++;
  }

  result.success = true;
  return result;
}

FTPResult FTPClient::write_file_internal_(const std::string &filename, const uint8_t *data, size_t length) {
  // Send WRQ (Write Request)
  FTPPacket wrq = FTPPacket::create_wrq(filename, FTP_MODE_OCTET);
  if (!this->send_packet_(wrq, this->server_, this->port_)) {
    return FTPResult::error("Failed to send WRQ");
  }

  std::string server_addr;
  uint16_t server_port;

  // Wait for ACK for block 0
  FTPPacket response;
  if (!this->receive_packet_(response, server_addr, server_port, FTP_TIMEOUT_MS)) {
    return FTPResult::error("Timeout waiting for ACK");
  }

  // Handle ERROR packet
  if (response.opcode == FTP_OPCODE_ERROR) {
    std::string error_msg = response.get_error_message();
    ESP_LOGE(TAG, "FTP error: %s", error_msg.c_str());
    return FTPResult::error("Server error: " + error_msg);
  }

  // Validate ACK packet
  if (response.opcode != FTP_OPCODE_ACK) {
    return FTPResult::error("Unexpected opcode: " + std::to_string(response.opcode));
  }

  // Send data blocks
  uint16_t block_number = 1;
  size_t offset = 0;

  while (offset < length) {
    // Calculate block size
    size_t block_size = std::min(FTP_DATA_SIZE, length - offset);

    // Create DATA packet
    FTPPacket data_packet = FTPPacket::create_data(block_number, data + offset, block_size);
    if (!this->send_packet_(data_packet, server_addr, server_port)) {
      return FTPResult::error("Failed to send DATA packet");
    }

    // Wait for ACK
    if (!this->receive_packet_(response, server_addr, server_port, FTP_TIMEOUT_MS)) {
      return FTPResult::error("Timeout waiting for ACK");
    }

    // Handle ERROR packet
    if (response.opcode == FTP_OPCODE_ERROR) {
      std::string error_msg = response.get_error_message();
      ESP_LOGE(TAG, "FTP error: %s", error_msg.c_str());
      return FTPResult::error("Server error: " + error_msg);
    }

    // Validate ACK packet
    if (response.opcode != FTP_OPCODE_ACK) {
      return FTPResult::error("Unexpected opcode: " + std::to_string(response.opcode));
    }

    // Validate block number
    uint16_t ack_block = response.get_block_number();
    if (ack_block != block_number) {
      return FTPResult::error("Unexpected ACK block: " + std::to_string(ack_block));
    }

    offset += block_size;
    block_number++;
  }

  // Send final block if needed (when file size is exact multiple of 512)
  if (length % FTP_DATA_SIZE == 0) {
    FTPPacket final_packet = FTPPacket::create_data(block_number, nullptr, 0);
    if (!this->send_packet_(final_packet, server_addr, server_port)) {
      return FTPResult::error("Failed to send final DATA packet");
    }

    // Wait for final ACK
    if (!this->receive_packet_(response, server_addr, server_port, FTP_TIMEOUT_MS)) {
      return FTPResult::error("Timeout waiting for final ACK");
    }
  }

  return FTPResult::ok(length);
}

bool FTPClient::is_server_reachable() {
  if (!this->initialized_) {
    return false;
  }

  // Try to read a dummy file to check connectivity
  // Most FTP servers will respond with "file not found" error
  FTPPacket rrq = FTPPacket::create_rrq(".esphome_test", FTP_MODE_OCTET);
  if (!this->send_packet_(rrq, this->server_, this->port_)) {
    return false;
  }

  std::string server_addr;
  uint16_t server_port;
  FTPPacket response;

  // Wait for any response (DATA or ERROR)
  if (!this->receive_packet_(response, server_addr, server_port, FTP_TIMEOUT_MS)) {
    return false;
  }

  // Any response means server is reachable
  return true;
}

}  // namespace ftp_client
}  // namespace esphome
