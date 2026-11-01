#pragma once

#include "esphome/core/defines.h"
#ifdef USE_MEDIA_PLAYER_PICTURES

#include "esphome/components/online_image/online_image.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <string>
#include <vector>

namespace esphome::media_player {

class MediaPictures : public Component {
 public:
  static MediaPictures *create();

  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_image(online_image::OnlineImage *image) { this->image_ = image; }
  void add_directory(const char *dir) { this->directories_.push_back(dir); }
  void set_slideshow_interval(uint32_t ms) { this->interval_ms_ = ms; }
  void set_default_index(int index) { this->default_index_ = index; }
  void add_thumbnail(online_image::OnlineImage *image) { this->thumbs_.push_back(image); }
  void add_on_picture_callback(std::function<void()> &&callback) { this->callbacks_.add(std::move(callback)); }
  Trigger<> *get_picture_trigger() { return &this->picture_trigger_; }

  void open_directory(const std::string &dir);
  void show_sources(std::vector<std::string> sources, std::vector<std::string> titles, size_t index);
  void show(int index);
  void show_path(const std::string &path);
  void next();
  void previous();
  void slideshow_start();
  void slideshow_stop();
  void slideshow_toggle();
  void change_interval(uint32_t ms);
  void thumbnails_page(int step);
  void thumbnail_show(size_t slot);
  void close();

  size_t image_count() const { return this->names_.size(); }
  int current_index() const { return this->index_; }
  std::string current_path() const;
  std::string current_title() const;
  const char *directory() const { return this->dir_; }
  bool slideshow_running() const { return this->running_; }
  uint32_t slideshow_interval() const { return this->interval_ms_; }
  bool listing_failed() const { return this->failed_; }
  size_t thumbnail_start() const { return this->window_; }
  static constexpr uint16_t SLIDESHOW_STEPS_S[] = {3, 5, 8, 10, 15, 20, 30, 60};
  static constexpr size_t SLIDESHOW_STEP_COUNT = sizeof(SLIDESHOW_STEPS_S) / sizeof(SLIDESHOW_STEPS_S[0]);
  static size_t slideshow_step_index(uint32_t ms);
  static const char *slideshow_step_text(size_t index);

 protected:
  static bool is_picture_(const char *name);
  void list_();
  void listed_();
  void load_(int index);
  void fill_thumbnails_();
  void schedule_();

  online_image::OnlineImage *image_{nullptr};
  std::vector<const char *> directories_;
  std::vector<online_image::OnlineImage *> thumbs_;
  std::vector<std::string, RAMAllocator<std::string>> names_{RAMAllocator<std::string>(RAMAllocator<std::string>::ALLOC_EXTERNAL)};
  std::vector<std::string, RAMAllocator<std::string>> titles_{RAMAllocator<std::string>(RAMAllocator<std::string>::ALLOC_EXTERNAL)};
  std::vector<std::string, RAMAllocator<std::string>> *found_{nullptr};
  char dir_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  char pending_name_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  int index_{-1};
  int default_index_{0};
  uint32_t interval_ms_{5000};
  size_t window_{0};
  bool running_{false};
  bool listing_{false};
  bool failed_{false};
  CallbackManager<void()> callbacks_;
  Trigger<> picture_trigger_;
};

template<typename... Ts> class PicturesShowAction : public Action<Ts...>, public Parented<MediaPictures> {
 public:
  TEMPLATABLE_VALUE(int, index)
  TEMPLATABLE_VALUE(std::string, path)
  void set_by_path(bool by_path) { this->by_path_ = by_path; }
  void play(const Ts &...x) override {
    if (this->by_path_) {
      this->parent_->show_path(this->path_.value(x...));
    } else {
      this->parent_->show(this->index_.value(x...));
    }
  }

 protected:
  bool by_path_{false};
};

template<typename... Ts> class PicturesOpenAction : public Action<Ts...>, public Parented<MediaPictures> {
 public:
  TEMPLATABLE_VALUE(std::string, directory)
  void play(const Ts &...x) override { this->parent_->open_directory(this->directory_.value(x...)); }
};

template<typename... Ts> class PicturesIntervalAction : public Action<Ts...>, public Parented<MediaPictures> {
 public:
  TEMPLATABLE_VALUE(uint32_t, interval)
  void play(const Ts &...x) override { this->parent_->change_interval(this->interval_.value(x...)); }
};

template<typename... Ts> class PicturesThumbnailAction : public Action<Ts...>, public Parented<MediaPictures> {
 public:
  TEMPLATABLE_VALUE(int, slot)
  void play(const Ts &...x) override { this->parent_->thumbnail_show(static_cast<size_t>(this->slot_.value(x...))); }
};

template<typename... Ts> class PicturesCommandAction : public Action<Ts...>, public Parented<MediaPictures> {
 public:
  void set_command(uint8_t command) { this->command_ = command; }
  void play(const Ts &...x) override {
    switch (this->command_) {
      case 0:
        this->parent_->next();
        break;
      case 1:
        this->parent_->previous();
        break;
      case 2:
        this->parent_->slideshow_start();
        break;
      case 3:
        this->parent_->slideshow_stop();
        break;
      case 4:
        this->parent_->slideshow_toggle();
        break;
      case 5:
        this->parent_->thumbnails_page(1);
        break;
      case 6:
        this->parent_->thumbnails_page(-1);
        break;
      default:
        this->parent_->close();
        break;
    }
  }

 protected:
  uint8_t command_{0};
};

}

#endif
