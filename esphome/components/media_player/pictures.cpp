#include "pictures.h"

#ifdef USE_MEDIA_PLAYER_PICTURES

#include "esphome/core/log.h"

#include <algorithm>
#include <iterator>
#include <cstring>
#include <new>
#include <strings.h>

namespace esphome::media_player {

static const char *const TAG = "media_player.pictures";
static const char *const PICTURE_SUFFIXES[] = {".jpg", ".jpeg", ".png", ".bmp", ".qoi"};
static constexpr size_t LIST_MAX = 2000;

using StringList = std::vector<std::string, RAMAllocator<std::string>>;

MediaPictures *MediaPictures::create() {
  void *mem = RAMAllocator<MediaPictures>(RAMAllocator<MediaPictures>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) MediaPictures();
  return new MediaPictures();
}

void MediaPictures::setup() {
  this->callbacks_.add([this]() { this->picture_trigger_.trigger(); });
  if (this->image_ != nullptr) {
    this->image_->add_on_finished_callback([this](bool) { this->callbacks_.call(); });
    this->image_->add_on_error_callback([this]() {
      ESP_LOGW(TAG, "%s not shown", this->current_path().c_str());
      this->callbacks_.call();
    });
  }
  if (!this->directories_.empty())
    this->open_directory(this->directories_.front());
}

bool MediaPictures::is_picture_(const char *name) {
  const size_t len = strlen(name);
  for (const char *suffix : PICTURE_SUFFIXES) {
    const size_t n = strlen(suffix);
    if (len > n && strcasecmp(name + len - n, suffix) == 0)
      return true;
  }
  return false;
}

std::string MediaPictures::current_path() const {
  if (this->index_ < 0 || static_cast<size_t>(this->index_) >= this->names_.size())
    return std::string();
  if (this->dir_[0] == '\0')
    return this->names_[this->index_];
  return std::string(this->dir_) + "/" + this->names_[this->index_];
}

std::string MediaPictures::current_title() const {
  if (this->index_ < 0 || static_cast<size_t>(this->index_) >= this->names_.size())
    return std::string();
  if (static_cast<size_t>(this->index_) < this->titles_.size())
    return this->titles_[this->index_];
  return this->names_[this->index_];
}

void MediaPictures::show_sources(std::vector<std::string> sources, std::vector<std::string> titles, size_t index) {
  this->defer([this, sources = std::move(sources), titles = std::move(titles), index]() {
    if (sources.empty() || index >= sources.size())
      return;
    this->dir_[0] = '\0';
    this->failed_ = false;
    this->names_.assign(sources.begin(), sources.end());
    this->titles_.assign(titles.begin(), titles.end());
    this->load_(static_cast<int>(index));
  });
}

void MediaPictures::open_directory(const std::string &dir) {
  if (dir.empty() || dir.size() > storage::STORAGE_VFS_PATH_MAX)
    return;
  std::string trimmed = dir;
  while (trimmed.size() > 1 && trimmed.back() == '/')
    trimmed.pop_back();
  memcpy(this->dir_, trimmed.c_str(), trimmed.size() + 1);
  this->defer([this]() { this->list_(); });
}

void MediaPictures::list_() {
  if (this->listing_)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                 ? storage::global_storage_registry->resolve_path(this->dir_, &rel)
                                 : nullptr;
  if (ps == nullptr || storage::global_storage_worker == nullptr) {
    this->failed_ = true;
    this->callbacks_.call();
    return;
  }
  void *mem = RAMAllocator<StringList>(RAMAllocator<StringList>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr)
    return;
  this->found_ = new (mem) StringList(RAMAllocator<std::string>(RAMAllocator<std::string>::ALLOC_EXTERNAL));
  this->listing_ = true;
  auto entry = [](const storage::FileStat *stat, void *ctx) -> bool {
    auto *found = static_cast<StringList *>(ctx);
    if (!stat->is_dir && MediaPictures::is_picture_(stat->name))
      found->emplace_back(stat->name);
    return found->size() < LIST_MAX;
  };
  const storage::StorageError err = storage::global_storage_worker->async_list_dir(
      ps, (rel == nullptr || rel[0] == '\0') ? "/" : rel, entry, this->found_,
      [this](storage::StorageError e) {
        this->failed_ = e != storage::StorageError::STORAGE_ERROR_OK;
        this->listed_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->failed_ = true;
    this->listed_();
  }
}

void MediaPictures::listed_() {
  this->listing_ = false;
  if (this->found_ != nullptr) {
    std::sort(this->found_->begin(), this->found_->end());
    this->names_.assign(std::make_move_iterator(this->found_->begin()), std::make_move_iterator(this->found_->end()));
    this->found_->~StringList();
    RAMAllocator<StringList>(RAMAllocator<StringList>::ALLOC_EXTERNAL).deallocate(this->found_, 1);
    this->found_ = nullptr;
  }
  this->window_ = 0;
  this->index_ = -1;
  if (this->pending_name_[0] != '\0') {
    for (size_t i = 0; i < this->names_.size(); i++) {
      if (this->names_[i] == this->pending_name_)
        this->index_ = static_cast<int>(i);
    }
    this->pending_name_[0] = '\0';
  } else if (this->default_index_ >= 0 && !this->names_.empty()) {
    this->index_ = std::min<int>(this->default_index_, static_cast<int>(this->names_.size()) - 1);
  }
  ESP_LOGD(TAG, "%s: %u pictures", this->dir_, static_cast<unsigned>(this->names_.size()));
  this->fill_thumbnails_();
  if (this->index_ >= 0) {
    this->load_(this->index_);
  } else {
    this->callbacks_.call();
  }
}

void MediaPictures::load_(int index) {
  if (index < 0 || static_cast<size_t>(index) >= this->names_.size())
    return;
  this->index_ = index;
  if (this->image_ != nullptr) {
    this->image_->set_url(this->current_path());
    this->image_->update();
  }
  this->callbacks_.call();
  if (this->running_)
    this->schedule_();
}

size_t MediaPictures::slideshow_step_index(uint32_t ms) {
  size_t best = 0;
  for (size_t i = 1; i < SLIDESHOW_STEP_COUNT; i++) {
    const auto diff = [ms](size_t index) {
      const int64_t d = static_cast<int64_t>(SLIDESHOW_STEPS_S[index]) * 1000 - ms;
      return d < 0 ? -d : d;
    };
    if (diff(i) < diff(best))
      best = i;
  }
  return best;
}

const char *MediaPictures::slideshow_step_text(size_t index) {
  static const char *const TEXTS[] = {"3 s", "5 s", "8 s", "10 s", "15 s", "20 s", "30 s", "60 s"};
  static_assert(sizeof(TEXTS) / sizeof(TEXTS[0]) == SLIDESHOW_STEP_COUNT, "a text per step");
  return index < SLIDESHOW_STEP_COUNT ? TEXTS[index] : "";
}

void MediaPictures::show(int index) {
  this->defer([this, index]() { this->load_(index); });
}

void MediaPictures::show_path(const std::string &path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0 || path.size() > storage::STORAGE_VFS_PATH_MAX)
    return;
  const std::string dir = path.substr(0, slash);
  const std::string name = path.substr(slash + 1);
  if (dir == this->dir_) {
    for (size_t i = 0; i < this->names_.size(); i++) {
      if (this->names_[i] == name) {
        this->show(static_cast<int>(i));
        return;
      }
    }
  }
  memcpy(this->pending_name_, name.c_str(), name.size() + 1);
  this->open_directory(dir);
}

void MediaPictures::next() {
  this->defer([this]() {
    if (!this->names_.empty())
      this->load_(this->index_ < 0 ? 0 : static_cast<int>((this->index_ + 1) % this->names_.size()));
  });
}

void MediaPictures::previous() {
  this->defer([this]() {
    if (this->names_.empty())
      return;
    const int count = static_cast<int>(this->names_.size());
    this->load_(this->index_ <= 0 ? count - 1 : this->index_ - 1);
  });
}

void MediaPictures::schedule_() {
  this->set_timeout("slideshow", this->interval_ms_, [this]() {
    if (!this->running_ || this->names_.empty())
      return;
    this->load_(static_cast<int>((this->index_ + 1) % this->names_.size()));
  });
}

void MediaPictures::slideshow_start() {
  if (this->running_)
    return;
  this->running_ = true;
  this->schedule_();
  this->callbacks_.call();
}

void MediaPictures::slideshow_stop() {
  if (!this->running_)
    return;
  this->running_ = false;
  this->cancel_timeout("slideshow");
  this->callbacks_.call();
}

void MediaPictures::slideshow_toggle() {
  if (this->running_) {
    this->slideshow_stop();
  } else {
    this->slideshow_start();
  }
}

void MediaPictures::change_interval(uint32_t ms) {
  this->interval_ms_ = std::max<uint32_t>(ms, 500);
  if (this->running_)
    this->schedule_();
  this->callbacks_.call();
}

void MediaPictures::fill_thumbnails_() {
  for (size_t slot = 0; slot < this->thumbs_.size(); slot++) {
    online_image::OnlineImage *thumb = this->thumbs_[slot];
    const size_t index = this->window_ + slot;
    if (index < this->names_.size()) {
      thumb->set_url(std::string(this->dir_) + "/" + this->names_[index]);
      thumb->update();
    } else {
      thumb->release();
    }
  }
}

void MediaPictures::thumbnails_page(int step) {
  this->defer([this, step]() {
    const size_t page = this->thumbs_.size();
    if (page == 0 || this->names_.empty())
      return;
    if (step > 0 && this->window_ + page < this->names_.size()) {
      this->window_ += page;
    } else if (step < 0 && this->window_ > 0) {
      this->window_ = this->window_ > page ? this->window_ - page : 0;
    } else {
      return;
    }
    this->fill_thumbnails_();
    this->callbacks_.call();
  });
}

void MediaPictures::thumbnail_show(size_t slot) { this->show(static_cast<int>(this->window_ + slot)); }

void MediaPictures::close() {
  this->defer([this]() {
    this->slideshow_stop();
    if (this->image_ != nullptr)
      this->image_->release();
    for (auto *thumb : this->thumbs_)
      thumb->release();
    this->index_ = -1;
    this->callbacks_.call();
  });
}

}

#endif
