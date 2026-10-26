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

  /// The file's title (LIST INFO, INAM), "" when it has none.
  const char *get_title() const { return this->title_; }

  /**
   * @brief Get audio stream information
   * @return Audio stream info, or nullptr if no audio stream
   */
  const AVIStreamInfo *get_audio_info() const;

  /// An empty video chunk: the previous picture once more (ffmpeg writes one for every jump in the
  /// picture timestamps, write_skip_frames in libavformat/avienc.c).
  static constexpr int REPEAT_FRAME = -3;

  /**
   * @brief Read next frame (video or audio)
   * @param frame Output: frame information
   * @param buffer Output: buffer to store frame data
   * @param buffer_size Size of output buffer
   * @return Number of bytes read, 0 on EOF, -1 on error, REPEAT_FRAME
   *
   * Damage is passed over: a chunk header that cannot be one, a video frame larger than the
   * buffer or a failed read continues at the next intact frame (through the index when loaded,
   * else at the next chunk header found).
   */
  int read_next_frame(AVIFrame &frame, uint8_t *buffer, size_t buffer_size);

  /// The player's memory for the index of every file (kept by the player, never freed here).
  void set_index_memory(uint32_t *memory, size_t bytes) {
    if (memory != this->index_memory_)
      this->release_index_();
    this->index_memory_ = memory;
    this->index_max_bytes_ = memory != nullptr ? bytes : 0;
  }

  /// Reads the index of the video frames into the index memory: the OpenDML one (indx, every RIFF of a
  /// file over 1 GB), else idx1 (the first RIFF only). Needs a blocking reader; the read position is
  /// restored. Tried once per file.
  bool load_index(uint8_t *scratch, size_t scratch_size);
  bool has_index() const { return this->index_ != nullptr; }

  /// Positions the reader at a video frame (0-based) through the index; clamped to the last frame.
  /// An index too large for its memory keeps every n-th frame: then the frame at or before it.
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
  bool plausible_chunk(const uint8_t *hdr, uint64_t at) const;

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
  /// A LIST INFO's chunks (size: after its type): INAM kept, the rest skipped.
  void parse_info_list_(uint32_t size);

  static constexpr size_t TITLE_MAX = 128;
  char title_[TITLE_MAX]{};

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
  bool read_odml_index_(uint8_t *scratch, size_t scratch_size);
  /// Keeps a video frame's chunk offset (from its segment's base) if the stride takes it, halving
  /// the index when full.
  void add_index_entry_(uint32_t video_frame, uint32_t offset, uint32_t capacity);
  /// File position of index entry k's chunk header, and the segment it lies in.
  uint64_t entry_pos_(uint32_t k) const;
  uint32_t segment_of_(uint32_t k) const;
  /// OpenDML: past the end of a movi list, the reader into the next one (in the next RIFF 'AVIX').
  bool next_movi_();
  void release_index_();
  /// Whether these 8 bytes can be the header of a chunk inside the movi list.
  bool plausible_header_(const uint8_t *hdr, size_t max_frame) const;
  /// After damage: the reader at the next intact video frame (index), else the next plausible
  /// chunk header read into hdr. False when there is none.
  bool resync_(uint8_t *hdr, size_t max_frame, bool &have_header);
  uint32_t *index_memory_{nullptr};
  size_t index_max_bytes_{0};
  // A movi list of the index: the first RIFF's, and one per RIFF 'AVIX' (OpenDML).
  struct Segment {
    uint64_t base;        // position of its 'movi' fourcc: what its entries count from
    uint32_t movi_size;   // its frame data
    uint32_t first_frame;
  };
  Segment *segments_{nullptr};  // at the start of index_memory_
  uint32_t segment_count_{0};
  uint32_t *index_{nullptr};  // chunk offset of every index_stride_-th video frame (index_memory_)
  uint32_t index_count_{0};
  uint32_t index_stride_{1};
  uint32_t recoveries_{0};  // in a row, without an intact frame between
  uint32_t damage_logged_{0};
  bool index_tried_{false};
  uint64_t first_movi_offset_{0};  // the first RIFF's movi list
  uint64_t first_movi_size_{0};
  uint64_t super_index_pos_{0};  // the video stream's indx (OpenDML), 0 if none
  uint32_t super_index_size_{0};
};

}  // namespace simple_video_player
}  // namespace esphome
