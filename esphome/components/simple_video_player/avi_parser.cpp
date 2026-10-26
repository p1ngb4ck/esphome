/**
 * @file avi_parser.cpp
 * @brief Implementation of lightweight AVI container parser
 */

#include "avi_parser.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "avi_parser";

// AVI FOURCC codes
static constexpr uint32_t FOURCC_RIFF = 0x46464952;  // 'RIFF'
static constexpr uint32_t FOURCC_AVI = 0x20495641;   // 'AVI '
static constexpr uint32_t FOURCC_AVIX = 0x58495641;  // 'AVIX': a further RIFF (OpenDML)
static constexpr uint32_t FOURCC_LIST = 0x5453494C;  // 'LIST'
static constexpr uint32_t FOURCC_hdrl = 0x6C726468;  // 'hdrl'
static constexpr uint32_t FOURCC_avih = 0x68697661;  // 'avih'
static constexpr uint32_t FOURCC_strl = 0x6C727473;  // 'strl'
static constexpr uint32_t FOURCC_strh = 0x68727473;  // 'strh'
static constexpr uint32_t FOURCC_strf = 0x66727473;  // 'strf'
static constexpr uint32_t FOURCC_movi = 0x69766F6D;  // 'movi'
static constexpr uint32_t FOURCC_idx1 = 0x31786469;  // 'idx1'
static constexpr uint32_t FOURCC_indx = 0x78646E69;  // 'indx': OpenDML index of the ix## indexes
static constexpr uint32_t FOURCC_vids = 0x73646976;  // 'vids'
static constexpr uint32_t FOURCC_auds = 0x73647561;  // 'auds'
static constexpr uint32_t FOURCC_INFO = 0x4F464E49;  // 'INFO'
static constexpr uint32_t FOURCC_INAM = 0x4D414E49;  // 'INAM': the title

// Video stream chunk IDs
static constexpr uint32_t FOURCC_00dc = 0x63643030;  // '00dc' - compressed video
static constexpr uint32_t FOURCC_00db = 0x62643030;  // '00db' - uncompressed video

// Audio stream chunk IDs
static constexpr uint32_t FOURCC_01wb = 0x62773130;  // '01wb' - audio data

AVIParser::AVIParser() {}

void AVIParser::parse_info_list_(uint32_t size) {
  const uint64_t end = this->reader_->tell() + size;
  while (this->reader_->tell() + 8 <= end) {
    uint32_t id, len;
    if (!this->read_fourcc_(id) || !this->read_uint32_(len) || this->reader_->tell() + len > end)
      break;
    size_t used = 0;
    if (id == FOURCC_INAM && len > 0) {
      used = std::min<size_t>(len, TITLE_MAX - 1);
      if (this->reader_->read(reinterpret_cast<uint8_t *>(this->title_), used) != static_cast<int>(used)) {
        this->title_[0] = '\0';
        break;
      }
      this->title_[used] = '\0';
    }
    if (!this->skip_bytes_(len - used + (len & 1)))
      break;
  }
  // Whatever is left of the list (a broken chunk) skipped, the next chunk read where it starts.
  if (this->reader_->tell() < end)
    this->skip_bytes_(end - this->reader_->tell());
}

AVIParser::~AVIParser() { this->close(); }

bool AVIParser::open(BufferedFileReader *reader) {
  if (reader == nullptr || !reader->is_open()) {
    ESP_LOGE(TAG, "Invalid file reader");
    return false;
  }

  this->reader_ = reader;
  this->has_video_ = false;
  this->has_audio_ = false;
  this->movi_offset_ = 0;
  this->movi_size_ = 0;
  this->current_offset_ = 0;
  this->current_frame_ = 0;
  this->release_index_();
  this->index_tried_ = false;
  this->super_index_pos_ = 0;
  this->super_index_size_ = 0;
  this->recoveries_ = 0;
  this->damage_logged_ = 0;
  this->title_[0] = '\0';

  // Parse AVI headers
  if (!this->parse_headers_()) {
    ESP_LOGE(TAG, "Failed to parse AVI headers");
    this->close();
    return false;
  }

  this->first_movi_offset_ = this->movi_offset_;
  this->first_movi_size_ = this->movi_size_;

  // Seek to start of movi chunk
  if (!this->reader_->seek(this->movi_offset_)) {
    ESP_LOGE(TAG, "Failed to seek to movi chunk");
    this->close();
    return false;
  }

  this->current_offset_ = 0;
  this->current_frame_ = 0;

  ESP_LOGI(TAG, "AVI file opened successfully");
  if (this->has_video_) {
    ESP_LOGI(TAG, "  Video: %" PRIu32 "x%" PRIu32 ", %.2f fps, codec: 0x%08" PRIX32, this->video_info_.width,
             this->video_info_.height, (float) this->video_info_.fps_num / this->video_info_.fps_den,
             this->video_info_.codec);
  }
  if (this->has_audio_) {
    ESP_LOGI(TAG, "  Audio: %" PRIu32 " Hz, %u channels, codec: 0x%04" PRIX32, this->audio_info_.sample_rate,
             this->audio_info_.channels, this->audio_info_.codec);
  }

  return true;
}

void AVIParser::close() {
  this->release_index_();
  this->reader_ = nullptr;
  this->has_video_ = false;
  this->has_audio_ = false;
}

const AVIStreamInfo *AVIParser::get_video_info() const { return this->has_video_ ? &this->video_info_ : nullptr; }

const AVIStreamInfo *AVIParser::get_audio_info() const { return this->has_audio_ ? &this->audio_info_ : nullptr; }

bool AVIParser::parse_headers_() {
  // Read RIFF header
  uint32_t fourcc;
  if (!this->read_fourcc_(fourcc) || fourcc != FOURCC_RIFF) {
    ESP_LOGE(TAG, "Not a RIFF file");
    return false;
  }

  uint32_t file_size;
  if (!this->read_uint32_(file_size)) {
    return false;
  }

  // Read AVI signature
  if (!this->read_fourcc_(fourcc) || fourcc != FOURCC_AVI) {
    ESP_LOGE(TAG, "Not an AVI file");
    return false;
  }

  // Parse chunks until we find hdrl and movi
  bool found_hdrl = false;
  bool found_movi = false;

  while (!found_hdrl || !found_movi) {
    if (!this->read_fourcc_(fourcc)) {
      ESP_LOGE(TAG, "Unexpected end of file");
      return false;
    }

    uint32_t chunk_size;
    if (!this->read_uint32_(chunk_size)) {
      return false;
    }

    if (fourcc == FOURCC_LIST) {
      // LIST chunk - check type
      uint32_t list_type;
      if (!this->read_fourcc_(list_type)) {
        return false;
      }

      if (list_type == FOURCC_hdrl) {
        // Header list - parse stream headers
        found_hdrl = true;
        uint64_t hdrl_end = this->reader_->tell() + chunk_size - 4;

        // Parse hdrl contents
        while (this->reader_->tell() < hdrl_end) {
          uint32_t sub_fourcc;
          if (!this->read_fourcc_(sub_fourcc)) {
            break;
          }

          uint32_t sub_size;
          if (!this->read_uint32_(sub_size)) {
            break;
          }

          if (sub_fourcc == FOURCC_LIST) {
            // Nested LIST (strl - stream list)
            uint32_t strl_type;
            if (!this->read_fourcc_(strl_type)) {
              break;
            }

            if (strl_type == FOURCC_strl) {
              // Stream header list - track position to skip any remaining data
              uint64_t strl_start = this->reader_->tell();

              // Parse stream header (strh + strf)
              const bool had_video = this->has_video_;
              this->parse_stream_header_();
              // The video stream's OpenDML index follows its strf.
              const uint64_t strl_end = strl_start + sub_size - 4;
              while (!had_video && this->has_video_ && this->reader_->tell() + 8 <= strl_end) {
                uint32_t id, size;
                if (!this->read_fourcc_(id) || !this->read_uint32_(size))
                  break;
                if (id == FOURCC_indx) {
                  this->super_index_pos_ = this->reader_->tell();
                  this->super_index_size_ = size;
                }
                if (this->reader_->tell() + size > strl_end || !this->skip_bytes_(size + (size & 1)))
                  break;
              }

              // Always skip to end of strl LIST in case there are additional chunks (strn, indx, etc.)
              // sub_size includes the strl_type we already read, so subtract 4
              uint64_t bytes_consumed = this->reader_->tell() - strl_start;
              if (bytes_consumed < (sub_size - 4)) {
                uint64_t bytes_remaining = (sub_size - 4) - bytes_consumed;
                this->skip_bytes_(bytes_remaining);
              }
            } else if (strl_type == FOURCC_INFO) {
              this->parse_info_list_(sub_size - 4);
            } else {
              // Skip unknown list
              this->skip_bytes_(sub_size - 4);
            }
          } else {
            // Skip other chunks in hdrl
            this->skip_bytes_(sub_size);
          }
        }
      } else if (list_type == FOURCC_movi) {
        // Skip if hdrl not found yet
        if (!found_hdrl) {
          this->skip_bytes_(chunk_size - 4);
          continue;
        }
        // Movie data chunk
        found_movi = true;
        this->movi_offset_ = this->reader_->tell();
        // 0xFFFFFFFF is AVI for "size not known" (a live stream, ffmpeg writing to a pipe): read
        // until the data ends.
        this->movi_size_ = chunk_size == 0xFFFFFFFFu ? UINT64_MAX : chunk_size - 4;

        // We're now positioned at the start of movi data - ready for read_next_frame()
        // Break immediately - if we continue the loop, it will read the first frame chunk as a fourcc!
        break;
      } else if (list_type == FOURCC_INFO) {
        this->parse_info_list_(chunk_size - 4);
      } else {
        // Skip unknown LIST
        this->skip_bytes_(chunk_size - 4);
      }
    } else {
      // Skip non-LIST chunks
      this->skip_bytes_(chunk_size);
    }

    // Align to word boundary
    if (chunk_size & 1) {
      this->skip_bytes_(1);
    }
  }

  return found_hdrl && found_movi && (this->has_video_ || this->has_audio_);
}

bool AVIParser::parse_stream_header_() {
  // Read strh (stream header)
  uint32_t fourcc;
  if (!this->read_fourcc_(fourcc) || fourcc != FOURCC_strh) {
    return false;
  }

  uint32_t strh_size;
  if (!this->read_uint32_(strh_size)) {
    return false;
  }

  uint32_t stream_type;
  if (!this->read_fourcc_(stream_type)) {
    return false;
  }

  AVIStreamInfo *info = nullptr;
  if (stream_type == FOURCC_vids && !this->has_video_) {
    info = &this->video_info_;
    info->type = AVIStreamType::VIDEO;
    this->has_video_ = true;
  } else if (stream_type == FOURCC_auds && !this->has_audio_) {
    info = &this->audio_info_;
    info->type = AVIStreamType::AUDIO;
    this->has_audio_ = true;
  }

  if (info != nullptr) {
    // Read codec
    if (!this->read_uint32_(info->codec)) {
      return false;
    }

    // Skip flags, priority, language, and initial frames
    // dwFlags(4) + wPriority(2) + wLanguage(2) + dwInitialFrames(4) = 12 bytes
    this->skip_bytes_(12);

    // Read scale and rate (for FPS)
    if (!this->read_uint32_(info->fps_den)) {
      return false;
    }
    if (!this->read_uint32_(info->fps_num)) {
      return false;
    }

    // Skip start time
    this->skip_bytes_(4);

    // Read total frames
    if (!this->read_uint32_(info->total_frames)) {
      return false;
    }

    // Skip rest of strh
    uint32_t remaining = strh_size - 36;
    this->skip_bytes_(remaining);
  } else {
    // Skip entire strh
    this->skip_bytes_(strh_size);
  }

  // Align to word boundary
  if (strh_size & 1) {
    this->skip_bytes_(1);
  }

  // Read strf (stream format)
  if (!this->read_fourcc_(fourcc) || fourcc != FOURCC_strf) {
    return false;
  }

  uint32_t strf_size;
  if (!this->read_uint32_(strf_size)) {
    return false;
  }

  if (info != nullptr) {
    if (info->type == AVIStreamType::VIDEO) {
      // Parse BITMAPINFOHEADER
      uint32_t bi_size;
      if (!this->read_uint32_(bi_size)) {
        return false;
      }

      if (!this->read_uint32_(info->width)) {
        return false;
      }

      if (!this->read_uint32_(info->height)) {
        return false;
      }

      // Skip rest of bitmap header
      size_t bytes_read = 12;  // biSize + width + height
      if (strf_size > bytes_read) {
        this->skip_bytes_(strf_size - bytes_read);
      }
    } else if (info->type == AVIStreamType::AUDIO) {
      // Parse WAVEFORMATEX
      // Minimum WAVEFORMATEX: wFormatTag(2) + nChannels(2) + nSamplesPerSec(4) +
      //                       nAvgBytesPerSec(4) + nBlockAlign(2) + wBitsPerSample(2) = 16 bytes
      if (strf_size < 16) {
        ESP_LOGE(TAG, "AUDIO strf chunk too small: %" PRIu32 " bytes (need at least 16 for WAVEFORMATEX)",
                 strf_size);
        return false;
      }

      // Read format tag (2 bytes)
      uint16_t format_tag;
      if (!this->read_uint16_(format_tag)) {
        ESP_LOGE(TAG, "Failed to read audio format tag");
        return false;
      }
      info->codec = format_tag;

      // Read channels (2 bytes)
      if (!this->read_uint16_(info->channels)) {
        ESP_LOGE(TAG, "Failed to read audio channels");
        return false;
      }

      // Read sample rate (4 bytes)
      if (!this->read_uint32_(info->sample_rate)) {
        ESP_LOGE(TAG, "Failed to read audio sample rate");
        return false;
      }

      // Skip bytes per second (4 bytes) and block align (2 bytes)
      if (!this->skip_bytes_(6)) {
        ESP_LOGE(TAG, "Failed to skip avg bytes/sec and block align");
        return false;
      }

      // Read bits per sample (2 bytes)
      if (!this->read_uint16_(info->bits_per_sample)) {
        ESP_LOGE(TAG, "Failed to read audio bits per sample");
        return false;
      }

      info->stream_header_len = 0;
      size_t used = 16;
      // FLAC: cbSize then the 34-byte STREAMINFO (ffmpeg: flacenc.c extradata, riffenc.c
      // ff_put_wav_header writes it after cbSize).
      static constexpr size_t STREAMINFO_SIZE = 34;
      if (info->codec == static_cast<uint32_t>(AVIAudioCodec::FLAC) && strf_size >= 18 + STREAMINFO_SIZE) {
        uint16_t cb_size;
        if (!this->read_uint16_(cb_size))
          return false;
        used += 2;
        if (cb_size >= STREAMINFO_SIZE) {
          uint8_t *header = info->stream_header;
          header[0] = 'f';
          header[1] = 'L';
          header[2] = 'a';
          header[3] = 'C';
          header[4] = 0x80;  // last metadata block, type 0 (STREAMINFO)
          header[5] = 0;
          header[6] = 0;
          header[7] = STREAMINFO_SIZE;
          if (this->reader_->read(header + 8, STREAMINFO_SIZE) != static_cast<int>(STREAMINFO_SIZE))
            return false;
          used += STREAMINFO_SIZE;
          info->stream_header_len = 8 + STREAMINFO_SIZE;
        }
      }
      // Skip rest of WAVEFORMATEX if there's extra data (cbSize + extra format data)
      if (strf_size > used) {
        if (!this->skip_bytes_(strf_size - used)) {
          ESP_LOGE(TAG, "Failed to skip extra WAVEFORMATEX data");
          return false;
        }
      }
    }
  } else {
    // Skip strf
    this->skip_bytes_(strf_size);
  }

  // Align to word boundary
  if (strf_size & 1) {
    this->skip_bytes_(1);
  }

  return true;
}

static inline uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static constexpr uint32_t FOURCC_JUNK = 0x4B4E554A;  // 'JUNK'
static constexpr uint32_t MAX_RECOVERIES = 32;          // in a row: then the file is given up
static constexpr size_t SCAN_LIMIT = 4 * 1024 * 1024;  // looked through for a chunk header
static constexpr uint32_t DAMAGE_LOG_MAX = 3;

static bool is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

enum class ChunkKind : uint8_t { VIDEO, AUDIO, LIST, SKIP, INVALID };

// What a chunk header in the movi list is; INVALID for one that cannot be (damage). Its size must
// fit in what is left of the list (when known).
static ChunkKind chunk_kind(const uint8_t *hdr, uint64_t remaining) {
  const uint32_t id = le32(hdr);
  const uint32_t size = le32(hdr + 4);
  if (size > remaining)
    return ChunkKind::INVALID;
  if (id == FOURCC_LIST)
    return size >= 4 ? ChunkKind::LIST : ChunkKind::INVALID;
  if (id == FOURCC_00dc || id == FOURCC_00db)
    return ChunkKind::VIDEO;
  if (id == FOURCC_01wb)
    return ChunkKind::AUDIO;
  if (is_digit(hdr[0]) && is_digit(hdr[1])) {
    const char a = static_cast<char>(hdr[2]), b = static_cast<char>(hdr[3]);
    if ((a == 'd' && (b == 'c' || b == 'b')) || (a == 'w' && b == 'b') || (a == 'p' && b == 'c') ||
        (a == 't' && b == 'x'))
      return ChunkKind::SKIP;  // another stream's
    return ChunkKind::INVALID;
  }
  if (id == FOURCC_JUNK || id == FOURCC_idx1 || (hdr[0] == 'i' && hdr[1] == 'x' && is_digit(hdr[2]) && is_digit(hdr[3])))
    return ChunkKind::SKIP;
  return ChunkKind::INVALID;
}

bool AVIParser::plausible_chunk(const uint8_t *hdr, uint64_t at) const {
  const uint64_t end = this->data_end();
  return chunk_kind(hdr, end == UINT64_MAX ? UINT64_MAX : (end > at ? end - at : 0)) != ChunkKind::INVALID;
}

bool AVIParser::plausible_header_(const uint8_t *hdr, size_t max_frame) const {
  const uint64_t end = this->data_end();
  const uint64_t at = this->reader_->tell();
  const ChunkKind kind = chunk_kind(hdr, end == UINT64_MAX ? UINT64_MAX : (end > at ? end - at : 0));
  return (kind == ChunkKind::VIDEO && le32(hdr + 4) <= max_frame) || kind == ChunkKind::AUDIO;
}

bool AVIParser::resync_(uint8_t *hdr, size_t max_frame, bool &have_header) {
  have_header = false;
  if (this->reader_->failed() && !this->reader_->seek(this->reader_->tell()))
    return false;
  const uint64_t at = this->reader_->tell();
  if (this->index_ != nullptr) {
    // The first indexed video frame past where reading stopped (offsets grow through the index).
    uint32_t lo = 0, hi = this->index_count_;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (this->entry_pos_(mid) < at) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (this->damage_logged_ < DAMAGE_LOG_MAX) {
      this->damage_logged_++;
      ESP_LOGW(TAG, "Damaged data at %llu: going on at frame %" PRIu32, static_cast<unsigned long long>(at),
               lo * this->index_stride_);
    }
    return lo < this->index_count_ && this->seek_to_frame(lo * this->index_stride_);
  }
  // Without an index: the next chunk header that can be one, byte by byte.
  if (this->damage_logged_ < DAMAGE_LOG_MAX) {
    this->damage_logged_++;
    ESP_LOGW(TAG, "Damaged data at %llu: looking for the next frame", static_cast<unsigned long long>(at));
  }
  if (this->reader_->read(hdr, 8) != 8)
    return false;
  for (size_t scanned = 0; scanned < SCAN_LIMIT; scanned++) {
    if (this->plausible_header_(hdr, max_frame)) {
      this->current_offset_ = this->reader_->tell() - this->movi_offset_;
      have_header = true;
      return true;
    }
    memmove(hdr, hdr + 1, 7);
    if (this->reader_->read(hdr + 7, 1) != 1)
      return false;
  }
  return false;
}

int AVIParser::read_next_frame(AVIFrame &frame, uint8_t *buffer, size_t buffer_size) {
  if (!this->is_open()) {
    return -1;
  }
  uint8_t hdr[8];
  bool have_header = false;  // found by resync_()
  auto damaged = [&]() -> bool {
    return ++this->recoveries_ <= MAX_RECOVERIES && this->resync_(hdr, buffer_size, have_header);
  };

  while (this->current_offset_ < this->movi_size_ || (!have_header && this->next_movi_())) {
    if (!have_header) {
      // Chunk header is fourcc(4) + size(4). One 8-byte read instead of two 4-byte ring ops.
      const int hn = this->reader_->read(hdr, sizeof(hdr));
      if (hn == 0)
        return 0;  // no more data at a chunk boundary -> end of stream
      if (hn < 0) {
        if (!damaged())
          return -1;
        continue;
      }
      if (hn != static_cast<int>(sizeof(hdr)))
        return 0;  // the file ends inside a header
      this->current_offset_ += 8;
    }
    have_header = false;
    const uint32_t chunk_size = le32(hdr + 4);
    const uint64_t remaining =
        this->movi_size_ == UINT64_MAX ? UINT64_MAX : this->movi_size_ - std::min(this->movi_size_, this->current_offset_);

    switch (chunk_kind(hdr, remaining)) {
      case ChunkKind::INVALID:
        if (!damaged())
          return -1;
        continue;
      case ChunkKind::LIST: {
        // LIST 'rec ' (some AVI files wrap frames in these): its chunks follow.
        uint32_t list_type;
        if (!this->read_fourcc_(list_type))
          return 0;
        this->current_offset_ += 4;
        continue;
      }
      case ChunkKind::SKIP:
        if (!this->skip_bytes_(chunk_size + (chunk_size & 1)))
          return 0;
        this->current_offset_ += chunk_size + (chunk_size & 1);
        continue;
      case ChunkKind::AUDIO:
        // An empty audio chunk carries nothing: ffmpeg writes one for every gap in a VBR track's
        // timestamps (write_skip_frames, libavformat/avienc.c -- FLAC and MP3, not PCM).
        if (chunk_size == 0)
          continue;
        if (chunk_size > buffer_size) {
          if (!this->skip_bytes_(chunk_size + (chunk_size & 1)))
            return 0;
          this->current_offset_ += chunk_size + (chunk_size & 1);
          continue;
        }
        frame.stream_type = AVIStreamType::AUDIO;
        break;
      case ChunkKind::VIDEO:
        if (chunk_size == 0) {
          this->current_frame_++;
          frame.stream_type = AVIStreamType::VIDEO;
          frame.size = 0;
          return REPEAT_FRAME;
        }
        if (chunk_size > buffer_size) {
          // Larger than any frame that plays: passed over, the previous picture stays.
          if (this->damage_logged_ < DAMAGE_LOG_MAX) {
            this->damage_logged_++;
            ESP_LOGW(TAG, "Frame %" PRIu32 " of %" PRIu32 " bytes is larger than the frame slot (%zu): passed over",
                     this->current_frame_, chunk_size, buffer_size);
          }
          if (!this->skip_bytes_(chunk_size + (chunk_size & 1)))
            return 0;
          this->current_offset_ += chunk_size + (chunk_size & 1);
          this->current_frame_++;
          frame.stream_type = AVIStreamType::VIDEO;
          frame.size = 0;
          return REPEAT_FRAME;
        }
        frame.stream_type = AVIStreamType::VIDEO;
        break;
    }

    frame.size = chunk_size;
    const int bytes_read = this->reader_->read(buffer, chunk_size);
    if (bytes_read < 0) {
      if (!damaged())
        return -1;
      continue;
    }
    if (bytes_read != static_cast<int>(chunk_size)) {
      // Ran out of data mid-frame (truncated tail / stream ended) -> end of stream.
      ESP_LOGW(TAG, "Frame payload short (%d/%" PRIu32 ") -- end of stream", bytes_read, chunk_size);
      return 0;
    }
    this->current_offset_ += chunk_size;
    if (chunk_size & 1) {
      this->skip_bytes_(1);
      this->current_offset_ += 1;
    }
    if (frame.stream_type == AVIStreamType::VIDEO) {
      this->current_frame_++;
      this->recoveries_ = 0;
    }
    return bytes_read;
  }

  // End of file
  return 0;
}

bool AVIParser::next_movi_() {
  if (this->movi_size_ == UINT64_MAX)
    return false;
  // ffmpeg: the movi list ends with its ix##, the first RIFF's then idx1; the next RIFF 'AVIX' starts
  // with its movi list. A few chunks (JUNK, idx1) are passed over.
  for (int i = 0; i < 8; i++) {
    uint32_t id, size;
    if (!this->read_fourcc_(id) || !this->read_uint32_(size))
      return false;
    if (id == FOURCC_RIFF || id == FOURCC_LIST) {
      uint32_t type;
      if (size < 4 || !this->read_fourcc_(type))
        return false;
      if (id == FOURCC_RIFF) {
        if (type != FOURCC_AVIX)
          return false;
        continue;
      }
      if (type == FOURCC_movi) {
        this->movi_offset_ = this->reader_->tell();
        this->movi_size_ = size - 4;
        this->current_offset_ = 0;
        return this->movi_size_ > 0;
      }
      size -= 4;
    }
    if (!this->skip_bytes_(size + (size & 1)))
      return false;
  }
  return false;
}

void AVIParser::release_index_() {
  this->segments_ = nullptr;
  this->segment_count_ = 0;
  this->index_ = nullptr;
  this->index_count_ = 0;
  this->index_stride_ = 1;
}

bool AVIParser::load_index(uint8_t *scratch, size_t scratch_size) {
  if (this->index_ != nullptr)
    return true;
  if (this->index_tried_ || !this->is_open() || this->first_movi_size_ == UINT64_MAX || scratch_size < 16)
    return false;
  this->index_tried_ = true;
  const uint64_t resume = this->reader_->tell();
  if (this->read_index_(scratch, scratch_size)) {
    ESP_LOGI(TAG, "Index: %" PRIu32 " entries, every %" PRIu32 ". video frame, %" PRIu32 " RIFF(s)",
             this->index_count_, this->index_stride_, this->segment_count_);
    return this->reader_->seek(resume);
  }
  this->release_index_();
  if (!this->reader_->seek(resume)) {
    ESP_LOGE(TAG, "Could not return to the read position");
    return false;
  }
  ESP_LOGW(TAG, "No usable index: seeking is not possible in this file");
  return false;
}

void AVIParser::add_index_entry_(uint32_t video_frame, uint32_t offset, uint32_t capacity) {
  if (video_frame % this->index_stride_ != 0)
    return;
  if (this->index_count_ == capacity) {
    for (uint32_t k = 0; k < capacity / 2; k++)
      this->index_[k] = this->index_[2 * k];
    this->index_count_ = capacity / 2;
    this->index_stride_ *= 2;
    if (video_frame % this->index_stride_ != 0)
      return;
  }
  this->index_[this->index_count_++] = offset;
}

uint32_t AVIParser::segment_of_(uint32_t k) const {
  const uint32_t frame = k * this->index_stride_;
  uint32_t lo = 0, hi = this->segment_count_;
  while (hi - lo > 1) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (this->segments_[mid].first_frame <= frame) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

uint64_t AVIParser::entry_pos_(uint32_t k) const {
  return this->segments_[this->segment_of_(k)].base + this->index_[k];
}

bool AVIParser::read_index_(uint8_t *scratch, size_t scratch_size) {
  if (this->index_memory_ == nullptr)
    return false;
  if (this->super_index_pos_ != 0) {
    if (this->read_odml_index_(scratch, scratch_size))
      return true;
    ESP_LOGW(TAG, "OpenDML index not usable: idx1 only (the first RIFF)");
    this->release_index_();
  }
  // idx1 follows the first movi list, possibly after other chunks (JUNK).
  uint64_t pos = this->first_movi_offset_ + this->first_movi_size_;
  pos += pos & 1;
  uint32_t idx_size = 0;
  for (int i = 0; i < 4 && idx_size == 0; i++) {
    uint32_t id;
    uint32_t size;
    if (!this->reader_->seek(pos) || !this->read_fourcc_(id) || !this->read_uint32_(size))
      return false;
    if (id == FOURCC_idx1) {
      idx_size = size;
    } else {
      pos += 8 + static_cast<uint64_t>(size) + (size & 1);
    }
  }
  const uint32_t entries = idx_size / 16;
  if (entries == 0 || this->index_max_bytes_ < sizeof(Segment) + 2 * sizeof(uint32_t))
    return false;
  // One segment, the first movi list; the entries after it. When the memory has not the room for
  // every frame, every second (fourth, ...) is kept.
  this->segments_ = reinterpret_cast<Segment *>(this->index_memory_);
  this->segment_count_ = 1;
  Segment &seg = this->segments_[0];
  seg.base = this->first_movi_offset_ - 4;
  seg.movi_size = static_cast<uint32_t>(this->first_movi_size_);
  seg.first_frame = 0;
  this->index_ = this->index_memory_ + sizeof(Segment) / sizeof(uint32_t);
  this->index_count_ = 0;
  this->index_stride_ = 1;
  const auto capacity = static_cast<uint32_t>(
                            std::min<size_t>(entries, (this->index_max_bytes_ - sizeof(Segment)) / sizeof(uint32_t))) &
                        ~1u;
  if (capacity < 2)
    return false;
  const size_t per_read = scratch_size / 16;
  uint32_t left = entries;
  uint32_t video = 0;
  while (left > 0) {
    const size_t n = std::min<size_t>(left, per_read);
    if (this->reader_->read(scratch, n * 16) != static_cast<int>(n * 16))
      return false;
    for (size_t i = 0; i < n; i++) {
      const uint8_t *entry = scratch + i * 16;
      const uint32_t id = le32(entry);
      if (id == FOURCC_00dc || id == FOURCC_00db)
        this->add_index_entry_(video++, le32(entry + 8), capacity);
    }
    left -= n;
  }
  if (this->index_count_ == 0)
    return false;
  // Offsets count from the 'movi' fourcc (AVI spec, ffmpeg); some writers store file offsets.
  for (const uint64_t base : {seg.base, static_cast<uint64_t>(0)}) {
    uint32_t id;
    if (base + this->index_[0] >= this->first_movi_offset_ && this->reader_->seek(base + this->index_[0]) &&
        this->read_fourcc_(id) && (id == FOURCC_00dc || id == FOURCC_00db)) {
      if (base == 0) {
        for (uint32_t k = 0; k < this->index_count_; k++) {
          if (this->index_[k] < seg.base)
            return false;
          this->index_[k] -= static_cast<uint32_t>(seg.base);
        }
      }
      return true;
    }
  }
  return false;
}

// OpenDML (ffmpeg avienc.c avi_write_ix()): the video stream's indx lists one ix## per RIFF
// (qwOffset, dwSize, dwDuration). An ix## has qwBaseOffset (its movi list's 'movi' fourcc) and per
// chunk dwOffset (to the chunk's data, from the base) and dwSize (bit 31: not a key frame).
bool AVIParser::read_odml_index_(uint8_t *scratch, size_t scratch_size) {
  uint8_t hdr[32];
  if (!this->reader_->seek(this->super_index_pos_) || this->reader_->read(hdr, 24) != 24)
    return false;
  const uint32_t ixes = le32(hdr + 4);
  // wLongsPerEntry 4, bIndexType 0 (AVI_INDEX_OF_INDEXES)
  if ((hdr[0] | hdr[1] << 8) != 4 || hdr[3] != 0 || ixes == 0 || ixes > scratch_size / 16 ||
      24 + static_cast<uint64_t>(ixes) * 16 > this->super_index_size_ ||
      this->index_max_bytes_ < ixes * sizeof(Segment) + 2 * sizeof(uint32_t))
    return false;
  if (this->reader_->read(scratch, ixes * 16) != static_cast<int>(ixes * 16))
    return false;
  // The segment table first, the entries after it. base holds the ix##'s position until it is read.
  this->segments_ = reinterpret_cast<Segment *>(this->index_memory_);
  this->segment_count_ = ixes;
  for (uint32_t i = 0; i < ixes; i++)
    this->segments_[i].base = le32(scratch + i * 16) | static_cast<uint64_t>(le32(scratch + i * 16 + 4)) << 32;
  this->index_ = this->index_memory_ + ixes * sizeof(Segment) / sizeof(uint32_t);
  this->index_count_ = 0;
  this->index_stride_ = 1;
  const auto capacity =
      static_cast<uint32_t>((this->index_max_bytes_ - ixes * sizeof(Segment)) / sizeof(uint32_t)) & ~1u;
  const size_t per_read = scratch_size / 8;
  uint32_t video = 0;
  for (uint32_t i = 0; i < ixes; i++) {
    Segment &seg = this->segments_[i];
    const uint64_t ix = seg.base;
    if (!this->reader_->seek(ix) || this->reader_->read(hdr, 32) != 32)
      return false;
    const uint32_t count = le32(hdr + 12);
    const uint32_t chunk = le32(hdr + 16);
    // ix##, wLongsPerEntry 2, bIndexType 1 (AVI_INDEX_OF_CHUNKS), the video stream's chunks
    if (hdr[0] != 'i' || hdr[1] != 'x' || (hdr[8] | hdr[9] << 8) != 2 || hdr[11] != 1 ||
        (chunk != FOURCC_00dc && chunk != FOURCC_00db) || 24 + static_cast<uint64_t>(count) * 8 > le32(hdr + 4))
      return false;
    seg.base = le32(hdr + 20) | static_cast<uint64_t>(le32(hdr + 24)) << 32;
    seg.first_frame = video;
    // The movi list's size, in its LIST header right before the base.
    uint8_t list[8];
    if (seg.base < 4 || !this->reader_->seek(seg.base - 4) || this->reader_->read(list, 8) != 8 ||
        le32(list + 4) != FOURCC_movi || le32(list) < 4)
      return false;
    seg.movi_size = le32(list) - 4;
    if (!this->reader_->seek(ix + 32))
      return false;
    uint32_t left = count;
    while (left > 0) {
      const size_t n = std::min<size_t>(left, per_read);
      if (this->reader_->read(scratch, n * 8) != static_cast<int>(n * 8))
        return false;
      for (size_t k = 0; k < n; k++) {
        const uint32_t offset = le32(scratch + k * 8);
        if (offset < 8)
          return false;
        this->add_index_entry_(video++, offset - 8, capacity);
      }
      left -= n;
    }
  }
  uint32_t id;
  return this->index_count_ > 0 && this->reader_->seek(this->entry_pos_(0)) && this->read_fourcc_(id) &&
         (id == FOURCC_00dc || id == FOURCC_00db);
}

bool AVIParser::seek_to_frame(uint32_t frame_number) {
  if (this->index_ == nullptr || !this->is_open())
    return false;
  const uint32_t entry = std::min(frame_number / this->index_stride_, this->index_count_ - 1);
  const Segment &seg = this->segments_[this->segment_of_(entry)];
  const uint64_t pos = seg.base + this->index_[entry];
  if (pos < seg.base + 4 || !this->reader_->seek(pos))
    return false;
  this->movi_offset_ = seg.base + 4;
  this->movi_size_ = seg.movi_size;
  this->current_offset_ = pos - this->movi_offset_;
  this->current_frame_ = entry * this->index_stride_;
  return true;
}

bool AVIParser::rewind() {
  if (!this->is_open()) {
    return false;
  }

  if (!this->reader_->seek(this->first_movi_offset_)) {
    return false;
  }

  this->movi_offset_ = this->first_movi_offset_;
  this->movi_size_ = this->first_movi_size_;
  this->current_offset_ = 0;
  this->current_frame_ = 0;
  return true;
}

bool AVIParser::read_fourcc_(uint32_t &fourcc) {
  uint8_t bytes[4];
  int read = this->reader_->read(bytes, 4);
  if (read != 4) {
    return false;
  }

  // FOURCC is little-endian
  fourcc = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  return true;
}

bool AVIParser::read_uint32_(uint32_t &value) {
  uint8_t bytes[4];
  int read = this->reader_->read(bytes, 4);
  if (read != 4) {
    return false;
  }

  // Little-endian
  value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
  return true;
}

bool AVIParser::read_uint16_(uint16_t &value) {
  uint8_t bytes[2];
  int read = this->reader_->read(bytes, 2);
  if (read != 2) {
    return false;
  }

  // Little-endian
  value = bytes[0] | (bytes[1] << 8);
  return true;
}

bool AVIParser::skip_bytes_(size_t count) {
  // Discard via the streaming read() path, not seek(): a forward skip within a sequential
  // stream is exactly what read_chunk() is for -- it serves straight out of
  // BufferedFileReader's read-ahead buffers whenever the bytes are already there (which they
  // almost always are here), with no extra storage-worker round trip at all. seek() is a
  // separate, real worker operation for actual random-access jumps -- and, on the current
  // double-buffered BufferedFileReader, ALSO eagerly forces a full synchronous re-fill of the
  // read-ahead pair afterward (to keep the prefetch invariant primed for genuine seeks), so
  // routing this function's very frequent small forward skips through seek() would now be worse,
  // not just wasteful: it'd trigger a full blocking refetch on nearly every AVI chunk boundary.
  // This function is the hot path for AVI parsing (called once per interleaved audio/video
  // chunk, and again for every odd-sized chunk's 1-byte alignment pad).
  uint8_t scratch[2048];
  size_t remaining = count;
  while (remaining > 0) {
    size_t to_read = std::min(remaining, sizeof(scratch));
    int n = this->reader_->read(scratch, to_read);
    if (n <= 0) {
      return false;  // EOF or read error before the skip finished
    }
    remaining -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace simple_video_player
}  // namespace esphome
