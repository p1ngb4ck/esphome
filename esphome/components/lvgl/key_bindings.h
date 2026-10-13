#pragma once

#include "esphome/core/defines.h"

#ifdef USE_LVGL_HID

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <string>
#include <vector>

namespace esphome::lvgl {

/// What the keys of HID keyboards and remotes do: the built-in navigation and the configuration's
/// own actions. An action takes any number of keys, a key belongs to one action. Kept in a file
/// (file:) or in preferences, written on every change.
class KeyBindings : public Component {
 public:
  /// The built-in actions, first in the action list in this order.
  enum Builtin : uint8_t {
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_NEXT,
    ACT_PREV,
    ACT_ENTER,
    ACT_BACK,
    ACT_HOME,
    ACT_END,
    ACT_PAGE_UP,
    ACT_PAGE_DOWN,
    ACT_BACKSPACE,
    ACT_DELETE,
    BUILTIN_COUNT,
  };
  /// Modifiers of a key, either side.
  enum Modifier : uint8_t { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_GUI = 8 };
  struct Key {
    uint16_t page;
    uint16_t usage;
    uint8_t mods;
    bool operator==(const Key &other) const {
      return this->page == other.page && this->usage == other.usage && this->mods == other.mods;
    }
  };

  // Codegen: the built-in actions first, in Builtin order.
  void add_action(const char *name, std::vector<Key> keys, Trigger<> *trigger);
  void set_file(const char *file) { this->file_ = file; }
  void set_preference_key(uint32_t key) { this->preference_key_ = key; }
  /// (action, key) when a learned key was bound; key "" when learning ended without one.
  Trigger<std::string, std::string> *get_learned_trigger() { return &this->learned_trigger_; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  // Changes, from anywhere on the main loop; each is saved.
  bool add(const std::string &action, const std::string &key);
  bool remove(const std::string &key);
  bool clear(const std::string &action);
  void reset();
  /// The next key pressed (modifiers held with it) is bound to the action.
  void learn(const std::string &action, uint32_t timeout_ms);
  void cancel_learn();
  bool is_learning() const { return this->learn_action_ >= 0; }
  /// The action a key is being learned for, -1 none.
  int learning_action() const { return this->learn_action_; }
  void log_bindings() const;

  size_t action_count() const { return this->actions_.size(); }
  const char *action_name(size_t index) const;
  /// The keys of an action, "UP, MENU_UP".
  std::string keys_text(size_t index) const;
  int find_action(const std::string &name) const;

  // For the HID keypads, on the LVGL thread.
  /// The action a key stands for, -1 for none: with its modifiers, else without them.
  int resolve(uint16_t page, uint16_t usage, uint8_t mods) const;
  /// While learning: binds the key and returns true.
  bool take_learned(uint16_t page, uint16_t usage, uint8_t mods);
  /// The action's own automation, if it has one.
  void fire(int action);

  /// Our modifiers from HID modifier bits.
  static uint8_t modifiers_of(uint8_t hid_modifiers);
  /// "SHIFT+TAB", "MENU_UP", "0x0C:0x1234".
  static std::string key_text(const Key &key);
  static bool parse_key(const std::string &text, Key *key);

 protected:
  struct Action {
    const char *name;
    std::vector<Key> defaults;
    Trigger<> *trigger;
  };
  struct Binding {
    Key key;
    uint8_t action;
  };
  struct PrefBinding {
    uint16_t page;
    uint16_t usage;
    uint8_t mods;
    uint8_t action;
  };
  static constexpr size_t PREF_BINDINGS = 64;
  struct PrefData {
    uint32_t actions_hash;
    uint16_t count;
    PrefBinding bindings[PREF_BINDINGS];
  };

  void defaults_();
  void bind_(const Key &key, uint8_t action);
  void changed_();
  bool load_();
  bool save_();
  std::string encode_() const;
  void decode_(const char *data, size_t len);
  uint32_t actions_hash_() const;

  std::vector<Action> actions_;
  std::vector<Binding> bindings_;
  const char *file_{nullptr};
  uint32_t preference_key_{0};
  ESPPreferenceObject pref_;
  Trigger<std::string, std::string> learned_trigger_;
  int learn_action_{-1};
  uint32_t learn_until_{0};
  uint32_t last_attempt_{0};
  bool loaded_{false};
  bool dirty_{false};
};

template<typename... Ts> class KeyBindingsLearnAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  TEMPLATABLE_VALUE(std::string, action)
  TEMPLATABLE_VALUE(uint32_t, timeout)
  void play(const Ts &...x) override { this->parent_->learn(this->action_.value(x...), this->timeout_.value(x...)); }
};

template<typename... Ts> class KeyBindingsAddAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  TEMPLATABLE_VALUE(std::string, action)
  TEMPLATABLE_VALUE(std::string, key)
  void play(const Ts &...x) override { this->parent_->add(this->action_.value(x...), this->key_.value(x...)); }
};

template<typename... Ts> class KeyBindingsRemoveAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  TEMPLATABLE_VALUE(std::string, key)
  void play(const Ts &...x) override { this->parent_->remove(this->key_.value(x...)); }
};

template<typename... Ts> class KeyBindingsClearAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  TEMPLATABLE_VALUE(std::string, action)
  void play(const Ts &...x) override { this->parent_->clear(this->action_.value(x...)); }
};

template<typename... Ts> class KeyBindingsResetAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  void play(const Ts &...x) override { this->parent_->reset(); }
};

template<typename... Ts> class KeyBindingsCancelLearnAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  void play(const Ts &...x) override { this->parent_->cancel_learn(); }
};

template<typename... Ts> class KeyBindingsLogAction : public Action<Ts...>, public Parented<KeyBindings> {
 public:
  void play(const Ts &...x) override { this->parent_->log_bindings(); }
};

}  // namespace esphome::lvgl

#endif  // USE_LVGL_HID
