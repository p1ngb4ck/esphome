// Keyboard layouts on the keyboards (keyboard_layouts:): svp_relay lists them and hands out each one's
// table (hid::parse_layout()); a layout used is kept in the directory and read from there after. Main
// loop, the files through the storage worker (each step after the last one's completion, the next one
// deferred to the loop), svp_relay through the channel task.

#include "simple_video_player.h"

#ifdef SVP_KEYBOARD

#include "esphome/core/log.h"

#include <cctype>
#include <cstring>
#include <strings.h>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.keyboard";

static constexpr const char *KBD_SUFFIX = ".kbl";

void SimpleVideoPlayer::keyboard_setup_() {
  this->kbd_list_ = reinterpret_cast<char *>(this->pool_.alloc(KBD_LIST_BYTES));
  this->kbd_entries_ = reinterpret_cast<KbdEntry *>(this->pool_.alloc(KBD_MAX * sizeof(KbdEntry)));
  this->kbd_slot_[0] = this->pool_.alloc(KBD_LAYOUT_BYTES);
  this->kbd_slot_[1] = this->pool_.alloc(KBD_LAYOUT_BYTES);
  if (this->kbd_list_ == nullptr || this->kbd_entries_ == nullptr || this->kbd_slot_[0] == nullptr ||
      this->kbd_slot_[1] == nullptr) {
    ESP_LOGE(TAG, "Keyboard layouts disabled: no room in the PSRAM pool");
    return;
  }
  for (hid::HIDInput *input : this->kbd_inputs_)
    this->kbd_own_.push_back(input->get_layout());
  if (storage::global_storage_registry != nullptr) {
    // Its storage may come later (a card put in): a layout that could not be had is tried again.
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) {
      if (this->kbd_failed_ && this->kbd_wanted_ != this->kbd_selected_) {
        this->kbd_pending_ = true;
        this->keyboard_next_();
      }
    });
  }
  this->kbd_ready_ = true;
  this->keyboard_next_();
}

void SimpleVideoPlayer::keyboard_loop_() {
#ifdef SVP_CHANNEL_LIST
  if (this->kbd_list_done_.exchange(false, std::memory_order_acquire)) {
    if (!this->kbd_list_ok_) {
      this->keyboard_list_local_(true);
    } else {
      // "name\tdescription\n" lines, cut into strings where they are.
      char *text = this->kbd_list_;
      text[this->kbd_list_len_] = '\0';
      size_t count = 0;
      for (char *line = text; *line != '\0' && count < KBD_MAX;) {
        char *end = std::strchr(line, '\n');
        if (end != nullptr)
          *end = '\0';
        char *tab = std::strchr(line, '\t');
        if (tab != nullptr)
          *tab = '\0';
        if (*line != '\0') {
          KbdEntry &entry = this->kbd_entries_[count++];
          entry.name = static_cast<uint16_t>(line - text);
          entry.description = static_cast<uint16_t>((tab != nullptr ? tab + 1 : line + std::strlen(line)) - text);
          entry.kept = false;
        }
        if (end == nullptr)
          break;
        line = end + 1;
      }
      this->kbd_count_ = count;
      this->kbd_list_remote_ = true;
      this->keyboard_list_local_(false);
    }
  }
  if (this->kbd_get_done_.exchange(false, std::memory_order_acquire) && this->kbd_step_ == KBD_FETCHING) {
    if (this->kbd_get_ok_ && this->keyboard_apply_(this->kbd_get_len_)) {
      this->keyboard_keep_(this->kbd_get_len_);
    } else {
      ESP_LOGW(TAG, "Layout %s not had from svp_relay", this->kbd_loading_name_.c_str());
      this->keyboard_done_(false);
    }
  }
#endif
}

void SimpleVideoPlayer::keyboard_list_fetch() {
  if (!this->kbd_ready_ || this->kbd_list_busy_)
    return;
  this->kbd_list_busy_ = true;
  this->kbd_count_ = 0;
  this->kbd_listed_ = false;
  this->kbd_list_remote_ = false;
#ifdef SVP_CHANNEL_LIST
  if (this->channel_q_ != nullptr && this->channel_request_(CHANNEL_CMD_KEYBOARD_LIST, 0))
    return;
#endif
  this->keyboard_list_local_(true);
}

void SimpleVideoPlayer::keyboard_list_local_(bool build) {
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                 ? storage::global_storage_registry->resolve_path(this->kbd_dir_.c_str(), &rel)
                                 : nullptr;
  // Not there, or the worker is on it on its task: nothing listed from it.
  if (ps == nullptr ||
      (storage::global_storage_worker != nullptr && storage::global_storage_worker->has_active_task_io(ps))) {
    if (build)
      this->kbd_count_ = 0;
    this->keyboard_listed_();
    return;
  }
  struct Context {
    SimpleVideoPlayer *p;
    bool build;
    size_t used;
  } context{this, build, 0};
  if (build)
    this->kbd_count_ = 0;
  ps->list_dir((rel == nullptr || rel[0] == '\0') ? "/" : rel,
               [](const storage::FileStat *entry, void *ctx) -> bool {
                 auto *c = static_cast<Context *>(ctx);
                 SimpleVideoPlayer *p = c->p;
                 const size_t len = std::strlen(entry->name);
                 const size_t suffix = std::strlen(KBD_SUFFIX);
                 if (entry->is_dir || len <= suffix || strcasecmp(entry->name + len - suffix, KBD_SUFFIX) != 0)
                   return true;
                 const size_t name_len = len - suffix;
                 if (!c->build) {
                   for (size_t i = 0; i < p->kbd_count_; i++) {
                     const char *name = p->kbd_list_ + p->kbd_entries_[i].name;
                     if (std::strlen(name) == name_len && std::strncmp(name, entry->name, name_len) == 0)
                       p->kbd_entries_[i].kept = true;
                   }
                   return true;
                 }
                 // The name and an empty description.
                 if (p->kbd_count_ == KBD_MAX || c->used + name_len + 2 > KBD_LIST_BYTES)
                   return false;
                 std::memcpy(p->kbd_list_ + c->used, entry->name, name_len);
                 p->kbd_list_[c->used + name_len] = '\0';
                 p->kbd_list_[c->used + name_len + 1] = '\0';
                 p->kbd_entries_[p->kbd_count_++] = {static_cast<uint16_t>(c->used),
                                                     static_cast<uint16_t>(c->used + name_len + 1), true};
                 c->used += name_len + 2;
                 return true;
               },
               &context);
  this->keyboard_listed_();
}

void SimpleVideoPlayer::keyboard_listed_() {
  this->kbd_listed_ = true;
  this->kbd_list_busy_ = false;
  this->kbd_callbacks_.call();
}

const char *SimpleVideoPlayer::keyboard_name(size_t index) const {
  return index < this->kbd_count_ ? this->kbd_list_ + this->kbd_entries_[index].name : "";
}

const char *SimpleVideoPlayer::keyboard_description(size_t index) const {
  return index < this->kbd_count_ ? this->kbd_list_ + this->kbd_entries_[index].description : "";
}

bool SimpleVideoPlayer::keyboard_kept(size_t index) const {
  return index < this->kbd_count_ && this->kbd_entries_[index].kept;
}

int SimpleVideoPlayer::keyboard_index(const std::string &name) const {
  for (size_t i = 0; i < this->kbd_count_; i++) {
    if (name == this->kbd_list_ + this->kbd_entries_[i].name)
      return static_cast<int>(i);
  }
  return -1;
}

// ASCII any case: whether text holds query (lower case already).
static bool holds(const char *text, const std::string &query) {
  const size_t n = query.size();
  for (const char *at = text; *at != '\0'; at++) {
    size_t i = 0;
    while (i < n && at[i] != '\0' && std::tolower(static_cast<unsigned char>(at[i])) == query[i])
      i++;
    if (i == n)
      return true;
  }
  return false;
}

size_t SimpleVideoPlayer::keyboard_find(const std::string &query, int *out, size_t max) const {
  std::string lower = query;
  for (char &c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  size_t total = 0;
  for (size_t i = 0; i < this->kbd_count_; i++) {
    const KbdEntry &entry = this->kbd_entries_[i];
    const bool match = lower.empty() ? entry.kept
                                     : holds(this->kbd_list_ + entry.name, lower) ||
                                           holds(this->kbd_list_ + entry.description, lower);
    if (!match)
      continue;
    if (total < max)
      out[total] = static_cast<int>(i);
    total++;
  }
  return total;
}

void SimpleVideoPlayer::keyboard_select(const std::string &name) {
  this->kbd_wanted_.assign(name.c_str());
  this->kbd_pending_ = true;
  this->keyboard_next_();
}

void SimpleVideoPlayer::keyboard_next_() {
  if (!this->kbd_ready_ || this->kbd_step_ != KBD_IDLE || !this->kbd_pending_)
    return;
  this->kbd_pending_ = false;
  if (this->kbd_wanted_ == this->kbd_selected_) {
    this->kbd_failed_ = false;
    this->kbd_callbacks_.call();
    return;
  }
  if (this->kbd_wanted_.empty()) {
    for (size_t i = 0; i < this->kbd_inputs_.size(); i++)
      this->kbd_inputs_[i]->set_layout(this->kbd_own_[i]);
    this->kbd_active_ = -1;
    this->kbd_selected_.clear();
    ESP_LOGI(TAG, "The keyboards' own layouts");
    this->keyboard_done_(true);
    return;
  }
  this->kbd_loading_name_ = this->kbd_wanted_;
  this->kbd_loading_ = this->kbd_active_ == 0 ? 1 : 0;
  this->kbd_len_ = 0;
  // Its file first.
  const std::string path = this->kbd_dir_ + "/" + this->kbd_loading_name_.c_str() + KBD_SUFFIX;
  const char *rel = nullptr;
  storage::StorageWorker *worker = storage::global_storage_worker;
  storage::PathStorage *ps = storage::global_storage_registry != nullptr && worker != nullptr
                                 ? storage::global_storage_registry->resolve_path(path.c_str(), &rel)
                                 : nullptr;
  if (ps != nullptr) {
    this->kbd_step_ = KBD_READING;
    const storage::StorageError err = worker->begin_read(ps, rel, &this->kbd_handle_, [this](storage::StorageError e) {
      if (e == storage::StorageError::STORAGE_ERROR_OK) {
        this->defer([this]() { this->keyboard_read_(); });
      } else {
        this->defer([this]() { this->keyboard_fetch_(); });
      }
    });
    if (err == storage::StorageError::STORAGE_ERROR_OK)
      return;
  }
  this->keyboard_fetch_();
}

void SimpleVideoPlayer::keyboard_read_() {
  const size_t room = KBD_LAYOUT_BYTES - this->kbd_len_;
  if (room == 0) {
    this->keyboard_read_end_(false);  // larger than any layout
    return;
  }
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->kbd_handle_, this->kbd_slot_[this->kbd_loading_] + this->kbd_len_, room, &this->kbd_got_,
      [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          this->defer([this]() { this->keyboard_read_end_(false); });
        } else if (this->kbd_got_ == 0) {
          this->defer([this]() { this->keyboard_read_end_(true); });
        } else {
          this->kbd_len_ += this->kbd_got_;
          this->defer([this]() { this->keyboard_read_(); });
        }
      });
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->keyboard_read_(); });
  } else if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->keyboard_read_end_(false);
  }
}

void SimpleVideoPlayer::keyboard_read_end_(bool ok) {
  const storage::StorageError closed =
      storage::global_storage_worker->end_read(this->kbd_handle_, [this, ok](storage::StorageError) {
        this->defer([this, ok]() {
          if (ok && this->keyboard_apply_(this->kbd_len_)) {
            this->keyboard_done_(true);
          } else {
            ESP_LOGW(TAG, "%s/%s%s is not a layout", this->kbd_dir_.c_str(), this->kbd_loading_name_.c_str(),
                     KBD_SUFFIX);
            this->keyboard_fetch_();
          }
        });
      });
  if (closed == storage::StorageError::STORAGE_ERROR_NOT_READY)
    this->defer([this, ok]() { this->keyboard_read_end_(ok); });
}

void SimpleVideoPlayer::keyboard_fetch_() {
#ifdef SVP_CHANNEL_LIST
  this->kbd_step_ = KBD_FETCHING;
  if (this->channel_q_ != nullptr && this->channel_request_(CHANNEL_CMD_KEYBOARD_GET, 0))
    return;
#endif
  ESP_LOGW(TAG, "Layout %s not in %s", this->kbd_loading_name_.c_str(), this->kbd_dir_.c_str());
  this->keyboard_done_(false);
}

bool SimpleVideoPlayer::keyboard_apply_(size_t len) {
  const uint8_t slot = this->kbd_loading_;
  if (!hid::parse_layout(this->kbd_slot_[slot], len, &this->kbd_layout_[slot]))
    return false;
  for (hid::HIDInput *input : this->kbd_inputs_)
    input->set_layout(&this->kbd_layout_[slot]);
  this->kbd_active_ = static_cast<int8_t>(slot);
  this->kbd_selected_ = this->kbd_loading_name_;
  ESP_LOGI(TAG, "Layout %s", this->kbd_selected_.c_str());
  return true;
}

// The layout had from svp_relay into its file (the directory made if need be); it is on the keyboards
// meanwhile, its slot not written to till the next one.
void SimpleVideoPlayer::keyboard_keep_(size_t len) {
  storage::StorageWorker *worker = storage::global_storage_worker;
  const char *dir_rel = nullptr;
  storage::PathStorage *dir = storage::global_storage_registry != nullptr && worker != nullptr
                                  ? storage::global_storage_registry->resolve_path(this->kbd_dir_.c_str(), &dir_rel)
                                  : nullptr;
  if (dir == nullptr || worker->has_active_task_io(dir)) {
    this->keyboard_done_(true);
    return;
  }
  if (dir_rel != nullptr && dir_rel[0] != '\0')
    dir->mkdir(dir_rel);  // already there: an error, and nothing to do
  const std::string path = this->kbd_dir_ + "/" + this->kbd_selected_.c_str() + KBD_SUFFIX;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path.c_str(), &rel);
  if (ps == nullptr) {
    this->keyboard_done_(true);
    return;
  }
  this->kbd_step_ = KBD_WRITING;
  this->kbd_len_ = len;
  this->kbd_kept_ = true;
  const storage::StorageError err = worker->begin_write(ps, rel, &this->kbd_handle_, [this](storage::StorageError e) {
    if (e != storage::StorageError::STORAGE_ERROR_OK) {
      ESP_LOGW(TAG, "Layout %s not kept: %s", this->kbd_selected_.c_str(), storage::error_to_string(e));
      this->defer([this]() { this->keyboard_done_(true); });
      return;
    }
    this->defer([this]() { this->keyboard_write_(); });
  });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->keyboard_done_(true);
}

void SimpleVideoPlayer::keyboard_write_() {
  auto close = [this]() {
    const storage::StorageError err =
        storage::global_storage_worker->end_write(this->kbd_handle_, [this](storage::StorageError e) {
          const bool kept = this->kbd_kept_ && e == storage::StorageError::STORAGE_ERROR_OK;
          this->defer([this, kept]() {
            if (kept) {
              const int index = this->keyboard_index(this->kbd_selected_.c_str());
              if (index >= 0)
                this->kbd_entries_[index].kept = true;
            } else {
              ESP_LOGW(TAG, "Layout %s not kept", this->kbd_selected_.c_str());
            }
            this->keyboard_done_(true);
          });
        });
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->defer([this]() { this->keyboard_write_(); });
  };
  if (this->kbd_len_ == 0) {
    close();
    return;
  }
  const storage::StorageError err = storage::global_storage_worker->write_chunk(
      this->kbd_handle_, this->kbd_slot_[this->kbd_active_], this->kbd_len_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK)
          this->kbd_kept_ = false;
        this->kbd_len_ = 0;  // written: what is left is to close it
        this->defer([this]() { this->keyboard_write_(); });
      });
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->keyboard_write_(); });
  } else if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->kbd_kept_ = false;
    close();
  }
}

void SimpleVideoPlayer::keyboard_done_(bool ok) {
  this->kbd_step_ = KBD_IDLE;
  this->kbd_failed_ = !ok;
  this->kbd_callbacks_.call();
  this->keyboard_next_();
}

}  // namespace esphome::simple_video_player

#endif  // SVP_KEYBOARD
