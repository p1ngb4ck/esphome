/**
 * @file avi_parser.h
 * @brief Lightweight AVI container parser for MJPEG video + audio
 *
 * Supports parsing AVI RIFF container format to extract:
 * - MJPEG video frames
 * - Audio streams (MP3, PCM, etc.)
 * - Frame timing information
 *
 * Design goals:
 * - Minimal memory footprint (stream-based parsing)
 * - Works with BufferedFileReader for optimal SD card performance
 * - Supports seeking to specific frames
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "buffered_file_reader.h"

namespace esphome {
namespace simple_video_player {

/**
 * @brief AVI stream types
 */
enum class AVIStreamType : uint8_t {
  VIDEO = 0,
  AUDIO = 1,
  UNKNOWN = 255,
};

/**
 * @brief AVI video codec types
 */
enum class AVIVideoCodec : uint32_t {
  MJPEG = 0x47504A4D,   // 'MJPG'
  MJPEGA = 0x4147504A,  // 'MJPA' (alternative)
  UNKNOWN = 0,
};

/**
 * @brief AVI audio codec types
 */
enum class AVIAudioCodec : uint16_t {
  PCM = 0x0001,
  MP3 = 0x0055,
  FLAC = 0xF1AC,
  UNKNOWN = 0,
};

/**
 * @brief AVI frame information
 */
struct AVIFrame {
  AVIStreamType stream_type;  // Video or audio
  uint64_t offset;            // Byte offset in file
  uint32_t size;              // Frame data size in bytes
  uint32_t timestamp_ms;      // Timestamp in milliseconds
  bool keyframe;              // Is this a keyframe/I-frame
};

/**
 * @brief AVI stream header information
 */
struct AVIStreamInfo {
  AVIStreamType type;
  uint32_t codec;            // Video: AVIVideoCodec, Audio: AVIAudioCodec
  uint32_t width;            // Video only
  uint32_t height;           // Video only
  uint32_t fps_num;          // Frame rate numerator
  uint32_t fps_den;          // Frame rate denominator
  uint32_t sample_rate;      // Audio only
  uint16_t channels;         // Audio only
  uint16_t bits_per_sample;  // Audio only
  uint32_t total_frames;     // Total number of frames
  // FLAC only: the native stream header ("fLaC", STREAMINFO block header, STREAMINFO) built from the
  // STREAMINFO an AVI keeps in its WAVEFORMATEX extra data; the chunks are bare frames.
  uint8_t stream_header[42];
  uint8_t stream_header_len;
};

/**
 * @brief Lightweight AVI container parser
 *
 * Parses AVI RIFF structure to extract video/audio frames.
 * Uses stream-based parsing to minimize memory usage.
 */
class AVIParser {
 public:
  AVIParser();
  ~AVIParser();

  /**
   * @brief Open and parse AVI file
   * @param reader Buffered file reader
   * @return true if AVI file is valid and opened successfully
   */
  bool open(BufferedFileReader *reader);

  /**
   * @brief Close AVI file
   */
  void close();

  /**
   * @brief Check if AVI file is open
   */
  bool is_open() const { return this->reader_ != nullptr; }

  /**
   * @brief Get video stream information
   * @return Video stream info, or nullptr if no video stream
   */
  const AVIStreamInfo *get_video_info() const;

  /**
   * @brief Get audio stream information
   * @return Audio stream info, or nullptr if no audio stream
   */
  const AVIStreamInfo *get_audio_info() const;

  /**
   * @brief Read next frame (video or audio)
   * @param frame Output: frame information
   * @param buffer Output: buffer to store frame data
   * @param buffer_size Size of output buffer
   * @return Number of bytes read, 0 on EOF, -1 on error
   */
  int read_next_frame(AVIFrame &frame, uint8_t *buffer, size_t buffer_size);

  /// Reads the idx1 index (offsets of the video frames) into PSRAM. Reads from the end of the file
  /// and needs a blocking reader; seek_to_frame() afterwards. On failure the read position is restored.
  /// Tried once per file.
  bool load_index(uint8_t *scratch, size_t scratch_size);
  bool has_index() const { return this->index_ != nullptr; }

  /// Positions the reader at a video frame (0-based) through the index; clamped to the last frame.
  bool seek_to_frame(uint32_t frame_number);

  /// Video frames read so far: the next frame read has this number.
  uint32_t current_frame() const { return this->current_frame_; }

  /**
   * @brief Seek to beginning of file (rewind)
   * @return true on success
   */
  bool rewind();

  /// Reader position where the frame data ends (UINT64_MAX while unknown: a live stream).
  uint64_t data_end() const {
    return this->movi_size_ == UINT64_MAX ? UINT64_MAX : this->movi_offset_ + this->movi_size_;
  }

 protected:
  /**
   * @brief Parse AVI file headers
   * @return true if valid AVI file
   */
  bool parse_headers_();

  /**
   * @brief Read FOURCC code (4-character code)
   */
  bool read_fourcc_(uint32_t &fourcc);

  /**
   * @brief Read 32-bit little-endian value
   */
  bool read_uint32_(uint32_t &value);

  /**
   * @brief Read 16-bit little-endian value
   */
  bool read_uint16_(uint16_t &value);

  /**
   * @brief Skip bytes in stream
   */
  bool skip_bytes_(size_t count);

  /**
   * @brief Parse stream header (strh/strf chunks)
   */
  bool parse_stream_header_();

  // File reader
  BufferedFileReader *reader_{nullptr};

  // Stream information
  AVIStreamInfo video_info_{};
  AVIStreamInfo audio_info_{};
  bool has_video_{false};
  bool has_audio_{false};

  // Parsing state
  uint64_t movi_offset_{0};     // Offset to 'movi' chunk (frame data)
  uint64_t movi_size_{0};       // Size of 'movi' chunk
  uint64_t current_offset_{0};  // Current read position in movi chunk
  uint32_t current_frame_{0};   // Current frame number

  bool read_index_(uint8_t *scratch, size_t scratch_size);
  void release_index_();
  uint32_t *index_{nullptr};  // chunk offset of each video frame (PSRAM)
  uint32_t index_count_{0};
  uint64_t index_base_{0};  // what the offsets count from
  bool index_tried_{false};
};

}  // namespace simple_video_player
}  // namespace esphome
