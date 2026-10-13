#include "key_bindings.h"

#ifdef USE_LVGL_HID

#include "esphome/components/hid/hid_input.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_LVGL_KEY_BINDINGS_FILE
#include "esphome/components/storage/storage.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstring>

namespace esphome::lvgl {

static const char *const TAG = "lvgl.key_bindings";
static constexpr uint32_t RETRY_MS = 2000;

static const struct {
  uint8_t mod;
  const char *name;
} MODIFIER_NAMES[] = {
    {KeyBindings::MOD_CTRL, "CTRL"},
    {KeyBindings::MOD_ALT, "ALT"},
    {KeyBindings::MOD_SHIFT, "SHIFT"},
    {KeyBindings::MOD_GUI, "GUI"},
};

static std::string trim(const std::string &text) {
  size_t start = 0, end = text.size();
  while (start < end && std::isspace(static_cast<unsigned char>(text[start])))
    start++;
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
    end--;
  return text.substr(start, end - start);
}

static std::string upper(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

uint8_t KeyBindings::modifiers_of(uint8_t hid_modifiers) {
  uint8_t mods = 0;
  if (hid_modifiers & (hid::MOD_LEFT_SHIFT | hid::MOD_RIGHT_SHIFT))
    mods |= MOD_SHIFT;
  if (hid_modifiers & (hid::MOD_LEFT_CTRL | hid::MOD_RIGHT_CTRL))
    mods |= MOD_CTRL;
  if (hid_modifiers & (hid::MOD_LEFT_ALT | hid::MOD_RIGHT_ALT))
    mods |= MOD_ALT;
  if (hid_modifiers & (hid::MOD_LEFT_GUI | hid::MOD_RIGHT_GUI))
    mods |= MOD_GUI;
  return mods;
}

std::string KeyBindings::key_text(const Key &key) {
  std::string text;
  for (const auto &modifier : MODIFIER_NAMES) {
    if (key.mods & modifier.mod) {
      text += modifier.name;
      text += '+';
    }
  }
  return text + hid::key_name(key.page, key.usage);
}

bool KeyBindings::parse_key(const std::string &text, Key *key) {
  Key result{hid::PAGE_KEYBOARD, 0, 0};
  std::string rest = upper(trim(text));
  // Modifiers first: "SHIFT+TAB"; a lone "+" is not one.
  for (size_t plus; (plus = rest.find('+')) != std::string::npos && plus + 1 < rest.size();) {
    const std::string name = trim(rest.substr(0, plus));
    uint8_t mod = 0;
    for (const auto &modifier : MODIFIER_NAMES) {
      if (name == modifier.name)
        mod = modifier.mod;
    }
    if (name == "META")
      mod = MOD_GUI;
    if (mod == 0)
      return false;
    result.mods |= mod;
    rest = trim(rest.substr(plus + 1));
  }
  if (rest.empty())
    return false;
  for (uint16_t i = 0; i < hid::KEY_NAMES_COUNT; i++) {
    if (rest == hid::KEY_NAMES[i].name) {
      result.page = hid::KEY_NAMES[i].page;
      result.usage = hid::KEY_NAMES[i].usage;
      *key = result;
      return true;
    }
  }
  // "page:usage", as key_name() writes a key without a name; a bare number is a keyboard usage.
  char *end = nullptr;
  const unsigned long first = strtoul(rest.c_str(), &end, 0);
  if (end == rest.c_str() || first > 0xFFFF)
    return false;
  if (*end == '\0') {
    result.usage = static_cast<uint16_t>(first);
  } else {
    if (*end != ':')
      return false;
    const char *second_start = end + 1;
    const unsigned long second = strtoul(second_start, &end, 0);
    if (end == second_start || *end != '\0' || second > 0xFFFF)
      return false;
    result.page = static_cast<uint16_t>(first);
    result.usage = static_cast<uint16_t>(second);
  }
  *key = result;
  return true;
}

void KeyBindings::add_action(const char *name, std::vector<Key> keys, Trigger<> *trigger) {
  this->actions_.push_back({name, std::move(keys), trigger});
}

void KeyBindings::setup() {
  this->defaults_();
  if (this->file_ == nullptr) {
    this->pref_ = global_preferences->make_preference(sizeof(PrefData), this->preference_key_);
    this->loaded_ = this->load_();
  }
}

void KeyBindings::loop() {
  const uint32_t now = millis();
  if (this->learn_action_ >= 0 && static_cast<int32_t>(now - this->learn_until_) >= 0) {
    const std::string action = this->actions_[this->learn_action_].name;
    this->learn_action_ = -1;
    ESP_LOGI(TAG, "No key learned for %s", action.c_str());
    this->learned_trigger_.trigger(action, "");
  }
  if (this->loaded_ && !this->dirty_)
    return;
  if (now - this->last_attempt_ < RETRY_MS && this->last_attempt_ != 0)
    return;
  this->last_attempt_ = now;
  if (!this->loaded_) {
    this->loaded_ = this->load_();
    return;
  }
  if (this->save_())
    this->dirty_ = false;
}

void KeyBindings::dump_config() {
  ESP_LOGCONFIG(TAG, "Key bindings: %u actions", static_cast<unsigned>(this->actions_.size()));
  if (this->file_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  File: %s", this->file_);
  }
}

const char *KeyBindings::action_name(size_t index) const {
  return index < this->actions_.size() ? this->actions_[index].name : "";
}

int KeyBindings::find_action(const std::string &name) const {
  for (size_t i = 0; i < this->actions_.size(); i++) {
    if (name == this->actions_[i].name)
      return static_cast<int>(i);
  }
  return -1;
}

std::string KeyBindings::keys_text(size_t index) const {
  std::string text;
  for (const Binding &binding : this->bindings_) {
    if (binding.action != index)
      continue;
    if (!text.empty())
      text += ", ";
    text += key_text(binding.key);
  }
  return text;
}

int KeyBindings::resolve(uint16_t page, uint16_t usage, uint8_t mods) const {
  int plain = -1;
  for (const Binding &binding : this->bindings_) {
    if (binding.key.page != page || binding.key.usage != usage)
      continue;
    if (binding.key.mods == mods)
      return binding.action;
    if (binding.key.mods == 0)
      plain = binding.action;
  }
  return plain;
}

bool KeyBindings::take_learned(uint16_t page, uint16_t usage, uint8_t mods) {
  if (this->learn_action_ < 0)
    return false;
  // Modifiers are learned with the key they are held for.
  if (page == hid::PAGE_KEYBOARD && usage >= 0xE0 && usage <= 0xE7)
    return true;
  const Key key{page, usage, mods};
  const auto action = static_cast<uint8_t>(this->learn_action_);
  this->learn_action_ = -1;
  this->bind_(key, action);
  this->changed_();
  const std::string text = key_text(key);
  ESP_LOGI(TAG, "%s learned for %s", text.c_str(), this->actions_[action].name);
  this->learned_trigger_.trigger(this->actions_[action].name, text);
  return true;
}

void KeyBindings::fire(int action) {
  if (action >= 0 && static_cast<size_t>(action) < this->actions_.size() && this->actions_[action].trigger != nullptr)
    this->actions_[action].trigger->trigger();
}

void KeyBindings::bind_(const Key &key, uint8_t action) {
  this->bindings_.erase(std::remove_if(this->bindings_.begin(), this->bindings_.end(),
                                       [&key](const Binding &binding) { return binding.key == key; }),
                        this->bindings_.end());
  this->bindings_.push_back({key, action});
}

void KeyBindings::changed_() {
  // A change before the stored bindings could be read: those are not read any more.
  this->loaded_ = true;
  this->dirty_ = true;
  this->last_attempt_ = 0;
}

bool KeyBindings::add(const std::string &action, const std::string &key) {
  const int index = this->find_action(action);
  Key parsed{};
  if (index < 0 || !parse_key(key, &parsed)) {
    ESP_LOGW(TAG, "Cannot bind '%s' to '%s'", key.c_str(), action.c_str());
    return false;
  }
  this->bind_(parsed, static_cast<uint8_t>(index));
  this->changed_();
  return true;
}

bool KeyBindings::remove(const std::string &key) {
  Key parsed{};
  if (!parse_key(key, &parsed)) {
    ESP_LOGW(TAG, "Unknown key '%s'", key.c_str());
    return false;
  }
  const size_t before = this->bindings_.size();
  this->bindings_.erase(std::remove_if(this->bindings_.begin(), this->bindings_.end(),
                                       [&parsed](const Binding &binding) { return binding.key == parsed; }),
                        this->bindings_.end());
  if (this->bindings_.size() == before)
    return false;
  this->changed_();
  return true;
}

bool KeyBindings::clear(const std::string &action) {
  const int index = this->find_action(action);
  if (index < 0) {
    ESP_LOGW(TAG, "Unknown action '%s'", action.c_str());
    return false;
  }
  this->bindings_.erase(std::remove_if(this->bindings_.begin(), this->bindings_.end(),
                                       [index](const Binding &binding) { return binding.action == index; }),
                        this->bindings_.end());
  this->changed_();
  return true;
}

void KeyBindings::defaults_() {
  this->bindings_.clear();
  for (size_t i = 0; i < this->actions_.size(); i++) {
    for (const Key &key : this->actions_[i].defaults)
      this->bind_(key, static_cast<uint8_t>(i));
  }
}

void KeyBindings::reset() {
  this->defaults_();
  this->changed_();
}

void KeyBindings::learn(const std::string &action, uint32_t timeout_ms) {
  const int index = this->find_action(action);
  if (index < 0) {
    ESP_LOGW(TAG, "Unknown action '%s'", action.c_str());
    return;
  }
  this->learn_action_ = index;
  this->learn_until_ = millis() + timeout_ms;
  ESP_LOGI(TAG, "Press the key for %s", action.c_str());
}

void KeyBindings::cancel_learn() {
  if (this->learn_action_ < 0)
    return;
  const std::string action = this->actions_[this->learn_action_].name;
  this->learn_action_ = -1;
  this->learned_trigger_.trigger(action, "");
}

void KeyBindings::log_bindings() const {
  for (size_t i = 0; i < this->actions_.size(); i++)
    ESP_LOGI(TAG, "%s: %s", this->actions_[i].name, this->keys_text(i).c_str());
}

// The file: one line per action, "name = KEY, KEY"; an action without a line keeps its defaults.
std::string KeyBindings::encode_() const {
  std::string text = "# action = keys\n";
  for (size_t i = 0; i < this->actions_.size(); i++) {
    text += this->actions_[i].name;
    text += " = ";
    text += this->keys_text(i);
    text += '\n';
  }
  return text;
}

void KeyBindings::decode_(const char *data, size_t len) {
  std::vector<bool> listed(this->actions_.size(), false);
  std::vector<Binding> bindings;
  size_t at = 0;
  while (at < len) {
    const char *line_end = static_cast<const char *>(memchr(data + at, '\n', len - at));
    const size_t line_len = line_end != nullptr ? static_cast<size_t>(line_end - (data + at)) : len - at;
    const std::string line = trim(std::string(data + at, line_len));
    at += line_len + 1;
    const size_t equals = line.find('=');
    if (line.empty() || line[0] == '#' || equals == std::string::npos)
      continue;
    const int action = this->find_action(trim(line.substr(0, equals)));
    if (action < 0)
      continue;
    listed[action] = true;
    const std::string keys = line.substr(equals + 1);
    size_t start = 0;
    while (start <= keys.size()) {
      size_t comma = keys.find(',', start);
      if (comma == std::string::npos)
        comma = keys.size();
      const std::string text = trim(keys.substr(start, comma - start));
      start = comma + 1;
      if (text.empty())
        continue;
      Key key{};
      if (parse_key(text, &key)) {
        bindings.push_back({key, static_cast<uint8_t>(action)});
      } else {
        ESP_LOGW(TAG, "Unknown key '%s' for %s", text.c_str(), this->actions_[action].name);
      }
    }
  }
  this->bindings_.erase(std::remove_if(this->bindings_.begin(), this->bindings_.end(),
                                       [&listed](const Binding &binding) { return listed[binding.action]; }),
                        this->bindings_.end());
  for (const Binding &binding : bindings)
    this->bind_(binding.key, binding.action);
}

uint32_t KeyBindings::actions_hash_() const {
  std::string names;
  for (const Action &action : this->actions_) {
    names += action.name;
    names += ',';
  }
  return fnv1_hash(names);
}

bool KeyBindings::load_() {
#ifdef USE_LVGL_KEY_BINDINGS_FILE
  if (this->file_ != nullptr) {
    if (storage::global_storage_registry == nullptr)
      return false;
    const char *rel = nullptr;
    storage::PathStorage *storage = storage::global_storage_registry->resolve_path(this->file_, &rel);
    if (storage == nullptr)
      return false;
    storage::RamBuffer buffer;
    size_t size = 0;
    const storage::StorageError err = storage::read_file(storage, rel, buffer, &size);
    if (err == storage::StorageError::STORAGE_ERROR_NOT_FOUND)
      return true;
    if (err != storage::StorageError::STORAGE_ERROR_OK)
      return false;
    this->decode_(reinterpret_cast<const char *>(buffer.get()), size);
    ESP_LOGD(TAG, "Read %s", this->file_);
    return true;
  }
#endif
  PrefData data{};
  if (!this->pref_.load(&data) || data.actions_hash != this->actions_hash_() || data.count > PREF_BINDINGS)
    return true;
  this->bindings_.clear();
  for (uint16_t i = 0; i < data.count; i++) {
    const PrefBinding &binding = data.bindings[i];
    if (binding.action < this->actions_.size())
      this->bind_({binding.page, binding.usage, binding.mods}, binding.action);
  }
  return true;
}

bool KeyBindings::save_() {
#ifdef USE_LVGL_KEY_BINDINGS_FILE
  if (this->file_ != nullptr) {
    if (storage::global_storage_registry == nullptr)
      return false;
    const char *rel = nullptr;
    storage::PathStorage *storage = storage::global_storage_registry->resolve_path(this->file_, &rel);
    if (storage == nullptr)
      return false;
    const std::string text = this->encode_();
    const storage::StorageError err =
        storage::write_file(storage, rel, reinterpret_cast<const uint8_t *>(text.data()), text.size());
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      ESP_LOGW(TAG, "Writing %s failed (%u), retrying", this->file_, static_cast<unsigned>(err));
      return false;
    }
    return true;
  }
#endif
  PrefData data{};
  data.actions_hash = this->actions_hash_();
  for (const Binding &binding : this->bindings_) {
    if (data.count == PREF_BINDINGS) {
      ESP_LOGW(TAG, "Only %u key bindings fit in preferences", static_cast<unsigned>(PREF_BINDINGS));
      break;
    }
    data.bindings[data.count++] = {binding.key.page, binding.key.usage, binding.key.mods, binding.action};
  }
  if (!this->pref_.save(&data) || !global_preferences->sync()) {
    ESP_LOGW(TAG, "Saving to preferences failed, retrying");
    return false;
  }
  return true;
}

}  // namespace esphome::lvgl

#endif  // USE_LVGL_HID
