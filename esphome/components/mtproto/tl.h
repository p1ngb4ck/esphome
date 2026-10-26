#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::mtproto {

extern const uint8_t TL_SCHEMA[];

constexpr uint32_t tl_hash(const char *text, uint32_t value = 2166136261u) {
  return *text == '\0' ? value : tl_hash(text + 1, (value ^ static_cast<uint8_t>(*text)) * 16777619u);
}

static constexpr uint32_t TL_VECTOR = 0x1cb5c415;

class TlWriter {
 public:
  TlWriter(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

  void u32(uint32_t value);
  void i32(int32_t value) { this->u32(static_cast<uint32_t>(value)); }
  void u64(uint64_t value);
  void i64(int64_t value) { this->u64(static_cast<uint64_t>(value)); }
  void raw(const uint8_t *data, size_t length);
  void bytes(const uint8_t *data, size_t length);
  void string(const char *text);
  void vector(uint32_t count) {
    this->u32(TL_VECTOR);
    this->u32(count);
  }

  size_t size() const { return this->size_; }
  bool ok() const { return this->ok_; }
  uint8_t *data() { return this->buffer_; }

 protected:
  bool reserve_(size_t length);

  uint8_t *buffer_;
  size_t capacity_;
  size_t size_{0};
  bool ok_{true};
};

class TlReader {
 public:
  TlReader() = default;
  TlReader(const uint8_t *data, size_t length) : pos_(data), end_(data + length) {}

  bool u32(uint32_t &value);
  bool i32(int32_t &value);
  bool u64(uint64_t &value);
  bool i64(int64_t &value);
  bool raw(uint8_t *out, size_t length);
  bool bytes(const uint8_t *&data, size_t &length);
  bool skip(size_t length);
  bool peek_u32(uint32_t &value) const;

  const uint8_t *pos() const { return this->pos_; }
  size_t remaining() const { return this->pos_ == nullptr ? 0 : static_cast<size_t>(this->end_ - this->pos_); }
  bool valid() const { return this->pos_ != nullptr; }

 protected:
  const uint8_t *pos_{nullptr};
  const uint8_t *end_{nullptr};
};

bool tl_skip_object(TlReader &reader, int depth = 0);
bool tl_known(uint32_t constructor);
bool tl_field(TlReader object, uint32_t name, TlReader &field);

}
