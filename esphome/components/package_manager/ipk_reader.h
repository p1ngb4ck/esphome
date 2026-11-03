#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include <cstddef>
#include <cstdint>
#include <miniz.h>

namespace esphome::package_manager {

class IpkReader {
 public:
  enum class Event : uint8_t { NEED_INPUT, FILE_DATA, FILE_END, DONE, ERROR };
  enum class Member : uint8_t { NONE, CONTROL, DATA };

  bool begin();
  void end();
  void feed(const uint8_t *data, size_t len, bool last);
  Event next();

  Member member() const { return this->member_; }
  const char *path() const { return this->path_; }
  const uint8_t *chunk() const { return this->chunk_; }
  size_t chunk_len() const { return this->chunk_len_; }
  uint64_t file_size() const { return this->file_size_; }
  const char *error() const { return this->error_; }

 protected:
  enum class State : uint8_t {
    AR_MAGIC,
    AR_HEADER,
    AR_SKIP,
    GZ_HEADER,
    GZ_EXTRA,
    GZ_NAME,
    GZ_COMMENT,
    GZ_CRC,
    INFLATE,
    TRAILER,
    DONE,
    ERROR,
  };
  enum class Tar : uint8_t { HEADER, BODY, PAD, END };

  Event fail_(const char *error);
  void use_(size_t n, bool member);
  bool take_(uint8_t *dst, size_t want, size_t &have, bool member);
  bool skip_(uint64_t &left, bool member);
  void start_gzip_();
  Event inflate_();
  Event tar_();

  State state_{State::AR_MAGIC};
  Tar tar_state_{Tar::HEADER};
  Member member_{Member::NONE};
  const uint8_t *in_{nullptr};
  size_t in_len_{0};
  bool last_{false};
  uint64_t member_left_{0};
  bool member_pad_{false};
  uint8_t head_[60];
  size_t head_have_{0};
  uint8_t gz_flags_{0};
  uint16_t gz_extra_left_{0};
  tinfl_decompressor *inflator_{nullptr};
  uint8_t *window_{nullptr};
  size_t window_ofs_{0};
  const uint8_t *out_{nullptr};
  size_t out_len_{0};
  bool inflate_done_{false};
  uint8_t *tar_head_{nullptr};
  size_t tar_have_{0};
  uint64_t tar_left_{0};
  uint64_t tar_pad_{0};
  bool tar_file_{false};
  char path_[101]{};
  uint64_t file_size_{0};
  const uint8_t *chunk_{nullptr};
  size_t chunk_len_{0};
  bool file_end_pending_{false};
  const char *error_{nullptr};
};

}

#endif
