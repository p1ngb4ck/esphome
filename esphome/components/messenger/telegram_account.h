#pragma once

#include "esphome/core/defines.h"

#ifdef USE_MESSENGER_ACCOUNT

#include "esphome/components/mtproto/mtproto.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace esphome::messenger {

enum class PeerKind : uint8_t { NONE = 0, USER, CHAT, CHANNEL };

struct AccountPeer {
  PeerKind kind;
  int64_t id;
  int64_t access_hash;
};

struct AccountChat {
  AccountPeer peer;
  char title[64];
  char last[96];
  char status[32];
  uint32_t date;
  int32_t unread;
  int32_t read_max;
  int32_t read_out;
  int64_t photo_id;
  int32_t photo_dc;
};

struct AccountContact {
  AccountPeer peer;
  char name[64];
  char status[32];
  char phone[20];
  char username[33];
  int64_t photo_id;
  int32_t photo_dc;
};

struct AccountFile {
  int64_t id;
  int64_t access_hash;
  int32_t dc;
  uint32_t size;
  uint8_t ref_len;
  uint8_t ref[80];
  char thumb[4];
  char view_thumb[4];
  uint32_t view_size;
  char small_thumb[4];
  uint32_t small_size;
  char name[48];
};

struct AccountIncomingFile {
  int32_t message_id;
  int64_t chat_id;
  uint8_t kind;
  char from[48];
  AccountFile file;
};

struct AccountMessage {
  int32_t id;
  bool out;
  uint8_t media;
  uint32_t date;
  char from[48];
  char text[512];
  AccountFile file;
};

class TelegramAccount {
 public:
  static constexpr uint8_t CHANGED_CHATS = 1;
  static constexpr uint8_t CHANGED_CONTACTS = 2;
  static constexpr uint8_t CHANGED_MESSAGES = 4;
  static constexpr uint8_t CHANGED_FILES = 8;
  static constexpr uint8_t CHANGED_RESULT = 16;
  static constexpr size_t MAX_INCOMING_FILES = 8;
  static constexpr uint32_t VIEW_MAX_SIDE = 800;
  static constexpr size_t MAX_CHATS = 60;
  static constexpr size_t MAX_CONTACTS = 300;
  static constexpr size_t MAX_MESSAGES = 60;
  static constexpr size_t DOWNLOAD_PART = 32768;

  void attach(mtproto::Mtproto *mtproto, Component *owner);
  void on_state(mtproto::State state);
  void set_active(bool active);
  uint8_t take_changes() { return this->changes_.exchange(0); }

  size_t chat_count() const;
  std::string chat_title(size_t index) const;
  std::string chat_last(size_t index) const;
  uint32_t chat_date(size_t index) const;
  int32_t chat_unread(size_t index) const;
  size_t contact_count() const;
  std::string contact_name(size_t index) const;
  std::string contact_status(size_t index) const;
  std::string contact_phone(size_t index) const;
  std::string contact_username(size_t index) const;
  int64_t contact_user_id(size_t index) const;
  bool contact_has_photo(size_t index) const;
  int64_t chat_peer_id(size_t index) const;
  bool chat_has_photo(size_t index) const;
  std::string chat_status(size_t index) const;
  int64_t open_peer_id() const;
  std::string open_status() const;
  bool message_read(size_t index) const;
  bool add_contact(const std::string &first_name, const std::string &last_name, const std::string &phone);
  bool delete_contact(size_t index);
  bool request_about(size_t index);
  std::string about() const;
  std::string result() const;
  bool photo_location(int64_t peer_id, AccountPeer *peer, int64_t *photo_id, int32_t *dc) const;
  bool download_peer_photo_part(const AccountPeer &peer, int64_t photo_id, int32_t dc, int64_t offset, uint8_t *out,
                                size_t capacity, size_t *got, std::string *error);
  size_t message_count() const;
  std::string message_text(size_t index) const;
  std::string message_from(size_t index) const;
  bool message_out(size_t index) const;
  uint32_t message_date(size_t index) const;
  uint8_t message_media(size_t index) const;
  int32_t message_id(size_t index) const;
  std::string message_file_name(size_t index) const;
  bool file_of(int32_t message_id, AccountFile *file) const;
  bool take_incoming_file(AccountIncomingFile *file);
  bool download_part(const AccountFile &file, int64_t offset, uint8_t *out, size_t capacity, size_t *got,
                     std::string *error);
  void end_download();

  bool open_chat(size_t index);
  bool open_contact(size_t index);
  void close_chat();
  bool has_open_chat() const;
  std::string open_title() const;
  int64_t open_user_id() const;
  bool open_user(int64_t *id, int64_t *access_hash) const;
  bool send_text(const std::string &text);
  bool send_media(uint8_t kind, const uint8_t *data, size_t length, const char *name, const char *mime,
                  const char *caption, uint32_t duration, std::string *error);
  void refresh();

 protected:
  bool allocate_();
  void release_();
  void update_();
  bool call_wait_(int dc, const uint8_t *body, size_t length, std::string *error,
                  std::function<bool(mtproto::TlReader)> &&on_result);
  void notify_(uint8_t what);
  void request_contacts_();
  void request_dialogs_();
  void request_history_(const AccountPeer &peer);
  void mark_read_(const AccountPeer &peer, int32_t max_id);
  void open_peer_(const AccountPeer &peer, const char *title);
  void on_contacts_(mtproto::TlReader result);
  void on_dialogs_(mtproto::TlReader result);
  void on_history_(const AccountPeer &peer, mtproto::TlReader result);
  void on_updates_(mtproto::TlReader updates);
  void on_new_message_(mtproto::TlReader message, mtproto::TlReader users, mtproto::TlReader chats);
  void on_update_item_(mtproto::TlReader update, mtproto::TlReader users, mtproto::TlReader chats);
  bool get_file_part_(int dc, const uint8_t *location, size_t location_len, int64_t offset, uint8_t *out,
                      size_t capacity, size_t *got, std::string *error);
  void set_result_(const char *text);
  void add_incoming_(const AccountPeer &peer, const char *title, int32_t id, bool out, uint32_t date, const char *from,
                     const char *text, uint8_t media, const AccountFile *file);
  AccountChat *find_chat_(PeerKind kind, int64_t id);
  void move_to_top_(size_t index);

  mtproto::Mtproto *mtproto_{nullptr};
  Component *owner_{nullptr};
  std::atomic<uint8_t> changes_{0};
  mutable Mutex lock_;
  std::atomic<bool> started_{false};
  bool logged_in_{false};
  bool active_{false};

  AccountChat *chats_{nullptr};
  size_t chat_count_{0};
  AccountContact *contacts_{nullptr};
  size_t contact_count_{0};
  AccountMessage *messages_{nullptr};
  size_t message_count_{0};
  AccountIncomingFile *incoming_{nullptr};
  uint32_t incoming_head_{0};
  uint32_t incoming_tail_{0};
  AccountPeer open_{};
  char open_title_[64]{};
  char about_[160]{};
  char result_[64]{};
  uint32_t history_serial_{0};
  Mutex wait_lock_;
  TaskHandle_t waiter_{nullptr};
  uint32_t wait_serial_{0};
  bool done_ok_{false};
  char done_error_[64]{};
};

}

#endif
