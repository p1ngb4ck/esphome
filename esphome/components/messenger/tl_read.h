#pragma once

#include "esphome/core/defines.h"

#ifdef USE_MESSENGER_ACCOUNT

#include "esphome/components/mtproto/tl.h"

#include <cstdio>
#include <cstring>

namespace esphome::messenger::tl_read {

static constexpr uint32_t ID_VECTOR = 0x1cb5c415;
static constexpr uint32_t ID_USER = 0x31774388;

inline void copy(char *dest, size_t size, const char *src, size_t length) {
  const size_t n = length < size - 1 ? length : size - 1;
  memcpy(dest, src, n);
  dest[n] = '\0';
}

inline bool i32(mtproto::TlReader object, const char *name, int32_t &value) {
  mtproto::TlReader field;
  return mtproto::tl_field(object, mtproto::tl_hash(name), field) && field.i32(value);
}

inline bool i64(mtproto::TlReader object, const char *name, int64_t &value) {
  mtproto::TlReader field;
  return mtproto::tl_field(object, mtproto::tl_hash(name), field) && field.i64(value);
}

inline bool u32(mtproto::TlReader object, const char *name, uint32_t &value) {
  mtproto::TlReader field;
  return mtproto::tl_field(object, mtproto::tl_hash(name), field) && field.u32(value);
}

inline bool bytes(mtproto::TlReader object, const char *name, const uint8_t *&data, size_t &length) {
  mtproto::TlReader field;
  return mtproto::tl_field(object, mtproto::tl_hash(name), field) && field.bytes(data, length);
}

inline bool text(mtproto::TlReader object, const char *name, char *out, size_t size) {
  const uint8_t *data = nullptr;
  size_t length = 0;
  if (!bytes(object, name, data, length)) {
    out[0] = '\0';
    return false;
  }
  copy(out, size, reinterpret_cast<const char *>(data), length);
  return true;
}

inline bool vector_items(mtproto::TlReader vector, mtproto::TlReader &items, uint32_t &count) {
  uint32_t ctor = 0;
  if (!vector.u32(ctor) || ctor != ID_VECTOR || !vector.u32(count))
    return false;
  items = vector;
  return true;
}

inline bool field_vector(mtproto::TlReader object, const char *name, mtproto::TlReader &items, uint32_t &count) {
  mtproto::TlReader field;
  return mtproto::tl_field(object, mtproto::tl_hash(name), field) && vector_items(field, items, count);
}

inline bool object_span(mtproto::TlReader object, const uint8_t *&data, size_t &length) {
  mtproto::TlReader end = object;
  if (!mtproto::tl_skip_object(end))
    return false;
  data = object.pos();
  length = static_cast<size_t>(end.pos() - object.pos());
  return true;
}

inline bool user_name(mtproto::TlReader users, uint32_t count, int64_t id, char *out, size_t size) {
  for (uint32_t i = 0; i < count; i++) {
    mtproto::TlReader user = users;
    uint32_t ctor = 0;
    if (!users.peek_u32(ctor))
      return false;
    int64_t user_id = 0;
    if (ctor == ID_USER && i64(user, "id", user_id) && user_id == id) {
      char first[48];
      char last[48];
      text(user, "first_name", first, sizeof(first));
      text(user, "last_name", last, sizeof(last));
      snprintf(out, size, "%s%s%s", first, (first[0] != '\0' && last[0] != '\0') ? " " : "", last);
      return out[0] != '\0';
    }
    if (!mtproto::tl_skip_object(users))
      return false;
  }
  return false;
}

}

#endif
