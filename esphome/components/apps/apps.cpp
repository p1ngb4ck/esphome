#include "apps.h"

namespace esphome::apps {

void AppSlot::start() {
  if (this->running_)
    return;
  for (size_t i = 0; i < this->excluded_count_; i++) {
    AppSlot *other = this->excluded_[i];
    if (other != this && other->is_running())
      other->stop();
  }
  this->running_ = true;
  this->start_trigger_.trigger();
}

void AppSlot::stop() {
  if (!this->running_)
    return;
  this->running_ = false;
  this->stop_trigger_.trigger();
}

}
