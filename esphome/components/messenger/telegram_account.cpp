#include "telegram_account.h"

#ifdef USE_MESSENGER_ACCOUNT

#include "esphome/core/log.h"
#include "esphome/core/time.h"

#include <esp_heap_caps.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace esphome::messenger {

static const char *const TAG = "messenger.account";

using mtproto::TlReader;
using mtproto::TlWriter;
using mtproto::tl_field;
using mtproto::tl_hash;
using mtproto::tl_skip_object;

namespace {

static constexpr uint32_t ID_VECTOR = 0x1cb5c415;
static constexpr uint32_t ID_INPUT_PEER_EMPTY = 0x7f3b18ea;
static constexpr uint32_t ID_INPUT_PEER_USER = 0xdde8a54c;
static constexpr uint32_t ID_INPUT_PEER_CHAT = 0x35a95cb9;
static constexpr uint32_t ID_INPUT_PEER_CHANNEL = 0x27bcbbfc;
static constexpr uint32_t ID_PEER_USER = 0x59511722;
static constexpr uint32_t ID_PEER_CHAT = 0x36c6019a;
static constexpr uint32_t ID_PEER_CHANNEL = 0xa2a5371e;
static constexpr uint32_t ID_CONTACTS_GET_CONTACTS = 0x5dd69e12;
static constexpr uint32_t ID_CONTACTS_CONTACTS = 0xeae87e42;
static constexpr uint32_t ID_MESSAGES_GET_DIALOGS = 0xa0f4cb4f;
static constexpr uint32_t ID_MESSAGES_DIALOGS = 0x15ba6c40;
static constexpr uint32_t ID_MESSAGES_DIALOGS_SLICE = 0x71e094f3;
static constexpr uint32_t ID_DIALOG = 0xfc89f7f3;
static constexpr uint32_t ID_MESSAGES_GET_HISTORY = 0x4423e6c5;
static constexpr uint32_t ID_MESSAGES_SEND_MESSAGE = 0x545cd15a;
static constexpr uint32_t ID_MESSAGES_READ_HISTORY = 0x0e306d3a;
static constexpr uint32_t ID_MESSAGE = 0x95ef6f2b;
static constexpr uint32_t ID_MESSAGE_SERVICE = 0x7a800e0a;
static constexpr uint32_t ID_USER = 0x31774388;
static constexpr uint32_t ID_CHAT = 0x41cbf256;
static constexpr uint32_t ID_CHAT_FORBIDDEN = 0x6592a1a7;
static constexpr uint32_t ID_CHANNEL = 0x1c32b11c;
static constexpr uint32_t ID_CHANNEL_FORBIDDEN = 0x17d493d5;
static constexpr uint32_t ID_STATUS_ONLINE = 0xedb93949;
static constexpr uint32_t ID_STATUS_OFFLINE = 0x008c703f;
static constexpr uint32_t ID_STATUS_RECENTLY = 0x7b197dc8;
static constexpr uint32_t ID_STATUS_LAST_WEEK = 0x541a1d1a;
static constexpr uint32_t ID_STATUS_LAST_MONTH = 0x65899777;
static constexpr uint32_t ID_UPDATE_SHORT = 0x78d4dec1;
static constexpr uint32_t ID_UPDATES = 0x74ae4240;
static constexpr uint32_t ID_UPDATES_COMBINED = 0x725b04c3;
static constexpr uint32_t ID_UPDATES_TOO_LONG = 0xe317af7e;
static constexpr uint32_t ID_UPDATE_SHORT_MESSAGE = 0x313bc7f8;
static constexpr uint32_t ID_UPDATE_SHORT_CHAT_MESSAGE = 0x4d6deea5;
static constexpr uint32_t ID_UPDATE_NEW_MESSAGE = 0x1f2b0afd;
static constexpr uint32_t ID_UPDATE_NEW_CHANNEL_MESSAGE = 0x62ba04d9;
static constexpr uint32_t ID_MEDIA_PHOTO = 0xe216eb63;
static constexpr uint32_t ID_MEDIA_DOCUMENT = 0x52d8ccd9;
static constexpr uint32_t ID_UPLOAD_SAVE_FILE_PART = 0xb304a621;
static constexpr uint32_t ID_INPUT_FILE = 0xf52ff27f;
static constexpr uint32_t ID_INPUT_MEDIA_UPLOADED_PHOTO = 0x7d8375da;
static constexpr uint32_t ID_INPUT_MEDIA_UPLOADED_DOCUMENT = 0x037c9330;
static constexpr uint32_t ID_DOCUMENT_ATTRIBUTE_AUDIO = 0x9852f9c6;
static constexpr uint32_t ID_DOCUMENT_ATTRIBUTE_FILENAME = 0x15590068;
static constexpr uint32_t ID_MESSAGES_SEND_MEDIA = 0x0330e77f;
static constexpr uint32_t ID_BOOL_TRUE = 0x997275b5;
static constexpr uint32_t ID_PHOTO = 0xfb197a65;
static constexpr uint32_t ID_PHOTO_SIZE = 0x75c78e60;
static constexpr uint32_t ID_PHOTO_SIZE_PROGRESSIVE = 0xfa3efb95;
static constexpr uint32_t ID_DOCUMENT = 0x8fd4c4d8;
static constexpr uint32_t ID_UPLOAD_GET_FILE = 0xbe5335be;
static constexpr uint32_t ID_UPLOAD_FILE = 0x096a18d5;
static constexpr uint32_t ID_INPUT_PHOTO_FILE_LOCATION = 0x40181ffe;
static constexpr uint32_t ID_INPUT_DOCUMENT_FILE_LOCATION = 0xbad07584;
static constexpr uint32_t ID_UPDATE_SHORT_SENT_MESSAGE = 0x9015e101;
static constexpr size_t UPLOAD_PART = 16384;
static constexpr size_t UPLOAD_MAX = 10 * 1024 * 1024;
static constexpr uint32_t UPLOAD_WAIT_MS = 60000;
static constexpr uint32_t DIALOG_LIMIT = 40;
static constexpr uint32_t HISTORY_LIMIT = 40;

void copy_text(char *dest, size_t size, const char *src, size_t length) {
  const size_t n = length < size - 1 ? length : size - 1;
  memcpy(dest, src, n);
  dest[n] = '\0';
}

bool read_int(TlReader object, const char *name, int32_t &value) {
  TlReader field;
  return tl_field(object, tl_hash(name), field) && field.i32(value);
}

bool read_long(TlReader object, const char *name, int64_t &value) {
  TlReader field;
  return tl_field(object, tl_hash(name), field) && field.i64(value);
}

bool read_text(TlReader object, const char *name, char *out, size_t size) {
  TlReader field;
  const uint8_t *data = nullptr;
  size_t length = 0;
  if (!tl_field(object, tl_hash(name), field) || !field.bytes(data, length)) {
    out[0] = '\0';
    return false;
  }
  copy_text(out, size, reinterpret_cast<const char *>(data), length);
  return true;
}

bool read_bytes(TlReader object, const char *name, const uint8_t *&data, size_t &length) {
  TlReader field;
  return tl_field(object, tl_hash(name), field) && field.bytes(data, length);
}

bool read_peer(TlReader peer, PeerKind &kind, int64_t &id) {
  uint32_t ctor = 0;
  if (!peer.u32(ctor) || !peer.i64(id))
    return false;
  kind = ctor == ID_PEER_USER ? PeerKind::USER : ctor == ID_PEER_CHAT ? PeerKind::CHAT
                                                : ctor == ID_PEER_CHANNEL ? PeerKind::CHANNEL
                                                                          : PeerKind::NONE;
  return kind != PeerKind::NONE;
}

bool vector_items(TlReader vector, TlReader &items, uint32_t &count) {
  uint32_t ctor = 0;
  if (!vector.u32(ctor) || ctor != ID_VECTOR || !vector.u32(count))
    return false;
  items = vector;
  return true;
}

bool field_vector(TlReader object, const char *name, TlReader &items, uint32_t &count) {
  TlReader field;
  return tl_field(object, tl_hash(name), field) && vector_items(field, items, count);
}

void user_name(TlReader user, char *out, size_t size) {
  char first[48];
  char last[48];
  read_text(user, "first_name", first, sizeof(first));
  read_text(user, "last_name", last, sizeof(last));
  if (first[0] == '\0' && last[0] == '\0') {
    char username[40];
    if (read_text(user, "username", username, sizeof(username))) {
      snprintf(out, size, "@%s", username);
      return;
    }
    snprintf(out, size, "%s", "Deleted account");
    return;
  }
  snprintf(out, size, "%s%s%s", first, (first[0] != '\0' && last[0] != '\0') ? " " : "", last);
}

void user_status(TlReader user, char *out, size_t size) {
  TlReader status;
  uint32_t ctor = 0;
  out[0] = '\0';
  if (!tl_field(user, tl_hash("status"), status) || !status.peek_u32(ctor))
    return;
  switch (ctor) {
    case ID_STATUS_ONLINE:
      snprintf(out, size, "%s", "online");
      break;
    case ID_STATUS_OFFLINE: {
      int32_t was = 0;
      if (read_int(status, "was_online", was) && was > 0) {
        ESPTime at = ESPTime::from_epoch_local(static_cast<time_t>(was));
        at.strftime(out, size, "seen %d.%m. %H:%M");
      }
      break;
    }
    case ID_STATUS_RECENTLY:
      snprintf(out, size, "%s", "recently");
      break;
    case ID_STATUS_LAST_WEEK:
      snprintf(out, size, "%s", "last week");
      break;
    case ID_STATUS_LAST_MONTH:
      snprintf(out, size, "%s", "last month");
      break;
    default:
      break;
  }
}

bool find_user(TlReader users, uint32_t count, int64_t id, TlReader &user) {
  for (uint32_t i = 0; i < count; i++) {
    TlReader start = users;
    uint32_t ctor = 0;
    if (!users.peek_u32(ctor))
      return false;
    if (ctor == ID_USER) {
      int64_t user_id = 0;
      if (read_long(start, "id", user_id) && user_id == id) {
        user = start;
        return true;
      }
    }
    if (!tl_skip_object(users))
      return false;
  }
  return false;
}

bool find_chat(TlReader chats, uint32_t count, PeerKind kind, int64_t id, TlReader &chat) {
  for (uint32_t i = 0; i < count; i++) {
    TlReader start = chats;
    uint32_t ctor = 0;
    if (!chats.peek_u32(ctor))
      return false;
    const bool channel = ctor == ID_CHANNEL || ctor == ID_CHANNEL_FORBIDDEN;
    const bool group = ctor == ID_CHAT || ctor == ID_CHAT_FORBIDDEN;
    if ((kind == PeerKind::CHANNEL && channel) || (kind == PeerKind::CHAT && group)) {
      int64_t chat_id = 0;
      if (read_long(start, "id", chat_id) && chat_id == id) {
        chat = start;
        return true;
      }
    }
    if (!tl_skip_object(chats))
      return false;
  }
  return false;
}

bool read_location(TlReader object, AccountFile &file) {
  const uint8_t *ref = nullptr;
  size_t ref_len = 0;
  if (!read_long(object, "id", file.id) || !read_long(object, "access_hash", file.access_hash) ||
      !read_bytes(object, "file_reference", ref, ref_len) || ref_len > sizeof(file.ref) ||
      !read_int(object, "dc_id", file.dc)) {
    file.id = 0;
    return false;
  }
  memcpy(file.ref, ref, ref_len);
  file.ref_len = static_cast<uint8_t>(ref_len);
  return true;
}

void read_photo(TlReader photo, AccountFile &file) {
  uint32_t ctor = 0;
  if (!photo.peek_u32(ctor) || ctor != ID_PHOTO || !read_location(photo, file))
    return;
  TlReader sizes;
  uint32_t count = 0;
  int64_t best = -1;
  int64_t view_best = -1;
  if (field_vector(photo, "sizes", sizes, count)) {
    for (uint32_t i = 0; i < count; i++) {
      TlReader item = sizes;
      uint32_t size_ctor = 0;
      if (!sizes.peek_u32(size_ctor))
        break;
      if (size_ctor == ID_PHOTO_SIZE || size_ctor == ID_PHOTO_SIZE_PROGRESSIVE) {
        int32_t w = 0;
        int32_t h = 0;
        const uint8_t *type = nullptr;
        size_t type_len = 0;
        read_int(item, "w", w);
        read_int(item, "h", h);
        const int64_t area = static_cast<int64_t>(w) * h;
        const bool fits = static_cast<uint32_t>(std::max(w, h)) <= TelegramAccount::VIEW_MAX_SIDE;
        if ((area > best || (fits && area > view_best)) && read_bytes(item, "type", type, type_len) && type_len > 0 &&
            type_len < sizeof(file.thumb)) {
          int32_t size = 0;
          if (size_ctor == ID_PHOTO_SIZE) {
            read_int(item, "size", size);
          } else {
            TlReader list;
            uint32_t n = 0;
            if (field_vector(item, "sizes", list, n)) {
              for (uint32_t j = 0; j < n; j++)
                list.i32(size);
            }
          }
          const uint32_t bytes = size > 0 ? static_cast<uint32_t>(size) : 0;
          if (area > best) {
            best = area;
            memcpy(file.thumb, type, type_len);
            file.thumb[type_len] = '\0';
            file.size = bytes;
          }
          if (fits && area > view_best) {
            view_best = area;
            memcpy(file.view_thumb, type, type_len);
            file.view_thumb[type_len] = '\0';
            file.view_size = bytes;
          }
        }
      }
      if (!tl_skip_object(sizes))
        break;
    }
  }
  if (best < 0) {
    file.id = 0;
    return;
  }
  if (view_best < 0) {
    memcpy(file.view_thumb, file.thumb, sizeof(file.thumb));
    file.view_size = file.size;
  }
  snprintf(file.name, sizeof(file.name), "photo_%lld.jpg", static_cast<long long>(file.id));
}

void read_document(TlReader document, uint8_t kind, AccountFile &file) {
  uint32_t ctor = 0;
  if (!document.peek_u32(ctor) || ctor != ID_DOCUMENT || !read_location(document, file))
    return;
  int64_t size = 0;
  read_long(document, "size", size);
  file.size = size > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<uint32_t>(size > 0 ? size : 0);
  file.thumb[0] = '\0';
  TlReader attributes;
  uint32_t count = 0;
  if (field_vector(document, "attributes", attributes, count)) {
    for (uint32_t i = 0; i < count; i++) {
      TlReader item = attributes;
      uint32_t attribute = 0;
      if (!attributes.peek_u32(attribute))
        break;
      if (attribute == ID_DOCUMENT_ATTRIBUTE_FILENAME)
        read_text(item, "file_name", file.name, sizeof(file.name));
      if (!tl_skip_object(attributes))
        break;
    }
  }
  for (char *c = file.name; *c != '\0'; c++) {
    if (*c == '/' || *c == '\\' || *c == ':')
      *c = '_';
  }
  if (file.name[0] != '\0')
    return;
  char mime[48];
  read_text(document, "mime_type", mime, sizeof(mime));
  const char *ext = ".bin";
  if (kind == 2 || strcmp(mime, "audio/ogg") == 0) {
    ext = ".ogg";
  } else if (kind == 3 || strcmp(mime, "video/mp4") == 0) {
    ext = ".mp4";
  } else if (strcmp(mime, "image/jpeg") == 0) {
    ext = ".jpg";
  } else if (strcmp(mime, "image/png") == 0) {
    ext = ".png";
  } else if (strcmp(mime, "audio/mpeg") == 0) {
    ext = ".mp3";
  } else if (strcmp(mime, "application/pdf") == 0) {
    ext = ".pdf";
  }
  snprintf(file.name, sizeof(file.name), "%s_%lld%s", kind == 2 ? "voice" : kind == 3 ? "video" : "file",
           static_cast<long long>(file.id), ext);
}

uint8_t read_media(TlReader message, char *label, size_t size, AccountFile *file) {
  TlReader media;
  uint32_t ctor = 0;
  label[0] = '\0';
  if (!tl_field(message, tl_hash("media"), media) || !media.peek_u32(ctor))
    return 0;
  if (ctor == ID_MEDIA_PHOTO) {
    snprintf(label, size, "%s", "[Photo]");
    TlReader photo;
    if (file != nullptr && tl_field(media, tl_hash("photo"), photo))
      read_photo(photo, *file);
    return 1;
  }
  if (ctor == ID_MEDIA_DOCUMENT) {
    TlReader flags_field;
    uint32_t flags = 0;
    if (tl_field(media, tl_hash("flags"), flags_field))
      flags_field.u32(flags);
    uint8_t kind = 4;
    if (flags & (1u << 8)) {
      snprintf(label, size, "%s", "[Voice message]");
      kind = 2;
    } else if (flags & (1u << 6)) {
      snprintf(label, size, "%s", "[Video]");
      kind = 3;
    } else {
      snprintf(label, size, "%s", "[File]");
    }
    TlReader document;
    if (file != nullptr && tl_field(media, tl_hash("document"), document))
      read_document(document, kind, *file);
    return kind;
  }
  snprintf(label, size, "%s", "[Media]");
  return 5;
}

void write_input_peer(TlWriter &w, const AccountPeer &peer) {
  switch (peer.kind) {
    case PeerKind::USER:
      w.u32(ID_INPUT_PEER_USER);
      w.i64(peer.id);
      w.i64(peer.access_hash);
      break;
    case PeerKind::CHAT:
      w.u32(ID_INPUT_PEER_CHAT);
      w.i64(peer.id);
      break;
    case PeerKind::CHANNEL:
      w.u32(ID_INPUT_PEER_CHANNEL);
      w.i64(peer.id);
      w.i64(peer.access_hash);
      break;
    default:
      w.u32(ID_INPUT_PEER_EMPTY);
      break;
  }
}

template<typename T> T *alloc_psram(size_t count) {
  return static_cast<T *>(heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

}

void TelegramAccount::attach(mtproto::Mtproto *mtproto, Component *owner) {
  this->mtproto_ = mtproto;
  this->owner_ = owner;
  mtproto->add_update_listener([this](TlReader updates) { this->on_updates_(updates); });
  mtproto->add_on_state_callback([this](mtproto::State state) { this->on_state(state); });
}

bool TelegramAccount::allocate_() {
  if (this->chats_ != nullptr)
    return true;
  auto *chats = alloc_psram<AccountChat>(MAX_CHATS);
  auto *contacts = alloc_psram<AccountContact>(MAX_CONTACTS);
  auto *incoming = alloc_psram<AccountIncomingFile>(MAX_INCOMING_FILES);
  if (chats == nullptr || contacts == nullptr || incoming == nullptr) {
    heap_caps_free(chats);
    heap_caps_free(contacts);
    heap_caps_free(incoming);
    ESP_LOGE(TAG, "No PSRAM for the chats");
    return false;
  }
  LockGuard guard(this->lock_);
  this->chats_ = chats;
  this->contacts_ = contacts;
  this->incoming_ = incoming;
  this->incoming_head_ = 0;
  this->incoming_tail_ = 0;
  this->chat_count_ = 0;
  this->contact_count_ = 0;
  this->started_ = true;
  return true;
}

void TelegramAccount::release_() {
  {
    LockGuard guard(this->lock_);
    if (this->chats_ == nullptr)
      return;
    this->started_ = false;
    heap_caps_free(this->chats_);
    heap_caps_free(this->contacts_);
    heap_caps_free(this->messages_);
    heap_caps_free(this->incoming_);
    this->chats_ = nullptr;
    this->contacts_ = nullptr;
    this->messages_ = nullptr;
    this->incoming_ = nullptr;
    this->incoming_head_ = 0;
    this->incoming_tail_ = 0;
    this->chat_count_ = 0;
    this->contact_count_ = 0;
    this->message_count_ = 0;
    this->open_ = AccountPeer{};
    this->open_title_[0] = '\0';
  }
  this->notify_(CHANGED_CHATS | CHANGED_CONTACTS | CHANGED_MESSAGES);
}

void TelegramAccount::update_() {
  if (!this->logged_in_ || !this->active_) {
    this->release_();
    return;
  }
  if (this->started_ || !this->allocate_())
    return;
  this->refresh();
}

void TelegramAccount::set_active(bool active) {
  this->active_ = active;
  this->update_();
}

void TelegramAccount::on_state(mtproto::State state) {
  if (state == mtproto::State::LOGGED_IN) {
    this->logged_in_ = true;
    if (this->started_) {
      this->refresh();
    } else {
      this->update_();
    }
    return;
  }
  if (state == mtproto::State::LOGIN_QR || state == mtproto::State::NO_API) {
    this->logged_in_ = false;
    this->update_();
  }
}

void TelegramAccount::refresh() {
  if (!this->started_)
    return;
  this->request_dialogs_();
  this->request_contacts_();
  AccountPeer peer;
  {
    LockGuard guard(this->lock_);
    peer = this->open_;
  }
  if (peer.kind != PeerKind::NONE)
    this->request_history_(peer);
}

void TelegramAccount::notify_(uint8_t what) {
  this->changes_.fetch_or(what);
  if (this->owner_ != nullptr)
    this->owner_->enable_loop_soon_any_context();
}

void TelegramAccount::request_contacts_() {
  uint8_t body[12];
  TlWriter w(body, sizeof(body));
  w.u32(ID_CONTACTS_GET_CONTACTS);
  w.i64(0);
  this->mtproto_->invoke(w.data(), w.size(), [this](const char *error, TlReader result) {
    if (error == nullptr)
      this->on_contacts_(result);
  });
}

void TelegramAccount::request_dialogs_() {
  uint8_t body[40];
  TlWriter w(body, sizeof(body));
  w.u32(ID_MESSAGES_GET_DIALOGS);
  w.u32(0);
  w.i32(0);
  w.i32(0);
  w.u32(ID_INPUT_PEER_EMPTY);
  w.i32(DIALOG_LIMIT);
  w.i64(0);
  this->mtproto_->invoke(w.data(), w.size(), [this](const char *error, TlReader result) {
    if (error == nullptr) {
      this->on_dialogs_(result);
    } else {
      ESP_LOGW(TAG, "Chats: %s", error);
    }
  });
}

void TelegramAccount::request_history_(const AccountPeer &peer) {
  uint8_t body[64];
  TlWriter w(body, sizeof(body));
  w.u32(ID_MESSAGES_GET_HISTORY);
  write_input_peer(w, peer);
  w.i32(0);
  w.i32(0);
  w.i32(0);
  w.i32(HISTORY_LIMIT);
  w.i32(0);
  w.i32(0);
  w.i64(0);
  const uint32_t serial = ++this->history_serial_;
  this->mtproto_->invoke(w.data(), w.size(), [this, peer, serial](const char *error, TlReader result) {
    if (error == nullptr && serial == this->history_serial_)
      this->on_history_(peer, result);
  });
}

void TelegramAccount::mark_read_(const AccountPeer &peer, int32_t max_id) {
  if (peer.kind == PeerKind::CHANNEL || max_id <= 0)
    return;
  uint8_t body[40];
  TlWriter w(body, sizeof(body));
  w.u32(ID_MESSAGES_READ_HISTORY);
  write_input_peer(w, peer);
  w.i32(max_id);
  this->mtproto_->invoke(w.data(), w.size(), nullptr);
}

void TelegramAccount::on_contacts_(TlReader result) {
  uint32_t ctor = 0;
  if (!result.peek_u32(ctor) || ctor != ID_CONTACTS_CONTACTS)
    return;
  TlReader users;
  uint32_t count = 0;
  if (!field_vector(result, "users", users, count))
    return;
  LockGuard guard(this->lock_);
  if (this->contacts_ == nullptr)
    return;
  size_t n = 0;
  for (uint32_t i = 0; i < count && n < MAX_CONTACTS; i++) {
    TlReader user = users;
    uint32_t user_ctor = 0;
    if (!users.peek_u32(user_ctor))
      break;
    if (user_ctor == ID_USER) {
      AccountContact &contact = this->contacts_[n];
      contact.peer.kind = PeerKind::USER;
      if (read_long(user, "id", contact.peer.id)) {
        contact.peer.access_hash = 0;
        read_long(user, "access_hash", contact.peer.access_hash);
        user_name(user, contact.name, sizeof(contact.name));
        user_status(user, contact.status, sizeof(contact.status));
        n++;
      }
    }
    if (!tl_skip_object(users))
      break;
  }
  for (size_t i = 1; i < n; i++) {
    AccountContact key = this->contacts_[i];
    size_t j = i;
    while (j > 0 && strcasecmp(this->contacts_[j - 1].name, key.name) > 0) {
      this->contacts_[j] = this->contacts_[j - 1];
      j--;
    }
    this->contacts_[j] = key;
  }
  this->contact_count_ = n;
  this->notify_(CHANGED_CONTACTS);
}

void TelegramAccount::on_dialogs_(TlReader result) {
  uint32_t ctor = 0;
  if (!result.peek_u32(ctor) || (ctor != ID_MESSAGES_DIALOGS && ctor != ID_MESSAGES_DIALOGS_SLICE))
    return;
  TlReader dialogs;
  TlReader messages;
  TlReader chats;
  TlReader users;
  uint32_t dialog_count = 0;
  uint32_t message_count = 0;
  uint32_t chat_count = 0;
  uint32_t user_count = 0;
  if (!field_vector(result, "dialogs", dialogs, dialog_count) ||
      !field_vector(result, "messages", messages, message_count) ||
      !field_vector(result, "chats", chats, chat_count) || !field_vector(result, "users", users, user_count))
    return;
  LockGuard guard(this->lock_);
  if (this->chats_ == nullptr)
    return;
  size_t n = 0;
  for (uint32_t i = 0; i < dialog_count && n < MAX_CHATS; i++) {
    TlReader dialog = dialogs;
    uint32_t dialog_ctor = 0;
    if (!dialogs.peek_u32(dialog_ctor))
      break;
    if (dialog_ctor == ID_DIALOG) {
      AccountChat &chat = this->chats_[n];
      chat = AccountChat{};
      TlReader peer_field;
      int32_t top = 0;
      if (tl_field(dialog, tl_hash("peer"), peer_field) && read_peer(peer_field, chat.peer.kind, chat.peer.id)) {
        read_int(dialog, "top_message", top);
        read_int(dialog, "unread_count", chat.unread);
        read_int(dialog, "read_inbox_max_id", chat.read_max);
        if (chat.peer.kind == PeerKind::USER) {
          TlReader user;
          if (find_user(users, user_count, chat.peer.id, user)) {
            read_long(user, "access_hash", chat.peer.access_hash);
            user_name(user, chat.title, sizeof(chat.title));
          }
        } else {
          TlReader group;
          if (find_chat(chats, chat_count, chat.peer.kind, chat.peer.id, group)) {
            read_text(group, "title", chat.title, sizeof(chat.title));
            if (chat.peer.kind == PeerKind::CHANNEL)
              read_long(group, "access_hash", chat.peer.access_hash);
          }
        }
        TlReader scan = messages;
        for (uint32_t m = 0; m < message_count; m++) {
          TlReader message = scan;
          uint32_t message_ctor = 0;
          if (!scan.peek_u32(message_ctor))
            break;
          int32_t id = 0;
          TlReader message_peer;
          PeerKind kind = PeerKind::NONE;
          int64_t peer_id = 0;
          if ((message_ctor == ID_MESSAGE || message_ctor == ID_MESSAGE_SERVICE) && read_int(message, "id", id) &&
              id == top && tl_field(message, tl_hash("peer_id"), message_peer) &&
              read_peer(message_peer, kind, peer_id) && kind == chat.peer.kind && peer_id == chat.peer.id) {
            int32_t date = 0;
            read_int(message, "date", date);
            chat.date = static_cast<uint32_t>(date);
            if (message_ctor == ID_MESSAGE) {
              char label[24];
              const uint8_t media = read_media(message, label, sizeof(label), nullptr);
              if (!read_text(message, "message", chat.last, sizeof(chat.last)) || chat.last[0] == '\0') {
                if (media != 0)
                  copy_text(chat.last, sizeof(chat.last), label, strlen(label));
              }
            }
            break;
          }
          if (!tl_skip_object(scan))
            break;
        }
        if (chat.title[0] == '\0')
          snprintf(chat.title, sizeof(chat.title), "%lld", static_cast<long long>(chat.peer.id));
        n++;
      }
    }
    if (!tl_skip_object(dialogs))
      break;
  }
  this->chat_count_ = n;
  this->notify_(CHANGED_CHATS);
}

void TelegramAccount::on_history_(const AccountPeer &peer, TlReader result) {
  TlReader messages;
  TlReader users;
  uint32_t message_count = 0;
  uint32_t user_count = 0;
  if (!field_vector(result, "messages", messages, message_count) ||
      !field_vector(result, "users", users, user_count))
    return;
  LockGuard guard(this->lock_);
  if (this->messages_ == nullptr || this->open_.kind != peer.kind || this->open_.id != peer.id)
    return;
  size_t n = 0;
  int32_t max_id = 0;
  AccountMessage *temp = this->messages_;
  for (uint32_t i = 0; i < message_count && n < MAX_MESSAGES; i++) {
    TlReader message = messages;
    uint32_t ctor = 0;
    if (!messages.peek_u32(ctor))
      break;
    if (ctor == ID_MESSAGE || ctor == ID_MESSAGE_SERVICE) {
      AccountMessage &entry = temp[n];
      entry = AccountMessage{};
      TlReader flags_field;
      uint32_t flags = 0;
      if (tl_field(message, tl_hash("flags"), flags_field))
        flags_field.u32(flags);
      entry.out = (flags & (1u << 1)) != 0;
      read_int(message, "id", entry.id);
      int32_t date = 0;
      read_int(message, "date", date);
      entry.date = static_cast<uint32_t>(date);
      if (ctor == ID_MESSAGE) {
        char label[24];
        entry.media = read_media(message, label, sizeof(label), &entry.file);
        read_text(message, "message", entry.text, sizeof(entry.text));
        if (entry.media != 0) {
          char combined[sizeof(entry.text)];
          snprintf(combined, sizeof(combined), "%s%s%s", label, entry.text[0] != '\0' ? " " : "", entry.text);
          copy_text(entry.text, sizeof(entry.text), combined, strlen(combined));
        }
      } else {
        snprintf(entry.text, sizeof(entry.text), "%s", "[Service message]");
      }
      if (!entry.out) {
        TlReader from_field;
        PeerKind kind = PeerKind::NONE;
        int64_t from_id = 0;
        if (tl_field(message, tl_hash("from_id"), from_field) && read_peer(from_field, kind, from_id) &&
            kind == PeerKind::USER) {
          TlReader user;
          if (find_user(users, user_count, from_id, user))
            user_name(user, entry.from, sizeof(entry.from));
        }
      }
      if (entry.id > max_id)
        max_id = entry.id;
      n++;
    }
    if (!tl_skip_object(messages))
      break;
  }
  for (size_t i = 0; i < n / 2; i++) {
    AccountMessage swap = temp[i];
    temp[i] = temp[n - 1 - i];
    temp[n - 1 - i] = swap;
  }
  this->message_count_ = n;
  AccountChat *chat = this->find_chat_(peer.kind, peer.id);
  if (chat != nullptr && chat->unread != 0) {
    chat->unread = 0;
    this->notify_(CHANGED_CHATS);
  }
  this->notify_(CHANGED_MESSAGES);
  this->mark_read_(peer, max_id);
}

void TelegramAccount::on_updates_(TlReader updates) {
  if (!this->started_)
    return;
  uint32_t ctor = 0;
  if (!updates.peek_u32(ctor))
    return;
  if (ctor == ID_UPDATES_TOO_LONG) {
    this->refresh();
    return;
  }
  if (ctor == ID_UPDATE_SHORT_MESSAGE || ctor == ID_UPDATE_SHORT_CHAT_MESSAGE) {
    TlReader flags_field;
    uint32_t flags = 0;
    if (tl_field(updates, tl_hash("flags"), flags_field))
      flags_field.u32(flags);
    int32_t id = 0;
    int32_t date = 0;
    read_int(updates, "id", id);
    read_int(updates, "date", date);
    char text[512];
    read_text(updates, "message", text, sizeof(text));
    AccountPeer peer{};
    if (ctor == ID_UPDATE_SHORT_MESSAGE) {
      peer.kind = PeerKind::USER;
      read_long(updates, "user_id", peer.id);
    } else {
      peer.kind = PeerKind::CHAT;
      read_long(updates, "chat_id", peer.id);
    }
    bool known = false;
    {
      LockGuard guard(this->lock_);
      known = this->find_chat_(peer.kind, peer.id) != nullptr;
    }
    if (!known) {
      this->request_dialogs_();
      return;
    }
    this->add_incoming_(peer, nullptr, id, (flags & (1u << 1)) != 0, static_cast<uint32_t>(date), "", text, 0,
                        nullptr);
    return;
  }
  TlReader update;
  if (ctor == ID_UPDATE_SHORT) {
    if (tl_field(updates, tl_hash("update"), update)) {
      uint32_t update_ctor = 0;
      if (update.peek_u32(update_ctor) &&
          (update_ctor == ID_UPDATE_NEW_MESSAGE || update_ctor == ID_UPDATE_NEW_CHANNEL_MESSAGE)) {
        TlReader message;
        if (tl_field(update, tl_hash("message"), message))
          this->on_new_message_(message, TlReader(), TlReader());
      }
    }
    return;
  }
  if (ctor != ID_UPDATES && ctor != ID_UPDATES_COMBINED)
    return;
  TlReader list;
  uint32_t count = 0;
  if (!field_vector(updates, "updates", list, count))
    return;
  TlReader users_vector;
  TlReader chats_vector;
  tl_field(updates, tl_hash("users"), users_vector);
  tl_field(updates, tl_hash("chats"), chats_vector);
  for (uint32_t i = 0; i < count; i++) {
    TlReader item = list;
    uint32_t update_ctor = 0;
    if (!list.peek_u32(update_ctor))
      return;
    if (update_ctor == ID_UPDATE_NEW_MESSAGE || update_ctor == ID_UPDATE_NEW_CHANNEL_MESSAGE) {
      TlReader message;
      if (tl_field(item, tl_hash("message"), message))
        this->on_new_message_(message, users_vector, chats_vector);
    }
    if (!tl_skip_object(list))
      return;
  }
}

void TelegramAccount::on_new_message_(TlReader message, TlReader users_vector, TlReader chats_vector) {
  uint32_t ctor = 0;
  if (!message.peek_u32(ctor) || ctor != ID_MESSAGE)
    return;
  TlReader flags_field;
  uint32_t flags = 0;
  if (tl_field(message, tl_hash("flags"), flags_field))
    flags_field.u32(flags);
  const bool out = (flags & (1u << 1)) != 0;
  int32_t id = 0;
  int32_t date = 0;
  read_int(message, "id", id);
  read_int(message, "date", date);
  TlReader peer_field;
  AccountPeer peer{};
  if (!tl_field(message, tl_hash("peer_id"), peer_field) || !read_peer(peer_field, peer.kind, peer.id))
    return;
  char text[512];
  char label[24];
  AccountFile file{};
  const uint8_t media = read_media(message, label, sizeof(label), &file);
  read_text(message, "message", text, sizeof(text));
  if (media != 0) {
    char combined[sizeof(text)];
    snprintf(combined, sizeof(combined), "%s%s%s", label, text[0] != '\0' ? " " : "", text);
    copy_text(text, sizeof(text), combined, strlen(combined));
  }
  char title[64] = "";
  char from[48] = "";
  TlReader users;
  TlReader chats;
  uint32_t user_count = 0;
  uint32_t chat_count = 0;
  const bool have_users = users_vector.valid() && vector_items(users_vector, users, user_count);
  const bool have_chats = chats_vector.valid() && vector_items(chats_vector, chats, chat_count);
  if (peer.kind == PeerKind::USER && have_users) {
    TlReader user;
    if (find_user(users, user_count, peer.id, user)) {
      read_long(user, "access_hash", peer.access_hash);
      user_name(user, title, sizeof(title));
    }
  } else if (peer.kind != PeerKind::USER && have_chats) {
    TlReader group;
    if (find_chat(chats, chat_count, peer.kind, peer.id, group)) {
      read_text(group, "title", title, sizeof(title));
      if (peer.kind == PeerKind::CHANNEL)
        read_long(group, "access_hash", peer.access_hash);
    }
  }
  if (!out && peer.kind != PeerKind::USER && have_users) {
    TlReader from_field;
    PeerKind kind = PeerKind::NONE;
    int64_t from_id = 0;
    TlReader user;
    if (tl_field(message, tl_hash("from_id"), from_field) && read_peer(from_field, kind, from_id) &&
        kind == PeerKind::USER && find_user(users, user_count, from_id, user))
      user_name(user, from, sizeof(from));
  }
  bool known = false;
  {
    LockGuard guard(this->lock_);
    known = this->find_chat_(peer.kind, peer.id) != nullptr;
  }
  if (!known && peer.access_hash == 0 && peer.kind != PeerKind::CHAT) {
    this->request_dialogs_();
    return;
  }
  this->add_incoming_(peer, title[0] != '\0' ? title : nullptr, id, out, static_cast<uint32_t>(date), from, text,
                      media, file.id != 0 ? &file : nullptr);
}

void TelegramAccount::add_incoming_(const AccountPeer &peer, const char *title, int32_t id, bool out, uint32_t date,
                                    const char *from, const char *text, uint8_t media, const AccountFile *file) {
  bool open = false;
  bool files = false;
  AccountPeer read_target{};
  {
    LockGuard guard(this->lock_);
    if (this->chats_ == nullptr)
      return;
    AccountChat *chat = this->find_chat_(peer.kind, peer.id);
    size_t index = 0;
    if (chat == nullptr) {
      if (this->chat_count_ < MAX_CHATS)
        this->chat_count_++;
      for (size_t i = this->chat_count_ - 1; i > 0; i--)
        this->chats_[i] = this->chats_[i - 1];
      chat = &this->chats_[0];
      *chat = AccountChat{};
      chat->peer = peer;
      copy_text(chat->title, sizeof(chat->title), title != nullptr ? title : "", title != nullptr ? strlen(title) : 0);
    } else {
      index = static_cast<size_t>(chat - this->chats_);
      if (chat->peer.access_hash == 0)
        chat->peer.access_hash = peer.access_hash;
      if (title != nullptr && chat->title[0] == '\0')
        copy_text(chat->title, sizeof(chat->title), title, strlen(title));
      this->move_to_top_(index);
      chat = &this->chats_[0];
    }
    copy_text(chat->last, sizeof(chat->last), text, strlen(text));
    chat->date = date;
    if (file != nullptr && !out && this->incoming_ != nullptr) {
      if (this->incoming_tail_ - this->incoming_head_ >= MAX_INCOMING_FILES)
        this->incoming_head_++;
      AccountIncomingFile &entry = this->incoming_[this->incoming_tail_ % MAX_INCOMING_FILES];
      entry.message_id = id;
      entry.chat_id = peer.id;
      entry.kind = media;
      const char *who = from != nullptr && from[0] != '\0' ? from : chat->title;
      copy_text(entry.from, sizeof(entry.from), who, strlen(who));
      entry.file = *file;
      this->incoming_tail_++;
      files = true;
    }
    open = this->messages_ != nullptr && this->open_.kind == peer.kind && this->open_.id == peer.id;
    if (!out && !open)
      chat->unread++;
    if (open) {
      if (this->message_count_ == MAX_MESSAGES) {
        memmove(this->messages_, this->messages_ + 1, sizeof(AccountMessage) * (MAX_MESSAGES - 1));
        this->message_count_--;
      }
      AccountMessage &entry = this->messages_[this->message_count_++];
      entry = AccountMessage{};
      entry.id = id;
      entry.out = out;
      entry.media = media;
      entry.date = date;
      copy_text(entry.from, sizeof(entry.from), from, strlen(from));
      copy_text(entry.text, sizeof(entry.text), text, strlen(text));
      if (file != nullptr)
        entry.file = *file;
      read_target = this->open_;
    }
  }
  this->notify_((open ? (CHANGED_CHATS | CHANGED_MESSAGES) : CHANGED_CHATS) | (files ? CHANGED_FILES : 0));
  if (open && !out)
    this->mark_read_(read_target, id);
}

AccountChat *TelegramAccount::find_chat_(PeerKind kind, int64_t id) {
  for (size_t i = 0; i < this->chat_count_; i++) {
    if (this->chats_[i].peer.kind == kind && this->chats_[i].peer.id == id)
      return &this->chats_[i];
  }
  return nullptr;
}

void TelegramAccount::move_to_top_(size_t index) {
  if (index == 0 || index >= this->chat_count_)
    return;
  AccountChat chat = this->chats_[index];
  for (size_t i = index; i > 0; i--)
    this->chats_[i] = this->chats_[i - 1];
  this->chats_[0] = chat;
}

size_t TelegramAccount::chat_count() const {
  LockGuard guard(this->lock_);
  return this->chat_count_;
}

std::string TelegramAccount::chat_title(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->chat_count_ ? this->chats_[index].title : "";
}

std::string TelegramAccount::chat_last(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->chat_count_ ? this->chats_[index].last : "";
}

uint32_t TelegramAccount::chat_date(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->chat_count_ ? this->chats_[index].date : 0;
}

int32_t TelegramAccount::chat_unread(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->chat_count_ ? this->chats_[index].unread : 0;
}

size_t TelegramAccount::contact_count() const {
  LockGuard guard(this->lock_);
  return this->contact_count_;
}

std::string TelegramAccount::contact_name(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->contact_count_ ? this->contacts_[index].name : "";
}

std::string TelegramAccount::contact_status(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->contact_count_ ? this->contacts_[index].status : "";
}

size_t TelegramAccount::message_count() const {
  LockGuard guard(this->lock_);
  return this->message_count_;
}

std::string TelegramAccount::message_text(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->message_count_ ? this->messages_[index].text : "";
}

std::string TelegramAccount::message_from(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->message_count_ ? this->messages_[index].from : "";
}

bool TelegramAccount::message_out(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->message_count_ && this->messages_[index].out;
}

uint32_t TelegramAccount::message_date(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->message_count_ ? this->messages_[index].date : 0;
}

uint8_t TelegramAccount::message_media(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->message_count_ ? this->messages_[index].media : 0;
}

int32_t TelegramAccount::message_id(size_t index) const {
  LockGuard guard(this->lock_);
  return index < this->message_count_ ? this->messages_[index].id : 0;
}

std::string TelegramAccount::message_file_name(size_t index) const {
  LockGuard guard(this->lock_);
  if (index >= this->message_count_ || this->messages_[index].file.id == 0)
    return "";
  return this->messages_[index].file.name;
}

bool TelegramAccount::file_of(int32_t message_id, AccountFile *file) const {
  LockGuard guard(this->lock_);
  for (size_t i = 0; i < this->message_count_; i++) {
    if (this->messages_[i].id == message_id && this->messages_[i].file.id != 0) {
      *file = this->messages_[i].file;
      return true;
    }
  }
  if (this->incoming_ != nullptr) {
    for (uint32_t n = this->incoming_head_; n != this->incoming_tail_; n++) {
      const AccountIncomingFile &entry = this->incoming_[n % MAX_INCOMING_FILES];
      if (entry.message_id == message_id && entry.file.id != 0) {
        *file = entry.file;
        return true;
      }
    }
  }
  return false;
}

bool TelegramAccount::take_incoming_file(AccountIncomingFile *file) {
  LockGuard guard(this->lock_);
  if (this->incoming_ == nullptr || this->incoming_head_ == this->incoming_tail_)
    return false;
  *file = this->incoming_[this->incoming_head_ % MAX_INCOMING_FILES];
  this->incoming_head_++;
  return true;
}

bool TelegramAccount::download_part(const AccountFile &file, int64_t offset, uint8_t *out, size_t capacity,
                                    size_t *got, std::string *error) {
  if (!this->logged_in_) {
    *error = "Telegram not logged in";
    return false;
  }
  uint8_t body[192];
  TlWriter w(body, sizeof(body));
  w.u32(ID_UPLOAD_GET_FILE);
  w.u32(0);
  w.u32(file.thumb[0] != '\0' ? ID_INPUT_PHOTO_FILE_LOCATION : ID_INPUT_DOCUMENT_FILE_LOCATION);
  w.i64(file.id);
  w.i64(file.access_hash);
  w.bytes(file.ref, file.ref_len);
  w.string(file.thumb);
  w.i64(offset);
  w.i32(static_cast<int32_t>(capacity));
  *got = 0;
  if (!w.ok()) {
    *error = "request too large";
    return false;
  }
  return this->call_wait_(file.dc, w.data(), w.size(), error, [out, capacity, got](TlReader result) {
    uint32_t ctor = 0;
    const uint8_t *data = nullptr;
    size_t length = 0;
    if (!result.peek_u32(ctor) || ctor != ID_UPLOAD_FILE || !read_bytes(result, "bytes", data, length) ||
        length > capacity)
      return false;
    memcpy(out, data, length);
    *got = length;
    return true;
  });
}

void TelegramAccount::end_download() {
  if (this->mtproto_ != nullptr)
    this->mtproto_->close_dc();
}

void TelegramAccount::open_peer_(const AccountPeer &peer, const char *title) {
  if (!this->started_)
    return;
  AccountMessage *messages = this->messages_ == nullptr ? alloc_psram<AccountMessage>(MAX_MESSAGES) : nullptr;
  {
    LockGuard guard(this->lock_);
    if (this->chats_ == nullptr) {
      heap_caps_free(messages);
      return;
    }
    if (this->messages_ == nullptr)
      this->messages_ = messages;
    if (this->messages_ == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for the chat");
      return;
    }
    this->open_ = peer;
    copy_text(this->open_title_, sizeof(this->open_title_), title, strlen(title));
    this->message_count_ = 0;
  }
  this->notify_(CHANGED_MESSAGES);
  this->request_history_(peer);
}

bool TelegramAccount::open_chat(size_t index) {
  AccountPeer peer;
  char title[64];
  {
    LockGuard guard(this->lock_);
    if (index >= this->chat_count_)
      return false;
    peer = this->chats_[index].peer;
    copy_text(title, sizeof(title), this->chats_[index].title, strlen(this->chats_[index].title));
  }
  this->open_peer_(peer, title);
  return true;
}

bool TelegramAccount::open_contact(size_t index) {
  AccountPeer peer;
  char title[64];
  {
    LockGuard guard(this->lock_);
    if (index >= this->contact_count_)
      return false;
    peer = this->contacts_[index].peer;
    copy_text(title, sizeof(title), this->contacts_[index].name, strlen(this->contacts_[index].name));
  }
  this->open_peer_(peer, title);
  return true;
}

void TelegramAccount::close_chat() {
  LockGuard guard(this->lock_);
  this->open_ = AccountPeer{};
  this->open_title_[0] = '\0';
  this->message_count_ = 0;
  heap_caps_free(this->messages_);
  this->messages_ = nullptr;
}

bool TelegramAccount::has_open_chat() const {
  LockGuard guard(this->lock_);
  return this->open_.kind != PeerKind::NONE;
}

std::string TelegramAccount::open_title() const {
  LockGuard guard(this->lock_);
  return this->open_title_;
}

int64_t TelegramAccount::open_user_id() const {
  LockGuard guard(this->lock_);
  return this->open_.kind == PeerKind::USER ? this->open_.id : 0;
}

bool TelegramAccount::open_user(int64_t *id, int64_t *access_hash) const {
  LockGuard guard(this->lock_);
  if (this->open_.kind != PeerKind::USER || this->open_.id == 0)
    return false;
  *id = this->open_.id;
  *access_hash = this->open_.access_hash;
  return true;
}

bool TelegramAccount::send_text(const std::string &text) {
  if (!this->started_ || text.empty())
    return false;
  AccountPeer peer;
  {
    LockGuard guard(this->lock_);
    peer = this->open_;
  }
  if (peer.kind == PeerKind::NONE)
    return false;
  const size_t capacity = 64 + text.size() + 8;
  auto *body = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr)
    return false;
  TlWriter w(body, capacity);
  w.u32(ID_MESSAGES_SEND_MESSAGE);
  w.u32(0);
  write_input_peer(w, peer);
  w.string(text.c_str());
  uint64_t random_id = 0;
  esp_fill_random(&random_id, sizeof(random_id));
  w.u64(random_id);
  const std::string copy = text;
  const bool ok = w.ok() && this->mtproto_->invoke(w.data(), w.size(), [this, peer, copy](const char *error, TlReader result) {
    if (error != nullptr) {
      ESP_LOGW(TAG, "Not sent: %s", error);
      return;
    }
    int32_t id = 0;
    int32_t date = 0;
    uint32_t ctor = 0;
    if (result.peek_u32(ctor) && ctor == ID_UPDATE_SHORT_SENT_MESSAGE) {
      read_int(result, "id", id);
      read_int(result, "date", date);
      this->add_incoming_(peer, nullptr, id, true, static_cast<uint32_t>(date), "", copy.c_str(), 0, nullptr);
    }
  });
  heap_caps_free(body);
  return ok;
}

bool TelegramAccount::call_wait_(int dc, const uint8_t *body, size_t length, std::string *error,
                                 std::function<bool(TlReader)> &&on_result) {
  uint32_t serial = 0;
  {
    LockGuard guard(this->wait_lock_);
    serial = ++this->wait_serial_;
    this->waiter_ = xTaskGetCurrentTaskHandle();
    this->done_ok_ = false;
    this->done_error_[0] = '\0';
  }
  ulTaskNotifyTake(pdTRUE, 0);
  const bool queued = this->mtproto_->invoke_dc(
      dc, body, length, [this, serial, handler = std::move(on_result)](const char *err, TlReader result) {
        LockGuard guard(this->wait_lock_);
        if (serial != this->wait_serial_)
          return;
        if (err != nullptr) {
          copy_text(this->done_error_, sizeof(this->done_error_), err, strlen(err));
        } else {
          this->done_ok_ = handler(result);
          if (!this->done_ok_)
            copy_text(this->done_error_, sizeof(this->done_error_), "refused", 7);
        }
        xTaskNotifyGive(this->waiter_);
      });
  if (!queued) {
    *error = "no memory";
    return false;
  }
  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UPLOAD_WAIT_MS)) == 0) {
    LockGuard guard(this->wait_lock_);
    this->wait_serial_++;
    *error = "no answer from Telegram";
    return false;
  }
  LockGuard guard(this->wait_lock_);
  if (!this->done_ok_)
    *error = this->done_error_;
  return this->done_ok_;
}

bool TelegramAccount::send_media(uint8_t kind, const uint8_t *data, size_t length, const char *name, const char *mime,
                                 const char *caption, uint32_t duration, std::string *error) {
  if (!this->started_) {
    *error = "Telegram not logged in";
    return false;
  }
  if (length == 0 || length > UPLOAD_MAX) {
    *error = "file empty or larger than 10 MB";
    return false;
  }
  AccountPeer peer;
  {
    LockGuard guard(this->lock_);
    peer = this->open_;
  }
  if (peer.kind == PeerKind::NONE) {
    *error = "no chat open";
    return false;
  }
  int64_t file_id = 0;
  esp_fill_random(&file_id, sizeof(file_id));
  const size_t parts = (length + UPLOAD_PART - 1) / UPLOAD_PART;
  const size_t capacity = 32 + UPLOAD_PART;
  auto *body = static_cast<uint8_t *>(heap_caps_malloc(capacity + 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (body == nullptr) {
    *error = "no memory";
    return false;
  }
  bool ok = true;
  for (size_t part = 0; part < parts && ok; part++) {
    const size_t offset = part * UPLOAD_PART;
    const size_t chunk = length - offset < UPLOAD_PART ? length - offset : UPLOAD_PART;
    TlWriter w(body, capacity);
    w.u32(ID_UPLOAD_SAVE_FILE_PART);
    w.i64(file_id);
    w.i32(static_cast<int32_t>(part));
    w.bytes(data + offset, chunk);
    ok = w.ok() && this->call_wait_(0, w.data(), w.size(), error, [](TlReader result) {
      uint32_t ctor = 0;
      return result.peek_u32(ctor) && ctor == ID_BOOL_TRUE;
    });
  }
  if (ok) {
    TlWriter w(body, capacity + 1024);
    w.u32(ID_MESSAGES_SEND_MEDIA);
    w.u32(0);
    write_input_peer(w, peer);
    if (kind == 1) {
      w.u32(ID_INPUT_MEDIA_UPLOADED_PHOTO);
      w.u32(0);
    } else {
      w.u32(ID_INPUT_MEDIA_UPLOADED_DOCUMENT);
      w.u32(0);
    }
    w.u32(ID_INPUT_FILE);
    w.i64(file_id);
    w.i32(static_cast<int32_t>(parts));
    w.string(name);
    w.string("");
    if (kind != 1) {
      w.string(mime);
      const bool voice = kind == 2;
      w.vector(voice ? 2 : 1);
      if (voice) {
        w.u32(ID_DOCUMENT_ATTRIBUTE_AUDIO);
        w.u32(1u << 10);
        w.i32(static_cast<int32_t>(duration));
      }
      w.u32(ID_DOCUMENT_ATTRIBUTE_FILENAME);
      w.string(name);
    }
    w.string(caption);
    uint64_t random_id = 0;
    esp_fill_random(&random_id, sizeof(random_id));
    w.u64(random_id);
    ok = w.ok() && this->call_wait_(0, w.data(), w.size(), error, [this](TlReader result) {
      this->on_updates_(result);
      return true;
    });
  }
  heap_caps_free(body);
  return ok;
}

}

#endif
