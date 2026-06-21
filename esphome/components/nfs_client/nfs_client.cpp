#include "nfs_client.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cstring>
#include <algorithm>

#ifdef USE_ESP_IDF
#include "mdns.h"
#include "esp_netif.h"
#endif

#if defined(USE_STORAGE)
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/network_storage.h"
#endif  // USE_STORAGE

namespace esphome {
namespace nfs_client {

//========================================================================
// XDR Buffer Implementation
//========================================================================

void XDRBuffer::encode_uint32(uint32_t value) {
  // Big-endian encoding
  this->data_.push_back((value >> 24) & 0xFF);
  this->data_.push_back((value >> 16) & 0xFF);
  this->data_.push_back((value >> 8) & 0xFF);
  this->data_.push_back(value & 0xFF);
}

void XDRBuffer::encode_uint64(uint64_t value) {
  this->encode_uint32(static_cast<uint32_t>(value >> 32));
  this->encode_uint32(static_cast<uint32_t>(value & 0xFFFFFFFF));
}

void XDRBuffer::encode_bytes(const uint8_t *data, size_t length) {
  this->data_.insert(this->data_.end(), data, data + length);
}

void XDRBuffer::encode_string(const std::string &str) {
  // Length + data + padding
  this->encode_uint32(str.length());
  this->encode_bytes(reinterpret_cast<const uint8_t *>(str.data()), str.length());

  // Add padding to 4-byte boundary
  size_t padding = align_4(str.length()) - str.length();
  for (size_t i = 0; i < padding; i++) {
    this->data_.push_back(0);
  }
}

void XDRBuffer::encode_opaque(const uint8_t *data, size_t length) {
  // Same as string but for binary data
  this->encode_uint32(length);
  this->encode_bytes(data, length);

  // Add padding to 4-byte boundary
  size_t padding = align_4(length) - length;
  for (size_t i = 0; i < padding; i++) {
    this->data_.push_back(0);
  }
}

bool XDRBuffer::decode_uint32(uint32_t &value) {
  if (this->position_ + 4 > this->data_.size()) {
    return false;
  }

  value = (static_cast<uint32_t>(this->data_[this->position_]) << 24) |
          (static_cast<uint32_t>(this->data_[this->position_ + 1]) << 16) |
          (static_cast<uint32_t>(this->data_[this->position_ + 2]) << 8) |
          static_cast<uint32_t>(this->data_[this->position_ + 3]);

  this->position_ += 4;
  return true;
}

bool XDRBuffer::decode_uint64(uint64_t &value) {
  uint32_t high, low;
  if (!this->decode_uint32(high) || !this->decode_uint32(low)) {
    return false;
  }
  value = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

bool XDRBuffer::decode_bytes(uint8_t *data, size_t length) {
  if (this->position_ + length > this->data_.size()) {
    return false;
  }

  memcpy(data, this->data_.data() + this->position_, length);
  this->position_ += length;
  return true;
}

bool XDRBuffer::decode_string(std::string &str) {
  uint32_t length;
  if (!this->decode_uint32(length)) {
    return false;
  }

  if (length > NFS_MAXPATHLEN) {
    return false;
  }

  if (this->position_ + length > this->data_.size()) {
    return false;
  }

  str.assign(reinterpret_cast<const char *>(&this->data_[this->position_]), length);
  this->position_ += align_4(length);

  return true;
}

bool XDRBuffer::decode_opaque(std::vector<uint8_t> &data) {
  uint32_t length;
  if (!this->decode_uint32(length)) {
    return false;
  }

  // Sanity check: limit to reasonable size (1MB) to prevent excessive memory allocation
  // Note: This is used for both file handles (≤64 bytes) and file data (up to FILE_BUFFER_SIZE)
  if (length > 1024 * 1024) {
    return false;
  }

  if (this->position_ + length > this->data_.size()) {
    return false;
  }

  data.assign(this->data_.begin() + this->position_, this->data_.begin() + this->position_ + length);
  this->position_ += align_4(length);

  return true;
}

bool XDRBuffer::decode_opaque_to_buffer(uint8_t *buffer, size_t max_len, size_t &actual_len) {
  uint32_t length;
  if (!this->decode_uint32(length)) {
    return false;
  }

  // Sanity check
  if (length > 1024 * 1024) {
    return false;
  }

  if (this->position_ + length > this->data_.size()) {
    return false;
  }

  // Check if caller's buffer is large enough
  if (length > max_len) {
    ESP_LOGE("XDRBuffer", "Buffer too small: need %" PRIu32 ", have %" PRIu32, (uint32_t) length, (uint32_t) max_len);
    return false;
  }

  // Copy directly to caller's buffer
  std::memcpy(buffer, this->data_.data() + this->position_, length);
  actual_len = length;
  this->position_ += align_4(length);

  return true;
}

bool XDRBuffer::decode_bool(bool &value) {
  uint32_t val;
  if (!this->decode_uint32(val)) {
    return false;
  }
  value = (val != 0);
  return true;
}

//========================================================================
// NFS Structures Implementation
//========================================================================

void NFSFileHandle::encode(XDRBuffer &xdr) const { xdr.encode_opaque(this->data.data(), this->data.size()); }

bool NFSFileHandle::decode(XDRBuffer &xdr) {
  if (!xdr.decode_opaque(this->data)) {
    return false;
  }
  // File handles must be ≤ NFS_FHSIZE3 (64 bytes) per RFC 1813
  if (this->data.size() > NFS_FHSIZE3) {
    return false;
  }
  return true;
}

bool NFSFileAttr::decode(XDRBuffer &xdr) {
  // fattr3 structure from RFC 1813 section 3.3.5
  // Total: 21 fields, 84 bytes
  uint32_t type_val;
  if (!xdr.decode_uint32(type_val)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at type");
    return false;
  }
  this->type = static_cast<NFSFileType>(type_val);

  if (!xdr.decode_uint32(this->mode)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at mode");
    return false;
  }
  if (!xdr.decode_uint32(this->nlink)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at nlink");
    return false;
  }
  if (!xdr.decode_uint32(this->uid)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at uid");
    return false;
  }
  if (!xdr.decode_uint32(this->gid)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at gid");
    return false;
  }
  if (!xdr.decode_uint64(this->size)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at size");
    return false;
  }
  if (!xdr.decode_uint64(this->used)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at used");
    return false;
  }
  // Skip rdev/specinfo (8 bytes) - only meaningful for device files
  uint64_t rdev;
  if (!xdr.decode_uint64(rdev)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at rdev");
    return false;
  }
  if (!xdr.decode_uint64(this->fsid)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at fsid, position=%" PRIu32 ", size=%" PRIu32, (uint32_t) xdr.position(), (uint32_t) xdr.size());
    return false;
  }
  if (!xdr.decode_uint64(this->fileid)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at fileid");
    return false;
  }
  // nfstime3 fields: seconds and nseconds are both uint32 (RFC 1813 section 2.2)
  uint32_t atime_sec_32, mtime_sec_32, ctime_sec_32;

  if (!xdr.decode_uint32(atime_sec_32)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at atime_sec");
    return false;
  }
  this->atime_sec = atime_sec_32;

  if (!xdr.decode_uint32(this->atime_nsec)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at atime_nsec");
    return false;
  }

  if (!xdr.decode_uint32(mtime_sec_32)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at mtime_sec");
    return false;
  }
  this->mtime_sec = mtime_sec_32;

  if (!xdr.decode_uint32(this->mtime_nsec)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at mtime_nsec");
    return false;
  }

  if (!xdr.decode_uint32(ctime_sec_32)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at ctime_sec");
    return false;
  }
  this->ctime_sec = ctime_sec_32;

  if (!xdr.decode_uint32(this->ctime_nsec)) {
    ESP_LOGW(TAG, "NFSFileAttr::decode failed at ctime_nsec, position=%" PRIu32 ", size=%" PRIu32, (uint32_t) xdr.position(), (uint32_t) xdr.size());
    return false;
  }

  return true;
}

//========================================================================
// RPC Client Implementation
//========================================================================

void RPCClient::build_call(XDRBuffer &xdr, uint32_t xid, uint32_t program, uint32_t version, uint32_t procedure,
                           uint32_t uid, uint32_t gid) {
  // RPC call header
  xdr.encode_uint32(xid);        // XID
  xdr.encode_uint32(RPC_CALL);   // Message type
  xdr.encode_uint32(2);          // RPC version
  xdr.encode_uint32(program);    // Program
  xdr.encode_uint32(version);    // Version
  xdr.encode_uint32(procedure);  // Procedure

  // Authentication
  if (uid != 0 || gid != 0) {
    this->encode_auth_unix_(xdr, uid, gid);
  } else {
    this->encode_auth_null_(xdr);
  }

  // Verifier (NULL)
  this->encode_auth_null_(xdr);
}

bool RPCClient::parse_reply(XDRBuffer &xdr, uint32_t expected_xid, RPCAcceptStatus &status) {
  uint32_t xid, msg_type, reply_status;

  if (!xdr.decode_uint32(xid)) {
    ESP_LOGE(TAG, "Failed to decode XID, position=%" PRIu32 " size=%" PRIu32, (uint32_t) xdr.position(), (uint32_t) xdr.size());
    return false;
  }
  if (xid != expected_xid) {
    ESP_LOGE(TAG, "XID mismatch: expected %" PRIu32 ", got %" PRIu32, expected_xid, xid);
    return false;
  }

  if (!xdr.decode_uint32(msg_type)) {
    ESP_LOGE(TAG, "Failed to decode msg_type");
    return false;
  }
  if (msg_type != RPC_REPLY) {
    ESP_LOGE(TAG, "Not an RPC reply, got: %" PRIu32, msg_type);
    return false;
  }

  if (!xdr.decode_uint32(reply_status)) {
    ESP_LOGE(TAG, "Failed to decode reply_status");
    return false;
  }

  if (reply_status != RPC_MSG_ACCEPTED) {
    ESP_LOGE(TAG, "RPC message denied, status: %" PRIu32, reply_status);
    return false;
  }

  // Skip verifier (flavor + length + data)
  uint32_t flavor, length;
  if (!xdr.decode_uint32(flavor)) {
    ESP_LOGE(TAG, "Failed to decode verifier flavor");
    return false;
  }
  if (!xdr.decode_uint32(length)) {
    ESP_LOGE(TAG, "Failed to decode verifier length");
    return false;
  }
  xdr.skip(XDRBuffer::align_4(length));  // Skip verifier data

  // Accept status
  uint32_t accept_status;
  if (!xdr.decode_uint32(accept_status)) {
    ESP_LOGE(TAG, "Failed to decode accept_status, position=%" PRIu32 ", size=%" PRIu32, (uint32_t) xdr.position(), (uint32_t) xdr.size());
    return false;
  }

  status = static_cast<RPCAcceptStatus>(accept_status);
  if (status != RPC_SUCCESS) {
    ESP_LOGE(TAG, "RPC not successful, status: %" PRIu32, static_cast<uint32_t>(status));
    return false;
  }
  return true;
}

void RPCClient::encode_auth_unix_(XDRBuffer &xdr, uint32_t uid, uint32_t gid) {
  // AUTH_UNIX credential
  xdr.encode_uint32(RPC_AUTH_UNIX);  // Flavor

  // Build AUTH_UNIX structure
  XDRBuffer auth_data;
  auth_data.encode_uint32(static_cast<uint32_t>(millis() / 1000));  // Timestamp
  auth_data.encode_string("esphome");                               // Machine name
  auth_data.encode_uint32(uid);                                     // UID
  auth_data.encode_uint32(gid);                                     // GID

  // Include primary GID in auxiliary GID list (common NFS client behavior)
  auth_data.encode_uint32(1);    // One auxiliary GID
  auth_data.encode_uint32(gid);  // Primary GID also in aux list

  // Encode AUTH_UNIX data as opaque
  xdr.encode_opaque(auth_data.data().data(), auth_data.size());
}

void RPCClient::encode_auth_null_(XDRBuffer &xdr) {
  xdr.encode_uint32(RPC_AUTH_NULL);  // Flavor
  xdr.encode_uint32(0);              // Length
}

//========================================================================
// NFSClient Implementation
//========================================================================

NFSClient::~NFSClient() {
  this->disconnect_();
  // Free RPC response buffer
  if (this->rpc_response_buffer_ != nullptr) {
    free(this->rpc_response_buffer_);
    this->rpc_response_buffer_ = nullptr;
  }
}

void NFSClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up NFS Client...");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Export: %s", this->export_path_.c_str());
  ESP_LOGCONFIG(TAG, "  UID: %" PRIu32 ", GID: %" PRIu32, this->uid_, this->gid_);

  if (!this->mount_path_.empty()) {
    ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_.c_str());
  }

  // Allocate RPC response buffer (65KB) - prefer PSRAM if available
  // This buffer is reused for all RPC calls to avoid runtime allocations
#if defined(USE_PSRAM) && defined(USE_ESP_IDF)
  this->rpc_response_buffer_ = (uint8_t *) heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->rpc_response_buffer_ != nullptr) {
    ESP_LOGI(TAG, "Allocated 65KB RPC buffer from PSRAM");
  } else {
    ESP_LOGW(TAG, "PSRAM allocation failed, using heap for RPC buffer");
    this->rpc_response_buffer_ = (uint8_t *) malloc(65536);
  }
#else
  this->rpc_response_buffer_ = (uint8_t *) malloc(65536);
  ESP_LOGI(TAG, "Allocated 65KB RPC buffer from heap");
#endif

  if (this->rpc_response_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate RPC response buffer!");
    this->mark_failed();
    return;
  }

  // Don't block in setup() - just set state to start mounting in loop()
  this->mount_state_ = MountState::IDLE;
  ESP_LOGI(TAG, "NFS mount will be attempted asynchronously in loop()");
}

void NFSClient::loop() {
  uint32_t now = millis();

  switch (this->mount_state_) {
    case MountState::IDLE:
      // Start mount attempt - first query portmapper for MOUNT port
      ESP_LOGD(TAG, "Starting NFS mount attempt for %s:%s", this->server_.c_str(), this->export_path_.c_str());
      this->mount_state_ = MountState::CONNECTING_PMAP;
      this->last_mount_attempt_ = now;
      break;

    case MountState::CONNECTING_PMAP:
      // Connect to portmapper (port 111)
      if (this->connect_()) {
        ESP_LOGD(TAG, "Connected to portmapper, querying for MOUNT service...");
        this->mount_state_ = MountState::QUERYING_PMAP_MOUNT;
      } else {
        ESP_LOGW(TAG, "Failed to connect to portmapper, will retry in %" PRIu32 " seconds", this->mount_retry_interval_ / 1000);
        this->mount_state_ = MountState::FAILED;
        this->last_mount_attempt_ = now;
      }
      break;

    case MountState::QUERYING_PMAP_MOUNT:
      // Query portmapper for MOUNT port
      if (this->query_portmapper_(MOUNT_PROGRAM, MOUNT_VERSION_3, this->mount_port_)) {
        this->mount_port_discovered_ = true;
        ESP_LOGI(TAG, "MOUNT service available on port %u", this->mount_port_);
        // Disconnect from portmapper and connect to MOUNT service
        this->close_connection_();
        this->mount_state_ = MountState::CONNECTING_MOUNT;
      } else {
        ESP_LOGW(TAG, "Failed to query portmapper for MOUNT, will retry in %" PRIu32 " seconds",
                 this->mount_retry_interval_ / 1000);
        this->close_connection_();
        this->mount_state_ = MountState::FAILED;
        this->last_mount_attempt_ = now;
      }
      break;

    case MountState::CONNECTING_MOUNT:
      // Connect to MOUNT service on discovered port
      if (this->connect_()) {
        ESP_LOGD(TAG, "Connected to MOUNT service, attempting mount...");
        this->mount_state_ = MountState::MOUNTING;
      } else {
        ESP_LOGW(TAG, "Failed to connect to MOUNT service, will retry in %" PRIu32 " seconds",
                 this->mount_retry_interval_ / 1000);
        this->mount_state_ = MountState::FAILED;
        this->last_mount_attempt_ = now;
      }
      break;

    case MountState::MOUNTING:
      // Attempt NFS mount
      if (this->mount_export_(this->export_path_, this->root_fh_)) {
        ESP_LOGI(TAG, "Successfully mounted NFS export: %s", this->export_path_.c_str());

        // RFC 1813: File handles are portable across connections
        // Close MOUNT connection, will query for NFS port next
        this->close_connection_();
        this->mount_state_ = MountState::QUERYING_PMAP_NFS;
      } else {
        ESP_LOGW(TAG, "Failed to mount NFS export, will retry in %" PRIu32 " seconds", this->mount_retry_interval_ / 1000);
        this->disconnect_();
        this->mount_state_ = MountState::FAILED;
        this->last_mount_attempt_ = now;
      }
      break;

    case MountState::QUERYING_PMAP_NFS:
      // Connect to portmapper to query for NFS port
      if (!this->connected_) {
        if (!this->connect_()) {
          ESP_LOGW(TAG, "Failed to connect to portmapper for NFS query, using configured port %u", this->port_);
          this->nfs_port_ = this->port_;
          this->nfs_port_discovered_ = false;
          this->mounted_ = true;
          this->mount_state_ = MountState::MOUNTED;
          if (!this->mount_path_.empty()) {
            this->register_with_storage();
          }
          break;
        }
      }

      // Query portmapper for NFS port
      if (this->query_portmapper_(NFS_PROGRAM, NFS_VERSION_3, this->nfs_port_)) {
        this->nfs_port_discovered_ = true;
        ESP_LOGI(TAG, "NFS service available on port %u", this->nfs_port_);
      } else {
        ESP_LOGW(TAG, "Portmapper query for NFS failed, using configured port %u", this->port_);
        this->nfs_port_ = this->port_;
        this->nfs_port_discovered_ = false;
      }
      this->close_connection_();
      this->mounted_ = true;
      this->mount_state_ = MountState::MOUNTED;

      // Register with storage if configured
      if (!this->mount_path_.empty()) {
        this->register_with_storage();
      }
      break;

    case MountState::MOUNTED:
      // Already mounted - nothing to do
      // Could add periodic health check here in the future
      break;

    case MountState::FAILED:
      // Wait for retry interval
      if (now - this->last_mount_attempt_ >= this->mount_retry_interval_) {
        ESP_LOGI(TAG, "Retrying NFS mount...");
        this->mount_state_ = MountState::IDLE;
      }
      break;
  }
}

void NFSClient::dump_config() {
  ESP_LOGCONFIG(TAG, "NFS Client:");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Export: %s", this->export_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Status: %s", this->mounted_ ? "Mounted" : "Not mounted");
  if (!this->mount_path_.empty()) {
    ESP_LOGCONFIG(TAG, "  Mount path: %s", this->mount_path_.c_str());
  }
}

void NFSClient::register_with_storage() {
#if defined(USE_STORAGE)
  if (storage::global_storage != nullptr) {
    storage::global_storage->register_network_storage(this);
    ESP_LOGI(TAG, "Registered NFS network storage with storage: %s", this->mount_path_.c_str());
  } else {
    ESP_LOGD(TAG, "storage not available, skipping network storage registration");
  }
#else
  ESP_LOGD(TAG, "storage component not compiled, network storage registration disabled");
#endif  // USE_STORAGE
}

//========================================================================
// Connection Management
//========================================================================

#ifdef USE_ESP_IDF
bool NFSClient::resolve_hostname_() {
  // Return cached result if already resolved
  if (this->server_addr_resolved_) {
    return true;
  }

  ESP_LOGD(TAG, "Resolving hostname: %s", this->server_.c_str());

  // Initialize server address structure
  memset(&this->server_addr_, 0, sizeof(this->server_addr_));
  this->server_addr_.sin_family = AF_INET;
  this->server_addr_.sin_port = htons(this->port_);

  // Check if this is a .local mDNS hostname
  bool is_mdns_host = this->server_.length() > 6 && this->server_.substr(this->server_.length() - 6) == ".local";

  if (is_mdns_host) {
    // Try mDNS resolution first for .local hostnames
    ESP_LOGD(TAG, "Attempting mDNS resolution for '%s'", this->server_.c_str());

    // Strip .local suffix for mdns_query_a (it expects just the hostname)
    std::string hostname = this->server_.substr(0, this->server_.length() - 6);

    esp_ip4_addr_t addr;
    esp_err_t err = mdns_query_a(hostname.c_str(), 2000, &addr);  // 2 second timeout

    if (err == ESP_OK) {
      this->server_addr_.sin_addr.s_addr = addr.addr;
      this->server_addr_resolved_ = true;
      ESP_LOGI(TAG, "Resolved '%s' via mDNS to " IPSTR, this->server_.c_str(), IP2STR(&addr));
      return true;
    } else {
      ESP_LOGW(TAG, "mDNS resolution failed for '%s' (error %d), trying DNS fallback", this->server_.c_str(), err);
      // Fall through to regular DNS resolution
    }
  }

  // Regular DNS resolution using getaddrinfo
  struct addrinfo hints {
  }, *result = nullptr;
  hints.ai_family = AF_INET;        // IPv4
  hints.ai_socktype = SOCK_STREAM;  // TCP
  hints.ai_protocol = IPPROTO_TCP;

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", this->port_);

  int ret_dns = getaddrinfo(this->server_.c_str(), port_str, &hints, &result);
  if (ret_dns != 0 || result == nullptr) {
    if (is_mdns_host) {
      ESP_LOGE(TAG, "Failed to resolve mDNS host '%s' via both mDNS and DNS: error %d", this->server_.c_str(), ret_dns);
      ESP_LOGE(TAG, "Recommendation: Use IP address instead (e.g., '192.168.1.100')");
    } else {
      ESP_LOGE(TAG, "Failed to resolve host '%s': error %d", this->server_.c_str(), ret_dns);
    }
    return false;
  }

  memcpy(&this->server_addr_, result->ai_addr, sizeof(this->server_addr_));
  freeaddrinfo(result);

  this->server_addr_resolved_ = true;
  ESP_LOGI(TAG, "Resolved '%s' to %s", this->server_.c_str(), inet_ntoa(this->server_addr_.sin_addr));
  return true;
}
#endif

bool NFSClient::connect_() {
  if (this->connected_) {
    return true;
  }

  // Determine which port to use based on mount state (RFC 1813)
  uint16_t target_port;
  const char *service_name;
  if (this->mount_state_ == MountState::CONNECTING_PMAP || this->mount_state_ == MountState::QUERYING_PMAP_NFS) {
    target_port = PMAP_PORT;
    service_name = "portmapper";
  } else if (this->mount_state_ == MountState::CONNECTING_MOUNT && this->mount_port_discovered_) {
    target_port = this->mount_port_;
    service_name = "MOUNT service";
  } else if (this->nfs_port_discovered_) {
    // Use discovered NFS port from portmapper
    target_port = this->nfs_port_;
    service_name = "NFS service (via portmapper)";
  } else {
    // Fallback to configured port (default 2049)
    target_port = this->port_;
    service_name = "NFS service (configured)";
  }

  ESP_LOGD(TAG, "Connecting to %s %s:%u...", service_name, this->server_.c_str(), target_port);

#ifdef USE_ESP_IDF
  // Resolve hostname first (uses cache if already resolved)
  if (!this->resolve_hostname_()) {
    return false;
  }

  // Create TCP socket
  // Use ::socket to avoid collision with the lwip_sockets.h macro that redefines 'socket'
  // without argument list, which would otherwise corrupt 'this->socket_' member references.
  this->socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
    return false;
  }

  // Try to bind to a privileged port (600-1023) to avoid NFS server "insecure" requirement
  // Traditional NFS servers require clients to connect from privileged ports (<1024)
  // ESP32/FreeRTOS doesn't have privilege restrictions, so we can bind to any port
  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = INADDR_ANY;

  bool bound_privileged = false;
  // Try ports 1023 down to 600 (avoid well-known ports <512)
  for (uint16_t port = 1023; port >= 600; port--) {
    bind_addr.sin_port = htons(port);
    if (::bind(this->socket_, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) == 0) {
      ESP_LOGI(TAG, "Bound to privileged source port %u", port);
      bound_privileged = true;
      break;
    }
    // Stop if we get an error other than "address already in use"
    if (errno != EADDRINUSE) {
      break;
    }
  }

  if (!bound_privileged) {
    ESP_LOGW(TAG, "Could not bind to privileged port (errno %d), NFS server may require 'insecure' option", errno);
    // Continue anyway - socket will use ephemeral port >1024
  }

  // Update port in server address structure
  struct sockaddr_in connect_addr = this->server_addr_;
  connect_addr.sin_port = htons(target_port);

  // Connect using server address with appropriate port
  if (::connect(this->socket_, (struct sockaddr *) &connect_addr, sizeof(connect_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to connect: errno %d", errno);
    close(this->socket_);
    this->socket_ = -1;
    return false;
  }

  // Get local address to log source IP/port
  struct sockaddr_in local_addr;
  socklen_t addr_len = sizeof(local_addr);
  if (getsockname(this->socket_, (struct sockaddr *) &local_addr, &addr_len) == 0) {
    ESP_LOGD(TAG, "Connected from %s:%u", inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
  }

  // Set TCP_NODELAY
  int nodelay = 1;
  setsockopt(this->socket_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  // Set socket receive timeout (RFC 1831 doesn't specify timeout, but prevent hanging)
  // Use 30 seconds to allow for slow NFS servers and large file operations
  struct timeval tv;
  tv.tv_sec = 30;
  tv.tv_usec = 0;
  if (setsockopt(this->socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    ESP_LOGW(TAG, "Failed to set receive timeout: errno %d", errno);
  } else {
    ESP_LOGD(TAG, "Set socket receive timeout to %lld seconds", (long long) tv.tv_sec);
  }

  this->connected_ = true;
  return true;
#else
  // Arduino WiFiClient
  this->client_ = std::make_unique<WiFiClient>();
  if (!this->client_->connect(this->server_.c_str(), this->port_)) {
    ESP_LOGE(TAG, "Failed to connect");
    this->client_ = nullptr;
    return false;
  }

  this->client_->setNoDelay(true);
  this->connected_ = true;
  return true;
#endif
}

void NFSClient::close_connection_() {
  // Close TCP connection without unmounting
#ifdef USE_ESP_IDF
  if (this->socket_ >= 0) {
    close(this->socket_);
    this->socket_ = -1;
  }
#else
  if (this->client_) {
    this->client_->stop();
    this->client_ = nullptr;
  }
#endif

  this->connected_ = false;
}

void NFSClient::disconnect_() {
  if (this->mounted_) {
    this->unmount();
  }
  this->close_connection_();
}

bool NFSClient::send_rpc_(const XDRBuffer &request, XDRBuffer &response) {
  if (!this->connected_ && !this->connect_()) {
    return false;
  }

  // RPC over TCP uses record marking (RFC 1831 section 11)
  // Format: [4-byte length with high bit set for last fragment] [data]
  uint32_t length = request.size() | 0x80000000;  // Set high bit for last fragment

#ifdef USE_ESP_IDF
  // Send length
  uint8_t length_buf[4];
  length_buf[0] = (length >> 24) & 0xFF;
  length_buf[1] = (length >> 16) & 0xFF;
  length_buf[2] = (length >> 8) & 0xFF;
  length_buf[3] = length & 0xFF;

  if (send(this->socket_, length_buf, 4, 0) != 4) {
    ESP_LOGE(TAG, "Failed to send RPC length");
    close(this->socket_);
    this->socket_ = -1;
    this->connected_ = false;
    return false;
  }

  // Send data
  if (send(this->socket_, request.data().data(), request.size(), 0) != static_cast<int>(request.size())) {
    ESP_LOGE(TAG, "Failed to send RPC data");
    close(this->socket_);
    this->socket_ = -1;
    this->connected_ = false;
    return false;
  }

  // Receive response length
  uint8_t response_length_buf[4];
  if (recv(this->socket_, response_length_buf, 4, MSG_WAITALL) != 4) {
    ESP_LOGE(TAG, "Failed to receive RPC response length");
    close(this->socket_);
    this->socket_ = -1;
    this->connected_ = false;
    return false;
  }

  uint32_t response_length =
      (static_cast<uint32_t>(response_length_buf[0]) << 24) | (static_cast<uint32_t>(response_length_buf[1]) << 16) |
      (static_cast<uint32_t>(response_length_buf[2]) << 8) | static_cast<uint32_t>(response_length_buf[3]);

  response_length &= 0x7FFFFFFF;  // Clear high bit

  if (response_length > 65536) {  // Sanity check
    ESP_LOGE(TAG, "Response too large: %" PRIu32 " bytes", response_length);
    close(this->socket_);
    this->socket_ = -1;
    this->connected_ = false;
    return false;
  }

  // Receive response data in a loop to handle partial reads
  std::vector<uint8_t> response_data(response_length);
  size_t total_received = 0;
  while (total_received < response_length) {
    int received = recv(this->socket_, response_data.data() + total_received, response_length - total_received, 0);
    if (received <= 0) {
      ESP_LOGE(TAG, "Failed to receive RPC response data: received=%d, expected=%" PRIu32 ", total_received=%" PRIu32 ", errno=%d",
               received, response_length, (uint32_t) total_received, errno);
      close(this->socket_);
      this->socket_ = -1;
      this->connected_ = false;
      return false;
    }
    total_received += received;
  }

  response = XDRBuffer(response_data);
  return true;
#else
  // Arduino WiFiClient
  // Send length
  uint8_t length_buf[4];
  length_buf[0] = (length >> 24) & 0xFF;
  length_buf[1] = (length >> 16) & 0xFF;
  length_buf[2] = (length >> 8) & 0xFF;
  length_buf[3] = length & 0xFF;

  this->client_->write(length_buf, 4);
  this->client_->write(request.data().data(), request.size());

  // Wait for response length (4 bytes) with 10 second timeout
  uint32_t start = millis();
  while (this->client_->available() < 4 && millis() - start < 10000) {
    delay(10);
  }

  if (this->client_->available() < 4) {
    ESP_LOGE(TAG, "Timeout waiting for response");
    return false;
  }

  // Read response length
  uint8_t response_length_buf[4];
  this->client_->read(response_length_buf, 4);

  uint32_t response_length =
      (static_cast<uint32_t>(response_length_buf[0]) << 24) | (static_cast<uint32_t>(response_length_buf[1]) << 16) |
      (static_cast<uint32_t>(response_length_buf[2]) << 8) | static_cast<uint32_t>(response_length_buf[3]);

  response_length &= 0x7FFFFFFF;

  if (response_length > 65536) {  // Sanity check
    ESP_LOGE(TAG, "Response too large: %" PRIu32 " bytes", response_length);
    return false;
  }

  // Read response data in a loop to handle partial reads
  std::vector<uint8_t> response_data(response_length);
  size_t total_read = 0;
  start = millis();

  while (total_read < response_length && millis() - start < 10000) {
    int available = this->client_->available();
    if (available > 0) {
      size_t to_read = std::min(static_cast<size_t>(available), response_length - total_read);
      int bytes_read = this->client_->read(response_data.data() + total_read, to_read);
      if (bytes_read > 0) {
        total_read += bytes_read;
      } else if (bytes_read < 0) {
        ESP_LOGE(TAG, "Failed to read response data");
        return false;
      }
    } else {
      delay(10);  // Wait for more data
    }
  }

  if (total_read < response_length) {
    ESP_LOGE(TAG, "Timeout waiting for full response: got %" PRIu32 " / %" PRIu32 " bytes", (uint32_t) total_read, response_length);
    return false;
  }

  response = XDRBuffer(response_data);
  return true;
#endif
}

//========================================================================
// Portmapper Protocol
//========================================================================

bool NFSClient::query_portmapper_(uint32_t program, uint32_t version, uint16_t &port) {
  ESP_LOGD(TAG, "Querying portmapper for program %" PRIu32 " version %" PRIu32, program, version);

  // Build PMAP GETPORT call
  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, PMAP_PROGRAM, PMAP_VERSION, PMAPPROC_GETPORT, 0, 0);

  // PMAP GETPORT arguments: prog, vers, prot, port (unused)
  request.encode_uint32(program);
  request.encode_uint32(version);
  request.encode_uint32(6);  // IPPROTO_TCP
  request.encode_uint32(0);  // port (unused in request)

  // Send RPC call
  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    ESP_LOGE(TAG, "Failed to send PMAP GETPORT");
    return false;
  }

  // Parse RPC reply - handle PROG_UNAVAIL specially for portmapper
  RPCAcceptStatus accept_status;
  if (!this->rpc_.parse_reply(response, xid, accept_status)) {
    // parse_reply returns false if accept_status != RPC_SUCCESS
    // For portmapper, PROG_UNAVAIL means program not registered (valid response)
    if (accept_status == RPC_PROG_UNAVAIL) {
      ESP_LOGI(TAG, "Program %" PRIu32 " version %" PRIu32 " not registered with portmapper", program, version);
    } else {
      ESP_LOGE(TAG, "Portmapper RPC failed with status %" PRIu32, static_cast<uint32_t>(accept_status));
    }
    return false;
  }

  // Decode port number
  uint32_t port_result;
  if (!response.decode_uint32(port_result)) {
    ESP_LOGE(TAG, "Failed to decode port from PMAP reply");
    return false;
  }

  if (port_result == 0) {
    ESP_LOGI(TAG, "Portmapper returned port 0 - program %" PRIu32 " version %" PRIu32 " not registered", program, version);
    return false;
  }

  port = static_cast<uint16_t>(port_result);
  ESP_LOGI(TAG, "Discovered port %u for program %" PRIu32 " version %" PRIu32 " via portmapper", port, program, version);
  return true;
}

//========================================================================
// MOUNT Protocol Implementation
//========================================================================

bool NFSClient::mount_export_(const std::string &export_path, NFSFileHandle &fh) {
  ESP_LOGD(TAG, "Mounting export: %s with UID=%" PRIu32 ", GID=%" PRIu32, export_path.c_str(), this->uid_, this->gid_);

  // Build MOUNT MNT call
  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, MOUNT_PROGRAM, MOUNT_VERSION_3, MOUNTPROC3_MNT, this->uid_, this->gid_);

  // MOUNT MNT arguments: dirpath
  ESP_LOGD(TAG, "Encoded export path: '%s'", export_path.c_str());
  request.encode_string(export_path);

  // Send RPC call
  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    ESP_LOGE(TAG, "Failed to send MOUNT MNT");
    return false;
  }

  // Parse RPC reply
  RPCAcceptStatus status;
  if (!this->rpc_.parse_reply(response, xid, status)) {
    ESP_LOGE(TAG, "Failed to parse MOUNT reply");
    return false;
  }

  // Parse MOUNT reply: status + file handle
  uint32_t mount_status;
  if (!response.decode_uint32(mount_status)) {
    ESP_LOGE(TAG, "Failed to decode MOUNT status");
    return false;
  }

  if (mount_status != 0) {
    // MOUNT v3 error codes from RFC 1813
    const char *error_str = "UNKNOWN";
    switch (mount_status) {
      case 1:
        error_str = "MNT3ERR_PERM (Not owner)";
        break;
      case 2:
        error_str = "MNT3ERR_NOENT (No such file or directory)";
        break;
      case 5:
        error_str = "MNT3ERR_IO (I/O error)";
        break;
      case 13:
        error_str = "MNT3ERR_ACCESS (Permission denied - check NFS export configuration)";
        break;
      case 20:
        error_str = "MNT3ERR_NOTDIR (Not a directory)";
        break;
      case 63:
        error_str = "MNT3ERR_NAMETOOLONG (Filename too long)";
        break;
      case 10004:
        error_str = "MNT3ERR_NOTSUPP (Operation not supported)";
        break;
      case 10006:
        error_str = "MNT3ERR_SERVERFAULT (Server fault)";
        break;
    }
    ESP_LOGE(TAG, "MOUNT failed: %s (status %" PRIu32 ")", error_str, mount_status);
    ESP_LOGE(TAG, "Check NFS server export configuration:");
    ESP_LOGE(TAG, "  - Verify export path exists on server");
    ESP_LOGE(TAG, "  - Check /etc/exports allows access from %s", inet_ntoa(this->server_addr_.sin_addr));
    ESP_LOGE(TAG, "  - Run 'exportfs -v' on server to verify exports");
    return false;
  }

  // Decode file handle
  if (!fh.decode(response)) {
    ESP_LOGE(TAG, "Failed to decode file handle");
    return false;
  }

  ESP_LOGD(TAG, "MOUNT successful, file handle size: %" PRIu32, (uint32_t) fh.data.size());
  return true;
}

bool NFSClient::unmount_export_(const std::string &export_path) {
  ESP_LOGD(TAG, "Unmounting export: %s", export_path.c_str());

  // Build MOUNT UMNT call
  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, MOUNT_PROGRAM, MOUNT_VERSION_3, MOUNTPROC3_UMNT, this->uid_, this->gid_);

  // MOUNT UMNT arguments: dirpath
  request.encode_string(export_path);

  // Send RPC call
  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    ESP_LOGW(TAG, "Failed to send MOUNT UMNT");
    return false;
  }

  // Parse RPC reply
  RPCAcceptStatus status;
  if (!this->rpc_.parse_reply(response, xid, status)) {
    ESP_LOGW(TAG, "Failed to parse UMNT reply");
    return false;
  }

  return true;
}

//========================================================================
// NFS Mount/Unmount
//========================================================================

bool NFSClient::mount() {
  // Trigger mount attempt if not already mounted or in progress
  if (this->mount_state_ == MountState::IDLE || this->mount_state_ == MountState::FAILED) {
    ESP_LOGI(TAG, "Mount requested, triggering async mount attempt");
    this->mount_state_ = MountState::IDLE;  // Will start in next loop()
    return false;                           // Not mounted yet, will happen async
  }

  // Return current mount status
  return this->mounted_;
}

void NFSClient::unmount() {
  if (!this->mounted_) {
    return;
  }

  ESP_LOGI(TAG, "Unmounting NFS export...");

  // Unregister from storage first
#if defined(USE_STORAGE)
  if (storage::global_storage != nullptr && !this->mount_path_.empty()) {
    // Note: storage doesn't have unregister_network_storage yet, but should
    ESP_LOGD(TAG, "Should unregister from storage here");
  }
#endif

  this->unmount_export_(this->export_path_);
  this->root_fh_.data.clear();
  this->mounted_ = false;
  this->mount_state_ = MountState::FAILED;  // Go to failed state to allow retry

  ESP_LOGI(TAG, "NFS unmounted");
}

//========================================================================
// Path Resolution
//========================================================================

std::vector<std::string> NFSClient::split_path_(const std::string &path) {
  std::vector<std::string> components;
  std::string current;

  for (char c : path) {
    if (c == '/') {
      if (!current.empty()) {
        components.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }

  if (!current.empty()) {
    components.push_back(current);
  }

  return components;
}

bool NFSClient::resolve_path_(const std::string &path, NFSFileHandle &fh, NFSFileAttr &attr) {
  if (!this->mounted_) {
    ESP_LOGW(TAG, "resolve_path_: not mounted");
    return false;
  }

  // Start from root
  fh = this->root_fh_;

  // Empty path = root
  if (path.empty() || path == "/") {
    ESP_LOGD(TAG, "resolve_path_: getting root attributes");
    bool result = this->nfs_getattr_(fh, attr);
    if (!result) {
      ESP_LOGW(TAG, "resolve_path_: failed to get root attributes");
    }
    return result;
  }

  // Split path into components
  std::vector<std::string> components = this->split_path_(path);

  // Resolve each component
  for (const auto &component : components) {
    NFSFileHandle next_fh;
    if (!this->nfs_lookup_(fh, component, next_fh, attr)) {
      return false;
    }
    fh = next_fh;
  }

  return true;
}

bool NFSClient::resolve_parent_path_(const std::string &path, NFSFileHandle &parent_fh, std::string &filename) {
  if (!this->mounted_) {
    return false;
  }

  // Split path
  std::vector<std::string> components = this->split_path_(path);
  if (components.empty()) {
    return false;
  }

  // Last component is the filename
  filename = components.back();
  components.pop_back();

  // Resolve parent directory
  parent_fh = this->root_fh_;
  NFSFileAttr attr;

  for (const auto &component : components) {
    NFSFileHandle next_fh;
    if (!this->nfs_lookup_(parent_fh, component, next_fh, attr)) {
      return false;
    }
    parent_fh = next_fh;
  }

  return true;
}

//========================================================================
// NFS Protocol Operations
//========================================================================

bool NFSClient::nfs_lookup_(const NFSFileHandle &dir_fh, const std::string &name, NFSFileHandle &fh,
                            NFSFileAttr &attr) {
  ESP_LOGVV(TAG, "NFS LOOKUP: %s", name.c_str());

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_LOOKUP, this->uid_, this->gid_);

  // LOOKUP arguments: dir file handle + name
  dir_fh.encode(request);
  request.encode_string(name);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "LOOKUP failed: status=%" PRIu32, nfs_status);
    return false;
  }

  // Decode file handle
  if (!fh.decode(response)) {
    return false;
  }

  // Decode attributes (optional)
  bool has_attr;
  if (response.decode_bool(has_attr) && has_attr) {
    attr.decode(response);
  }

  return true;
}

bool NFSClient::nfs_getattr_(const NFSFileHandle &fh, NFSFileAttr &attr) {
  ESP_LOGVV(TAG, "NFS GETATTR");

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_GETATTR, this->uid_, this->gid_);

  // GETATTR arguments: file handle
  fh.encode(request);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    ESP_LOGW(TAG, "GETATTR: send_rpc_ failed");
    return false;
  }

  ESP_LOGD(TAG, "GETATTR: response buffer size=%" PRIu32 ", position=%" PRIu32, (uint32_t) response.size(), (uint32_t) response.position());

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    ESP_LOGW(TAG, "GETATTR: parse_reply failed");
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status)) {
    ESP_LOGW(TAG, "GETATTR failed: could not decode NFS status, position=%" PRIu32 ", size=%" PRIu32, (uint32_t) response.position(),
             (uint32_t) response.size());
    return false;
  }
  if (nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "GETATTR failed: NFS status=%" PRIu32 " (NFS3_OK=%lu)", nfs_status, NFS3_OK);
    return false;
  }

  // GETATTR returns fattr3 directly (not post_op_attr like other operations)
  return attr.decode(response);
}

bool NFSClient::nfs_read_(const NFSFileHandle &fh, uint64_t offset, uint32_t count, std::vector<uint8_t> &data) {
  ESP_LOGVV(TAG, "NFS READ: offset=%llu, count=%" PRIu32, offset, count);

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_READ, this->uid_, this->gid_);

  // READ arguments: file handle + offset + count
  fh.encode(request);
  request.encode_uint64(offset);
  request.encode_uint32(count);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "READ failed: status=%" PRIu32, nfs_status);
    return false;
  }

  // Skip post-op attributes
  bool has_attr;
  NFSFileAttr attr;
  if (response.decode_bool(has_attr) && has_attr) {
    attr.decode(response);
  }

  // Decode count + EOF flag
  uint32_t bytes_read;
  bool eof;
  if (!response.decode_uint32(bytes_read) || !response.decode_bool(eof)) {
    return false;
  }

  // Decode data
  if (!response.decode_opaque(data)) {
    return false;
  }

  ESP_LOGVV(TAG, "NFS READ: read %" PRIu32 " bytes, EOF=%d", bytes_read, eof);
  return true;
}

bool NFSClient::nfs_write_(const NFSFileHandle &fh, uint64_t offset, const uint8_t *data, size_t length) {
  ESP_LOGVV(TAG, "NFS WRITE: offset=%llu, length=%" PRIu32, offset, (uint32_t) length);

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_WRITE, this->uid_, this->gid_);

  // WRITE arguments: file handle + offset + count + stable + data
  fh.encode(request);
  request.encode_uint64(offset);
  request.encode_uint32(length);
  request.encode_uint32(2);  // FILE_SYNC (write immediately to stable storage)
  request.encode_opaque(data, length);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "WRITE failed: status=%" PRIu32, nfs_status);
    return false;
  }

  // Skip wcc_data (pre_op_attr + post_op_attr)
  // pre_op_attr: bool + (if true: size, mtime, ctime)
  bool has_pre_op;
  if (!response.decode_bool(has_pre_op)) {
    ESP_LOGW(TAG, "WRITE: failed to decode has_pre_op");
    return false;
  }
  if (has_pre_op) {
    // Skip wcc_attr: size(8) + mtime(8) + ctime(8) = 24 bytes
    uint64_t dummy64;
    uint32_t dummy32;
    response.decode_uint64(dummy64);  // size
    response.decode_uint32(dummy32);  // mtime_sec
    response.decode_uint32(dummy32);  // mtime_nsec
    response.decode_uint32(dummy32);  // ctime_sec
    response.decode_uint32(dummy32);  // ctime_nsec
  }

  // post_op_attr
  bool has_post_op;
  if (!response.decode_bool(has_post_op)) {
    ESP_LOGW(TAG, "WRITE: failed to decode has_post_op");
    return false;
  }
  if (has_post_op) {
    NFSFileAttr attr;
    attr.decode(response);
  }

  // Decode bytes written
  uint32_t bytes_written;
  if (!response.decode_uint32(bytes_written)) {
    ESP_LOGW(TAG, "WRITE: failed to decode bytes_written");
    return false;
  }

  ESP_LOGVV(TAG, "NFS WRITE: wrote %" PRIu32 " bytes", bytes_written);
  return (bytes_written == length);
}

bool NFSClient::nfs_create_(const NFSFileHandle &dir_fh, const std::string &name, uint32_t mode, NFSFileHandle &fh) {
  ESP_LOGD(TAG, "NFS CREATE: %s (mode 0%" PRIo32 ")", name.c_str(), (uint32_t) mode);

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_CREATE, this->uid_, this->gid_);

  // CREATE arguments: dir fh + name + how + attributes
  dir_fh.encode(request);
  request.encode_string(name);
  request.encode_uint32(0);  // UNCHECKED

  // Set attributes
  request.encode_bool(true);  // mode
  request.encode_uint32(mode);
  request.encode_bool(true);  // uid
  request.encode_uint32(this->uid_);
  request.encode_bool(true);  // gid
  request.encode_uint32(this->gid_);
  request.encode_bool(false);  // size
  request.encode_uint32(0);    // atime: don't set
  request.encode_uint32(0);    // mtime: don't set

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "CREATE failed: status=%" PRIu32, nfs_status);
    return false;
  }

  // Decode file handle
  bool has_fh;
  if (!response.decode_bool(has_fh) || !has_fh) {
    return false;
  }

  return fh.decode(response);
}

bool NFSClient::nfs_remove_(const NFSFileHandle &dir_fh, const std::string &name) {
  ESP_LOGD(TAG, "NFS REMOVE: %s", name.c_str());

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_REMOVE, this->uid_, this->gid_);

  // REMOVE arguments: dir fh + name
  dir_fh.encode(request);
  request.encode_string(name);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "REMOVE failed: status=%" PRIu32, nfs_status);
    return false;
  }

  return true;
}

bool NFSClient::nfs_mkdir_(const NFSFileHandle &dir_fh, const std::string &name, uint32_t mode, NFSFileHandle &fh) {
  ESP_LOGD(TAG, "NFS MKDIR: %s (mode 0%" PRIo32 ")", name.c_str(), (uint32_t) mode);

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_MKDIR, this->uid_, this->gid_);

  // MKDIR arguments: dir fh + name + attributes
  dir_fh.encode(request);
  request.encode_string(name);

  // Set attributes
  request.encode_bool(true);              // mode
  request.encode_uint32(mode | 0040000);  // Add directory bit
  request.encode_bool(true);              // uid
  request.encode_uint32(this->uid_);
  request.encode_bool(true);  // gid
  request.encode_uint32(this->gid_);
  request.encode_bool(false);  // size
  request.encode_uint32(0);    // atime
  request.encode_uint32(0);    // mtime

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "MKDIR failed: status=%" PRIu32, nfs_status);
    return false;
  }

  // Decode file handle
  bool has_fh;
  if (!response.decode_bool(has_fh) || !has_fh) {
    return false;
  }

  return fh.decode(response);
}

bool NFSClient::nfs_rmdir_(const NFSFileHandle &dir_fh, const std::string &name) {
  ESP_LOGD(TAG, "NFS RMDIR: %s", name.c_str());

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_RMDIR, this->uid_, this->gid_);

  // RMDIR arguments: dir fh + name
  dir_fh.encode(request);
  request.encode_string(name);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "RMDIR failed: status=%" PRIu32, nfs_status);
    return false;
  }

  return true;
}

bool NFSClient::nfs_rename_(const NFSFileHandle &from_dir_fh, const std::string &from_name,
                            const NFSFileHandle &to_dir_fh, const std::string &to_name) {
  ESP_LOGD(TAG, "NFS RENAME: %s -> %s", from_name.c_str(), to_name.c_str());

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_RENAME, this->uid_, this->gid_);

  // RENAME arguments: from_dir fh + from_name + to_dir fh + to_name
  from_dir_fh.encode(request);
  request.encode_string(from_name);
  to_dir_fh.encode(request);
  request.encode_string(to_name);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "RENAME failed: status=%" PRIu32, nfs_status);
    return false;
  }

  return true;
}

bool NFSClient::nfs_readdir_(const NFSFileHandle &dir_fh, std::vector<NFSDirEntry> &entries) {
  // Use READDIRPLUS to get file attributes (needed for is_directory detection)
  ESP_LOGI(TAG, "NFS READDIRPLUS starting");

  entries.clear();
  uint64_t cookie = 0;
  uint8_t cookieverf[8] = {0};

  // READDIRPLUS may need multiple calls to get all entries
  while (true) {
    uint32_t xid = RPCClient::generate_xid();
    XDRBuffer request;
    this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_READDIRPLUS, this->uid_, this->gid_);

    // READDIRPLUS arguments: dir fh + cookie + cookieverf + dircount + maxcount
    dir_fh.encode(request);
    request.encode_uint64(cookie);
    request.encode_bytes(cookieverf, 8);  // Fixed 8 bytes, no length prefix
    request.encode_uint32(4096);          // dircount: max bytes of directory info
    request.encode_uint32(32768);         // maxcount: max bytes of total response

    XDRBuffer response;
    if (!this->send_rpc_(request, response)) {
      return false;
    }

    RPCAcceptStatus rpc_status;
    if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
      return false;
    }

    // Parse NFS status
    uint32_t nfs_status;
    if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
      ESP_LOGW(TAG, "READDIRPLUS failed: status=%" PRIu32, nfs_status);
      return false;
    }

    // Decode post-op attributes (optional) - directory attributes
    bool has_dir_attr;
    if (!response.decode_bool(has_dir_attr)) {
      ESP_LOGW(TAG, "READDIRPLUS: Failed to decode has_dir_attr");
      return false;
    }
    if (has_dir_attr) {
      NFSFileAttr dir_attr;
      if (!dir_attr.decode(response)) {
        ESP_LOGW(TAG, "READDIRPLUS: Failed to decode dir_attributes");
        return false;
      }
    }

    // Decode cookieverf (fixed 8 bytes per RFC 1813)
    if (!response.decode_bytes(cookieverf, 8)) {
      ESP_LOGW(TAG, "READDIRPLUS: Failed to decode cookieverf");
      return false;
    }

    // Decode directory entries (entryplus3)
    bool has_entry;
    while (response.decode_bool(has_entry) && has_entry) {
      NFSDirEntry entry;
      if (!response.decode_uint64(entry.fileid) || !response.decode_string(entry.name) ||
          !response.decode_uint64(entry.cookie)) {
        ESP_LOGW(TAG, "READDIRPLUS: Failed to decode entry base");
        return false;
      }

      // Decode name_attributes (post_op_attr)
      bool has_name_attr;
      if (!response.decode_bool(has_name_attr)) {
        ESP_LOGW(TAG, "READDIRPLUS: Failed to decode has_name_attr for %s", entry.name.c_str());
        return false;
      }
      if (has_name_attr) {
        if (!entry.attr.decode(response)) {
          ESP_LOGW(TAG, "READDIRPLUS: Failed to decode name_attributes for %s", entry.name.c_str());
          return false;
        }
        entry.has_attr = true;
      }

      // Decode name_handle (post_op_fh3) - we don't store it but must skip it
      bool has_name_handle;
      if (!response.decode_bool(has_name_handle)) {
        ESP_LOGW(TAG, "READDIRPLUS: Failed to decode has_name_handle for %s", entry.name.c_str());
        return false;
      }
      if (has_name_handle) {
        // Skip the file handle (opaque<NFS3_FHSIZE>)
        std::string fh_data;
        if (!response.decode_string(fh_data)) {
          ESP_LOGW(TAG, "READDIRPLUS: Failed to skip name_handle for %s", entry.name.c_str());
          return false;
        }
      }

      if (entry.name != "." && entry.name != "..") {
        entries.push_back(entry);
      }
      cookie = entry.cookie;
    }

    // Decode EOF flag
    bool eof;
    if (!response.decode_bool(eof)) {
      ESP_LOGW(TAG, "READDIRPLUS: Failed to decode EOF");
      return false;
    }

    if (eof) {
      ESP_LOGI(TAG, "READDIRPLUS: Got %" PRIu32 " entries", (uint32_t) entries.size());
      break;
    }

    if (cookie == 0) {
      ESP_LOGW(TAG, "READDIRPLUS: No progress, aborting");
      return false;
    }
  }

  return true;
}

//========================================================================
// High-Level File Operations
//========================================================================

bool NFSClient::read_file(const std::string &path, std::vector<uint8_t> &data) {
  NFSFileHandle fh;
  NFSFileAttr attr;

  // Resolve path to file handle
  if (!this->resolve_path_(path, fh, attr)) {
    ESP_LOGW(TAG, "Failed to resolve path: %s", path.c_str());
    return false;
  }

  // Check if it's a regular file
  if (attr.type != NF3REG) {
    ESP_LOGW(TAG, "Not a regular file: %s", path.c_str());
    return false;
  }

  // Read file in chunks
  data.clear();
  data.reserve(attr.size);

  uint64_t offset = 0;
  uint32_t chunk_size = 8192;

  while (offset < attr.size) {
    std::vector<uint8_t> chunk;
    if (!this->nfs_read_(fh, offset, chunk_size, chunk)) {
      ESP_LOGE(TAG, "Failed to read file at offset %llu", offset);
      return false;
    }

    if (chunk.empty()) {
      break;  // EOF
    }

    data.insert(data.end(), chunk.begin(), chunk.end());
    offset += chunk.size();
  }

  ESP_LOGI(TAG, "Read file: %s (%" PRIu32 " bytes)", path.c_str(), (uint32_t) data.size());
  return true;
}

bool NFSClient::read_file_chunk(const std::string &path, uint8_t *buffer, size_t offset, size_t max_len,
                                size_t &bytes_read) {
  bytes_read = 0;

  NFSFileHandle fh;
  NFSFileAttr attr;

  // Check cache (valid for 5 seconds to handle sequential chunked reads)
  uint32_t now = millis();
  if (this->cached_path_ == path && (now - this->cache_timestamp_) < 5000 && this->cached_fh_.is_valid()) {
    fh = this->cached_fh_;
    attr = this->cached_attr_;
    ESP_LOGVV(TAG, "Using cached file handle for %s", path.c_str());
  } else {
    // Resolve path to file handle
    if (!this->resolve_path_(path, fh, attr)) {
      ESP_LOGW(TAG, "Failed to resolve path: %s", path.c_str());
      return false;
    }

    // Cache the file handle for subsequent reads
    this->cached_path_ = path;
    this->cached_fh_ = fh;
    this->cached_attr_ = attr;
    this->cache_timestamp_ = now;
    ESP_LOGVV(TAG, "Cached file handle for %s", path.c_str());
  }

  // Check if it's a regular file
  if (attr.type != NF3REG) {
    ESP_LOGW(TAG, "Not a regular file: %s", path.c_str());
    return false;
  }

  // Check if offset is beyond file size (EOF)
  if (offset >= attr.size) {
    bytes_read = 0;
    return true;  // EOF, not an error
  }

  // Calculate actual read size (don't read past EOF)
  size_t to_read = max_len;
  if (offset + to_read > attr.size) {
    to_read = attr.size - offset;
  }

  // Perform NFS READ RPC
  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_READ, this->uid_, this->gid_);

  fh.encode(request);
  request.encode_uint64(offset);
  request.encode_uint32(to_read);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    this->cached_path_.clear();
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    this->cached_path_.clear();
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "READ failed: status=%" PRIu32, nfs_status);
    this->cached_path_.clear();
    return false;
  }

  // Skip post-op attributes
  bool has_attr_result;
  NFSFileAttr attr_result;
  if (response.decode_bool(has_attr_result) && has_attr_result) {
    attr_result.decode(response);
  }

  // Decode count + EOF flag
  uint32_t nfs_bytes_read;
  bool eof;
  if (!response.decode_uint32(nfs_bytes_read) || !response.decode_bool(eof)) {
    this->cached_path_.clear();
    return false;
  }

  // Decode data directly into caller's buffer (no intermediate allocation)
  size_t actual_bytes;
  if (!response.decode_opaque_to_buffer(buffer, max_len, actual_bytes)) {
    ESP_LOGW(TAG, "Failed to decode file data at offset %" PRIu32, (uint32_t) offset);
    this->cached_path_.clear();
    return false;
  }

  bytes_read = actual_bytes;
  ESP_LOGVV(TAG, "Read file chunk: %s (offset=%" PRIu32 ", bytes_read=%" PRIu32 ")", path.c_str(), (uint32_t) offset, (uint32_t) bytes_read);
  return true;
}

bool NFSClient::write_file(const std::string &path, const uint8_t *data, size_t length) {
  NFSFileHandle parent_fh;
  std::string filename;

  // Resolve parent directory
  if (!this->resolve_parent_path_(path, parent_fh, filename)) {
    ESP_LOGW(TAG, "Failed to resolve parent path: %s", path.c_str());
    return false;
  }

  // Create file
  NFSFileHandle fh;
  if (!this->nfs_create_(parent_fh, filename, 0644, fh)) {
    ESP_LOGW(TAG, "Failed to create file: %s", path.c_str());
    return false;
  }

  // Write data in chunks
  uint64_t offset = 0;
  uint32_t chunk_size = 8192;

  while (offset < length) {
    uint32_t to_write = std::min(chunk_size, static_cast<uint32_t>(length - offset));
    if (!this->nfs_write_(fh, offset, data + offset, to_write)) {
      ESP_LOGE(TAG, "Failed to write file at offset %llu", offset);
      return false;
    }
    offset += to_write;
  }

  ESP_LOGI(TAG, "Wrote file: %s (%" PRIu32 " bytes)", path.c_str(), (uint32_t) length);
  return true;
}

bool NFSClient::write_file_chunk(const std::string &path, const uint8_t *data, size_t offset, size_t length,
                                 bool create) {
  NFSFileHandle fh;
  NFSFileAttr attr;

  // Try to resolve existing file
  if (!this->resolve_path_(path, fh, attr)) {
    if (!create) {
      ESP_LOGW(TAG, "File does not exist and create=false: %s", path.c_str());
      return false;
    }

    // Create file first
    NFSFileHandle parent_fh;
    std::string filename;
    if (!this->resolve_parent_path_(path, parent_fh, filename)) {
      ESP_LOGW(TAG, "Failed to resolve parent path: %s", path.c_str());
      return false;
    }

    if (!this->nfs_create_(parent_fh, filename, 0644, fh)) {
      ESP_LOGW(TAG, "Failed to create file: %s", path.c_str());
      return false;
    }
  }

  // Write chunk at offset
  if (!this->nfs_write_(fh, offset, data, length)) {
    ESP_LOGE(TAG, "Failed to write chunk at offset %" PRIu32, (uint32_t) offset);
    return false;
  }

  return true;
}

bool NFSClient::delete_file(const std::string &path) {
  NFSFileHandle parent_fh;
  std::string filename;

  if (!this->resolve_parent_path_(path, parent_fh, filename)) {
    return false;
  }

  return this->nfs_remove_(parent_fh, filename);
}

bool NFSClient::file_exists(const std::string &path) {
  NFSFileHandle fh;
  NFSFileAttr attr;
  return this->resolve_path_(path, fh, attr);
}

bool NFSClient::list_directory(const std::string &path, std::vector<NFSDirEntry> &entries) {
  NFSFileHandle fh;
  NFSFileAttr attr;

  if (!this->resolve_path_(path, fh, attr)) {
    return false;
  }

  if (attr.type != NF3DIR) {
    ESP_LOGW(TAG, "Not a directory: %s", path.c_str());
    return false;
  }

  return this->nfs_readdir_(fh, entries);
}

bool NFSClient::create_directory(const std::string &path) {
  NFSFileHandle parent_fh;
  std::string dirname;

  if (!this->resolve_parent_path_(path, parent_fh, dirname)) {
    return false;
  }

  NFSFileHandle fh;
  return this->nfs_mkdir_(parent_fh, dirname, 0777, fh);
}

bool NFSClient::delete_directory(const std::string &path) {
  NFSFileHandle parent_fh;
  std::string dirname;

  if (!this->resolve_parent_path_(path, parent_fh, dirname)) {
    return false;
  }

  return this->nfs_rmdir_(parent_fh, dirname);
}

bool NFSClient::is_directory(const std::string &path) {
  NFSFileHandle fh;
  NFSFileAttr attr;

  if (!this->resolve_path_(path, fh, attr)) {
    return false;
  }

  return (attr.type == NF3DIR);
}

bool NFSClient::get_file_attributes(const std::string &path, NFSFileAttr &attr) {
  NFSFileHandle fh;
  return this->resolve_path_(path, fh, attr);
}

bool NFSClient::get_space_info(uint64_t &total_bytes, uint64_t &free_bytes) {
  if (!this->mounted_ || !this->root_fh_.is_valid()) {
    return false;
  }

  uint32_t xid = RPCClient::generate_xid();
  XDRBuffer request;
  this->rpc_.build_call(request, xid, NFS_PROGRAM, NFS_VERSION_3, NFSPROC3_FSSTAT, this->uid_, this->gid_);

  // FSSTAT arguments: just the root file handle
  this->root_fh_.encode(request);

  XDRBuffer response;
  if (!this->send_rpc_(request, response)) {
    return false;
  }

  RPCAcceptStatus rpc_status;
  if (!this->rpc_.parse_reply(response, xid, rpc_status)) {
    return false;
  }

  // Parse NFS status
  uint32_t nfs_status;
  if (!response.decode_uint32(nfs_status) || nfs_status != NFS3_OK) {
    ESP_LOGW(TAG, "FSSTAT failed: status=%" PRIu32, nfs_status);
    return false;
  }

  // Skip post-op attributes
  bool has_attr;
  if (!response.decode_bool(has_attr)) {
    return false;
  }
  if (has_attr) {
    NFSFileAttr attr;
    if (!attr.decode(response)) {
      return false;
    }
  }

  // Decode space info (all uint64)
  uint64_t tbytes, fbytes, abytes;
  uint64_t tfiles, ffiles, afiles;
  uint32_t invarsec;

  if (!response.decode_uint64(tbytes) || !response.decode_uint64(fbytes) || !response.decode_uint64(abytes) ||
      !response.decode_uint64(tfiles) || !response.decode_uint64(ffiles) || !response.decode_uint64(afiles) ||
      !response.decode_uint32(invarsec)) {
    ESP_LOGW(TAG, "FSSTAT: Failed to decode response");
    return false;
  }

  total_bytes = tbytes;
  free_bytes = fbytes;  // Use fbytes (free to privileged), could use abytes for non-privileged

  ESP_LOGD(TAG, "FSSTAT: total=%llu, free=%llu, avail=%llu", tbytes, fbytes, abytes);
  return true;
}

#if defined(USE_STORAGE)
// NetworkStorage interface override - converts NFSDirEntry to NetworkStorage::DirEntry
bool NFSClient::list_directory(const std::string &path, std::vector<storage::NetworkStorage::DirEntry> &entries) {
  // Call existing NFS-specific list_directory
  std::vector<NFSDirEntry> nfs_entries;
  if (!this->list_directory(path, nfs_entries)) {
    ESP_LOGW(TAG, "Failed to list directory: %s", path.c_str());
    return false;
  }

  // Convert NFSDirEntry to NetworkStorage::DirEntry
  entries.clear();
  entries.reserve(nfs_entries.size());
  for (const auto &nfs_entry : nfs_entries) {
    storage::NetworkStorage::DirEntry entry;
    entry.name = nfs_entry.name;
    entry.size = nfs_entry.has_attr ? nfs_entry.attr.size : 0;
    entry.is_directory = nfs_entry.has_attr && (nfs_entry.attr.type == NF3DIR);
    entries.push_back(entry);
  }

  return true;
}

bool NFSClient::rename_path(const std::string &old_path, const std::string &new_path) {
  if (!this->is_mounted()) {
    ESP_LOGW(TAG, "Not mounted, cannot rename");
    return false;
  }

  // Resolve parent directories and filenames for both paths
  NFSFileHandle old_parent_fh;
  std::string old_name;
  if (!this->resolve_parent_path_(old_path, old_parent_fh, old_name)) {
    ESP_LOGW(TAG, "Failed to resolve old path: %s", old_path.c_str());
    return false;
  }

  NFSFileHandle new_parent_fh;
  std::string new_name;
  if (!this->resolve_parent_path_(new_path, new_parent_fh, new_name)) {
    ESP_LOGW(TAG, "Failed to resolve new path: %s", new_path.c_str());
    return false;
  }

  // Use NFS RENAME RPC call
  return this->nfs_rename_(old_parent_fh, old_name, new_parent_fh, new_name);
}
#endif

}  // namespace nfs_client
}  // namespace esphome
