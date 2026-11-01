#include "emulator.h"

#ifdef USE_EMULATOR

#include <algorithm>
#include <cstring>
#include <new>
#include <strings.h>

#include "emu_shim.h"
#include "esp_cache.h"
#include "esp_rom_crc.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_STORAGE_TRANSFER_BUFFER
#include "esphome/components/storage/transfer_buffer.h"
#endif

namespace esphome::emulator {

static const char *const TAG = "emulator";

static const char *const MSX_BIOS[] = {"MSX.ROM",     "MSX2.ROM",  "MSX2EXT.ROM", "MSX2P.ROM",
                                       "MSX2PEXT.ROM", "DISK.ROM",  "MSXDOS2.ROM", "FMPAC.ROM",
                                       "KANJI.ROM",   "PAINTER.ROM", "RS232.ROM",  "CMOS.ROM"};
static constexpr size_t MSX_BIOS_COUNT = sizeof(MSX_BIOS) / sizeof(MSX_BIOS[0]);

static const char *const SYSTEM_NAMES[] = {"NES", "Game Boy",     "Master System", "PC Engine",          "Lynx",
                                           "MSX", "Game & Watch", "Super Nintendo", "Mega Drive / Genesis"};

struct SystemExtension {
  const char *ext;
  EmuSystem system;
};

static const SystemExtension SYSTEM_EXTENSIONS[] = {
    {"nes", EmuSystem::NES}, {"gb", EmuSystem::GB},   {"gbc", EmuSystem::GB},   {"sms", EmuSystem::SMS},
    {"gg", EmuSystem::SMS},  {"sg", EmuSystem::SMS},  {"col", EmuSystem::SMS},  {"pce", EmuSystem::PCE},
    {"lnx", EmuSystem::LYNX}, {"rom", EmuSystem::MSX}, {"mx1", EmuSystem::MSX}, {"mx2", EmuSystem::MSX},
    {"dsk", EmuSystem::MSX}, {"gw", EmuSystem::GW},   {"sfc", EmuSystem::SNES}, {"smc", EmuSystem::SNES},
    {"md", EmuSystem::MD},   {"gen", EmuSystem::MD},  {"smd", EmuSystem::MD},
};

Emulator *Emulator::create() {
  void *mem = heap_caps_calloc(1, sizeof(Emulator), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (mem != nullptr)
    return new (mem) Emulator();
  auto *emu = new Emulator();
  emu->usable_ = false;
  return emu;
}

void Emulator::setup() {
  for (size_t i = 0; i < this->system_count_; i++) {
    const auto system = static_cast<uint32_t>(this->systems_[i].system);
    this->scale_pref_[system] = global_preferences->make_preference<uint16_t>(fnv1_hash("emulator.scale") ^ system);
    uint16_t kept = 0;
    if (this->scale_pref_[system].load(&kept))
      this->scale_kept_[system] = kept;
  }
#ifdef USE_EMULATOR_TOUCH
  if (this->touchscreen_ != nullptr)
    this->touchscreen_->register_listener(&this->touch_listener_);
#endif
#ifdef USE_EMULATOR_KEY_BINDINGS
  this->set_close_action(this->close_name_);
#endif
  this->disable_loop();
}

void Emulator::dump_config() {
  ESP_LOGCONFIG(TAG, "Emulator:");
  for (size_t i = 0; i < this->system_count_; i++) {
    const auto system = static_cast<size_t>(this->systems_[i].system);
    ESP_LOGCONFIG(TAG, "  %s: %s (scale at least %u/16, kept %u/16)", SYSTEM_NAMES[system], this->systems_[i].dir,
                  this->systems_[i].min_scale, this->scale_kept_[system]);
  }
}

void Emulator::add_system(EmuSystem system, const char *dir, uint16_t min_scale) {
  if (this->system_count_ < MAX_SYSTEMS)
    this->systems_[this->system_count_++] = {system, dir, min_scale};
}

void Emulator::add_key(uint16_t page, uint16_t usage, uint16_t buttons) {
  if (this->key_count_ < MAX_KEYS)
    this->keys_[this->key_count_++] = {page, usage, buttons};
}

void Emulator::add_menu_key(uint16_t page, uint16_t usage) {
  if (this->menu_key_count_ < MAX_MENU_KEYS)
    this->menu_keys_[this->menu_key_count_++] = {page, usage, 0};
}

const char *Emulator::system_name(size_t index) const {
  if (index >= this->system_count_)
    return "";
  return SYSTEM_NAMES[static_cast<size_t>(this->systems_[index].system)];
}

const char *Emulator::state_name() const {
  switch (this->state_) {
    case State::IDLE:
      return "";
    case State::PAUSED:
      return "paused";
    default:
      return "running";
  }
}

const char *Emulator::link_state_name() const {
  switch (static_cast<LinkState>(this->link_state_.load(std::memory_order_acquire))) {
    case LinkState::WAITING:
      return "waiting";
    case LinkState::CONNECTED:
      return "connected";
    case LinkState::LOST:
      return "lost";
    default:
      return "off";
  }
}

void Emulator::add_peer(const char *host) {
  if (this->peer_count_ < MAX_PEERS)
    this->peers_[this->peer_count_++] = host;
}

void Emulator::link_host() {
#ifdef USE_EMULATOR_LINK
  this->link_wanted_ = LinkMode::HOST;
  this->state_callbacks_.call();
#endif
}

void Emulator::link_join(const std::string &host) {
#ifdef USE_EMULATOR_LINK
  if (host.empty() || host.size() > LINK_HOST_MAX)
    return;
  memcpy(this->link_target_, host.c_str(), host.size() + 1);
  this->link_wanted_ = LinkMode::JOIN;
  this->state_callbacks_.call();
#endif
}

void Emulator::link_off() {
  this->link_wanted_ = LinkMode::OFF;
  this->state_callbacks_.call();
}

const Emulator::SystemEntry *Emulator::entry_(EmuSystem system) const {
  for (size_t i = 0; i < this->system_count_; i++) {
    if (this->systems_[i].system == system)
      return &this->systems_[i];
  }
  return nullptr;
}

bool Emulator::system_for_(const char *path, EmuSystem *system) const {
  for (size_t i = 0; i < this->system_count_; i++) {
    const size_t len = strlen(this->systems_[i].dir);
    if (len != 0 && strncmp(path, this->systems_[i].dir, len) == 0 && path[len] == '/') {
      *system = this->systems_[i].system;
      return true;
    }
  }
  const char *dot = strrchr(path, '.');
  if (dot == nullptr)
    return false;
  for (const auto &entry : SYSTEM_EXTENSIONS) {
    if (strcasecmp(dot + 1, entry.ext) != 0)
      continue;
    if (this->entry_(entry.system) != nullptr) {
      *system = entry.system;
      return true;
    }
  }
  return false;
}

uint32_t Emulator::internal_budget_(EmuSystem system) const {
  switch (system) {
    case EmuSystem::NES:
      return 24 * 1024;
    case EmuSystem::GB:
      return 24 * 1024;
    case EmuSystem::SMS:
      return 25 * 1024;
    case EmuSystem::SNES:
      return 96 * 1024;
    case EmuSystem::MD:
      return 14 * 1024;
    case EmuSystem::PCE:
      return 8 * 1024;
    case EmuSystem::LYNX:
      return 12 * 1024;
    default:
      return 0;
  }
}

bool Emulator::hot_only_(EmuSystem system) const {
  return system == EmuSystem::SNES || system == EmuSystem::MD || system == EmuSystem::PCE || system == EmuSystem::LYNX;
}

size_t Emulator::rom_alloc_(EmuSystem system, size_t size) const {
  switch (system) {
    case EmuSystem::SNES:
      return size + 0x10200;
    case EmuSystem::MD:
      return (size + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
    default:
      return size;
  }
}

uint8_t *Emulator::psram_(size_t size) const {
  return static_cast<uint8_t *>(heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

bool Emulator::start(const std::string &path) { return this->start_(path, false); }

bool Emulator::start_(const std::string &path, bool assign) {
  if (!this->usable_) {
    ESP_LOGE(TAG, "No PSRAM for the emulator");
    return false;
  }
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "A game runs already");
    return false;
  }
  if (path.size() > storage::STORAGE_VFS_PATH_MAX) {
    ESP_LOGE(TAG, "Path too long");
    return false;
  }
  if (!this->system_for_(path.c_str(), &this->system_)) {
    ESP_LOGE(TAG, "No configured system for %s", path.c_str());
    return false;
  }
  if (this->output_ == nullptr || this->canvas_ != nullptr)
    return false;
  this->closed_home_ = false;
  this->canvas_ = this->output_->open_rows(this);
  if (this->canvas_ == nullptr) {
    ESP_LOGW(TAG, "Refused: the screen is held by another app");
    return false;
  }
  memcpy(this->path_, path.c_str(), path.size() + 1);
  this->rom_ = nullptr;
  this->rom_size_ = 0;
  this->read_bytes_ = 0;
  this->storage_ = nullptr;
  this->load_step_ = 0;
  this->file_pending_ = true;
  this->job_ = Job::NONE;
  this->job_wanted_ = Job::NONE;
  this->sram_crc_ = 0;
  this->sram_stored_ = false;
  this->resume_pending_ = false;
  for (auto &entry : this->bios_)
    entry = {nullptr, nullptr, 0};
  this->task_stop_.store(false, std::memory_order_relaxed);
  this->task_pause_.store(true, std::memory_order_relaxed);
  this->task_parked_.store(false, std::memory_order_relaxed);
  this->task_done_.store(false, std::memory_order_relaxed);
  this->pads_[0].store(0, std::memory_order_relaxed);
  this->pads_[1].store(0, std::memory_order_relaxed);
  this->pause_wanted_.store(false, std::memory_order_relaxed);
  this->assign_active_ = assign;
  this->link_mode_ = this->link_wanted_;
  this->link_wanted_ = LinkMode::OFF;
  memcpy(this->link_peer_, this->link_target_, sizeof(this->link_peer_));
  const char *dot = strrchr(this->path_, '.');
  this->link_cable_ = this->system_ == EmuSystem::GB || this->system_ == EmuSystem::LYNX ||
                      (this->system_ == EmuSystem::SMS && dot != nullptr && strcasecmp(dot + 1, "gg") == 0);
  this->link_state_.store(static_cast<uint8_t>(LinkState::OFF), std::memory_order_release);
  this->state_ = State::LOADING;
  ESP_LOGI(TAG, "Starting %s (%s)", this->path_, SYSTEM_NAMES[static_cast<size_t>(this->system_)]);
  this->state_callbacks_.call();
  this->enable_loop();
  return true;
}

void Emulator::loop() {
  if (this->output_ != nullptr)
    this->output_->loop();
  if (this->scale_save_.exchange(false, std::memory_order_acq_rel)) {
    const auto system = static_cast<size_t>(this->system_);
    this->scale_pref_[system].save(&this->scale_kept_[system]);
  }
  if (this->pause_wanted_.exchange(false, std::memory_order_acq_rel))
    this->pause();
  if (this->link_changed_.exchange(false, std::memory_order_acq_rel))
    this->state_callbacks_.call();
  if (this->players_open_)
    this->players_service_();
  if (this->job_wanted_ != Job::NONE && this->job_ == Job::NONE)
    this->job_begin_();
  switch (this->state_) {
    case State::LOADING:
      if (this->task_stop_.load(std::memory_order_acquire) && !this->file_pending_ && !this->stream_open_ &&
          this->job_ == Job::NONE) {
        this->job_wanted_ = Job::NONE;
        this->end_();
        break;
      }
      if (this->file_pending_ && !this->stream_open_) {
        if (this->task_stop_.load(std::memory_order_acquire)) {
          this->end_();
          break;
        }
        this->next_file_();
      }
      break;
    case State::LINKING:
      if (this->task_parked_.load(std::memory_order_acquire))
        this->go_panel_();
      break;
    case State::PAUSING:
      if (this->task_parked_.load(std::memory_order_acquire)) {
        this->state_ = State::PAUSED;
        this->set_key_tap_(false);
        if (this->sram_changed_())
          this->job_wanted_ = Job::SAV_STORE;
        ESP_LOGI(TAG, "Paused");
        this->state_callbacks_.call();
        this->paused_trigger_.trigger();
      }
      break;
    case State::STOPPING:
      if (this->task_.is_created() && !this->task_done_.load(std::memory_order_acquire))
        break;
      if (this->task_.is_created() && !this->task_.deallocate())
        break;
      if (this->busy())
        break;
      if (!this->sram_stored_ && this->sram_changed_()) {
        this->sram_stored_ = true;
        this->job_wanted_ = Job::SAV_STORE;
        break;
      }
      this->end_();
      break;
    default:
      break;
  }
  const bool waiting = this->state_ == State::LOADING || this->state_ == State::STOPPING ||
                       this->job_wanted_ != Job::NONE || (this->state_ == State::IDLE && this->canvas_ != nullptr);
  if (this->state_ == State::IDLE && this->canvas_ != nullptr && this->canvas_->idle())
    this->canvas_ = nullptr;
  if (!waiting)
    this->disable_loop();
}

void Emulator::next_file_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (worker == nullptr || storage::global_storage_registry == nullptr) {
    this->fail_("no storage");
    return;
  }
  while (true) {
    const char *path = this->path_;
    const char *bios = nullptr;
    if (this->load_step_ > 0) {
      const size_t index = this->load_step_ - 1;
      if (this->system_ != EmuSystem::MSX || this->bios_dir_ == nullptr || index >= MSX_BIOS_COUNT) {
        this->file_pending_ = false;
        this->loaded_();
        return;
      }
      bios = MSX_BIOS[index];
      if (snprintf(this->file_path_, sizeof(this->file_path_), "%s/%s", this->bios_dir_, bios) >=
          static_cast<int>(sizeof(this->file_path_))) {
        this->load_step_++;
        continue;
      }
      path = this->file_path_;
    }
    const char *rel = nullptr;
    storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path, &rel);
    if (ps == nullptr) {
      if (bios == nullptr) {
        this->fail_("no storage for the path");
        return;
      }
      this->load_step_++;
      continue;
    }
    if (worker->has_active_task_io(ps))
      return;
    storage::FileStat st{};
    if (ps->stat(rel, &st) != storage::StorageError::STORAGE_ERROR_OK || st.is_dir || st.size == 0) {
      if (bios == nullptr) {
        this->fail_("not readable");
        return;
      }
      this->load_step_++;
      continue;
    }
    this->file_size_ = static_cast<size_t>(st.size);
    const size_t alloc = bios == nullptr ? this->rom_alloc_(this->system_, this->file_size_) : this->file_size_;
    this->file_data_ = this->psram_(alloc);
    if (this->file_data_ == nullptr) {
      this->fail_("no PSRAM for the file");
      return;
    }
    if (alloc > this->file_size_)
      memset(this->file_data_ + this->file_size_, 0, alloc - this->file_size_);
    if (bios == nullptr) {
      this->rom_ = this->file_data_;
      this->rom_size_ = this->file_size_;
    } else {
      for (auto &entry : this->bios_) {
        if (entry.data == nullptr) {
          entry = {bios, this->file_data_, this->file_size_};
          break;
        }
      }
    }
    this->read_bytes_ = 0;
    this->storage_ = ps;
    const storage::StorageError err = worker->begin_read(ps, rel, &this->stream_, [this](storage::StorageError e) {
      if (e != storage::StorageError::STORAGE_ERROR_OK) {
        this->read_done_(e);
        return;
      }
      this->read_next_();
    });
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      this->fail_("storage busy");
      return;
    }
    this->stream_open_ = true;
    this->file_pending_ = false;
    return;
  }
}

void Emulator::read_next_() {
  if (this->task_stop_.load(std::memory_order_acquire)) {
    this->read_done_(storage::StorageError::STORAGE_ERROR_OK);
    return;
  }
  const size_t want = std::min<size_t>(READ_CHUNK, this->file_size_ - this->read_bytes_);
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->stream_, this->file_data_ + this->read_bytes_, want, &this->chunk_bytes_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->chunk_bytes_ == 0) {
          this->read_done_(e != storage::StorageError::STORAGE_ERROR_OK ? e
                                                                        : storage::StorageError::STORAGE_ERROR_READ_ERROR);
          return;
        }
        this->read_bytes_ += this->chunk_bytes_;
        if (this->read_bytes_ >= this->file_size_) {
          this->read_done_(storage::StorageError::STORAGE_ERROR_OK);
          return;
        }
        this->read_next_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->read_done_(err);
}

void Emulator::read_done_(storage::StorageError err) {
  this->read_err_ = err;
  if (storage::global_storage_worker->end_read(this->stream_, [this](storage::StorageError) { this->read_closed_(); }) !=
      storage::StorageError::STORAGE_ERROR_OK)
    this->read_closed_();
}

void Emulator::read_closed_() {
  this->stream_open_ = false;
  this->enable_loop();
  if (this->task_stop_.load(std::memory_order_acquire)) {
    this->end_();
    return;
  }
  if (this->read_err_ != storage::StorageError::STORAGE_ERROR_OK || this->read_bytes_ < this->file_size_) {
    this->fail_("read failed");
    return;
  }
  this->load_step_++;
  this->file_pending_ = true;
}

void Emulator::loaded_() {
  emu_bss_clear();
  emu_set_allocator(&Emulator::alloc_cb_, &Emulator::realloc_cb_, &Emulator::free_cb_, this);
  emu_set_host(&Emulator::host_frame_cb_, this);
  this->internal_used_ = 0;
  this->internal_budget_now_ = this->internal_budget_(this->system_);
  this->hot_only_now_ = this->hot_only_(this->system_);
  this->core_ = emu_make_core(this->system_, this->core_store_);
  if (this->core_ == nullptr) {
    this->fail_("system not built in");
    return;
  }
  for (const auto &entry : this->bios_) {
    if (entry.data != nullptr)
      this->core_->add_file(entry.name, entry.data, entry.size);
  }
  if (!this->core_->load(this->rom_, this->rom_size_, this->path_, this->audio_rate_, this->audio_channels_)) {
    this->fail_("the ROM did not load");
    return;
  }
  if (!this->setup_output_()) {
    this->fail_("no output");
    return;
  }
  this->silence_frames_ =
      static_cast<size_t>(static_cast<uint64_t>(this->core_->sample_rate()) * this->core_->frame_us() / 1000000u) + 1;
  this->silence_ = this->psram_(this->silence_frames_ * this->core_->channels() * sizeof(int16_t));
  if (this->silence_ == nullptr) {
    this->fail_("no PSRAM for the sound");
    return;
  }
  memset(this->silence_, 0, this->silence_frames_ * this->core_->channels() * sizeof(int16_t));
#ifdef USE_EMULATOR_SPEAKER
  if (this->speaker_ != nullptr)
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, this->core_->channels(), this->core_->sample_rate()));
#endif
  ESP_LOGI(TAG, "Loaded: %zu bytes, %u x %u, %u Hz %u ch, internal %zu bytes", this->rom_size_,
           this->core_->video().width, this->core_->video().height, static_cast<unsigned>(this->core_->sample_rate()),
           this->core_->channels(), this->internal_used_);
  size_t sram_size = 0;
  if (this->core_->sram(&sram_size) != nullptr && sram_size != 0) {
    this->job_wanted_ = Job::SAV_LOAD;
    return;
  }
  this->ready_();
}

void Emulator::ready_() {
  this->started_trigger_.trigger();
#ifdef USE_EMULATOR_LINK
  if (this->link_mode_ != LinkMode::OFF) {
    this->link_begin_();
    return;
  }
#endif
  this->go_panel_();
}

void Emulator::go_panel_() {
  this->state_ = State::WAIT_PANEL;
  this->task_pause_.store(false, std::memory_order_release);
  if (this->task_.is_created()) {
    xTaskNotifyGive(this->task_.get_handle());
  } else if (!this->create_task_()) {
    return;
  }
  this->start_pacing_();
  this->state_ = State::RUNNING;
  this->set_key_tap_(true);
  ESP_LOGI(TAG, "Running");
  this->state_callbacks_.call();
}

void Emulator::fail_(const char *why) {
  ESP_LOGE(TAG, "%s: %s", this->path_, why);
  if (this->stream_open_) {
    this->task_stop_.store(true, std::memory_order_release);
    return;
  }
  this->end_();
}

void Emulator::stop() {
  switch (this->state_) {
    case State::IDLE:
    case State::STOPPING:
      return;
    case State::LOADING:
      this->task_stop_.store(true, std::memory_order_release);
      this->enable_loop();
      return;
    default:
      break;
  }
  this->task_stop_.store(true, std::memory_order_release);
  this->state_ = State::STOPPING;
  this->set_key_tap_(false);
  this->link_wake_();
  if (this->task_.is_created())
    xTaskNotifyGive(this->task_.get_handle());
  this->enable_loop();
}

void Emulator::pause() {
  if (this->state_ != State::RUNNING)
    return;
  this->task_pause_.store(true, std::memory_order_release);
  this->state_ = State::PAUSING;
  this->link_wake_();
  if (this->task_.is_created())
    xTaskNotifyGive(this->task_.get_handle());
}

void Emulator::resume() {
  if (this->state_ != State::PAUSED)
    return;
  if (this->busy()) {
    this->resume_pending_ = true;
    return;
  }
  this->resume_pending_ = false;
  this->cleared_ = 0;
  this->go_panel_();
}

void Emulator::set_key_tap_(bool game) {
  this->tap_game_.store(game, std::memory_order_release);
#ifdef USE_LVGL_HID
  if (this->lvgl_ != nullptr)
    this->lvgl_->set_key_tap(game || this->players_open_ ? this : nullptr);
#endif
}

bool Emulator::create_task_() {
  if (this->task_.create(&Emulator::task_entry_, "emulator", TASK_STACK, this, TASK_PRIORITY, false, 0))
    return true;
  ESP_LOGE(TAG, "No task for the game");
  this->task_stop_.store(true, std::memory_order_release);
  this->state_ = State::STOPPING;
  this->enable_loop();
  return false;
}

void Emulator::start_pacing_() {
  if (this->core_->frame_us() == 0 || this->pace_timer_ != nullptr)
    return;
  esp_timer_create_args_t args{};
  args.callback = [](void *arg) {
    auto *emu = static_cast<Emulator *>(arg);
    if (emu->task_.is_created())
      xTaskNotifyGive(emu->task_.get_handle());
  };
  args.arg = this;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "emulator_pace";
  if (esp_timer_create(&args, &this->pace_timer_) != ESP_OK)
    this->pace_timer_ = nullptr;
}

void Emulator::pace_reset_() { this->pace_anchored_ = false; }

void Emulator::pace_() {
  const int64_t frame_us = this->core_->frame_us();
  if (this->pace_timer_ == nullptr || frame_us == 0) {
    this->pass_check_();
    return;
  }
  const int64_t now = esp_timer_get_time();
  this->pace_next_us_ = this->pace_anchored_ ? this->pace_next_us_ + frame_us : now;
  this->pace_anchored_ = true;
  if (this->pace_next_us_ < now - frame_us * PACE_CATCH_UP_FRAMES)
    this->pace_next_us_ = now;
  ulTaskNotifyTake(pdTRUE, 0);
  while (true) {
    this->pass_check_();
    const int64_t wait = this->pace_next_us_ - esp_timer_get_time();
    if (wait <= 0 || this->task_stop_.load(std::memory_order_acquire) ||
        this->task_pause_.load(std::memory_order_acquire))
      return;
    esp_timer_stop(this->pace_timer_);
    if (esp_timer_start_once(this->pace_timer_, static_cast<uint64_t>(wait)) != ESP_OK)
      return;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }
}

void Emulator::end_() {
  this->link_close_();
  if (this->pace_timer_ != nullptr) {
    esp_timer_stop(this->pace_timer_);
    esp_timer_delete(this->pace_timer_);
    this->pace_timer_ = nullptr;
  }
  this->job_release_();
  this->job_ = Job::NONE;
  this->job_wanted_ = Job::NONE;
  this->resume_pending_ = false;
  this->release_output_();
  heap_caps_free(this->silence_);
  this->silence_ = nullptr;
  this->core_ = nullptr;
  this->release_all_();
  emu_set_allocator(nullptr, nullptr, nullptr, nullptr);
  emu_set_host(nullptr, nullptr);
  for (auto &entry : this->bios_) {
    heap_caps_free(entry.data);
    entry = {nullptr, nullptr, 0};
  }
  heap_caps_free(this->rom_);
  this->rom_ = nullptr;
  this->rom_size_ = 0;
  this->storage_ = nullptr;
  this->set_key_tap_(false);
#ifdef USE_EMULATOR_KEY_BINDINGS
  this->key_held_ = false;
#endif
  this->fingers_ = false;
  this->cancel_timeout("close");
  if (this->canvas_ != nullptr && this->canvas_->held_by(this))
    this->canvas_->close();
  const bool was_loading = this->state_ == State::LOADING;
  this->state_ = State::IDLE;
  ESP_LOGI(TAG, "Game %s", was_loading ? "not started" : "ended");
  this->state_callbacks_.call();
  this->ended_trigger_.trigger();
  this->enable_loop();
}

void Emulator::add_hid(hid::HIDInput *input) {
  if (this->hid_count_ < MAX_HIDS)
    this->hids_[this->hid_count_++] = input;
}

bool Emulator::players_wanted() {
  size_t devices = 0;
  std::vector<hid::InputDevice> list;
  for (size_t i = 0; i < this->hid_count_; i++) {
    list.clear();
    this->hids_[i]->list_devices(list);
    if (!list.empty()) {
      for (const auto &device : list) {
        const bool keys = device.kinds == 0 || (device.kinds & (hid::HID_DEVICE_KEYBOARD | hid::HID_DEVICE_GAMEPAD)) != 0;
        if (device.connected && keys)
          devices++;
      }
    } else if (this->hids_[i]->get_device_kinds() != 0) {
      devices++;
    }
  }
  return devices >= 2;
}

void Emulator::players_open(const std::string &path) {
  if (this->state_ != State::IDLE && this->state_ != State::PAUSED)
    return;
  if (this->state_ == State::IDLE) {
    if (path.size() > storage::STORAGE_VFS_PATH_MAX)
      return;
    memcpy(this->players_path_, path.c_str(), path.size() + 1);
  }
  this->player_count_ = 0;
  this->tap_head_.store(this->tap_tail_.load(std::memory_order_acquire), std::memory_order_release);
  this->players_open_ = true;
  this->set_key_tap_(false);
  this->players_changed_trigger_.trigger();
}

void Emulator::players_close_() {
  this->players_open_ = false;
  this->set_key_tap_(false);
}

void Emulator::players_start() {
  if (!this->players_open_)
    return;
  bool any = false;
  for (uint8_t i = 0; i < this->player_count_; i++)
    any = any || this->players_[i].player != 0;
  if (!any)
    return;
  this->players_close_();
  this->players_closed_trigger_.trigger();
  if (this->state_ == State::PAUSED) {
    this->assign_active_ = true;
    this->pads_[0].store(0, std::memory_order_relaxed);
    this->pads_[1].store(0, std::memory_order_relaxed);
    return;
  }
  this->start_(this->players_path_, true);
}

void Emulator::players_cancel() {
  if (!this->players_open_)
    return;
  this->players_close_();
  this->players_closed_trigger_.trigger();
}

void Emulator::players_set(uint8_t index, uint8_t player) {
  if (index >= this->player_count_ || player > 2)
    return;
  this->players_[index].player = player;
  this->players_changed_trigger_.trigger();
}

void Emulator::players_service_() {
  bool changed = false;
  while (this->tap_head_.load(std::memory_order_relaxed) != this->tap_tail_.load(std::memory_order_acquire)) {
    const uint8_t head = this->tap_head_.load(std::memory_order_relaxed);
    const TapEvent event = this->tap_ring_[head % TAP_EVENTS];
    this->tap_head_.store(static_cast<uint8_t>(head + 1), std::memory_order_release);
    for (size_t i = 0; i < this->menu_key_count_; i++) {
      if (this->menu_keys_[i].page == event.page && this->menu_keys_[i].usage == event.usage) {
        this->players_cancel();
        return;
      }
    }
    PlayerDevice *device = nullptr;
    for (uint8_t i = 0; i < this->player_count_; i++) {
      if (this->players_[i].source == event.source)
        device = &this->players_[i];
    }
    if (device == nullptr) {
      if (this->player_count_ >= MAX_PLAYER_DEVICES)
        continue;
      device = &this->players_[this->player_count_++];
      device->source = event.source;
      memcpy(device->label, event.label, sizeof(device->label));
      device->player = 0;
      changed = true;
      continue;
    }
    const uint16_t buttons = this->buttons_of_(event.page, event.usage);
    if ((buttons & EMU_LEFT) != 0) {
      device->player = device->player == 2 ? 0 : 1;
      changed = true;
    } else if ((buttons & EMU_RIGHT) != 0) {
      device->player = device->player == 1 ? 0 : 2;
      changed = true;
    } else if ((buttons & (EMU_A | EMU_START)) != 0 && device->player != 0) {
      if (changed)
        this->players_changed_trigger_.trigger();
      this->players_start();
      return;
    } else if ((buttons & EMU_B) != 0) {
      this->players_cancel();
      return;
    }
  }
  if (changed)
    this->players_changed_trigger_.trigger();
}

bool Emulator::save_state(uint8_t slot) {
  if (this->state_ != State::PAUSED || this->busy() || slot < 1 || slot > SLOTS)
    return false;
  this->job_slot_ = slot;
  this->job_wanted_ = Job::STATE_SAVE;
  this->enable_loop();
  return true;
}

bool Emulator::load_state(uint8_t slot) {
  if (this->state_ != State::PAUSED || this->busy() || slot < 1 || slot > SLOTS)
    return false;
  this->job_slot_ = slot;
  this->job_wanted_ = Job::STATE_LOAD;
  this->enable_loop();
  return true;
}

uint32_t Emulator::slot_time(uint8_t slot) {
  if (this->state_ == State::IDLE || slot < 1 || slot > SLOTS || storage::global_storage_registry == nullptr)
    return 0;
  char suffix[8];
  snprintf(suffix, sizeof(suffix), ".st%u", static_cast<unsigned>(slot));
  char path[storage::STORAGE_VFS_PATH_MAX + 1];
  if (!this->side_path_(path, sizeof(path), suffix))
    return 0;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path, &rel);
  if (ps == nullptr || storage::global_storage_worker == nullptr ||
      storage::global_storage_worker->has_active_task_io(ps))
    return 0;
  storage::FileStat st{};
  if (ps->stat(rel, &st) != storage::StorageError::STORAGE_ERROR_OK || st.is_dir)
    return 0;
  return st.mtime != 0 ? st.mtime : 1;
}

bool Emulator::side_path_(char *out, size_t len, const char *suffix) const {
  const size_t path_len = strlen(this->path_);
  const char *slash = strrchr(this->path_, '/');
  const char *dot = strrchr(this->path_, '.');
  const size_t base =
      dot != nullptr && (slash == nullptr || dot > slash) ? static_cast<size_t>(dot - this->path_) : path_len;
  if (base + strlen(suffix) + 1 > len)
    return false;
  memcpy(out, this->path_, base);
  strcpy(out + base, suffix);
  return true;
}

bool Emulator::sram_changed_() {
  if (this->core_ == nullptr)
    return false;
  size_t size = 0;
  uint8_t *sram = this->core_->sram(&size);
  if (sram == nullptr || size == 0)
    return false;
  return esp_rom_crc32_le(0, sram, static_cast<uint32_t>(size)) != this->sram_crc_;
}

void Emulator::job_begin_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  const Job job = this->job_wanted_;
  char suffix[8];
  if (job == Job::SAV_LOAD || job == Job::SAV_STORE) {
    strcpy(suffix, ".sav");
  } else {
    snprintf(suffix, sizeof(suffix), ".st%u", static_cast<unsigned>(this->job_slot_));
  }
  const char *rel = nullptr;
  storage::PathStorage *ps = nullptr;
  if (worker != nullptr && storage::global_storage_registry != nullptr && this->core_ != nullptr &&
      this->side_path_(this->job_path_, sizeof(this->job_path_), suffix))
    ps = storage::global_storage_registry->resolve_path(this->job_path_, &rel);
  if (ps == nullptr) {
    this->job_wanted_ = Job::NONE;
    this->job_ = job;
    this->job_finished_(false);
    return;
  }
  if (worker->has_active_task_io(ps))
    return;
  this->job_wanted_ = Job::NONE;
  this->job_ = job;
  this->job_done_ = 0;
  this->job_len_ = 0;
  this->job_err_ = storage::StorageError::STORAGE_ERROR_OK;
  size_t sram_size = 0;
  uint8_t *sram = this->core_->sram(&sram_size);
  if (job == Job::SAV_LOAD || job == Job::STATE_LOAD) {
    storage::FileStat st{};
    if (ps->stat(rel, &st) != storage::StorageError::STORAGE_ERROR_OK || st.is_dir || st.size == 0) {
      this->job_finished_(job == Job::SAV_LOAD);
      return;
    }
    if (job == Job::SAV_LOAD) {
      if (sram == nullptr || st.size > sram_size) {
        this->job_finished_(false);
        return;
      }
      this->job_data_ = sram;
    } else {
#ifdef USE_STORAGE_TRANSFER_BUFFER
      if (storage::global_transfer_buffer != nullptr && st.size <= storage::global_transfer_buffer->capacity())
        this->state_buf_ = storage::global_transfer_buffer->try_acquire(static_cast<size_t>(st.size));
#endif
      if (this->state_buf_ == nullptr) {
        this->job_finished_(false);
        return;
      }
      this->job_data_ = this->state_buf_;
    }
    this->job_len_ = static_cast<size_t>(st.size);
    this->job_write_ = false;
  } else {
    if (job == Job::SAV_STORE) {
      if (sram == nullptr) {
        this->job_finished_(false);
        return;
      }
      this->job_data_ = sram;
      this->job_len_ = sram_size;
      this->job_crc_ = esp_rom_crc32_le(0, sram, static_cast<uint32_t>(sram_size));
    } else {
#ifdef USE_STORAGE_TRANSFER_BUFFER
      if (storage::global_transfer_buffer != nullptr) {
        const size_t cap = std::min<size_t>(STATE_MAX, storage::global_transfer_buffer->capacity());
        this->state_buf_ = cap != 0 ? storage::global_transfer_buffer->try_acquire(cap) : nullptr;
        if (this->state_buf_ != nullptr)
          this->job_len_ = this->core_->save_state(this->state_buf_, cap);
      }
#endif
      if (this->state_buf_ == nullptr || this->job_len_ == 0) {
        this->job_finished_(false);
        return;
      }
      this->job_data_ = this->state_buf_;
    }
    this->job_write_ = true;
  }
  auto opened = [this](storage::StorageError e) {
    if (e != storage::StorageError::STORAGE_ERROR_OK) {
      this->job_close_(e);
      return;
    }
    if (this->job_write_) {
      this->job_write_next_();
    } else {
      this->job_read_next_();
    }
  };
  const storage::StorageError err = this->job_write_ ? worker->begin_write(ps, rel, &this->job_stream_, opened)
                                                     : worker->begin_read(ps, rel, &this->job_stream_, opened);
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->job_finished_(false);
    return;
  }
  this->job_open_ = true;
}

void Emulator::job_read_next_() {
  const size_t want = std::min<size_t>(READ_CHUNK, this->job_len_ - this->job_done_);
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->job_stream_, this->job_data_ + this->job_done_, want, &this->job_chunk_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->job_chunk_ == 0) {
          this->job_close_(e != storage::StorageError::STORAGE_ERROR_OK ? e
                                                                       : storage::StorageError::STORAGE_ERROR_READ_ERROR);
          return;
        }
        this->job_done_ += this->job_chunk_;
        if (this->job_done_ >= this->job_len_) {
          this->job_close_(storage::StorageError::STORAGE_ERROR_OK);
          return;
        }
        this->job_read_next_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->job_close_(err);
}

void Emulator::job_write_next_() {
  this->job_chunk_ = std::min<size_t>(READ_CHUNK, this->job_len_ - this->job_done_);
  const storage::StorageError err = storage::global_storage_worker->write_chunk(
      this->job_stream_, this->job_data_ + this->job_done_, this->job_chunk_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          this->job_close_(e);
          return;
        }
        this->job_done_ += this->job_chunk_;
        if (this->job_done_ >= this->job_len_) {
          this->job_close_(storage::StorageError::STORAGE_ERROR_OK);
          return;
        }
        this->job_write_next_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->job_close_(err);
}

void Emulator::job_close_(storage::StorageError err) {
  this->job_err_ = err;
  auto closed = [this](storage::StorageError e) {
    this->job_open_ = false;
    if (this->job_err_ == storage::StorageError::STORAGE_ERROR_OK)
      this->job_err_ = e;
    this->job_finished_(this->job_err_ == storage::StorageError::STORAGE_ERROR_OK && this->job_done_ >= this->job_len_);
  };
  storage::StorageWorker *worker = storage::global_storage_worker;
  const storage::StorageError e =
      this->job_write_ ? worker->end_write(this->job_stream_, closed) : worker->end_read(this->job_stream_, closed);
  if (e != storage::StorageError::STORAGE_ERROR_OK) {
    this->job_open_ = false;
    this->job_finished_(false);
  }
}

void Emulator::job_finished_(bool ok) {
  const Job job = this->job_;
  this->job_ = Job::NONE;
  this->enable_loop();
  switch (job) {
    case Job::SAV_LOAD: {
      size_t size = 0;
      uint8_t *sram = this->core_ != nullptr ? this->core_->sram(&size) : nullptr;
      this->sram_crc_ = sram != nullptr ? esp_rom_crc32_le(0, sram, static_cast<uint32_t>(size)) : 0;
      if (!ok)
        ESP_LOGW(TAG, "%s not read", this->job_path_);
      if (this->state_ == State::LOADING && !this->task_stop_.load(std::memory_order_acquire))
        this->ready_();
      break;
    }
    case Job::SAV_STORE:
      if (ok) {
        this->sram_crc_ = this->job_crc_;
        ESP_LOGI(TAG, "Saved %s", this->job_path_);
      } else {
        ESP_LOGE(TAG, "%s not written", this->job_path_);
      }
      break;
    case Job::STATE_SAVE:
      if (ok) {
        ESP_LOGI(TAG, "State saved: %s", this->job_path_);
      } else {
        ESP_LOGE(TAG, "State not saved: %s", this->job_path_);
      }
      break;
    case Job::STATE_LOAD:
      if (ok && this->core_ != nullptr && this->core_->load_state(this->job_data_, this->job_len_)) {
        ESP_LOGI(TAG, "State loaded: %s", this->job_path_);
      } else {
        ESP_LOGE(TAG, "State not loaded: %s", this->job_path_);
      }
      break;
    default:
      break;
  }
  this->job_release_();
  this->state_callbacks_.call();
  if (this->resume_pending_ && this->state_ == State::PAUSED && !this->busy())
    this->resume();
}

void Emulator::job_release_() {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (this->state_buf_ != nullptr && storage::global_transfer_buffer != nullptr)
    storage::global_transfer_buffer->release();
#endif
  this->state_buf_ = nullptr;
  this->job_data_ = nullptr;
}

void *Emulator::alloc_cb_(void *ctx, size_t size, bool zero, uint8_t tier) {
  return static_cast<Emulator *>(ctx)->alloc_(size, zero, tier);
}

void *Emulator::realloc_cb_(void *ctx, void *ptr, size_t size) {
  return static_cast<Emulator *>(ctx)->realloc_(ptr, size);
}

void Emulator::free_cb_(void *ctx, void *ptr) { static_cast<Emulator *>(ctx)->free_(ptr); }

Emulator::Piece *Emulator::piece_(void *ptr) {
  for (auto &piece : this->pieces_) {
    if (piece.ptr == ptr)
      return &piece;
  }
  return nullptr;
}

void *Emulator::alloc_(size_t size, bool zero, uint8_t tier) {
  if (size == 0)
    size = 1;
  Piece *slot = this->piece_(nullptr);
  if (slot == nullptr)
    return nullptr;
  void *ptr = nullptr;
  bool internal = false;
  bool fast = false;
  const bool hot = tier != 0;
  if (tier == 2) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPM | MALLOC_CAP_8BIT);
    fast = ptr != nullptr;
  }
  if (ptr == nullptr && (hot || (!this->hot_only_now_ && size <= INTERNAL_PIECE_MAX)) &&
      this->internal_used_ + size <= this->internal_budget_now_) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_SIMD);
    internal = ptr != nullptr;
  }
  if (ptr == nullptr)
    ptr = this->psram_(size);
  if (ptr == nullptr)
    return nullptr;
  if (zero)
    memset(ptr, 0, size);
  *slot = {ptr, size, internal || fast, fast};
  if (internal)
    this->internal_used_ += size;
  return ptr;
}

void *Emulator::realloc_(void *ptr, size_t size) {
  Piece *piece = this->piece_(ptr);
  if (piece == nullptr)
    return nullptr;
  if (size <= piece->size)
    return ptr;
  void *grown = this->alloc_(size, false, piece->fast ? 2 : (piece->internal ? 1 : 0));
  if (grown == nullptr)
    return nullptr;
  piece = this->piece_(ptr);
  memcpy(grown, ptr, piece->size);
  this->free_(ptr);
  return grown;
}

void Emulator::free_(void *ptr) {
  Piece *piece = this->piece_(ptr);
  if (piece == nullptr)
    return;
  if (piece->internal && !piece->fast)
    this->internal_used_ -= piece->size;
  heap_caps_free(piece->ptr);
  *piece = {nullptr, 0, false, false};
}

void Emulator::release_all_() {
  for (auto &piece : this->pieces_) {
    if (piece.ptr != nullptr)
      this->free_(piece.ptr);
  }
  this->internal_used_ = 0;
}

bool Emulator::setup_output_() {
  const EmuVideo video = this->core_->video();
  if (video.width == 0 || video.height == 0 || this->canvas_ == nullptr)
    return false;
  const SystemEntry *entry = this->entry_(this->system_);
  this->min_scale_ = entry != nullptr ? entry->min_scale : SCALE_ONE;
  const uint16_t kept = this->scale_kept_[static_cast<size_t>(this->system_)];
  this->scale_cap_ = kept >= this->min_scale_ ? kept : SCALE_MAX;
  this->rotation_ = this->canvas_->rotation();
  this->lay_key_ = 0;
  this->lay_layout_ = 0;
  this->cleared_ = 0;
  this->pass_count_ = 0;
  this->pass_sum_us_ = 0;
  this->pass_skipped_ = 0;
  this->ppa_busy_ = false;
  this->ppa_finished_.store(false, std::memory_order_relaxed);
  display::Display *display = this->canvas_->display();
  void *fbs[2]{};
  display::ColorBitness bitness{};
  if (display == nullptr || display->get_scanout_buffers(fbs, 2, &bitness) != 2)
    return true;
  const size_t line = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
  const size_t fb_size = static_cast<size_t>(display->get_native_width()) * display->get_native_height() *
                         (bitness == display::COLOR_BITNESS_888 ? 3 : 2);
  if (fb_size % line != 0 || reinterpret_cast<uintptr_t>(fbs[0]) % line != 0 ||
      reinterpret_cast<uintptr_t>(fbs[1]) % line != 0)
    return true;
  ppa_client_config_t cfg{};
  cfg.oper_type = PPA_OPERATION_SRM;
  cfg.max_pending_trans_num = 1;
  if (ppa_register_client(&cfg, &this->ppa_) != ESP_OK) {
    this->ppa_ = nullptr;
    return true;
  }
  ppa_event_callbacks_t cbs{};
  cbs.on_trans_done = &Emulator::ppa_done_cb_;
  if (ppa_client_register_event_callbacks(this->ppa_, &cbs) != ESP_OK) {
    ppa_unregister_client(this->ppa_);
    this->ppa_ = nullptr;
    return true;
  }
  if (video.palette == nullptr)
    return true;
  this->frame_size_ = std::max<size_t>(static_cast<size_t>(video.width) * video.height * 2, FRAME_MIN_BYTES);
  this->frame_ = this->psram_(this->frame_size_);
  if (this->frame_ == nullptr) {
    ppa_unregister_client(this->ppa_);
    this->ppa_ = nullptr;
    return false;
  }
  return true;
}

bool Emulator::ppa_done_cb_(ppa_client_handle_t client, ppa_event_data_t *event, void *arg) {
  auto *emu = static_cast<Emulator *>(arg);
  emu->ppa_end_us_.store(static_cast<uint32_t>(esp_timer_get_time()), std::memory_order_relaxed);
  emu->ppa_finished_.store(true, std::memory_order_release);
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(emu->task_.get_handle(), &woken);
  return woken == pdTRUE;
}

void Emulator::pass_check_() {
  if (!this->ppa_busy_ || !this->ppa_finished_.exchange(false, std::memory_order_acq_rel))
    return;
  this->ppa_busy_ = false;
  this->ppa_source_held_ = false;
  this->canvas_->rows_end(true);
  this->pass_measure_(this->ppa_end_us_.load(std::memory_order_relaxed) - this->ppa_start_us_, false);
}

void Emulator::pass_wait_() {
  while (this->ppa_busy_) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    this->pass_check_();
  }
}

void Emulator::pass_measure_(uint32_t pass_us, bool skipped) {
  if (skipped) {
    this->pass_skipped_++;
  } else {
    this->pass_sum_us_ += pass_us;
  }
  if (++this->pass_count_ < PASS_WINDOW)
    return;
  const uint32_t passes = this->pass_count_ - this->pass_skipped_;
  const uint32_t mean = passes != 0 ? this->pass_sum_us_ / passes : 0;
  const uint32_t budget = this->core_->frame_us() / 2;
  const bool over = this->pass_skipped_ != 0 || (budget != 0 && mean > budget);
  const uint32_t skipped_now = this->pass_skipped_;
  this->pass_count_ = 0;
  this->pass_sum_us_ = 0;
  this->pass_skipped_ = 0;
  if (!over)
    return;
  uint16_t next = this->scale_;
  if (this->scale_ > SCALE_ONE && this->scale_ % SCALE_ONE == 0) {
    next = this->scale_ - SCALE_ONE;
  } else if (this->scale_ > this->min_scale_) {
    next = this->scale_ - 1;
  }
  if (next < this->min_scale_ || next == this->scale_)
    return;
  ESP_LOGI(TAG, "PPA pass %u us (%u skipped) over half a frame (%u us): scale %u/16 -> %u/16",
           static_cast<unsigned>(mean), static_cast<unsigned>(skipped_now), static_cast<unsigned>(budget),
           this->scale_, next);
  this->scale_cap_ = next;
  this->scale_kept_[static_cast<size_t>(this->system_)] = next;
  this->scale_save_.store(true, std::memory_order_release);
  this->enable_loop_soon_any_context();
  this->lay_key_ = 0;
}

uint16_t Emulator::fit_scale_(uint16_t width, uint16_t height) const {
  const bool turned =
      this->rotation_ == display::DISPLAY_ROTATION_90_DEGREES || this->rotation_ == display::DISPLAY_ROTATION_270_DEGREES;
  const uint32_t lw = turned ? this->out_h_ : this->out_w_;
  const uint32_t lh = turned ? this->out_w_ : this->out_h_;
  const uint32_t fit = std::min<uint32_t>(lw * SCALE_ONE / width, lh * SCALE_ONE / height);
  uint32_t scale = fit >= SCALE_ONE ? fit / SCALE_ONE * SCALE_ONE : fit;
  scale = std::min<uint32_t>(scale, SCALE_MAX);
  if (this->scale_cap_ < scale) {
    scale = this->scale_cap_;
    if (scale > SCALE_ONE && scale % SCALE_ONE != 0 && fit >= SCALE_ONE)
      scale = scale / SCALE_ONE * SCALE_ONE;
  }
  return static_cast<uint16_t>(std::max<uint32_t>(scale, 1));
}

void Emulator::layout_(const lvgl::LvglComponent::VideoCanvasTarget &target, uint16_t width, uint16_t height) {
  const int32_t W = target.width;
  const int32_t H = target.height;
  this->out_w_ = W;
  this->out_h_ = H;
  this->scale_ = this->fit_scale_(width, height);
  const bool turned =
      this->rotation_ == display::DISPLAY_ROTATION_90_DEGREES || this->rotation_ == display::DISPLAY_ROTATION_270_DEGREES;
  const int32_t lw = turned ? H : W;
  const int32_t lh = turned ? W : H;
  const int32_t ow = std::min<int32_t>(static_cast<int32_t>(width) * this->scale_ / SCALE_ONE, lw);
  const int32_t oh = std::min<int32_t>(static_cast<int32_t>(height) * this->scale_ / SCALE_ONE, lh);
  const int32_t lx = (lw - ow) / 2;
  const int32_t ly = (lh - oh) / 2;
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      this->out_x_ = W - ly - oh;
      this->out_y_ = lx;
      this->out_w_ = oh;
      this->out_h_ = ow;
      this->ppa_rotation_ = PPA_SRM_ROTATION_ANGLE_270;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      this->out_x_ = W - lx - ow;
      this->out_y_ = H - ly - oh;
      this->out_w_ = ow;
      this->out_h_ = oh;
      this->ppa_rotation_ = PPA_SRM_ROTATION_ANGLE_180;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      this->out_x_ = ly;
      this->out_y_ = H - lx - ow;
      this->out_w_ = oh;
      this->out_h_ = ow;
      this->ppa_rotation_ = PPA_SRM_ROTATION_ANGLE_90;
      break;
    default:
      this->out_x_ = lx;
      this->out_y_ = ly;
      this->out_w_ = ow;
      this->out_h_ = oh;
      this->ppa_rotation_ = PPA_SRM_ROTATION_ANGLE_0;
      break;
  }
  this->lay_key_ = (static_cast<uint32_t>(width) << 16) | height;
  this->lay_layout_ = target.layout;
  this->cleared_ = 0;
}

void Emulator::release_output_() {
  if (this->ppa_ != nullptr)
    ppa_unregister_client(this->ppa_);
  this->ppa_ = nullptr;
  this->ppa_busy_ = false;
  heap_caps_free(this->frame_);
  this->frame_ = nullptr;
  this->frame_size_ = 0;
}

void Emulator::task_entry_(void *arg) {
  auto *emu = static_cast<Emulator *>(arg);
  emu->task_loop_();
  emu->pass_wait_();
  emu->link_note_(LINK_BYE);
  emu->task_done_.store(true, std::memory_order_release);
  emu->enable_loop_soon_any_context();
  while (true)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

bool Emulator::park_() {
  if (this->task_pause_.load(std::memory_order_acquire)) {
    this->pass_wait_();
    this->pace_reset_();
    this->link_note_(LINK_PAUSE);
    this->task_parked_.store(true, std::memory_order_release);
    this->enable_loop_soon_any_context();
    while (this->task_pause_.load(std::memory_order_acquire) && !this->task_stop_.load(std::memory_order_acquire))
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    this->task_parked_.store(false, std::memory_order_release);
    if (!this->task_stop_.load(std::memory_order_acquire))
      this->link_note_(LINK_RESUME);
  }
  return !this->task_stop_.load(std::memory_order_acquire);
}

bool Emulator::frame_pads_(uint16_t *pads) {
#ifdef USE_EMULATOR_LINK
  while (this->link_fd_ >= 0 && !this->link_cable_) {
    const int got = this->net_pads_(pads);
    if (got > 0)
      return true;
    if (got < 0 && !this->park_())
      return false;
  }
#endif
  pads[0] = this->pads_[0].load(std::memory_order_relaxed);
  pads[1] = this->pads_[1].load(std::memory_order_relaxed);
  return true;
}

void Emulator::task_loop_() {
  this->pace_reset_();
#ifdef USE_EMULATOR_LINK
  if (this->link_mode_ != LinkMode::OFF) {
    this->link_connect_();
    this->task_pause_.store(true, std::memory_order_release);
  }
#endif
  while (this->park_()) {
    uint16_t pads[2];
    if (!this->frame_pads_(pads))
      return;
    if (this->ppa_source_held_)
      this->pass_wait_();
    this->core_->run_frame(pads);
    if (this->core_->owns_loop())
      return;
    this->draw_frame_(this->core_->video());
    this->play_audio_();
    this->pace_();
  }
}

bool Emulator::host_frame_cb_(void *ctx, uint16_t *pad1, uint16_t *pad2) {
  return static_cast<Emulator *>(ctx)->host_frame_(pad1, pad2);
}

bool Emulator::host_frame_(uint16_t *pad1, uint16_t *pad2) {
  this->draw_frame_(this->core_->video());
  this->play_audio_();
  this->pace_();
  if (!this->park_())
    return false;
  uint16_t pads[2];
  if (!this->frame_pads_(pads))
    return false;
  if (this->ppa_source_held_)
    this->pass_wait_();
  *pad1 = pads[0];
  *pad2 = pads[1];
  return true;
}

void Emulator::draw_frame_(const EmuVideo &video) {
  this->pass_check_();
  if (this->ppa_busy_) {
    this->pass_measure_(0, true);
    return;
  }
  lvgl::LvglComponent::VideoCanvasTarget target{};
  const uint32_t wait_ms = std::max<uint32_t>(1, this->core_->frame_us() / 1000);
  if (!this->canvas_->rows_begin(&target, wait_ms))
    return;
  if (target.width == 0 || target.height == 0) {
    this->canvas_->rows_end(false);
    return;
  }
  const uint32_t key = (static_cast<uint32_t>(video.width) << 16) | video.height;
  if (key != this->lay_key_ || target.layout != this->lay_layout_)
    this->layout_(target, video.width, video.height);
  const size_t len = static_cast<size_t>(target.width) * target.height * target.bpp;
  if ((this->cleared_ & (1u << target.index)) == 0) {
    memset(target.rows, 0, len);
    esp_cache_msync(target.rows, len,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    this->cleared_ |= static_cast<uint8_t>(1u << target.index);
  }
  const bool straight = video.palette == nullptr && video.stride % 2 == 0 && video.stride / 2 >= video.width;
  if (this->ppa_ != nullptr &&
      (straight || (this->frame_ != nullptr && static_cast<size_t>(video.width) * video.height * 2 <= this->frame_size_)) &&
      this->draw_ppa_(video, target, straight))
    return;
  this->draw_cpu_(video, target);
  this->canvas_->rows_end(true);
}

bool Emulator::draw_ppa_(const EmuVideo &video, const lvgl::LvglComponent::VideoCanvasTarget &target, bool straight) {
  display::Display *display = this->canvas_->display();
  void *fbs[2]{};
  display::ColorBitness bitness{};
  if (display == nullptr || display->get_scanout_buffers(fbs, 2, &bitness) != 2)
    return false;
  auto *base = static_cast<uint8_t *>(fbs[target.index]);
  const uint32_t panel_w = static_cast<uint32_t>(display->get_native_width());
  const uint32_t panel_h = static_cast<uint32_t>(display->get_native_height());
  const size_t stride = static_cast<size_t>(panel_w) * target.bpp;
  if (target.rows < base || panel_w != target.width)
    return false;
  const auto band_y = static_cast<uint32_t>(static_cast<size_t>(target.rows - base) / stride);
  const void *in = video.pixels;
  uint32_t in_w = video.stride / 2;
  if (!straight) {
    auto *out = reinterpret_cast<uint16_t *>(this->frame_);
    const uint16_t *pal = video.palette;
    const uint16_t quads = (video.width & 1) == 0 ? video.width / 4 : 0;
    for (uint16_t y = 0; y < video.height; y++) {
      const uint8_t *row = video.pixels + static_cast<size_t>(y) * video.stride;
      auto *out32 = reinterpret_cast<uint32_t *>(out);
      for (uint16_t q = 0; q < quads; q++) {
        const uint8_t *r = row + q * 4;
        out32[q * 2] = pal[r[0]] | (static_cast<uint32_t>(pal[r[1]]) << 16);
        out32[q * 2 + 1] = pal[r[2]] | (static_cast<uint32_t>(pal[r[3]]) << 16);
      }
      for (uint16_t x = quads * 4; x < video.width; x++)
        out[x] = pal[row[x]];
      out += video.width;
    }
    in = this->frame_;
    in_w = video.width;
  }
  ppa_srm_oper_config_t cfg{};
  cfg.in.buffer = in;
  cfg.in.pic_w = in_w;
  cfg.in.pic_h = video.height;
  cfg.in.block_w = video.width;
  cfg.in.block_h = video.height;
  cfg.in.block_offset_x = 0;
  cfg.in.block_offset_y = 0;
  cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  cfg.out.buffer = base;
  cfg.out.buffer_size = stride * panel_h;
  cfg.out.pic_w = panel_w;
  cfg.out.pic_h = panel_h;
  cfg.out.block_offset_x = this->out_x_;
  cfg.out.block_offset_y = band_y + this->out_y_;
  cfg.out.srm_cm = target.bpp == 3 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
  cfg.rotation_angle = this->ppa_rotation_;
  cfg.scale_x = static_cast<float>(this->scale_) / SCALE_ONE;
  cfg.scale_y = static_cast<float>(this->scale_) / SCALE_ONE;
  cfg.mode = PPA_TRANS_MODE_NON_BLOCKING;
  cfg.user_data = this;
  this->ppa_finished_.store(false, std::memory_order_relaxed);
  this->ppa_start_us_ = static_cast<uint32_t>(esp_timer_get_time());
  if (ppa_do_scale_rotate_mirror(this->ppa_, &cfg) != ESP_OK)
    return false;
  this->ppa_busy_ = true;
  this->ppa_source_held_ = straight && !video.double_buffered;
  return true;
}

void Emulator::draw_cpu_(const EmuVideo &video, const lvgl::LvglComponent::VideoCanvasTarget &target) {
  const int32_t W = target.width;
  const int32_t H = target.height;
  const size_t bpp = target.bpp;
  const bool turned =
      this->rotation_ == display::DISPLAY_ROTATION_90_DEGREES || this->rotation_ == display::DISPLAY_ROTATION_270_DEGREES;
  const int32_t ow = turned ? this->out_h_ : this->out_w_;
  const int32_t oh = turned ? this->out_w_ : this->out_h_;
  for (int32_t ly = 0; ly < oh; ly++) {
    const uint8_t *row = video.pixels + static_cast<size_t>(ly * SCALE_ONE / this->scale_) * video.stride;
    for (int32_t lx = 0; lx < ow; lx++) {
      const int32_t sx = lx * SCALE_ONE / this->scale_;
      const uint16_t color = video.palette != nullptr ? video.palette[row[sx]]
                                                      : reinterpret_cast<const uint16_t *>(row)[sx];
      int32_t px;
      int32_t py;
      switch (this->rotation_) {
        case display::DISPLAY_ROTATION_90_DEGREES:
          px = this->out_x_ + this->out_w_ - 1 - ly;
          py = this->out_y_ + lx;
          break;
        case display::DISPLAY_ROTATION_180_DEGREES:
          px = this->out_x_ + this->out_w_ - 1 - lx;
          py = this->out_y_ + this->out_h_ - 1 - ly;
          break;
        case display::DISPLAY_ROTATION_270_DEGREES:
          px = this->out_x_ + ly;
          py = this->out_y_ + this->out_h_ - 1 - lx;
          break;
        default:
          px = this->out_x_ + lx;
          py = this->out_y_ + ly;
          break;
      }
      if (px < 0 || py < 0 || px >= W || py >= H)
        continue;
      uint8_t *dst = target.rows + (static_cast<size_t>(py) * W + px) * bpp;
      if (bpp == 2) {
        *reinterpret_cast<uint16_t *>(dst) = color;
      } else {
        dst[0] = static_cast<uint8_t>((color & 0x1F) << 3);
        dst[1] = static_cast<uint8_t>(((color >> 5) & 0x3F) << 2);
        dst[2] = static_cast<uint8_t>(((color >> 11) & 0x1F) << 3);
      }
    }
  }
  const size_t stride = static_cast<size_t>(W) * bpp;
  esp_cache_msync(target.rows + static_cast<size_t>(this->out_y_) * stride, static_cast<size_t>(this->out_h_) * stride,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void Emulator::play_audio_() {
#ifdef USE_EMULATOR_SPEAKER
  if (this->speaker_ == nullptr)
    return;
  size_t frames = 0;
  const int16_t *pcm = this->core_->audio(&frames);
  if (pcm == nullptr || frames == 0) {
    pcm = reinterpret_cast<const int16_t *>(this->silence_);
    frames = this->silence_frames_;
  }
  this->speaker_->play(reinterpret_cast<const uint8_t *>(pcm), frames * this->core_->channels() * sizeof(int16_t), 0);
#endif
}

uint16_t Emulator::buttons_of_(uint16_t page, uint16_t usage) const {
  uint16_t buttons = 0;
  for (size_t i = 0; i < this->key_count_; i++) {
    if (this->keys_[i].page == page && this->keys_[i].usage == usage)
      buttons |= this->keys_[i].buttons;
  }
  return buttons;
}

bool Emulator::game_key_(uint16_t page, uint16_t usage, bool pressed, uint32_t source) {
  for (size_t i = 0; i < this->menu_key_count_; i++) {
    if (this->menu_keys_[i].page == page && this->menu_keys_[i].usage == usage) {
      if (pressed) {
        this->pause_wanted_.store(true, std::memory_order_release);
        this->enable_loop_soon_any_context();
      }
      return true;
    }
  }
  if (page == 0x07 && this->core_ != nullptr && this->core_->keyboard()) {
    this->core_->key(usage, pressed);
    return true;
  }
  const uint16_t buttons = this->buttons_of_(page, usage);
  if (buttons == 0)
    return true;
  uint8_t pad = 0;
  if (this->assign_active_) {
    uint8_t player = 0;
    for (uint8_t i = 0; i < this->player_count_; i++) {
      if (this->players_[i].source == source)
        player = this->players_[i].player;
    }
    if (player == 0)
      return true;
    pad = player - 1;
  }
  if (pressed) {
    this->pads_[pad].fetch_or(buttons, std::memory_order_relaxed);
  } else {
    this->pads_[pad].fetch_and(static_cast<uint16_t>(~buttons), std::memory_order_relaxed);
  }
  return true;
}

bool Emulator::close_key_(uint16_t page, uint16_t usage, bool pressed, uint32_t source) {
#ifdef USE_EMULATOR_KEY_BINDINGS
  if (pressed && !this->key_held_ && this->bindings_ != nullptr && this->close_action_ >= 0) {
    const int action = this->bindings_->resolve(page, usage, 0);
    if (action == this->close_action_) {
      this->key_held_ = true;
      this->key_page_ = page;
      this->key_usage_ = usage;
      this->key_at_ = millis();
      this->close_check_();
      return true;
    }
  }
  if (!pressed && this->key_held_ && page == this->key_page_ && usage == this->key_usage_) {
    this->key_held_ = false;
    this->close_check_();
    this->game_key_(page, usage, true, source);
    this->game_key_(page, usage, false, source);
    return true;
  }
#endif
  return false;
}

void Emulator::close_check_() {
  const uint32_t now = millis();
  uint32_t due = UINT32_MAX;
  auto left = [now](uint32_t since, uint32_t hold) { return now - since >= hold ? 0 : hold - (now - since); };
#ifdef USE_EMULATOR_KEY_BINDINGS
  if (this->key_held_)
    due = std::min(due, left(this->key_at_, this->close_hold_ms_));
#endif
  if (this->fingers_)
    due = std::min(due, left(this->fingers_at_, this->touch_hold_ms_));
  if (due == UINT32_MAX) {
    this->cancel_timeout("close");
    return;
  }
  if (due != 0) {
    this->set_timeout("close", due, [this]() { this->close_check_(); });
    return;
  }
#ifdef USE_EMULATOR_KEY_BINDINGS
  this->key_held_ = false;
#endif
  this->fingers_ = false;
  if (this->state_ != State::RUNNING && this->state_ != State::PAUSED)
    return;
  ESP_LOGI(TAG, "Closed by holding");
  this->closed_home_ = true;
  this->stop();
}

#ifdef USE_EMULATOR_TOUCH
void EmulatorTouchListener::update(const touchscreen::TouchPoints_t &points) {
  this->parent_->touch(static_cast<uint8_t>(points.size()));
}

void EmulatorTouchListener::release() { this->parent_->touch(0); }

void Emulator::touch(uint8_t fingers) {
  if (this->state_ != State::RUNNING)
    return;
  if (fingers >= 2) {
    if (!this->fingers_) {
      this->fingers_ = true;
      this->fingers_at_ = millis();
      this->close_check_();
    }
  } else if (this->fingers_) {
    this->fingers_ = false;
    this->close_check_();
  }
}
#endif

#ifdef USE_LVGL_HID
bool Emulator::key_tap(uint32_t source, const char *label, uint16_t page, uint16_t usage, bool pressed) {
  if (this->tap_game_.load(std::memory_order_acquire)) {
    if (this->close_key_(page, usage, pressed, source))
      return true;
    return this->game_key_(page, usage, pressed, source);
  }
  if (!pressed)
    return true;
  const uint8_t tail = this->tap_tail_.load(std::memory_order_relaxed);
  if (static_cast<uint8_t>(tail - this->tap_head_.load(std::memory_order_acquire)) >= TAP_EVENTS)
    return true;
  TapEvent &event = this->tap_ring_[tail % TAP_EVENTS];
  event.source = source;
  event.page = page;
  event.usage = usage;
  strncpy(event.label, label != nullptr ? label : "", sizeof(event.label) - 1);
  event.label[sizeof(event.label) - 1] = '\0';
  this->tap_tail_.store(static_cast<uint8_t>(tail + 1), std::memory_order_release);
  this->enable_loop_soon_any_context();
  return true;
}
#endif

}

#endif
