#include "ipk_reader.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include "esphome/core/helpers.h"
#include <algorithm>
#include <cstring>

namespace esphome::package_manager {

static const size_t TAR_BLOCK = 512;
static const size_t AR_HEADER = 60;
static const uint8_t GZ_FHCRC = 0x02;
static const uint8_t GZ_FEXTRA = 0x04;
static const uint8_t GZ_FNAME = 0x08;
static const uint8_t GZ_FCOMMENT = 0x10;

static uint64_t parse_number(const uint8_t *text, size_t len, uint8_t base) {
  uint64_t value = 0;
  for (size_t i = 0; i < len; i++) {
    const uint8_t c = text[i];
    if (c == ' ' || c == '\0') {
      if (value != 0)
        break;
      continue;
    }
    if (c < '0' || c >= '0' + base)
      break;
    value = value * base + (c - '0');
  }
  return value;
}

bool IpkReader::begin() {
  this->end();
  this->window_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(TINFL_LZ_DICT_SIZE);
  this->tar_head_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(TAR_BLOCK);
  this->inflator_ = RAMAllocator<tinfl_decompressor>(RAMAllocator<tinfl_decompressor>::ALLOC_EXTERNAL).allocate(1);
  if (this->window_ == nullptr || this->tar_head_ == nullptr || this->inflator_ == nullptr) {
    this->end();
    return false;
  }
  this->state_ = State::AR_MAGIC;
  this->member_ = Member::NONE;
  this->in_ = nullptr;
  this->in_len_ = 0;
  this->last_ = false;
  this->member_left_ = 0;
  this->member_pad_ = false;
  this->head_have_ = 0;
  this->out_len_ = 0;
  this->file_end_pending_ = false;
  this->error_ = nullptr;
  return true;
}

void IpkReader::end() {
  if (this->window_ != nullptr)
    RAMAllocator<uint8_t>().deallocate(this->window_, TINFL_LZ_DICT_SIZE);
  if (this->tar_head_ != nullptr)
    RAMAllocator<uint8_t>().deallocate(this->tar_head_, TAR_BLOCK);
  if (this->inflator_ != nullptr)
    RAMAllocator<tinfl_decompressor>().deallocate(this->inflator_, 1);
  this->window_ = nullptr;
  this->tar_head_ = nullptr;
  this->inflator_ = nullptr;
}

void IpkReader::feed(const uint8_t *data, size_t len, bool last) {
  this->in_ = data;
  this->in_len_ = len;
  this->last_ = last;
}

IpkReader::Event IpkReader::fail_(const char *error) {
  this->state_ = State::ERROR;
  this->error_ = error;
  return Event::ERROR;
}

void IpkReader::use_(size_t n, bool member) {
  this->in_ += n;
  this->in_len_ -= n;
  if (member)
    this->member_left_ -= n;
}

bool IpkReader::take_(uint8_t *dst, size_t want, size_t &have, bool member) {
  size_t n = std::min<size_t>(want - have, this->in_len_);
  if (member)
    n = static_cast<size_t>(std::min<uint64_t>(n, this->member_left_));
  std::memcpy(dst + have, this->in_, n);
  have += n;
  this->use_(n, member);
  return have == want;
}

bool IpkReader::skip_(uint64_t &left, bool member) {
  const size_t n = static_cast<size_t>(std::min<uint64_t>(left, this->in_len_));
  left -= n;
  this->use_(n, member);
  return left == 0;
}

void IpkReader::start_gzip_() {
  tinfl_init(this->inflator_);
  this->window_ofs_ = 0;
  this->out_len_ = 0;
  this->inflate_done_ = false;
  this->tar_state_ = Tar::HEADER;
  this->tar_have_ = 0;
  this->state_ = State::INFLATE;
}

IpkReader::Event IpkReader::next() {
  if (this->file_end_pending_) {
    this->file_end_pending_ = false;
    return Event::FILE_END;
  }
  while (true) {
    switch (this->state_) {
      case State::AR_MAGIC:
        if (!this->take_(this->head_, 8, this->head_have_, false))
          return this->last_ ? this->fail_("not an ipk") : Event::NEED_INPUT;
        if (std::memcmp(this->head_, "!<arch>\n", 8) != 0)
          return this->fail_("not an ipk");
        this->head_have_ = 0;
        this->state_ = State::AR_HEADER;
        break;
      case State::AR_HEADER: {
        if (this->member_pad_) {
          if (this->in_len_ == 0)
            return this->last_ ? this->fail_("truncated") : Event::NEED_INPUT;
          this->use_(1, false);
          this->member_pad_ = false;
        }
        if (this->head_have_ == 0 && this->in_len_ == 0 && this->last_) {
          this->state_ = State::DONE;
          return Event::DONE;
        }
        if (!this->take_(this->head_, AR_HEADER, this->head_have_, false))
          return this->last_ ? this->fail_("truncated") : Event::NEED_INPUT;
        this->head_have_ = 0;
        if (this->head_[58] != '`' || this->head_[59] != '\n')
          return this->fail_("bad ar header");
        size_t name_len = 16;
        while (name_len > 0 && (this->head_[name_len - 1] == ' ' || this->head_[name_len - 1] == '/'))
          name_len--;
        this->member_left_ = parse_number(this->head_ + 48, 10, 10);
        this->member_pad_ = (this->member_left_ & 1) != 0;
        auto is = [this, name_len](const char *name) {
          return std::strlen(name) == name_len && std::memcmp(this->head_, name, name_len) == 0;
        };
        if (is("control.tar.gz")) {
          this->member_ = Member::CONTROL;
          this->state_ = State::GZ_HEADER;
        } else if (is("data.tar.gz")) {
          this->member_ = Member::DATA;
          this->state_ = State::GZ_HEADER;
        } else {
          this->member_ = Member::NONE;
          this->state_ = State::AR_SKIP;
        }
        break;
      }
      case State::AR_SKIP:
      case State::TRAILER:
        if (!this->skip_(this->member_left_, false))
          return this->last_ ? this->fail_("truncated") : Event::NEED_INPUT;
        this->member_ = Member::NONE;
        this->state_ = State::AR_HEADER;
        break;
      case State::GZ_HEADER:
        if (!this->take_(this->head_, 10, this->head_have_, true))
          return this->last_ || this->member_left_ == 0 ? this->fail_("truncated gzip") : Event::NEED_INPUT;
        this->head_have_ = 0;
        if (this->head_[0] != 0x1f || this->head_[1] != 0x8b || this->head_[2] != 8)
          return this->fail_("not gzip");
        this->gz_flags_ = this->head_[3];
        this->state_ = State::GZ_EXTRA;
        break;
      case State::GZ_EXTRA:
        if ((this->gz_flags_ & GZ_FEXTRA) != 0) {
          if (this->gz_extra_left_ == 0) {
            if (!this->take_(this->head_, 2, this->head_have_, true))
              return this->last_ ? this->fail_("truncated gzip") : Event::NEED_INPUT;
            this->head_have_ = 0;
            this->gz_extra_left_ = static_cast<uint16_t>(this->head_[0] | (this->head_[1] << 8));
          }
          uint64_t left = this->gz_extra_left_;
          const bool done = this->skip_(left, true);
          this->gz_extra_left_ = static_cast<uint16_t>(left);
          if (!done)
            return this->last_ ? this->fail_("truncated gzip") : Event::NEED_INPUT;
          this->gz_flags_ &= ~GZ_FEXTRA;
        }
        this->state_ = State::GZ_NAME;
        break;
      case State::GZ_NAME:
      case State::GZ_COMMENT: {
        const uint8_t flag = this->state_ == State::GZ_NAME ? GZ_FNAME : GZ_FCOMMENT;
        while ((this->gz_flags_ & flag) != 0 && this->in_len_ > 0 && this->member_left_ > 0) {
          const bool end = *this->in_ == 0;
          this->use_(1, true);
          if (end)
            this->gz_flags_ &= ~flag;
        }
        if ((this->gz_flags_ & flag) != 0)
          return this->last_ ? this->fail_("truncated gzip") : Event::NEED_INPUT;
        this->state_ = this->state_ == State::GZ_NAME ? State::GZ_COMMENT : State::GZ_CRC;
        break;
      }
      case State::GZ_CRC:
        if ((this->gz_flags_ & GZ_FHCRC) != 0) {
          if (!this->take_(this->head_, 2, this->head_have_, true))
            return this->last_ ? this->fail_("truncated gzip") : Event::NEED_INPUT;
          this->head_have_ = 0;
        }
        this->start_gzip_();
        break;
      case State::INFLATE: {
        const Event event = this->inflate_();
        if (event != Event::NEED_INPUT || this->state_ == State::INFLATE)
          return event;
        break;
      }
      case State::DONE:
        return Event::DONE;
      case State::ERROR:
        return Event::ERROR;
    }
  }
}

IpkReader::Event IpkReader::inflate_() {
  while (true) {
    if (this->out_len_ > 0) {
      const Event event = this->tar_();
      if (event != Event::NEED_INPUT)
        return event;
    }
    if (this->inflate_done_) {
      this->state_ = State::TRAILER;
      return Event::NEED_INPUT;
    }
    const size_t avail = static_cast<size_t>(std::min<uint64_t>(this->in_len_, this->member_left_));
    if (avail == 0 && !this->last_ && this->member_left_ > 0)
      return Event::NEED_INPUT;
    size_t in_bytes = avail;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - this->window_ofs_;
    const mz_uint32 flags = this->member_left_ > avail ? TINFL_FLAG_HAS_MORE_INPUT : 0;
    const tinfl_status status = tinfl_decompress(this->inflator_, this->in_, &in_bytes, this->window_,
                                                 this->window_ + this->window_ofs_, &out_bytes, flags);
    this->use_(in_bytes, true);
    this->out_ = this->window_ + this->window_ofs_;
    this->out_len_ = out_bytes;
    this->window_ofs_ = (this->window_ofs_ + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    if (status < TINFL_STATUS_DONE)
      return this->fail_("corrupt gzip data");
    if (status == TINFL_STATUS_DONE)
      this->inflate_done_ = true;
    else if (status == TINFL_STATUS_NEEDS_MORE_INPUT && in_bytes == 0 && out_bytes == 0)
      return this->member_left_ == 0 || this->last_ ? this->fail_("truncated gzip") : Event::NEED_INPUT;
  }
}

IpkReader::Event IpkReader::tar_() {
  while (this->out_len_ > 0) {
    switch (this->tar_state_) {
      case Tar::HEADER: {
        const size_t n = std::min<size_t>(TAR_BLOCK - this->tar_have_, this->out_len_);
        std::memcpy(this->tar_head_ + this->tar_have_, this->out_, n);
        this->tar_have_ += n;
        this->out_ += n;
        this->out_len_ -= n;
        if (this->tar_have_ < TAR_BLOCK)
          return Event::NEED_INPUT;
        this->tar_have_ = 0;
        bool empty = true;
        for (size_t i = 0; i < TAR_BLOCK && empty; i++)
          empty = this->tar_head_[i] == 0;
        if (empty) {
          this->tar_state_ = Tar::END;
          break;
        }
        const size_t name_len = strnlen(reinterpret_cast<const char *>(this->tar_head_), 100);
        const size_t prefix_len = strnlen(reinterpret_cast<const char *>(this->tar_head_ + 345), 155);
        if (prefix_len + (prefix_len > 0 ? 1 : 0) + name_len >= sizeof(this->path_))
          return this->fail_("path too long");
        size_t len = 0;
        if (prefix_len > 0) {
          std::memcpy(this->path_, this->tar_head_ + 345, prefix_len);
          len = prefix_len;
          this->path_[len++] = '/';
        }
        std::memcpy(this->path_ + len, this->tar_head_, name_len);
        this->path_[len + name_len] = '\0';
        this->tar_left_ = parse_number(this->tar_head_ + 124, 12, 8);
        this->tar_pad_ = (TAR_BLOCK - this->tar_left_ % TAR_BLOCK) % TAR_BLOCK;
        const uint8_t type = this->tar_head_[156];
        this->tar_file_ = type == '0' || type == '\0';
        this->file_size_ = this->tar_left_;
        this->tar_state_ = Tar::BODY;
        if (this->tar_file_ && this->tar_left_ == 0) {
          this->tar_state_ = Tar::PAD;
          return Event::FILE_END;
        }
        break;
      }
      case Tar::BODY: {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(this->tar_left_, this->out_len_));
        const uint8_t *data = this->out_;
        this->out_ += n;
        this->out_len_ -= n;
        this->tar_left_ -= n;
        if (this->tar_left_ == 0)
          this->tar_state_ = Tar::PAD;
        if (this->tar_file_) {
          this->chunk_ = data;
          this->chunk_len_ = n;
          this->file_end_pending_ = this->tar_left_ == 0;
          return Event::FILE_DATA;
        }
        break;
      }
      case Tar::PAD: {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(this->tar_pad_, this->out_len_));
        this->out_ += n;
        this->out_len_ -= n;
        this->tar_pad_ -= n;
        if (this->tar_pad_ == 0)
          this->tar_state_ = Tar::HEADER;
        break;
      }
      case Tar::END:
        this->out_len_ = 0;
        break;
    }
  }
  return Event::NEED_INPUT;
}

}

#endif
