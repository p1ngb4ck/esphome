/**
 * @file mkv_demuxer.cpp
 * @brief Matroska/MKV container parser implementation
 */

#include "mkv_demuxer.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace video_player {

static const char *const TAG = "mkv_demuxer";

// ========== EBMLReader Implementation ==========

bool EBMLReader::read_element_id(FILE *file, uint32_t &id) {
  // EBML variable-length ID
  // First byte indicates length by number of leading zero bits
  uint8_t first_byte;
  if (fread(&first_byte, 1, 1, file) != 1) {
    return false;
  }

  // Determine length from leading zeros
  size_t length = 1;
  if ((first_byte & 0x80) == 0x80) {
    length = 1;
  } else if ((first_byte & 0xC0) == 0x40) {
    length = 2;
  } else if ((first_byte & 0xE0) == 0x20) {
    length = 3;
  } else if ((first_byte & 0xF0) == 0x10) {
    length = 4;
  } else {
    ESP_LOGE(TAG, "Invalid EBML ID length");
    return false;
  }

  // Read remaining bytes
  id = first_byte;
  for (size_t i = 1; i < length; i++) {
    uint8_t byte;
    if (fread(&byte, 1, 1, file) != 1) {
      return false;
    }
    id = (id << 8) | byte;
  }

  return true;
}

bool EBMLReader::read_element_size(FILE *file, uint64_t &size) {
  // EBML variable-length size encoding
  uint8_t first_byte;
  if (fread(&first_byte, 1, 1, file) != 1) {
    return false;
  }

  // Determine length from leading zeros
  size_t length = 0;
  uint8_t mask = 0x80;
  for (size_t i = 0; i < 8; i++) {
    if (first_byte & (mask >> i)) {
      length = i + 1;
      break;
    }
  }

  if (length == 0) {
    ESP_LOGE(TAG, "Invalid EBML size encoding");
    return false;
  }

  // Remove length marker and read value
  size = first_byte & ((1 << (8 - length)) - 1);
  for (size_t i = 1; i < length; i++) {
    uint8_t byte;
    if (fread(&byte, 1, 1, file) != 1) {
      return false;
    }
    size = (size << 8) | byte;
  }

  return true;
}

bool EBMLReader::read_uint(FILE *file, uint64_t &value, size_t length) {
  if (length == 0 || length > 8) {
    return false;
  }

  value = 0;
  for (size_t i = 0; i < length; i++) {
    uint8_t byte;
    if (fread(&byte, 1, 1, file) != 1) {
      return false;
    }
    value = (value << 8) | byte;
  }

  return true;
}

bool EBMLReader::read_float(FILE *file, double &value, size_t length) {
  if (length == 4) {
    // 32-bit float
    uint32_t bits;
    if (!read_uint(file, reinterpret_cast<uint64_t &>(bits), 4)) {
      return false;
    }
    float f;
    memcpy(&f, &bits, 4);
    value = f;
    return true;
  } else if (length == 8) {
    // 64-bit double
    uint64_t bits;
    if (!read_uint(file, bits, 8)) {
      return false;
    }
    memcpy(&value, &bits, 8);
    return true;
  }
  return false;
}

bool EBMLReader::read_string(FILE *file, std::string &str, size_t length) {
  if (length == 0) {
    str.clear();
    return true;
  }

  str.resize(length);
  if (fread(&str[0], 1, length, file) != length) {
    return false;
  }

  return true;
}

// ========== MKVDemuxer Implementation ==========

MKVDemuxer::MKVDemuxer() {}

MKVDemuxer::~MKVDemuxer() { this->close(); }

bool MKVDemuxer::open(const std::string &file_path) {
  this->close();

  this->file_ = fopen(file_path.c_str(), "rb");
  if (!this->file_) {
    ESP_LOGE(TAG, "Failed to open file: %s", file_path.c_str());
    return false;
  }

  this->file_path_ = file_path;

  // Get file size
  fseek(this->file_, 0, SEEK_END);
  this->file_size_ = ftell(this->file_);
  fseek(this->file_, 0, SEEK_SET);

  ESP_LOGI(TAG, "Opened MKV file: %s (%llu bytes)", file_path.c_str(), this->file_size_);

  // Parse EBML header
  if (!this->parse_ebml_header_()) {
    ESP_LOGE(TAG, "Failed to parse EBML header");
    this->close();
    return false;
  }

  // Parse Segment
  if (!this->parse_segment_()) {
    ESP_LOGE(TAG, "Failed to parse Segment");
    this->close();
    return false;
  }

  if (!this->has_video_ && !this->has_audio_) {
    ESP_LOGE(TAG, "No video or audio tracks found");
    this->close();
    return false;
  }

  ESP_LOGI(TAG, "MKV file parsed successfully");
  return true;
}

void MKVDemuxer::close() {
  if (this->file_) {
    fclose(this->file_);
    this->file_ = nullptr;
  }

  this->has_video_ = false;
  this->has_audio_ = false;
  this->current_video_sample_ = 0;
  this->current_audio_sample_ = 0;
}

void MKVDemuxer::reset() {
  this->current_video_sample_ = 0;
  this->current_audio_sample_ = 0;
  this->current_cluster_offset_ = this->first_cluster_offset_;
  this->current_cluster_timecode_ = 0;
}

bool MKVDemuxer::parse_ebml_header_() {
  uint32_t id;
  uint64_t size;

  // Read EBML element
  if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
    ESP_LOGE(TAG, "Failed to read EBML header");
    return false;
  }

  if (id != EBML_ID_EBML) {
    ESP_LOGE(TAG, "Invalid EBML header ID: 0x%08X", id);
    return false;
  }

  ESP_LOGD(TAG, "EBML header size: %llu", size);

  // Skip EBML header content (we just need to verify it exists)
  fseek(this->file_, size, SEEK_CUR);

  return true;
}

bool MKVDemuxer::parse_segment_() {
  uint32_t id;
  uint64_t size;

  // Read Segment element
  if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
    ESP_LOGE(TAG, "Failed to read Segment");
    return false;
  }

  if (id != EBML_ID_SEGMENT) {
    ESP_LOGE(TAG, "Invalid Segment ID: 0x%08X", id);
    return false;
  }

  this->segment_offset_ = ftell(this->file_);
  uint64_t segment_end = this->segment_offset_ + size;

  ESP_LOGD(TAG, "Segment size: %llu bytes", size);

  // Parse Segment children (Info, Tracks, Clusters)
  while (ftell(this->file_) < segment_end) {
    uint64_t element_start = ftell(this->file_);

    if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
      break;
    }

    ESP_LOGD(TAG, "Segment child: ID=0x%08X, size=%llu", id, size);

    switch (id) {
      case EBML_ID_INFO:
        if (!this->parse_info_()) {
          ESP_LOGW(TAG, "Failed to parse Info");
        }
        break;

      case EBML_ID_TRACKS:
        if (!this->parse_tracks_()) {
          ESP_LOGE(TAG, "Failed to parse Tracks");
          return false;
        }
        break;

      case EBML_ID_CLUSTER:
        // Store first cluster offset for seeking/reset
        if (this->first_cluster_offset_ == 0) {
          this->first_cluster_offset_ = element_start;
          this->current_cluster_offset_ = element_start;
          ESP_LOGI(TAG, "First cluster at offset: %llu", this->first_cluster_offset_);
        }
        // Skip cluster for now (will parse during playback)
        fseek(this->file_, element_start + size + (ftell(this->file_) - element_start), SEEK_SET);
        break;

      case EBML_ID_SEEKHEAD:
        // Skip SeekHead (index) for now
        this->skip_element_(size);
        break;

      default:
        // Skip unknown elements
        this->skip_element_(size);
        break;
    }
  }

  return true;
}

bool MKVDemuxer::parse_info_() {
  uint64_t info_end = ftell(this->file_);
  uint32_t id;
  uint64_t size;

  // Read Info size from current position
  if (!EBMLReader::read_element_size(this->file_, size)) {
    return false;
  }
  info_end += size;

  while (ftell(this->file_) < info_end) {
    if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
      break;
    }

    switch (id) {
      case EBML_ID_TIMECODESCALE:
        if (!EBMLReader::read_uint(this->file_, this->timecode_scale_, size)) {
          return false;
        }
        ESP_LOGI(TAG, "Timecode scale: %llu ns", this->timecode_scale_);
        break;

      case EBML_ID_DURATION:
        if (!EBMLReader::read_float(this->file_, this->duration_seconds_, size)) {
          return false;
        }
        ESP_LOGI(TAG, "Duration: %.2f seconds", this->duration_seconds_);
        break;

      default:
        this->skip_element_(size);
        break;
    }
  }

  return true;
}

bool MKVDemuxer::parse_tracks_() {
  uint64_t tracks_end = ftell(this->file_);
  uint32_t id;
  uint64_t size;

  // Tracks element already read, get its size
  if (!EBMLReader::read_element_size(this->file_, size)) {
    return false;
  }
  tracks_end += size;

  while (ftell(this->file_) < tracks_end) {
    if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
      break;
    }

    if (id == EBML_ID_TRACKENTRY) {
      if (!this->parse_track_entry_()) {
        ESP_LOGW(TAG, "Failed to parse TrackEntry");
      }
    } else {
      this->skip_element_(size);
    }
  }

  return true;
}

bool MKVDemuxer::parse_track_entry_() {
  uint64_t entry_end = ftell(this->file_);
  uint32_t id;
  uint64_t size;

  // TrackEntry size already read
  // We're now inside the TrackEntry element
  entry_end = ftell(this->file_) - (ftell(this->file_) - entry_end);  // Calculate end position

  uint8_t track_number = 0;
  uint8_t track_type = 0;
  std::string codec_id;
  uint16_t width = 0, height = 0;
  uint16_t sample_rate = 0, channels = 0, bit_depth = 16;

  while (ftell(this->file_) < entry_end) {
    uint64_t pos = ftell(this->file_);
    if (!EBMLReader::read_element_id(this->file_, id)) {
      break;
    }

    // Check if we've gone past the entry
    if (pos >= entry_end) {
      break;
    }

    if (!EBMLReader::read_element_size(this->file_, size)) {
      break;
    }

    switch (id) {
      case EBML_ID_TRACKNUMBER: {
        uint64_t track_num;
        if (EBMLReader::read_uint(this->file_, track_num, size)) {
          track_number = static_cast<uint8_t>(track_num);
        }
        break;
      }

      case EBML_ID_TRACKTYPE: {
        uint64_t type;
        if (EBMLReader::read_uint(this->file_, type, size)) {
          track_type = static_cast<uint8_t>(type);
        }
        break;
      }

      case EBML_ID_CODECID:
        EBMLReader::read_string(this->file_, codec_id, size);
        break;

      case EBML_ID_VIDEO: {
        // Parse video settings
        uint64_t video_end = ftell(this->file_) + size;
        while (ftell(this->file_) < video_end) {
          uint32_t vid_id;
          uint64_t vid_size;
          if (!EBMLReader::read_element_id(this->file_, vid_id) ||
              !EBMLReader::read_element_size(this->file_, vid_size)) {
            break;
          }

          if (vid_id == EBML_ID_PIXELWIDTH) {
            uint64_t w;
            if (EBMLReader::read_uint(this->file_, w, vid_size)) {
              width = static_cast<uint16_t>(w);
            }
          } else if (vid_id == EBML_ID_PIXELHEIGHT) {
            uint64_t h;
            if (EBMLReader::read_uint(this->file_, h, vid_size)) {
              height = static_cast<uint16_t>(h);
            }
          } else {
            fseek(this->file_, vid_size, SEEK_CUR);
          }
        }
        break;
      }

      case EBML_ID_AUDIO: {
        // Parse audio settings
        uint64_t audio_end = ftell(this->file_) + size;
        while (ftell(this->file_) < audio_end) {
          uint32_t aud_id;
          uint64_t aud_size;
          if (!EBMLReader::read_element_id(this->file_, aud_id) ||
              !EBMLReader::read_element_size(this->file_, aud_size)) {
            break;
          }

          if (aud_id == EBML_ID_SAMPLINGFREQ) {
            double sr;
            if (EBMLReader::read_float(this->file_, sr, aud_size)) {
              sample_rate = static_cast<uint16_t>(sr);
            }
          } else if (aud_id == EBML_ID_CHANNELS) {
            uint64_t ch;
            if (EBMLReader::read_uint(this->file_, ch, aud_size)) {
              channels = static_cast<uint16_t>(ch);
            }
          } else if (aud_id == EBML_ID_BITDEPTH) {
            uint64_t bd;
            if (EBMLReader::read_uint(this->file_, bd, aud_size)) {
              bit_depth = static_cast<uint16_t>(bd);
            }
          } else {
            fseek(this->file_, aud_size, SEEK_CUR);
          }
        }
        break;
      }

      default:
        this->skip_element_(size);
        break;
    }
  }

  // Store track info
  if (track_type == MKV_TRACK_TYPE_VIDEO && codec_id == "V_MPEG4/ISO/AVC") {
    this->has_video_ = true;
    this->video_track_number_ = track_number;
    this->video_track_.track_id = track_number;
    this->video_track_.width = width;
    this->video_track_.height = height;
    this->video_track_.timescale = 1000000000 / this->timecode_scale_;  // Convert to Hz
    ESP_LOGI(TAG, "Video track: %ux%u H.264", width, height);
  } else if (track_type == MKV_TRACK_TYPE_AUDIO) {
    this->has_audio_ = true;
    this->audio_track_number_ = track_number;
    this->audio_track_.track_id = track_number;
    this->audio_track_.sample_rate = sample_rate;
    this->audio_track_.channels = channels;
    this->audio_track_.bits_per_sample = bit_depth;
    this->audio_track_.codec_type = this->detect_audio_codec_(codec_id);
    this->audio_track_.timescale = 1000000000 / this->timecode_scale_;

    const char *codec_name = "unknown";
    if (this->audio_track_.codec_type == AudioCodecType::AAC)
      codec_name = "AAC";
    else if (this->audio_track_.codec_type == AudioCodecType::MP3)
      codec_name = "MP3";
    else if (this->audio_track_.codec_type == AudioCodecType::FLAC)
      codec_name = "FLAC";

    ESP_LOGI(TAG, "Audio track: %s %u Hz, %u channels, %u bits", codec_name, sample_rate, channels, bit_depth);
  }

  return true;
}

AudioCodecType MKVDemuxer::detect_audio_codec_(const std::string &codec_id) {
  if (codec_id == "A_AAC") {
    return AudioCodecType::AAC;
  } else if (codec_id == "A_MPEG/L3") {
    return AudioCodecType::MP3;
  } else if (codec_id == "A_FLAC") {
    return AudioCodecType::FLAC;
  }
  return AudioCodecType::UNKNOWN;
}

bool MKVDemuxer::skip_element_(uint64_t size) {
  if (size == 0) {
    return true;
  }
  return fseek(this->file_, size, SEEK_CUR) == 0;
}

uint64_t MKVDemuxer::get_video_duration_ms() const { return static_cast<uint64_t>(this->duration_seconds_ * 1000.0); }

uint64_t MKVDemuxer::get_audio_duration_ms() const { return static_cast<uint64_t>(this->duration_seconds_ * 1000.0); }

bool MKVDemuxer::parse_cluster_() {
  if (!this->file_ || this->current_cluster_offset_ == 0) {
    return false;
  }

  // Seek to cluster
  fseek(this->file_, this->current_cluster_offset_, SEEK_SET);

  uint32_t id;
  uint64_t size;

  // Read Cluster element
  if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
    ESP_LOGE(TAG, "Failed to read Cluster element");
    return false;
  }

  if (id != EBML_ID_CLUSTER) {
    ESP_LOGE(TAG, "Not a Cluster element: 0x%08X", id);
    return false;
  }

  uint64_t cluster_end = ftell(this->file_) + size;

  // Parse Cluster children (Timecode, SimpleBlock, BlockGroup)
  while (ftell(this->file_) < cluster_end) {
    uint64_t element_start = ftell(this->file_);

    if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
      break;
    }

    if (id == EBML_ID_TIMECODE) {
      // Read cluster timecode
      if (!EBMLReader::read_uint(this->file_, this->current_cluster_timecode_, size)) {
        ESP_LOGW(TAG, "Failed to read cluster timecode");
        this->skip_element_(size);
      }
      ESP_LOGD(TAG, "Cluster timecode: %llu", this->current_cluster_timecode_);
    } else {
      // Skip all other elements for now - blocks will be parsed on demand
      this->skip_element_(size);
    }
  }

  // Move to next cluster
  this->current_cluster_offset_ = cluster_end;

  return true;
}

bool MKVDemuxer::get_next_video_sample(Sample &sample, uint8_t *data, size_t max_size) {
  if (!this->has_video_ || !this->file_) {
    return false;
  }

  // Parse cluster if needed
  if (this->current_cluster_offset_ == this->first_cluster_offset_ && this->current_cluster_timecode_ == 0) {
    if (!this->parse_cluster_()) {
      return false;
    }
  }

  // Seek to cluster data area
  fseek(this->file_, this->current_cluster_offset_, SEEK_SET);

  uint32_t id;
  uint64_t size;

  // Search for next video block in cluster
  while (ftell(this->file_) < this->file_size_) {
    uint64_t element_start = ftell(this->file_);

    if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
      // Try next cluster
      if (!this->parse_cluster_()) {
        return false;
      }
      continue;
    }

    // Check if we hit another cluster
    if (id == EBML_ID_CLUSTER) {
      // Save position and parse new cluster
      this->current_cluster_offset_ = element_start;
      if (!this->parse_cluster_()) {
        return false;
      }
      continue;
    }

    // SimpleBlock format:
    // - Track number (variable-length)
    // - Timecode (int16, relative to cluster)
    // - Flags (uint8)
    // - Frame data
    if (id == EBML_ID_SIMPLEBLOCK) {
      uint64_t block_start = ftell(this->file_);

      // Read track number (EBML variable-length)
      uint8_t track_byte;
      if (fread(&track_byte, 1, 1, this->file_) != 1) {
        this->skip_element_(size - 1);
        continue;
      }

      uint8_t track_number = track_byte & 0x7F;  // Simple case: single byte track number

      // Check if this is our video track
      if (track_number != this->video_track_number_) {
        // Wrong track, skip this block
        fseek(this->file_, block_start + size, SEEK_SET);
        continue;
      }

      // Read timecode (int16, big-endian, relative to cluster)
      int16_t relative_timecode;
      uint8_t tc_bytes[2];
      if (fread(tc_bytes, 1, 2, this->file_) != 2) {
        this->skip_element_(size - 3);
        continue;
      }
      relative_timecode = (static_cast<int16_t>(tc_bytes[0]) << 8) | tc_bytes[1];

      // Read flags
      uint8_t flags;
      if (fread(&flags, 1, 1, this->file_) != 1) {
        this->skip_element_(size - 4);
        continue;
      }

      // Calculate frame size (remaining bytes in block)
      size_t bytes_read = ftell(this->file_) - block_start;
      size_t frame_size = size - bytes_read;

      if (frame_size > max_size) {
        ESP_LOGE(TAG, "Video frame too large: %zu bytes (max: %zu)", frame_size, max_size);
        this->skip_element_(frame_size);
        continue;
      }

      // Read frame data
      if (fread(data, 1, frame_size, this->file_) != frame_size) {
        ESP_LOGE(TAG, "Failed to read video frame data");
        return false;
      }

      // Calculate absolute timestamp
      // MKV timecode = (cluster_timecode + relative_timecode) * timecode_scale / 1,000,000 ms
      uint64_t absolute_timecode = this->current_cluster_timecode_ + relative_timecode;
      uint64_t timestamp_ms = (absolute_timecode * this->timecode_scale_) / 1000000;

      // Fill sample info
      sample.offset = element_start;
      sample.size = frame_size;
      sample.timestamp_ms = timestamp_ms;
      sample.duration_ms = 0;                    // MKV doesn't store frame duration in SimpleBlock
      sample.is_keyframe = (flags & 0x80) != 0;  // Bit 7 = keyframe flag

      this->current_video_sample_++;

      ESP_LOGV(TAG, "Video sample #%u: size=%u, timestamp=%llu ms, keyframe=%d", this->current_video_sample_,
               sample.size, sample.timestamp_ms, sample.is_keyframe);

      return true;
    }

    // BlockGroup is more complex (has Duration element)
    if (id == EBML_ID_BLOCKGROUP) {
      uint64_t blockgroup_end = ftell(this->file_) + size;
      uint32_t duration_ms = 0;

      // Parse BlockGroup children
      while (ftell(this->file_) < blockgroup_end) {
        uint32_t bg_id;
        uint64_t bg_size;

        if (!EBMLReader::read_element_id(this->file_, bg_id) || !EBMLReader::read_element_size(this->file_, bg_size)) {
          break;
        }

        if (bg_id == EBML_ID_BLOCK) {
          // Same structure as SimpleBlock but without flags
          uint64_t block_start = ftell(this->file_);

          uint8_t track_byte;
          if (fread(&track_byte, 1, 1, this->file_) != 1) {
            this->skip_element_(bg_size - 1);
            continue;
          }

          uint8_t track_number = track_byte & 0x7F;

          if (track_number != this->video_track_number_) {
            fseek(this->file_, block_start + bg_size, SEEK_SET);
            continue;
          }

          int16_t relative_timecode;
          uint8_t tc_bytes[2];
          if (fread(tc_bytes, 1, 2, this->file_) != 2) {
            this->skip_element_(bg_size - 3);
            continue;
          }
          relative_timecode = (static_cast<int16_t>(tc_bytes[0]) << 8) | tc_bytes[1];

          // No flags byte in Block (only in SimpleBlock)
          size_t bytes_read = ftell(this->file_) - block_start;
          size_t frame_size = bg_size - bytes_read;

          if (frame_size > max_size) {
            ESP_LOGE(TAG, "Video frame too large: %zu bytes", frame_size);
            this->skip_element_(frame_size);
            break;
          }

          if (fread(data, 1, frame_size, this->file_) != frame_size) {
            ESP_LOGE(TAG, "Failed to read video frame data");
            return false;
          }

          uint64_t absolute_timecode = this->current_cluster_timecode_ + relative_timecode;
          uint64_t timestamp_ms = (absolute_timecode * this->timecode_scale_) / 1000000;

          sample.offset = element_start;
          sample.size = frame_size;
          sample.timestamp_ms = timestamp_ms;
          sample.duration_ms = duration_ms;
          sample.is_keyframe = true;  // Assume keyframe for BlockGroup

          this->current_video_sample_++;

          ESP_LOGV(TAG, "Video sample #%u: size=%u, timestamp=%llu ms", this->current_video_sample_, sample.size,
                   sample.timestamp_ms);

          return true;

        } else if (bg_id == EBML_ID_DURATION) {
          // Duration in timecode scale units
          uint64_t duration_ticks;
          if (EBMLReader::read_uint(this->file_, duration_ticks, bg_size)) {
            duration_ms = (duration_ticks * this->timecode_scale_) / 1000000;
          }
        } else {
          this->skip_element_(bg_size);
        }
      }

      // If we processed BlockGroup, continue searching
      continue;
    }

    // Skip other elements
    this->skip_element_(size);
  }

  // Reached end of file
  return false;
}

bool MKVDemuxer::get_next_audio_sample(Sample &sample, uint8_t *data, size_t max_size) {
  if (!this->has_audio_ || !this->file_) {
    return false;
  }

  // Parse cluster if needed
  if (this->current_cluster_offset_ == this->first_cluster_offset_ && this->current_cluster_timecode_ == 0) {
    if (!this->parse_cluster_()) {
      return false;
    }
  }

  // Seek to cluster data area
  fseek(this->file_, this->current_cluster_offset_, SEEK_SET);

  uint32_t id;
  uint64_t size;

  // Search for next audio block in cluster
  while (ftell(this->file_) < this->file_size_) {
    uint64_t element_start = ftell(this->file_);

    if (!EBMLReader::read_element_id(this->file_, id) || !EBMLReader::read_element_size(this->file_, size)) {
      // Try next cluster
      if (!this->parse_cluster_()) {
        return false;
      }
      continue;
    }

    // Check if we hit another cluster
    if (id == EBML_ID_CLUSTER) {
      this->current_cluster_offset_ = element_start;
      if (!this->parse_cluster_()) {
        return false;
      }
      continue;
    }

    if (id == EBML_ID_SIMPLEBLOCK) {
      uint64_t block_start = ftell(this->file_);

      uint8_t track_byte;
      if (fread(&track_byte, 1, 1, this->file_) != 1) {
        this->skip_element_(size - 1);
        continue;
      }

      uint8_t track_number = track_byte & 0x7F;

      if (track_number != this->audio_track_number_) {
        fseek(this->file_, block_start + size, SEEK_SET);
        continue;
      }

      int16_t relative_timecode;
      uint8_t tc_bytes[2];
      if (fread(tc_bytes, 1, 2, this->file_) != 2) {
        this->skip_element_(size - 3);
        continue;
      }
      relative_timecode = (static_cast<int16_t>(tc_bytes[0]) << 8) | tc_bytes[1];

      uint8_t flags;
      if (fread(&flags, 1, 1, this->file_) != 1) {
        this->skip_element_(size - 4);
        continue;
      }

      size_t bytes_read = ftell(this->file_) - block_start;
      size_t sample_size = size - bytes_read;

      if (sample_size > max_size) {
        ESP_LOGE(TAG, "Audio sample too large: %zu bytes (max: %zu)", sample_size, max_size);
        this->skip_element_(sample_size);
        continue;
      }

      if (fread(data, 1, sample_size, this->file_) != sample_size) {
        ESP_LOGE(TAG, "Failed to read audio sample data");
        return false;
      }

      uint64_t absolute_timecode = this->current_cluster_timecode_ + relative_timecode;
      uint64_t timestamp_ms = (absolute_timecode * this->timecode_scale_) / 1000000;

      sample.offset = element_start;
      sample.size = sample_size;
      sample.timestamp_ms = timestamp_ms;
      sample.duration_ms = 0;
      sample.is_keyframe = false;  // Audio doesn't have keyframes

      this->current_audio_sample_++;

      ESP_LOGV(TAG, "Audio sample #%u: size=%u, timestamp=%llu ms", this->current_audio_sample_, sample.size,
               sample.timestamp_ms);

      return true;
    }

    if (id == EBML_ID_BLOCKGROUP) {
      uint64_t blockgroup_end = ftell(this->file_) + size;
      uint32_t duration_ms = 0;

      while (ftell(this->file_) < blockgroup_end) {
        uint32_t bg_id;
        uint64_t bg_size;

        if (!EBMLReader::read_element_id(this->file_, bg_id) || !EBMLReader::read_element_size(this->file_, bg_size)) {
          break;
        }

        if (bg_id == EBML_ID_BLOCK) {
          uint64_t block_start = ftell(this->file_);

          uint8_t track_byte;
          if (fread(&track_byte, 1, 1, this->file_) != 1) {
            this->skip_element_(bg_size - 1);
            continue;
          }

          uint8_t track_number = track_byte & 0x7F;

          if (track_number != this->audio_track_number_) {
            fseek(this->file_, block_start + bg_size, SEEK_SET);
            continue;
          }

          int16_t relative_timecode;
          uint8_t tc_bytes[2];
          if (fread(tc_bytes, 1, 2, this->file_) != 2) {
            this->skip_element_(bg_size - 3);
            continue;
          }
          relative_timecode = (static_cast<int16_t>(tc_bytes[0]) << 8) | tc_bytes[1];

          size_t bytes_read = ftell(this->file_) - block_start;
          size_t sample_size = bg_size - bytes_read;

          if (sample_size > max_size) {
            ESP_LOGE(TAG, "Audio sample too large: %zu bytes", sample_size);
            this->skip_element_(sample_size);
            break;
          }

          if (fread(data, 1, sample_size, this->file_) != sample_size) {
            ESP_LOGE(TAG, "Failed to read audio sample data");
            return false;
          }

          uint64_t absolute_timecode = this->current_cluster_timecode_ + relative_timecode;
          uint64_t timestamp_ms = (absolute_timecode * this->timecode_scale_) / 1000000;

          sample.offset = element_start;
          sample.size = sample_size;
          sample.timestamp_ms = timestamp_ms;
          sample.duration_ms = duration_ms;
          sample.is_keyframe = false;

          this->current_audio_sample_++;

          ESP_LOGV(TAG, "Audio sample #%u: size=%u, timestamp=%llu ms", this->current_audio_sample_, sample.size,
                   sample.timestamp_ms);

          return true;

        } else if (bg_id == EBML_ID_DURATION) {
          uint64_t duration_ticks;
          if (EBMLReader::read_uint(this->file_, duration_ticks, bg_size)) {
            duration_ms = (duration_ticks * this->timecode_scale_) / 1000000;
          }
        } else {
          this->skip_element_(bg_size);
        }
      }

      continue;
    }

    this->skip_element_(size);
  }

  return false;
}

bool MKVDemuxer::seek_video(uint64_t timestamp_ms) {
  if (!this->has_video_) {
    return false;
  }

  // Simple implementation: reset to beginning and scan forward
  // A full implementation would use CuePoint/SeekHead for efficient seeking
  this->reset();

  Sample sample;
  uint8_t dummy_buffer[16];  // Small buffer just for scanning

  // Scan forward until we find a keyframe at or before target timestamp
  while (true) {
    uint64_t current_pos = ftell(this->file_);

    if (!this->get_next_video_sample(sample, dummy_buffer, sizeof(dummy_buffer))) {
      // Reached end, reset and return false
      this->reset();
      return false;
    }

    if (sample.timestamp_ms >= timestamp_ms && sample.is_keyframe) {
      // Found target keyframe, seek back to read it properly
      fseek(this->file_, current_pos, SEEK_SET);
      return true;
    }

    if (sample.timestamp_ms > timestamp_ms + 5000) {
      // Overshot by more than 5 seconds, give up
      this->reset();
      return false;
    }
  }
}

bool MKVDemuxer::seek_audio(uint64_t timestamp_ms) {
  if (!this->has_audio_) {
    return false;
  }

  // Simple implementation: reset and scan forward
  this->reset();

  Sample sample;
  uint8_t dummy_buffer[16];

  while (true) {
    uint64_t current_pos = ftell(this->file_);

    if (!this->get_next_audio_sample(sample, dummy_buffer, sizeof(dummy_buffer))) {
      this->reset();
      return false;
    }

    if (sample.timestamp_ms >= timestamp_ms) {
      fseek(this->file_, current_pos, SEEK_SET);
      return true;
    }

    if (sample.timestamp_ms > timestamp_ms + 5000) {
      this->reset();
      return false;
    }
  }
}

}  // namespace video_player
}  // namespace esphome
