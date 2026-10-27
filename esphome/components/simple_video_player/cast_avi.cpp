#include "cast_avi.h"

#include <cstring>

namespace esphome::simple_video_player {

namespace {

void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
void put64(uint8_t *p, uint64_t v) {
  put32(p, static_cast<uint32_t>(v));
  put32(p + 4, static_cast<uint32_t>(v >> 32));
}
void fourcc(uint8_t *p, const char *cc) { std::memcpy(p, cc, 4); }

// Header layout (bytes): RIFF 'AVI ' 12, LIST 'hdrl' 12, avih 8 + 56, then per stream LIST 'strl' 12,
// strh 8 + 56, strf 8 + 40 (picture) or 8 + 16 (PCM), indx 8 + 24 + 16 per super entry; LIST 'odml' 12 with
// dmlh 8 + 248; LIST 'movi' 12.
constexpr size_t AVIH = 56;
constexpr size_t STRH = 56;
constexpr size_t STRF_VIDEO = 40;
constexpr size_t STRF_AUDIO = 16;
constexpr size_t INDX = 24 + 16 * CastAvi::MAX_SEGMENTS;
constexpr size_t DMLH = 248;
constexpr size_t STRL_VIDEO = 12 + 8 + STRH + 8 + STRF_VIDEO + 8 + INDX;
constexpr size_t STRL_AUDIO = 12 + 8 + STRH + 8 + STRF_AUDIO + 8 + INDX;
constexpr size_t ODML = 12 + 8 + DMLH;
constexpr size_t HDRL_START = 12;
constexpr size_t AVIH_DATA = HDRL_START + 12 + 8;
constexpr size_t STRL_VIDEO_START = AVIH_DATA + AVIH;

constexpr uint32_t AVIF_ISINTERLEAVED = 0x100;
constexpr uint32_t AVIF_TRUSTCKTYPE = 0x800;
constexpr uint32_t DELTA_FRAME = 0x80000000u;

}  // namespace

size_t CastAvi::Batch::bytes() const {
  size_t n = CHUNK_HEADER + this->jpeg_len + (this->jpeg_len & 1) + CHUNK_HEADER * (this->pictures - 1);
  if (this->audio_len != 0)
    n += CHUNK_HEADER + this->audio_len + (this->audio_len & 1);
  return n;
}

uint8_t *CastAvi::lay_out(uint8_t *pic, const Batch &batch) {
  fourcc(pic - CHUNK_HEADER, "00dc");
  put32(pic - CHUNK_HEADER + 4, batch.jpeg_len);
  uint8_t *p = pic + batch.jpeg_len;
  if (batch.jpeg_len & 1)
    *p++ = 0;
  for (uint16_t i = 1; i < batch.pictures; i++) {
    fourcc(p, "00dc");
    put32(p + 4, 0);
    p += CHUNK_HEADER;
  }
  if (batch.audio_len == 0)
    return nullptr;
  fourcc(p, "01wb");
  put32(p + 4, batch.audio_len);
  if (batch.audio_len & 1)
    p[CHUNK_HEADER + batch.audio_len] = 0;
  return p + CHUNK_HEADER;
}

bool CastAvi::begin(uint16_t width, uint16_t height, uint32_t fps, uint32_t sample_rate, uint8_t channels,
                    uint32_t max_chunk, uint8_t *index, size_t index_bytes, bool h264) {
  if (index == nullptr || fps == 0 || index_bytes < SUPER_BYTES + 2 * (IX_HEADER + 8 * 256))
    return false;
  this->h264_ = h264;
  this->index_ = index;
  this->ix_bytes_ = ((index_bytes - SUPER_BYTES) / 2) & ~static_cast<size_t>(127);
  this->width_ = width;
  this->height_ = height;
  this->fps_ = fps;
  this->rate_ = channels != 0 ? sample_rate : 0;
  this->channels_ = this->rate_ != 0 ? channels : 0;
  this->max_chunk_ = max_chunk;
  this->strl_audio_ = STRL_VIDEO_START + STRL_VIDEO;
  this->odml_ = this->strl_audio_ + (this->rate_ != 0 ? STRL_AUDIO : 0);
  this->header_size_ = this->odml_ + ODML + 12;
  // A header and a segment's start are built into the first stream's part once its ix## is written.
  if (this->header_size_ + SEGMENT_START + 8 > this->ix_bytes_)
    return false;
  this->file_pos_ = this->header_size_;
  this->seg_start_ = 0;
  this->movi_pos_ = this->header_size_ - 4;
  this->segment_ = 0;
  this->entries_[0] = this->entries_[1] = 0;
  this->total_pictures_ = 0;
  this->first_riff_pictures_ = 0;
  this->total_audio_bytes_ = 0;
  this->first_riff_bytes_ = 0;
  this->first_movi_bytes_ = 0;
  this->seg_riff_bytes_ = 0;
  this->seg_movi_bytes_ = 0;
  this->closed_ = 0;
  std::memset(this->super_(0), 0, SUPER_BYTES);
  return true;
}

void CastAvi::build_header(uint8_t *out) const {
  std::memset(out, 0, this->header_size_);
  const bool audio = this->rate_ != 0;
  const uint32_t closed = this->closed_;
  const uint16_t block = static_cast<uint16_t>(this->channels_ * 2);

  fourcc(out, "RIFF");
  put32(out + 4, this->first_riff_bytes_);
  fourcc(out + 8, "AVI ");
  fourcc(out + HDRL_START, "LIST");
  put32(out + HDRL_START + 4, static_cast<uint32_t>(this->odml_ + ODML - HDRL_START - 8));
  fourcc(out + HDRL_START + 8, "hdrl");

  uint8_t *p = out + AVIH_DATA - 8;
  fourcc(p, "avih");
  put32(p + 4, AVIH);
  p += 8;
  put32(p + 0, 1000000 / this->fps_);
  put32(p + 12, AVIF_ISINTERLEAVED | AVIF_TRUSTCKTYPE);
  put32(p + 16, this->first_riff_bytes_ != 0 ? this->first_riff_pictures_ : this->total_pictures_);
  put32(p + 24, audio ? 2 : 1);
  put32(p + 28, this->max_chunk_);
  put32(p + 32, this->width_);
  put32(p + 36, this->height_);

  auto strl = [&](uint8_t *s, bool video) {
    const size_t size = video ? STRL_VIDEO : STRL_AUDIO;
    fourcc(s, "LIST");
    put32(s + 4, static_cast<uint32_t>(size - 8));
    fourcc(s + 8, "strl");
    uint8_t *h = s + 12;
    fourcc(h, "strh");
    put32(h + 4, STRH);
    h += 8;
    if (video) {
      fourcc(h, "vids");
      fourcc(h + 4, this->h264_ ? "H264" : "MJPG");
      put32(h + 20, 1);           // dwScale
      put32(h + 24, this->fps_);  // dwRate
      put32(h + 32, this->total_pictures_);
      put32(h + 36, this->max_chunk_);
      put32(h + 40, 0xFFFFFFFFu);  // dwQuality: default
      put16(h + 52, this->width_);
      put16(h + 54, this->height_);
    } else {
      fourcc(h, "auds");
      put32(h + 20, block);
      put32(h + 24, this->rate_ * block);
      put32(h + 32, static_cast<uint32_t>(this->total_audio_bytes_ / block));
      put32(h + 40, 0xFFFFFFFFu);
      put32(h + 44, block);  // dwSampleSize
    }
    h += STRH;
    fourcc(h, "strf");
    put32(h + 4, video ? STRF_VIDEO : STRF_AUDIO);
    h += 8;
    if (video) {
      put32(h, STRF_VIDEO);
      put32(h + 4, this->width_);
      put32(h + 8, this->height_);
      put16(h + 12, 1);
      put16(h + 14, 24);
      fourcc(h + 16, this->h264_ ? "H264" : "MJPG");
      put32(h + 20, static_cast<uint32_t>(this->width_) * this->height_ * 3);
      h += STRF_VIDEO;
    } else {
      put16(h, 1);  // PCM
      put16(h + 2, this->channels_);
      put32(h + 4, this->rate_);
      put32(h + 8, this->rate_ * block);
      put16(h + 12, block);
      put16(h + 14, 16);
      h += STRF_AUDIO;
    }
    // The super index: one entry per closed segment (its ix##).
    fourcc(h, "indx");
    put32(h + 4, INDX);
    h += 8;
    put16(h, 4);  // wLongsPerEntry
    h[3] = 0;     // AVI_INDEX_OF_INDEXES
    put32(h + 4, closed);
    fourcc(h + 8, video ? "00dc" : "01wb");
    const Super *entries = this->super_(video ? 0 : 1);
    for (uint32_t i = 0; i < closed; i++) {
      uint8_t *e = h + 24 + 16 * i;
      put64(e, entries[i].pos);
      put32(e + 8, entries[i].size);
      put32(e + 12, entries[i].count);
    }
  };
  strl(out + STRL_VIDEO_START, true);
  if (audio)
    strl(out + this->strl_audio_, false);

  p = out + this->odml_;
  fourcc(p, "LIST");
  put32(p + 4, static_cast<uint32_t>(ODML - 8));
  fourcc(p + 8, "odml");
  fourcc(p + 12, "dmlh");
  put32(p + 16, DMLH);
  put32(p + 20, this->total_pictures_);

  p = out + this->header_size_ - 12;
  fourcc(p, "LIST");
  put32(p + 4, this->first_movi_bytes_);
  fourcc(p + 8, "movi");
}

bool CastAvi::fits(const Batch &batch) const {
  const size_t cap = this->ix_capacity_();
  if (this->entries_[0] + batch.pictures > cap || this->entries_[1] + batch.audio_chunks() > cap)
    return false;
  const uint64_t indexes =
      2 * IX_HEADER + 8ull * (this->entries_[0] + batch.pictures + this->entries_[1] + batch.audio_chunks());
  return this->file_pos_ + batch.bytes() + indexes - this->seg_start_ <= SEGMENT_BYTES;
}

void CastAvi::put_entry_(uint8_t stream, uint64_t chunk_pos, uint32_t size) {
  uint8_t *e = this->ix_(stream) + IX_HEADER + 8 * this->entries_[stream]++;
  put32(e, static_cast<uint32_t>(chunk_pos - this->movi_pos_ + CHUNK_HEADER));
  put32(e + 4, size);
}

void CastAvi::add(const Batch &batch) {
  uint64_t pos = this->file_pos_;
  this->put_entry_(0, pos, batch.jpeg_len | (batch.key ? 0 : DELTA_FRAME));
  pos += CHUNK_HEADER + batch.jpeg_len + (batch.jpeg_len & 1);
  for (uint16_t i = 1; i < batch.pictures; i++) {
    this->put_entry_(0, pos, this->h264_ ? DELTA_FRAME : 0);
    pos += CHUNK_HEADER;
  }
  if (batch.audio_len != 0 && this->rate_ != 0) {
    this->put_entry_(1, pos, batch.audio_len);
    pos += CHUNK_HEADER + batch.audio_len + (batch.audio_len & 1);
    this->total_audio_bytes_ += batch.audio_len;
  }
  this->file_pos_ = pos;
  this->total_pictures_ += batch.pictures;
}

const uint8_t *CastAvi::close_index(uint8_t stream, size_t *len) {
  if (stream == 1 && this->rate_ == 0) {
    *len = 0;
    return nullptr;
  }
  const uint32_t n = this->entries_[stream];
  uint8_t *ix = this->ix_(stream);
  fourcc(ix, stream == 0 ? "ix00" : "ix01");
  put32(ix + 4, 24 + 8 * n);
  put16(ix + 8, 2);  // wLongsPerEntry
  ix[10] = 0;
  ix[11] = 1;  // AVI_INDEX_OF_CHUNKS
  put32(ix + 12, n);
  fourcc(ix + 16, stream == 0 ? "00dc" : "01wb");
  put64(ix + 20, this->movi_pos_);
  put32(ix + 28, 0);
  *len = IX_HEADER + 8 * n;
  this->super_(stream)[this->segment_] = {this->file_pos_, static_cast<uint32_t>(*len), n};
  this->file_pos_ += *len;
  return ix;
}

void CastAvi::close_sizes() {
  const auto riff = static_cast<uint32_t>(this->file_pos_ - this->seg_start_ - 8);
  const auto movi = static_cast<uint32_t>(this->file_pos_ - this->movi_pos_);
  if (this->segment_ == 0) {
    this->first_riff_bytes_ = riff;
    this->first_movi_bytes_ = movi;
    this->first_riff_pictures_ = this->total_pictures_;
  } else {
    this->seg_riff_bytes_ = riff;
    this->seg_movi_bytes_ = movi;
  }
  this->closed_ = this->segment_ + 1;
}

void CastAvi::build_segment_patch(uint8_t *out) const {
  fourcc(out, "RIFF");
  put32(out + 4, this->seg_riff_bytes_);
  fourcc(out + 8, "AVIX");
  fourcc(out + 12, "LIST");
  put32(out + 16, this->seg_movi_bytes_);
  fourcc(out + 20, "movi");
}

void CastAvi::next_segment(uint8_t *out) {
  this->segment_++;
  this->seg_start_ = this->file_pos_;
  this->movi_pos_ = this->file_pos_ + SEGMENT_START - 4;
  this->entries_[0] = this->entries_[1] = 0;
  fourcc(out, "RIFF");
  put32(out + 4, 0);
  fourcc(out + 8, "AVIX");
  fourcc(out + 12, "LIST");
  put32(out + 16, 0);
  fourcc(out + 20, "movi");
  this->file_pos_ += SEGMENT_START;
}

}  // namespace esphome::simple_video_player
