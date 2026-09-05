/**
 * @file avi_parser.cpp
 * @brief Implementation of lightweight AVI container parser
 */

#include "avi_parser.h"
#include "esphome/core/log.h"
#include <cinttypes>
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
              this->parse_stream_header_();

              // Always skip to end of strl LIST in case there are additional chunks (strn, indx, etc.)
              // sub_size includes the strl_type we already read, so subtract 4
              uint64_t bytes_consumed = this->reader_->tell() - strl_start;
              if (bytes_consumed < (sub_size - 4)) {
                uint64_t bytes_remaining = (sub_size - 4) - bytes_consumed;
                this->skip_bytes_(bytes_remaining);
              }
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
        this->movi_size_ = chunk_size - 4;

        // We're now positioned at the start of movi data - ready for read_next_frame()
        // Break immediately - if we continue the loop, it will read the first frame chunk as a fourcc!
        break;
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

      // Skip rest of WAVEFORMATEX if there's extra data (cbSize + extra format data)
      if (strf_size > 16) {
        if (!this->skip_bytes_(strf_size - 16)) {
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

int AVIParser::read_next_frame(AVIFrame &frame, uint8_t *buffer, size_t buffer_size) {
  if (!this->is_open()) {
    return -1;
  }

  // Search for next frame chunk
  while (this->current_offset_ < this->movi_size_) {
    uint32_t chunk_id;
    if (!this->read_fourcc_(chunk_id)) {
      ESP_LOGW(TAG, "Failed to read chunk_id at offset %llu", this->current_offset_);
      return -1;
    }

    uint32_t chunk_size;
    if (!this->read_uint32_(chunk_size)) {
      return -1;
    }

    this->current_offset_ += 8;

    // Handle LIST 'rec ' chunks (some AVI files wrap frames in these)
    if (chunk_id == FOURCC_LIST) {
      uint32_t list_type;
      if (!this->read_fourcc_(list_type)) {
        return -1;
      }
      this->current_offset_ += 4;
      // Just skip into the LIST - the next iteration will read the actual frame chunk
      continue;
    }

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
        ESP_LOGE(TAG, "Frame too large: %" PRIu32 " > %zu", chunk_size, buffer_size);
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
  // Discard via the streaming read() path, not seek(): a forward skip within a sequential
  // stream is exactly what read_chunk() is for -- it serves straight out of
  // BufferedFileReader's read-ahead buffer whenever the bytes are already there (which they
  // almost always are here), with no extra storage-worker round trip at all. seek() is a
  // separate, real worker operation for actual random-access jumps; BufferedFileReader::seek()
  // unconditionally discards the whole read-ahead buffer on every call, so routing this
  // function's very frequent small forward skips through it would mean throwing away and
  // re-fetching that buffer to skip as little as a single byte. This function is the hot path
  // for AVI parsing (called once per interleaved audio/video chunk, and again for every
  // odd-sized chunk's 1-byte alignment pad).
  uint8_t scratch[512];
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
