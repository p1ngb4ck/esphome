#include "s3_client.h"
#ifdef USE_ESP_IDF

#include "esphome/components/storage/storage.h"
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"

#ifdef USE_CERT_STORE
#include "esphome/components/cert_store/cert_store.h"
#endif

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include <cinttypes>
#include <lwip/netdb.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace esphome {
namespace s3_client {

static const char *const TAG = "s3_client";

static constexpr uint32_t IO_TIMEOUT_MS = 10000;
// list/stat response bodies (XML) are bounded; a single ListObjectsV2 page with 1000 keys stays
// well under this, and we page with max-keys anyway.
static constexpr size_t XML_ACCUM_LIMIT = 16384;
static constexpr const char *UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

// ---------------------------------------------------------------------------
// S3Connection
// ---------------------------------------------------------------------------

bool S3Connection::open(const char *host, uint16_t port, uint32_t timeout_ms) {
  this->timeout_ms_ = timeout_ms;
  // DNS + numeric addresses alike via getaddrinfo (bsd_sockets on ESP-IDF) -- the same client
  // pattern ftp_client documents; ESPHome has no portable resolver of its own.
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);
  struct addrinfo *res = nullptr;
  if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS/resolve failed for %s", host);
    return false;
  }
  this->sock_ = esphome::socket::socket_ip(SOCK_STREAM, 0);
  if (this->sock_ == nullptr) {
    freeaddrinfo(res);
    return false;
  }
  this->sock_->setblocking(true);
  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  this->sock_->setsockopt(SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  this->sock_->setsockopt(SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  int rc = this->sock_->connect(res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  return rc == 0;
}

#ifdef USE_CERT_STORE
int S3Connection::tls_send_(void *ctx, const unsigned char *buf, size_t len) {
  auto *self = static_cast<S3Connection *>(ctx);
  ssize_t n = self->sock_->write(buf, len);
  if (n < 0)
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  return static_cast<int>(n);
}
int S3Connection::tls_recv_(void *ctx, unsigned char *buf, size_t len) {
  auto *self = static_cast<S3Connection *>(ctx);
  ssize_t n = self->sock_->read(buf, len);
  if (n < 0)
    return MBEDTLS_ERR_SSL_WANT_READ;
  if (n == 0)
    return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
  return static_cast<int>(n);
}

bool S3Connection::start_tls(const char *host, const char *ca_entry) {
  mbedtls_ssl_init(&this->ssl_);
  mbedtls_ssl_config_init(&this->conf_);
  mbedtls_x509_crt_init(&this->cacert_);
  mbedtls_ctr_drbg_init(&this->ctr_drbg_);
  mbedtls_entropy_init(&this->entropy_);
  this->tls_active_ = true;  // from here on, close() must free the contexts

  if (mbedtls_ctr_drbg_seed(&this->ctr_drbg_, mbedtls_entropy_func, &this->entropy_, nullptr, 0) != 0)
    return false;
  if (mbedtls_ssl_config_defaults(&this->conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    return false;

  // The CA comes from cert_store, which owns the source (embedded flash / storage / built-in
  // bundle) and streams it through the worker if needed -- we never hold or parse the PEM here.
  // apply_ca_async is async; we run on the worker task (start_tls is reached only via the
  // worker-routed mount / data path), so waiting for its one-shot completion here does not block
  // the main loop. cacert_ lives as long as this connection, which is what mbedTLS needs.
  if (cert_store::global_cert_store == nullptr) {
    ESP_LOGE(TAG, "TLS requested but no cert_store configured");
    return false;
  }
  volatile bool ca_done = false;
  volatile bool ca_ok = false;
  cert_store::global_cert_store->apply_ca_async(&this->conf_, &this->cacert_, esphome::StringRef(ca_entry),
                                                [&ca_done, &ca_ok](storage::StorageError e) {
                                                  ca_ok = (e == storage::StorageError::OK);
                                                  ca_done = true;
                                                });
  // The storage path completes from the worker's stream callbacks, which are pumped by the
  // component loop; embedded/bundle complete inline (ca_done already true here).
  uint32_t start = millis();
  while (!ca_done) {
    if (millis() - start > 10000) {
      ESP_LOGE(TAG, "CA application timed out");
      return false;
    }
    App.feed_wdt();
  }
  if (!ca_ok) {
    ESP_LOGE(TAG, "CA application failed");
    return false;
  }
  mbedtls_ssl_conf_authmode(&this->conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_rng(&this->conf_, mbedtls_ctr_drbg_random, &this->ctr_drbg_);
  if (mbedtls_ssl_setup(&this->ssl_, &this->conf_) != 0)
    return false;
  if (mbedtls_ssl_set_hostname(&this->ssl_, host) != 0)
    return false;
  mbedtls_ssl_set_bio(&this->ssl_, this, tls_send_, tls_recv_, nullptr);
  int ret;
  while ((ret = mbedtls_ssl_handshake(&this->ssl_)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      ESP_LOGE(TAG, "TLS handshake failed (-0x%04x)", -ret);
      return false;
    }
  }
  return true;
}
#endif  // USE_CERT_STORE

bool S3Connection::send_all(const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
#ifdef USE_CERT_STORE
    if (this->tls_active_) {
      int n = mbedtls_ssl_write(&this->ssl_, data + sent, len - sent);
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      if (n <= 0)
        return false;
      sent += static_cast<size_t>(n);
      continue;
    }
#endif
    ssize_t n = this->sock_->write(data + sent, len - sent);
    if (n <= 0)
      return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

int S3Connection::recv_some(uint8_t *buf, size_t len) {
#ifdef USE_CERT_STORE
  if (this->tls_active_) {
    // Bounded retry loop -- recursing here overflows the stack on a stalled connection.
    for (int attempt = 0; attempt < 64; attempt++) {
      int n = mbedtls_ssl_read(&this->ssl_, buf, len);
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        continue;
      if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
      return n;  // >0 data, <0 error
    }
    return -1;
  }
#endif
  ssize_t n = this->sock_->read(buf, len);
  return static_cast<int>(n);
}

void S3Connection::close() {
#ifdef USE_CERT_STORE
  if (this->tls_active_) {
    mbedtls_ssl_close_notify(&this->ssl_);
    mbedtls_ssl_free(&this->ssl_);
    mbedtls_ssl_config_free(&this->conf_);
    mbedtls_x509_crt_free(&this->cacert_);
    mbedtls_ctr_drbg_free(&this->ctr_drbg_);
    mbedtls_entropy_free(&this->entropy_);
    this->tls_active_ = false;
  }
#endif
  if (this->sock_ != nullptr) {
    this->sock_->close();
    this->sock_.reset();
  }
}

// ---------------------------------------------------------------------------
// small helpers: hashing, hex, dates
// ---------------------------------------------------------------------------

static void sha256_hex(const uint8_t *data, size_t len, char out_hex[65]) {
  uint8_t digest[32];
  mbedtls_sha256(data, len, digest, 0);
  static const char *hexd = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out_hex[i * 2] = hexd[digest[i] >> 4];
    out_hex[i * 2 + 1] = hexd[digest[i] & 0xF];
  }
  out_hex[64] = '\0';
}

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md, key, key_len, msg, msg_len, out);
}

// "Tue, 03 Jun 2025 10:00:00 GMT" (RFC 7231) -> epoch; 0 on parse failure.
static uint32_t parse_http_date(const char *s) {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  struct tm tmv {};
  // Skip the weekday ("Tue, ").
  const char *p = strchr(s, ',');
  if (p == nullptr)
    return 0;
  p++;
  if (sscanf(p, " %d %3s %d %d:%d:%d", &tmv.tm_mday, mon, &tmv.tm_year, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6)
    return 0;
  const char *m = strstr(months, mon);
  if (m == nullptr)
    return 0;
  tmv.tm_mon = static_cast<int>((m - months) / 3);
  tmv.tm_year -= 1900;
  // timegm() equivalent: days-since-epoch arithmetic, avoiding timezone state.
  static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int y = tmv.tm_year + 1900;
  int64_t days = (y - 1970) * 365LL + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cum[tmv.tm_mon] + (tmv.tm_mday - 1);
  if (tmv.tm_mon >= 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
    days += 1;
  return static_cast<uint32_t>(days * 86400 + tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
}

// ISO8601 "2025-06-03T10:00:00.000Z" (ListObjectsV2 LastModified) -> epoch; 0 on failure.
static uint64_t parse_iso8601(const char *s) {
  struct tm tmv {};
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min,
             &tmv.tm_sec) != 6)
    return 0;
  static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int y = tmv.tm_year;
  int mon = tmv.tm_mon - 1;
  if (mon < 0 || mon > 11)
    return 0;
  int64_t days = (y - 1970) * 365LL + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cum[mon] + (tmv.tm_mday - 1);
  if (mon >= 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
    days += 1;
  return static_cast<uint64_t>(days * 86400 + tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
}

// Minimal XML scan: value of the first <tag>...</tag> at/after *pos; advances *pos past it.
static bool xml_next(const std::string &xml, const char *tag, size_t *pos, std::string *out) {
  std::string open = std::string("<") + tag + ">";
  std::string close = std::string("</") + tag + ">";
  size_t a = xml.find(open, *pos);
  if (a == std::string::npos)
    return false;
  a += open.size();
  size_t b = xml.find(close, a);
  if (b == std::string::npos)
    return false;
  out->assign(xml, a, b - a);
  *pos = b + close.size();
  return true;
}

// ---------------------------------------------------------------------------
// S3Client: paths, signing, requests
// ---------------------------------------------------------------------------

std::string S3Client::key_of_(const char *path) const {
  const char *p = path;
  while (*p == '/')
    p++;
  return std::string(p);
}

std::string S3Client::uri_encode_(const char *s, bool keep_slash) {
  static const char *hexd = "0123456789ABCDEF";
  std::string out;
  for (const char *p = s; *p != '\0'; p++) {
    char ch = *p;
    bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                      ch == '-' || ch == '_' || ch == '.' || ch == '~';
    if (unreserved || (keep_slash && ch == '/')) {
      out += ch;
    } else {
      out += '%';
      out += hexd[(static_cast<uint8_t>(ch)) >> 4];
      out += hexd[static_cast<uint8_t>(ch) & 0xF];
    }
  }
  return out;
}

std::string S3Client::host_() const {
  if (this->path_style_)
    return this->endpoint_;
  return this->bucket_ + "." + this->endpoint_;
}

std::string S3Client::uri_for_(const std::string &key_enc) const {
  // base_path_ is already normalised (starts with '/', no trailing '/', empty means none), so the
  // parts join with single slashes -- never "//", which some proxies and SigV4 verifiers reject.
  if (this->path_style_)
    return this->base_path_ + "/" + this->bucket_ + "/" + key_enc;
  return this->base_path_ + "/" + key_enc;
}

uint32_t S3Client::now_epoch_() const {
  return static_cast<uint32_t>(static_cast<int64_t>(::time(nullptr)) + this->clock_offset_);
}

void S3Client::signing_key_(const char *yyyymmdd, uint8_t out[32]) {
  if (strcmp(this->signing_day_, yyyymmdd) == 0) {
    memcpy(out, this->signing_key_cache_, 32);
    return;
  }
  // AWS4 key derivation chain, cached per UTC day.
  std::string k0 = "AWS4" + this->secret_key_;
  uint8_t k1[32], k2[32], k3[32];
  hmac_sha256(reinterpret_cast<const uint8_t *>(k0.data()), k0.size(),
              reinterpret_cast<const uint8_t *>(yyyymmdd), strlen(yyyymmdd), k1);
  hmac_sha256(k1, 32, reinterpret_cast<const uint8_t *>(this->region_.data()), this->region_.size(), k2);
  hmac_sha256(k2, 32, reinterpret_cast<const uint8_t *>("s3"), 2, k3);
  hmac_sha256(k3, 32, reinterpret_cast<const uint8_t *>("aws4_request"), 12, out);
  memcpy(this->signing_key_cache_, out, 32);
  strncpy(this->signing_day_, yyyymmdd, sizeof(this->signing_day_) - 1);
}

std::string S3Client::authorization_(const char *method, const std::string &canonical_uri, const std::string &query,
                                     const std::string &host, const char *amz_date, const char *yyyymmdd,
                                     const char *amz_name, const char *amz_value) {
  // SigV4 requires EVERY x-amz-* header on the wire to be signed; an unsigned one is a
  // guaranteed SignatureDoesNotMatch (bit us with x-amz-copy-source on rename). The optional
  // extra header slots alphabetically between x-amz-content-sha256 and x-amz-date -- true for
  // x-amz-copy-source, asserted implicitly by keeping the set fixed otherwise.
  std::string canonical = std::string(method) + "\n" + canonical_uri + "\n" + query + "\n" +
                          "host:" + host + "\n" +
                          "x-amz-content-sha256:" + UNSIGNED_PAYLOAD + "\n";
  std::string signed_headers = "host;x-amz-content-sha256;";
  if (amz_name != nullptr) {
    canonical += std::string(amz_name) + ":" + amz_value + "\n";
    signed_headers += std::string(amz_name) + ";";
  }
  canonical += std::string("x-amz-date:") + amz_date + "\n\n";
  signed_headers += "x-amz-date";
  canonical += signed_headers + "\n" + UNSIGNED_PAYLOAD;
  char creq_hash[65];
  sha256_hex(reinterpret_cast<const uint8_t *>(canonical.data()), canonical.size(), creq_hash);
  std::string scope = std::string(yyyymmdd) + "/" + this->region_ + "/s3/aws4_request";
  std::string to_sign = std::string("AWS4-HMAC-SHA256\n") + amz_date + "\n" + scope + "\n" + creq_hash;
  uint8_t key[32], sig[32];
  this->signing_key_(yyyymmdd, key);
  hmac_sha256(key, 32, reinterpret_cast<const uint8_t *>(to_sign.data()), to_sign.size(), sig);
  static const char *hexd = "0123456789abcdef";
  char sig_hex[65];
  for (int i = 0; i < 32; i++) {
    sig_hex[i * 2] = hexd[sig[i] >> 4];
    sig_hex[i * 2 + 1] = hexd[sig[i] & 0xF];
  }
  sig_hex[64] = '\0';
  return std::string("AWS4-HMAC-SHA256 Credential=") + this->access_key_ + "/" + scope +
         ", SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=" + sig_hex;
}

storage::StorageError S3Client::map_status_(int status) const {
  if (status >= 200 && status < 300)
    return storage::StorageError::OK;
  switch (status) {
    case 404:
      return storage::StorageError::NOT_FOUND;
    case 401:
    case 403:
      return storage::StorageError::PERMISSION_DENIED;
    case 416:
      return storage::StorageError::INVALID_ARGS;
    case 409:
      return storage::StorageError::ALREADY_EXISTS;
    default:
      return status >= 500 ? storage::StorageError::READ_ERROR : storage::StorageError::NOT_SUPPORTED;
  }
}

storage::StorageError S3Client::request_(const char *method, const std::string &key_enc, const std::string &query,
                                         const uint8_t *body, size_t body_len, const char *extra_header, uint8_t *out,
                                         size_t out_cap, size_t *out_len, std::string *accum, size_t accum_limit,
                                         HttpResponse *resp, bool retrying_skew) {
  if (!this->ensure_mounted_())
    return storage::StorageError::NOT_READY;
  std::string host = this->host_();
  std::string uri = this->uri_for_(key_enc);

  uint32_t epoch = this->now_epoch_();
  time_t t = static_cast<time_t>(epoch);
  struct tm g {};
  gmtime_r(&t, &g);
  char amz_date[24], yyyymmdd[9];
  snprintf(amz_date, sizeof(amz_date), "%04u%02u%02uT%02u%02u%02uZ", static_cast<unsigned>(g.tm_year + 1900) % 10000,
           static_cast<unsigned>(g.tm_mon + 1) % 100, static_cast<unsigned>(g.tm_mday) % 100,
           static_cast<unsigned>(g.tm_hour) % 100, static_cast<unsigned>(g.tm_min) % 100,
           static_cast<unsigned>(g.tm_sec) % 100);
  memcpy(yyyymmdd, amz_date, 8);
  yyyymmdd[8] = '\0';

  // An extra header starting with x-amz- must participate in the signature (SigV4 rule);
  // anything else (Range) stays unsigned. extra_header arrives as "Name: value".
  std::string amz_name, amz_value;
  if (extra_header != nullptr && strncasecmp(extra_header, "x-amz-", 6) == 0) {
    const char *colon = strchr(extra_header, ':');
    if (colon != nullptr) {
      amz_name.assign(extra_header, colon - extra_header);
      for (auto &ch2 : amz_name)
        ch2 = static_cast<char>(tolower(static_cast<unsigned char>(ch2)));
      const char *v = colon + 1;
      while (*v == ' ')
        v++;
      amz_value = v;
    }
  }
  std::string auth = this->authorization_(method, uri, query, host, amz_date, yyyymmdd,
                                          amz_name.empty() ? nullptr : amz_name.c_str(),
                                          amz_name.empty() ? nullptr : amz_value.c_str());

  std::string req = std::string(method) + " " + uri + (query.empty() ? "" : "?" + query) + " HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" + "x-amz-date: " + std::string(amz_date) + "\r\n" +
                    "x-amz-content-sha256: " + UNSIGNED_PAYLOAD + "\r\n" + "Authorization: " + auth + "\r\n" +
                    "Connection: close\r\n";
  if (extra_header != nullptr && extra_header[0] != '\0')
    req += std::string(extra_header) + "\r\n";
  char clen[48];
  snprintf(clen, sizeof(clen), "Content-Length: %u\r\n", static_cast<unsigned>(body_len));
  req += clen;
  req += "\r\n";

  S3Connection conn;
  if (!conn.open(this->endpoint_.c_str(), this->port_, IO_TIMEOUT_MS))
    return storage::StorageError::NOT_READY;
#ifdef USE_CERT_STORE
  if (this->tls_) {
    if (!conn.start_tls(host.c_str(), this->ca_entry_.c_str()))
      return storage::StorageError::NOT_READY;
  }
#endif
  if (!conn.send_all(reinterpret_cast<const uint8_t *>(req.data()), req.size()))
    return storage::StorageError::WRITE_ERROR;
  if (body_len > 0 && !conn.send_all(body, body_len))
    return storage::StorageError::WRITE_ERROR;

  // ---- response: status line + headers ----
  std::string head;
  head.reserve(512);
  uint8_t ch;
  // Read byte-wise until the blank line; header sections are tiny compared to any payload.
  while (head.size() < 4096) {
    int n = conn.recv_some(&ch, 1);
    if (n <= 0)
      return storage::StorageError::READ_ERROR;
    head += static_cast<char>(ch);
    if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0)
      break;
  }
  HttpResponse r{};
  if (sscanf(head.c_str(), "HTTP/%*d.%*d %d", &r.status) != 1)
    return storage::StorageError::READ_ERROR;
  // Header lines of interest (case per S3/MinIO practice; fall back to lowercase probes).
  auto header_val = [&head](const char *name) -> std::string {
    size_t pos = 0;
    size_t nlen = strlen(name);
    while ((pos = head.find("\r\n", pos)) != std::string::npos) {
      pos += 2;
      if (strncasecmp(head.c_str() + pos, name, nlen) == 0 && head[pos + nlen] == ':') {
        size_t v = pos + nlen + 1;
        while (v < head.size() && head[v] == ' ')
          v++;
        size_t e = head.find("\r\n", v);
        return head.substr(v, e - v);
      }
    }
    return "";
  };
  std::string v = header_val("Content-Length");
  if (!v.empty()) {
    r.has_content_length = true;
    r.content_length = strtoull(v.c_str(), nullptr, 10);
  }
  v = header_val("Transfer-Encoding");
  if (v.find("chunked") != std::string::npos)
    r.chunked = true;
  v = header_val("Date");
  if (!v.empty()) {
    r.date_epoch = parse_http_date(v.c_str());
    // Keep the clock offset fresh from every response: a node without SNTP has no real time,
    // and if SNTP later steps the clock the offset learned at mount would drift. Cheap and
    // self-correcting -- the next request always signs against the server's own clock.
    if (r.date_epoch != 0)
      this->clock_offset_ = static_cast<int32_t>(static_cast<int64_t>(r.date_epoch) -
                                                 static_cast<int64_t>(::time(nullptr)));
  }
  v = header_val("Last-Modified");
  if (!v.empty())
    r.last_modified_epoch = parse_http_date(v.c_str());

  // ---- response body ----
  // 4xx/5xx bodies carry the S3 error XML (<Code>AccessDenied|SignatureDoesNotMatch|...) --
  // exactly the information a bench log needs; capture a bounded slice and log it below.
  std::string err_body;
  if (r.status >= 400 && accum == nullptr) {
    accum = &err_body;
    accum_limit = 300;
    out = nullptr;
  }
  size_t written_out = 0;
  auto consume = [&](const uint8_t *data, size_t len) -> bool {
    if (accum != nullptr) {
      if (accum->size() + len > accum_limit) {
        // Bounded capture: for error bodies keep the head and drain the rest instead of
        // failing the whole request over a diagnostics buffer.
        if (accum->size() < accum_limit)
          accum->append(reinterpret_cast<const char *>(data), accum_limit - accum->size());
        return true;
      }
      accum->append(reinterpret_cast<const char *>(data), len);
      return true;
    }
    if (out != nullptr && written_out < out_cap) {
      size_t take = std::min(len, out_cap - written_out);
      memcpy(out + written_out, data, take);
      written_out += take;
    }
    return true;  // beyond out_cap: drain silently
  };
  uint8_t buf[512];
  if (r.chunked) {
    // HTTP/1.1 chunked decoding: size line (hex), payload, CRLF, repeated; 0-chunk terminates.
    for (;;) {
      std::string line;
      while (line.size() < 32) {
        int n = conn.recv_some(&ch, 1);
        if (n <= 0)
          return storage::StorageError::READ_ERROR;
        line += static_cast<char>(ch);
        if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0)
          break;
      }
      size_t chunk = strtoul(line.c_str(), nullptr, 16);
      if (chunk == 0) {
        uint8_t crlf[2];
        conn.recv_some(crlf, 2);
        break;
      }
      while (chunk > 0) {
        int n = conn.recv_some(buf, std::min(chunk, sizeof(buf)));
        if (n <= 0)
          return storage::StorageError::READ_ERROR;
        if (!consume(buf, static_cast<size_t>(n)))
          return storage::StorageError::NO_SPACE;
        chunk -= static_cast<size_t>(n);
      }
      uint8_t crlf[2];
      if (conn.recv_some(crlf, 2) <= 0)
        return storage::StorageError::READ_ERROR;
    }
  } else {
    uint64_t remaining = r.has_content_length ? r.content_length : UINT64_MAX;
    while (remaining > 0) {
      int n = conn.recv_some(buf, static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(buf))));
      if (n < 0)
        return storage::StorageError::READ_ERROR;
      if (n == 0)
        break;  // Connection: close delimits the body when no length was sent
      if (!consume(buf, static_cast<size_t>(n)))
        return storage::StorageError::NO_SPACE;
      remaining -= static_cast<uint64_t>(n);
      App.feed_wdt();
    }
  }
  if (r.status == 403 && !retrying_skew && err_body.find("RequestTimeTooSkewed") != std::string::npos &&
      r.date_epoch != 0) {
    // The offset was already refreshed above from this response's Date; sign again with it once.
    ESP_LOGD(TAG, "retrying after clock-skew correction (offset now %" PRId32 " s)", this->clock_offset_);
    return this->request_(method, key_enc, query, body, body_len, extra_header, out, out_cap, out_len, accum,
                          accum_limit, resp, true);
  }
  if (r.status >= 400 && !err_body.empty())
    ESP_LOGW(TAG, "%s %s -> %d: %.200s", method, uri.c_str(), r.status, err_body.c_str());
  if (out_len != nullptr)
    *out_len = written_out;
  if (resp != nullptr)
    *resp = r;
  return storage::StorageError::OK;
}

// ---------------------------------------------------------------------------
// write episodes
// ---------------------------------------------------------------------------

bool S3Client::episode_matches_(const char *key) const {
  return this->episode_.active && strcmp(this->episode_.key, key) == 0;
}

bool S3Client::episode_reserve_(size_t need) {
  if (need <= this->episode_.cap)
    return true;
  size_t new_cap = this->episode_.cap == 0 ? 4096 : this->episode_.cap;
  while (new_cap < need)
    new_cap *= 2;
  uint8_t *n = this->episode_alloc_.allocate(new_cap);
  if (n == nullptr)
    return false;
  if (this->episode_.data != nullptr) {
    memcpy(n, this->episode_.data, this->episode_.size);
    this->episode_alloc_.deallocate(this->episode_.data, this->episode_.cap);
  }
  this->episode_.data = n;
  this->episode_.cap = new_cap;
  return true;
}

void S3Client::drop_episode_() {
  if (this->episode_.data != nullptr)
    this->episode_alloc_.deallocate(this->episode_.data, this->episode_.cap);
  this->episode_ = WriteEpisode{};
}

storage::StorageError S3Client::flush_episode_() {
  if (!this->episode_.active)
    return storage::StorageError::OK;
  std::string key_enc = uri_encode_(this->episode_.key, true);
  HttpResponse r{};
  storage::StorageError err =
      this->request_("PUT", key_enc, "", this->episode_.data, this->episode_.size, nullptr, nullptr, 0, nullptr,
                     nullptr, 0, &r);
  if (err == storage::StorageError::OK)
    err = this->map_status_(r.status);
  if (err != storage::StorageError::OK)
    ESP_LOGE(TAG, "flush of '%s' (%u bytes) failed (%s)", this->episode_.key,
             static_cast<unsigned>(this->episode_.size), storage::error_to_string(err));
  else
    ESP_LOGD(TAG, "uploaded '%s' (%u bytes)", this->episode_.key, static_cast<unsigned>(this->episode_.size));
  this->drop_episode_();
  return err;
}

storage::StorageError S3Client::flush_if_key_(const char *key) {
  if (this->episode_matches_(key))
    return this->flush_episode_();
  return storage::StorageError::OK;
}

// ---------------------------------------------------------------------------
// NetworkStorage / PathStorage operations
// ---------------------------------------------------------------------------

storage::StorageError S3Client::truncate(const char *path, uint64_t size) {
  std::string key = this->key_of_(path);
  if (size != 0) {
    // S3 cannot resize an object in place; only the interface's create/overwrite signal (0).
    return storage::StorageError::NOT_SUPPORTED;
  }
  // Episode start: flush whatever other key is in flight, then open a fresh empty episode. The
  // upload happens on episode end -- an immediate empty PUT here would be wasted for the common
  // "truncate then write chunks" sequence the interface guarantees.
  if (this->episode_.active && !this->episode_matches_(key.c_str())) {
    storage::StorageError err = this->flush_episode_();
    if (err != storage::StorageError::OK)
      return err;
  }
  this->drop_episode_();
  if (key.size() >= sizeof(this->episode_.key))
    return storage::StorageError::INVALID_ARGS;
  strncpy(this->episode_.key, key.c_str(), sizeof(this->episode_.key) - 1);
  this->episode_.active = true;
  this->episode_.size = 0;
  this->episode_.last_ms = millis();
  return storage::StorageError::OK;
}

storage::StorageError S3Client::write_chunk(const char *path, const uint8_t *buf, uint64_t offset, size_t len,
                                            size_t *bytes_transferred) {
  *bytes_transferred = 0;
  std::string key = this->key_of_(path);
  if (this->episode_matches_(key.c_str())) {
    if (offset != this->episode_.size) {
      // The interface delivers episode chunks strictly sequentially; anything else is a caller bug.
      ESP_LOGE(TAG, "non-sequential write to '%s' (offset %" PRIu64 ", episode at %u)", key.c_str(), offset,
               static_cast<unsigned>(this->episode_.size));
      return storage::StorageError::INVALID_ARGS;
    }
  } else if (offset == 0) {
    // write_file()-style whole-object write without a preceding truncate: start an episode.
    storage::StorageError err = this->truncate(path, 0);
    if (err != storage::StorageError::OK)
      return err;
  } else {
    // Out-of-episode chunk at an arbitrary offset: the append_file() pattern (stat then write at
    // EOF). One read-modify-write: fetch the existing object into a fresh episode, then append.
    storage::StorageError err = this->flush_episode_();  // an unrelated episode may be open
    if (err != storage::StorageError::OK)
      return err;
    storage::FileStat st{};
    err = this->stat(path, &st);
    if (err != storage::StorageError::OK)
      return err;
    if (st.size != offset) {
      ESP_LOGE(TAG, "offset write into '%s' unsupported (S3 objects are immutable)", key.c_str());
      return storage::StorageError::NOT_SUPPORTED;
    }
    err = this->truncate(path, 0);
    if (err != storage::StorageError::OK)
      return err;
    if (st.size > 0) {
      if (!this->episode_reserve_(static_cast<size_t>(st.size)))
        return storage::StorageError::NO_SPACE;
      size_t got = 0;
      std::string key_enc = uri_encode_(key.c_str(), true);
      char range[64];
      snprintf(range, sizeof(range), "Range: bytes=0-%" PRIu64, st.size - 1);
      HttpResponse r{};
      err = this->request_("GET", key_enc, "", nullptr, 0, range, this->episode_.data,
                           static_cast<size_t>(st.size), &got, nullptr, 0, &r);
      if (err == storage::StorageError::OK)
        err = this->map_status_(r.status);
      if (err != storage::StorageError::OK) {
        this->drop_episode_();
        return err;
      }
      this->episode_.size = got;
    }
  }
  if (!this->episode_reserve_(this->episode_.size + len))
    return storage::StorageError::NO_SPACE;
  memcpy(this->episode_.data + this->episode_.size, buf, len);
  this->episode_.size += len;
  this->episode_.last_ms = millis();
  *bytes_transferred = len;
  return storage::StorageError::OK;
}

storage::StorageError S3Client::read_chunk(const char *path, uint8_t *buf, uint64_t offset, size_t len,
                                           size_t *bytes_transferred) {
  *bytes_transferred = 0;
  std::string key = this->key_of_(path);
  storage::StorageError err = this->flush_if_key_(key.c_str());
  if (err != storage::StorageError::OK)
    return err;
  std::string key_enc = uri_encode_(key.c_str(), true);
  char range[80];
  snprintf(range, sizeof(range), "Range: bytes=%" PRIu64 "-%" PRIu64, offset, offset + len - 1);
  HttpResponse r{};
  size_t got = 0;
  err = this->request_("GET", key_enc, "", nullptr, 0, range, buf, len, &got, nullptr, 0, &r);
  if (err != storage::StorageError::OK)
    return err;
  if (r.status == 416) {
    // Range fully past the end: EOF, not an error (mirrors read() returning 0).
    return storage::StorageError::OK;
  }
  err = this->map_status_(r.status);
  if (err != storage::StorageError::OK)
    return err;
  *bytes_transferred = got;
  return storage::StorageError::OK;
}

storage::StorageError S3Client::stat(const char *path, storage::FileStat *st) {
  std::string key = this->key_of_(path);
  *st = storage::FileStat{};
  storage::StorageError err = this->flush_if_key_(key.c_str());
  if (err != storage::StorageError::OK)
    return err;
  // The mount root is a directory by definition.
  if (key.empty()) {
    st->is_dir = true;
    return storage::StorageError::OK;
  }
  const char *base = strrchr(key.c_str(), '/');
  strncpy(st->name, base != nullptr ? base + 1 : key.c_str(), sizeof(st->name) - 1);
  // 1) object with this exact key -> file
  std::string key_enc = uri_encode_(key.c_str(), true);
  HttpResponse r{};
  err = this->request_("HEAD", key_enc, "", nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, &r);
  if (err != storage::StorageError::OK)
    return err;
  if (r.status >= 200 && r.status < 300) {
    st->size = r.content_length;
    st->mtime = static_cast<uint32_t>(r.last_modified_epoch);
    st->is_dir = false;
    return storage::StorageError::OK;
  }
  if (r.status != 404)
    return this->map_status_(r.status);
  // 2) marker object or any key under the prefix -> directory
  std::string probe;
  std::string query = "list-type=2&max-keys=1&prefix=" + uri_encode_((key + "/").c_str(), false);
  err = this->request_("GET", std::string(""), query, nullptr, 0, nullptr, nullptr, 0, nullptr, &probe,
                       XML_ACCUM_LIMIT, &r);
  if (err != storage::StorageError::OK)
    return err;
  if (r.status < 200 || r.status >= 300)
    return this->map_status_(r.status);
  if (probe.find("<Key>") != std::string::npos) {
    st->is_dir = true;
    return storage::StorageError::OK;
  }
  return storage::StorageError::NOT_FOUND;
}

storage::StorageError S3Client::list_dir(const char *path, bool (*callback)(const storage::FileStat *entry, void *ctx),
                                         void *ctx) {
  std::string key = this->key_of_(path);
  storage::StorageError err = this->flush_episode_();  // anything pending becomes visible first
  if (err != storage::StorageError::OK)
    return err;
  std::string prefix = key.empty() ? "" : key + "/";
  std::string token;
  for (;;) {
    // Query parameters in canonical (sorted) order -- the same string signs and ships.
    std::string query;
    if (!token.empty())
      query += "continuation-token=" + uri_encode_(token.c_str(), false) + "&";
    query += "delimiter=%2F&list-type=2&max-keys=200";
    if (!prefix.empty())
      query += "&prefix=" + uri_encode_(prefix.c_str(), false);
    std::string xml;
    HttpResponse r{};
    err = this->request_("GET", std::string(""), query, nullptr, 0, nullptr, nullptr, 0, nullptr, &xml,
                         XML_ACCUM_LIMIT, &r);
    if (err != storage::StorageError::OK)
      return err;
    err = this->map_status_(r.status);
    if (err != storage::StorageError::OK)
      return err;

    // Files: <Contents><Key>k</Key>...<Size>n</Size><LastModified>...</LastModified></Contents>
    size_t pos = 0;
    std::string val;
    while (true) {
      size_t entry_pos = xml.find("<Contents>", pos);
      if (entry_pos == std::string::npos)
        break;
      pos = entry_pos;
      std::string k, sz, lm;
      size_t scan = pos;
      if (!xml_next(xml, "Key", &scan, &k))
        break;
      size_t scan2 = pos;
      xml_next(xml, "Size", &scan2, &sz);
      size_t scan3 = pos;
      xml_next(xml, "LastModified", &scan3, &lm);
      pos = scan;
      if (k == prefix)
        continue;  // the directory's own marker object is not an entry
      storage::FileStat st{};
      const char *name = k.c_str() + prefix.size();
      strncpy(st.name, name, sizeof(st.name) - 1);
      st.size = strtoull(sz.c_str(), nullptr, 10);
      st.mtime = static_cast<uint32_t>(parse_iso8601(lm.c_str()));
      st.is_dir = false;
      if (!callback(&st, ctx))
        return storage::StorageError::OK;
    }
    // Directories: <CommonPrefixes><Prefix>a/b/</Prefix></CommonPrefixes>
    pos = 0;
    while (true) {
      size_t cp = xml.find("<CommonPrefixes>", pos);
      if (cp == std::string::npos)
        break;
      pos = cp + 1;
      size_t scan = cp;
      if (!xml_next(xml, "Prefix", &scan, &val))
        break;
      // "prefix/sub/" -> "sub"
      std::string sub = val.substr(prefix.size());
      if (!sub.empty() && sub.back() == '/')
        sub.pop_back();
      storage::FileStat st{};
      strncpy(st.name, sub.c_str(), sizeof(st.name) - 1);
      st.is_dir = true;
      if (!callback(&st, ctx))
        return storage::StorageError::OK;
    }
    size_t tp = 0;
    if (!xml_next(xml, "NextContinuationToken", &tp, &token) || token.empty())
      break;
  }
  return storage::StorageError::OK;
}

storage::StorageError S3Client::mkdir(const char *path) {
  std::string key = this->key_of_(path) + "/";
  std::string key_enc = uri_encode_(key.c_str(), true);
  HttpResponse r{};
  storage::StorageError err =
      this->request_("PUT", key_enc, "", nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, &r);
  if (err != storage::StorageError::OK)
    return err;
  return this->map_status_(r.status);
}

storage::StorageError S3Client::rmdir(const char *path) {
  std::string key = this->key_of_(path);
  storage::StorageError err = this->flush_episode_();
  if (err != storage::StorageError::OK)
    return err;
  // Refuse when anything but the marker lives under the prefix (mirrors POSIX ENOTEMPTY).
  std::string probe;
  HttpResponse r{};
  std::string query = "list-type=2&max-keys=2&prefix=" + uri_encode_((key + "/").c_str(), false);
  err = this->request_("GET", std::string(""), query, nullptr, 0, nullptr, nullptr, 0, nullptr, &probe,
                       XML_ACCUM_LIMIT, &r);
  if (err != storage::StorageError::OK)
    return err;
  size_t pos = 0;
  std::string k;
  while (xml_next(probe, "Key", &pos, &k)) {
    if (k != key + "/")
      return storage::StorageError::NOT_EMPTY;
  }
  std::string key_enc = uri_encode_((key + "/").c_str(), true);
  err = this->request_("DELETE", key_enc, "", nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, &r);
  if (err != storage::StorageError::OK)
    return err;
  return this->map_status_(r.status);
}

storage::StorageError S3Client::remove(const char *path) {
  std::string key = this->key_of_(path);
  if (this->episode_matches_(key.c_str()))
    this->drop_episode_();  // deleting the file being written: the episode is void, not flushed
  // S3 DELETE succeeds for absent keys; stat first so NOT_FOUND is reported honestly.
  storage::FileStat st{};
  storage::StorageError err = this->stat(path, &st);
  if (err != storage::StorageError::OK)
    return err;
  if (st.is_dir)
    return storage::StorageError::NOT_SUPPORTED;  // directories go through rmdir()
  std::string key_enc = uri_encode_(key.c_str(), true);
  HttpResponse r{};
  err = this->request_("DELETE", key_enc, "", nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, &r);
  if (err != storage::StorageError::OK)
    return err;
  return this->map_status_(r.status);
}

storage::StorageError S3Client::rename(const char *old_path, const char *new_path) {
  std::string old_key = this->key_of_(old_path);
  std::string new_key = this->key_of_(new_path);
  storage::StorageError err = this->flush_if_key_(old_key.c_str());
  if (err != storage::StorageError::OK)
    return err;
  storage::FileStat st{};
  err = this->stat(old_path, &st);
  if (err != storage::StorageError::OK)
    return err;
  if (st.is_dir) {
    // A server-side recursive move would be one copy+delete per object; the worker's tree ops
    // handle that generically, so a flat rename() keeps to single objects.
    return storage::StorageError::NOT_SUPPORTED;
  }
  // Copy the object through the interface primitives (GET the source, PUT it under the new
  // key via a write episode) and delete the source -- the same read_chunk/write_chunk/remove
  // path the worker's cross-device copy uses, which is proven to work against the server.
  // A server-side CopyObject (x-amz-copy-source) would be cheaper but is deliberately avoided:
  // it is the only operation that needs an extra signed header, and the object here is small
  // (this backs the staged-upload finalize rename, a handful of bytes to a few KB). st.size is
  // known from the stat above.
  err = this->truncate(new_path, 0);  // opens a write episode for the new key
  if (err != storage::StorageError::OK)
    return err;
  uint64_t copied = 0;
  while (copied < st.size) {
    uint8_t buf[2048];
    size_t want = static_cast<size_t>(st.size - copied < sizeof(buf) ? st.size - copied : sizeof(buf));
    size_t got = 0;
    err = this->read_chunk(old_path, buf, copied, want, &got);
    if (err != storage::StorageError::OK) {
      this->drop_episode_();
      return err;
    }
    if (got == 0)
      break;
    if (!this->episode_reserve_(this->episode_.size + got)) {
      this->drop_episode_();
      return storage::StorageError::NO_SPACE;
    }
    memcpy(this->episode_.data + this->episode_.size, buf, got);
    this->episode_.size += got;
    copied += got;
    App.feed_wdt();
  }
  err = this->flush_episode_();  // one PUT of the new object -- the path copy already exercises
  if (err != storage::StorageError::OK)
    return err;
  // Source gone only after the destination is safely written.
  std::string old_enc = uri_encode_(old_key.c_str(), true);
  HttpResponse r{};
  err = this->request_("DELETE", old_enc, "", nullptr, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, &r);
  if (err != storage::StorageError::OK)
    return err;
  return this->map_status_(r.status);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

storage::StorageError S3Client::get_info(storage::StorageInfo *info) {
  if (info == nullptr)
    return storage::StorageError::INVALID_ARGS;
  // Storage contract: get_info() must succeed even while registered-but-unmounted and report
  // that via is_mounted -- never via a non-OK error, never with a server round-trip.
  info->id = this->get_mount_path();
  info->name = "S3";
  info->kind = "s3";
  info->block_size = 0;
  info->is_removable = false;
  info->is_read_only = false;
  info->is_mounted = this->mounted_;
  info->total_bytes = 0;  // S3 reports no medium capacity; the browser suppresses zero sizes
  info->free_bytes = 0;
  return storage::StorageError::OK;
}

bool S3Client::ensure_mounted_() {
  if (this->mounted_)
    return true;
  // Never block here: control-plane calls (list/stat from the browser) run on the MAIN LOOP,
  // so an inline probe would freeze it -- the exact failure the async-mount routing exists to
  // prevent. Kick off one deduplicated background attempt and report not-ready; the caller's
  // operation returns NOT_READY now, the change feed / next poll sees the share once the mount
  // lands. Registered-but-unmounted is a normal, queryable state.
  this->request_async_mount_();
  return false;
}


void S3Client::request_async_mount_() {
  if (this->mounted_ || this->mount_pending_)
    return;
#ifdef USE_STORAGE_WORKER
  if (storage::global_storage_worker != nullptr) {
    this->mount_pending_ = true;
    storage::StorageError sub = storage::global_storage_worker->async_mount(
        this, [this](storage::StorageError /*err*/) { this->mount_pending_ = false; });
    if (sub != storage::StorageError::OK)
      this->mount_pending_ = false;
    return;
  }
#endif
  // No worker in this build: one bounded blocking attempt is the only option (nfs_client-style).
  this->mount();
}

storage::StorageError S3Client::mount() {
  if (this->mounted_)
    return storage::StorageError::OK;
  // One synchronous blocking probe. Callers route this through the worker's async_mount()
  // (see loop()/ensure_mounted_()), so on a worker build this body executes on the WORKER
  // TASK -- the main loop stays free no matter how long resolve/connect/TLS take. Without a
  // worker the direct call remains one bounded attempt, nfs_client-style.
  // Probe the endpoint and learn the server clock: SigV4 tolerates only ~15 minutes of skew and
  // a node without an SNTP source has no idea what time it is. The unauthenticated probe's 403
  // still carries a Date: header -- that is all we need.
  S3Connection conn;
  if (!conn.open(this->endpoint_.c_str(), this->port_, IO_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "endpoint %s:%u not reachable", this->endpoint_.c_str(), this->port_);
    return storage::StorageError::NOT_READY;
  }
#ifdef USE_CERT_STORE
  if (this->tls_) {
    if (!conn.start_tls(this->host_().c_str(), this->ca_entry_.c_str()))
      return storage::StorageError::NOT_READY;
  }
#endif
  // Probe the proxied prefix, not "/": behind a reverse proxy "/" is the front site, not the
  // bucket endpoint. base_path_ empty -> "/". This request is unsigned (reachability + clock only).
  std::string probe_path = this->base_path_.empty() ? "/" : this->base_path_ + "/";
  std::string req = "GET " + probe_path + " HTTP/1.1\r\nHost: " + this->host_() + "\r\nConnection: close\r\n\r\n";
  if (!conn.send_all(reinterpret_cast<const uint8_t *>(req.data()), req.size()))
    return storage::StorageError::WRITE_ERROR;
  std::string head;
  uint8_t ch;
  while (head.size() < 2048) {
    int n = conn.recv_some(&ch, 1);
    if (n <= 0)
      break;
    head += static_cast<char>(ch);
    if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0)
      break;
  }
  size_t dp = head.find("\r\nDate:");
  if (dp == std::string::npos)
    dp = head.find("\r\ndate:");
  if (dp != std::string::npos) {
    uint32_t server = parse_http_date(head.c_str() + dp + 7);
    if (server != 0) {
      this->clock_offset_ = static_cast<int32_t>(static_cast<int64_t>(server) - static_cast<int64_t>(::time(nullptr)));
      if (this->clock_offset_ != 0)
        ESP_LOGD(TAG, "clock offset to server: %" PRId32 " s", this->clock_offset_);
    }
  }
  this->mounted_ = true;
  this->signing_day_[0] = '\0';  // re-derive against the corrected clock

  // The unauthenticated probe only proves reachability and syncs the clock -- a share with
  // wrong credentials or a missing bucket would still "mount" and every later operation would
  // fail. Verify real access with one signed ListObjectsV2 before reporting success.
  // mounted_ was set provisionally above because request_() gates on it; reverted on failure.
  std::string probe_xml;
  HttpResponse vr{};
  storage::StorageError verr = this->request_("GET", std::string(""), "list-type=2&max-keys=1", nullptr, 0, nullptr,
                                              nullptr, 0, nullptr, &probe_xml, XML_ACCUM_LIMIT, &vr);
  if (verr == storage::StorageError::OK)
    verr = this->map_status_(vr.status);
  if (verr != storage::StorageError::OK) {
    this->mounted_ = false;
    ESP_LOGW(TAG, "S3 mount rejected: bucket '%s' not accessible (%s)", this->bucket_.c_str(),
             storage::error_to_string(verr));
    return verr;
  }
  ESP_LOGI(TAG, "mounted s3://%s at %s", this->bucket_.c_str(), this->get_mount_path());
  return storage::StorageError::OK;
}

storage::StorageError S3Client::unmount() {
  if (!this->mounted_)
    return storage::StorageError::OK;
  // Quiesce first: the drain guarantees no in-flight worker call remains, which makes this the
  // safe place to flush a pending write episode.
  // Quiesce (drain guarantee: no in-flight worker call remains), flush the pending episode,
  // then simply mark unmounted -- the device STAYS registered (see setup()).
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->quiesce_storage(this);
  storage::StorageError err = this->flush_episode_();
  this->mounted_ = false;
  ESP_LOGI(TAG, "unmounted s3://%s", this->bucket_.c_str());
  return err;
}

void S3Client::setup() {
  // Register with the storage registry now: registered-but-unmounted is the normal state
  // for a mountable device (see nfs_client/sd_storage). The registry entry, the codegen
  // mount-path table and every consumer stay consistent from boot on; mount()/unmount()
  // only toggle the connection state.
  if (storage::global_storage_registry != nullptr) {
    if (storage::global_storage_registry->register_storage(this) != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Storage registration failed");
      this->mark_failed();
    }
  }
}

void S3Client::loop() {
  // auto_connect: ONE mount attempt per rising edge of network connectivity (NFS pattern).
  // The attempt goes through the worker's async_mount(), so resolve/connect/TLS/probe run on
  // the worker task -- the loop only submits. Direct call only without a worker in the build.
  bool connected = esphome::network::is_connected();
  if (this->auto_connect_ && connected && !this->was_connected_ && !this->mounted_)
    this->request_async_mount_();
  this->was_connected_ = connected;
  // Idle safety net: an episode nobody ended (e.g. a caller that never reads back) uploads after
  // a quiet period, so data cannot sit in RAM indefinitely.
  if (this->episode_.active && millis() - this->episode_.last_ms > EPISODE_IDLE_FLUSH_MS)
    this->flush_episode_();
}

void S3Client::dump_config() {
  ESP_LOGCONFIG(TAG, "S3 client:");
  ESP_LOGCONFIG(TAG, "  Endpoint: %s:%u", this->endpoint_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Bucket: %s (region %s, %s style)", this->bucket_.c_str(), this->region_.c_str(),
                this->path_style_ ? "path" : "virtual-hosted");
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->get_mount_path());
#ifdef USE_CERT_STORE
  ESP_LOGCONFIG(TAG, "  TLS: %s", this->tls_ ? "yes" : "no");
#endif
}

}  // namespace s3_client
}  // namespace esphome

#endif  // USE_ESP_IDF
