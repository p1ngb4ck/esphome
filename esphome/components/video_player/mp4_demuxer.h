/**
 * @file mp4_demuxer.h
 * @brief Lightweight MP4 container parser for extracting H264 video and audio streams
 *
 * This demuxer parses MP4 files to extract:
 * - H264 video frames with timing information
 * - Audio samples (AAC/MP3) with timing information
 * - Track metadata (resolution, duration, sample rate)
 *
 * MP4 Container Structure:
 * - ftyp: File type identification
 * - moov: Movie metadata (track info, sample tables)
 *   - mvhd: Movie header (duration, timescale)
 *   - trak: Video/audio tracks
 *     - stbl: Sample table (frame offsets, sizes, timing)
 * - mdat: Media data (actual H264 + audio bytes)
 */

#pragma once

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif

namespace esphome {
namespace video_player {

// MP4 box types (four-character codes)
constexpr uint32_t BOX_TYPE_FTYP = 0x66747970;  // 'ftyp'
constexpr uint32_t BOX_TYPE_MOOV = 0x6D6F6F76;  // 'moov'
constexpr uint32_t BOX_TYPE_MVHD = 0x6D766864;  // 'mvhd'
constexpr uint32_t BOX_TYPE_TRAK = 0x7472616B;  // 'trak'
constexpr uint32_t BOX_TYPE_TKHD = 0x746B6864;  // 'tkhd'
constexpr uint32_t BOX_TYPE_MDIA = 0x6D646961;  // 'mdia'
constexpr uint32_t BOX_TYPE_MDHD = 0x6D646864;  // 'mdhd'
constexpr uint32_t BOX_TYPE_HDLR = 0x68646C72;  // 'hdlr'
constexpr uint32_t BOX_TYPE_MINF = 0x6D696E66;  // 'minf'
constexpr uint32_t BOX_TYPE_STBL = 0x7374626C;  // 'stbl'
constexpr uint32_t BOX_TYPE_STSD = 0x73747364;  // 'stsd'
constexpr uint32_t BOX_TYPE_STTS = 0x73747473;  // 'stts' - time-to-sample
constexpr uint32_t BOX_TYPE_STSC = 0x73747363;  // 'stsc' - sample-to-chunk
constexpr uint32_t BOX_TYPE_STSZ = 0x7374737A;  // 'stsz' - sample sizes
constexpr uint32_t BOX_TYPE_STCO = 0x7374636F;  // 'stco' - chunk offsets (32-bit)
constexpr uint32_t BOX_TYPE_CO64 = 0x636F3634;  // 'co64' - chunk offsets (64-bit)
constexpr uint32_t BOX_TYPE_MDAT = 0x6D646174;  // 'mdat'
constexpr uint32_t BOX_TYPE_AVC1 = 0x61766331;  // 'avc1' - H264
constexpr uint32_t BOX_TYPE_MP4A = 0x6D703461;  // 'mp4a' - AAC audio
constexpr uint32_t BOX_TYPE_MP3 = 0x2E6D7033;   // '.mp3' - MP3 audio
constexpr uint32_t BOX_TYPE_FLAC = 0x664C6143;  // 'fLaC' - FLAC audio

// Handler types
constexpr uint32_t HANDLER_VIDEO = 0x76696465;  // 'vide'
constexpr uint32_t HANDLER_AUDIO = 0x736F756E;  // 'soun'

/**
 * @brief Track type enumeration
 */
enum class TrackType {
  UNKNOWN,
  VIDEO,
  AUDIO,
};

/**
 * @brief Audio codec type enumeration
 */
enum class AudioCodecType : uint8_t {
  UNKNOWN = 0,
  AAC,
  MP3,
  FLAC,
};

/**
 * @brief Video track information
 */
struct VideoTrackInfo {
  uint32_t track_id{0};
  uint32_t timescale{0};                   // Time units per second
  uint64_t duration{0};                    // Duration in timescale units
  uint16_t width{0};                       // Video width in pixels
  uint16_t height{0};                      // Video height in pixels
  uint32_t sample_count{0};                // Total number of frames
  std::vector<uint32_t> sample_sizes;      // Size of each frame in bytes
  std::vector<uint64_t> sample_offsets;    // File offset of each frame
  std::vector<uint32_t> sample_durations;  // Duration of each frame in timescale units
  uint8_t nalu_length_size{4};             // Size of NALU length field (usually 4 bytes)
  std::vector<uint8_t> sps_data;           // H264 SPS (Sequence Parameter Set)
  std::vector<uint8_t> pps_data;           // H264 PPS (Picture Parameter Set)
};

/**
 * @brief Audio track information
 */
struct AudioTrackInfo {
  uint32_t track_id{0};
  uint32_t timescale{0};                               // Time units per second
  uint64_t duration{0};                                // Duration in timescale units
  uint16_t sample_rate{0};                             // Audio sample rate (Hz)
  uint16_t channels{0};                                // Number of audio channels
  uint16_t bits_per_sample{0};                         // Bits per sample
  AudioCodecType codec_type{AudioCodecType::UNKNOWN};  // Detected audio codec
  uint32_t sample_count{0};                            // Total number of audio samples
  std::vector<uint32_t> sample_sizes;                  // Size of each audio sample
  std::vector<uint64_t> sample_offsets;                // File offset of each sample
  std::vector<uint32_t> sample_durations;              // Duration of each sample in timescale units
};

/**
 * @brief Sample (frame/audio chunk) data
 */
struct Sample {
  uint64_t offset{0};        // File offset
  uint32_t size{0};          // Size in bytes
  uint64_t timestamp_ms{0};  // Presentation timestamp in milliseconds
  uint32_t duration_ms{0};   // Duration in milliseconds
  bool is_keyframe{false};   // True for video keyframes (IDR frames)
};

/**
 * @brief MP4 demuxer class
 *
 * Parses MP4 container files and provides sequential access to video and audio samples.
 */
class MP4Demuxer {
 public:
  MP4Demuxer();
  ~MP4Demuxer();

  /**
   * @brief Open and parse MP4 file
   * @param file_path Path to MP4 file
   * @return true if successful, false otherwise
   */
  bool open(const std::string &file_path);

  /**
   * @brief Close MP4 file and release resources
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
   * @brief Set readahead buffer size (must be called before open())
   * @param size Buffer size in bytes (default: 4MB)
   */
  void set_readahead_buffer_size(size_t size) { this->readahead_buffer_capacity_ = size; }

  /**
   * @brief Get video duration in milliseconds
   */
  uint64_t get_video_duration_ms() const {
    if (!this->has_video_ || this->video_track_.timescale == 0)
      return 0;
    return (this->video_track_.duration * 1000) / this->video_track_.timescale;
  }

  /**
   * @brief Get audio duration in milliseconds
   */
  uint64_t get_audio_duration_ms() const {
    if (!this->has_audio_ || this->audio_track_.timescale == 0)
      return 0;
    return (this->audio_track_.duration * 1000) / this->audio_track_.timescale;
  }

  /**
   * @brief RTOS task function for async buffer refill (public for wrapper access)
   * @param param Pointer to MP4Demuxer instance
   */
  static void refill_task_func_(void *param);

 protected:
  // Box parsing helpers
  bool read_u32(uint32_t &value);
  bool read_u16(uint16_t &value);
  bool read_u8(uint8_t &value);
  bool read_u64(uint64_t &value);
  bool skip_bytes(size_t count);
  bool read_box_header(uint32_t &size, uint32_t &type);

  // High-level box parsers
  bool parse_ftyp_box(uint32_t size);
  bool parse_moov_box(uint32_t size);
  bool parse_trak_box(uint32_t size, TrackType &track_type);
  bool parse_mdia_box(uint32_t size, TrackType &track_type);
  bool parse_hdlr_box(uint32_t size, TrackType &track_type);
  bool parse_stbl_box(uint32_t size, TrackType track_type);

  // Sample table parsers
  bool parse_stsd_box(uint32_t size, TrackType track_type);
  bool parse_stsz_box(uint32_t size, std::vector<uint32_t> &sample_sizes);
  bool parse_stco_box(uint32_t size, std::vector<uint64_t> &chunk_offsets);
  bool parse_co64_box(uint32_t size, std::vector<uint64_t> &chunk_offsets);
  bool parse_stsc_box(uint32_t size, std::vector<uint64_t> &sample_offsets, const std::vector<uint64_t> &chunk_offsets,
                      const std::vector<uint32_t> &sample_sizes);
  bool parse_stts_box(uint32_t size, std::vector<uint32_t> &sample_durations, uint32_t timescale);

  // File I/O
  FILE *file_{nullptr};
  std::string file_path_;
  uint64_t file_size_{0};
  uint64_t mdat_offset_{0};  // Offset where mdat box starts

  // Readahead buffer (PSRAM) to minimize USB seek/read latency - triple-buffered for async refill
  struct ReadaheadBuffer {
    std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> data;  // Allocated in PSRAM
    uint64_t start_offset{0};                                  // File offset where buffer starts
    size_t valid_size{0};                                      // How much of the buffer contains valid data
    bool is_ready{false};                                      // True when buffer has been filled and is ready to use
  };
  ReadaheadBuffer buffers_[3];    // Triple buffer (one active, two for prefetch/refill)
  uint8_t active_buffer_idx_{0};  // Index of buffer currently being read from (0-2)
  size_t readahead_buffer_capacity_{2 * 1024 * 1024 + 512 * 1024};  // 2.5MB per buffer (7.5MB total)

  // Async buffer refill task
  TaskHandle_t refill_task_handle_{nullptr};
  SemaphoreHandle_t buffer_mutex_{nullptr};      // Protects buffer swap operations
  SemaphoreHandle_t refill_semaphore_{nullptr};  // Signals refill task to start work
  volatile bool refill_task_running_{false};
  volatile bool stop_refill_task_flag_{false};
  volatile uint64_t next_refill_offset_{0};  // Offset to refill for next buffer

  void start_refill_task_();
  void stop_refill_task_();
  bool try_swap_buffers_(uint64_t target_offset);  // Try to swap to background buffer if it has the data we need

  // Track information
  bool has_video_{false};
  bool has_audio_{false};
  VideoTrackInfo video_track_;
  AudioTrackInfo audio_track_;

  // Playback state
  uint32_t current_video_sample_{0};
  uint32_t current_audio_sample_{0};
};

}  // namespace video_player
}  // namespace esphome
