/**
 * @file mp4_demuxer.cpp
 * @brief MP4 container parser implementation
 */

#include "mp4_demuxer.h"
#include <cstring>

namespace esphome {
namespace video_player {

static const char *const TAG = "mp4_demuxer";

// Helper to convert big-endian to native
static inline uint32_t be32_to_cpu(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | (static_cast<uint32_t>(data[3]));
}

static inline uint64_t be64_to_cpu(const uint8_t *data) {
  return (static_cast<uint64_t>(data[0]) << 56) | (static_cast<uint64_t>(data[1]) << 48) |
         (static_cast<uint64_t>(data[2]) << 40) | (static_cast<uint64_t>(data[3]) << 32) |
         (static_cast<uint64_t>(data[4]) << 24) | (static_cast<uint64_t>(data[5]) << 16) |
         (static_cast<uint64_t>(data[6]) << 8) | (static_cast<uint64_t>(data[7]));
}

MP4Demuxer::MP4Demuxer() {}

MP4Demuxer::~MP4Demuxer() { this->close(); }

bool MP4Demuxer::open(const std::string &file_path) {
  this->close();

  this->file_path_ = file_path;
  this->file_ = fopen(file_path.c_str(), "rb");
  if (this->file_ == nullptr) {
    ESP_LOGE(TAG, "Failed to open file: %s", file_path.c_str());
    return false;
  }

  // Get file size
  fseek(this->file_, 0, SEEK_END);
  this->file_size_ = ftell(this->file_);
  fseek(this->file_, 0, SEEK_SET);

  ESP_LOGI(TAG, "Opened MP4 file: %s (size: %llu bytes)", file_path.c_str(),
           static_cast<unsigned long long>(this->file_size_));

  // Parse MP4 structure
  bool found_ftyp = false;
  bool found_moov = false;

  while (ftell(this->file_) < static_cast<long>(this->file_size_)) {
    uint32_t box_size, box_type;
    uint64_t box_start = ftell(this->file_);

    if (!this->read_box_header(box_size, box_type)) {
      ESP_LOGE(TAG, "Failed to read box header at offset %llu", static_cast<unsigned long long>(box_start));
      this->close();
      return false;
    }

    // Handle extended size (size == 1 means actual size is in next 8 bytes)
    uint64_t actual_size = box_size;
    if (box_size == 1) {
      if (!this->read_u64(actual_size)) {
        ESP_LOGE(TAG, "Failed to read extended box size");
        this->close();
        return false;
      }
    }

    ESP_LOGD(TAG, "Found box type: 0x%08X, size: %llu at offset %llu", box_type,
             static_cast<unsigned long long>(actual_size), static_cast<unsigned long long>(box_start));

    switch (box_type) {
      case BOX_TYPE_FTYP:
        if (!this->parse_ftyp_box(actual_size)) {
          ESP_LOGE(TAG, "Failed to parse ftyp box");
          this->close();
          return false;
        }
        found_ftyp = true;
        break;

      case BOX_TYPE_MOOV:
        if (!this->parse_moov_box(actual_size)) {
          ESP_LOGE(TAG, "Failed to parse moov box");
          this->close();
          return false;
        }
        found_moov = true;
        break;

      case BOX_TYPE_MDAT:
        // Store mdat offset for later frame reading
        this->mdat_offset_ = box_start + 8;  // Skip box header
        ESP_LOGI(TAG, "Found mdat at offset %llu", static_cast<unsigned long long>(this->mdat_offset_));
        // Skip mdat content (we'll read frames from it later)
        fseek(this->file_, box_start + actual_size, SEEK_SET);
        break;

      default:
        // Skip unknown boxes
        fseek(this->file_, box_start + actual_size, SEEK_SET);
        break;
    }
  }

  if (!found_ftyp || !found_moov) {
    ESP_LOGE(TAG, "Invalid MP4 file: missing ftyp or moov box");
    this->close();
    return false;
  }

  if (!this->has_video_ && !this->has_audio_) {
    ESP_LOGE(TAG, "No video or audio tracks found");
    this->close();
    return false;
  }

  ESP_LOGI(TAG, "MP4 parsing complete:");
  if (this->has_video_) {
    ESP_LOGI(TAG, "  Video: %ux%u, %u frames, %.2f seconds", this->video_track_.width, this->video_track_.height,
             this->video_track_.sample_count, static_cast<float>(this->get_video_duration_ms()) / 1000.0f);
  }
  if (this->has_audio_) {
    ESP_LOGI(TAG, "  Audio: %u Hz, %u channels, %u samples, %.2f seconds", this->audio_track_.sample_rate,
             this->audio_track_.channels, this->audio_track_.sample_count,
             static_cast<float>(this->get_audio_duration_ms()) / 1000.0f);
  }

  return true;
}

void MP4Demuxer::close() {
  if (this->file_ != nullptr) {
    fclose(this->file_);
    this->file_ = nullptr;
  }
  this->has_video_ = false;
  this->has_audio_ = false;
  this->video_track_ = VideoTrackInfo{};
  this->audio_track_ = AudioTrackInfo{};
  this->current_video_sample_ = 0;
  this->current_audio_sample_ = 0;
}

bool MP4Demuxer::get_next_video_sample(Sample &sample, uint8_t *data, size_t max_size) {
  if (!this->has_video_ || this->current_video_sample_ >= this->video_track_.sample_count) {
    return false;
  }

  uint32_t idx = this->current_video_sample_;
  sample.offset = this->video_track_.sample_offsets[idx];
  sample.size = this->video_track_.sample_sizes[idx];

  // Calculate timestamp in milliseconds
  uint64_t ts = 0;
  for (uint32_t i = 0; i < idx; i++) {
    ts += this->video_track_.sample_durations[i];
  }
  sample.timestamp_ms = (ts * 1000) / this->video_track_.timescale;
  sample.duration_ms = (this->video_track_.sample_durations[idx] * 1000) / this->video_track_.timescale;

  // Read frame data
  if (sample.size > max_size) {
    ESP_LOGE(TAG, "Video sample size (%u) exceeds buffer size (%zu)", sample.size, max_size);
    return false;
  }

  fseek(this->file_, sample.offset, SEEK_SET);
  size_t read = fread(data, 1, sample.size, this->file_);
  if (read != sample.size) {
    ESP_LOGE(TAG, "Failed to read video sample %u (expected %u, got %zu)", idx, sample.size, read);
    return false;
  }

  this->current_video_sample_++;
  return true;
}

bool MP4Demuxer::get_next_audio_sample(Sample &sample, uint8_t *data, size_t max_size) {
  if (!this->has_audio_ || this->current_audio_sample_ >= this->audio_track_.sample_count) {
    return false;
  }

  uint32_t idx = this->current_audio_sample_;
  sample.offset = this->audio_track_.sample_offsets[idx];
  sample.size = this->audio_track_.sample_sizes[idx];

  // Calculate timestamp in milliseconds
  uint64_t ts = 0;
  for (uint32_t i = 0; i < idx; i++) {
    ts += this->audio_track_.sample_durations[i];
  }
  sample.timestamp_ms = (ts * 1000) / this->audio_track_.timescale;
  sample.duration_ms = (this->audio_track_.sample_durations[idx] * 1000) / this->audio_track_.timescale;

  // Read audio data
  if (sample.size > max_size) {
    ESP_LOGE(TAG, "Audio sample size (%u) exceeds buffer size (%zu)", sample.size, max_size);
    return false;
  }

  fseek(this->file_, sample.offset, SEEK_SET);
  size_t read = fread(data, 1, sample.size, this->file_);
  if (read != sample.size) {
    ESP_LOGE(TAG, "Failed to read audio sample %u (expected %u, got %zu)", idx, sample.size, read);
    return false;
  }

  this->current_audio_sample_++;
  return true;
}

bool MP4Demuxer::seek_video(uint64_t timestamp_ms) {
  if (!this->has_video_) {
    return false;
  }

  // Find sample index for target timestamp
  uint64_t current_ts = 0;
  for (uint32_t i = 0; i < this->video_track_.sample_count; i++) {
    uint64_t sample_ts_ms = (current_ts * 1000) / this->video_track_.timescale;
    if (sample_ts_ms >= timestamp_ms) {
      this->current_video_sample_ = i;
      ESP_LOGD(TAG, "Seeked video to sample %u (timestamp %llu ms)", i, static_cast<unsigned long long>(sample_ts_ms));
      return true;
    }
    current_ts += this->video_track_.sample_durations[i];
  }

  // Seek to end
  this->current_video_sample_ = this->video_track_.sample_count;
  return false;
}

bool MP4Demuxer::seek_audio(uint64_t timestamp_ms) {
  if (!this->has_audio_) {
    return false;
  }

  // Find sample index for target timestamp
  uint64_t current_ts = 0;
  for (uint32_t i = 0; i < this->audio_track_.sample_count; i++) {
    uint64_t sample_ts_ms = (current_ts * 1000) / this->audio_track_.timescale;
    if (sample_ts_ms >= timestamp_ms) {
      this->current_audio_sample_ = i;
      ESP_LOGD(TAG, "Seeked audio to sample %u (timestamp %llu ms)", i, static_cast<unsigned long long>(sample_ts_ms));
      return true;
    }
    current_ts += this->audio_track_.sample_durations[i];
  }

  // Seek to end
  this->current_audio_sample_ = this->audio_track_.sample_count;
  return false;
}

void MP4Demuxer::reset() {
  this->current_video_sample_ = 0;
  this->current_audio_sample_ = 0;
}

// ========== File I/O Helpers ==========

bool MP4Demuxer::read_u32(uint32_t &value) {
  uint8_t buf[4];
  if (fread(buf, 1, 4, this->file_) != 4) {
    return false;
  }
  value = be32_to_cpu(buf);
  return true;
}

bool MP4Demuxer::read_u16(uint16_t &value) {
  uint8_t buf[2];
  if (fread(buf, 1, 2, this->file_) != 2) {
    return false;
  }
  value = (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
  return true;
}

bool MP4Demuxer::read_u8(uint8_t &value) { return fread(&value, 1, 1, this->file_) == 1; }

bool MP4Demuxer::read_u64(uint64_t &value) {
  uint8_t buf[8];
  if (fread(buf, 1, 8, this->file_) != 8) {
    return false;
  }
  value = be64_to_cpu(buf);
  return true;
}

bool MP4Demuxer::skip_bytes(size_t count) { return fseek(this->file_, count, SEEK_CUR) == 0; }

bool MP4Demuxer::read_box_header(uint32_t &size, uint32_t &type) {
  return this->read_u32(size) && this->read_u32(type);
}

// ========== Box Parsers ==========

bool MP4Demuxer::parse_ftyp_box(uint32_t size) {
  // Just verify it's a valid MP4 file - we don't need detailed brand info
  uint32_t major_brand;
  if (!this->read_u32(major_brand)) {
    return false;
  }
  ESP_LOGD(TAG, "MP4 major brand: 0x%08X", major_brand);
  // Skip rest of ftyp
  return this->skip_bytes(size - 12);  // 12 = header(8) + major_brand(4)
}

bool MP4Demuxer::parse_moov_box(uint32_t size) {
  uint64_t moov_end = ftell(this->file_) + size - 8;  // -8 for box header already read

  while (ftell(this->file_) < static_cast<long>(moov_end)) {
    uint32_t box_size, box_type;
    uint64_t box_start = ftell(this->file_);

    if (!this->read_box_header(box_size, box_type)) {
      return false;
    }

    switch (box_type) {
      case BOX_TYPE_TRAK: {
        TrackType track_type = TrackType::UNKNOWN;
        if (!this->parse_trak_box(box_size, track_type)) {
          ESP_LOGE(TAG, "Failed to parse trak box");
          return false;
        }
        break;
      }

      default:
        // Skip unknown boxes in moov
        fseek(this->file_, box_start + box_size, SEEK_SET);
        break;
    }
  }

  return true;
}

bool MP4Demuxer::parse_trak_box(uint32_t size, TrackType &track_type) {
  uint64_t trak_end = ftell(this->file_) + size - 8;

  while (ftell(this->file_) < static_cast<long>(trak_end)) {
    uint32_t box_size, box_type;
    uint64_t box_start = ftell(this->file_);

    if (!this->read_box_header(box_size, box_type)) {
      return false;
    }

    if (box_type == BOX_TYPE_MDIA) {
      if (!this->parse_mdia_box(box_size, track_type)) {
        return false;
      }
    } else {
      fseek(this->file_, box_start + box_size, SEEK_SET);
    }
  }

  return true;
}

bool MP4Demuxer::parse_mdia_box(uint32_t size, TrackType &track_type) {
  uint64_t mdia_end = ftell(this->file_) + size - 8;

  while (ftell(this->file_) < static_cast<long>(mdia_end)) {
    uint32_t box_size, box_type;
    uint64_t box_start = ftell(this->file_);

    if (!this->read_box_header(box_size, box_type)) {
      return false;
    }

    switch (box_type) {
      case BOX_TYPE_HDLR:
        if (!this->parse_hdlr_box(box_size, track_type)) {
          return false;
        }
        break;

      case BOX_TYPE_MINF: {
        // Parse minf to find stbl
        uint64_t minf_end = ftell(this->file_) + box_size - 8;
        while (ftell(this->file_) < static_cast<long>(minf_end)) {
          uint32_t inner_size, inner_type;
          uint64_t inner_start = ftell(this->file_);

          if (!this->read_box_header(inner_size, inner_type)) {
            return false;
          }

          if (inner_type == BOX_TYPE_STBL) {
            if (!this->parse_stbl_box(inner_size, track_type)) {
              return false;
            }
          } else {
            fseek(this->file_, inner_start + inner_size, SEEK_SET);
          }
        }
        break;
      }

      default:
        fseek(this->file_, box_start + box_size, SEEK_SET);
        break;
    }
  }

  return true;
}

bool MP4Demuxer::parse_hdlr_box(uint32_t size, TrackType &track_type) {
  // hdlr format:
  // version(1) + flags(3) + pre_defined(4) + handler_type(4) + ...
  this->skip_bytes(8);  // version, flags, pre_defined

  uint32_t handler_type;
  if (!this->read_u32(handler_type)) {
    return false;
  }

  if (handler_type == HANDLER_VIDEO) {
    track_type = TrackType::VIDEO;
    ESP_LOGD(TAG, "Found video track");
  } else if (handler_type == HANDLER_AUDIO) {
    track_type = TrackType::AUDIO;
    ESP_LOGD(TAG, "Found audio track");
  }

  // Skip rest of hdlr box
  return this->skip_bytes(size - 20);  // 20 = header(8) + fields_read(12)
}

bool MP4Demuxer::parse_stbl_box(uint32_t size, TrackType track_type) {
  if (track_type == TrackType::UNKNOWN) {
    return true;  // Skip unknown tracks
  }

  uint64_t stbl_end = ftell(this->file_) + size - 8;

  std::vector<uint32_t> sample_sizes;
  std::vector<uint64_t> chunk_offsets;
  std::vector<uint64_t> sample_offsets;
  std::vector<uint32_t> sample_durations;
  uint32_t timescale = 1000;  // Default if not found

  // First pass: collect all sample table data
  while (ftell(this->file_) < static_cast<long>(stbl_end)) {
    uint32_t box_size, box_type;
    uint64_t box_start = ftell(this->file_);

    if (!this->read_box_header(box_size, box_type)) {
      return false;
    }

    switch (box_type) {
      case BOX_TYPE_STSD:
        // Parse sample description to get codec info and audio metadata
        if (!this->parse_stsd_box(box_size, track_type)) {
          return false;
        }
        break;

      case BOX_TYPE_STSZ:
        if (!this->parse_stsz_box(box_size, sample_sizes)) {
          return false;
        }
        break;

      case BOX_TYPE_STCO:
        if (!this->parse_stco_box(box_size, chunk_offsets)) {
          return false;
        }
        break;

      case BOX_TYPE_CO64:
        if (!this->parse_co64_box(box_size, chunk_offsets)) {
          return false;
        }
        break;

      case BOX_TYPE_STSC:
        // Parse stsc after we have chunk_offsets and sample_sizes
        if (!chunk_offsets.empty() && !sample_sizes.empty()) {
          if (!this->parse_stsc_box(box_size, sample_offsets, chunk_offsets, sample_sizes)) {
            return false;
          }
        } else {
          fseek(this->file_, box_start + box_size, SEEK_SET);
        }
        break;

      case BOX_TYPE_STTS:
        if (!this->parse_stts_box(box_size, sample_durations, timescale)) {
          return false;
        }
        break;

      default:
        fseek(this->file_, box_start + box_size, SEEK_SET);
        break;
    }
  }

  // Second pass: if we didn't parse stsc yet, do it now
  if (sample_offsets.empty() && !chunk_offsets.empty() && !sample_sizes.empty()) {
    // Need to re-parse stbl to find stsc
    // This is a simplified implementation - in production code we'd cache the stsc offset
    ESP_LOGW(TAG, "stsc box needs second pass - this is inefficient");
  }

  // Store track information
  if (track_type == TrackType::VIDEO) {
    this->has_video_ = true;
    this->video_track_.sample_count = sample_sizes.size();
    this->video_track_.sample_sizes = std::move(sample_sizes);
    this->video_track_.sample_offsets = std::move(sample_offsets);
    this->video_track_.sample_durations = std::move(sample_durations);
    this->video_track_.timescale = timescale;

    // Calculate total duration
    uint64_t total_duration = 0;
    for (uint32_t dur : this->video_track_.sample_durations) {
      total_duration += dur;
    }
    this->video_track_.duration = total_duration;

  } else if (track_type == TrackType::AUDIO) {
    this->has_audio_ = true;
    this->audio_track_.sample_count = sample_sizes.size();
    this->audio_track_.sample_sizes = std::move(sample_sizes);
    this->audio_track_.sample_offsets = std::move(sample_offsets);
    this->audio_track_.sample_durations = std::move(sample_durations);
    this->audio_track_.timescale = timescale;

    // Calculate total duration
    uint64_t total_duration = 0;
    for (uint32_t dur : this->audio_track_.sample_durations) {
      total_duration += dur;
    }
    this->audio_track_.duration = total_duration;
  }

  return true;
}

// ========== Sample Table Parsers ==========

bool MP4Demuxer::parse_stsd_box(uint32_t size, TrackType track_type) {
  // stsd format:
  // version(1) + flags(3) + entry_count(4) + [sample_description_entries...]

  this->skip_bytes(4);  // version + flags

  uint32_t entry_count;
  if (!this->read_u32(entry_count)) {
    return false;
  }

  if (entry_count == 0) {
    ESP_LOGW(TAG, "stsd: no sample description entries");
    return true;
  }

  // Parse first entry (usually only one codec per track)
  uint32_t entry_size, entry_type;
  if (!this->read_box_header(entry_size, entry_type)) {
    return false;
  }

  ESP_LOGD(TAG, "stsd: codec type 0x%08X", entry_type);

  if (track_type == TrackType::VIDEO) {
    // Video sample description (VisualSampleEntry)
    if (entry_type == BOX_TYPE_AVC1) {
      // H.264/AVC1 format
      // Skip: reserved(6) + data_reference_index(2) + pre_defined(16) + width(2) + height(2) + ...
      this->skip_bytes(6 + 2 + 16);  // reserved + data_reference_index + pre_defined fields

      uint16_t width, height;
      if (!this->read_u16(width) || !this->read_u16(height)) {
        return false;
      }

      this->video_track_.width = width;
      this->video_track_.height = height;

      ESP_LOGD(TAG, "stsd: H.264 video %ux%u", width, height);
    }

  } else if (track_type == TrackType::AUDIO) {
    // Audio sample description (AudioSampleEntry)
    // Format: reserved(6) + data_reference_index(2) + version(2) + revision(2) + vendor(4) +
    //         channels(2) + sample_size(2) + pre_defined(2) + reserved(2) + sample_rate(4)

    this->skip_bytes(6 + 2 + 2 + 2 + 4);  // Skip to channels

    uint16_t channels, bits_per_sample;
    if (!this->read_u16(channels) || !this->read_u16(bits_per_sample)) {
      return false;
    }

    this->skip_bytes(2 + 2);  // pre_defined + reserved

    // Sample rate is stored as fixed-point 16.16 (upper 16 bits = integer part)
    uint32_t sample_rate_fixed;
    if (!this->read_u32(sample_rate_fixed)) {
      return false;
    }
    uint16_t sample_rate = sample_rate_fixed >> 16;

    this->audio_track_.channels = channels;
    this->audio_track_.bits_per_sample = bits_per_sample;
    this->audio_track_.sample_rate = sample_rate;

    if (entry_type == BOX_TYPE_FLAC) {
      ESP_LOGI(TAG, "stsd: FLAC audio %u Hz, %u channels, %u bits", sample_rate, channels, bits_per_sample);
    } else if (entry_type == BOX_TYPE_MP4A) {
      ESP_LOGI(TAG, "stsd: AAC audio %u Hz, %u channels, %u bits", sample_rate, channels, bits_per_sample);
    } else {
      ESP_LOGW(TAG, "stsd: Unknown audio codec 0x%08X (%u Hz, %u channels)", entry_type, sample_rate, channels);
    }
  }

  return true;
}

bool MP4Demuxer::parse_stsz_box(uint32_t size, std::vector<uint32_t> &sample_sizes) {
  // stsz format:
  // version(1) + flags(3) + sample_size(4) + sample_count(4) + [sizes...]

  this->skip_bytes(4);  // version + flags

  uint32_t uniform_size, sample_count;
  if (!this->read_u32(uniform_size) || !this->read_u32(sample_count)) {
    return false;
  }

  sample_sizes.resize(sample_count);

  if (uniform_size != 0) {
    // All samples have same size
    for (uint32_t i = 0; i < sample_count; i++) {
      sample_sizes[i] = uniform_size;
    }
  } else {
    // Each sample has individual size
    for (uint32_t i = 0; i < sample_count; i++) {
      if (!this->read_u32(sample_sizes[i])) {
        return false;
      }
    }
  }

  ESP_LOGD(TAG, "stsz: %u samples", sample_count);
  return true;
}

bool MP4Demuxer::parse_stco_box(uint32_t size, std::vector<uint64_t> &chunk_offsets) {
  // stco format:
  // version(1) + flags(3) + entry_count(4) + [offsets...]

  this->skip_bytes(4);  // version + flags

  uint32_t entry_count;
  if (!this->read_u32(entry_count)) {
    return false;
  }

  chunk_offsets.resize(entry_count);
  for (uint32_t i = 0; i < entry_count; i++) {
    uint32_t offset;
    if (!this->read_u32(offset)) {
      return false;
    }
    chunk_offsets[i] = offset;
  }

  ESP_LOGD(TAG, "stco: %u chunk offsets", entry_count);
  return true;
}

bool MP4Demuxer::parse_co64_box(uint32_t size, std::vector<uint64_t> &chunk_offsets) {
  // co64 format: same as stco but with 64-bit offsets
  this->skip_bytes(4);  // version + flags

  uint32_t entry_count;
  if (!this->read_u32(entry_count)) {
    return false;
  }

  chunk_offsets.resize(entry_count);
  for (uint32_t i = 0; i < entry_count; i++) {
    if (!this->read_u64(chunk_offsets[i])) {
      return false;
    }
  }

  ESP_LOGD(TAG, "co64: %u chunk offsets", entry_count);
  return true;
}

bool MP4Demuxer::parse_stsc_box(uint32_t size, std::vector<uint64_t> &sample_offsets,
                                const std::vector<uint64_t> &chunk_offsets, const std::vector<uint32_t> &sample_sizes) {
  // stsc format (sample-to-chunk):
  // version(1) + flags(3) + entry_count(4) + [first_chunk(4) + samples_per_chunk(4) + description(4)]...

  this->skip_bytes(4);  // version + flags

  uint32_t entry_count;
  if (!this->read_u32(entry_count)) {
    return false;
  }

  // Read stsc entries
  struct StscEntry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t description;
  };
  std::vector<StscEntry> entries(entry_count);

  for (uint32_t i = 0; i < entry_count; i++) {
    if (!this->read_u32(entries[i].first_chunk) || !this->read_u32(entries[i].samples_per_chunk) ||
        !this->read_u32(entries[i].description)) {
      return false;
    }
  }

  // Build sample offsets from chunk offsets + stsc mapping
  sample_offsets.clear();
  sample_offsets.reserve(sample_sizes.size());

  uint32_t sample_idx = 0;
  for (size_t chunk_idx = 0; chunk_idx < chunk_offsets.size(); chunk_idx++) {
    // Find which stsc entry applies to this chunk
    uint32_t samples_in_chunk = entries[0].samples_per_chunk;
    for (size_t i = 0; i < entries.size(); i++) {
      if (chunk_idx + 1 >= entries[i].first_chunk) {
        samples_in_chunk = entries[i].samples_per_chunk;
      } else {
        break;
      }
    }

    // Calculate offset for each sample in this chunk
    uint64_t offset = chunk_offsets[chunk_idx];
    for (uint32_t i = 0; i < samples_in_chunk && sample_idx < sample_sizes.size(); i++) {
      sample_offsets.push_back(offset);
      offset += sample_sizes[sample_idx];
      sample_idx++;
    }
  }

  ESP_LOGD(TAG, "stsc: mapped %zu samples from %zu chunks", sample_offsets.size(), chunk_offsets.size());
  return true;
}

bool MP4Demuxer::parse_stts_box(uint32_t size, std::vector<uint32_t> &sample_durations, uint32_t timescale) {
  // stts format (time-to-sample):
  // version(1) + flags(3) + entry_count(4) + [sample_count(4) + sample_delta(4)]...

  this->skip_bytes(4);  // version + flags

  uint32_t entry_count;
  if (!this->read_u32(entry_count)) {
    return false;
  }

  sample_durations.clear();

  for (uint32_t i = 0; i < entry_count; i++) {
    uint32_t sample_count, sample_delta;
    if (!this->read_u32(sample_count) || !this->read_u32(sample_delta)) {
      return false;
    }

    // Add sample_delta for each sample in this run
    for (uint32_t j = 0; j < sample_count; j++) {
      sample_durations.push_back(sample_delta);
    }
  }

  ESP_LOGD(TAG, "stts: %zu sample durations", sample_durations.size());
  return true;
}

}  // namespace video_player
}  // namespace esphome
