#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace esphome::sip_phone {

struct Span {
  const char *p{nullptr};
  uint16_t n{0};
  bool empty() const { return this->n == 0; }
  bool equals(const char *text) const;
  bool starts_with(const char *text) const;
  int to_int(int fallback = -1) const;
  size_t copy_to(char *dst, size_t cap) const;
};

Span make_span(const char *text);
Span trim(Span s);
Span header_param(Span value, const char *name);
Span uri_of(Span value);
Span display_name_of(Span value);
Span auth_param(Span value, const char *name);
Span uri_user(Span uri);
Span uri_host(Span uri);
uint16_t uri_port(Span uri, uint16_t fallback);

class SipMessage {
 public:
  static constexpr uint8_t MAX_HEADERS = 40;

  bool parse(const char *buf, size_t len);
  bool is_request() const { return this->status_ == 0; }
  Span method() const { return this->method_; }
  Span request_uri() const { return this->uri_; }
  int status() const { return this->status_; }
  Span header(const char *name, uint8_t index = 0) const;
  uint8_t header_count(const char *name) const;
  Span body() const { return this->body_; }
  int cseq_number() const;
  Span cseq_method() const;

 protected:
  struct Header {
    Span name;
    Span value;
  };
  Header headers_[MAX_HEADERS];
  uint8_t count_{0};
  Span method_;
  Span uri_;
  int status_{0};
  Span body_;
};

class SipWriter {
 public:
  SipWriter(char *buf, size_t cap) : buf_(buf), cap_(cap) {}
  void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void add_span(Span s);
  size_t length() const { return this->len_; }
  bool ok() const { return this->ok_; }
  char *buffer() { return this->buf_; }

 protected:
  char *buf_;
  size_t cap_;
  size_t len_{0};
  bool ok_{true};
};

}
