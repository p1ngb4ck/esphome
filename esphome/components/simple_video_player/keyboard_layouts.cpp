// Keyboard layouts on the keyboards (keyboard_layouts:): svp_relay lists them and hands out each one's
// table (hid::parse_layout()); a layout used is kept in the directory and read from there after. Each
// device behind a keyboard (hid::InputDevice) types with the layout given it, else with its keyboard's
// standard one. Main loop, the files through the storage worker (each step after the last one's
// completion, the next one deferred to the loop), svp_relay through the channel task.

#include "simple_video_player.h"

#ifdef SVP_KEYBOARD

#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.keyboard";

static constexpr const char *KBD_SUFFIX = ".kbl";

// Into a fixed field: what does not fit is cut.
static void copy_field(char *out, size_t size, const std::string &text) {
  std::snprintf(out, size, "%s", text.c_str());
}

void SimpleVideoPlayer::add_keyboard(hid::HIDInput *input, const std::string &name, const std::string &layout) {
  this->kbd_inputs_.push_back(KbdInput{input, name, layout, nullptr});
}

void SimpleVideoPlayer::keyboard_setup_() {
  this->kbd_list_ = reinterpret_cast<char *>(this->pool_.alloc(KBD_LIST_BYTES));
  this->kbd_entries_ = reinterpret_cast<KbdEntry *>(this->pool_.alloc(KBD_MAX * sizeof(KbdEntry)));
  this->kbd_assign_ = reinterpret_cast<KbdAssignTable *>(this->pool_.alloc(sizeof(KbdAssignTable)));
  this->kbd_assign_copy_ = reinterpret_cast<KbdAssignTable *>(this->pool_.alloc(sizeof(KbdAssignTable)));
  bool ok = this->kbd_list_ != nullptr && this->kbd_entries_ != nullptr && this->kbd_assign_ != nullptr &&
            this->kbd_assign_copy_ != nullptr;
  for (KbdSlot &slot : this->kbd_slots_) {
    slot = KbdSlot{};
    slot.data = this->pool_.alloc(KBD_LAYOUT_BYTES);
    ok = ok && slot.data != nullptr;
  }
  if (!ok) {
    ESP_LOGE(TAG, "Keyboard layouts disabled: no room in the PSRAM pool");
    return;
  }
  std::memset(this->kbd_assign_, 0, sizeof(KbdAssignTable));
  this->kbd_pref_ = global_preferences->make_preference<KbdAssignTable>(fnv1_hash("simple_video_player.keyboards"));
  for (KbdInput &input : this->kbd_inputs_) {
    input.own = input.input->get_layout();
    // A device came or went: it gets its layout; a USB one is remembered.
    const auto index = static_cast<uint8_t>(&input - this->kbd_inputs_.data());
    input.input->add_on_devices_callback([this, index]() {
      std::vector<hid::InputDevice> devices;
      this->kbd_inputs_[index].input->list_devices(devices);
      bool added = false;
      for (const auto &device : devices) {
        if (device.key.rfind("usb:", 0) != 0 || this->keyboard_assigned_(device.key) != nullptr)
          continue;
        for (KbdAssign &entry : this->kbd_assign_->entries) {
          if (entry.key[0] == '\0') {
            copy_field(entry.key, sizeof(entry.key), device.key);
            copy_field(entry.name, sizeof(entry.name), device.name);
            entry.layout[0] = '\0';
            entry.input = index;
            added = true;
            break;
          }
        }
      }
      if (added)
        this->keyboard_save_();
      this->keyboard_resolve_();
      this->kbd_callbacks_.call();
    });
  }
  if (storage::global_storage_registry != nullptr) {
    // Its storage may come later (a card put in): the layouts that could not be had are tried again.
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) {
      if (!this->kbd_table_loaded_)
        this->keyboard_table_load_();
      for (KbdSlot &slot : this->kbd_slots_) {
        if (slot.state == KBD_FAILED)
          slot.state = KBD_EMPTY;
      }
      this->keyboard_resolve_();
    });
  }
  this->kbd_ready_ = true;
  this->keyboard_resolve_();
  this->keyboard_table_load_();
#ifdef SVP_KEYBOARD_ENTITIES
  this->kbd_entity_labels_ =
      reinterpret_cast<char (*)[KBD_ENTITY_LABEL]>(this->pool_.alloc(KBD_ENTITY_MAX * KBD_ENTITY_LABEL));
  this->kbd_entity_keys_ =
      reinterpret_cast<char (*)[sizeof(KbdAssign::key)]>(this->pool_.alloc(KBD_ENTITY_MAX * sizeof(KbdAssign::key)));
  if (this->kbd_entity_labels_ == nullptr || this->kbd_entity_keys_ == nullptr) {
    ESP_LOGE(TAG, "Keyboard device select disabled: no room in the PSRAM pool");
    return;
  }
  this->kbd_callbacks_.add([this]() { this->keyboard_entities_(); });
  this->keyboard_entities_();
#endif
}

#ifdef SVP_KEYBOARD_ENTITIES
#ifdef USE_SELECT
void KeyboardDeviceSelect::control(size_t index) { this->parent_->keyboard_entity_pick(index); }
#endif

void SimpleVideoPlayer::keyboard_entity_pick(size_t index) {
  if (index >= this->kbd_entity_count_)
    return;
  std::memcpy(this->kbd_entity_pick_, this->kbd_entity_keys_[index], sizeof(this->kbd_entity_pick_));
  this->keyboard_entities_();
}

// The select's options: each keyboard's standard ("name (standard)") and each device of theirs (its
// name, numbered when another has it too); the sensor: the layout of the one picked ("standard": its
// keyboard's, "own": a standard's configured one).
void SimpleVideoPlayer::keyboard_entities_() {
  if (this->kbd_entity_labels_ == nullptr)
    return;
  const auto devices = this->keyboard_devices();
  size_t count = 0;
  size_t picked = 0;
  bool found = false;
  for (const auto &device : devices) {
    if (count >= KBD_ENTITY_MAX)
      break;
    char *label = this->kbd_entity_labels_[count];
    const bool standard = !device.key.empty() && device.key[0] == '@';
    std::snprintf(label, KBD_ENTITY_LABEL, standard ? "%s (standard)" : "%s", device.name.c_str());
    for (unsigned n = 2;; n++) {
      bool taken = false;
      for (size_t i = 0; i < count && !taken; i++)
        taken = std::strcmp(this->kbd_entity_labels_[i], label) == 0;
      if (!taken)
        break;
      std::snprintf(label, KBD_ENTITY_LABEL, standard ? "%s (standard) %u" : "%s %u", device.name.c_str(), n);
    }
    copy_field(this->kbd_entity_keys_[count], sizeof(KbdAssign::key), device.key);
    if (!found && std::strcmp(this->kbd_entity_keys_[count], this->kbd_entity_pick_) == 0) {
      picked = count;
      found = true;
    }
    count++;
  }
  this->kbd_entity_count_ = count;
  if (count == 0)
    return;
  if (!found)
    std::memcpy(this->kbd_entity_pick_, this->kbd_entity_keys_[0], sizeof(this->kbd_entity_pick_));
#ifdef USE_SELECT
  if (this->kbd_select_ != nullptr) {
    FixedVector<const char *> options;
    options.init(count);
    for (size_t i = 0; i < count; i++)
      options.push_back(this->kbd_entity_labels_[i]);
    this->kbd_select_->traits.set_options(options);
    this->kbd_select_->publish_state(picked);
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->kbd_layout_sensor_ != nullptr) {
    const auto &device = devices[picked];
    const bool standard = device.key[0] == '@';
    this->kbd_layout_sensor_->publish_state(!device.layout.empty() ? device.layout
                                            : standard            ? std::string("own")
                                                                  : std::string("standard"));
  }
#endif
}
#endif

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
    if (this->kbd_get_ok_ && this->keyboard_loaded_(this->kbd_get_len_)) {
      this->keyboard_keep_(this->kbd_get_len_);
    } else {
      ESP_LOGW(TAG, "Layout %s not had from svp_relay", this->kbd_loading_name_.c_str());
      this->keyboard_done_(false);
    }
  }
#endif
}

void SimpleVideoPlayer::keyboard_set_directory(const std::string &path) {
  const std::string &dir = path.empty() ? this->kbd_dir_default_ : path;
  if (dir == this->kbd_dir_)
    return;
  this->kbd_dir_ = dir;
  ESP_LOGI(TAG, "Layouts in %s", dir.c_str());
  if (!this->kbd_ready_)
    return;
  for (KbdSlot &slot : this->kbd_slots_) {
    if (slot.state == KBD_FAILED)
      slot.state = KBD_EMPTY;
  }
  if (this->kbd_listed_) {
    for (size_t i = 0; i < this->kbd_count_; i++)
      this->kbd_entries_[i].kept = false;
    this->keyboard_list_local_(!this->kbd_list_remote_);
  }
  this->keyboard_resolve_();
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

std::string SimpleVideoPlayer::keyboard_label(const std::string &name) const {
  if (name.empty())
    return name;
  const int index = this->keyboard_index(name);
  const char *description = index >= 0 ? this->keyboard_description(index) : "";
  return description[0] != '\0' ? std::string(description) : name;
}

const SimpleVideoPlayer::KbdAssign *SimpleVideoPlayer::keyboard_assigned_(const std::string &key) const {
  if (this->kbd_assign_ == nullptr || key.empty())
    return nullptr;
  char cut[sizeof(KbdAssign::key)];
  copy_field(cut, sizeof(cut), key);
  for (const KbdAssign &entry : this->kbd_assign_->entries) {
    if (entry.key[0] != '\0' && std::strcmp(entry.key, cut) == 0)
      return &entry;
  }
  return nullptr;
}

std::string SimpleVideoPlayer::keyboard_standard_(size_t input) const {
  const KbdAssign *entry = this->keyboard_assigned_("@" + this->kbd_inputs_[input].name);
  return entry != nullptr ? std::string(entry->layout) : this->kbd_inputs_[input].layout;
}

std::vector<SimpleVideoPlayer::KeyboardDevice> SimpleVideoPlayer::keyboard_devices() const {
  std::vector<KeyboardDevice> out;
  for (size_t i = 0; i < this->kbd_inputs_.size(); i++) {
    const KbdInput &input = this->kbd_inputs_[i];
    const KbdAssign *standard = this->keyboard_assigned_("@" + input.name);
    out.push_back(KeyboardDevice{"@" + input.name, input.name, static_cast<uint8_t>(i), true,
                                 standard != nullptr ? std::string(standard->layout) : input.layout});
    std::vector<hid::InputDevice> devices;
    input.input->list_devices(devices);
    for (const auto &device : devices) {
      const KbdAssign *entry = this->keyboard_assigned_(device.key);
      out.push_back(KeyboardDevice{device.key, device.name, static_cast<uint8_t>(i), device.connected,
                                   entry != nullptr ? std::string(entry->layout) : std::string()});
    }
  }
  // The USB devices seen before and not there now: under the keyboard they were seen behind.
  if (this->kbd_assign_ != nullptr) {
    for (const KbdAssign &entry : this->kbd_assign_->entries) {
      if (std::strncmp(entry.key, "usb:", 4) != 0 || entry.input >= this->kbd_inputs_.size())
        continue;
      bool listed = false;
      for (const auto &device : out)
        listed = listed || device.key.compare(0, sizeof(entry.key) - 1, entry.key) == 0;
      if (!listed)
        out.push_back(KeyboardDevice{entry.key, entry.name, entry.input, false, entry.layout});
    }
  }
  return out;
}

bool SimpleVideoPlayer::keyboard_assign(const std::string &key, const std::string &layout) {
  if (this->kbd_assign_ == nullptr || key.empty())
    return false;
  KbdAssign *entry = const_cast<KbdAssign *>(this->keyboard_assigned_(key));
  std::string name;
  if (entry == nullptr) {
    if (key[0] == '@') {
      name = key.substr(1);
      bool known = false;
      for (const KbdInput &input : this->kbd_inputs_)
        known = known || input.name == name;
      if (!known)
        return false;
    } else {
      std::vector<hid::InputDevice> devices;
      for (const KbdInput &input : this->kbd_inputs_)
        input.input->list_devices(devices);
      bool known = false;
      for (const auto &device : devices) {
        if (device.key == key) {
          known = true;
          name = device.name;
        }
      }
      if (!known)
        return false;
    }
    for (KbdAssign &free : this->kbd_assign_->entries) {
      if (free.key[0] == '\0') {
        entry = &free;
        break;
      }
    }
    if (entry == nullptr) {
      ESP_LOGW(TAG, "No room to keep the layout of %s", key.c_str());
      return false;
    }
    copy_field(entry->key, sizeof(entry->key), key);
    copy_field(entry->name, sizeof(entry->name), name);
  }
  copy_field(entry->layout, sizeof(entry->layout), layout);
  // Nothing left to keep of a Bluetooth device or a standard given none.
  if (layout.empty() && std::strncmp(entry->key, "usb:", 4) != 0)
    std::memset(entry, 0, sizeof(KbdAssign));
  ESP_LOGI(TAG, "%s: layout %s", key.c_str(), layout.empty() ? "(standard)" : layout.c_str());
  for (KbdSlot &slot : this->kbd_slots_) {
    if (slot.state == KBD_FAILED && layout == slot.name)
      slot.state = KBD_EMPTY;  // asked for again: tried again
  }
  this->keyboard_save_();
  this->keyboard_resolve_();
  this->kbd_callbacks_.call();
  return true;
}

bool SimpleVideoPlayer::keyboard_assign_named(const std::string &device, const std::string &given) {
  // "standard" / "own" (the layout sensor's words): none of its own.
  const std::string layout = given == "standard" || given == "own" ? std::string() : given;
#ifdef SVP_KEYBOARD_ENTITIES
  for (size_t i = 0; i < this->kbd_entity_count_; i++) {
    if (device == this->kbd_entity_labels_[i])
      return this->keyboard_assign(this->kbd_entity_keys_[i], layout);
  }
#endif
  for (const auto &known : this->keyboard_devices()) {
    if (known.key == device || known.name == device || (known.key[0] == '@' && known.key.substr(1) == device))
      return this->keyboard_assign(known.key, layout);
  }
  ESP_LOGW(TAG, "No keyboard or device %s", device.c_str());
  return false;
}

std::string SimpleVideoPlayer::keyboard_summary() const {
  std::string out;
  for (const auto &device : this->keyboard_devices()) {
    if (!out.empty())
      out += '\n';
    std::string layout = device.layout;
    if (layout.empty())
      layout = device.key[0] == '@' ? "own" : "standard";
    out += (device.key[0] == '@' ? "Standard " : "") + device.name + ": " + layout;
  }
  return out;
}

void SimpleVideoPlayer::keyboard_download(const std::vector<std::string> &names) {
  for (const auto &name : names) {
    if (!name.empty())
      this->kbd_downloads_.emplace_back(name.c_str());
  }
  this->keyboard_next_();
  this->kbd_callbacks_.call();
}

bool SimpleVideoPlayer::keyboard_failed(const std::string &name) const {
  for (const KbdSlot &slot : this->kbd_slots_) {
    if (slot.state == KBD_FAILED && name == slot.name)
      return true;
  }
  return false;
}

SimpleVideoPlayer::KbdSlot *SimpleVideoPlayer::keyboard_slot_(const std::string &name) {
  for (KbdSlot &slot : this->kbd_slots_) {
    if (slot.state != KBD_EMPTY && name == slot.name)
      return &slot;
  }
  return nullptr;
}

// What every keyboard and device types with: the layouts there are put on them now, the first one
// missing is read next (into a slot no one wants).
void SimpleVideoPlayer::keyboard_resolve_() {
  if (!this->kbd_ready_)
    return;
  std::vector<std::string> wanted;
  auto want = [&wanted](const std::string &name) {
    if (!name.empty() && std::find(wanted.begin(), wanted.end(), name) == wanted.end())
      wanted.push_back(name);
  };
  for (size_t i = 0; i < this->kbd_inputs_.size(); i++) {
    const KbdInput &input = this->kbd_inputs_[i];
    const std::string standard = this->keyboard_standard_(i);
    want(standard);
    KbdSlot *slot = standard.empty() ? nullptr : this->keyboard_slot_(standard);
    input.input->set_layout(slot != nullptr && slot->state == KBD_READY ? &slot->layout : input.own);
    std::vector<hid::InputDevice> devices;
    input.input->list_devices(devices);
    for (const auto &device : devices) {
      const KbdAssign *entry = this->keyboard_assigned_(device.key);
      const std::string layout = entry != nullptr ? std::string(entry->layout) : std::string();
      want(layout);
      KbdSlot *own = layout.empty() ? nullptr : this->keyboard_slot_(layout);
      input.input->set_device_layout(device.key, own != nullptr && own->state == KBD_READY ? &own->layout : nullptr);
    }
  }
  // Slots no one wants are free (not one being read into).
  for (size_t i = 0; i < KBD_SLOTS; i++) {
    KbdSlot &slot = this->kbd_slots_[i];
    if (static_cast<int8_t>(i) == this->kbd_loading_ && this->kbd_step_ != KBD_IDLE)
      continue;
    if (slot.state == KBD_READY && std::find(wanted.begin(), wanted.end(), std::string(slot.name)) == wanted.end())
      slot.state = KBD_EMPTY;
  }
  if (this->kbd_step_ != KBD_IDLE)
    return;
  for (const auto &name : wanted) {
    if (this->keyboard_slot_(name) != nullptr)
      continue;  // read, being read, or failed
    for (size_t i = 0; i < KBD_SLOTS; i++) {
      KbdSlot &slot = this->kbd_slots_[i];
      if (slot.state != KBD_EMPTY)
        continue;
      copy_field(slot.name, sizeof(slot.name), name);
      slot.state = KBD_LOADING;
      this->kbd_loading_ = static_cast<int8_t>(i);
      this->kbd_downloading_ = false;
      this->kbd_loading_name_.assign(name.c_str());
      this->keyboard_next_();
      return;
    }
    ESP_LOGW(TAG, "More than %u layouts in use: %s waits", static_cast<unsigned>(KBD_SLOTS), name.c_str());
    break;
  }
  this->keyboard_next_();
}

// The read set up (keyboard_resolve_()), else the next download, from the file first.
void SimpleVideoPlayer::keyboard_next_() {
  if (!this->kbd_ready_ || this->kbd_step_ != KBD_IDLE)
    return;
  if (this->kbd_loading_ < 0 || this->kbd_slots_[this->kbd_loading_].state != KBD_LOADING) {
    // A download: into a free slot, kept in the directory only.
    if (this->kbd_downloads_.empty())
      return;
    int8_t free = -1;
    for (size_t i = 0; i < KBD_SLOTS && free < 0; i++) {
      if (this->kbd_slots_[i].state == KBD_EMPTY)
        free = static_cast<int8_t>(i);
    }
    if (free < 0)
      return;
    this->kbd_loading_ = free;
    this->kbd_downloading_ = true;
    this->kbd_loading_name_ = this->kbd_downloads_.front();
    this->kbd_downloads_.erase(this->kbd_downloads_.begin());
    KbdSlot &slot = this->kbd_slots_[free];
    copy_field(slot.name, sizeof(slot.name), this->kbd_loading_name_.c_str());
    slot.state = KBD_LOADING;
    this->kbd_len_ = 0;
    this->keyboard_fetch_();
    return;
  }
  this->kbd_len_ = 0;
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
      this->kbd_handle_, this->kbd_slots_[this->kbd_loading_].data + this->kbd_len_, room, &this->kbd_got_,
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
          if (ok && this->keyboard_loaded_(this->kbd_len_)) {
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

bool SimpleVideoPlayer::keyboard_loaded_(size_t len) {
  KbdSlot &slot = this->kbd_slots_[this->kbd_loading_];
  if (!hid::parse_layout(slot.data, len, &slot.layout))
    return false;
  slot.len = len;
  return true;
}

// The layout had from svp_relay into its file (the directory made if need be); its slot is not
// written to till that is done.
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
  const std::string path = this->kbd_dir_ + "/" + this->kbd_loading_name_.c_str() + KBD_SUFFIX;
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
      ESP_LOGW(TAG, "Layout %s not kept: %s", this->kbd_loading_name_.c_str(), storage::error_to_string(e));
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
              const int index = this->keyboard_index(this->kbd_loading_name_.c_str());
              if (index >= 0)
                this->kbd_entries_[index].kept = true;
            } else {
              ESP_LOGW(TAG, "Layout %s not kept", this->kbd_loading_name_.c_str());
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
      this->kbd_handle_, this->kbd_slots_[this->kbd_loading_].data, this->kbd_len_, [this](storage::StorageError e) {
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
  KbdSlot &slot = this->kbd_slots_[this->kbd_loading_];
  if (this->kbd_downloading_) {
    slot.state = KBD_EMPTY;  // kept only
    if (!ok)
      ESP_LOGW(TAG, "Layout %s not downloaded", this->kbd_loading_name_.c_str());
  } else {
    slot.state = ok ? KBD_READY : KBD_FAILED;
    if (ok)
      ESP_LOGI(TAG, "Layout %s", slot.name);
  }
  this->kbd_step_ = KBD_IDLE;
  this->kbd_loading_ = -1;
  this->keyboard_resolve_();
  this->kbd_callbacks_.call();
}

void SimpleVideoPlayer::keyboard_save_() {
  this->kbd_table_dirty_ = true;
  if (this->kbd_table_loaded_ && !this->kbd_table_busy_)
    this->keyboard_table_write_();
}

using storage::StorageError;

void SimpleVideoPlayer::keyboard_table_failed_(const char *what, StorageError err) {
  ESP_LOGW(TAG, "%s %s%s failed: %s", what, this->kbd_dir_default_.c_str(), KBD_TABLE_FILE,
           storage::error_to_string(err));
  this->kbd_table_busy_ = false;
}

// Once a storage holds the directory (else again when one is registered).
void SimpleVideoPlayer::keyboard_table_load_() {
  if (this->kbd_table_loaded_ || this->kbd_table_busy_ || storage::global_storage_registry == nullptr ||
      storage::global_storage_worker == nullptr)
    return;
  const std::string path = this->kbd_dir_default_ + KBD_TABLE_FILE;
  const char *rel = nullptr;
  this->kbd_table_storage_ = storage::global_storage_registry->resolve_path(path.c_str(), &rel);
  if (this->kbd_table_storage_ == nullptr)
    return;
  this->kbd_table_rel_ = rel;
  this->kbd_table_len_ = 0;
  this->kbd_table_busy_ = true;
  const StorageError err = storage::global_storage_worker->begin_read(
      this->kbd_table_storage_, this->kbd_table_rel_.c_str(), &this->kbd_table_handle_, [this](StorageError opened) {
        if (opened == StorageError::STORAGE_ERROR_OK) {
          this->keyboard_table_read_();
          return;
        }
        this->kbd_table_busy_ = false;
        if (opened != StorageError::STORAGE_ERROR_NOT_FOUND) {
          this->keyboard_table_failed_("Reading", opened);
          return;
        }
        // None yet: the preferences' table, of before, is taken over.
        if (this->kbd_pref_.load(this->kbd_assign_)) {
          ESP_LOGI(TAG, "Keyboard layouts given taken over from preferences");
          this->kbd_table_dirty_ = true;
        } else {
          std::memset(this->kbd_assign_, 0, sizeof(KbdAssignTable));
        }
        this->keyboard_table_loaded_();
      });
  if (err != StorageError::STORAGE_ERROR_OK)
    this->keyboard_table_failed_("Reading", err);
}

void SimpleVideoPlayer::keyboard_table_read_() {
  auto *buf = reinterpret_cast<uint8_t *>(this->kbd_assign_copy_);
  const StorageError err = storage::global_storage_worker->read_chunk(
      this->kbd_table_handle_, buf + this->kbd_table_len_, sizeof(KbdAssignTable) - this->kbd_table_len_,
      &this->kbd_table_got_, [this](StorageError read) {
        if (read != StorageError::STORAGE_ERROR_OK) {
          storage::global_storage_worker->end_read(this->kbd_table_handle_, [](StorageError) {});
          this->keyboard_table_failed_("Reading", read);
          return;
        }
        this->kbd_table_len_ += this->kbd_table_got_;
        if (this->kbd_table_got_ != 0 && this->kbd_table_len_ < sizeof(KbdAssignTable)) {
          this->keyboard_table_read_();
          return;
        }
        storage::global_storage_worker->end_read(this->kbd_table_handle_, [this](StorageError) {
          this->kbd_table_busy_ = false;
          if (this->kbd_table_len_ == sizeof(KbdAssignTable)) {
            std::memcpy(this->kbd_assign_, this->kbd_assign_copy_, sizeof(KbdAssignTable));
          } else {
            ESP_LOGW(TAG, "%s%s is not a keyboard table, starting empty", this->kbd_dir_default_.c_str(),
                     KBD_TABLE_FILE);
            std::memset(this->kbd_assign_, 0, sizeof(KbdAssignTable));
          }
          this->keyboard_table_loaded_();
        });
      });
  if (err != StorageError::STORAGE_ERROR_OK) {
    storage::global_storage_worker->end_read(this->kbd_table_handle_, [](StorageError) {});
    this->keyboard_table_failed_("Reading", err);
  }
}

void SimpleVideoPlayer::keyboard_table_loaded_() {
  for (KbdAssign &entry : this->kbd_assign_->entries) {
    entry.key[sizeof(entry.key) - 1] = '\0';
    entry.layout[sizeof(entry.layout) - 1] = '\0';
    entry.name[sizeof(entry.name) - 1] = '\0';
  }
  this->kbd_table_loaded_ = true;
  this->keyboard_resolve_();
  this->kbd_callbacks_.call();
  if (this->kbd_table_dirty_)
    this->keyboard_table_write_();
}

void SimpleVideoPlayer::keyboard_table_write_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (worker == nullptr || this->kbd_table_storage_ == nullptr)
    return;
  // The directory made if need be (as a layout kept there), not while the worker's task is in it.
  const char *dir_rel = nullptr;
  storage::PathStorage *dir = storage::global_storage_registry->resolve_path(this->kbd_dir_default_.c_str(), &dir_rel);
  if (dir != nullptr && !worker->has_active_task_io(dir) && dir_rel != nullptr && dir_rel[0] != '\0')
    dir->mkdir(dir_rel);  // already there: an error, and nothing to do
  std::memcpy(this->kbd_assign_copy_, this->kbd_assign_, sizeof(KbdAssignTable));
  this->kbd_table_dirty_ = false;
  this->kbd_table_tmp_ = this->kbd_table_rel_ + ".tmp";
  this->kbd_table_busy_ = true;
  const StorageError err = worker->begin_write(
      this->kbd_table_storage_, this->kbd_table_tmp_.c_str(), &this->kbd_table_handle_, [this](StorageError opened) {
        if (opened != StorageError::STORAGE_ERROR_OK) {
          this->kbd_table_dirty_ = true;
          this->keyboard_table_failed_("Writing", opened);
          return;
        }
        const StorageError queued = storage::global_storage_worker->write_chunk(
            this->kbd_table_handle_, reinterpret_cast<const uint8_t *>(this->kbd_assign_copy_),
            sizeof(KbdAssignTable), [this](StorageError written) {
              storage::global_storage_worker->end_write(this->kbd_table_handle_, [this, written](StorageError closed) {
                if (written != StorageError::STORAGE_ERROR_OK || closed != StorageError::STORAGE_ERROR_OK) {
                  this->kbd_table_dirty_ = true;
                  this->keyboard_table_failed_("Writing", written != StorageError::STORAGE_ERROR_OK ? written : closed);
                  return;
                }
                const StorageError moved = storage::global_storage_worker->async_move(
                    this->kbd_table_storage_, this->kbd_table_tmp_.c_str(), this->kbd_table_storage_,
                    this->kbd_table_rel_.c_str(),
                    [this](StorageError result) {
                      if (result != StorageError::STORAGE_ERROR_OK) {
                        this->kbd_table_dirty_ = true;
                        this->keyboard_table_failed_("Replacing", result);
                        return;
                      }
                      this->kbd_table_busy_ = false;
                      if (this->kbd_table_dirty_)
                        this->keyboard_table_write_();  // changed meanwhile
                    },
                    nullptr, true);
                if (moved != StorageError::STORAGE_ERROR_OK) {
                  this->kbd_table_dirty_ = true;
                  this->keyboard_table_failed_("Replacing", moved);
                }
              });
            });
        if (queued != StorageError::STORAGE_ERROR_OK) {
          storage::global_storage_worker->end_write(this->kbd_table_handle_, [](StorageError) {});
          this->kbd_table_dirty_ = true;
          this->keyboard_table_failed_("Writing", queued);
        }
      });
  if (err != StorageError::STORAGE_ERROR_OK) {
    this->kbd_table_dirty_ = true;
    this->keyboard_table_failed_("Writing", err);
  }
}

}  // namespace esphome::simple_video_player

#endif  // SVP_KEYBOARD
