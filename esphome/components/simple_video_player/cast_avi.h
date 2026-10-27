#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::simple_video_player {

/// The bytes of a screencast's AVI (OpenDML, laid out as ffmpeg's avienc writes it, read back by
/// AVIParser): the first RIFF 'AVI ' with the headers, then a RIFF 'AVIX' each up to SEGMENT_BYTES.
/// Every movi list ends with the standard indexes of its chunks (ix00 the pictures, ix01 the sound);
/// the streams' super indexes (indx, in their headers) list those. Pictures '00dc' (an empty one: the
/// picture before once more), sound '01wb' (PCM). Writing the bytes is the caller's.
///
/// The index memory: a part for each stream's ix## (its header, then 8 bytes per chunk), and the
/// super index entries at its end. The ix## are written from it as they are; the header is built
/// into it once they are (index_free()).
class CastAvi {
 public:
  static constexpr size_t MAX_SEGMENTS = 256;  // super index entries per stream (ffmpeg: AVI_MASTER_INDEX_SIZE)
  static constexpr uint64_t SEGMENT_BYTES = 1000ull * 1000 * 1000;  // a RIFF at most (ffmpeg: 1 GB)
  static constexpr size_t SEGMENT_START = 24;                       // RIFF size 'AVIX' LIST size 'movi'
  static constexpr size_t CHUNK_HEADER = 8;
  static constexpr size_t IX_HEADER = 32;  // an ix##'s chunk header and its fields
  static constexpr size_t SUPER_BYTES = 2 * MAX_SEGMENTS * 16;

  /// A run of chunks written in one go: a picture (jpeg_len bytes, 0 for an empty one), the
  /// further empty pictures, then the sound (audio_len bytes, 0 for none).
  struct Batch {
    uint32_t jpeg_len;
    uint16_t pictures;  // at least 1
    uint32_t audio_len;
    bool key{true};
    uint32_t audio_chunks() const { return this->audio_len != 0 ? 1 : 0; }
    /// The bytes of its chunks (headers, data, padding).
    size_t bytes() const;
  };

  /// Writes the chunk headers (and padding) of a batch: the picture's at pic - 8, the rest after
  /// its data (at pic + jpeg_len); the sound is copied in by the caller to the returned position.
  /// Returns where the sound's data goes (nullptr without sound).
  static uint8_t *lay_out(uint8_t *pic, const Batch &batch);

  /// sample_rate 0: no sound stream. index: index_bytes long, its end SUPER_BYTES the super entries.
  bool begin(uint16_t width, uint16_t height, uint32_t fps, uint32_t sample_rate, uint8_t channels,
             uint32_t max_chunk, uint8_t *index, size_t index_bytes, bool h264 = false);
  size_t header_size() const { return this->header_size_; }
  /// The first RIFF's header, through its movi list's 'movi' fourcc, with everything known so far
  /// (0 for a size not known yet: read to the end). For the network stream: before the first chunk.
  void build_header(uint8_t *out) const;

  /// Whether the batch still fits into this segment (bytes and index entries).
  bool fits(const Batch &batch) const;
  /// The batch written at the end of the file: its chunks into the index.
  void add(const Batch &batch);
  uint64_t file_size() const { return this->file_pos_; }
  uint32_t pictures() const { return this->total_pictures_; }
  bool segments_full() const { return this->segment_ + 1 >= MAX_SEGMENTS; }

  // Ending a segment: its ix00 (and ix01) at the end of the file, then the header (and an AVIX
  // segment's start) again with the sizes, then (unless it was the last) the next segment's start.
  /// The index of a stream (0 pictures, 1 sound) as written at the end of the file now; its bytes in
  /// the index memory.
  const uint8_t *close_index(uint8_t stream, size_t *len);
  /// After the indexes: the segment's sizes. The header (header_size()) and, for an AVIX segment, its
  /// start (SEGMENT_START, at segment_start()) are built into out (index_free() bytes).
  void close_sizes();
  uint64_t segment_start() const { return this->seg_start_; }
  bool first_segment() const { return this->segment_ == 0; }
  uint8_t *index_free() const { return this->index_; }
  void build_segment_patch(uint8_t *out) const;
  /// The next segment's start (SEGMENT_START bytes, at the end of the file) into out.
  void next_segment(uint8_t *out);

 protected:
  struct Super {
    uint64_t pos;
    uint32_t size;
    uint32_t count;
  };
  uint8_t *ix_(uint8_t stream) const { return this->index_ + stream * this->ix_bytes_; }
  Super *super_(uint8_t stream) const {
    return reinterpret_cast<Super *>(this->index_ + 2 * this->ix_bytes_) + stream * MAX_SEGMENTS;
  }
  size_t ix_capacity_() const { return (this->ix_bytes_ - IX_HEADER) / 8; }
  void put_entry_(uint8_t stream, uint64_t chunk_pos, uint32_t size);

  uint8_t *index_{nullptr};
  size_t ix_bytes_{0};  // each stream's part

  uint16_t width_{0};
  uint16_t height_{0};
  bool h264_{false};
  uint32_t fps_{25};
  uint32_t rate_{0};
  uint8_t channels_{0};
  uint32_t max_chunk_{0};
  size_t header_size_{0};
  // Positions in the header of what is filled in later.
  size_t strl_audio_{0};
  size_t odml_{0};

  uint64_t file_pos_{0};
  uint64_t seg_start_{0};
  uint64_t movi_pos_{0};  // this segment's 'movi' fourcc
  uint32_t segment_{0};
  uint32_t entries_[2]{};
  uint32_t total_pictures_{0};
  uint32_t first_riff_pictures_{0};
  uint64_t total_audio_bytes_{0};
  uint32_t first_riff_bytes_{0};  // the first RIFF's size field, 0 until it is closed
  uint32_t first_movi_bytes_{0};
  uint32_t seg_riff_bytes_{0};  // the segment just closed
  uint32_t seg_movi_bytes_{0};
  uint32_t closed_{0};  // segments in the super indexes
};

}  // namespace esphome::simple_video_player
