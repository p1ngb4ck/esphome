#include "ftp_client.h"

#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_TCP)

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

// DNS: ESPHome has no portable resolver, so use each socket backend's own facility (same choice
// the other network components make). bsd_sockets and lwip_sockets provide getaddrinfo; lwip_tcp
// (esp8266 / rp2) has no socket layer, so it uses lwip's raw resolver dns_gethostbyname_addrtype,
// exactly like mqtt.
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS)
#include <netdb.h>
#elif defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
#include "lwip/netdb.h"
#elif defined(USE_SOCKET_IMPL_LWIP_TCP)
#include "esphome/components/network/ip_address.h"
#include "lwip/dns.h"
#endif

#include <sys/time.h>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace esphome {
namespace ftp_client {

static const char *const TAG = "ftp_client";

static constexpr uint32_t FTP_TIMEOUT_MS = 10000;
static constexpr uint32_t FTP_INLINE_MOUNT_MIN_INTERVAL_MS = 3000;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static const char *basename_of(const std::string &path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

static void fill_name(storage::FileStat *out, const char *name) {
  strncpy(out->name, name, storage::STORAGE_NAME_MAX);
  out->name[storage::STORAGE_NAME_MAX] = '\0';
}

static void configure_socket(socket::Socket *sock) {
  struct timeval tv;
  tv.tv_sec = FTP_TIMEOUT_MS / 1000;
  tv.tv_usec = (FTP_TIMEOUT_MS % 1000) * 1000;
  sock->setsockopt(SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sock->setsockopt(SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sock->setblocking(true);
}

#if defined(USE_SOCKET_IMPL_LWIP_TCP)
namespace {
struct FtpDnsResult {
  volatile bool done{false};
  volatile bool ok{false};
  ip_addr_t addr{};
};
// The lwip DNS callback prototype gained a const on the ip_addr_t across lwip versions.
#if defined(USE_ESP8266) && LWIP_VERSION_MAJOR == 1
void ftp_dns_found(const char *name, ip_addr_t *ipaddr, void *arg) {
#else
void ftp_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg) {
#endif
  (void) name;
  auto *r = (FtpDnsResult *) arg;
  if (ipaddr != nullptr) {
    r->addr = *ipaddr;
    r->ok = true;
  }
  r->done = true;
}
}  // namespace
#endif  // USE_SOCKET_IMPL_LWIP_TCP

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void FTPClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up FTP client...");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->get_mount_path());

  // Register before the first connect, like nfs_client/sd_storage: the device is visible to
  // path routing and the mount/unmount actions even while unmounted (get_info() reports the
  // mounted state).
  if (storage::global_storage_registry != nullptr) {
    if (storage::global_storage_registry->register_storage(this) != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Storage registration failed");
      this->mark_failed();
    }
  }
}

void FTPClient::loop() {
  // Auto-connect on each rising edge of network connectivity, same event-edge pattern as
  // nfs_client -- no periodic retry.
  const bool net_connected = network::is_connected();
  if (this->auto_connect_ && net_connected && !this->network_was_connected_ && !this->connected_) {
    this->mount_requested_ = true;
  }
  this->network_was_connected_ = net_connected;

  if (this->mount_requested_ && !this->connected_) {
    this->mount_requested_ = false;
    this->do_connect_();
  }
}

void FTPClient::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP client:");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Username: %s", this->username_.c_str());
  // Password is a secret -- never logged, same as the wifi password.
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->get_mount_path());
  ESP_LOGCONFIG(TAG, "  Auto connect: %s", YESNO(this->auto_connect_));
  ESP_LOGCONFIG(TAG, "  Status: %s", this->connected_ ? "connected" : "disconnected");
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

bool FTPClient::ensure_connected_() {
  if (this->connected_)
    return true;
  // Rate-limited inline connect so a dead server does not turn every action into a fresh
  // stack of socket timeouts (mirrors nfs_client::ensure_mounted_).
  uint32_t now = millis();
  if (now - this->last_inline_mount_ms_ < FTP_INLINE_MOUNT_MIN_INTERVAL_MS)
    return false;
  this->last_inline_mount_ms_ = now;
  return this->do_connect_() == storage::StorageError::OK;
}

std::unique_ptr<socket::Socket> FTPClient::open_tcp_(const std::string &host, uint16_t port) {
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
  // getaddrinfo path (bsd_sockets / lwip_sockets). Resolves hostnames and numeric IPs alike,
  // so it also connects the PASV data address.
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;  // IPv4 only for now
  hints.ai_socktype = SOCK_STREAM;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS/resolve failed for %s", host.c_str());
    return nullptr;
  }

  std::unique_ptr<socket::Socket> sock = socket::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock == nullptr) {
    freeaddrinfo(res);
    return nullptr;
  }
  configure_socket(sock.get());
  int rc = sock->connect(res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  return rc == 0 ? std::move(sock) : nullptr;

#elif defined(USE_SOCKET_IMPL_LWIP_TCP)
  // lwip raw resolver path (esp8266 / rp2): no getaddrinfo, so resolve with lwip's own DNS.
  FtpDnsResult dns;
  err_t err;
  {
    LwIPLock lock;
#if USE_NETWORK_IPV6
    err = dns_gethostbyname_addrtype(host.c_str(), &dns.addr, ftp_dns_found, &dns, LWIP_DNS_ADDRTYPE_IPV6_IPV4);
#else
    err = dns_gethostbyname_addrtype(host.c_str(), &dns.addr, ftp_dns_found, &dns, LWIP_DNS_ADDRTYPE_IPV4);
#endif
  }
  if (err == ERR_OK) {
    dns.ok = true;
    dns.done = true;
  } else if (err == ERR_INPROGRESS) {
    uint32_t start = millis();
    while (!dns.done && millis() - start < FTP_TIMEOUT_MS) {
      delay(1);
      App.feed_wdt();
    }
  } else {
    ESP_LOGW(TAG, "DNS/resolve failed for %s", host.c_str());
    return nullptr;
  }
  if (!dns.ok) {
    ESP_LOGW(TAG, "DNS/resolve failed for %s", host.c_str());
    return nullptr;
  }

  network::IPAddress ip(&dns.addr);
  char ip_buf[network::IP_ADDRESS_BUFFER_SIZE];
  ip.str_to(ip_buf);

  struct sockaddr_storage ss;
  socklen_t sl = socket::set_sockaddr((struct sockaddr *) &ss, sizeof(ss), ip_buf, port);
  std::unique_ptr<socket::Socket> sock = socket::socket_ip(SOCK_STREAM, 0);
  if (sock == nullptr)
    return nullptr;
  configure_socket(sock.get());
  return sock->connect((struct sockaddr *) &ss, sl) == 0 ? std::move(sock) : nullptr;

#else
  return nullptr;
#endif
}

// Wraps an ESPHome socket so the FTP protocol code reads/writes the same way whether or not the
// connection is TLS. TLS is mbedTLS and only exists on ESP-IDF.
ssize_t FtpStream::read(void *buf, size_t len) {
#ifdef USE_CERT_STORE
  if (this->tls_) {
    int ret = mbedtls_ssl_read(&this->ssl_, static_cast<unsigned char *>(buf), len);
    if (ret > 0)
      return ret;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
      return 0;  // clean EOF
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      errno = EAGAIN;
      return -1;
    }
    return -1;
  }
#endif
  if (this->sock_ == nullptr)
    return -1;
  return this->sock_->read(buf, len);
}

ssize_t FtpStream::write(const void *buf, size_t len) {
#ifdef USE_CERT_STORE
  if (this->tls_) {
    int ret = mbedtls_ssl_write(&this->ssl_, static_cast<const unsigned char *>(buf), len);
    if (ret >= 0)
      return ret;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      errno = EAGAIN;
      return -1;
    }
    return -1;
  }
#endif
  if (this->sock_ == nullptr)
    return -1;
  return this->sock_->write(buf, len);
}

void FtpStream::close() {
#ifdef USE_CERT_STORE
  if (this->tls_) {
    mbedtls_ssl_close_notify(&this->ssl_);
    mbedtls_ssl_free(&this->ssl_);
    this->tls_ = false;
  }
#endif
  this->sock_.reset();
}

#ifdef USE_CERT_STORE
int FtpStream::bio_send_(void *ctx, const unsigned char *buf, size_t len) {
  auto *sock = static_cast<socket::Socket *>(ctx);
  ssize_t n = sock->write(buf, len);
  if (n > 0)
    return static_cast<int>(n);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  return -1;
}

int FtpStream::bio_recv_(void *ctx, unsigned char *buf, size_t len) {
  auto *sock = static_cast<socket::Socket *>(ctx);
  ssize_t n = sock->read(buf, len);
  if (n > 0)
    return static_cast<int>(n);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return MBEDTLS_ERR_SSL_WANT_READ;
  return -1;
}

bool FtpStream::start_tls(mbedtls_ssl_config *conf, const char *hostname) {
  if (this->sock_ == nullptr)
    return false;
  mbedtls_ssl_init(&this->ssl_);
  if (mbedtls_ssl_setup(&this->ssl_, conf) != 0) {
    mbedtls_ssl_free(&this->ssl_);
    return false;
  }
  mbedtls_ssl_set_hostname(&this->ssl_, hostname);
  mbedtls_ssl_set_bio(&this->ssl_, this->sock_.get(), &FtpStream::bio_send_, &FtpStream::bio_recv_, nullptr);
  const uint32_t start = millis();
  while (true) {
    int ret = mbedtls_ssl_handshake(&this->ssl_);
    if (ret == 0)
      break;
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      mbedtls_ssl_free(&this->ssl_);
      return false;
    }
    if (millis() - start > FTP_TIMEOUT_MS) {
      mbedtls_ssl_free(&this->ssl_);
      return false;
    }
    delay(1);
    App.feed_wdt();
  }
  this->tls_ = true;
  return true;
}
#endif  // USE_CERT_STORE

// Wraps a freshly connected socket in a stream (or nullptr if the connect failed).
static std::unique_ptr<FtpStream> make_ftp_stream(std::unique_ptr<socket::Socket> sock) {
  if (sock == nullptr)
    return nullptr;
  auto stream = std::unique_ptr<FtpStream>(new FtpStream());  // NOLINT(cppcoreguidelines-owning-memory)
  stream->set_socket(std::move(sock));
  return stream;
}

#ifdef USE_CERT_STORE
bool FTPClient::setup_tls_() {
  if (this->tls_ready_)
    return true;
  // If a CA is configured, fetch it from the single cert_store before allocating anything -- so
  // waiting for it to load from storage is a cheap no-op retry, not a leak.
  if (cert_store::global_cert_store == nullptr) {
    ESP_LOGW(TAG, "auth_tls needs a cert_store, none is configured");
    return false;
  }
  const char *ca_pem = cert_store::global_cert_store->str(this->ca_entry_);
  if (ca_pem == nullptr)
    return false;  // CA not loaded from storage yet -- ensure_connected_ comes back

  mbedtls_ssl_config_init(&this->conf_);
  mbedtls_x509_crt_init(&this->cacert_);
  mbedtls_ctr_drbg_init(&this->drbg_);
  mbedtls_entropy_init(&this->entropy_);

  if (mbedtls_ctr_drbg_seed(&this->drbg_, mbedtls_entropy_func, &this->entropy_, nullptr, 0) != 0) {
    ESP_LOGE(TAG, "RNG seed failed");
    return false;
  }
  if (mbedtls_ssl_config_defaults(&this->conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    ESP_LOGE(TAG, "TLS config failed");
    return false;
  }
  mbedtls_ssl_conf_rng(&this->conf_, mbedtls_ctr_drbg_random, &this->drbg_);

  if (mbedtls_x509_crt_parse(&this->cacert_, reinterpret_cast<const unsigned char *>(ca_pem),
                             strlen(ca_pem) + 1) != 0) {
    ESP_LOGE(TAG, "CA '%s' does not parse", this->ca_entry_);
    return false;
  }
  mbedtls_ssl_conf_ca_chain(&this->conf_, &this->cacert_, nullptr);
  mbedtls_ssl_conf_authmode(&this->conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
  this->tls_ready_ = true;
  return true;
}
#endif  // USE_CERT_STORE

storage::StorageError FTPClient::do_connect_() {
  if (this->connected_)
    return storage::StorageError::OK;
  if (!network::is_connected())
    return storage::StorageError::NOT_READY;

#ifdef USE_CERT_STORE
  if (this->auth_tls_ && !this->setup_tls_())
    return storage::StorageError::NOT_READY;  // e.g. the CA is not loaded from storage yet
#endif

  this->control_ = make_ftp_stream(this->open_tcp_(this->server_, this->port_));
  if (this->control_ == nullptr) {
    ESP_LOGW(TAG, "Control connection to %s:%u failed", this->server_.c_str(), this->port_);
    return storage::StorageError::NOT_READY;
  }

  // Greeting
  if (this->read_reply_(nullptr) != 220) {
    ESP_LOGW(TAG, "No 220 greeting");
    this->control_close_();
    return storage::StorageError::NOT_READY;
  }

#ifdef USE_CERT_STORE
  // FTPS (AUTH TLS): upgrade the already-open control channel before logging in.
  if (this->auth_tls_) {
    if (this->send_cmd_("AUTH TLS") != 234) {
      ESP_LOGW(TAG, "AUTH TLS refused");
      this->control_close_();
      return storage::StorageError::NOT_READY;
    }
    if (!this->control_->start_tls(&this->conf_, this->server_.c_str())) {
      ESP_LOGW(TAG, "Control TLS handshake failed");
      this->control_close_();
      return storage::StorageError::NOT_READY;
    }
  }
#endif

  int c = this->send_cmd_("USER " + this->username_);
  if (c == 331) {
    c = this->send_cmd_("PASS " + this->password_);
  }
  if (c != 230) {
    ESP_LOGW(TAG, "Login failed (code %d)", c);
    this->control_close_();
    return storage::StorageError::PERMISSION_DENIED;
  }

#ifdef USE_CERT_STORE
  // Protect the data channel too: PBSZ 0 then PROT P, so every PASV transfer is TLS.
  if (this->auth_tls_) {
    this->send_cmd_("PBSZ 0");
    if (this->send_cmd_("PROT P") != 200) {
      ESP_LOGW(TAG, "PROT P refused -- refusing a cleartext data channel");
      this->control_close_();
      return storage::StorageError::NOT_READY;
    }
  }
#endif

  // Binary transfers.
  this->send_cmd_("TYPE I");

  this->connected_ = true;
  ESP_LOGI(TAG, "Connected to %s:%u as %s", this->server_.c_str(), this->port_, this->username_.c_str());
  return storage::StorageError::OK;
}

void FTPClient::do_disconnect_() {
  if (this->control_ != nullptr) {
    this->send_cmd_("QUIT");
    this->control_close_();
  }
  this->connected_ = false;
}

void FTPClient::control_close_() { this->control_.reset(); }

// ---------------------------------------------------------------------------
// FTP control protocol
// ---------------------------------------------------------------------------

int FTPClient::read_reply_(std::string *text) {
  if (this->control_ == nullptr)
    return -1;
  int code = -1;
  bool have_code = false;
  uint32_t start = millis();

  while (true) {
    std::string line;
    // Read one CRLF-terminated line.
    while (true) {
      char ch;
      ssize_t n = this->control_->read(&ch, 1);
      if (n == 1) {
        if (ch == '\n')
          break;
        if (ch != '\r')
          line.push_back(ch);
        continue;
      }
      if (n == 0)
        return -1;  // peer closed
      if (millis() - start > FTP_TIMEOUT_MS)
        return -1;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        delay(1);
        App.feed_wdt();
        continue;
      }
      return -1;
    }

    if (text != nullptr) {
      *text += line;
      text->push_back('\n');
    }

    if (line.size() >= 3 && isdigit((unsigned char) line[0]) && isdigit((unsigned char) line[1]) &&
        isdigit((unsigned char) line[2])) {
      int this_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
      char sep = line.size() >= 4 ? line[3] : ' ';
      if (!have_code) {
        code = this_code;
        have_code = true;
        if (sep != '-')
          return code;  // single-line reply
        // else: multi-line, keep reading until the terminating "code " line
      } else if (this_code == code && sep == ' ') {
        return code;  // end of multi-line reply
      }
    }
    // Non-code or intermediate continuation line: keep going.
  }
}

int FTPClient::send_cmd_(const std::string &cmd, std::string *reply_text) {
  if (this->control_ == nullptr)
    return -1;
  std::string line = cmd + "\r\n";
  if (!this->send_all_(this->control_.get(), reinterpret_cast<const uint8_t *>(line.data()), line.size()))
    return -1;
  return this->read_reply_(reply_text);
}

bool FTPClient::send_all_(FtpStream *stream, const uint8_t *data, size_t len) {
  if (stream == nullptr)
    return false;
  size_t sent = 0;
  uint32_t start = millis();
  while (sent < len) {
    ssize_t n = stream->write(data + sent, len - sent);
    if (n > 0) {
      sent += (size_t) n;
      continue;
    }
    if (millis() - start > FTP_TIMEOUT_MS)
      return false;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      delay(1);
      App.feed_wdt();
      continue;
    }
    return false;
  }
  return true;
}

std::unique_ptr<FtpStream> FTPClient::open_pasv_data_() {
  std::string text;
  if (this->send_cmd_("PASV", &text) != 227)
    return nullptr;

  // "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
  size_t open = text.find('(');
  if (open == std::string::npos)
    return nullptr;
  int v[6] = {0};
  if (sscanf(text.c_str() + open + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return nullptr;

  char ip[16];
  snprintf(ip, sizeof(ip), "%d.%d.%d.%d", v[0], v[1], v[2], v[3]);
  uint16_t port = (uint16_t) ((v[4] << 8) | v[5]);
  // open_tcp_ resolves the dotted-quad numerically, so it doubles as the data connector.
  auto stream = make_ftp_stream(this->open_tcp_(ip, port));
#ifdef USE_CERT_STORE
  // After PROT P the data channel is TLS too; verify against the server name, not the PASV IP.
  if (stream != nullptr && this->auth_tls_ &&
      !stream->start_tls(&this->conf_, this->server_.c_str())) {
    ESP_LOGW(TAG, "Data TLS handshake failed");
    return nullptr;
  }
#endif
  return stream;
}

std::string FTPClient::remote_path_(const char *vfs_path) const {
  std::string full(vfs_path == nullptr ? "" : vfs_path);
  const char *mp = this->get_mount_path();
  if (mp != nullptr) {
    size_t mp_len = strlen(mp);
    if (full.compare(0, mp_len, mp) == 0)
      full.erase(0, mp_len);
  }
  if (full.empty())
    full = "/";
  else if (full[0] != '/')
    full.insert(full.begin(), '/');
  return full;
}

// ---------------------------------------------------------------------------
// MountableStorage
// ---------------------------------------------------------------------------

storage::StorageError FTPClient::mount() { return this->do_connect_(); }

storage::StorageError FTPClient::unmount() {
  this->do_disconnect_();
  return storage::StorageError::OK;
}

// ---------------------------------------------------------------------------
// Storage / NetworkStorage
// ---------------------------------------------------------------------------

storage::StorageError FTPClient::get_info(storage::StorageInfo *info) {
  info->id = this->get_mount_path();
  info->name = this->server_.c_str();
  info->kind = "ftp";
  info->total_bytes = 0;  // FTP has no portable "df"
  info->free_bytes = 0;
  info->block_size = 512;
  info->is_mounted = this->connected_;
  info->is_removable = true;
  info->is_read_only = false;
  return storage::StorageError::OK;
}

storage::StorageError FTPClient::read_chunk(const char *path, uint8_t *buf, uint64_t offset, size_t len,
                                            size_t *bytes_transferred) {
  *bytes_transferred = 0;
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  std::unique_ptr<FtpStream> data = this->open_pasv_data_();
  if (data == nullptr)
    return storage::StorageError::READ_ERROR;

  if (offset > 0) {
    if (this->send_cmd_("REST " + std::to_string((unsigned long long) offset)) != 350)
      return storage::StorageError::READ_ERROR;
  }

  int c = this->send_cmd_("RETR " + remote);
  if (c != 150 && c != 125)
    return c == 550 ? storage::StorageError::NOT_FOUND : storage::StorageError::READ_ERROR;

  size_t total = 0;
  uint32_t start = millis();
  while (total < len) {
    ssize_t n = data->read(buf + total, len - total);
    if (n > 0) {
      total += (size_t) n;
      continue;
    }
    if (n == 0)
      break;  // EOF
    if (millis() - start > FTP_TIMEOUT_MS)
      break;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      delay(1);
      App.feed_wdt();
      continue;
    }
    break;
  }
  data.reset();                // close the data connection
  this->read_reply_(nullptr);  // 226/426

  *bytes_transferred = total;  // partial (< len) means EOF, per the read contract
  return storage::StorageError::OK;
}

storage::StorageError FTPClient::write_chunk(const char *path, const uint8_t *buf, uint64_t offset, size_t len,
                                             size_t *bytes_transferred) {
  *bytes_transferred = 0;
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  std::unique_ptr<FtpStream> data = this->open_pasv_data_();
  if (data == nullptr)
    return storage::StorageError::WRITE_ERROR;

  // Full-file only: the first chunk stores (truncating), later contiguous chunks append.
  const char *verb = offset == 0 ? "STOR " : "APPE ";
  int c = this->send_cmd_(std::string(verb) + remote);
  if (c != 150 && c != 125)
    return storage::StorageError::WRITE_ERROR;

  bool ok = this->send_all_(data.get(), buf, len);
  data.reset();
  int rc = this->read_reply_(nullptr);
  if (!ok || (rc != 226 && rc != 250))
    return storage::StorageError::WRITE_ERROR;

  *bytes_transferred = len;
  return storage::StorageError::OK;
}

storage::StorageError FTPClient::truncate(const char *path, uint64_t size) {
  // Full-file write only: the copy path calls truncate(path, 0) before the first chunk to
  // create/empty the file, which STOR of the first chunk then overwrites. Any non-zero
  // truncation would need random-offset rewrites, which FTP here does not support.
  if (size != 0)
    return storage::StorageError::NOT_SUPPORTED;
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  std::unique_ptr<FtpStream> data = this->open_pasv_data_();
  if (data == nullptr)
    return storage::StorageError::WRITE_ERROR;
  int c = this->send_cmd_("STOR " + remote);
  if (c != 150 && c != 125)
    return storage::StorageError::WRITE_ERROR;
  data.reset();  // close with nothing written -> empty file
  int rc = this->read_reply_(nullptr);
  return (rc == 226 || rc == 250) ? storage::StorageError::OK : storage::StorageError::WRITE_ERROR;
}

storage::StorageError FTPClient::stat(const char *path, storage::FileStat *out) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  *out = storage::FileStat{};
  fill_name(out, basename_of(remote));

  std::string text;
  int c = this->send_cmd_("SIZE " + remote, &text);
  if (c == 213) {
    out->is_dir = false;
    out->size = (uint64_t) strtoull(text.c_str() + 3, nullptr, 10);
    return storage::StorageError::OK;
  }
  // SIZE fails on directories on most servers -- probe with CWD (all our paths are absolute,
  // so a changed working directory does not affect later commands).
  if (this->send_cmd_("CWD " + remote) == 250) {
    out->is_dir = true;
    out->size = 0;
    return storage::StorageError::OK;
  }
  return storage::StorageError::NOT_FOUND;
}

storage::StorageError FTPClient::list_dir(const char *path,
                                          bool (*callback)(const storage::FileStat *entry, void *ctx), void *ctx) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  std::unique_ptr<FtpStream> data = this->open_pasv_data_();
  if (data == nullptr)
    return storage::StorageError::READ_ERROR;

  // Prefer MLSD (machine-readable). Fall back to LIST on servers that reject it.
  bool mlsd = true;
  int c = this->send_cmd_("MLSD " + remote);
  if (c != 150 && c != 125) {
    mlsd = false;
    data = this->open_pasv_data_();
    if (data == nullptr)
      return storage::StorageError::READ_ERROR;
    c = this->send_cmd_("LIST " + remote);
    if (c != 150 && c != 125)
      return c == 550 ? storage::StorageError::NOT_FOUND : storage::StorageError::READ_ERROR;
  }

  // Slurp the listing.
  std::string listing;
  char buf[512];
  uint32_t start = millis();
  while (true) {
    ssize_t n = data->read(buf, sizeof(buf));
    if (n > 0) {
      listing.append(buf, (size_t) n);
      continue;
    }
    if (n == 0)
      break;
    if (millis() - start > FTP_TIMEOUT_MS)
      break;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      delay(1);
      App.feed_wdt();
      continue;
    }
    break;
  }
  data.reset();
  this->read_reply_(nullptr);  // 226

  size_t pos = 0;
  while (pos < listing.size()) {
    size_t nl = listing.find('\n', pos);
    std::string line = listing.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? listing.size() : nl + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;

    storage::FileStat e{};
    std::string name;
    if (mlsd) {
      // "type=file;size=123;modify=YYYYMMDDHHMMSS; name"
      size_t sp = line.find(' ');
      if (sp == std::string::npos)
        continue;
      std::string facts = line.substr(0, sp);
      name = line.substr(sp + 1);
      bool is_dir = false, skip = false;
      size_t fp = 0;
      while (fp < facts.size()) {
        size_t semi = facts.find(';', fp);
        std::string fact = facts.substr(fp, semi == std::string::npos ? std::string::npos : semi - fp);
        fp = semi == std::string::npos ? facts.size() : semi + 1;
        if (fact.compare(0, 5, "type=") == 0) {
          std::string t = fact.substr(5);
          if (t == "cdir" || t == "pdir")
            skip = true;  // "." / ".."
          is_dir = (t == "dir" || t == "cdir" || t == "pdir");
        } else if (fact.compare(0, 5, "size=") == 0) {
          e.size = (uint64_t) strtoull(fact.c_str() + 5, nullptr, 10);
        }
      }
      if (skip)
        continue;
      e.is_dir = is_dir;
    } else {
      // Basic "ls -l" parse: perms, links, owner, group, size, month, day, time/year, name...
      e.is_dir = line[0] == 'd';
      int field = 0;
      size_t i = 0;
      size_t name_start = std::string::npos;
      while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
          i++;
        size_t tok = i;
        while (i < line.size() && line[i] != ' ')
          i++;
        if (field == 4)
          e.size = (uint64_t) strtoull(line.c_str() + tok, nullptr, 10);
        field++;
        if (field == 8) {
          name_start = i < line.size() ? i + 1 : std::string::npos;
          break;
        }
      }
      if (name_start == std::string::npos || name_start >= line.size())
        continue;
      name = line.substr(name_start);
      if (name == "." || name == "..")
        continue;
    }

    fill_name(&e, basename_of(name));
    if (!callback(&e, ctx))
      break;
  }

  return storage::StorageError::OK;
}

storage::StorageError FTPClient::mkdir(const char *path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  int c = this->send_cmd_("MKD " + this->remote_path_(path));
  if (c == 257)
    return storage::StorageError::OK;
  return c == 550 ? storage::StorageError::ALREADY_EXISTS : storage::StorageError::WRITE_ERROR;
}

storage::StorageError FTPClient::rmdir(const char *path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  int c = this->send_cmd_("RMD " + this->remote_path_(path));
  return c == 250 ? storage::StorageError::OK : storage::StorageError::WRITE_ERROR;
}

storage::StorageError FTPClient::remove(const char *path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  int c = this->send_cmd_("DELE " + this->remote_path_(path));
  return c == 250 ? storage::StorageError::OK : storage::StorageError::NOT_FOUND;
}

storage::StorageError FTPClient::rename(const char *old_path, const char *new_path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  if (this->send_cmd_("RNFR " + this->remote_path_(old_path)) != 350)
    return storage::StorageError::NOT_FOUND;
  int c = this->send_cmd_("RNTO " + this->remote_path_(new_path));
  return c == 250 ? storage::StorageError::OK : storage::StorageError::WRITE_ERROR;
}

}  // namespace ftp_client
}  // namespace esphome

#endif  // socket impl available
