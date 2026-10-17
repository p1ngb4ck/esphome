#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "psram_pool.h"

namespace esphome::simple_video_player {

/// A string of at most N - 1 bytes kept in place: the player's list entries hold no memory of their
/// own. Longer text is cut.
template<size_t N> class FixedString {
  static_assert(N > 1 && N <= 65535, "FixedString size");

 public:
  static constexpr size_t CAPACITY = N - 1;
  /// Whether text of len bytes fits whole.
  static constexpr bool fits(size_t len) { return len <= CAPACITY; }
  static bool fits(const char *s) { return fits(s != nullptr ? std::strlen(s) : 0); }

  FixedString() = default;
  FixedString(const char *s) { this->assign(s); }          // NOLINT(google-explicit-constructor)
  FixedString(const std::string &s) { this->assign(s.data(), s.size()); }  // NOLINT(google-explicit-constructor)
  FixedString &operator=(const char *s) {
    this->assign(s);
    return *this;
  }
  FixedString &operator=(const std::string &s) {
    this->assign(s.data(), s.size());
    return *this;
  }
  void assign(const char *s) { this->assign(s, s != nullptr ? std::strlen(s) : 0); }
  void assign(const char *s, size_t len) {
    len = std::min(len, N - 1);
    if (len != 0)
      std::memmove(this->buf_, s, len);
    this->buf_[len] = '\0';
    this->len_ = static_cast<uint16_t>(len);
  }
  FixedString &operator+=(const char *s) {
    const size_t add = std::min(s != nullptr ? std::strlen(s) : 0, N - 1 - this->len_);
    std::memcpy(this->buf_ + this->len_, s, add);
    this->len_ = static_cast<uint16_t>(this->len_ + add);
    this->buf_[this->len_] = '\0';
    return *this;
  }
  FixedString &operator+=(char c) {
    if (this->len_ + 1 < N) {
      this->buf_[this->len_++] = c;
      this->buf_[this->len_] = '\0';
    }
    return *this;
  }
  const char *c_str() const { return this->buf_; }
  const char *data() const { return this->buf_; }
  size_t size() const { return this->len_; }
  size_t length() const { return this->len_; }
  bool empty() const { return this->len_ == 0; }
  void clear() {
    this->len_ = 0;
    this->buf_[0] = '\0';
  }
  /// Whether it starts with prefix.
  bool starts_with(const char *prefix) const { return std::strncmp(this->buf_, prefix, std::strlen(prefix)) == 0; }
  char operator[](size_t i) const { return i < this->len_ ? this->buf_[i] : '\0'; }
  size_t find(char c, size_t from = 0) const {
    for (size_t i = from; i < this->len_; i++) {
      if (this->buf_[i] == c)
        return i;
    }
    return std::string::npos;
  }
  size_t rfind(char c) const {
    for (size_t i = this->len_; i > 0; i--) {
      if (this->buf_[i - 1] == c)
        return i - 1;
    }
    return std::string::npos;
  }
  /// A copy of part of it, for text put together on the spot (as std::string's).
  std::string substr(size_t pos, size_t n = std::string::npos) const {
    pos = std::min<size_t>(pos, this->len_);
    return std::string(this->buf_ + pos, std::min(n, this->len_ - pos));
  }
  operator std::string() const { return std::string(this->buf_, this->len_); }  // NOLINT(google-explicit-constructor)
  bool operator==(const char *s) const { return std::strcmp(this->buf_, s != nullptr ? s : "") == 0; }
  bool operator!=(const char *s) const { return !(*this == s); }
  bool operator==(const std::string &s) const { return *this == s.c_str(); }
  bool operator!=(const std::string &s) const { return !(*this == s.c_str()); }
  template<size_t M> bool operator==(const FixedString<M> &other) const { return *this == other.c_str(); }
  template<size_t M> bool operator!=(const FixedString<M> &other) const { return !(*this == other.c_str()); }

 protected:
  char buf_[N]{};
  uint16_t len_{0};
};

template<size_t N> std::string operator+(const char *a, const FixedString<N> &b) { return std::string(a) + b.c_str(); }
template<size_t N> std::string operator+(const std::string &a, const FixedString<N> &b) { return a + b.c_str(); }
template<size_t N> std::string operator+(const FixedString<N> &a, const char *b) { return std::string(a.c_str()) + b; }
template<size_t N> std::string operator+(const FixedString<N> &a, const std::string &b) {
  return std::string(a.c_str()) + b;
}
template<size_t N, size_t M> std::string operator+(const FixedString<N> &a, const FixedString<M> &b) {
  return std::string(a.c_str()) + b.c_str();
}
template<size_t N> bool operator==(const char *s, const FixedString<N> &f) { return f == s; }
template<size_t N> bool operator!=(const char *s, const FixedString<N> &f) { return f != s; }
template<size_t N> bool operator==(const std::string &s, const FixedString<N> &f) { return f == s; }
template<size_t N> bool operator!=(const std::string &s, const FixedString<N> &f) { return f != s; }

/// Text kept elsewhere (a FixedString's, which must outlive it), "" for none: what the player's
/// accessors give.
class TextView {
 public:
  TextView() = default;
  template<size_t N> TextView(const FixedString<N> &text)  // NOLINT(google-explicit-constructor)
      : text_(text.c_str()), len_(text.size()) {}
  const char *c_str() const { return this->text_; }
  const char *data() const { return this->text_; }
  size_t size() const { return this->len_; }
  size_t length() const { return this->len_; }
  bool empty() const { return this->len_ == 0; }
  operator std::string() const { return std::string(this->text_, this->len_); }  // NOLINT(google-explicit-constructor)
  bool operator==(const char *s) const { return std::strcmp(this->text_, s != nullptr ? s : "") == 0; }
  bool operator!=(const char *s) const { return !(*this == s); }
  bool operator==(const std::string &s) const { return *this == s.c_str(); }
  bool operator!=(const std::string &s) const { return !(*this == s.c_str()); }

 protected:
  const char *text_{""};
  size_t len_{0};
};

/// A list of a fixed capacity in one piece at the end of the player's pool (taken once by reserve() in
/// setup, counted in plan_pool_() by bytes()). Full: push_back() refuses. Its entries are kept in place
/// (no memory of their own).
template<class T> class PoolList {
 public:
  static size_t bytes(size_t capacity) {
    return (capacity * sizeof(T) + PsramPool::ALIGN - 1) & ~(PsramPool::ALIGN - 1);
  }

  PoolList() = default;
  PoolList(const PoolList &) = delete;
  PoolList &operator=(const PoolList &) = delete;
  ~PoolList() { this->release(); }

  /// The piece for capacity entries from the pool; false when it has no room.
  bool reserve(PsramPool &pool, size_t capacity) {
    if (this->data_ != nullptr)
      return this->capacity_ >= capacity;
    uint8_t *piece = pool.alloc_top(capacity * sizeof(T));
    if (piece == nullptr)
      return false;
    this->pool_ = &pool;
    this->data_ = reinterpret_cast<T *>(piece);
    this->capacity_ = capacity;
    return true;
  }
  void release() {
    this->clear();
    if (this->pool_ != nullptr && this->data_ != nullptr)
      this->pool_->free(reinterpret_cast<uint8_t *>(this->data_));
    this->data_ = nullptr;
    this->capacity_ = 0;
  }

  size_t size() const { return this->size_; }
  size_t capacity() const { return this->capacity_; }
  bool empty() const { return this->size_ == 0; }
  bool full() const { return this->size_ >= this->capacity_; }
  T &operator[](size_t i) { return this->data_[i]; }
  const T &operator[](size_t i) const { return this->data_[i]; }
  T &front() { return this->data_[0]; }
  const T &front() const { return this->data_[0]; }
  T &back() { return this->data_[this->size_ - 1]; }
  const T &back() const { return this->data_[this->size_ - 1]; }
  T *begin() { return this->data_; }
  T *end() { return this->data_ + this->size_; }
  const T *begin() const { return this->data_; }
  const T *end() const { return this->data_ + this->size_; }

  bool push_back(const T &value) {
    if (this->full())
      return false;
    ::new (this->data_ + this->size_) T(value);
    this->size_++;
    return true;
  }
  template<class... Args> T *emplace_back(Args &&...args) {
    if (this->full())
      return nullptr;
    T *slot = ::new (this->data_ + this->size_) T(std::forward<Args>(args)...);
    this->size_++;
    return slot;
  }
  void pop_back() {
    if (this->size_ > 0)
      this->data_[--this->size_].~T();
  }
  void erase(T *at) {
    if (at < this->begin() || at >= this->end())
      return;
    std::move(at + 1, this->end(), at);
    this->pop_back();
  }
  bool insert(T *at, const T &value) {
    if (this->full() || at < this->begin() || at > this->end())
      return false;
    const size_t index = static_cast<size_t>(at - this->begin());
    ::new (this->data_ + this->size_) T();
    this->size_++;
    std::move_backward(this->begin() + index, this->end() - 1, this->end());
    this->data_[index] = value;
    return true;
  }
  void clear() {
    while (this->size_ > 0)
      this->data_[--this->size_].~T();
  }
  /// Place i past the entries (nullptr past the capacity): filled there by its own writer, taken in
  /// by adopt() once that writer is done.
  T *place(size_t i) { return i < this->capacity_ ? this->data_ + i : nullptr; }
  /// The entries are the first n places (the ones past the size made in place()).
  void adopt(size_t n) { this->size_ = std::min(n, this->capacity_); }
  /// Keeps the first n.
  void truncate(size_t n) {
    while (this->size_ > n)
      this->data_[--this->size_].~T();
  }
  /// Both lists' contents trade places (their pieces too).
  void swap(PoolList &other) {
    std::swap(this->pool_, other.pool_);
    std::swap(this->data_, other.data_);
    std::swap(this->size_, other.size_);
    std::swap(this->capacity_, other.capacity_);
  }

 protected:
  PsramPool *pool_{nullptr};
  T *data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

/// Sorts list by less through order (its index scratch, as large as list): the indexes are sorted,
/// then each entry moved once into its place (in cycles, one entry held aside).
template<class T, class Less> void sort_by_order(PoolList<T> &list, PoolList<uint16_t> &order, Less less) {
  const size_t count = list.size();
  if (count < 2 || order.capacity() < count || count > UINT16_MAX) {
    std::sort(list.begin(), list.end(), less);
    return;
  }
  order.clear();
  for (size_t i = 0; i < count; i++)
    order.push_back(static_cast<uint16_t>(i));
  std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) { return less(list[a], list[b]); });
  // order[at]: the entry that goes to at; set to at once it is there.
  for (size_t i = 0; i < count; i++) {
    if (order[i] == i)
      continue;
    T held = list[i];
    size_t at = i;
    while (true) {
      const size_t from = order[at];
      order[at] = static_cast<uint16_t>(at);
      if (from == i) {
        list[at] = held;
        break;
      }
      list[at] = list[from];
      at = from;
    }
  }
  order.clear();
}

}  // namespace esphome::simple_video_player
