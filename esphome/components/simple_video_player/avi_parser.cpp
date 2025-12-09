/**
 * @file avi_parser.cpp
 * @brief Implementation of lightweight AVI container parser
 */

#include "avi_parser.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace simple_video_player {

static const char *const TAG = "avi_parser";

// AVI FOURCC codes
static constexpr uint32_t FOURCC_RIFF = 0x46464952;  // 'RIFF'
static constexpr uint32_t FOURCC_AVI = 0x20495641;   // 'AVI '
static constexpr uint32_t FOURCC_LIST = 0x5453494C;  // 'LIST'
static constexpr uint32_t FOURCC_hdrl = 0x6C726468;  // 'hdrl'
static constexpr uint32_t FOURCC_avih = 0x68697661;  // 'avih'
static constexpr uint32_t FOURCC_strl = 0x6C727473;  // 'strl'
static constexpr uint32_t FOURCC_strh = 0x68727473;  // 'strh'
static constexpr uint32_t FOURCC_strf = 0x66727473;  // 'strf'
static constexpr uint32_t FOURCC_movi = 0x69766F6D;  // 'movi'
static constexpr uint32_t FOURCC_vids = 0x73646976;  // 'vids'
static constexpr uint32_t FOURCC_auds = 0x73647561;  // 'auds'

// Video stream chunk IDs
static constexpr uint32_t FOURCC_00dc = 0x63643030;  // '00dc' - compressed video
static constexpr uint32_t FOURCC_00db = 0x62643030;  // '00db' - uncompressed video

// Audio stream chunk IDs
static constexpr uint32_t FOURCC_01wb = 0x62773130;  // '01wb' - audio data

AVIParser::AVIParser() {}

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

  // Parse AVI headers
  if (!this->parse_headers_()) {
    ESP_LOGE(TAG, "Failed to parse AVI headers");
    this->close();
    return false;
  }

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
    ESP_LOGI(TAG, "  Video: %ux%u, %.2f fps, codec: 0x%08X", this->video_info_.width, this->video_info_.height,
             (float) this->video_info_.fps_num / this->video_info_.fps_den, this->video_info_.codec);
  }
  if (this->has_audio_) {
    ESP_LOGI(TAG, "  Audio: %u Hz, %u channels, codec: 0x%04X", this->audio_info_.sample_rate,
             this->audio_info_.channels, this->audio_info_.codec);
  }

  return true;
}

void AVIParser::close() {
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
              // Stream header list
              this->parse_stream_header_();
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
        // continue if hdrl not found yet
        if (!found_hdrl) {
          ESP_LOGV(TAG, "movi chunk found before hdrl");
          continue;
        }
        // Movie data chunk
        found_movi = true;
        this->movi_offset_ = this->reader_->tell();
        this->movi_size_ = chunk_size - 4;
        ESP_LOGD(TAG, "Found movi chunk at offset %llu, size %u", this->movi_offset_, this->movi_size_);

        // Skip movi chunk for now (we'll come back to it)
        this->skip_bytes_(this->movi_size_);
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

    // Skip flags and priority
    this->skip_bytes_(8);

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
      ESP_LOGD(TAG, "Parsing BITMAPINFOHEADER, strf_size=%u", strf_size);

      uint32_t bi_size;
      if (!this->read_uint32_(bi_size)) {
        return false;
      }
      ESP_LOGD(TAG, "biSize=%u", bi_size);

      if (!this->read_uint32_(info->width)) {
        return false;
      }
      ESP_LOGD(TAG, "Width=%u", info->width);

      if (!this->read_uint32_(info->height)) {
        return false;
      }
      ESP_LOGD(TAG, "Height=%u", info->height);

      // Skip rest of bitmap header
      size_t bytes_read = 12;  // biSize + width + height
      if (strf_size > bytes_read) {
        this->skip_bytes_(strf_size - bytes_read);
      }
    } else if (info->type == AVIStreamType::AUDIO) {
      // Parse WAVEFORMATEX
      uint16_t format_tag;
      if (!this->read_uint16_(format_tag)) {
        return false;
      }
      info->codec = format_tag;

      if (!this->read_uint16_(info->channels)) {
        return false;
      }
      if (!this->read_uint32_(info->sample_rate)) {
        return false;
      }

      // Skip bytes per second and block align
      this->skip_bytes_(6);

      if (!this->read_uint16_(info->bits_per_sample)) {
        return false;
      }

      // Skip rest
      this->skip_bytes_(strf_size - 16);
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

int AVIParser::read_next_frame(AVIFrame &frame, uint8_t *buffer, size_t buffer_size) {
  if (!this->is_open()) {
    return -1;
  }

  // Search for next frame chunk
  while (this->current_offset_ < this->movi_size_) {
    uint32_t chunk_id;
    if (!this->read_fourcc_(chunk_id)) {
      return -1;
    }

    uint32_t chunk_size;
    if (!this->read_uint32_(chunk_size)) {
      return -1;
    }

    this->current_offset_ += 8;

    // Check if this is a video or audio chunk
    bool is_video = (chunk_id == FOURCC_00dc || chunk_id == FOURCC_00db);
    bool is_audio = (chunk_id == FOURCC_01wb);

    if (is_video || is_audio) {
      // Found a frame
      frame.stream_type = is_video ? AVIStreamType::VIDEO : AVIStreamType::AUDIO;
      frame.offset = this->reader_->tell();
      frame.size = chunk_size;
      frame.keyframe = true;   // MJPEG frames are all keyframes
      frame.timestamp_ms = 0;  // TODO: Calculate from frame rate

      // Check buffer size
      if (chunk_size > buffer_size) {
        ESP_LOGE(TAG, "Frame too large: %u > %zu", chunk_size, buffer_size);
        return -1;
      }

      // Read frame data
      int bytes_read = this->reader_->read(buffer, chunk_size);
      if (bytes_read != static_cast<int>(chunk_size)) {
        ESP_LOGE(TAG, "Failed to read frame data");
        return -1;
      }

      this->current_offset_ += chunk_size;

      // Align to word boundary
      if (chunk_size & 1) {
        this->skip_bytes_(1);
        this->current_offset_ += 1;
      }

      if (is_video) {
        this->current_frame_++;
      }

      return bytes_read;
    } else {
      // Skip unknown chunk
      this->skip_bytes_(chunk_size);
      this->current_offset_ += chunk_size;

      // Align to word boundary
      if (chunk_size & 1) {
        this->skip_bytes_(1);
        this->current_offset_ += 1;
      }
    }
  }

  // End of file
  return 0;
}

bool AVIParser::seek_to_frame(uint32_t frame_number) {
  // TODO: Implement index-based seeking
  // For now, just rewind and skip frames
  if (!this->rewind()) {
    return false;
  }

  AVIFrame frame;
  uint8_t dummy_buffer[16];  // Just for skipping

  for (uint32_t i = 0; i < frame_number; i++) {
    if (this->read_next_frame(frame, dummy_buffer, sizeof(dummy_buffer)) <= 0) {
      return false;
    }

    // Skip to next frame
    if (frame.stream_type == AVIStreamType::VIDEO) {
      this->skip_bytes_(frame.size);
    }
  }

  return true;
}

bool AVIParser::rewind() {
  if (!this->is_open()) {
    return false;
  }

  if (!this->reader_->seek(this->movi_offset_)) {
    return false;
  }

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
  uint64_t current = this->reader_->tell();
  return this->reader_->seek(current + count);
}

}  // namespace simple_video_player
}  // namespace esphome
