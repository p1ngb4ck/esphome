#include "queue.h"

#ifdef USE_MEDIA_PLAYER_QUEUE

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#endif

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <new>

namespace esphome::media_player {

static const char *const TAG = "media_player.queue";
static constexpr uint32_t RESTART_MS = 3000;

MediaQueue *MediaQueue::create() {
  void *mem = RAMAllocator<MediaQueue>(RAMAllocator<MediaQueue>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) MediaQueue();
  return new MediaQueue();
}

void MediaQueue::setup() {
  this->player_->add_on_state_callback([this](MediaPlayerState state) {
    const MediaPlayerState last = this->last_;
    this->last_ = state;
    if (!this->playing_)
      return;
    if (state == MEDIA_PLAYER_STATE_PLAYING) {
      this->started_ = true;
      return;
    }
    if (state != MEDIA_PLAYER_STATE_IDLE || !this->started_ || last == MEDIA_PLAYER_STATE_IDLE)
      return;
    this->defer([this]() { this->advance_(false); });
  });
}

std::string MediaQueue::title(size_t index) const {
  if (index >= this->entries_.size())
    return "";
  const Entry &entry = this->entries_[index];
  if (!entry.title.empty())
    return entry.title;
  const size_t slash = entry.path.rfind('/');
  return slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
}

void MediaQueue::add(const std::string &path, const std::string &title) {
  if (path.empty() || this->entries_.size() >= this->max_)
    return;
  this->entries_.push_back(Entry{path, title});
  this->order_.push_back(static_cast<uint16_t>(this->entries_.size() - 1));
  this->generation_++;
  this->callbacks_.call();
}

void MediaQueue::clear() {
  if (this->playing_)
    this->stop();
  this->entries_.clear();
  this->entries_.shrink_to_fit();
  this->order_.clear();
  this->order_.shrink_to_fit();
  this->current_ = false;
  this->pos_ = 0;
  this->generation_++;
  this->callbacks_.call();
}

void MediaQueue::reorder_() {
  const int playing = this->index();
  this->order_.clear();
  for (size_t i = 0; i < this->entries_.size(); i++)
    this->order_.push_back(static_cast<uint16_t>(i));
  if (this->shuffle_) {
    for (size_t i = this->order_.size(); i > 1; i--)
      std::swap(this->order_[i - 1], this->order_[random_uint32() % i]);
  }
  this->pos_ = 0;
  if (playing >= 0) {
    for (size_t i = 0; i < this->order_.size(); i++) {
      if (this->order_[i] == playing)
        this->pos_ = i;
    }
  }
}

void MediaQueue::start_(size_t pos) {
  if (pos >= this->order_.size())
    return;
  this->pos_ = pos;
  this->current_ = true;
  this->playing_ = true;
  this->started_ = false;
  this->started_ms_ = millis();
  const Entry &entry = this->entries_[this->order_[pos]];
  ESP_LOGD(TAG, "Track %u: %s", static_cast<unsigned>(this->order_[pos]), entry.path.c_str());
#ifdef USE_MEDIA_PLAYER_QUEUE_RELAY
  if (this->relay_ != nullptr && entry.path.rfind("media-source://", 0) == 0) {
    this->relay_->ha_play(entry.path);
    this->callbacks_.call();
    return;
  }
#endif
  const std::string url = entry.path.rfind('/', 0) == 0 ? "file://" + entry.path : entry.path;
  this->player_->make_call().set_media_url(url).perform();
  this->callbacks_.call();
}

void MediaQueue::advance_(bool wrap) {
  if (this->order_.empty())
    return;
  if (this->repeat_ == REPEAT_ONE && !wrap) {
    this->start_(this->pos_);
    return;
  }
  size_t next = this->pos_ + 1;
  if (next >= this->order_.size()) {
    if (this->repeat_ != REPEAT_ALL && !wrap) {
      this->playing_ = false;
      this->callbacks_.call();
      return;
    }
    if (this->shuffle_)
      this->reorder_();
    next = 0;
  }
  this->start_(next);
}

void MediaQueue::play(size_t index) {
  this->defer([this, index]() {
    for (size_t i = 0; i < this->order_.size(); i++) {
      if (this->order_[i] == index) {
        this->start_(i);
        return;
      }
    }
  });
}

void MediaQueue::play_or_resume() {
  this->defer([this]() {
    if (this->order_.empty())
      return;
    if (this->player_->state == MEDIA_PLAYER_STATE_PAUSED && this->playing_) {
      this->player_->make_call().set_command(MEDIA_PLAYER_COMMAND_PLAY).perform();
      return;
    }
    if (!this->current_ || (!this->playing_ && this->pos_ + 1 >= this->order_.size())) {
      if (this->shuffle_)
        this->reorder_();
      this->start_(0);
      return;
    }
    this->start_(this->pos_);
  });
}

void MediaQueue::next() {
  this->defer([this]() { this->advance_(true); });
}

void MediaQueue::previous() {
  this->defer([this]() {
    if (!this->current_)
      return;
    if (millis() - this->started_ms_ > RESTART_MS || this->pos_ == 0) {
      this->start_(this->pos_);
      return;
    }
    this->start_(this->pos_ - 1);
  });
}

void MediaQueue::stop() {
  this->defer([this]() {
    if (!this->playing_)
      return;
    this->playing_ = false;
    this->player_->make_call().set_command(MEDIA_PLAYER_COMMAND_STOP).perform();
    this->callbacks_.call();
  });
}

void MediaQueue::set_shuffle(bool shuffle) {
  if (this->shuffle_ == shuffle)
    return;
  this->shuffle_ = shuffle;
  this->reorder_();
  this->callbacks_.call();
}

#ifdef USE_STORAGE
void MediaQueue::play_folder(const std::string &path, std::vector<std::string> extensions) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0 || this->folder_ != nullptr || storage::global_storage_registry == nullptr ||
      storage::global_storage_worker == nullptr) {
    ESP_LOGW(TAG, "Cannot list the folder of %s", path.c_str());
    return;
  }
  void *mem = RAMAllocator<Folder>(RAMAllocator<Folder>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr)
    return;
  this->folder_ = new (mem) Folder();
  this->folder_->dir = path.substr(0, slash);
  this->folder_->start = path.substr(slash + 1);
  this->folder_->extensions = std::move(extensions);
  this->folder_->max = this->max_;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->folder_->dir.c_str(), &rel);
  if (ps == nullptr) {
    this->folder_listed_(false);
    return;
  }
  auto entry = [](const storage::FileStat *stat, void *ctx) -> bool {
    auto *folder = static_cast<Folder *>(ctx);
    if (stat->is_dir || stat->name[0] == '.')
      return true;
    const char *dot = strrchr(stat->name, '.');
    if (dot == nullptr)
      return true;
    for (const std::string &ext : folder->extensions) {
      if (strcasecmp(dot, ext.c_str()) == 0) {
        folder->names.emplace_back(stat->name);
        break;
      }
    }
    return folder->names.size() < folder->max;
  };
  const storage::StorageError err = storage::global_storage_worker->async_list_dir(
      ps, (rel == nullptr || rel[0] == '\0') ? "/" : rel, entry, this->folder_,
      [this](storage::StorageError e) { this->folder_listed_(e == storage::StorageError::STORAGE_ERROR_OK); });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->folder_listed_(false);
}

void MediaQueue::folder_listed_(bool ok) {
  Folder *folder = this->folder_;
  this->folder_ = nullptr;
  if (!ok)
    ESP_LOGW(TAG, "Listing %s failed", folder->dir.c_str());
  if (this->playing_)
    this->stop();
  this->entries_.clear();
  this->order_.clear();
  this->current_ = false;
  if (ok) {
    std::sort(folder->names.begin(), folder->names.end());
    for (const std::string &name : folder->names) {
      this->entries_.push_back(Entry{folder->dir + "/" + name, name});
      this->order_.push_back(static_cast<uint16_t>(this->entries_.size() - 1));
    }
  }
  if (this->entries_.empty()) {
    this->entries_.push_back(Entry{folder->dir + "/" + folder->start, folder->start});
    this->order_.push_back(0);
  }
  this->entries_.shrink_to_fit();
  this->order_.shrink_to_fit();
  if (this->shuffle_)
    this->reorder_();
  size_t start = 0;
  for (size_t i = 0; i < this->entries_.size(); i++) {
    if (this->entries_[i].title == folder->start)
      start = i;
  }
  folder->~Folder();
  RAMAllocator<Folder>(RAMAllocator<Folder>::ALLOC_EXTERNAL).deallocate(folder, 1);
  this->generation_++;
  for (size_t i = 0; i < this->order_.size(); i++) {
    if (this->order_[i] == start) {
      this->start_(i);
      return;
    }
  }
  this->callbacks_.call();
}
#endif

void MediaQueue::set_repeat(uint8_t repeat) {
  this->repeat_ = repeat <= REPEAT_ALL ? repeat : static_cast<uint8_t>(REPEAT_OFF);
  this->callbacks_.call();
}

}

#endif
