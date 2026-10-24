#include "sip_message.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace esphome::sip_phone {

static const char *const COMPACT_FORMS[][2] = {
    {"v", "Via"},          {"f", "From"},           {"t", "To"},      {"i", "Call-ID"}, {"m", "Contact"},
    {"l", "Content-Length"}, {"c", "Content-Type"}, {"k", "Supported"}, {"e", "Content-Encoding"},
};

static bool ieq(const char *a, size_t an, const char *b) {
  const size_t bn = strlen(b);
  if (an != bn)
    return false;
  for (size_t i = 0; i < an; i++) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

static bool name_matches(Span name, const char *wanted) {
  if (ieq(name.p, name.n, wanted))
    return true;
  for (const auto &form : COMPACT_FORMS) {
    if (ieq(form[1], strlen(form[1]), wanted) && ieq(name.p, name.n, form[0]))
      return true;
  }
  return false;
}

bool Span::equals(const char *text) const { return this->p != nullptr && ieq(this->p, this->n, text); }

bool Span::starts_with(const char *text) const {
  const size_t len = strlen(text);
  return this->p != nullptr && this->n >= len && ieq(this->p, len, text);
}

int Span::to_int(int fallback) const {
  if (this->n == 0)
    return fallback;
  int value = 0;
  bool any = false;
  for (uint16_t i = 0; i < this->n; i++) {
    const char c = this->p[i];
    if (c < '0' || c > '9')
      break;
    value = value * 10 + (c - '0');
    any = true;
  }
  return any ? value : fallback;
}

size_t Span::copy_to(char *dst, size_t cap) const {
  if (cap == 0)
    return 0;
  const size_t len = this->n < cap - 1 ? this->n : cap - 1;
  if (len > 0)
    memcpy(dst, this->p, len);
  dst[len] = '\0';
  return len;
}

Span make_span(const char *text) {
  Span s;
  s.p = text;
  s.n = text == nullptr ? 0 : static_cast<uint16_t>(strlen(text));
  return s;
}

Span trim(Span s) {
  while (s.n > 0 && (*s.p == ' ' || *s.p == '\t')) {
    s.p++;
    s.n--;
  }
  while (s.n > 0 && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' || s.p[s.n - 1] == '\r'))
    s.n--;
  return s;
}

static Span sub(const char *from, const char *to) {
  Span s;
  s.p = from;
  s.n = static_cast<uint16_t>(to - from);
  return s;
}

Span header_param(Span value, const char *name) {
  const char *end = value.p + value.n;
  const char *cursor = value.p;
  const char *close = static_cast<const char *>(memchr(value.p, '>', value.n));
  if (close != nullptr)
    cursor = close + 1;
  const size_t name_len = strlen(name);
  while (cursor < end) {
    const char *semi = static_cast<const char *>(memchr(cursor, ';', end - cursor));
    if (semi == nullptr)
      break;
    const char *start = semi + 1;
    while (start < end && *start == ' ')
      start++;
    const char *stop = start;
    while (stop < end && *stop != ';' && *stop != ',')
      stop++;
    const char *eq = static_cast<const char *>(memchr(start, '=', stop - start));
    const char *key_end = eq != nullptr ? eq : stop;
    const Span key = trim(sub(start, key_end));
    if (key.n == name_len && ieq(key.p, key.n, name)) {
      if (eq == nullptr)
        return sub(stop, stop);
      Span v = trim(sub(eq + 1, stop));
      if (v.n >= 2 && v.p[0] == '"' && v.p[v.n - 1] == '"') {
        v.p++;
        v.n -= 2;
      }
      return v;
    }
    cursor = stop;
  }
  return Span{};
}

Span uri_of(Span value) {
  const char *open = static_cast<const char *>(memchr(value.p, '<', value.n));
  if (open != nullptr) {
    const char *end = value.p + value.n;
    const char *close = static_cast<const char *>(memchr(open, '>', end - open));
    if (close != nullptr)
      return sub(open + 1, close);
  }
  Span t = trim(value);
  const char *semi = static_cast<const char *>(memchr(t.p, ';', t.n));
  return semi != nullptr ? sub(t.p, semi) : t;
}

Span display_name_of(Span value) {
  Span t = trim(value);
  if (t.n > 0 && t.p[0] == '"') {
    const char *close = static_cast<const char *>(memchr(t.p + 1, '"', t.n - 1));
    if (close != nullptr)
      return sub(t.p + 1, close);
  }
  const char *open = static_cast<const char *>(memchr(t.p, '<', t.n));
  if (open != nullptr)
    return trim(sub(t.p, open));
  return Span{};
}

Span auth_param(Span value, const char *name) {
  const char *end = value.p + value.n;
  const char *cursor = value.p;
  const char *space = static_cast<const char *>(memchr(value.p, ' ', value.n));
  if (space != nullptr)
    cursor = space + 1;
  const size_t name_len = strlen(name);
  while (cursor < end) {
    while (cursor < end && (*cursor == ' ' || *cursor == ','))
      cursor++;
    const char *eq = static_cast<const char *>(memchr(cursor, '=', end - cursor));
    if (eq == nullptr)
      break;
    Span key = trim(sub(cursor, eq));
    const char *vstart = eq + 1;
    const char *vend;
    Span v;
    if (vstart < end && *vstart == '"') {
      vstart++;
      vend = static_cast<const char *>(memchr(vstart, '"', end - vstart));
      if (vend == nullptr)
        vend = end;
      v = sub(vstart, vend);
      cursor = vend < end ? vend + 1 : end;
    } else {
      vend = static_cast<const char *>(memchr(vstart, ',', end - vstart));
      if (vend == nullptr)
        vend = end;
      v = trim(sub(vstart, vend));
      cursor = vend;
    }
    if (key.n == name_len && ieq(key.p, key.n, name))
      return v;
  }
  return Span{};
}

Span uri_user(Span uri) {
  const char *colon = static_cast<const char *>(memchr(uri.p, ':', uri.n));
  const char *start = colon != nullptr ? colon + 1 : uri.p;
  const char *end = uri.p + uri.n;
  const char *at = static_cast<const char *>(memchr(start, '@', end - start));
  if (at == nullptr)
    return Span{};
  return sub(start, at);
}

Span uri_host(Span uri) {
  const char *end = uri.p + uri.n;
  const char *colon = static_cast<const char *>(memchr(uri.p, ':', uri.n));
  const char *start = colon != nullptr ? colon + 1 : uri.p;
  const char *at = static_cast<const char *>(memchr(start, '@', end - start));
  if (at != nullptr)
    start = at + 1;
  const char *stop = start;
  while (stop < end && *stop != ':' && *stop != ';' && *stop != '>' && *stop != '?')
    stop++;
  return sub(start, stop);
}

uint16_t uri_port(Span uri, uint16_t fallback) {
  Span host = uri_host(uri);
  const char *end = uri.p + uri.n;
  const char *after = host.p + host.n;
  if (after < end && *after == ':') {
    Span port = sub(after + 1, end);
    const int value = port.to_int(-1);
    if (value > 0 && value < 65536)
      return static_cast<uint16_t>(value);
  }
  return fallback;
}

bool SipMessage::parse(const char *buf, size_t len) {
  this->count_ = 0;
  this->status_ = 0;
  this->method_ = Span{};
  this->uri_ = Span{};
  this->body_ = Span{};
  const char *end = buf + len;
  const char *line_end = static_cast<const char *>(memchr(buf, '\n', len));
  if (line_end == nullptr)
    return false;
  Span first = trim(sub(buf, line_end));
  if (first.starts_with("SIP/2.0 ")) {
    this->status_ = Span{first.p + 8, static_cast<uint16_t>(first.n - 8)}.to_int(0);
    if (this->status_ < 100)
      return false;
  } else {
    const char *sp1 = static_cast<const char *>(memchr(first.p, ' ', first.n));
    if (sp1 == nullptr)
      return false;
    const char *rest = sp1 + 1;
    const char *sp2 = static_cast<const char *>(memchr(rest, ' ', first.p + first.n - rest));
    if (sp2 == nullptr)
      return false;
    this->method_ = sub(first.p, sp1);
    this->uri_ = sub(rest, sp2);
  }
  const char *cursor = line_end + 1;
  while (cursor < end) {
    const char *nl = static_cast<const char *>(memchr(cursor, '\n', end - cursor));
    const char *stop = nl != nullptr ? nl : end;
    Span line = sub(cursor, stop);
    if (line.n > 0 && line.p[line.n - 1] == '\r')
      line.n--;
    if (line.n == 0) {
      cursor = nl != nullptr ? nl + 1 : end;
      break;
    }
    if ((line.p[0] == ' ' || line.p[0] == '\t') && this->count_ > 0) {
      Header &prev = this->headers_[this->count_ - 1];
      prev.value.n = static_cast<uint16_t>(line.p + line.n - prev.value.p);
    } else {
      const char *colon = static_cast<const char *>(memchr(line.p, ':', line.n));
      if (colon != nullptr && this->count_ < MAX_HEADERS) {
        Header &h = this->headers_[this->count_++];
        h.name = trim(sub(line.p, colon));
        h.value = trim(sub(colon + 1, line.p + line.n));
      }
    }
    cursor = nl != nullptr ? nl + 1 : end;
  }
  const int content_length = this->header("Content-Length").to_int(-1);
  size_t body_len = static_cast<size_t>(end - cursor);
  if (content_length >= 0 && static_cast<size_t>(content_length) < body_len)
    body_len = static_cast<size_t>(content_length);
  this->body_ = sub(cursor, cursor + body_len);
  return true;
}

Span SipMessage::header(const char *name, uint8_t index) const {
  for (uint8_t i = 0; i < this->count_; i++) {
    if (name_matches(this->headers_[i].name, name)) {
      if (index == 0)
        return this->headers_[i].value;
      index--;
    }
  }
  return Span{};
}

uint8_t SipMessage::header_count(const char *name) const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < this->count_; i++) {
    if (name_matches(this->headers_[i].name, name))
      count++;
  }
  return count;
}

int SipMessage::cseq_number() const { return this->header("CSeq").to_int(-1); }

Span SipMessage::cseq_method() const {
  Span cseq = this->header("CSeq");
  const char *space = static_cast<const char *>(memchr(cseq.p, ' ', cseq.n));
  if (space == nullptr)
    return Span{};
  return trim(sub(space + 1, cseq.p + cseq.n));
}

void SipWriter::add(const char *fmt, ...) {
  if (!this->ok_)
    return;
  va_list args;
  va_start(args, fmt);
  const int written = vsnprintf(this->buf_ + this->len_, this->cap_ - this->len_, fmt, args);
  va_end(args);
  if (written < 0 || static_cast<size_t>(written) >= this->cap_ - this->len_) {
    this->ok_ = false;
    return;
  }
  this->len_ += written;
}

void SipWriter::add_span(Span s) {
  if (!this->ok_)
    return;
  if (s.n >= this->cap_ - this->len_) {
    this->ok_ = false;
    return;
  }
  memcpy(this->buf_ + this->len_, s.p, s.n);
  this->len_ += s.n;
  this->buf_[this->len_] = '\0';
}

}
