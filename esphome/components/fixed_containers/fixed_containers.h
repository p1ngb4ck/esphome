#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include <esp_heap_caps.h>

namespace esphome::fixed_containers {

template<size_t N> class FixedString {
  static_assert(N > 1 && N <= 65535, "FixedString size");

 public:
  static constexpr size_t CAPACITY = N - 1;
  static constexpr bool fits(size_t len) { return len <= CAPACITY; }
  static bool fits(const char *s) { return fits(s != nullptr ? std::strlen(s) : 0); }

  FixedString() = default;
  FixedString(const char *s) { this->assign(s); }
  FixedString(const std::string &s) { this->assign(s.data(), s.size()); }
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
  std::string substr(size_t pos, size_t n = std::string::npos) const {
    pos = std::min<size_t>(pos, this->len_);
    return std::string(this->buf_ + pos, std::min(n, this->len_ - pos));
  }
  operator std::string() const { return std::string(this->buf_, this->len_); }
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

class TextView {
 public:
  TextView() = default;
  template<size_t N> TextView(const FixedString<N> &text) : text_(text.c_str()), len_(text.size()) {}
  const char *c_str() const { return this->text_; }
  const char *data() const { return this->text_; }
  size_t size() const { return this->len_; }
  size_t length() const { return this->len_; }
  bool empty() const { return this->len_ == 0; }
  operator std::string() const { return std::string(this->text_, this->len_); }
  bool operator==(const char *s) const { return std::strcmp(this->text_, s != nullptr ? s : "") == 0; }
  bool operator!=(const char *s) const { return !(*this == s); }
  bool operator==(const std::string &s) const { return *this == s.c_str(); }
  bool operator!=(const std::string &s) const { return !(*this == s.c_str()); }

 protected:
  const char *text_{""};
  size_t len_{0};
};

template<class T> class FixedList {
 public:
  FixedList() = default;
  FixedList(const FixedList &) = delete;
  FixedList &operator=(const FixedList &) = delete;
  ~FixedList() { this->release(); }

  bool reserve(size_t capacity) {
    if (this->data_ != nullptr)
      return this->capacity_ >= capacity;
    void *piece = heap_caps_malloc(capacity * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (piece == nullptr)
      return false;
    this->data_ = static_cast<T *>(piece);
    this->capacity_ = capacity;
    return true;
  }
  void release() {
    this->clear();
    heap_caps_free(this->data_);
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
  void clear() {
    while (this->size_ > 0)
      this->data_[--this->size_].~T();
  }
  void swap(FixedList &other) {
    std::swap(this->data_, other.data_);
    std::swap(this->size_, other.size_);
    std::swap(this->capacity_, other.capacity_);
  }

 protected:
  T *data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

}
