/**
 * @file mkv_demuxer.h
 * @brief Lightweight Matroska/MKV container parser for extracting H264 video and audio streams
 *
 * This demuxer parses MKV files to extract:
 * - H264 video frames with timing information
 * - Audio samples (AAC/MP3/FLAC) with timing information
 * - Track metadata (resolution, duration, sample rate)
 *
 * MKV/Matroska Container Structure (EBML-based):
 * - EBML Header: Format identification and metadata
 * - Segment: Main container
 *   - SeekHead: Index of top-level elements
 *   - Info: Duration, timecode scale
 *   - Tracks: Video/audio track information
 *   - Cluster: Media data with timecodes
 *
 * Reference: RFC 9559 (October 2024) - Matroska Media Container Format
 */

#pragma once

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "mp4_demuxer.h"  // Reuse VideoTrackInfo, AudioTrackInfo, Sample, AudioCodecType
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace video_player {

// EBML Element IDs (variable-length encoded)
constexpr uint32_t EBML_ID_EBML = 0x1A45DFA3;         // EBML header
constexpr uint32_t EBML_ID_SEGMENT = 0x18538067;      // Main segment
constexpr uint32_t EBML_ID_SEEKHEAD = 0x114D9B74;     // Seek index
constexpr uint32_t EBML_ID_INFO = 0x1549A966;         // Segment info
constexpr uint32_t EBML_ID_TRACKS = 0x1654AE6B;       // Track info
constexpr uint32_t EBML_ID_CLUSTER = 0x1F43B675;      // Media cluster
constexpr uint32_t EBML_ID_TIMECODE = 0xE7;           // Cluster timecode
constexpr uint32_t EBML_ID_SIMPLEBLOCK = 0xA3;        // Simple block (frame)
constexpr uint32_t EBML_ID_BLOCKGROUP = 0xA0;         // Block group
constexpr uint32_t EBML_ID_BLOCK = 0xA1;              // Block data
constexpr uint32_t EBML_ID_DURATION = 0x4489;         // Duration
constexpr uint32_t EBML_ID_TIMECODESCALE = 0x2AD7B1;  // Timecode scale

// Track element IDs
constexpr uint32_t EBML_ID_TRACKENTRY = 0xAE;    // Track entry
constexpr uint32_t EBML_ID_TRACKNUMBER = 0xD7;   // Track number
constexpr uint32_t EBML_ID_TRACKTYPE = 0x83;     // Track type
constexpr uint32_t EBML_ID_CODECID = 0x86;       // Codec ID string
constexpr uint32_t EBML_ID_VIDEO = 0xE0;         // Video settings
constexpr uint32_t EBML_ID_AUDIO = 0xE1;         // Audio settings
constexpr uint32_t EBML_ID_PIXELWIDTH = 0xB0;    // Video width
constexpr uint32_t EBML_ID_PIXELHEIGHT = 0xBA;   // Video height
constexpr uint32_t EBML_ID_SAMPLINGFREQ = 0xB5;  // Audio sample rate
constexpr uint32_t EBML_ID_CHANNELS = 0x9F;      // Audio channels
constexpr uint32_t EBML_ID_BITDEPTH = 0x6264;    // Audio bit depth

// Track types
constexpr uint8_t MKV_TRACK_TYPE_VIDEO = 0x01;
constexpr uint8_t MKV_TRACK_TYPE_AUDIO = 0x02;

/**
 * @brief EBML variable-length integer reader
 *
 * EBML uses variable-length encoding for element IDs and sizes.
 * First byte indicates length by leading zero bits.
 */
class EBMLReader {
 public:
  /**
   * @brief Read EBML variable-length element ID
   * @param file File pointer
   * @param id Output element ID
   * @return true if successful
   */
  static bool read_element_id(FILE *file, uint32_t &id);

  /**
   * @brief Read EBML variable-length size
   * @param file File pointer
   * @param size Output size
   * @return true if successful
   */
  static bool read_element_size(FILE *file, uint64_t &size);

  /**
   * @brief Read unsigned integer (big-endian)
   * @param file File pointer
   * @param value Output value
   * @param length Number of bytes to read (1-8)
   * @return true if successful
   */
  static bool read_uint(FILE *file, uint64_t &value, size_t length);

  /**
   * @brief Read float (IEEE 754, big-endian)
   * @param file File pointer
   * @param value Output value
   * @param length Number of bytes (4 or 8)
   * @return true if successful
   */
  static bool read_float(FILE *file, double &value, size_t length);

  /**
   * @brief Read string (UTF-8)
   * @param file File pointer
   * @param str Output string
   * @param length Number of bytes to read
   * @return true if successful
   */
  static bool read_string(FILE *file, std::string &str, size_t length);
};

/**
 * @brief MKV/Matroska demuxer class
 *
 * Parses MKV container files and provides sequential access to video and audio samples.
 * Uses EBML parser to navigate the hierarchical structure.
 */
class MKVDemuxer {
 public:
  MKVDemuxer();
  ~MKVDemuxer();

  /**
   * @brief Open and parse MKV file
   * @param file_path Path to MKV file
   * @return true if successful, false otherwise
   */
  bool open(const std::string &file_path);

  /**
   * @brief Close MKV file and release resources
   */
  void close();

  /**
   * @brief Check if demuxer is open and ready
   */
  bool is_open() const { return this->file_ != nullptr; }

  /**
   * @brief Get next video sample (H264 frame)
   * @param sample Output sample information
   * @param data Output buffer for frame data (allocated by caller)
   * @param max_size Maximum size of output buffer
   * @return true if sample retrieved, false if end of track or error
   */
  bool get_next_video_sample(Sample &sample, uint8_t *data, size_t max_size);

  /**
   * @brief Get next audio sample
   * @param sample Output sample information
   * @param data Output buffer for audio data (allocated by caller)
   * @param max_size Maximum size of output buffer
   * @return true if sample retrieved, false if end of track or error
   */
  bool get_next_audio_sample(Sample &sample, uint8_t *data, size_t max_size);

  /**
   * @brief Seek video track to specific timestamp
   * @param timestamp_ms Timestamp in milliseconds
   * @return true if seek successful
   */
  bool seek_video(uint64_t timestamp_ms);

  /**
   * @brief Seek audio track to specific timestamp
   * @param timestamp_ms Timestamp in milliseconds
   * @return true if seek successful
   */
  bool seek_audio(uint64_t timestamp_ms);

  /**
   * @brief Reset to beginning of file
   */
  void reset();

  /**
   * @brief Get video track information
   */
  const VideoTrackInfo *get_video_track() const { return this->has_video_ ? &this->video_track_ : nullptr; }

  /**
   * @brief Get audio track information
   */
  const AudioTrackInfo *get_audio_track() const { return this->has_audio_ ? &this->audio_track_ : nullptr; }

  /**
   * @brief Check if file has video track
   */
  bool has_video() const { return this->has_video_; }

  /**
   * @brief Check if file has audio track
   */
  bool has_audio() const { return this->has_audio_; }

  /**
   * @brief Get video duration in milliseconds
   */
  uint64_t get_video_duration_ms() const;

  /**
   * @brief Get audio duration in milliseconds
   */
  uint64_t get_audio_duration_ms() const;

 protected:
  // EBML parsing helpers
  bool parse_ebml_header_();
  bool parse_segment_();
  bool parse_info_();
  bool parse_tracks_();
  bool parse_track_entry_();
  bool parse_cluster_();
  bool skip_element_(uint64_t size);

  // Codec detection
  AudioCodecType detect_audio_codec_(const std::string &codec_id);

  // File I/O
  FILE *file_{nullptr};
  std::string file_path_;
  uint64_t file_size_{0};
  uint64_t segment_offset_{0};  // Offset where Segment starts

  // Segment info
  double duration_seconds_{0.0};      // Segment duration
  uint64_t timecode_scale_{1000000};  // Timecode scale in nanoseconds (default: 1ms)

  // Track information
  bool has_video_{false};
  bool has_audio_{false};
  VideoTrackInfo video_track_;
  AudioTrackInfo audio_track_;
  uint8_t video_track_number_{0};
  uint8_t audio_track_number_{0};

  // Current cluster state
  uint64_t current_cluster_timecode_{0};  // Base timecode for current cluster
  uint64_t first_cluster_offset_{0};      // File offset of first cluster
  uint64_t current_cluster_offset_{0};    // File offset of current cluster

  // Playback state
  uint32_t current_video_sample_{0};
  uint32_t current_audio_sample_{0};
};

}  // namespace video_player
}  // namespace esphome
