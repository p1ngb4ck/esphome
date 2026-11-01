#include "tl.h"

#include <cstring>

namespace esphome::mtproto {

namespace {

enum Kind : uint8_t {
  KIND_INT = 0,
  KIND_LONG = 1,
  KIND_DOUBLE = 2,
  KIND_INT128 = 3,
  KIND_INT256 = 4,
  KIND_BYTES = 5,
  KIND_TRUE = 6,
  KIND_FLAGS = 7,
  KIND_OBJECT = 8,
  KIND_VECTOR = 9,
  KIND_BARE = 10,
  KIND_BARE_VECTOR = 11,
};

static constexpr uint8_t NO_FLAG = 0xFF;
static constexpr size_t PARAM_SIZE = 12;
static constexpr int MAX_DEPTH = 24;
static constexpr size_t MAX_FLAGS = 4;
static constexpr uint32_t GZIP_PACKED = 0x3072cfa1;

uint32_t load_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

const uint8_t *find_constructor(uint32_t id) {
  const uint32_t count = load_u32(TL_SCHEMA);
  uint32_t low = 0;
  uint32_t high = count;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    const uint8_t *entry = TL_SCHEMA + 4 + mid * 8;
    const uint32_t key = load_u32(entry);
    if (key == id)
      return TL_SCHEMA + load_u32(entry + 4);
    if (key < id) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return nullptr;
}

bool skip_value(TlReader &reader, uint8_t kind, uint32_t ref, int depth);

bool skip_params(TlReader &reader, const uint8_t *params, int depth) {
  const uint8_t count = params[0];
  uint32_t flags[MAX_FLAGS] = {};
  const uint8_t *param = params + 1;
  for (uint8_t i = 0; i < count; i++, param += PARAM_SIZE) {
    const uint8_t kind = param[4];
    const uint8_t flag_field = param[5];
    const uint8_t flag_bit = param[6];
    if (flag_field != NO_FLAG && (flag_field >= MAX_FLAGS || (flags[flag_field] & (1u << flag_bit)) == 0))
      continue;
    if (kind == KIND_FLAGS) {
      if (flag_bit >= MAX_FLAGS || !reader.u32(flags[flag_bit]))
        return false;
      continue;
    }
    if (kind == KIND_VECTOR || kind == KIND_BARE_VECTOR) {
      uint32_t count_items = 0;
      if (kind == KIND_VECTOR) {
        uint32_t id = 0;
        if (!reader.u32(id) || id != TL_VECTOR)
          return false;
      }
      if (!reader.u32(count_items))
        return false;
      for (uint32_t item = 0; item < count_items; item++) {
        if (!skip_value(reader, param[7], load_u32(param + 8), depth + 1))
          return false;
      }
      continue;
    }
    if (!skip_value(reader, kind, load_u32(param + 8), depth + 1))
      return false;
  }
  return true;
}

bool skip_value(TlReader &reader, uint8_t kind, uint32_t ref, int depth) {
  if (depth > MAX_DEPTH)
    return false;
  switch (kind) {
    case KIND_INT:
      return reader.skip(4);
    case KIND_LONG:
    case KIND_DOUBLE:
      return reader.skip(8);
    case KIND_INT128:
      return reader.skip(16);
    case KIND_INT256:
      return reader.skip(32);
    case KIND_BYTES: {
      const uint8_t *data = nullptr;
      size_t length = 0;
      return reader.bytes(data, length);
    }
    case KIND_TRUE:
      return true;
    case KIND_OBJECT:
      return tl_skip_object(reader, depth + 1);
    case KIND_BARE: {
      const uint8_t *params = find_constructor(ref);
      return params != nullptr && skip_params(reader, params, depth + 1);
    }
    default:
      return false;
  }
}

}

void TlWriter::u32(uint32_t value) {
  if (!this->reserve_(4))
    return;
  uint8_t *p = this->buffer_ + this->size_;
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
  this->size_ += 4;
}

void TlWriter::u64(uint64_t value) {
  this->u32(static_cast<uint32_t>(value));
  this->u32(static_cast<uint32_t>(value >> 32));
}

void TlWriter::raw(const uint8_t *data, size_t length) {
  if (!this->reserve_(length))
    return;
  memcpy(this->buffer_ + this->size_, data, length);
  this->size_ += length;
}

void TlWriter::bytes(const uint8_t *data, size_t length) {
  size_t header = 1;
  if (length < 254) {
    const uint8_t small = static_cast<uint8_t>(length);
    this->raw(&small, 1);
  } else {
    const uint8_t big[4] = {254, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                            static_cast<uint8_t>(length >> 16)};
    this->raw(big, 4);
    header = 4;
  }
  this->raw(data, length);
  static const uint8_t ZEROS[3] = {0, 0, 0};
  const size_t padding = (4 - (header + length) % 4) % 4;
  this->raw(ZEROS, padding);
}

void TlWriter::string(const char *text) {
  this->bytes(reinterpret_cast<const uint8_t *>(text), strlen(text));
}

bool TlWriter::reserve_(size_t length) {
  if (!this->ok_ || this->size_ + length > this->capacity_) {
    this->ok_ = false;
    return false;
  }
  return true;
}

bool TlReader::u32(uint32_t &value) {
  if (this->remaining() < 4) {
    this->pos_ = nullptr;
    return false;
  }
  value = load_u32(this->pos_);
  this->pos_ += 4;
  return true;
}

bool TlReader::i32(int32_t &value) {
  uint32_t raw_value = 0;
  if (!this->u32(raw_value))
    return false;
  value = static_cast<int32_t>(raw_value);
  return true;
}

bool TlReader::u64(uint64_t &value) {
  uint32_t low = 0;
  uint32_t high = 0;
  if (!this->u32(low) || !this->u32(high))
    return false;
  value = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

bool TlReader::i64(int64_t &value) {
  uint64_t raw_value = 0;
  if (!this->u64(raw_value))
    return false;
  value = static_cast<int64_t>(raw_value);
  return true;
}

bool TlReader::raw(uint8_t *out, size_t length) {
  if (this->remaining() < length) {
    this->pos_ = nullptr;
    return false;
  }
  memcpy(out, this->pos_, length);
  this->pos_ += length;
  return true;
}

bool TlReader::skip(size_t length) {
  if (this->remaining() < length) {
    this->pos_ = nullptr;
    return false;
  }
  this->pos_ += length;
  return true;
}

bool TlReader::bytes(const uint8_t *&data, size_t &length) {
  if (this->remaining() < 1) {
    this->pos_ = nullptr;
    return false;
  }
  size_t header = 1;
  length = this->pos_[0];
  if (length == 254) {
    if (this->remaining() < 4) {
      this->pos_ = nullptr;
      return false;
    }
    length = static_cast<size_t>(this->pos_[1]) | (static_cast<size_t>(this->pos_[2]) << 8) |
             (static_cast<size_t>(this->pos_[3]) << 16);
    header = 4;
  }
  const size_t total = (header + length + 3) & ~static_cast<size_t>(3);
  if (this->remaining() < total) {
    this->pos_ = nullptr;
    return false;
  }
  data = this->pos_ + header;
  this->pos_ += total;
  return true;
}

bool TlReader::peek_u32(uint32_t &value) const {
  if (this->remaining() < 4)
    return false;
  value = load_u32(this->pos_);
  return true;
}

bool tl_known(uint32_t constructor) { return find_constructor(constructor) != nullptr; }

bool tl_skip_object(TlReader &reader, int depth) {
  uint32_t id = 0;
  if (depth > MAX_DEPTH || !reader.u32(id))
    return false;
  if (id == TL_VECTOR) {
    uint32_t count = 0;
    if (!reader.u32(count))
      return false;
    for (uint32_t i = 0; i < count; i++) {
      if (!tl_skip_object(reader, depth + 1))
        return false;
    }
    return true;
  }
  const uint8_t *params = find_constructor(id);
  if (params == nullptr) {
    return false;
  }
  return skip_params(reader, params, depth);
}

bool tl_field(TlReader object, uint32_t name, TlReader &field) {
  uint32_t id = 0;
  if (!object.u32(id))
    return false;
  const uint8_t *params = find_constructor(id);
  if (params == nullptr)
    return false;
  const uint8_t count = params[0];
  uint32_t flags[MAX_FLAGS] = {};
  const uint8_t *param = params + 1;
  for (uint8_t i = 0; i < count; i++, param += PARAM_SIZE) {
    const uint8_t kind = param[4];
    const uint8_t flag_field = param[5];
    const uint8_t flag_bit = param[6];
    const bool present =
        flag_field == NO_FLAG || (flag_field < MAX_FLAGS && (flags[flag_field] & (1u << flag_bit)) != 0);
    if (load_u32(param) == name) {
      if (!present)
        return false;
      field = object;
      return true;
    }
    if (!present)
      continue;
    if (kind == KIND_FLAGS) {
      if (flag_bit >= MAX_FLAGS || !object.u32(flags[flag_bit]))
        return false;
      continue;
    }
    if (kind == KIND_VECTOR || kind == KIND_BARE_VECTOR) {
      uint32_t items = 0;
      if (kind == KIND_VECTOR) {
        uint32_t vid = 0;
        if (!object.u32(vid) || vid != TL_VECTOR)
          return false;
      }
      if (!object.u32(items))
        return false;
      for (uint32_t item = 0; item < items; item++) {
        if (!skip_value(object, param[7], load_u32(param + 8), 1))
          return false;
      }
      continue;
    }
    if (!skip_value(object, kind, load_u32(param + 8), 1))
      return false;
  }
  return false;
}

}
