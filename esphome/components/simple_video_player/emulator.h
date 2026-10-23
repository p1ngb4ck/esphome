#pragma once

#include "simple_video_player.h"

#ifdef SVP_EMULATOR

#include "driver/ppa.h"
#include "esphome/core/static_task.h"
#include "esphome/components/hid/hid_input.h"

namespace esphome::simple_video_player {

enum EmuButton : uint16_t {
  EMU_UP = 1 << 0,
  EMU_DOWN = 1 << 1,
  EMU_LEFT = 1 << 2,
  EMU_RIGHT = 1 << 3,
  EMU_A = 1 << 4,
  EMU_B = 1 << 5,
  EMU_X = 1 << 6,
  EMU_Y = 1 << 7,
  EMU_L = 1 << 8,
  EMU_R = 1 << 9,
  EMU_START = 1 << 10,
  EMU_SELECT = 1 << 11,
};

enum class EmuSystem : uint8_t { NES, GB, SMS, PCE, LYNX, MSX, GW, SNES, MD, COUNT };

struct EmuVideo {
  const uint8_t *pixels{nullptr};
  const uint16_t *palette{nullptr};
  uint32_t stride{0};
  uint16_t width{0};
  uint16_t height{0};
};

class EmuCore {
 public:
  virtual ~EmuCore() = default;
  virtual bool load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) = 0;
  virtual void run_frame(const uint16_t *pads) = 0;
  virtual EmuVideo video() = 0;
  virtual const int16_t *audio(size_t *frames) = 0;
  virtual uint32_t sample_rate() const = 0;
  virtual uint8_t channels() const = 0;
  virtual uint32_t frame_us() const = 0;
  virtual bool owns_loop() const { return false; }
  virtual bool keyboard() const { return false; }
  virtual void key(uint16_t usage, bool pressed) {}
  virtual void add_file(const char *name, const uint8_t *data, size_t size) {}
  virtual uint8_t *sram(size_t *size) { return nullptr; }
  virtual size_t save_state(uint8_t *buf, size_t cap) { return 0; }
  virtual bool load_state(const uint8_t *buf, size_t len) { return false; }
};

static constexpr size_t EMU_CORE_STORE = 768;
EmuCore *emu_make_core(EmuSystem system, void *store);

class GameEmulator
#ifdef USE_LVGL_HID
    : public lvgl::LvglComponent::PausedInput,
      public lvgl::LvglComponent::KeyTap
#endif
{
 public:
  enum class State : uint8_t { IDLE, LOADING, LINKING, WAIT_PANEL, RUNNING, PAUSING, PAUSED, STOPPING };
  enum class LinkMode : uint8_t { OFF, HOST, JOIN };
  enum class LinkState : uint8_t { OFF, WAITING, CONNECTED, LOST };
  enum class Job : uint8_t { NONE, SAV_LOAD, SAV_STORE, STATE_SAVE, STATE_LOAD };
  static constexpr uint8_t SLOTS = 4;

  static GameEmulator *create(SimpleVideoPlayer *parent);

  void add_system(EmuSystem system, const char *dir);
  void add_key(uint16_t page, uint16_t usage, uint16_t buttons);
  void add_menu_key(uint16_t page, uint16_t usage);
  void set_bios_dir(const char *dir) { this->bios_dir_ = dir; }
  void add_hid(hid::HIDInput *input);
  Trigger<> *get_players_changed_trigger() { return &this->players_changed_trigger_; }
  Trigger<> *get_players_closed_trigger() { return &this->players_closed_trigger_; }

  bool players_wanted();
  void players_open(const std::string &path);
  void players_start();
  void players_cancel();
  void players_set(uint8_t index, uint8_t player);
  bool players_shown() const { return this->players_open_; }
  uint8_t players_count() const { return this->player_count_; }
  const char *players_label(uint8_t index) const {
    return index < this->player_count_ ? this->players_[index].label : "";
  }
  uint8_t players_player(uint8_t index) const { return index < this->player_count_ ? this->players_[index].player : 0; }
  void set_link_port(uint16_t port) { this->link_port_ = port; }
  void add_peer(const char *host);
  size_t peer_count() const { return this->peer_count_; }
  const char *peer(size_t index) const { return index < this->peer_count_ ? this->peers_[index] : ""; }
  void link_host();
  void link_join(const std::string &host);
  void link_off();
  LinkMode link_wanted() const { return this->link_wanted_; }
  const char *link_state_name() const;
  Trigger<> *get_paused_trigger() { return &this->paused_trigger_; }
  Trigger<> *get_ended_trigger() { return &this->ended_trigger_; }

  bool start(const std::string &path);
  void stop();
  void pause();
  void resume();
  bool save_state(uint8_t slot);
  bool load_state(uint8_t slot);
  uint32_t slot_time(uint8_t slot);
  bool busy() const { return this->job_ != Job::NONE || this->job_wanted_ != Job::NONE; }
  State state() const { return this->state_; }
  bool active() const { return this->state_ != State::IDLE; }
  const char *state_name() const;
  size_t system_count() const { return this->system_count_; }
  const char *system_dir(size_t index) const { return index < this->system_count_ ? this->systems_[index].dir : ""; }
  const char *system_name(size_t index) const;
  bool usable() const { return this->usable_; }

  void service();
  bool wants_panel() const { return this->wants_panel_; }
  void panel_ready(bool ready);
  void app_ended();

#ifdef USE_LVGL_HID
  bool paused_key(uint16_t page, uint16_t usage, uint8_t mods, bool pressed, int action, uint32_t codepoint,
                  bool level3, uint32_t source) override;
  bool key_tap(uint32_t source, const char *label, uint16_t page, uint16_t usage, bool pressed) override;
  bool paused_text(uint32_t codepoint) override { return true; }
  bool paused_pointer(int16_t dx, int16_t dy, int16_t wheel, uint8_t buttons) override { return true; }
#endif

 protected:
  explicit GameEmulator(SimpleVideoPlayer *parent) : parent_(parent) {}

  static constexpr size_t MAX_SYSTEMS = static_cast<size_t>(EmuSystem::COUNT);
  static constexpr size_t MAX_KEYS = 48;
  static constexpr size_t MAX_MENU_KEYS = 4;
  static constexpr size_t MAX_PIECES = 96;
  static constexpr size_t READ_CHUNK = 64 * 1024;
  static constexpr uint32_t TASK_STACK = 8192;
  static constexpr uint32_t INTERNAL_PIECE_MAX = 16 * 1024;
  static constexpr size_t FRAME_MIN_BYTES = 256 * 240 * 2;

  struct SystemEntry {
    EmuSystem system;
    const char *dir;
  };
  struct KeyEntry {
    uint16_t page;
    uint16_t usage;
    uint16_t buttons;
  };
  struct Piece {
    void *ptr;
    size_t size;
    bool internal;
  };
  struct PlayerDevice {
    uint32_t source;
    char label[24];
    uint8_t player;
  };
  struct TapEvent {
    uint32_t source;
    uint16_t page;
    uint16_t usage;
    char label[24];
  };
  static constexpr size_t MAX_PLAYER_DEVICES = 4;
  static constexpr size_t TAP_EVENTS = 8;
  static constexpr size_t MAX_HIDS = 8;
  struct BiosFile {
    const char *name;
    uint8_t *data;
    size_t size;
  };
  static constexpr size_t MAX_BIOS = 12;
  struct LinkMsg {
    uint8_t type;
    uint8_t a;
    uint16_t b;
    uint32_t c;
  };
  static constexpr size_t MAX_PEERS = 8;
  static constexpr size_t LINK_HOST_MAX = 64;
  static constexpr size_t LINK_BUF = 16 * sizeof(LinkMsg);
  static constexpr uint8_t LINK_PAD = 0x10;
  static constexpr uint8_t LINK_PAUSE = 0x11;
  static constexpr uint8_t LINK_RESUME = 0x12;
  static constexpr uint8_t LINK_BYE = 0x13;
  static constexpr uint8_t LINK_HELLO = 0x20;

  bool start_(const std::string &path, bool assign);
  bool system_for_(const char *path, EmuSystem *system) const;
  uint32_t internal_budget_(EmuSystem system) const;
  size_t rom_alloc_(EmuSystem system, size_t size) const;
  void next_file_();
  void read_next_();
  void read_done_(storage::StorageError err);
  void read_closed_();
  void loaded_();
  void fail_(const char *why);
  void end_();
  void release_all_();
  void set_wants_panel_(bool wants);
  void players_service_();
  void players_close_();
  uint16_t buttons_of_(uint16_t page, uint16_t usage) const;
  void go_panel_();
  void ready_();
  bool create_task_();
  void start_pacing_();
  bool frame_pads_(uint16_t *pads);
  void link_begin_();
  void link_connect_();
  int link_listen_();
  int link_dial_();
  int link_wait_(int fd, bool write, uint32_t timeout_ms);
  bool link_send_(uint8_t type, uint8_t a, uint16_t b, uint32_t c);
  int link_take_(LinkMsg *msg, uint32_t wait_ms);
  int net_pads_(uint16_t *pads);
  void link_note_(uint8_t type);
  void link_lost_(const char *why);
  void link_wake_();
  void link_close_();
  static bool link_send_cb_(void *ctx, uint8_t type, uint8_t a, uint16_t b);
  static bool link_recv_cb_(void *ctx, uint8_t *type, uint8_t *a, uint16_t *b, uint32_t wait_ms);
  bool side_path_(char *out, size_t len, const char *suffix) const;
  bool sram_changed_();
  void job_begin_();
  void job_read_next_();
  void job_write_next_();
  void job_close_(storage::StorageError err);
  void job_finished_(bool ok);
  void job_release_();

  static void *alloc_cb_(void *ctx, size_t size, bool zero);
  static void *realloc_cb_(void *ctx, void *ptr, size_t size);
  static void free_cb_(void *ctx, void *ptr);
  void *alloc_(size_t size, bool zero);
  void *realloc_(void *ptr, size_t size);
  void free_(void *ptr);
  Piece *piece_(void *ptr);

  bool setup_output_();
  void layout_(uint16_t width, uint16_t height);
  void release_output_();
  static void task_entry_(void *arg);
  void task_loop_();
  bool park_();
  static bool host_frame_cb_(void *ctx, uint16_t *pad1, uint16_t *pad2);
  bool host_frame_(uint16_t *pad1, uint16_t *pad2);
  void draw_frame_(const EmuVideo &video);
  bool draw_cpu_(const EmuVideo &video, uint8_t *fb);
  void play_audio_();

  SimpleVideoPlayer *parent_;
  bool usable_{true};
  SystemEntry systems_[MAX_SYSTEMS]{};
  size_t system_count_{0};
  KeyEntry keys_[MAX_KEYS]{};
  size_t key_count_{0};
  KeyEntry menu_keys_[MAX_MENU_KEYS]{};
  size_t menu_key_count_{0};
  Trigger<> paused_trigger_;
  Trigger<> ended_trigger_;
  Trigger<> players_changed_trigger_;
  Trigger<> players_closed_trigger_;
  hid::HIDInput *hids_[MAX_HIDS]{};
  size_t hid_count_{0};
  PlayerDevice players_[MAX_PLAYER_DEVICES]{};
  uint8_t player_count_{0};
  bool players_open_{false};
  bool assign_active_{false};
  char players_path_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  TapEvent tap_ring_[TAP_EVENTS]{};
  std::atomic<uint8_t> tap_head_{0};
  std::atomic<uint8_t> tap_tail_{0};

  State state_{State::IDLE};
  EmuSystem system_{EmuSystem::NES};
  bool wants_panel_{false};
  bool panel_ok_{false};
  char path_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  storage::PathStorage *storage_{nullptr};
  storage::StreamHandle stream_{};
  bool stream_open_{false};
  size_t read_bytes_{0};
  size_t chunk_bytes_{0};
  storage::StorageError read_err_{storage::StorageError::STORAGE_ERROR_OK};

  uint8_t *rom_{nullptr};
  size_t rom_size_{0};
  const char *bios_dir_{nullptr};
  BiosFile bios_[MAX_BIOS]{};
  size_t load_step_{0};
  bool file_pending_{false};
  char file_path_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  uint8_t *file_data_{nullptr};
  size_t file_size_{0};
  alignas(8) uint8_t core_store_[EMU_CORE_STORE]{};
  EmuCore *core_{nullptr};
  Piece pieces_[MAX_PIECES]{};
  size_t internal_used_{0};
  size_t internal_budget_now_{0};
  bool on_main_loop_{false};

  uint8_t *frame_{nullptr};
  size_t frame_size_{0};
  ppa_client_handle_t ppa_{nullptr};
  uint32_t factor_{1};
  int32_t out_x_{0};
  int32_t out_y_{0};
  int32_t out_w_{0};
  int32_t out_h_{0};
  ppa_srm_rotation_angle_t ppa_rotation_{PPA_SRM_ROTATION_ANGLE_0};
  display::DisplayRotation rotation_{display::DISPLAY_ROTATION_0_DEGREES};
  uint8_t black_{0};
  uint16_t lay_w_{0};
  uint16_t lay_h_{0};
  uint8_t *silence_{nullptr};
  size_t silence_frames_{0};

  static constexpr size_t STATE_MAX = 768 * 1024;
  Job job_{Job::NONE};
  Job job_wanted_{Job::NONE};
  uint8_t job_slot_{1};
  bool job_write_{false};
  bool job_open_{false};
  storage::StreamHandle job_stream_{};
  uint8_t *job_data_{nullptr};
  size_t job_len_{0};
  size_t job_done_{0};
  size_t job_chunk_{0};
  uint32_t job_crc_{0};
  storage::StorageError job_err_{storage::StorageError::STORAGE_ERROR_OK};
  char job_path_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  uint8_t *state_buf_{nullptr};
  uint32_t sram_crc_{0};
  bool sram_stored_{false};
  bool resume_pending_{false};

  StaticTask task_;
  std::atomic<bool> task_stop_{false};
  std::atomic<bool> task_pause_{false};
  std::atomic<bool> task_parked_{false};
  std::atomic<bool> task_done_{false};
  std::atomic<uint16_t> pads_[2]{};
  esp_timer_handle_t pace_timer_{nullptr};

  const char *peers_[MAX_PEERS]{};
  size_t peer_count_{0};
  uint16_t link_port_{6666};
  LinkMode link_wanted_{LinkMode::OFF};
  LinkMode link_mode_{LinkMode::OFF};
  char link_target_[LINK_HOST_MAX + 1]{};
  char link_peer_[LINK_HOST_MAX + 1]{};
  bool link_cable_{false};
  int link_fd_{-1};
  int link_efd_{-1};
  std::atomic<uint8_t> link_state_{0};
  std::atomic<bool> link_changed_{false};
  bool peer_paused_{false};
  uint32_t net_frame_{0};
  bool net_sent_{false};
  uint16_t net_local_{0};
  uint16_t net_next_{0};
  uint8_t link_buf_[LINK_BUF]{};
  size_t link_buf_len_{0};
};

template<typename... Ts> class GameStartAction : public Action<Ts...> {
 public:
  explicit GameStartAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override {
    GameEmulator *emu = this->player_->get_emulator();
    if (emu != nullptr)
      emu->start(this->path_.value(x...));
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class GameStateAction : public Action<Ts...> {
 public:
  explicit GameStateAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(uint8_t, slot)
  void set_save(bool save) { this->save_ = save; }
  void play(const Ts &...x) override {
    GameEmulator *emu = this->player_->get_emulator();
    if (emu == nullptr)
      return;
    if (this->save_) {
      emu->save_state(this->slot_.value(x...));
    } else {
      emu->load_state(this->slot_.value(x...));
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  bool save_{true};
};

template<typename... Ts> class GamePlayersAction : public Action<Ts...> {
 public:
  explicit GamePlayersAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override {
    GameEmulator *emu = this->player_->get_emulator();
    if (emu != nullptr)
      emu->players_open(this->path_.value(x...));
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class GamePlayerSetAction : public Action<Ts...> {
 public:
  explicit GamePlayerSetAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(uint8_t, index)
  TEMPLATABLE_VALUE(uint8_t, seat)
  void play(const Ts &...x) override {
    GameEmulator *emu = this->player_->get_emulator();
    if (emu != nullptr)
      emu->players_set(this->index_.value(x...), this->seat_.value(x...));
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class GameLinkAction : public Action<Ts...> {
 public:
  explicit GameLinkAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(std::string, host)
  void set_mode(uint8_t mode) { this->mode_ = mode; }
  void play(const Ts &...x) override {
    GameEmulator *emu = this->player_->get_emulator();
    if (emu == nullptr)
      return;
    switch (this->mode_) {
      case 0:
        emu->link_off();
        break;
      case 1:
        emu->link_host();
        break;
      default:
        emu->link_join(this->host_.value(x...));
        break;
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  uint8_t mode_{0};
};

template<typename... Ts> class GameCommandAction : public Action<Ts...> {
 public:
  explicit GameCommandAction(SimpleVideoPlayer *player) : player_(player) {}
  void set_command(uint8_t command) { this->command_ = command; }
  void play(const Ts &...x) override {
    GameEmulator *emu = this->player_->get_emulator();
    if (emu == nullptr)
      return;
    switch (this->command_) {
      case 0:
        emu->stop();
        break;
      case 1:
        emu->pause();
        break;
      case 2:
        emu->resume();
        break;
      case 3:
        emu->players_start();
        break;
      default:
        emu->players_cancel();
        break;
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  uint8_t command_{0};
};

}

#endif
