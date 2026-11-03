#pragma once

#include "esphome/core/defines.h"
#ifdef USE_KEYBOARD_LAYOUTS

#include "esphome/components/fixed_containers/fixed_containers.h"
#include "esphome/components/hid/hid_input.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#ifdef USE_KEYBOARD_LAYOUTS_LINK
#include "esphome/components/relay_link/relay_link.h"
#endif
#ifdef USE_KEYBOARD_LAYOUTS_ENTITIES
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#endif

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace esphome::keyboard_layouts {

using fixed_containers::FixedList;
using fixed_containers::FixedString;

class KeyboardLayouts;

#if defined(USE_KEYBOARD_LAYOUTS_ENTITIES) && defined(USE_SELECT)
class KeyboardDeviceSelect : public select::Select {
 public:
  void set_parent(KeyboardLayouts *parent) { this->parent_ = parent; }

 protected:
  void control(size_t index) override;
  KeyboardLayouts *parent_{nullptr};
};
#endif

class KeyboardLayouts : public Component {
 public:
  static KeyboardLayouts *create();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

#ifdef USE_KEYBOARD_LAYOUTS_LINK
  void set_relay(relay_link::RelayLink *relay) {
    this->link_ = relay;
    this->relay_source_ = true;
  }
  void add_http_source(relay_link::RelayLink *link, const char *base) {
    this->link_ = link;
    if (this->http_count_ < HTTP_SOURCES_MAX)
      this->http_sources_[this->http_count_++] = base;
  }
#endif
  void set_keyboard_directory(const std::string &dir) {
    this->kbd_dir_ = dir;
    this->kbd_dir_default_ = dir;
  }
  void add_keyboard(hid::HIDInput *input, const std::string &name, const std::string &layout);
  void keyboard_set_directory(const std::string &dir);
  const std::string &keyboard_default_directory() const { return this->kbd_dir_default_; }
  const std::string &keyboard_directory() const { return this->kbd_dir_; }
  void keyboard_list_fetch();
  bool keyboard_listed() const { return this->kbd_listed_; }
  bool keyboard_list_remote() const { return this->kbd_list_remote_; }
  size_t keyboard_count() const { return this->kbd_count_; }
  const char *keyboard_name(size_t index) const;
  const char *keyboard_description(size_t index) const;
  bool keyboard_kept(size_t index) const;
  int keyboard_index(const std::string &name) const;
  std::string keyboard_label(const std::string &name) const;
  size_t keyboard_find(const std::string &query, int *out, size_t max) const;

  struct KeyboardDevice {
    FixedString<48> key;
    FixedString<48> name;
    uint8_t input{0};
    bool connected{false};
    FixedString<32> layout;
  };
  size_t keyboard_input_count() const { return this->kbd_inputs_.size(); }
  const std::string &keyboard_input_name(size_t index) const { return this->kbd_inputs_[index].name; }
  const FixedList<KeyboardDevice> &keyboard_devices() const;
  bool keyboard_assign(const std::string &key, const std::string &layout);
  bool keyboard_assign_named(const std::string &device, const std::string &layout);
  std::string keyboard_summary() const;
  void keyboard_download(const std::vector<std::string> &names);
  size_t keyboard_downloads_left() const { return this->kbd_downloads_.size(); }
  bool keyboard_loading() const { return this->kbd_step_ != KBD_IDLE; }
  bool keyboard_failed(const std::string &name) const;
  void add_on_keyboard_callback(std::function<void()> &&callback) { this->kbd_callbacks_.add(std::move(callback)); }
#ifdef USE_KEYBOARD_LAYOUTS_ENTITIES
#ifdef USE_SELECT
  void set_keyboard_select(select::Select *select) { this->kbd_select_ = select; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_keyboard_layout_sensor(text_sensor::TextSensor *sensor) { this->kbd_layout_sensor_ = sensor; }
#endif
  void keyboard_entity_pick(size_t index);
#endif

 protected:
  static constexpr size_t KBD_LIST_BYTES = 32 * 1024;
  static constexpr size_t KBD_MAX = 1024;
  static constexpr size_t KBD_LAYOUT_BYTES = 8 * 1024;
  static constexpr size_t KBD_SLOTS = 6;
  static constexpr size_t KBD_ASSIGN_MAX = 24;
  struct KbdEntry {
    uint16_t name;
    uint16_t description;
    bool kept;
  };
  enum KbdStep : uint8_t { KBD_IDLE, KBD_READING, KBD_FETCHING, KBD_WRITING };
  enum KbdSlotState : uint8_t { KBD_EMPTY, KBD_LOADING, KBD_READY, KBD_FAILED };
  struct KbdSlot {
    char name[32];
    KbdSlotState state;
    size_t len;
    uint8_t *data;
    hid::KeyboardLayout layout;
  };
  struct KbdAssign {
    char key[48];
    char layout[32];
    char name[32];
    uint8_t input;
  };
  struct KbdAssignTable {
    KbdAssign entries[KBD_ASSIGN_MAX];
  };
  struct KbdInput {
    hid::HIDInput *input;
    std::string name;
    std::string layout;
    const hid::KeyboardLayout *own;
  };
  void keyboard_list_local_(bool build);
  void keyboard_listed_();
  void keyboard_resolve_();
  std::string keyboard_standard_(size_t input) const;
  const KbdAssign *keyboard_assigned_(const std::string &key) const;
  KbdSlot *keyboard_slot_(const std::string &name);
  void keyboard_next_();
  void keyboard_read_();
  void keyboard_read_end_(bool ok);
  void keyboard_fetch_();
  bool keyboard_loaded_(size_t len);
  void keyboard_keep_(size_t len);
  void keyboard_write_();
  void keyboard_done_(bool ok);
  void keyboard_save_();
#ifdef USE_KEYBOARD_LAYOUTS_LINK
  static constexpr size_t HTTP_SOURCES_MAX = 4;
  static constexpr uint8_t NO_SOURCE = 0xFF;
  static void keyboard_list_job_(void *ctx, uint16_t index);
  static void keyboard_get_job_(void *ctx, uint16_t index);
  void keyboard_list_task_();
  void keyboard_get_task_();
  uint8_t next_source_(uint8_t from) const;
  bool source_http_(uint8_t source, const char *path, uint8_t *into, size_t cap, size_t *len);
  void keyboard_list_add_(size_t start, size_t got);
  relay_link::RelayLink *link_{nullptr};
  bool relay_source_{false};
  const char *http_sources_[HTTP_SOURCES_MAX]{};
  uint8_t http_count_{0};
  uint8_t list_source_{NO_SOURCE};
  uint8_t get_source_{NO_SOURCE};
  bool kbd_list_any_{false};
#endif
  std::string kbd_dir_, kbd_dir_default_;
  std::vector<KbdInput, RAMAllocator<KbdInput>> kbd_inputs_;
  char *kbd_list_{nullptr};
  KbdEntry *kbd_entries_{nullptr};
  size_t kbd_count_{0};
  bool kbd_listed_{false};
  bool kbd_list_remote_{false};
  bool kbd_list_busy_{false};
  KbdSlot kbd_slots_[KBD_SLOTS]{};
  KbdAssignTable *kbd_assign_{nullptr};
  ESPPreferenceObject kbd_pref_;
  static constexpr const char *KBD_TABLE_FILE = "/keyboards.bin";
  void keyboard_table_load_();
  void keyboard_table_read_();
  void keyboard_table_loaded_();
  void keyboard_table_write_();
  void keyboard_table_failed_(const char *what, storage::StorageError err);
  KbdAssignTable *kbd_assign_copy_{nullptr};
  storage::StreamHandle kbd_table_handle_{};
  storage::PathStorage *kbd_table_storage_{nullptr};
  std::string kbd_table_rel_, kbd_table_tmp_;
  size_t kbd_table_len_{0}, kbd_table_got_{0};
  bool kbd_table_loaded_{false}, kbd_table_busy_{false}, kbd_table_dirty_{false};
  static constexpr size_t KBD_DOWNLOADS_MAX = 16;
  FixedList<FixedString<32>> kbd_downloads_;
  static constexpr size_t KBD_DEVICES_MAX = KBD_ASSIGN_MAX + 8;
  mutable FixedList<KeyboardDevice> kbd_devices_;
  int8_t kbd_loading_{-1};
  bool kbd_downloading_{false};
  KbdStep kbd_step_{KBD_IDLE};
  bool kbd_ready_{false};
  bool kbd_kept_{false};
  FixedString<32> kbd_loading_name_;
  size_t kbd_len_{0};
  size_t kbd_got_{0};
  storage::StreamHandle kbd_handle_{};
  CallbackManager<void()> kbd_callbacks_;
  std::atomic<bool> kbd_list_done_{false}, kbd_get_done_{false};
  bool kbd_list_ok_{false}, kbd_get_ok_{false};
  size_t kbd_list_len_{0}, kbd_list_got_{0}, kbd_get_len_{0};
#ifdef USE_KEYBOARD_LAYOUTS_ENTITIES
  static constexpr size_t KBD_ENTITY_MAX = KBD_ASSIGN_MAX + 8;
  static constexpr size_t KBD_ENTITY_LABEL = 48;
  void keyboard_entities_();
#ifdef USE_SELECT
  select::Select *kbd_select_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *kbd_layout_sensor_{nullptr};
#endif
  char (*kbd_entity_labels_)[KBD_ENTITY_LABEL]{nullptr};
  char (*kbd_entity_keys_)[sizeof(KbdAssign::key)]{nullptr};
  size_t kbd_entity_count_{0};
  char kbd_entity_pick_[sizeof(KbdAssign::key)]{};
#endif
};

}

#endif
