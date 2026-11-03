#pragma once

#include "esphome/core/defines.h"
#ifdef USE_MEDIA_PLAYER_QUEUE

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "media_player.h"
#ifdef USE_MEDIA_PLAYER_QUEUE_RELAY
#include "esphome/components/relay_link/relay_link.h"
#endif

#include <string>
#include <vector>

namespace esphome::media_player {

class MediaQueue : public Component {
 public:
  enum Repeat : uint8_t { REPEAT_OFF, REPEAT_ONE, REPEAT_ALL };

  static MediaQueue *create();

  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_player(MediaPlayer *player) { this->player_ = player; }
#ifdef USE_MEDIA_PLAYER_QUEUE_RELAY
  void set_relay(relay_link::RelayLink *relay) { this->relay_ = relay; }
#endif
  void set_max_entries(size_t max) { this->max_ = max; }
  void add_on_change_callback(std::function<void()> &&callback) { this->callbacks_.add(std::move(callback)); }

  void add(const std::string &path, const std::string &title);
  void clear();
  void play(size_t index);
  void play_or_resume();
  void next();
  void previous();
  void stop();
  void set_shuffle(bool shuffle);
  void set_repeat(uint8_t repeat);
#ifdef USE_STORAGE
  void play_folder(const std::string &path, std::vector<std::string> extensions);
#endif

  size_t size() const { return this->entries_.size(); }
  std::string title(size_t index) const;
  std::string path(size_t index) const { return index < this->entries_.size() ? this->entries_[index].path : ""; }
  int index() const { return this->current_ ? static_cast<int>(this->order_[this->pos_]) : -1; }
  uint32_t generation() const { return this->generation_; }
  bool shuffle() const { return this->shuffle_; }
  uint8_t repeat() const { return this->repeat_; }
  bool active() const { return this->playing_; }

 protected:
  struct Entry {
    std::string path;
    std::string title;
  };
  void start_(size_t pos);
  void advance_(bool wrap);
  void reorder_();
#ifdef USE_STORAGE
  struct Folder {
    std::string dir;
    std::string start;
    std::vector<std::string> extensions;
    std::vector<std::string, RAMAllocator<std::string>> names{
        RAMAllocator<std::string>(RAMAllocator<std::string>::ALLOC_EXTERNAL)};
    size_t max;
  };
  void folder_listed_(bool ok);
  Folder *folder_{nullptr};
#endif

  MediaPlayer *player_{nullptr};
#ifdef USE_MEDIA_PLAYER_QUEUE_RELAY
  relay_link::RelayLink *relay_{nullptr};
#endif
  std::vector<Entry, RAMAllocator<Entry>> entries_{RAMAllocator<Entry>(RAMAllocator<Entry>::ALLOC_EXTERNAL)};
  std::vector<uint16_t, RAMAllocator<uint16_t>> order_{RAMAllocator<uint16_t>(RAMAllocator<uint16_t>::ALLOC_EXTERNAL)};
  size_t max_{500};
  size_t pos_{0};
  bool current_{false};
  bool playing_{false};
  bool started_{false};
  bool shuffle_{false};
  uint8_t repeat_{REPEAT_OFF};
  uint32_t started_ms_{0};
  uint32_t generation_{0};
  MediaPlayerState last_{MEDIA_PLAYER_STATE_NONE};
  CallbackManager<void()> callbacks_;
};

template<typename... Ts> class QueueAddAction : public Action<Ts...>, public Parented<MediaQueue> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::string, title)
  void play(const Ts &...x) override { this->parent_->add(this->path_.value(x...), this->title_.value(x...)); }
};

template<typename... Ts> class QueuePlayAction : public Action<Ts...>, public Parented<MediaQueue> {
 public:
  TEMPLATABLE_VALUE(int, index)
  void play(const Ts &...x) override { this->parent_->play(static_cast<size_t>(this->index_.value(x...))); }
};

template<typename... Ts> class QueueModeAction : public Action<Ts...>, public Parented<MediaQueue> {
 public:
  TEMPLATABLE_VALUE(bool, shuffle)
  TEMPLATABLE_VALUE(int, repeat)
  void set_kind(uint8_t kind) { this->kind_ = kind; }
  void play(const Ts &...x) override {
    if (this->kind_ == 0) {
      this->parent_->set_shuffle(this->shuffle_.value(x...));
    } else {
      this->parent_->set_repeat(static_cast<uint8_t>(this->repeat_.value(x...)));
    }
  }

 protected:
  uint8_t kind_{0};
};

template<typename... Ts> class QueueCommandAction : public Action<Ts...>, public Parented<MediaQueue> {
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
        this->parent_->play_or_resume();
        break;
      case 3:
        this->parent_->stop();
        break;
      default:
        this->parent_->clear();
        break;
    }
  }

 protected:
  uint8_t command_{0};
};

}

#endif
