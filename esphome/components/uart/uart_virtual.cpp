#include "esphome/core/defines.h"
#ifdef USE_UART_VIRTUAL

#include "uart_virtual.h"

#include <algorithm>
#include <cstring>

namespace esphome::uart {

VirtualUARTComponent::VirtualUARTComponent(size_t rx_buffer_size) { this->alloc_rx_(rx_buffer_size); }

bool VirtualUARTComponent::alloc_rx_(size_t size) {
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  if (this->rx_ != nullptr) {
    allocator.deallocate(this->rx_, this->rx_capacity_);
    this->rx_ = nullptr;
  }
  this->rx_capacity_ = 0;
  this->rx_head_ = 0;
  this->rx_size_ = 0;
  this->rx_buffer_size_ = size;
  if (size == 0) {
    return true;
  }
  this->rx_ = allocator.allocate(size);
  if (this->rx_ == nullptr) {
    this->rx_buffer_size_ = 0;
    return false;
  }
  this->rx_capacity_ = size;
  return true;
}

bool VirtualUARTComponent::inject_rx(const uint8_t *data, size_t len) {
  if (len == 0) {
    return true;
  }
  if (len > this->rx_capacity_ - this->rx_size_) {
    return false;
  }
  size_t tail = (this->rx_head_ + this->rx_size_) % this->rx_capacity_;
  size_t first = std::min(len, this->rx_capacity_ - tail);
  std::memcpy(this->rx_ + tail, data, first);
  std::memcpy(this->rx_, data + first, len - first);
  this->rx_size_ += len;
  return true;
}

bool VirtualUARTComponent::peek_byte(uint8_t *data) {
  if (this->rx_size_ == 0) {
    return false;
  }
  *data = this->rx_[this->rx_head_];
  return true;
}

bool VirtualUARTComponent::read_array(uint8_t *data, size_t len) {
  if (this->rx_size_ < len) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  size_t first = std::min(len, this->rx_capacity_ - this->rx_head_);
  std::memcpy(data, this->rx_ + this->rx_head_, first);
  std::memcpy(data + first, this->rx_, len - first);
  this->rx_head_ = (this->rx_head_ + len) % this->rx_capacity_;
  this->rx_size_ -= len;
#ifdef USE_UART_DEBUGGER
  for (size_t i = 0; i < len; i++)
    this->debug_callback_.call(UART_DIRECTION_RX, data[i], StringRef());
#endif
  return true;
}

}  // namespace esphome::uart

#endif  // USE_UART_VIRTUAL
