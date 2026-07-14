#include "store_yaml.h"

#if defined(USE_STORE_YAML) && defined(USE_STORE_YAML_EXPORT)

#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#endif

// Minimal prototypes for the two symbols we use from the vendored
// zstddeclib.c amalgamation (its full header is internal to that file).
extern "C" {
size_t ZSTD_decompress(void *dst, size_t dst_capacity, const void *src, size_t src_size);
unsigned ZSTD_isError(size_t code);
}

namespace esphome::store_yaml {

static const char *const TAG = "store_yaml";

static constexpr uint8_t ENVELOPE_MAGIC[4] = {'E', 'H', 'Y', '1'};
static constexpr size_t MAX_EXPORT_PATH = 512;

// Little-endian readers — the envelope is packed with struct.pack("<...").
static uint16_t read_u16_le(const uint8_t *p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
static uint32_t read_u32_le(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool StoreYamlComponent::ensure_parent_dirs_(const char *file_path) {
  // mkdir -p for everything above the final path component. Segment-wise
  // mkdir with EEXIST tolerated; other errors abort.
  char buf[MAX_EXPORT_PATH];
  size_t len = strlen(file_path);
  if (len >= sizeof(buf))
    return false;
  memcpy(buf, file_path, len + 1);
  for (size_t i = 1; i < len; i++) {
    if (buf[i] != '/')
      continue;
    buf[i] = '\0';
    if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
      ESP_LOGE(TAG, "mkdir '%s' failed: %d", buf, errno);
      return false;
    }
    buf[i] = '/';
  }
  return true;
}

bool StoreYamlComponent::write_file_(const char *path, const uint8_t *data, size_t len) {
  if (!this->ensure_parent_dirs_(path))
    return false;
  FILE *f = fopen(path, "wb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "fopen '%s' failed: %d", path, errno);
    return false;
  }
  bool ok = len == 0 || fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0)
    ok = false;
  if (!ok)
    ESP_LOGE(TAG, "Writing '%s' failed", path);
  return ok;
}

bool StoreYamlComponent::export_to_storage(const char *path, bool raw) {
  if (this->data_ == nullptr || this->size_ == 0) {
    ESP_LOGE(TAG, "No embedded YAML available");
    return false;
  }
  if (path == nullptr || path[0] != '/' || strlen(path) >= MAX_EXPORT_PATH) {
    ESP_LOGE(TAG, "Export path must be absolute and shorter than %u chars", (unsigned) MAX_EXPORT_PATH);
    return false;
  }

  // PROGMEM is a no-op on every platform this branch's storage stack supports
  // (ESP32 / host) — the blob is directly addressable.
  if (raw) {
    // The compressed EHY1 envelope as-is; decompress off-device
    // (zstd CLI / Python `compression.zstd`).
    if (!this->write_file_(path, this->data_, this->size_))
      return false;
    ESP_LOGI(TAG, "Exported compressed YAML envelope (%zu bytes) to '%s'", this->size_, path);
    return true;
  }

  // Decompressed export: one-shot decode into a single buffer, PSRAM
  // preferred. Envelope size == uncompressed size is known from codegen, so
  // no streaming machinery is needed; configs are small by nature.
  const size_t out_size = this->uncompressed_size_;
  uint8_t *out = nullptr;
#ifdef USE_ESP32
  out = static_cast<uint8_t *>(heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  if (out == nullptr)
    out = static_cast<uint8_t *>(malloc(out_size));  // NOLINT
  if (out == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate %zu bytes for decompression (no PSRAM?) — use raw: true instead", out_size);
    return false;
  }

  bool ok = false;
  size_t written_files = 0;
  const size_t res = ZSTD_decompress(out, out_size, this->data_, this->size_);
  if (ZSTD_isError(res) || res != out_size) {
    ESP_LOGE(TAG, "zstd decompression failed (result %zu, expected %zu)", res, out_size);
  } else if (out_size < 8 || memcmp(out, ENVELOPE_MAGIC, 4) != 0) {
    ESP_LOGE(TAG, "Invalid envelope (bad magic)");
  } else {
    // Walk the envelope: magic(4) | u32 count | { u16 path_len | path | u32 content_len | content }
    const uint32_t count = read_u32_le(out + 4);
    size_t off = 8;
    ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
      if (off + 2 > out_size) {
        ok = false;
        break;
      }
      const uint16_t path_len = read_u16_le(out + off);
      off += 2;
      if (off + path_len + 4 > out_size) {
        ok = false;
        break;
      }
      char rel[MAX_EXPORT_PATH];
      if (path_len == 0 || path_len >= sizeof(rel)) {
        ok = false;
        break;
      }
      memcpy(rel, out + off, path_len);
      rel[path_len] = '\0';
      off += path_len;
      const uint32_t content_len = read_u32_le(out + off);
      off += 4;
      if (off + content_len > out_size) {
        ok = false;
        break;
      }
      // Path sanitizing: envelope paths are relative by construction; reject
      // anything that could escape the target directory.
      if (rel[0] == '/' || strstr(rel, "..") != nullptr) {
        ESP_LOGE(TAG, "Refusing unsafe envelope path '%s'", rel);
        ok = false;
        break;
      }
      char full[MAX_EXPORT_PATH];
      const int n = snprintf(full, sizeof(full), "%s/%s", path, rel);
      if (n < 0 || static_cast<size_t>(n) >= sizeof(full)) {
        ESP_LOGE(TAG, "Target path too long for '%s'", rel);
        ok = false;
        break;
      }
      if (!this->write_file_(full, out + off, content_len)) {
        ok = false;
        break;
      }
      off += content_len;
      written_files++;
    }
    if (ok && off != out_size) {
      ESP_LOGW(TAG, "Envelope has %zu trailing bytes (ignored)", out_size - off);
    }
  }

  free(out);  // NOLINT — matches both allocation paths (heap_caps_free aliases free on ESP-IDF)
  if (ok) {
    ESP_LOGI(TAG, "Exported %zu YAML file(s) below '%s'", written_files, path);
  } else {
    ESP_LOGE(TAG, "YAML export to '%s' failed after %zu file(s)", path, written_files);
  }
  return ok;
}

}  // namespace esphome::store_yaml

#endif  // USE_STORE_YAML && USE_STORE_YAML_EXPORT
