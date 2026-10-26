#pragma once

#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef SVP_USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "lvgl.h"
#endif
// Keys, typed characters and the mouse go to a udisp sender that asks for them ('K', 'X', 'P').
#if defined(SVP_STREAM) && defined(SVP_USE_LVGL) && defined(USE_LVGL_HID)
#define SVP_STREAM_INPUT
#endif

#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"

// ESP32-P4 hardware JPEG decoder only -- other variants cannot decode fast enough for video.

#ifdef SVP_DSI_OUTPUT
#include "esphome/components/display/display.h"
#endif

#if defined(SVP_STREAM_TOUCH) || defined(SVP_CONTROLS)
#include "esphome/components/touchscreen/touchscreen.h"
#endif

#include "esphome/components/storage/storage_worker.h"
#ifdef SVP_PICTURES
#include "esphome/components/runtime_image/runtime_image.h"
#endif

#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#ifdef USE_AUDIO
#include "esphome/components/audio/audio_decoder.h"
#endif
#ifdef SVP_AUDIO_ROUTER
#include "esphome/components/resampler/speaker/resampler_speaker.h"
#endif

#ifdef SVP_CHANNEL_LIST
#include "esphome/components/http_request/http_request.h"
#endif
#ifdef SVP_KEYBOARD
#include "esphome/components/hid/hid_input.h"
#endif
#ifdef SVP_KEYBOARD_ENTITIES
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#endif
#ifdef SVP_MEDIA_PLAYER
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/media_player/media_player.h"
#endif
#ifdef SVP_CAPTURE
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "cast_avi.h"
#endif
#ifdef SVP_CAPTURE_MIC
#include "esphome/components/microphone/microphone_source.h"
#endif
// ring_buffer::RingBuffer is used for the audio input/decoded rings (see USE_AUDIO members).
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "buffered_file_reader.h"
#include "avi_parser.h"
#include "psram_pool.h"
#include "pool_list.h"
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdio>
#include <atomic>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

namespace esphome::simple_video_player {

/// The player's tasks, stacks in PSRAM (CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM, set by codegen):
/// none of them works on the flash itself, so no stack is touched while the cache is off. The TCB
/// stays internal (xTaskCreatePinnedToCoreWithCaps).
inline bool create_psram_task(TaskFunction_t entry, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                              TaskHandle_t *handle, BaseType_t core) {
  return xTaskCreatePinnedToCoreWithCaps(entry, name, stack, arg, priority, handle, core,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

/// Speaker channel modes for audio routing
enum class SpeakerChannelMode : uint8_t {
  SPEAKER_CHANNEL_MONO = 0,    // Downmix stereo to mono (average L+R)
  SPEAKER_CHANNEL_LEFT = 1,    // Use only left channel
  SPEAKER_CHANNEL_RIGHT = 2,   // Use only right channel
  SPEAKER_CHANNEL_STEREO = 3,  // Pass through stereo unchanged
};

/// Player states
enum class PlayerState : uint8_t {
  STOPPED = 0,
  PLAYING = 1,
  PAUSED = 2,
  ERROR = 3,
};

/// Playback error codes
/// What follows the end of a file: nothing, the same file again, or the playlist from the top.
enum class RepeatMode : uint8_t {
  OFF = 0,
  FILE = 1,
  PLAYLIST = 2,
};

enum class PlaybackError : uint8_t {
  NONE = 0,
  FILE_NOT_FOUND = 1,
  DECODER_INIT_FAILED = 2,
  BUFFER_ALLOCATION_FAILED = 3,
  DECODE_ERROR = 4,
  FILE_READ_ERROR = 5,
  INVALID_VIDEO_FORMAT = 6,
};

/// Video file formats
enum class VideoFormat : uint8_t {
  UNKNOWN = 0,
  RAW_MJPEG = 1,  // Raw concatenated JPEG frames
  AVI_MJPEG = 2,  // AVI container with MJPEG video
};

// Forward declarations for automation
class SimpleVideoPlayer;
#ifdef SVP_EMULATOR
class GameEmulator;
#endif
#ifdef SVP_SPICE_USB
class SpiceUsbSlot;
#endif

#if defined(SVP_KEYBOARD_ENTITIES) && defined(USE_SELECT)
/// Home Assistant's keyboard device select (keyboard_layouts: device_select): the keyboards' standards
/// and their devices; the one picked is what the layout sensor shows.
class KeyboardDeviceSelect : public select::Select {
 public:
  void set_parent(SimpleVideoPlayer *parent) { this->parent_ = parent; }

 protected:
  void control(size_t index) override;
  SimpleVideoPlayer *parent_{nullptr};
};
#endif

#if defined(USE_AUDIO) || defined(SVP_CAPTURE_MIC)
/// ring_buffer::RingBuffer in the player's pool memory (create() allocates its own); the object with
/// its ring structure in PSRAM only.
class PoolRingBuffer : public ring_buffer::RingBuffer {
 public:
  ~PoolRingBuffer() {
    if (this->created_)
      vRingbufferDelete(this->handle_());
  }
  static std::shared_ptr<ring_buffer::RingBuffer> create(uint8_t *storage, size_t len);

 protected:
  bool created_{false};
};
#endif

/// Trigger fired when playback starts
class PlaybackStartedTrigger : public Trigger<> {
 public:
  explicit PlaybackStartedTrigger(SimpleVideoPlayer *parent);
};

/// Trigger fired when playback finishes normally
class PlaybackFinishedTrigger : public Trigger<> {
 public:
  explicit PlaybackFinishedTrigger(SimpleVideoPlayer *parent);
};

/// Trigger fired when playback is paused
class PlaybackPausedTrigger : public Trigger<> {
 public:
  explicit PlaybackPausedTrigger(SimpleVideoPlayer *parent);
};

/// Trigger fired when playback error occurs
class PlaybackErrorTrigger : public Trigger<uint8_t> {
 public:
  explicit PlaybackErrorTrigger(SimpleVideoPlayer *parent);
};

#ifdef SVP_CONTROLS
/// Touches for the on-screen controls (LVGL is paused while a file plays on the panel).
class ControlsTouchListener : public touchscreen::TouchListener {
 public:
  explicit ControlsTouchListener(SimpleVideoPlayer *parent) : parent_(parent) {}
  void update(const touchscreen::TouchPoints_t &points) override;
  void release() override;

 protected:
  SimpleVideoPlayer *parent_;
};
#endif

#ifdef SVP_STREAM_INPUT
/// What LVGL's keyboards, remotes and mice report while it is paused, for the network stream sender.
class StreamInputTaker : public lvgl::LvglComponent::PausedInput {
 public:
  explicit StreamInputTaker(SimpleVideoPlayer *parent) : parent_(parent) {}
  bool paused_key(uint16_t page, uint16_t usage, uint8_t mods, bool pressed, int action, uint32_t codepoint,
                  bool level3, uint32_t source) override;
  bool paused_text(uint32_t codepoint) override;
  bool paused_pointer(int16_t dx, int16_t dy, int16_t wheel, uint8_t buttons) override;

 protected:
  SimpleVideoPlayer *parent_;
};
#endif

#ifdef SVP_STREAM_TOUCH
/// Forwards the touchscreen to the network stream sender ('T' messages).
class StreamTouchListener : public touchscreen::TouchListener {
 public:
  explicit StreamTouchListener(SimpleVideoPlayer *parent) : parent_(parent) {}
  void update(const touchscreen::TouchPoints_t &points) override;
  void release() override;

 protected:
  SimpleVideoPlayer *parent_;
};
#endif

#ifdef SVP_PICTURES
enum class SlideshowMode : uint8_t { STOPPED, PLAYING, PAUSED };
enum class ImageFitMode : uint8_t {
  SCALE_TO_FIT,   // whole picture, aspect kept (bars)
  SCALE_TO_FILL,  // fills the canvas, aspect kept (cropped)
  STRETCH,        // fills the canvas, aspect ignored
  CENTER,         // unscaled, centred
};
enum class ThumbnailLayout : uint8_t { HORIZONTAL, VERTICAL, GRID };
enum class ThumbnailSlideEdge : uint8_t { LEFT, RIGHT, TOP, BOTTOM };
#endif

/// Names and paths of the lists (pictures, playlist, channels) in PSRAM: plain new/malloc only takes
/// internal RAM here (CONFIG_SPIRAM_USE_CAPS_ALLOC). Internal RAM only if PSRAM is out.
template<class T> struct PsramAllocator {
  using value_type = T;
  PsramAllocator() = default;
  template<class U> constexpr PsramAllocator(const PsramAllocator<U> & /*other*/) noexcept {}
  T *allocate(size_t n) {
    auto *p = static_cast<T *>(
        heap_caps_malloc_prefer(n * sizeof(T), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT));
    if (p == nullptr)
      abort();
    return p;
  }
  void deallocate(T *p, size_t /*n*/) { heap_caps_free(p); }
};
template<class T, class U> bool operator==(const PsramAllocator<T> & /*a*/, const PsramAllocator<U> & /*b*/) {
  return true;
}
template<class T, class U> bool operator!=(const PsramAllocator<T> & /*a*/, const PsramAllocator<U> & /*b*/) {
  return false;
}
using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
template<class T> using PsramVector = std::vector<T, PsramAllocator<T>>;
/// An object of the player's in a piece at its pool's end (counted in plan_pool_()); empty when the
/// pool has no room for it.
inline void *pool_object_alloc(size_t size) {
  PsramPool *pool = PsramPool::instance();
  return pool != nullptr ? pool->alloc_top(size) : nullptr;
}
inline void pool_object_free(void *ptr) {
  PsramPool *pool = PsramPool::instance();
  if (pool != nullptr)
    pool->free(static_cast<uint8_t *>(ptr));
}
template<class T> struct PoolDeleter {
  void operator()(T *p) const {
    p->~T();
    pool_object_free(p);
  }
};
template<class T> using PsramPtr = std::unique_ptr<T, PoolDeleter<T>>;
template<class T, class... Args> PsramPtr<T> psram_make(Args &&...args) {
  void *mem = pool_object_alloc(sizeof(T));
  if (mem == nullptr)
    return PsramPtr<T>();
  return PsramPtr<T>(::new (mem) T(std::forward<Args>(args)...));
}
/// For a shared_ptr's control block with its object in one pool piece (allocate_shared).
template<class T> struct PoolAllocator {
  using value_type = T;
  PoolAllocator() = default;
  template<class U> constexpr PoolAllocator(const PoolAllocator<U> & /*other*/) noexcept {}
  T *allocate(size_t n) {
    auto *p = static_cast<T *>(pool_object_alloc(n * sizeof(T)));
    if (p == nullptr)
      abort();
    return p;
  }
  void deallocate(T *p, size_t /*n*/) { pool_object_free(p); }
};
template<class T, class U> bool operator==(const PoolAllocator<T> & /*a*/, const PoolAllocator<U> & /*b*/) {
  return true;
}
template<class T, class U> bool operator!=(const PoolAllocator<T> & /*a*/, const PoolAllocator<U> & /*b*/) {
  return false;
}

using SvpPath = FixedString<256>;   // a local file's path
using SvpId = FixedString<384>;     // a local path or a Home Assistant media id
using SvpTitle = FixedString<128>;  // a name shown

#ifdef SVP_PICTURES
struct ImageEntry {
  SvpId path;
  SvpTitle filename;
  uint64_t size{0};
};

/// Fired with the image index when a thumbnail is tapped (replaces showing that image).
class ThumbnailClickTrigger : public Trigger<size_t> {
 public:
  explicit ThumbnailClickTrigger(SimpleVideoPlayer *parent);
};
#endif

/// Main video player component
class SimpleVideoPlayer : public Component
#ifdef SVP_MEDIA_PLAYER
    , public api::CustomAPIDevice
#endif
{
 public:
  ~SimpleVideoPlayer();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  //========================================================================
  // Configuration (called from codegen)
  //========================================================================

#ifdef SVP_USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl_component) { this->lvgl_component_ = lvgl_component; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  // The canvas widget's size (YAML): the output buffers are made for it, and nothing larger plays.
  void set_canvas_size(uint16_t width, uint16_t height) {
    this->canvas_size_w_ = width;
    this->canvas_size_h_ = height;
  }
#endif
  void set_psram_pool_size(uint32_t size) { this->psram_pool_size_ = size; }
  /// What the read-ahead ring holds ahead besides the sound's lead (ms, at MAX_MEDIA_RATE); from
  /// the next video on. Kept within MIN_PREBUFFER_MS .. max_prebuffer().
  void set_prebuffer(uint32_t ms);
  uint32_t get_prebuffer() const { return this->prebuffer_ms_.load(std::memory_order_relaxed); }
  uint32_t max_prebuffer() const;
  static constexpr uint32_t MIN_PREBUFFER_MS = 100;
  void set_target_fps(float fps) { this->target_fps_ = fps; }

#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  /// How long the audio output takes to be heard (ms): the sound starts this much before the picture.
  template<typename V> void set_audio_latency(V latency) { this->audio_latency_ = latency; }
  template<typename V> void set_audio_preroll(V preroll) { this->audio_preroll_ = preroll; }
  /// The most the sound may lead with the prebuffer set (ms): ring and sound fit in their share.
  uint32_t max_audio_latency() const { return this->max_audio_latency_ms_; }
  void set_speaker_channel_mode(SpeakerChannelMode mode) { this->speaker_channel_mode_ = mode; }
#endif
#ifdef SVP_AUDIO_ROUTER
  /// The speaker is this router: sound is wanted in its selected output's rate and channel count.
  void set_audio_router(resampler::ResamplerSpeaker *router) { this->audio_router_ = router; }
#endif
#ifdef SVP_AUDIO_FILES
  // Audio files (audio_file_speaker_id, a resampler in front of the speaker): a queue of their own,
  // apart from the video playlist, played in the same sessions as a video (one at a time).
  void set_audio_file_speaker(speaker::Speaker *speaker) { this->file_speaker_ = speaker; }
  /// path: a file, or a Home Assistant media source id (played by svp_relay); title: shown for it
  /// (empty: the file's name).
  void audio_queue_add(const std::string &path, const std::string &title = "");
  void audio_queue_clear();
  /// Plays track `index` of the queue.
  void audio_play(size_t index);
  void audio_next();
  /// The track again after a few seconds of it, else the one before.
  void audio_previous();
  /// Resumes a paused track, plays the current one again after a stop; at the end of the queue (or
  /// before anything played) the queue starts over, shuffled anew.
  void audio_play_or_resume();
  void set_audio_shuffle(bool shuffle);
  bool get_audio_shuffle() const { return this->audio_shuffle_; }
  void set_audio_repeat(RepeatMode mode) {
    this->audio_repeat_ = mode;
    this->audio_changed_.store(true, std::memory_order_release);
  }
  RepeatMode get_audio_repeat() const { return this->audio_repeat_; }
  size_t audio_queue_size() const { return this->audio_queue_.size(); }
  /// Counts the changes of the queue's tracks (added, cleared).
  uint32_t audio_queue_generation() const { return this->audio_queue_gen_; }
  TextView audio_queue_item(size_t index) const { return this->audio_queue_[index]; }
  std::string audio_queue_title(size_t index) const;
  /// The queue's track that plays or played last; -1 for none.
  int audio_queue_index() const {
    return this->audio_current_ ? static_cast<int>(this->audio_order_[this->audio_pos_]) : -1;
  }
  /// The session plays a track of the queue: get_state() is its (a Home Assistant track's is the
  /// network session's).
  bool is_audio_session() const {
    return this->audio_session_.load(std::memory_order_acquire) ||
           this->audio_remote_.load(std::memory_order_acquire) != AUDIO_REMOTE_NONE;
  }
  /// Main loop: the queue, its track or the state of a track changed.
  void add_on_audio_callback(std::function<void()> &&callback) { this->audio_callbacks_.add(std::move(callback)); }
#endif
  // The device's one volume, 0..1: this player's speaker and, with media_player_id, the media player's.
  void set_volume(float volume);
  float get_volume();

#ifdef SVP_MEDIA_PLAYER
  // The panel's audio player (YAML media_player_id, internal): svp_relay plays sound through the
  // actions registered in setup() and shows it in Home Assistant as part of the panel's one player.
  void set_media_player(media_player::MediaPlayer *player) { this->media_player_ = player; }
  void on_svp_play_audio(std::string url, bool announce);
  void on_svp_audio_command(std::string command);
  void on_svp_set_volume(float volume);
#endif
#ifdef SVP_BACKGROUND
  /// A picture file behind a page in this canvas (external_buffer), fitted as the pictures: decoded into
  /// a piece of the pool (the pictures' share) while the page shows, given back when it is left or another
  /// user needs the room (shown again the next time).
  void set_background_canvas(lv_obj_t *canvas) { this->bg_canvas_ = canvas; }
  /// The file (a full VFS path; "" none): shown anew when it changes.
  void set_background_path(const std::string &path);
  /// The page with the background shows (true) or is left (false: its piece goes back).
  void set_background_wanted(bool wanted);
  bool background_shown() const { return this->bg_shown_; }
#endif
#ifdef SVP_AUDIO_COVER
  /// The playing sound's cover art (svp_relay) shows in these canvases (external_buffer), each fitted
  /// into its own size; at most COVER_VIEWS.
  void add_audio_cover_canvas(lv_obj_t *canvas) {
    if (this->cover_view_count_ < COVER_VIEWS)
      this->cover_views_[this->cover_view_count_++].canvas = canvas;
  }
  /// svp_relay: the playing sound's cover art changed: fetched again, or cleared when there is none.
  void on_svp_cover();
  /// Main loop: the cover art shows.
  bool has_audio_cover() const { return this->cover_shown_; }
#endif

  // What svp_relay reads from the status text sensor: a short JSON (Home Assistant states are at most
  // 255 characters). Status callbacks run whenever it may have changed.
  std::string status_json();
  void add_on_status_callback(std::function<void()> &&callback) { this->status_callbacks_.add(std::move(callback)); }
  /// A video file of the panel's own plays or is paused (not an AVI of the relay's, not an audio file).
  bool local_video_active() const {
    const PlayerState state = this->state_.load(std::memory_order_acquire);
    bool audio_file = false;
#ifdef SVP_AUDIO_FILES
    audio_file = this->audio_session_.load(std::memory_order_acquire);
#endif
    bool relay_avi = false;
#ifdef SVP_STREAM
    relay_avi = this->net_session_;
#endif
    return (state == PlayerState::PLAYING || state == PlayerState::PAUSED) && !relay_avi && !audio_file;
  }

#ifdef SVP_DSI_OUTPUT
  // Direct output onto a display with draw_pixels_at() while LVGL is paused. Wired by the FINAL
  // codegen coroutine together with the display's color_depth (16 or 24).
  void set_dsi(display::Display *dsi) { this->dsi_ = dsi; }
  void set_dsi_color_depth(uint8_t bits) { this->dsi_fb_bpp_ = bits == 24 ? 3 : 2; }
#endif

#ifdef SVP_STREAM
  // Network receiver: an AVI (svp_relay) plays like a file; with direct display output also
  // Portall-style udisp JPEG rectangles, drawn while no file is playing.
  void set_stream_port(uint16_t port) { this->stream_port_ = port; }
  // Tell the sender the panel sleeps (it stops drawing) or is awake again (it redraws), as Portall's
  // portall.sleep / portall.wake.
  void stream_sleep(bool asleep);
  // The output a stream's picture is made for: the display (fullscreen) or the canvas. A connected
  // sender is told the new picture at once.
  void set_stream_fullscreen(bool fullscreen);
  bool canvas_upright_() const {
#ifdef SVP_USE_LVGL
    if (this->lvgl_component_ != nullptr)
      return this->lvgl_component_->get_rotation() == display::DISPLAY_ROTATION_0_DEGREES;
#endif
#ifdef SVP_DSI_OUTPUT
    if (this->dsi_ != nullptr)
      return this->dsi_->get_rotation() == display::DISPLAY_ROTATION_0_DEGREES;
#endif
    return true;
  }
  void init_stream_fullscreen(bool fullscreen) { this->stream_fullscreen_.store(fullscreen); }
  bool is_stream_fullscreen() const { return this->stream_direct_(); }
  // A line per second while a sender is connected (stream_statistics): what arrived, what was drawn.
  void set_stream_statistics(bool enable);
#ifdef SVP_STREAM_TOUCH
  void set_stream_touchscreen(touchscreen::Touchscreen *ts) { this->stream_touchscreen_ = ts; }
  void stream_on_touch(const touchscreen::TouchPoints_t &points);
#endif
#ifdef SVP_STREAM_INPUT
  /// A message for the sender ('K', 'X', 'P'), when it asked for input and its picture is shown.
  bool stream_on_input(const uint8_t *message, uint8_t length);
#endif
#ifdef SVP_SPICE
  /// SPICE client (spice_client.cpp): the server's desktop on the stream's output, its pictures
  /// (JPEG, MJPEG streams, plain bitmaps) drawn like udisp rectangles. The panel is held meanwhile.
  void spice_connect(const std::string &host, uint16_t port, const std::string &password);
  /// The Browser: svp_relay's host runs a browser in a VNC server of its own, shown as a VNC desktop
  /// (one per panel; no host, port or password: the relay starts and knows it).
  void spice_connect_browser();
  /// The connect asked for last is the Browser's (its session, or why it failed).
  bool is_spice_browser() const { return this->spice_browser_.load(std::memory_order_acquire); }
  void spice_disconnect();
  bool is_spice_connected() const { return this->spice_on_.load(std::memory_order_acquire); }
  /// A connect under way; the last one ended without a session (refused, unreachable, wrong password).
  bool is_spice_connecting() const { return this->spice_connecting_.load(std::memory_order_acquire); }
  bool spice_failed() const { return this->spice_failed_.load(std::memory_order_acquire); }
  /// Any task: the SPICE task has work (input, USB completions).
  void spice_wake();
  /// Whether the speaker's sound reaches the device the audio files / the media player play on
  /// (codegen, by their speaker chains): SPICE sound stops them then.
  void set_spice_sound_shares(bool files, bool media_player) {
    this->spice_sound_shares_files_ = files;
    this->spice_sound_shares_media_player_ = media_player;
  }
  // Saved connections: name, host and port (the password is given at each connect), spice: connections.
  void set_spice_connections(uint8_t count) { this->spice_saved_count_ = count; }
  /// The saved connections, each with the USB device of every redirection slot, in this file (a
  /// storage path) instead of preferences; read at boot, written whenever one changes.
  void set_spice_connections_file(const std::string &path) { this->spice_file_path_ = path; }
  uint8_t spice_saved_count() const { return this->spice_saved_count_; }
  /// The connection saved at index; false when there is none.
  bool spice_saved(uint8_t index, std::string &name, std::string &host, uint16_t &port) const;
  void spice_save(uint8_t index, const std::string &name, const std::string &host, uint16_t port);
  void spice_forget(uint8_t index);
  /// Saved at the first free index (the Desktop screen's "save"); -1: all are taken.
  int spice_save_new(const std::string &name, const std::string &host, uint16_t port);
  void spice_connect_saved(uint8_t index, const std::string &password);
  /// Main loop: a saved connection changed, or a session began or ended.
  void add_on_spice_callback(std::function<void()> &&callback) { this->spice_callbacks_.add(std::move(callback)); }
  // Leaving a session on the panel, as the dashboard: the close key held (bit 0 Back, bit 1 Home;
  // pressed briefly it goes to the desktop), or two fingers held.
  void set_spice_close_keys(uint8_t keys) { this->spice_close_keys_.store(keys, std::memory_order_release); }
  void set_spice_close_hold(float seconds) {
    this->spice_close_hold_ms_.store(static_cast<uint32_t>(seconds * 1000), std::memory_order_release);
  }
  void set_spice_touch_hold(float seconds) {
    this->spice_touch_hold_ms_.store(static_cast<uint32_t>(seconds * 1000), std::memory_order_release);
  }
  /// A remote desktop session's memory, from the pool (what pictures and a video kept went back to it
  /// as the session took the panel).
  uint8_t *desktop_alloc(size_t size);
  void desktop_free(uint8_t *piece) { this->pool_.free(piece); }
#ifdef SVP_SPICE_USB
  void add_spice_usb_slot(SpiceUsbSlot *slot);
  /// Main loop: a slot picked the device at addr; the others let it go.
  void spice_usb_selected(SpiceUsbSlot *slot, uint8_t addr);
  /// Main loop: a slot's device was picked or let go; kept with the saved connection in use.
  void spice_usb_changed(SpiceUsbSlot *slot);
  /// VNC session: the slot's device is imported by the desktop's host (`usbip attach -r <panel> -b
  /// 1-<slot+1>`); its change runs the spice callbacks.
  bool usbip_attached(uint8_t slot) const { return (this->usbip_attached_.load(std::memory_order_acquire) >> slot) & 1; }
#endif
#endif
#endif

#ifdef SVP_CHANNEL_LIST
  // Channel list from the svp_relay Home Assistant integration, optionally shown in an LVGL dropdown;
  // channel_play() asks the relay to stream a channel to stream_port.
  void set_channel_url(const std::string &url) { this->channel_url_ = url; }
  void set_channel_http_request(http_request::HttpRequestComponent *http) { this->channel_http_request_ = http; }
  void set_channel_token(const std::string &token) { this->channel_token_ = token; }
  void set_channel_widget(lvgl::LvDropdownType *widget) { this->channel_widget_ = widget; }
  void channel_list_refresh() { this->channel_request_(CHANNEL_CMD_REFRESH, 0); }
  // The dropdown's selection, without one the channel played last.
  void channel_play();
  void channel_play(int index);
  void channel_stop() { this->channel_request_(CHANNEL_CMD_STOP, 0); }
  // Main loop: the names of the channels; the version changes with every new list.
  const PoolList<FixedString<64>> &get_channel_names() const { return this->channel_names_; }
  uint32_t get_channel_list_version() const { return this->channel_version_; }
  // The channel played last, -1 for none.
  int get_channel_index() const { return this->channel_index_; }

  // Home Assistant media through svp_relay: the media sources that have a type ("audio", "video",
  // "image"; id empty) or a folder of one, listed for the main loop (add_on_ha_browse_callback); a
  // sound or video played through the relay, pictures shown on the picture canvas like local ones.
  struct HaItem {
    SvpTitle title;
    SvpId id;
    bool dir{false};
  };
  void ha_browse(const std::string &type, const std::string &id);
  // The sources of a type, listed last; whether the callback is for them (else for a folder).
  const PoolList<HaItem> &ha_source_items() const { return this->ha_src_items_; }
  TextView ha_sources_type() const { return this->ha_texts_ ? TextView(this->ha_texts_->sources_type) : TextView(); }
  bool ha_sources_listed() const { return this->ha_src_ok_; }
  bool ha_last_sources() const { return this->ha_last_sources_; }
  // The folder listed last.
  const PoolList<HaItem> &ha_items() const { return this->ha_items_; }
  TextView ha_type() const { return this->ha_texts_ ? TextView(this->ha_texts_->listing().type) : TextView(); }
  TextView ha_id() const { return this->ha_texts_ ? TextView(this->ha_texts_->listing().id) : TextView(); }
  TextView ha_title() const { return this->ha_texts_ ? TextView(this->ha_texts_->listing().title) : TextView(); }
  TextView ha_path() const { return this->ha_texts_ ? TextView(this->ha_texts_->listing().path) : TextView(); }
  TextView ha_parent() const {  // empty: none
    return this->ha_texts_ ? TextView(this->ha_texts_->listing().parent) : TextView();
  }
  bool ha_listed() const { return this->ha_ok_; }  // false: the last listing failed
  void ha_play(const std::string &id);
  // The pictures of the folder listed, from ha_items()[index] on.
  bool ha_show_pictures(size_t index);
  void add_on_ha_browse_callback(std::function<void()> &&callback) { this->ha_callbacks_.add(std::move(callback)); }

  // This panel's settings in svp_relay (its options in Home Assistant), read and changed here.
  void relay_options_fetch();
  bool relay_options_ok() const { return this->opt_ok_; }
  std::string relay_option(const std::string &key) const;
  // The values a setting takes, one per line (an LVGL dropdown's options); empty: free.
  std::string relay_choices(const std::string &key) const;
  void relay_option_set(const std::string &key, const std::string &value);
  void add_on_relay_options_callback(std::function<void()> &&callback) {
    this->opt_callbacks_.add(std::move(callback));
  }
#endif

#ifdef SVP_KEYBOARD
  // Keyboard layouts (keyboard_layouts:): each device behind the keyboards (hid::InputDevice) types
  // with the layout given it, else with its keyboard's (input's) standard one, else with the
  // input's own. Layouts are read from the directory, else had from svp_relay (GET /keyboard) and
  // then kept there. What is given each is kept in preferences. Main loop.
  void set_keyboard_directory(const std::string &dir) {
    this->kbd_dir_ = dir;
    this->kbd_dir_default_ = dir;
  }
  /// A keyboard (input): its name, and its standard layout from the configuration ("" its own).
  void add_keyboard(hid::HIDInput *input, const std::string &name, const std::string &layout);
  /// The directory changed (the "Keyboard layout folder"; "" the configuration's): its layouts from now on.
  void keyboard_set_directory(const std::string &dir);
  const std::string &keyboard_default_directory() const { return this->kbd_dir_default_; }
  const std::string &keyboard_directory() const { return this->kbd_dir_; }
  /// The list: svp_relay's, else the directory's (names only); the callback once it is there.
  void keyboard_list_fetch();
  bool keyboard_listed() const { return this->kbd_listed_; }  // false: on its way, or none
  bool keyboard_list_remote() const { return this->kbd_list_remote_; }  // svp_relay's
  size_t keyboard_count() const { return this->kbd_count_; }
  const char *keyboard_name(size_t index) const;
  const char *keyboard_description(size_t index) const;  // "" for none
  bool keyboard_kept(size_t index) const;                // its file is in the directory
  int keyboard_index(const std::string &name) const;      // -1: not in the list
  /// A layout's description where the list has one, else its name; "" stays "".
  std::string keyboard_label(const std::string &name) const;
  /// Up to max indexes of the layouts whose name or description holds query (any case), for an empty
  /// query those kept; returns how many there are in all.
  size_t keyboard_find(const std::string &query, int *out, size_t max) const;

  struct KeyboardDevice {
    FixedString<48> key;  // hid::InputDevice's; "@" and the keyboard's name for a keyboard's standard
    FixedString<48> name;
    uint8_t input{0};     // which keyboard
    bool connected{false};
    FixedString<32> layout;  // given it: "" none (the keyboard's standard / for a standard: its own)
  };
  size_t keyboard_input_count() const { return this->kbd_inputs_.size(); }
  const std::string &keyboard_input_name(size_t index) const { return this->kbd_inputs_[index].name; }
  /// Each keyboard's standard (key "@name") and every device of theirs: those behind them now, and
  /// the USB ones seen before. Made anew by every call (one list, kept by the player).
  const PoolList<KeyboardDevice> &keyboard_devices() const;
  /// The layout given a device or a keyboard's standard (key as KeyboardDevice's), "" none: kept,
  /// then put on it once read. False: no such device, or no room to keep it.
  bool keyboard_assign(const std::string &key, const std::string &layout);
  /// The same by a device's key or name, or a keyboard's name (Home Assistant).
  bool keyboard_assign_named(const std::string &device, const std::string &layout);
  /// "name: layout" of each, one per line (a text sensor).
  std::string keyboard_summary() const;
  /// The layouts to have in the directory, from svp_relay, one after another.
  void keyboard_download(const std::vector<std::string> &names);
  size_t keyboard_downloads_left() const { return this->kbd_downloads_.size(); }
  bool keyboard_loading() const { return this->kbd_step_ != KBD_IDLE; }
  /// A layout asked for that could not be had (neither in the directory nor from svp_relay).
  bool keyboard_failed(const std::string &name) const;
  void add_on_keyboard_callback(std::function<void()> &&callback) { this->kbd_callbacks_.add(std::move(callback)); }
#ifdef SVP_KEYBOARD_ENTITIES
#ifdef USE_SELECT
  void set_keyboard_select(select::Select *select) { this->kbd_select_ = select; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_keyboard_layout_sensor(text_sensor::TextSensor *sensor) { this->kbd_layout_sensor_ = sensor; }
#endif
  /// The device select's option `index` picked: the sensor shows its layout.
  void keyboard_entity_pick(size_t index);
#endif
#endif

  //========================================================================
  // Playback Control API
  //========================================================================

  /// Start playing a video file (local or network path)
  void play(const std::string &video_path) { this->play(video_path, false); }
  /// folder_playlist: the playlist becomes every video of the file's folder (by name), else just this file.
  void play(const std::string &video_path, bool folder_playlist);
  /// The playlist (full paths); plays item `start`.
  void play_list(std::vector<std::string> paths, size_t start = 0);
  /// Next / previous item of the playlist. previous() restarts the file if it has played for a few
  /// seconds.
  void next();
  void previous();
  size_t playlist_size() const { return this->playlist_.size(); }
  size_t playlist_index() const { return this->playlist_index_; }

  /// Pause playback (can be resumed)
  void pause();
  /// pause() while playing, resume() while paused.
  void toggle_pause() {
    if (this->is_playing()) {
      this->pause();
    } else if (this->is_paused()) {
      this->resume();
    }
  }

  /// Resume playback from paused state
  void resume();

  /// Stop playback completely
  void stop();

  /// Set loop mode (restart from beginning when finished)
  void set_loop(bool loop) { this->set_repeat(loop ? RepeatMode::FILE : RepeatMode::OFF); }
  void set_repeat(RepeatMode mode) {
    this->repeat_ = mode;
    this->loop_ = mode == RepeatMode::FILE;
  }
  RepeatMode get_repeat() const { return this->repeat_; }

  /// Jumps to `seconds` into a playing or paused local AVI file (clamped to the file); paused stays
  /// paused. The first jump in a file reads its index.
  void seek(float seconds);
  /// seek() relative to the position shown.
  void seek_relative(float seconds) { this->seek(this->get_position() + seconds); }
#ifdef SVP_CONTROLS
  // On-screen controls: a bar of LVGL widgets the player builds itself (which ones and their styles
  // from the configuration). Over a video on the panel it is rendered before playback starts, as
  // pictures the playback task copies into each frame: no LVGL drawing while the video plays. A tap
  // shows it, taps on its buttons act, a tap or drag on the seek bar seeks; it hides after
  // hide_after without a touch while playing.
  enum ControlButton : uint8_t {
    CTL_PREVIOUS,
    CTL_REWIND,
    CTL_PLAY_PAUSE,
    CTL_FORWARD,
    CTL_NEXT,
    CTL_STOP,
    CTL_REPEAT,
    CTL_VOLUME_DOWN,
    CTL_VOLUME_UP,
    CTL_EXIT,
    CTL_SLIDE_TIME,  // over a picture: the slideshow's picture duration (a dropdown)
    CTL_BUTTON_COUNT,
  };
  void set_controls_touchscreen(touchscreen::Touchscreen *touchscreen) { this->ctl_touchscreen_ = touchscreen; }
  void set_controls_hide_after(uint32_t ms) { this->ctl_hide_after_ms_ = ms; }
  /// Styles added over the built-in look (any may be null).
  void set_controls_styles(lv_style_t *bar, lv_style_t *button, lv_style_t *accent_button, lv_style_t *seek_bar,
                           lv_style_t *seek_indicator, lv_style_t *focus) {
    this->ctl_user_bar_style_ = bar;
    this->ctl_user_button_style_ = button;
    this->ctl_user_accent_style_ = accent_button;
    this->ctl_user_seek_style_ = seek_bar;
    this->ctl_user_indicator_style_ = seek_indicator;
    this->ctl_user_focus_style_ = focus;
  }
  void set_video_controls(std::vector<uint8_t> buttons, bool title, bool time, bool seek_bar) {
    this->ctl_video_.buttons.assign(buttons.begin(), buttons.end());
    this->ctl_video_.title = title;
    this->ctl_video_.time = time;
    this->ctl_video_.seek_bar = seek_bar;
    this->ctl_video_.enabled = true;
  }
#ifdef SVP_PICTURES
  /// Over a picture in full screen (LVGL shows them): time is the picture's number in the folder.
  void set_picture_controls(std::vector<uint8_t> buttons, bool title, bool counter) {
    this->ctl_picture_.buttons.assign(buttons.begin(), buttons.end());
    this->ctl_picture_.title = title;
    this->ctl_picture_.time = counter;
    this->ctl_picture_.seek_bar = false;
    this->ctl_picture_.picture = true;
    this->ctl_picture_.enabled = true;
  }
  /// The exit button over a picture: leaving full screen is the configuration's (its layout).
  Trigger<> *get_picture_exit_trigger() { return &this->ctl_picture_.exit_trigger; }
#endif
#ifdef SVP_AUDIO_CONTROLS
  /// Over the audio queue's cover art in full screen (LVGL shows them, as over a picture): time and
  /// seek bar while the track's length is known.
  void set_audio_controls(std::vector<uint8_t> buttons, bool title, bool time) {
    this->ctl_audio_.buttons.assign(buttons.begin(), buttons.end());
    this->ctl_audio_.title = title;
    this->ctl_audio_.time = time;
    this->ctl_audio_.seek_bar = time;
    this->ctl_audio_.audio = true;
    this->ctl_audio_.enabled = true;
  }
  /// The exit button over the cover art: leaving full screen is the configuration's (its layout).
  Trigger<> *get_audio_exit_trigger() { return &this->ctl_audio_.exit_trigger; }
  /// The configuration shows the cover art in full screen: show_controls() means this bar then.
  void set_audio_fullscreen(bool fullscreen);
  bool is_audio_fullscreen() const { return this->audio_fullscreen_; }
#endif
  /// Over a full-screen video, or a picture in full screen.
  void show_controls();
  void hide_controls();
  /// Whether the controls show (over a full-screen video, or a picture in full screen).
  bool controls_shown() const;
  /// Listener: a touch (pressed, display coordinates) or its release.
  void controls_touch(bool pressed, int32_t x, int32_t y);
#endif
#ifdef SVP_PICTURES
  // Pictures: JPEG/PNG/BMP files of a folder shown in an LVGL canvas, with slideshow, play/pause
  // overlay, swipe navigation and a thumbnail strip. Main loop only; files are read through the
  // storage worker and decoded by runtime_image. Held while a video plays.
  void set_picture_canvas(lv_obj_t *canvas) { this->pic_canvas_ = canvas; }
  void add_picture_directory(const std::string &path) { this->pic_dirs_.emplace_back(path.c_str()); }
  void set_slideshow_interval(uint32_t ms) { this->slideshow_interval_ms_ = ms; }
  uint32_t get_slideshow_interval() const { return this->slideshow_interval_ms_; }
  /// The picture durations offered (the Media page, the controls' slide_time), and the one nearest ms.
  static constexpr uint16_t SLIDESHOW_STEPS_S[] = {3, 5, 8, 10, 15, 20, 30, 60};
  static constexpr size_t SLIDESHOW_STEP_COUNT = sizeof(SLIDESHOW_STEPS_S) / sizeof(SLIDESHOW_STEPS_S[0]);
  static size_t slideshow_step_index(uint32_t ms);
  /// "3 s", "5 s", ...: a step as shown (static); all of them as an LVGL dropdown's options.
  static const char *slideshow_step_text(size_t index);
  static std::string slideshow_step_options();
  /// Main loop: a duration picked on the controls over a picture (slide_time), in ms.
  void add_on_slideshow_interval_callback(std::function<void(uint32_t)> &&callback) {
    this->slideshow_interval_callbacks_.add(std::move(callback));
  }
  void set_fit_mode(ImageFitMode mode) { this->fit_mode_ = mode; }
  void set_overlay_icon_size(int size) { this->overlay_icon_size_ = size; }
  void set_overlay_icon_color(uint32_t color) { this->overlay_icon_color_ = color; }
  void set_overlay_duration(uint32_t ms) { this->overlay_duration_ms_ = ms; }
  /// -1: nothing is shown until asked for.
  void set_default_image_index(int index) { this->default_image_index_ = index; }
  void set_long_press_time(uint32_t ms) { this->long_press_time_ms_ = ms; }
  void set_enable_thumbnails(bool enable) { this->thumbs_enabled_ = enable; }
  void set_thumbnail_size(int width, int height) {
    this->thumb_w_ = width;
    this->thumb_h_ = height;
  }
  void set_thumbnail_max_count(size_t count) { this->thumb_count_ = count; }
  void set_thumbnail_lazy_load(bool lazy) { this->thumb_lazy_ = lazy; }
  void set_thumbnail_preload_count(size_t count) { this->thumb_preload_ = count; }
  void set_thumbnail_layout(ThumbnailLayout layout) { this->thumb_layout_ = layout; }
  void set_thumbnail_spacing(int spacing) { this->thumb_spacing_ = spacing; }
  void set_thumbnail_grid_columns(int columns) { this->thumb_columns_ = columns; }
  void set_thumbnail_container(lv_obj_t *container) { this->thumb_container_ = container; }
  void set_thumbnail_style(lv_style_t *style) { this->thumb_style_ = style; }
  void set_thumbnail_active_style(lv_style_t *style) { this->thumb_active_style_ = style; }
  void set_thumbnail_container_style(lv_style_t *style) { this->thumb_container_style_ = style; }
  void set_thumbnail_label_style(lv_style_t *style) { this->thumb_label_style_ = style; }
  /// {index} (1-based), {filename}, {name} (without extension).
  void set_thumbnail_label_pattern(const std::string &pattern) { this->thumb_label_pattern_ = pattern; }
  void set_thumbnail_slide_edge(ThumbnailSlideEdge edge) { this->thumb_slide_edge_ = edge; }
  void set_thumbnail_slide_enabled(bool enabled) { this->thumb_slide_enabled_ = enabled; }
  void set_thumbnail_auto_hide_timeout(uint32_t ms) { this->thumb_auto_hide_ms_ = ms; }
  void add_on_thumbnail_click_callback(std::function<void(size_t)> &&callback) {
    this->thumb_click_callbacks_.add(std::move(callback));
    this->thumb_click_handled_ = true;
  }
  void set_thumbnail_ready_callback(std::function<void(size_t)> callback) {
    this->thumb_ready_callback_ = std::move(callback);
  }

  /// The apps beside the video / stream / desktop sessions (Olli: only pictures + audio, VNC + audio, camera + audio
  /// if it fits; everything else exclusive). One runs at a time.
  enum class AppKind : uint8_t { NONE, PICTURES, CAMERA, GAME };
  /// Main loop: @p app takes the panel - a video, a desktop session and the other app end; for the camera the
  /// audio ends too when PSRAM has no free block of @p need_bytes beside it.
  bool app_take(AppKind app, size_t need_bytes = 0);
  /// Main loop: @p app ended by itself (nothing else ends).
  void app_release(AppKind app);
  AppKind active_app() const { return this->app_; }
  /// The app ended by another taking the panel (main loop): its owner gives everything back.
  void add_on_app_end_callback(std::function<void(AppKind)> &&callback) { this->app_end_callbacks_.add(std::move(callback)); }
#ifdef SVP_EMULATOR
  GameEmulator *make_emulator();
  GameEmulator *get_emulator() const { return this->emu_; }
  bool game_touch(uint8_t fingers);
#ifdef SVP_MEDIA_PLAYER
  void on_svp_emu_start(std::string path);
  void on_svp_emu_stop();
  void on_svp_emu_pause();
  void on_svp_emu_resume();
  void on_svp_emu_save_state(int32_t slot);
  void on_svp_emu_load_state(int32_t slot);
#ifdef SVP_EMU_LINK
  void on_svp_emu_host();
  void on_svp_emu_join(std::string host);
#endif
#endif
#endif

  /// Requests an image of the current folder; it shows once read and decoded. False when out of range.
  bool show_image(size_t index);
  /// An image by full path or file name; a path outside the current folder opens its folder.
  bool show_image(const std::string &path);
  bool next_image();
  bool previous_image();
  /// No picture shown any more: the slideshow stops, the canvas empties, the pictures' buffers go back
  /// to the pool (once nothing is read or decoded).
  void close_picture();
  bool show_canvas_picture(lv_obj_t *canvas, const uint8_t *data, size_t length, const char *name,
                           std::function<void(bool)> &&done);
  void close_canvas_picture();
  bool decode_picture_into(const uint8_t *data, size_t length, const char *name, uint8_t *target, size_t capacity,
                           int32_t w, int32_t h, uint32_t stride, std::function<void(bool)> &&done);
  /// A camera's frame for the camera page (a UVC camera): decoded (MJPEG, the HW JPEG decoder) or converted (YUYV /
  /// UYVY, the PPA) and fit into target (RGB565, target_w x target_h, a pool piece) by the picture task. done(ok)
  /// on the main loop once the frame is no longer read. False: the task is busy, or the format is not shown here -
  /// the frame was not taken. target nullptr (MJPEG only): decoded into one of two scratch pieces only (the one the
  /// reader does not hold), camera_decoded() gives its pixels: the next frame can be decoded while the reader still
  /// shows the last one (as the video's decoded double buffer).
  enum class CameraFrameFormat : uint8_t { MJPEG, YUYV, UYVY, RGB565 };
  bool show_camera_frame(const uint8_t *data, size_t length, CameraFrameFormat format, uint16_t width,
                         uint16_t height, uint8_t *target, size_t target_capacity, int32_t target_w, int32_t target_h,
                         std::function<void(bool)> &&done);
  /// The camera's picture ends (main loop): its scratch piece back to the pool once no frame is decoded.
  void camera_preview_end();
  /// Main loop: the last frame decoded without a target (RGB565): pic_w x pic_h rows (the decoder's padding), w x h of
  /// it the picture, its piece held by the reader (slot) until camera_decoded_done(slot). False: none.
  bool camera_decoded(const uint8_t *&pixels, uint16_t &pic_w, uint16_t &pic_h, uint16_t &w, uint16_t &h,
                      uint8_t &slot) {
    const CameraJob &job = this->cam_job_data_;
    if (job.target != nullptr || !job.ok || this->cam_scratch_[job.slot] == nullptr)
      return false;
    pixels = this->cam_scratch_[job.slot];
    pic_w = job.pic_w;
    pic_h = job.pic_h;
    w = static_cast<uint16_t>(job.out_w);
    h = static_cast<uint16_t>(job.out_h);
    slot = job.slot;
    this->cam_held_ |= static_cast<uint8_t>(1u << job.slot);
    return true;
  }
  /// Main loop: the reader is done with a piece camera_decoded() gave.
  void camera_decoded_done(uint8_t slot) { this->cam_held_ &= static_cast<uint8_t>(~(1u << slot)); }
  /// Camera full screen through the display's direct output (as a video on the panel): LVGL paused by loop() while it
  /// runs; each frame 1:1 in the panel's own orientation, centred, black around (cut when larger), never scaled.
  /// Main loop. False: no direct output with two scanout buffers.
  bool camera_direct_start();
  void camera_direct_stop();
  /// LVGL is paused for it: frames are drawn (show_camera_frame_direct()).
  bool camera_direct_running() const { return this->cam_direct_ready_.load(std::memory_order_acquire); }
  /// A frame for the full screen: MJPEG decoded, RGB565 / YUYV / UYVY copied (converted) by the picture task into the
  /// scanout buffer not shown, then flipped to. done(ok) on the main loop once the frame is not read any more. False:
  /// not running, the task busy, or the format not shown - not taken.
  bool show_camera_frame_direct(const uint8_t *data, size_t length, CameraFrameFormat format, uint16_t width,
                                uint16_t height, std::function<void(bool)> &&done);
  /// A tap on the panel while the camera is full screen (main loop): its owner ends it.
  void add_on_camera_direct_tap_callback(std::function<void()> &&callback) {
    this->cam_direct_tap_callbacks_.add(std::move(callback));
  }
  /// Where in the target the last camera frame went (target pixels): what to redraw.
  void camera_frame_rect(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const {
    x = this->cam_job_data_.out_x;
    y = this->cam_job_data_.out_y;
    w = this->cam_job_data_.out_w;
    h = this->cam_job_data_.out_h;
  }
  void start_slideshow();
  void stop_slideshow();
  void pause_slideshow();
  void toggle_slideshow();
  SlideshowMode get_slideshow_mode() const { return this->slideshow_mode_; }
  size_t get_image_count() const { return this->images_.size(); }
  /// The folder's last listing failed (no pictures listed).
  bool picture_listing_failed() const { return this->pic_list_failed_; }
  int get_current_index() const { return this->current_image_; }
  const ImageEntry *get_current_image() const {
    return this->current_image_ >= 0 && static_cast<size_t>(this->current_image_) < this->images_.size()
               ? &this->images_[this->current_image_]
               : nullptr;
  }
  /// One of the configured picture directories.
  bool set_current_directory(size_t index);
  /// Any folder (full path, e.g. "/sd/photos").
  bool open_directory(const std::string &path);
  const std::string &get_current_directory() const { return this->pic_dir_; }
  size_t get_directory_count() const { return this->pic_dirs_.size(); }
  size_t get_current_directory_index() const { return this->pic_dir_index_; }
  /// Lists the current folder again.
  void refresh_images();
  /// After the canvas widget was resized: a buffer of its new size, the picture fitted again.
  void set_fullscreen(bool fullscreen);
  /// Before the canvas widget is resized: the picture fitted into width x height first; once it is
  /// decoded the canvas takes that size together with it (picture callbacks; is_fullscreen_shown()).
  void set_fullscreen(bool fullscreen, int32_t width, int32_t height);
  bool is_fullscreen() const { return this->fullscreen_; }
  /// Whether the canvas has the size of full screen yet (set_fullscreen() with a size).
  bool is_fullscreen_shown() const { return this->pic_shown_fullscreen_; }
  size_t get_thumbnail_count() const { return this->images_.size(); }
  /// RGB565 pixels of a loaded thumbnail (stride: width * 2 rounded as LVGL does), else nullptr.
  const uint8_t *get_thumbnail_data(size_t index, int &width, int &height);
  lv_obj_t *get_thumbnail_button(size_t index);
  /// Queues a thumbnail of the strip; true when it is loaded already.
  bool request_thumbnail(size_t index);
#endif
  /// A local AVI file plays or is paused, so seek() can work.
  bool can_seek() const;
  /// Position of the frame shown and length of the file, in seconds; 0 when unknown.
  float get_position() const;
  float get_duration() const;

  //========================================================================
  // State Query
  //========================================================================

  /// A Home Assistant track of the audio queue plays on the media player: its state then.
  PlayerState get_state() const;
  bool is_playing() const { return this->get_state() == PlayerState::PLAYING; }
  bool is_paused() const { return this->get_state() == PlayerState::PAUSED; }
  bool is_stopped() const { return this->get_state() == PlayerState::STOPPED; }
  PlaybackError get_last_error() const { return this->last_error_; }
  const std::string &get_current_file() const { return this->video_path_; }
  /// Whether what shows came over the network (an svp_relay AVI or udisp rectangles) rather than
  /// from a file: the sender controls it, and touches go to the sender.
  bool is_network_session() const {
#ifdef SVP_STREAM
    return this->stream_avi_.load(std::memory_order_acquire) || this->stream_connected_.load(std::memory_order_acquire);
#else
    return false;
#endif
  }

  //========================================================================
  // Automation Callbacks
  //========================================================================

  void add_on_started_callback(std::function<void()> &&callback) {
    this->on_started_callbacks_.add(std::move(callback));
  }
  void add_on_finished_callback(std::function<void()> &&callback) {
    this->on_finished_callbacks_.add(std::move(callback));
  }
  void add_on_paused_callback(std::function<void()> &&callback) { this->on_paused_callbacks_.add(std::move(callback)); }
  void add_on_error_callback(std::function<void(uint8_t)> &&callback) {
    this->on_error_callbacks_.add(std::move(callback));
  }
  /// Main loop: get_state() or the current file may have changed.
  void add_on_state_callback(std::function<void()> &&callback) { this->state_callbacks_.add(std::move(callback)); }
#ifdef SVP_CHANNEL_LIST
  /// Main loop: get_channel_names() changed.
  void add_on_channels_callback(std::function<void()> &&callback) { this->channels_callbacks_.add(std::move(callback)); }
#endif
#ifdef SVP_PICTURES
  /// Main loop: the picture shown, the picture list or the slideshow mode changed.
  void add_on_picture_callback(std::function<void()> &&callback) { this->picture_callbacks_.add(std::move(callback)); }
#endif

#ifdef SVP_CAPTURE
  // Screenshots and screencasts of what LVGL shows (screen_capture:), as LVGL shows it. The pictures
  // come from LVGL's own drawing (FrameCapture), the HW JPEG encoder makes the JPEGs; a screencast is
  // an MJPEG AVI with the microphone's PCM, into its folder and/or to svp_relay. Its buffers come from
  // the pool (the capture's row of the owner table). Not while a video, a stream (dashboard) or a
  // remote desktop has the panel: one of them starting ends the screencast. Main loop.
  enum CastState : uint8_t { CAST_IDLE, CAST_STARTING, CAST_RUNNING, CAST_ENDING };
  enum CastTarget : uint8_t { CAST_LOCAL = 1, CAST_RELAY = 2 };
  void set_screenshot_directory(const std::string &dir) { this->shot_dir_ = dir; }
  const std::string &get_screenshot_directory() const { return this->shot_dir_; }
  void set_screencast_directory(const std::string &dir) { this->cast_dir_ = dir; }
  const std::string &get_screencast_directory() const { return this->cast_dir_; }
  /// 1..100, for screenshots and screencasts.
  void set_capture_quality(uint8_t quality) {
    this->cap_quality_.store(std::clamp<uint8_t>(quality, 1, 100), std::memory_order_relaxed);
  }
  uint8_t get_capture_quality() const { return this->cap_quality_.load(std::memory_order_relaxed); }
  /// 1..30 pictures per second; taken at a screencast's start.
  void set_screencast_fps(uint8_t fps) { this->cast_fps_ = std::clamp<uint8_t>(fps, 1, 30); }
  uint8_t get_screencast_fps() const { return this->cast_fps_; }
  /// CastTarget bits; taken at a screencast's start.
  void set_screencast_target(uint8_t target) {
    if ((target & (CAST_LOCAL | CAST_RELAY)) != 0)
      this->cast_target_ = target & (CAST_LOCAL | CAST_RELAY);
  }
  uint8_t get_screencast_target() const { return this->cast_target_; }
  /// The microphone's sound in it (with microphone:); taken at a screencast's start.
  void set_screencast_sound(bool on) { this->cast_sound_ = on; }
  bool get_screencast_sound() const { return this->cast_sound_; }
#ifdef SVP_CAPTURE_MIC
  void set_capture_microphone(microphone::MicrophoneSource *mic) { this->cap_mic_ = mic; }
#endif
  /// A screenshot into the screenshot folder; false when it cannot be taken now (logged).
  bool screenshot();
  /// False when it cannot start now (logged, on_screencast_stopped with the reason).
  bool screencast_start();
  void screencast_stop();
  void screencast_toggle() {
    if (this->cast_state_ == CAST_IDLE) {
      this->screencast_start();
    } else {
      this->screencast_stop();
    }
  }
  bool camera_screenshot();
  bool camera_record_start(uint16_t width, uint16_t height);
  void camera_record_stop();
  bool is_camera_recording() const {
    return this->cap_camera_ && (this->cast_state_ == CAST_STARTING || this->cast_state_ == CAST_RUNNING);
  }
  void camera_capture_end();
  bool camera_capture_frame(const uint8_t *data, size_t length, CameraFrameFormat format, uint16_t width,
                            uint16_t height, std::function<void()> &&done);
  CastState screencast_state() const { return this->cast_state_; }
  bool screencast_running() const { return this->cast_state_ != CAST_IDLE; }
  /// The HW JPEG encoder engine (the screen capture's; created on first use, kept): for other components' encodes.
  /// jpeg_encoder_process() holds the codec's mutex for the whole encode, so callers on other tasks are serialized.
  jpeg_encoder_handle_t shared_jpeg_encoder();
  /// Whether the running screencast records the microphone (configured, sound on, the microphone started).
  bool screencast_mic_on() const {
#ifdef SVP_CAPTURE_MIC
    return this->cast_state_ == CAST_RUNNING && this->cap_mic_on_;
#else
    return false;
#endif
  }
  /// The screencast's file in the screencast folder ("" before its first one, or with target relay only).
  const std::string &get_screencast_path() const { return this->cast_path_; }
  /// The screenshot's file, "" when it failed.
  Trigger<std::string> *get_screenshot_trigger() { return &this->shot_trigger_; }
  Trigger<> *get_screencast_started_trigger() { return &this->cast_started_trigger_; }
  /// Why it ended: "" when asked to, else the reason (a notice).
  Trigger<std::string> *get_screencast_stopped_trigger() { return &this->cast_stopped_trigger_; }
  /// Main loop: screencast_state() changed.
  void add_on_capture_callback(std::function<void()> &&callback) { this->cap_callbacks_.add(std::move(callback)); }
#endif

 protected:
  //========================================================================
  // Playback Task (Core 1): open file, probe dimensions, allocate buffers, then loop:
  // read the next compressed frame (demux + audio feed) straight from BufferedFileReader,
  // HW-decode into A/B, pace, present.
  //========================================================================

  /// FreeRTOS task entry point (decode/playback task, pinned to Core 1)
  static void playback_task_entry_(void *param);

  /// Hands the session to the playback task. Only while no session runs (session_busy_()), under
  /// session_lock_: from run_pending_start_() and, for the network stream, the network task.
  void start_playback_(const std::string &video_path);
  /// A task of the last session still works (playback, reader, audio): nothing new starts before.
  bool session_busy_() const;
  /// session_busy_() of a session with a picture (a video): an audio file's takes no output.
  bool picture_session_busy_() const {
#ifdef SVP_AUDIO_FILES
    if (this->audio_session_.load(std::memory_order_acquire))
      return false;
#endif
    return this->session_busy_();
  }
  /// setup(): the playback, reader and audio tasks, made once; between sessions they block on their
  /// notification.
  bool create_tasks_();
  /// state_ changed: loop() runs the state callbacks, and whichever task waits on it looks again.
  void note_state_();
  /// Main loop: what play()/next()/... asked for, once the session before has fully ended. The
  /// main loop never waits for that: the ending session's file closes through the storage worker,
  /// whose completions only the main loop delivers.
  void run_pending_start_();
  /// The current session is told to end (play(), next() and the like start another after it).
  void end_session_();
  /// Main loop: the queue's Home Assistant track and what the media player plays (not an
  /// announcement) stop.
  void sound_take_over_();
  std::atomic<bool> sound_takeover_req_{false};  // sound_take_over_() asked for by another task
  // The app switch: the running app; another task's session start asks the main loop to end it
  AppKind app_{AppKind::NONE};
  std::atomic<bool> app_end_req_{false};
#ifdef SVP_EMULATOR
  friend class GameEmulator;
  GameEmulator *emu_{nullptr};
  bool emu_ready_{false};
  std::atomic<bool> emu_on_{false};
  void paused_input_default_();
  bool emu_refuses_() const;
#endif
  CallbackManager<void(AppKind)> app_end_callbacks_;
  /// Main loop: the running app ends (pictures close, the camera's owner is told).
  void app_end_();

  /// Main playback loop (runs in task)
  void playback_loop_();

  /// Reader/producer task entry point (demux + fill the compressed-frame pre-buffer, pinned to
  /// Core 0 -- never touches DMA2D, so it cannot race decode/PPA on Core 1, see esp-idf#18999).
  static void reader_task_entry_(void *param);
  void reader_loop_();


  /// Playback task: carries out a seek() -- holds the sound, has the reader jump, restarts the sound.
  void handle_seek_();
  /// Reader task: positions the file at a video frame and restarts the audio feed there.
  bool reader_seek_(uint32_t frame);
  /// Reader task: back to the first frame (loop).
  bool reader_rewind_();

  /// Main loop, no session running: the videos of path's folder (path is the playlist's one entry)
  /// put round it, listed by the storage worker (playlist_listed_()); synchronously without one.
  void load_folder_playlist_(const std::string &path);
  static bool playlist_entry_(const storage::FileStat *entry, void *ctx);
  void playlist_listed_(storage::StorageError err);
  // A local folder listed by the storage worker (async_list_dir) into a list's places past its
  // entries: the entry callback on the worker's task, the list taking them in (adopt()) on the main
  // loop at the completion. Nothing else writes those places meanwhile.
  struct FolderListing {
    SimpleVideoPlayer *player{nullptr};
    SvpId dir;
    size_t first{0};  // the first place written
    size_t count{0};
    uint32_t gen{0};
    bool active{false};
  };
  struct FolderListings {
    FolderListing pictures, playlist;
  };
  PsramPtr<FolderListings> listings_;  // a pool piece
  uint32_t playlist_gen_{0};           // bumped with every new playlist
  bool playlist_list_wanted_{false};   // the current playlist's folder, once the one on its way is done
  /// Main loop: playlist item `index` is to play (run_pending_start_()).
  void play_index_(size_t index);

  //========================================================================
  // Frame Processing
  //========================================================================

  /// Read the next JPEG frame into dest_buffer (capacity dest_capacity)
  /// Returns frame size or 0 if EOF, -1 on error
  int read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity);

  /// read_next_frame_() into dest (capacity cap), with loop rewind and a stop check.
  /// Returns payload size (> 0), 0 at EOF, -1 on read error, -2 if stopped/aborted.
  int read_frame_(uint8_t *dest, size_t cap);

  /// Decode JPEG frame (from the ring slot the decode task currently holds) and update canvas
  bool decode_frame_(const uint8_t *frame_data, size_t frame_size);
  /// Whether the current session draws onto the display (else into the canvas).
  bool direct_session_() const {
#ifdef SVP_DSI_OUTPUT
    return this->direct_.load(std::memory_order_relaxed);
#else
    return false;
#endif
  }

#ifdef SVP_DSI_OUTPUT
  /// setup(): read the panel size and allocate the decode buffer. false -> mark_failed.
  bool init_dsi_output_();
#endif
#ifdef SVP_STREAM
  bool setup_stream_();
  static void stream_net_task_entry_(void *param);
  void stream_net_loop_();
  static void stream_draw_task_entry_(void *param);
  void stream_draw_loop_();
  void stream_feed_(const uint8_t *data, size_t len);
  /// Whether a sender's picture is made for the display (else the canvas).
  bool stream_direct_() const {
#if defined(SVP_DSI_OUTPUT) && defined(SVP_CANVAS_OUTPUT)
    return this->stream_fullscreen_.load(std::memory_order_acquire) || !this->canvas_upright_();
#elif defined(SVP_DSI_OUTPUT)
    return true;
#else
    return false;
#endif
  }
#ifdef SVP_CANVAS_OUTPUT
  /// Canvas output: copy the part at src_x, src_y of one decoded rectangle (src_w wide) into the page
  /// buffer the canvas shows, at x, y.
  void stream_copy_to_canvas_(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t src_x, uint16_t src_y,
                              uint16_t src_w);
#endif
  struct StreamFrame;
  /// Draw task: a frame's picture into target as the output takes it (JPEG decoded, bitmap converted).
  bool stream_render_(StreamFrame *frame, jpeg_decode_cfg_t *cfg, uint8_t *target, size_t target_len);
  /// Wi-Fi roaming scans and power saving held off (want) or allowed again; counted, each released once.
  static void hold_wifi_(bool &held, bool want);
  void stream_send_pending_(int client);
#ifdef SVP_SPICE
  /// A SPICE session holds the panel: what would take it (a file, another output) is refused.
  bool spice_refuses_() const;
  friend class SpiceSession;  // spice_client.cpp: one session, over the stream's frames and outputs
  static void spice_task_entry_(void *param);
  void spice_loop_();
  // A desktop session's share of the panel (SPICE, VNC), spice task.
  /// The output the session draws on: the display, else the canvas, and its size.
  void desktop_output_(bool &direct, uint32_t &w, uint32_t &h);
  /// The panel to the session: a playing video ends, a stream sender is let go, no cursor yet; with
  /// sound (SPICE) the sound playing ends too (an audio file here, the queue's Home Assistant track
  /// and the media player's on the main loop). false: they did not let go in time.
  bool desktop_take_panel_(bool direct, bool &wifi_held, bool with_sound);
  /// Spice task: until the video (with sound: any session) and the stream sender let go, woken by
  /// spice_wake(); false past TAKEOVER_WAIT_MS or when a new request came.
  bool desktop_wait_free_(bool with_sound);
  /// Spice task, as the session takes the panel: the session's cursor buffers from the pool (only when
  /// they do not fit, other users' idle buffers come back: desktop_alloc()).
  bool desktop_buffers_take_();
  /// The desktop's buffers back to the pool while no session runs (media wants the room); any task.
  bool release_desktop_();
  SemaphoreHandle_t desktop_lock_{nullptr};  // its buffers: taken, or given back
  /// The panel back (panel_held: what desktop_take_panel_() took).
  void desktop_release_panel_(bool &panel_held, bool &wifi_held);
  /// The probe of spice_connect(): a VNC server greets first, a SPICE server waits for the client.
  enum class DesktopKind : uint8_t { UNREACHED, SPICE, VNC };
  DesktopKind desktop_probe_(const char *host, uint16_t port);
  /// The desktop's picture through svp_relay (whole JPEGs of the output's size, as its stream sender):
  /// asked for, its sender let in. connection_id: the SPICE session's. false: the relay refused it.
  bool desktop_relay_start_(const char *kind, const char *host, uint16_t port, const char *password,
                            uint32_t connection_id);
  void desktop_relay_stop_();
  /// A VNC session: all of it through the relay, the panel's keys, mouse and touches too. kind: "vnc",
  /// or "browser" (the relay's own VNC server, host and port unused).
  void desktop_vnc_(const char *kind, const char *host, uint16_t port, const char *password);
  struct SpiceCursorShape;
  /// Network task: the relay's VNC pointer shape shown, or (changed) where it is.
  void desktop_cursor_set_(const SpiceCursorShape *shape);
  void desktop_cursor_changed_();
  /// Leaving a session on the panel: the close key held, two fingers held.
  struct DesktopClose {
    enum Key : uint8_t { OTHER, TAKEN, TAPPED };
    bool key_held{false};
    uint16_t page{0}, usage{0};
    uint32_t key_at{0};
    bool fingers{false};
    uint32_t fingers_at{0};
    /// A key: TAKEN (the close key went down: nothing to send yet), TAPPED (it was let go before its
    /// time: press and release it on the desktop), OTHER (as it is).
    Key key(const SimpleVideoPlayer *p, uint8_t action, uint16_t page, uint16_t usage, bool pressed);
    /// Two or more fingers down (true the first time: the finger on the desktop lets go), or fewer.
    bool two_fingers(bool down);
    /// The ms until the session ends on the panel (UINT32_MAX: nothing held); 0: now.
    uint32_t due(const SimpleVideoPlayer *p) const;
  };
  /// Draw task, output held: the SPICE cursor taken off (what was under it back), or drawn where it is
  /// now (what is under it kept).
  void spice_cursor_hide_();
  void spice_cursor_show_();
  bool spice_cursor_over_(int32_t x, int32_t y, int32_t w, int32_t h) const;
  /// Draw task: pixels of the session's output, rows packed w * bpp.
  void spice_out_read_(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t *dst);
  void spice_out_write_(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *src);
  /// Draw task: the cursor drawn anew if it changed (holds the output for it).
  void spice_cursor_update_();
  /// Main loop: audio files and the media player stop where they share the session's sound device.
  void spice_sound_take_();
#endif
#ifdef SVP_STREAM_STATS
  /// Network task: starts, reports and stops the per-second figures; the ms until the next report.
  void stream_stats_step_(int client);
  uint32_t stream_stats_due_ms_() const;
#endif
  /// Tell the sender whether the panel shows its picture (false while a file plays).
  void stream_set_awake_(bool awake);
  /// The network task blocks in select() on its socket and on stream_wake_fd_ (an eventfd): whatever
  /// it has to act on wakes it here.
  void stream_wake_();
  static void stream_wake_cb_(void *arg) { static_cast<SimpleVideoPlayer *>(arg)->stream_wake_(); }
  int stream_wake_fd_{-1};
#ifdef SVP_CHANNEL_LIST
  enum : uint8_t {
    CHANNEL_CMD_REFRESH,
    CHANNEL_CMD_PLAY,
    CHANNEL_CMD_STOP,
    CHANNEL_CMD_HA_BROWSE,
    CHANNEL_CMD_HA_PLAY,
    CHANNEL_CMD_IMAGE,
    CHANNEL_CMD_OPTIONS_GET,
    CHANNEL_CMD_OPTIONS_SET,
    CHANNEL_CMD_COVER,
    CHANNEL_CMD_KEYBOARD_LIST,
    CHANNEL_CMD_KEYBOARD_GET,
    CHANNEL_CMD_CAST,  // index 1: svp_relay takes a screencast (POST /cast start), 0: it ends
  };
  struct ChannelCmd {
    uint8_t cmd;
    uint16_t index;
  };
  bool setup_channel_list_();
  bool channel_lists_(PsramPool *pool, size_t *bytes);
  bool channel_request_(uint8_t cmd, uint16_t index);
  void channel_loop_();
  void ha_loop_();
  static void channel_task_entry_(void *param);
  void channel_task_loop_();
  /// Blocking HTTP to the relay (channel task only). response: PSRAM buffer, caller frees. fetch: a
  /// fetch into a buffer of the main loop's (into), written only while it is still asked for.
  struct ChannelFetch;
  bool channel_http_(const char *path, const char *body, char **response, size_t *response_len,
                     uint8_t *into = nullptr, size_t into_cap = 0, uint32_t timeout_ms = 0,
                     ChannelFetch *fetch = nullptr, uint16_t gen = 0);
  // A fetch of the channel task into a pool buffer of the main loop's (an HA picture, cover art). The
  // main loop may want the buffer back meanwhile (a video starts): a fetch not writing yet is dropped
  // at once (it never writes), one writing stops after the block it reads. state: the request's
  // generation (never 0) << 3 | FETCH_*.
  enum : uint8_t { FETCH_IDLE, FETCH_QUEUED, FETCH_WRITING, FETCH_STOP, FETCH_DONE };
  struct ChannelFetch {
    std::atomic<uint32_t> state{0};
    std::atomic<uint8_t *> into{nullptr};  // set by the main loop before the request
    std::atomic<size_t> cap{0};
    uint16_t gen{0};  // main loop: the last asked for
    size_t len{0};    // channel task, read by the main loop at FETCH_DONE
    bool ok{false};
  };
  /// Main loop: asks the channel task for the fetch (false: the last one still stops, or the queue is full).
  bool fetch_start_(ChannelFetch &fetch, uint8_t cmd, uint8_t *into, size_t cap);
  /// Main loop: the fetch asked for last has ended (len, ok); it is idle again.
  bool fetch_done_(ChannelFetch &fetch);
  /// Main loop: true once the channel task no longer writes into the fetch's buffer (and never will
  /// for this request); false while it stops (asked to here), next loop again.
  bool fetch_drop_(ChannelFetch &fetch);
  /// Channel task: still asked for? Then its buffer.
  static bool fetch_begin_(ChannelFetch &fetch, uint16_t gen, uint8_t **into, size_t *cap);
  /// Channel task: the result, given to the main loop only while still asked for.
  static void fetch_end_(ChannelFetch &fetch, uint16_t gen, bool ok, size_t len);
  void ha_browse_task_();
  void ha_play_request_(const std::string &id);
#ifdef SVP_KEYBOARD
  void keyboard_list_task_();
  void keyboard_get_task_();
#endif
#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER)
  // The media player's state: the queue's Home Assistant track started, or ended (the queue goes on).
  void audio_remote_player_(media_player::MediaPlayerState state);
  // Its track asked to stop, pause or play on.
  bool audio_remote_command_(media_player::MediaPlayerCommand command);
#endif
  void image_task_(uint16_t gen);
  void options_get_task_();
  void options_set_task_();
  static bool remote_image_(const ImageEntry &entry);
#endif
#ifdef SVP_AUDIO_COVER
  // The cover art in each canvas, one after the other: the relay fits a JPEG into the canvas' size,
  // the picture task's hardware decoder writes it into the view's buffer (the pool's, while no video wants it), LVGL shows
  // it at its own size (centred). A canvas lets go of its buffer before it is written, and LVGL is
  // touched only while it runs.
  struct CoverView {
    lv_obj_t *canvas{nullptr};
    int32_t w{0}, h{0};  // the canvas' size
    uint8_t *data{nullptr};
    size_t capacity{0};
    lv_draw_buf_t buf{};
    int32_t shown_w{0}, shown_h{0};
    uint32_t stride{0};
  };
  static constexpr size_t COVER_VIEWS = 3;
  static constexpr size_t COVER_FILE_MAX = 512 * 1024;
  enum class CoverStep : uint8_t { IDLE, FETCHING, DECODING };
  CoverView cover_views_[COVER_VIEWS];
  size_t cover_view_count_{0};
  CoverStep cover_step_{CoverStep::IDLE};
  size_t cover_view_{0};       // position in cover_order_ of the view being fetched or decoded
  uint8_t cover_order_[COVER_VIEWS]{};  // fetch order: the view shown at the start first
  CoverView &cover_current_() { return this->cover_views_[this->cover_order_[this->cover_view_]]; }
  bool cover_wanted_{false};   // svp_cover: all views again
  bool cover_clear_{false};    // a local track: no cover art
  bool cover_shown_{false};
  bool cover_detach_pending_{false};  // given back while LVGL was paused: the canvases let go once it runs
  uint8_t *cover_file_{nullptr};
  // The channel task fetches into cover_file_ (cover_fetch_w_ x cover_fetch_h_; cover_fetch_), the
  // picture task decodes it (its flags, release / acquire).
  int32_t cover_fetch_w_{0}, cover_fetch_h_{0};
  size_t cover_len_{0};
  bool cover_fetch_ok_{false};
  ChannelFetch cover_fetch_;
  std::atomic<bool> cover_job_{false};
  std::atomic<bool> cover_decoded_{false};
  bool cover_decode_ok_{false};
  size_t cover_bytes_();
  size_t cover_reserve_{0};  // the views' buffers and the file: their share of the picture + audio mode
  uint8_t *cover_alloc_(size_t size);
  bool cover_buffers_();
  void release_cover_buffers_();
  void cover_loop_();
  void cover_task_(uint16_t gen);  // channel task
  void cover_decode_();   // picture task
  void cover_detach_(CoverView &view);
  void cover_clear_views_();
#endif
#ifdef USE_AUDIO
  /// PCM payload bytes from the sender (network task): converted to the speaker's channel count and
  /// played in 10 ms blocks; dropped while a file plays.
  void stream_on_audio_(const uint8_t *data, size_t len);
  /// The same, whatever plays: stream_audio_src_ch_ and stream_audio_rate_ say what it is.
  void stream_play_audio_(const uint8_t *data, size_t len);
  void stream_emit_audio_frames_(const uint8_t *src, size_t frames);
  void stream_flush_audio_block_();
#endif
#endif
  /// Create the ESP32-P4 hardware JPEG decoder engine, called once from setup().
  bool init_decoder_();
  /// Header-only parse (width/height, no pixel decode), used by get_video_dimensions_() for the
  /// raw-MJPEG case.
  bool parse_header_(const uint8_t *buffer, size_t size, uint32_t &width, uint32_t &height);

  /// Get video dimensions from first frame
  bool get_video_dimensions_(uint32_t &width, uint32_t &height);

#ifdef USE_AUDIO
  /// Initialize audio decoder for AVI audio stream
  bool init_audio_decoder_();

  /// Process audio frames from AVI
  void process_audio_frame_(const AVIFrame &frame, const uint8_t *data, size_t size);

  /// Audio processing task entry point (runs on Core 0)
  static void audio_task_entry_(void *param);

  /// Audio processing loop (decodes and converts channels)
  void audio_processing_loop_();

  /// Stop audio processing task
  void stop_audio_task_();
#endif

  //========================================================================
  // File I/O Abstraction (supports local and network storage)
  //========================================================================

  /// Detect video file format (AVI vs raw MJPEG)
  VideoFormat detect_format_();

  /// Open file (local FatFS or network storage)
  bool open_file_(const std::string &path);

  /// Close file
  void close_file_();
#ifdef SVP_STREAM
  /// open_file_() for the network stream: an AVI pushed into file_reader_ by the network task.
  bool open_stream_source_();
  void prefill_stream_();
#endif

  /// Read data from file (handles both local and network)
  int read_data_(uint8_t *buffer, size_t size);

  /// Seek to position in file
  bool seek_to_(uint64_t position);

  /// Get file size
  bool get_file_size_(uint64_t &size);

  //========================================================================
  // Buffer Management
  //========================================================================

  /// Allocate all buffers (cache, input, output)
  bool allocate_buffers_(uint32_t video_width, uint32_t video_height);

  /// Free all buffers
  void free_buffers_();

  // The PSRAM pool (psram_pool_size) holds every buffer of the player. setup() shares it out once
  // (plan_pool_()): the stream frames, sound buffers and thumbnails stay at its start for good; a
  // video session takes its frame slot, outputs, controls and index, and the read-ahead ring as much
  // as the prebuffer and the sound's lead need. Session ("media") buffers are kept after a video; the
  // pictures' are kept after a picture. Only when one side's request does not fit are the other
  // side's idle buffers handed back (the owner table below).
  bool plan_pool_();
  // The owner table of the pool's buffers taken on demand (the plan's fixed ones aside). A user takes
  // from the pool; only when a piece does not fit are other users' idle buffers handed back, one
  // group after the other in its row's order, and only while it still does not fit.
  enum : uint8_t {
    POOL_PICTURES = 1 << 0,  // the pictures' pages, files, decode buffer and the cover art's (main loop)
    POOL_VIDEO = 1 << 1,     // read-ahead ring, compressed-frame slot, index (main loop, no video session)
    POOL_DIRECT = 1 << 2,    // the display's decode buffer and the controls (main loop)
    POOL_CANVAS = 1 << 3,    // the canvas output pair (main loop)
    POOL_DESKTOP = 1 << 4,   // the desktop's cursors and USB transfers (release_desktop_(), no session)
    // The screencast's or a screenshot's pictures and JPEGs (main loop: the screencast ends, they go
    // once nothing uses them; its index and sound ring follow when its file is complete).
    POOL_CAPTURE = 1 << 5,
  };
  enum : uint8_t {
    POOL_USER_VIDEO,          // playback task: ring, sound block, frame slot, index
    POOL_USER_DIRECT,         // playback task: the display's decode buffer
    POOL_USER_CANVAS,         // playback task: the canvas pair
    POOL_USER_STREAM_DIRECT,  // stream draw task (dashboard, desktop picture): the display's decode buffer
    POOL_USER_STREAM_CANVAS,  // stream draw task: the canvas pair
    POOL_USER_PICTURES,       // main loop: pictures and cover art
    POOL_USER_DESKTOP,        // spice task: cursors, USB transfers
    POOL_USER_CAPTURE,        // main loop: screenshots and the screencast
    POOL_USER_GAME,
    POOL_USERS,
  };
  static constexpr size_t POOL_STEPS = 5;
  static constexpr uint8_t POOL_OWNERS[POOL_USERS][POOL_STEPS] = {
      {POOL_CAPTURE, POOL_PICTURES, POOL_DESKTOP, 0, 0},
      {POOL_CAPTURE, POOL_CANVAS, POOL_PICTURES, POOL_DESKTOP, 0},
      {POOL_CAPTURE, POOL_DIRECT, POOL_PICTURES, POOL_DESKTOP, 0},
      {POOL_CAPTURE, POOL_CANVAS, POOL_PICTURES, POOL_VIDEO, POOL_DESKTOP},
      {POOL_CAPTURE, POOL_DIRECT, POOL_PICTURES, POOL_VIDEO, POOL_DESKTOP},
      {POOL_DIRECT | POOL_CANVAS, POOL_VIDEO, POOL_DESKTOP, 0, 0},
      {POOL_CAPTURE, POOL_PICTURES, POOL_VIDEO, 0, 0},
      // What LVGL shows (pictures among it) stays; a video, a stream or a desktop excludes it.
      {POOL_VIDEO, POOL_DIRECT | POOL_CANVAS, 0, 0, 0},
      {POOL_DIRECT | POOL_CANVAS, POOL_VIDEO, POOL_DESKTOP, 0, 0},
  };
  /// A piece for this user: from the pool, else after its row's groups were handed back step by step.
  uint8_t *pool_take_(size_t size, uint8_t user);
  /// One step of the user's row: true when it was answered (worth trying the pool again).
  bool pool_give_back_(uint8_t groups, uint8_t user);
  /// Any task but the main loop: asks the main loop for the groups, waits at most 2 s; not answered by
  /// then, the request is taken back (the main loop frees nothing for nobody).
  bool pool_request_(uint8_t groups, uint8_t user);
  /// Main loop: a waiting request for one of the groups; answered (the requester woken).
  bool pool_wanted_(uint8_t groups) const;
  void pool_answer_(uint8_t groups);
  /// Main loop: the requests for the outputs and the video's buffers, each as soon as it can go.
  void pool_service_();
  /// Main loop: no video session uses the video's buffers (also none starting).
  bool video_session_() const;
  /// Main loop, the media lock held: the groups' outputs / the video's buffers back to the pool.
  bool release_outputs_(uint8_t groups);
  bool release_video_buffers_();
  /// Main loop, for pictures and cover art: a step of their row, when nothing plays or streams.
  bool give_back_media_(uint8_t groups);
  /// From any task but the main loop: a piece for the video (the owner table's video row).
  uint8_t *media_alloc_(size_t size);
  /// Playback task: the read-ahead ring, the sound block and the compressed-frame slot of a session
  /// (the index is taken in open_file_(), only for a file's AVI). Ring and sound block are taken
  /// anew only when the size this session needs differs from the kept one's.
  bool acquire_session_buffers_();
  /// One try from the pool alone; what it took goes back when one does not fit.
  bool take_session_buffers_(uint32_t lead_ms, size_t ring_len);
  /// The display's decode buffer (and the controls) / the canvas output pair, kept once taken. Only
  /// when it does not fit, the owner table's row hands back (the other output first).
  bool ensure_direct_target_(bool with_target);
  bool ensure_canvas_outputs_();
  uint8_t *output_alloc_(size_t size, bool direct);
  size_t canvas_output_size_() const;

  //========================================================================
  // Error Handling
  //========================================================================

  void set_error_(PlaybackError error);

  //========================================================================
  // Members
  //========================================================================

  // Configuration
#ifdef SVP_USE_LVGL
  lvgl::LvglComponent *lvgl_component_{nullptr};
  lv_obj_t *canvas_{nullptr};
  uint16_t canvas_size_w_{0}, canvas_size_h_{0};
  lv_draw_buf_t canvas_draw_buf_{};  // the shown output buffer: the video's own size, the decoder's rows
#endif
  uint32_t psram_pool_size_{12 * 1024 * 1024};
  PsramPool pool_;
  SemaphoreHandle_t media_lock_{nullptr};      // taking or giving back media buffers
  uint8_t *ring_buf_{nullptr};                 // the read-ahead ring (pool)
  uint32_t *index_buf_{nullptr};               // a file's index (pool), kept with the ring
  // The shares of the pool (plan_pool_()).
  size_t ring_room_{0};         // what is left for the read-ahead ring and the sound block together
  size_t ring_size_{0};         // the read-ahead ring taken: prebuffer + the session's lead
  size_t frame_bytes_{0};       // compressed-frame slot: the largest frame that plays
  size_t output_reserve_{0};    // the larger of the display's decode buffer and the canvas pair
  size_t controls_reserve_{0};  // both controls pictures
  size_t index_reserve_{0};     // a file's index
  // The most a video brings per second (picture and sound) that the ring is sized for: MJPEG at
  // the panel's size and 25 fps with ~150 KiB frames, plus PCM.
  static constexpr size_t MAX_MEDIA_RATE = 4 * 1024 * 1024;
  static constexpr uint32_t LATENCY_STEP_MS = 10;
  std::atomic<uint32_t> prebuffer_ms_{500};
  // The most the sound runs ahead of the picture (audio_latency, e.g. a Bluetooth device's delay)
  // with this prebuffer: from the room (latency_limit_ms_()).
  uint32_t max_audio_latency_ms_{0};
  /// The read-ahead ring for this much ahead (whole fill chunks, at least two).
  static size_t ring_bytes_(uint32_t ms);
  /// The sound block for this lead (the larger ring kind the configuration uses); 0 without sound.
  size_t sound_bytes_(uint32_t lead_ms) const;
  uint32_t latency_limit_ms_(uint32_t prebuffer_ms) const;
  // From the pool (PSRAM): raw MJPEG reads, index and audio-chunk scratch.
  static constexpr size_t CACHE_BUFFER_SIZE = 64 * 1024;
  float target_fps_{30.0f};  // Target frame rate

#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};  // Optional speaker for audio playback
  SpeakerChannelMode speaker_channel_mode_{SpeakerChannelMode::SPEAKER_CHANNEL_MONO};  // Channel routing mode
#endif

  // Playback state. std::atomic because the playback task (Core 1) reads it in its loop while
  // play()/pause()/resume()/stop()/set_error_() write it from the main-loop core -- a plain field
  // could be hoisted out of the loop by the compiler and carries no cross-core visibility
  // guarantee. Transitions use compare_exchange/exchange; no mutex.
  std::atomic<PlayerState> state_{PlayerState::STOPPED};
  // Atomic for the same cross-core reason; written just before state_ becomes ERROR so a reader
  // that sees ERROR also sees the reason.
  std::atomic<PlaybackError> last_error_{PlaybackError::NONE};
  std::atomic<bool> loop_{false};  // main loop writes (set_repeat), playback task reads
  std::string video_path_;  // written in play() before the task starts, then read only by the task

  // File reader (backed by storage::StorageWorker -- handles local/network storage
  // transparently, see buffered_file_reader.h)
  PsramPtr<BufferedFileReader> file_reader_;
  uint64_t file_size_{0};

  // Video format and container parser
  VideoFormat video_format_{VideoFormat::UNKNOWN};
  PsramPtr<AVIParser> avi_parser_;  // AVI container parser (if AVI format)

#ifdef USE_AUDIO
  // This player commits to ONE fixed audio format for every video. sample_rate/channels/
  // bits_per_sample are resolved from the referenced speaker's own config (a hardware property,
  // not asked of the user twice -- see __init__.py's _resolve_speaker_audio_format()); audio_codec
  // is the one piece that's actually a user-facing YAML option, since it's a property of the
  // video file, not the speaker. All four reach here as SVP_AUDIO_* defines. That's what makes it
  // possible to size the audio ring
  // buffers/temp buffer once and allocate them once, in setup(), like every other persistent
  // buffer in this component -- instead of recomputing sizes from whatever a given AVI file's
  // audio stream header happens to say and reallocating per play() (AGENTS.md: no heap
  // allocation after setup()). Per-file auto-detected format info is still read at open time
  // (init_audio_decoder_()), but only to VALIDATE it matches this fixed format -- a mismatch is
  // a hard error (no audio for that file), never a reason to resize anything.
#if defined(SVP_AUDIO_SAMPLE_RATE)
  static constexpr uint32_t AUDIO_SAMPLE_RATE = SVP_AUDIO_SAMPLE_RATE;
  static constexpr uint8_t AUDIO_SOURCE_CHANNELS = SVP_AUDIO_SOURCE_CHANNELS;
  static constexpr uint8_t AUDIO_BITS_PER_SAMPLE = SVP_AUDIO_BITS_PER_SAMPLE;
  static constexpr size_t AUDIO_BYTES_PER_FRAME =
      static_cast<size_t>(AUDIO_SOURCE_CHANNELS) * (AUDIO_BITS_PER_SAMPLE / 8);
  static constexpr size_t AUDIO_BYTES_PER_SEC = static_cast<size_t>(AUDIO_SAMPLE_RATE) * AUDIO_BYTES_PER_FRAME;
  // Both sound rings hold what the sound runs ahead of the picture plus room (sound_block_for_()).
  static size_t audio_input_bytes(uint32_t latency_ms) {
    return std::max<size_t>(AUDIO_BYTES_PER_SEC * (latency_ms + 250) / 1000, 32 * 1024);
  }
  static size_t audio_decoded_bytes(uint32_t latency_ms) {
    return std::max<size_t>(AUDIO_BYTES_PER_SEC * std::max<uint32_t>(latency_ms + 250, 500) / 1000, 16 * 1024);
  }
  // Only the sound buffers the configuration uses: the input ring for compressed sound (audio_codec
  // mp3/flac, audio files), the decoded ring for PCM in a video; the temp buffer for both PCM and files.
#if defined(SVP_AUDIO_SUPPORT_MP3) || defined(SVP_AUDIO_SUPPORT_FLAC) || defined(SVP_AUDIO_FILES)
  static constexpr bool AUDIO_INPUT_RING = true;
#else
  static constexpr bool AUDIO_INPUT_RING = false;
#endif
#ifdef SVP_AUDIO_SUPPORT_PCM
  static constexpr bool AUDIO_DECODED_RING = true;
#else
  static constexpr bool AUDIO_DECODED_RING = false;
#endif
#if defined(SVP_AUDIO_SUPPORT_PCM) || defined(SVP_AUDIO_FILES)
  static constexpr bool AUDIO_TEMP_BUFFER = true;
#else
  static constexpr bool AUDIO_TEMP_BUFFER = false;
#endif
  static constexpr size_t AUDIO_TEMP_BUFFER_SIZE = (AUDIO_BYTES_PER_SEC * 100 / 1000) > (8 * 1024)
                                                       ? (AUDIO_BYTES_PER_SEC * 100 / 1000)
                                                       : (8 * 1024);
  static constexpr size_t AUDIO_DECODER_INPUT_BUFFER_SIZE = AUDIO_SAMPLE_RATE > 48000 ? (96 * 1024) : (64 * 1024);
  static constexpr size_t AUDIO_DECODER_OUTPUT_BUFFER_SIZE = AUDIO_SAMPLE_RATE > 48000 ? (48 * 1024) : (32 * 1024);
#endif

  // Audio decoding (for AVI with audio streams). audio_decoder_/audio_input_ring_buffer_/
  // audio_decoded_ring_buffer_/audio_temp_buffer_ are ALL allocated ONCE in setup() (sized from
  // the AUDIO_* constants above, only when the fixed codec is MP3/FLAC for audio_decoder_ itself
  // -- PCM mode never uses it) and reused for every play() -- only reset() (ring buffers) or
  // re-add_source()/add_sink()/start()'d (audio_decoder_) between sessions, never freed/recreated.
  // Verified against the real audio component source: AudioDecoder::start() already resets its
  // own per-file state (potentially_failed_count_, end_of_file_, a fresh per-codec sub-decoder)
  // on every call, so calling it again on a persistent instance is exactly what it's for.
  PsramPtr<audio::AudioDecoder> audio_decoder_;                       // Audio decoder (MP3/FLAC/PCM)
  std::shared_ptr<ring_buffer::RingBuffer> audio_input_ring_buffer_;  // Ring buffer for encoded audio (in PSRAM)
  std::shared_ptr<ring_buffer::RingBuffer>
      audio_decoded_ring_buffer_;                 // Ring buffer for decoded audio (in PSRAM, before conversion)
  uint8_t *audio_temp_buffer_{nullptr};           // Temporary buffer for audio processing (pool)
  // One block for the sound ring (pool), as large as the larger kind the configuration uses for the
  // lead it was taken for (setup(): none; a video: its lead, taken anew only when that size changes).
  // The ring over it is the kind the file needs: the input ring (compressed) or the decoded ring
  // (PCM), the other one is null. Made anew over the block when the kind or the block changes.
  uint8_t *sound_block_{nullptr};
  size_t sound_block_len_{0};
  uint32_t sound_lead_ms_{0};  // the lead the block and its ring are sized for
  size_t sound_ring_len_{0};   // the ring made over it
  /// From the pool alone (the session start hands back when it does not fit); taken: newly taken.
  bool sound_block_for_(uint32_t lead_ms, bool *taken);
  bool sound_ring_for_(bool compressed);
  uint8_t source_audio_channels_{0};              // Fixed source channel count (mirrors AUDIO_SOURCE_CHANNELS)
  uint8_t speaker_audio_channels_{1};             // Number of channels speaker expects
  uint32_t audio_sample_rate_{0};                 // Fixed sample rate (mirrors AUDIO_SAMPLE_RATE)
  uint8_t audio_bits_per_sample_{16};             // Fixed bits per sample (mirrors AUDIO_BITS_PER_SAMPLE)
  size_t audio_frame_bytes_{0};                   // This file's PCM frame
  // The rate and channel count sound is wanted in, reported to senders: the speaker's, or with an
  // audio router its selected output's (they change with the selection).
  std::atomic<uint32_t> wanted_audio_rate_{0};
  std::atomic<uint8_t> wanted_audio_channels_{0};
  // The rates taken as they are: the speaker's one, or the selected router output's range.
  std::atomic<uint32_t> wanted_audio_min_rate_{0};
  std::atomic<uint32_t> wanted_audio_max_rate_{0};
#ifdef SVP_AUDIO_ROUTER
  resampler::ResamplerSpeaker *audio_router_{nullptr};
  void on_audio_output_changed_();
#endif
  std::atomic<bool> audio_enabled_{false};        // Audio stream available and enabled (playback, reader, audio tasks)
  // Whether THIS file's audio track actually uses audio_decoder_ (compressed: MP3/FLAC) vs direct
  // PCM. In a multi-codec build audio_decoder_ exists regardless, so the object pointer can no
  // longer signal the per-file mode -- this flag does. Set in init_audio_decoder_() per play().
  std::atomic<bool> audio_use_decoder_{false};
  TaskHandle_t audio_task_handle_{nullptr};       // Core 0, made once in setup()
  std::atomic<bool> audio_busy_{false};           // a session's sound runs on it
  void end_session_audio_();
  void speaker_pause_(bool paused);
#ifdef SVP_AUDIO_FILES
  speaker::Speaker *file_speaker_{nullptr};
  // Its own reader and ring (setup()), apart from the video's whose memory the pictures may take.
  PsramPtr<BufferedFileReader> audio_file_reader_;
  static constexpr size_t AUDIO_FILE_RING_SIZE = 128 * 1024;
  // Main loop only. audio_order_ is the queue's order of play (shuffled or as added), audio_pos_
  // the place in it of the track that plays or played last.
  PoolList<SvpId> audio_queue_;
  PoolList<SvpTitle> audio_titles_;  // per track; empty: its file's name
  PoolList<uint16_t> audio_order_;
  size_t audio_pos_{0};
  uint32_t audio_queue_gen_{0};
  bool audio_current_{false};  // audio_pos_ names a track: not before the first and after the last
  bool audio_shuffle_{false};
  RepeatMode audio_repeat_{RepeatMode::OFF};
  uint32_t audio_track_started_ms_{0};
  std::atomic<bool> audio_session_{false};
  std::atomic<bool> audio_start_pending_{false};
  std::atomic<bool> audio_advance_req_{false};  // the track played to its end
  // A Home Assistant track of the queue: asked of svp_relay, then the media player plays it (its
  // svp_play_audio came); svp_relay's own sessions (TV, dashboard, desktop) run beside it.
  static constexpr uint8_t AUDIO_REMOTE_NONE = 0, AUDIO_REMOTE_ASKED = 1, AUDIO_REMOTE_PLAYING = 2;
  std::atomic<uint8_t> audio_remote_{AUDIO_REMOTE_NONE};
  uint32_t audio_remote_play_ms_{0};  // when its svp_play_audio came: an idle player before is the last track's
  // The queue plays (a track, or between two): not stopped and not past its end. In the status
  // (svp_relay: the panel is not idle then); main loop.
  bool audio_halted_{false};
  bool audio_playlist_shown_{false};
  bool audio_playlist_active_() const { return this->audio_current_ && !this->audio_halted_; }
  std::atomic<bool> audio_file_eof_{false};     // the reader has handed on the whole file
  std::atomic<bool> audio_changed_{false};
  // The file speaker stays open from one track to the next of the same format; audio task while a
  // session runs, else the main loop.
  bool file_speaker_open_{false};
  audio::AudioStreamInfo file_speaker_info_;
  CallbackManager<void()> audio_callbacks_;
  void audio_order_build_(size_t first);
  void audio_start_(size_t pos);
  void audio_advance_();
  void start_audio_session_();
  void audio_file_feed_();  // reader task
  void audio_file_loop_();  // audio task
  // A track's length and a seek in it: the reader task reads the file's layout as it opens it
  // (audio_file_prepare_()); position and duration in milliseconds through duration_frames_ /
  // position_frame_ (frame_duration_us_ 1000 for an audio file). A seek starts the track anew from
  // that place (the decoder fresh, its header fed first where the format has one).
  struct AudioFileLayout {
    uint64_t size{0};
    uint64_t data_start{0};  // the first byte of audio, after tags and headers
    uint64_t data_end{0};
    uint32_t duration_ms{0};  // 0: not known, no seeking
    // MP3: the Xing/Info header's
    uint32_t xing_bytes{0};
    bool toc{false};
    uint8_t xing_toc[100]{};
    // FLAC
    uint8_t streaminfo[34]{};
    uint32_t flac_rate{0};
    uint16_t flac_block{0};  // the fixed block size (min == max), 0 variable
    // WAV
    uint8_t fmt[40]{};
    uint8_t fmt_len{0};
    uint32_t byte_rate{0};
    uint16_t block_align{0};
  };
  AudioFileLayout audio_layout_;                     // reader task
  std::atomic<uint32_t> audio_seek_ms_{0};           // where the track's next start begins (a seek)
  std::atomic<uint32_t> audio_position_base_ms_{0};  // the position the decoder started at
  bool audio_seek_paused_{false};                    // main loop: the seek came while paused
  bool audio_restart_{false};                        // main loop: this start is a seek's (no started event)
  bool audio_started_pending_{false};                // main loop: the track's started event, from loop()
  void audio_seek_(uint32_t ms);
  /// Reader task, the file open: its layout; where to feed from for `seek_ms` (`from`), the header
  /// the decoder takes first there (into `head`, its length returned) and the position it starts at.
  size_t audio_file_prepare_(BufferedFileReader *reader, uint32_t seek_ms, uint8_t *head, uint64_t *from,
                             uint32_t *base_ms);
#endif
  bool session_audio_{false};  // this session started the audio task and the speaker
  // PCM bytes of incomplete audio frames dropped (reader side), logged once at the session's end.
  std::atomic<uint32_t> audio_dropped_bytes_{0};
  volatile bool audio_task_stop_{false};          // Signal to stop audio task
#endif

  // Cache buffer state (for frame parsing - only used for raw MJPEG)
  size_t cache_buffer_valid_{0};   // Valid bytes in cache
  size_t cache_buffer_offset_{0};  // Read offset within cache

  // Video properties
  uint32_t video_width_{0};
  uint32_t video_height_{0};

  // Buffers -- allocated ONCE in setup(), sized for the max resolution, reused every play().
  uint8_t *cache_buffer_{nullptr};  // pool: raw MJPEG reads, index and sound-cursor scratch
  // Decoded RGB888 double-buffer for the LVGL-canvas path (PSRAM). The P4 HW JPEG decoder
  // DMA2D-writes the next frame into the back buffer while LVGL's PPA reads the front buffer to
  // blit/rotate it, so neither side ever sees a half-written frame -- the former single buffer tore
  // because decode could be tick-preempted mid-write while LVGL blitted it. BOTH buffers come from
  // the PSRAM pool (whole cache lines, as the decoder and LVGL's PPA are both DMA engines), taken
  // with the first video on the canvas and kept.
  uint8_t *output_buffers_[2]{};
  size_t output_buffer_size_{0};
  uint8_t decode_buf_idx_{0};  // which output buffer decode_frame_() writes next (playback-task-local)

  // Published by decode_frame_() (video task, Core 1) once a frame is complete; consumed by loop()
  // (LVGL thread, Core 1) which points the canvas at it and invalidates. All lv_canvas_* calls
  // happen on the LVGL thread only -- never from the video task (LVGL is not thread-safe).
  std::atomic<uint8_t *> display_buffer_{nullptr};
  uint32_t canvas_w_{0};  // the picture's own size, set once per session
  uint32_t canvas_h_{0};
  std::atomic<uint32_t> canvas_stride_{0};  // bytes per row of the published buffer

  // Set by decode_frame_() after a frame is decoded; consumed by loop() on the LVGL thread.
  std::atomic<bool> frame_ready_{false};

  // Start handshake: the playback task sets 1 once preload is done; loop() (main thread) pauses LVGL
  // for DSI output, fires on_playback_started and sets 2. Nothing is presented or played before that.
  std::atomic<uint8_t> start_req_{0};
  std::atomic<bool> av_started_{false};

  // Audio latency compensation. The reader hands the audio on audio_lead_frames_ video frames ahead
  // of the picture: an audio cursor walks the read-ahead ring with peek() and feeds the audio chunks,
  // the frame read skips the ones it fed. At the start the sound is released audio_lead_us_ before
  // the first frame is shown.
  TemplatableFn<uint32_t> audio_latency_{};
  TemplatableFn<uint32_t> audio_preroll_{};
  uint32_t audio_preroll_us_{0};
  uint32_t audio_lead_us_{0};      // this session's latency, read when playback starts
  uint32_t audio_lead_frames_{0};  // the same in video frames
  uint64_t audio_cursor_pos_{0};   // reader position of the next chunk the cursor reads (reader task)
  std::atomic<uint32_t> audio_cursor_frames_{0};  // video chunks the cursor has passed
  std::atomic<uint32_t> reader_frames_{0};        // video frames the reader has handed on
  std::atomic<bool> audio_hold_{false};           // the audio task stops feeding (seek)
  std::atomic<bool> audio_held_{false};           // set by the audio task once it does
#ifdef USE_AUDIO
  audio::AudioFileType audio_file_type_{audio::AudioFileType::NONE};
  void restart_audio_feed_();
  void advance_audio_cursor_();
  void reader_audio_chunk_(const AVIFrame &frame, const uint8_t *data, size_t size);
  void reader_passed_video_();
  void reset_audio_cursor_();
  std::atomic<uint32_t> sound_written_{0};
  std::atomic<uint32_t> sound_played_{0};
  uint32_t sound_base_{0};
  void sound_preroll_(TickType_t wait);
#endif
  uint32_t avi_fps_num_{0};
  uint32_t avi_fps_den_{0};

  std::atomic<bool> playback_parked_{false};  // the playback task sits in its pause wait
#ifdef SVP_CONTROLS
  // A piece of the rendered controls, in the panel's orientation; area: where it is in LVGL's.
  struct ControlsSprite {
    lv_area_t area{};
    int32_t pw{0}, ph{0};
    uint8_t *data{nullptr};
  };
  struct ControlsBar;
  struct ControlsTarget {  // what a button's LVGL event acts on
    SimpleVideoPlayer *player;
    ControlsBar *bar;
    uint8_t button;
  };
  struct ControlsBar {
    bool enabled{false};
    bool picture{false};
    bool audio{false};
    PsramVector<uint8_t> buttons;
    bool title{true}, time{true}, seek_bar{true};
    Trigger<> exit_trigger;
    ControlsTarget target[CTL_BUTTON_COUNT]{};
    uint32_t shown_ms{0};
    int shown_image{-2};
    int shown_count{-1};
    int shown_playing{-1};
    uint32_t shown_slide{0};  // the slideshow interval its slide_time shows
    char slide_text[12]{};     // its text (LVGL keeps the pointer)
    int shown_position{-1};
    uint32_t shown_gen{UINT32_MAX};
    bool visible{false};  // LVGL shows it (main loop's own note: nothing read of LVGL while paused)
    bool dragging{false};  // its seek bar (an LVGL bar's)
    uint16_t drag_permille{0};
    ControlsTarget seek_target{};
    lv_obj_t *bar{nullptr};
    lv_obj_t *title_label{nullptr};
    lv_obj_t *position_label{nullptr};
    lv_obj_t *duration_label{nullptr};
    lv_obj_t *seek{nullptr};
    lv_obj_t *button[CTL_BUTTON_COUNT]{};
    lv_obj_t *button_label[CTL_BUTTON_COUNT]{};
  };
  static constexpr size_t CTL_GLYPHS = 11;  // 0-9 and ':'
  ControlsBar ctl_video_;
#ifdef SVP_AUDIO_CONTROLS
  ControlsBar ctl_audio_;
  bool audio_fullscreen_{false};
  void audio_controls_loop_();
  void show_audio_controls_();
  void hide_audio_controls_();
  void update_audio_controls_(bool force);
  static void audio_seek_event_(lv_event_t *event);
#endif
#ifdef SVP_PICTURES
  ControlsBar ctl_picture_;
  void picture_controls_loop_();
  void show_picture_controls_();
  void hide_picture_controls_();
  void update_picture_controls_(bool force);
  /// A tap on the picture in full screen: the controls shown, or hidden again.
  void picture_controls_tap_();
  void picture_controls_touched_();
#endif
  static void controls_button_event_(lv_event_t *event);
  lv_style_t *ctl_user_bar_style_{nullptr};
  lv_style_t *ctl_user_button_style_{nullptr};
  lv_style_t *ctl_user_accent_style_{nullptr};
  lv_style_t *ctl_user_seek_style_{nullptr};
  lv_style_t *ctl_user_indicator_style_{nullptr};
  lv_style_t *ctl_user_focus_style_{nullptr};
  lv_style_t ctl_bar_style_{};
  lv_style_t ctl_row_style_{};
  lv_style_t ctl_button_style_{};
  lv_style_t ctl_accent_style_{};
  lv_style_t ctl_focus_style_{};  // the button keys are on
  lv_style_t ctl_seek_style_{};
  lv_style_t ctl_indicator_style_{};
  touchscreen::Touchscreen *ctl_touchscreen_{nullptr};
  ControlsTouchListener ctl_listener_{this};
  uint32_t ctl_hide_after_ms_{5000};
  // Pool memory of a direct session (controls_reserve_): the snapshot, then the sprites in it.
  uint8_t *ctl_mem_{nullptr};
  size_t ctl_mem_used_{0};
  size_t ctl_snap_bytes_{0};  // a snapshot of the bar
  display::DisplayRotation ctl_rotation_{display::DISPLAY_ROTATION_0_DEGREES};
  ControlsSprite ctl_static_;       // the bar: paused icon, repeat off, empty seek bar and time
  ControlsSprite ctl_play_, ctl_pause_;
  ControlsSprite ctl_repeat_sprite_[3];  // RepeatMode
  ControlsSprite ctl_progress_;          // the seek bar full
  ControlsSprite ctl_glyph_[CTL_GLYPHS];
  lv_area_t ctl_area_{};           // the bar (LVGL coordinates)
  lv_area_t ctl_seek_area_{};
  lv_area_t ctl_time_area_{};      // the position label's text
  lv_area_t ctl_button_area_[CTL_BUTTON_COUNT]{};
  int32_t ctl_cell_w_{0};          // a digit, as wide as the widest
  // Title and duration change per file: written into ctl_static_ without LVGL's widgets, straight
  // from the label's font (glyph descriptors and bitmaps: font data only), over the empty field's
  // background kept beside it; so the next file of a playlist needs no LVGL while it is paused.
  struct ControlsText {
    lv_area_t area{};  // the label's text (LVGL coordinates)
    const lv_font_t *font{nullptr};
    lv_color_t color{};
    int32_t letter_space{0};
    ControlsSprite background;  // the field without text
  };
  ControlsText ctl_title_text_, ctl_duration_text_;
  lv_draw_buf_t ctl_glyph_buf_{};  // a glyph's coverage (A8), over the snapshot's memory
  bool ctl_prerendered_{false};    // the sprites stand, ctl_mem_ held since
  /// The file's title: its metadata's (AVI INFO INAM) when it has one, else its name.
  const char *video_title_() const;
  /// Main loop, no widget touched: the background back, then the text over it.
  void ctl_draw_texts_();
  void ctl_draw_text_(ControlsText &field, const char *text, bool dots);
  uint8_t *ctl_static_px_(int32_t x, int32_t y) const;
  std::atomic<bool> ctl_ready_{false};  // rendered for this session
  std::atomic<bool> ctl_shown_{false};  // copied into each frame
  std::atomic<bool> ctl_dragging_{false};
  std::atomic<uint16_t> ctl_drag_permille_{0};
  std::atomic<bool> ctl_repaint_{false};  // paused: present the frame again with the controls
  std::atomic<bool> ctl_step_{false};     // paused after a seek: show the new frame
  bool ctl_failed_{false};
  bool ctl_pressed_{false};
  int32_t ctl_x_{0}, ctl_y_{0};
  uint32_t ctl_last_input_ms_{0};
  bool controls_active_() const;
  void controls_setup_();
  bool build_controls_bar_(ControlsBar &bar);
#ifdef SVP_PICTURES
  void build_slide_time_(ControlsBar &bar, lv_obj_t *row);
#endif
  /// Setup: pool bytes a direct session needs for them (0: none).
  size_t controls_bytes_();
  /// Main loop, before a direct session starts: everything the playback task copies.
  void prerender_controls_();
  void controls_loop_();
  float controls_fraction_(int32_t x) const;
  void controls_press_(uint8_t button);
  uint8_t *ctl_alloc_(size_t bytes);
  bool ctl_take_(ControlsSprite &sprite, const lv_draw_buf_t *snapshot, const lv_area_t &snap_area, lv_area_t part);
  void ctl_blit_(const ControlsSprite &sprite, int32_t x, int32_t y, int32_t width = -1);
  void ctl_panel_rect_(const lv_area_t &area, int32_t *x, int32_t *y, int32_t *w, int32_t *h) const;
  /// Playback task: the controls into the video's frame (direct_frame_).
  void composite_controls_();
  /// Playback task, paused: shows the video's frame (with the controls) again.
  void repaint_paused_();
#endif
#ifdef SVP_PICTURES
  struct ThumbSlot {
    int image{-1};  // image index shown, -1 = none
    bool loaded{false};
    uint8_t *data{nullptr};  // RGB565, inside thumb_buffer_
    lv_obj_t *button{nullptr};
    lv_obj_t *canvas{nullptr};
    lv_obj_t *label{nullptr};
  };
  // A whole picture file, compressed, in PSRAM: the shown one and the ones read ahead.
  struct PicFile {
    int index{-1};  // image, -1 for none
    uint32_t generation{0};
    uint8_t *data{nullptr};
    size_t capacity{0};
    size_t length{0};
    uint32_t used_ms{0};
  };
  static constexpr size_t PIC_FILE_COUNT = 3;  // the shown picture's and its neighbours'
  // A picture decoded at the canvas' size, in the pool: the shown one and its neighbours, so a step
  // only switches what the canvas shows; the next one is decoded into one not shown.
  struct PicPage {
    int index{-1};  // image, -1 for none (or being decoded, or drawn over)
    uint32_t generation{0};
    uint8_t *data{nullptr};
    size_t capacity{0};
    int32_t w{0}, h{0};  // the canvas size it was decoded for
    uint32_t stride{0};
    lv_draw_buf_t buf{};  // what the canvas shows of it
  };
  static constexpr size_t PIC_PAGE_COUNT = 2;  // the shown picture and the next one decoded (nothing ahead)
  // The file being read through the storage worker (whole, in one request), then decoded on the
  // picture task.
  struct PictureRead {
    bool active{false};
    bool done{false};
    bool failed{false};
    bool thumbnail{false};
    bool preload{false};  // only read ahead, not shown
    size_t index{0};      // image
    size_t slot{0};       // thumbnail slot
    uint32_t generation{0};
    PicFile *file{nullptr};
    storage::StreamHandle handle{};
    size_t limit{0};  // one byte more than the listed size: a file that grew is noticed
    size_t length{0};
    size_t chunk{0};
    bool decoding{false};  // handed to the picture task
    bool remote{false};    // Home Assistant's, fetched by the channel task (pic_fetch_)
  };
  // What the picture task decodes, and into what.
  struct PictureJob {
    bool thumbnail{false};
    const uint8_t *data{nullptr};
    size_t length{0};
    std::string path;
    // The canvas memory (a picture) or the slot (a thumbnail), RGB565.
    uint8_t *target{nullptr};
    size_t target_capacity{0};
    int32_t target_w{0}, target_h{0};
    uint32_t target_stride{0};
    ImageFitMode fit{ImageFitMode::SCALE_TO_FIT};
    PicPage *page{nullptr};  // a picture's
    uint8_t *scratch{nullptr};  // the whole decoded JPEG when it does not go straight in (pool)
    size_t scratch_capacity{0};
    // Result: straight into the target at the picture's own size (stride: the decoder's rows), or
    // scaled into all of it.
    bool ok{false};
    bool direct{false};
    int32_t width{0}, height{0};
    uint32_t stride{0};
    bool background{false};  // the page background's (written out to PSRAM as a picture is)
  };

  bool pic_ready_{false};
  enum class ViewStep : uint8_t { IDLE, WAITING, DECODING };
  lv_obj_t *view_canvas_{nullptr};
  ViewStep view_step_{ViewStep::IDLE};
  bool view_close_{false};
  bool view_into_{false};
  bool view_attached_{false};
  bool view_detach_pending_{false};
  uint8_t *view_data_{nullptr};
  size_t view_capacity_{0};
  uint8_t *view_scratch_{nullptr};
  lv_draw_buf_t view_buf_{};
  PictureJob view_job_data_;
  std::atomic<bool> view_job_{false};
  std::atomic<bool> view_decoded_{false};
  std::function<void(bool)> view_done_;
  void view_loop_();
  void release_view_();
  void cancel_view_();
#ifdef SVP_BACKGROUND
  // The page background: its file read by the worker into a pool piece, decoded by the picture task
  // (decode_job_) into the canvas' pool piece; both pieces of the pictures' share.
  enum class BgStep : uint8_t { IDLE, READING, READ, DECODING };
  lv_obj_t *bg_canvas_{nullptr};
  char bg_path_[256]{};
  bool bg_wanted_{false};
  bool bg_reload_{false};    // another file: what shows goes, the new one comes
  bool bg_failed_{false};    // this file did not show (not tried again until it or the page changes)
  bool bg_shown_{false};
  bool bg_attached_{false};  // the canvas reads the piece
  bool bg_detach_pending_{false};  // given back while LVGL was paused: the canvas lets go once it runs
  BgStep bg_step_{BgStep::IDLE};
  uint8_t *bg_data_{nullptr};
  size_t bg_capacity_{0};
  uint8_t *bg_file_{nullptr};
  size_t bg_len_{0};
  size_t bg_limit_{0};
  size_t bg_chunk_{0};
  bool bg_read_failed_{false};
  uint8_t *bg_scratch_{nullptr};  // a JPEG larger than the canvas: decoded whole here first (this decode only)
  storage::StreamHandle bg_handle_{};
  lv_draw_buf_t bg_buf_{};
  PictureJob bg_job_data_;
  std::atomic<bool> bg_job_{false};
  std::atomic<bool> bg_decoded_{false};
  void background_loop_();
  void release_background_();
  void bg_detach_();
  void bg_read_next_();
  void bg_finish_read_();
  bool bg_busy_() const { return this->bg_step_ == BgStep::READING || this->bg_step_ == BgStep::DECODING; }
#endif
  lv_obj_t *pic_canvas_{nullptr};
  PsramVector<PsramString> pic_dirs_;
  std::string pic_dir_;                  // folder whose images_ are listed
  size_t pic_dir_index_{SIZE_MAX};       // in pic_dirs_, SIZE_MAX for another folder
  std::string pic_select_;               // shown once the folder is listed
  bool pic_list_pending_{false};
  uint32_t pic_list_retry_ms_{0};
  uint32_t pic_generation_{0};           // bumped with every listing; older reads are dropped
  static constexpr size_t MAX_IMAGES = 1000;
  PoolList<ImageEntry> images_;
  PoolList<uint16_t> images_order_;  // sorting images_ (sort_by_order)
  bool pic_list_failed_{false};  // the folder's listing failed: images_ empty
  int pic_ha_show_{-1};           // ha_show_pictures() asked for while a folder is listed
  int current_image_{-1};
  int pic_wanted_{-1};                   // requested, not read yet
  PictureRead pic_read_;
#ifdef SVP_CHANNEL_LIST
  ChannelFetch pic_fetch_;  // a picture of Home Assistant's into its file (pic_read_.remote)
#endif
  PicFile pic_files_[PIC_FILE_COUNT];
  int pic_direction_{1};        // of the last step: the next picture read ahead
  uint8_t pic_preload_tries_{0};  // neighbours of the shown picture tried (each once)
  // PNG and BMP: no hardware for them; decoded by runtime_image, scaled, and let go.
  PsramPtr<runtime_image::RuntimeImage> pic_image_;
  // Decoding runs on the picture task; the job's file and target are its until pic_decoded_.
  TaskHandle_t pic_task_{nullptr};
  PictureJob pic_job_;
  uint8_t *pic_decode_buf_{nullptr};  // pool, grown in 1MB steps for a larger picture
  size_t pic_decode_cap_{0};
  std::atomic<bool> pic_decoded_{false};
  // A camera frame for the camera page (show_camera_frame): its job on the picture task, done on the main loop
  struct CameraJob {
    const uint8_t *data{nullptr};
    size_t length{0};
    CameraFrameFormat format{CameraFrameFormat::MJPEG};
    uint16_t width{0}, height{0};
    uint8_t *target{nullptr};
    size_t target_capacity{0};
    int32_t target_w{0}, target_h{0};
    bool ok{false};
    int32_t out_x{0}, out_y{0}, out_w{0}, out_h{0};  // the picture's rectangle in the target
    uint16_t pic_w{0}, pic_h{0};                     // no target: the decoded rows in the scratch
    uint8_t slot{0};                                 // the scratch piece decoded into
    bool direct{false};                              // full screen: into the scanout buffer not shown
  };
  CameraJob cam_job_data_{};
  std::atomic<bool> cam_job_{false};
  std::atomic<bool> cam_decoded_{false};
  bool cam_busy_{false};
  bool cam_end_pending_{false};  // camera_preview_end() while a frame was decoded: the scratch goes after it
  std::function<void(bool)> cam_done_{};
  // A JPEG larger than the canvas is decoded whole here, then scaled (pool, while the camera's picture shows)
  // No target (the camera layer): two, the next frame decoded into the one the reader does not hold (cam_held_)
  uint8_t *cam_scratch_[2]{};
  size_t cam_scratch_capacity_[2]{};
  uint8_t cam_held_{0};  // bit n: the reader shows scratch n (main loop)
  // Camera full screen (direct output): wanted (main loop), LVGL paused for it (loop(), read by the main loop); which
  // scanout buffers are black around the picture of cam_direct_key_ (picture task)
  std::atomic<bool> cam_direct_{false};
  std::atomic<bool> cam_direct_ready_{false};
  uint8_t cam_direct_black_{0};
  uint32_t cam_direct_key_{0};
  bool cam_direct_touch_{false};
  CallbackManager<void()> cam_direct_tap_callbacks_;
  // The canvas' black around a YUV frame the PPA fits in: drawn once per size
  uint32_t cam_cleared_key_{0};
  int pic_render_{-1};                    // image to decode again (overlay, canvas size), -1 none
  PicPage pic_pages_[PIC_PAGE_COUNT];
  PicPage *pic_shown_{nullptr};
  int32_t pic_w_{0}, pic_h_{0};           // the canvas size pictures are decoded for
  uint32_t pic_stride_{0};
  size_t pictures_bytes_() const;  // the pictures' share of the picture + audio mode (pages and files)
  size_t pic_page_bytes_{0};              // a page: the display's whole size, any canvas size fits
  bool pic_buffers_failed_{false};        // the pool had no room for them; tried again after a video
  lv_draw_buf_t *pic_lvgl_buf_{nullptr};  // the canvas' own buffer, shown while no page is; none: external_buffer
  bool pic_restore_{false};               // the pages went to a video: the picture decoded again after it
  bool pic_close_req_{false};             // close_picture(): the buffers go once nothing is in flight
  int8_t pic_fullscreen_pending_{-1};     // set_fullscreen() while a picture is decoded
  int32_t pic_pending_w_{0}, pic_pending_h_{0};  // its size; 0: the widget's
  bool pic_shown_fullscreen_{false};      // the canvas has the full screen size

  uint32_t slideshow_interval_ms_{5000};
  CallbackManager<void(uint32_t)> slideshow_interval_callbacks_;
  SlideshowMode slideshow_mode_{SlideshowMode::STOPPED};
  uint32_t last_slide_ms_{0};
  ImageFitMode fit_mode_{ImageFitMode::SCALE_TO_FIT};
  bool fullscreen_{false};
  int default_image_index_{0};

  int overlay_icon_size_{128};
  uint32_t overlay_icon_color_{0xFFFFFF};
  uint32_t overlay_duration_ms_{1500};
  bool overlay_visible_{false};
  bool overlay_playing_{false};
  uint32_t overlay_shown_ms_{0};

  uint32_t long_press_time_ms_{1000};
  uint32_t press_ms_{0};
  bool press_handled_{false};
  bool gesture_seen_{false};
  int32_t press_x_{0}, press_y_{0};

  bool thumbs_enabled_{true};
  int thumb_w_{120};
  int thumb_h_{90};
  size_t thumb_count_{10};
  bool thumb_lazy_{true};
  size_t thumb_preload_{5};
  ThumbnailLayout thumb_layout_{ThumbnailLayout::HORIZONTAL};
  int thumb_spacing_{5};
  int thumb_columns_{3};
  lv_obj_t *thumb_container_{nullptr};
  lv_style_t *thumb_style_{nullptr};
  lv_style_t *thumb_active_style_{nullptr};
  lv_style_t *thumb_container_style_{nullptr};
  lv_style_t *thumb_label_style_{nullptr};
  lv_style_t thumb_default_style_{};
  lv_style_t thumb_default_active_style_{};
  std::string thumb_label_pattern_;
  ThumbnailSlideEdge thumb_slide_edge_{ThumbnailSlideEdge::RIGHT};
  bool thumb_slide_enabled_{false};
  uint32_t thumb_auto_hide_ms_{10000};
  bool thumbs_visible_{false};
  uint32_t thumb_touched_ms_{0};
  PoolList<ThumbSlot> thumb_slots_;
  uint8_t *thumb_buffer_{nullptr};  // PSRAM, all slots
  uint32_t thumb_stride_{0};
  size_t thumb_window_{0};               // image shown by slot 0
  PoolList<size_t> thumb_queue_;         // slots to load, first first
  int32_t thumb_scroll_pos_{0};
  uint32_t thumb_scroll_ms_{0};
  float thumb_scroll_velocity_{0.0f};
  bool thumb_click_handled_{false};
  bool thumb_shifting_{false};
  CallbackManager<void(size_t)> thumb_click_callbacks_;
  std::function<void(size_t)> thumb_ready_callback_;

  void pictures_setup_();
  void pictures_loop_();
  bool pictures_held_() const;
  void list_directory_();
  static bool picture_entry_(const storage::FileStat *entry, void *ctx);
  void pictures_listed_(storage::StorageError err);
  uint8_t *picture_alloc_(size_t size);
  bool reserve_decode_(size_t size);
  bool acquire_picture_buffers_();
  void release_picture_buffers_();
  void release_decode_();
  PicFile *cached_file_(size_t index);
  PicFile *free_file_();
  bool reserve_file_(PicFile *file, size_t size);
  void start_read_(bool thumbnail, size_t index, size_t slot, bool preload = false);
  void read_next_chunk_();
  void finish_read_();
  void picture_read_done_();
  void preload_neighbours_();
  bool start_decode_(bool thumbnail, size_t index, size_t slot, PicFile *file, bool preload = false);
  void picture_decoded_();
  PicPage *page_of_(int index);
  PicPage *free_page_();
  void show_page_(PicPage *page);
  void update_picture_size_();
  void apply_picture_size_();
  static void picture_task_entry_(void *arg);
  void decode_job_(PictureJob &job);
  void camera_decode_();   // picture task
  bool camera_direct_draw_();  // picture task: the job's frame into the scanout buffer not shown, flipped to
  // Picture task: a 1:1 PPA copy (YUV converted) of a block of a picture into a picture of out_w x out_h
  bool camera_ppa_copy_(const void *in, uint32_t in_cm, uint32_t pic_w, uint32_t pic_h, uint32_t sx, uint32_t sy,
                        uint32_t bw, uint32_t bh, uint8_t *out, uint32_t out_cm, uint32_t out_w, uint32_t out_h,
                        size_t out_size, uint32_t dx, uint32_t dy, float scale);
  // Picture task: a picture (RGB565 or YUV422 block of pic_w x pic_h) fit into the job's target by the PPA, black
  // around it (drawn once per size). False: no PPA client or the PPA refused it.
  bool camera_ppa_fit_(const void *in, uint32_t in_cm, uint32_t pic_w, uint32_t pic_h, uint32_t block_w,
                       uint32_t block_h);
  void camera_service_();  // main loop
  void render_current_();
  void finish_picture_();
  void show_overlay_icon_(bool playing);
  void hide_overlay_icon_();
  void draw_play_icon_(int cx, int cy, int size, uint16_t color);
  void draw_pause_icon_(int cx, int cy, int size, uint16_t color);
  void fill_rect_(int x0, int y0, int x1, int y1, uint16_t color);
  bool setup_thumbnails_();
  void place_thumbnail_window_(size_t start);
  bool visible_slots_(size_t *first, size_t *last) const;
  void queue_thumbnails_(bool all);
  void queue_slot_(size_t slot);
  void show_slot_(size_t slot);
  void highlight_thumbnail_(bool scroll);
  void slide_thumbnails_(bool show);
  void thumbnails_scrolled_();
  std::string thumbnail_label_(size_t image) const;
  void canvas_gesture_(lv_dir_t dir);
  static void canvas_event_(lv_event_t *event);
  static void thumbnail_event_(lv_event_t *event);
#endif
#ifdef SVP_DSI_OUTPUT
  display::Display *dsi_{nullptr};  // non-null -> direct display output instead of the LVGL canvas
  bool dsi_lvgl_paused_{false};     // LVGL was paused for the duration of direct playback
  // Paused file playback: once the playback task is parked, LVGL gets the panel (on-device
  // controls). resume() then hands it back from loop(), before the task draws again.
  std::atomic<bool> resume_req_{false};  // resume() asked while LVGL shows the paused UI
  bool paused_ui_{false};                // LVGL was handed the panel for the pause (loop() only)
  std::atomic<bool> direct_{false};       // the current session draws onto the display
  uint16_t dsi_out_w_{0}, dsi_out_h_{0};  // panel native resolution; the video must match it
  uint8_t dsi_fb_bpp_{2};                 // display color_depth: 24 -> 3, 16 -> 2 bytes/pixel
  uint8_t *decode_target_{nullptr};       // own decode buffer (PSRAM pool): parts of a frame, one-buffer displays
  size_t decode_target_len_{0};
  // The display output of every source (video, stream, desktop). A whole frame is decoded into the
  // scanout buffer not shown, once the last flip took effect (woken by the display, at most
  // FLIP_WAIT_MS), and flipped to; a part of one, or a display without a second buffer in its depth,
  // goes into decode_target_ and is copied in.
  static constexpr uint32_t FLIP_WAIT_MS = 50;
  bool direct_flips_() const;
  /// Where a frame is decoded to (len: its room); nullptr: none.
  uint8_t *direct_target_(bool whole, size_t *len);
  /// The frame decoded into target shows: flipped to, or (decode_target_) copied in.
  void direct_show_(uint8_t *target, int x, int y, int w, int h, int x_pad);
  uint8_t *direct_frame_{nullptr};  // playback task: where the video's frame was decoded last (controls, repaint)
#endif

#ifdef SVP_STREAM
  struct StreamFrame {
    uint8_t *data{nullptr};
    size_t capacity{0};
    int32_t x{0}, y{0};  // the picture's top left on the output
    uint16_t w{0}, h{0};
    uint32_t total{0};
    uint32_t received{0};
    bool direct{false};  // for the display, else the canvas page
  };
  static constexpr uint8_t STREAM_FRAME_COUNT = 4;
  static constexpr size_t STREAM_RECV_BYTES = 32768;     // one recv(), into stream_recv_buf_ (pool)
  static constexpr uint32_t STREAM_AUDIO_BLOCK_MS = 10;  // stream_audio_block_ (pool)
  uint8_t *stream_recv_buf_{nullptr};
  uint16_t stream_port_{0};
  uint32_t stream_max_frame_bytes_{128 * 1024};
  StreamFrame stream_frames_[STREAM_FRAME_COUNT]{};
  QueueHandle_t stream_empty_q_{nullptr};
  QueueHandle_t stream_filled_q_{nullptr};
  TaskHandle_t stream_draw_task_{nullptr};
  // Held by the file playback for its whole session and by the stream draw per rectangle: both use
  // decode_target_ and the display, and the file has priority.
  SemaphoreHandle_t output_lock_{nullptr};
  // Parser state (network task only).
  StreamFrame *stream_current_{nullptr};
  uint8_t stream_hdr_[16]{};
  size_t stream_hdr_len_{0};
  uint32_t stream_skip_{0};
  bool stream_logged_bad_header_{false};
  std::atomic<bool> stream_connected_{false};  // a sender's (or the SPICE session's) picture shows
  std::atomic<bool> stream_drop_sender_{false};  // the network task lets its sender go (SPICE takes over)
  std::atomic<bool> stream_sender_busy_{false};  // the network task serves a sender (accepted until closed)
  std::atomic<bool> stream_output_ok_{false};  // loop(): connected and LVGL (if any) paused
  std::atomic<bool> stream_awake_{true};   // false while a file plays
  std::atomic<bool> stream_asleep_{false};  // stream_sleep()
  std::atomic<bool> stream_fullscreen_{true};  // set_stream_fullscreen()
  std::atomic<bool> stream_switch_{false};
  std::atomic<bool> stream_status_pending_{false};
  std::atomic<uint32_t> stream_echo_{0};  // bit 16 set: echo the low 16 bits back ('R'), udisp 0x22
  std::atomic<bool> stream_depth_pending_{false};
  // The sender streams an AVI (svp_relay) rather than udisp rectangles: it plays through the file
  // playback path, fed from the network task through stream_sink_.
  bool net_session_{false};                                // set before start_playback_()
  std::atomic<bool> stream_avi_{false};                    // an AVI connection is up
  std::atomic<bool> stream_played_to_end_{false};          // the network AVI's playback reached its end
  std::atomic<BufferedFileReader *> stream_sink_{nullptr};  // open network source, or none
#ifdef SVP_STREAM_STATS
  // Each counted by one task; taken and reset by the network task at every report.
  struct StreamStats {
    std::atomic<uint32_t> bytes{0};        // received
    std::atomic<uint32_t> rects{0};        // JPEG rectangles received whole
    std::atomic<uint32_t> skipped{0};      // rectangles counted out: no free buffer, or not usable
    std::atomic<uint32_t> wait_us{0};      // waiting for a free buffer
    std::atomic<uint32_t> drawn{0};        // rectangles decoded and drawn (draw task)
    std::atomic<uint32_t> draw_us{0};      // decoding and drawing them
    std::atomic<uint32_t> sound_bytes{0};  // PCM received
    std::atomic<uint32_t> frames{0};       // pictures decoded by the playback task (an AVI, a file)
    std::atomic<uint32_t> frame_us{0};     // decoding them
    std::atomic<uint32_t> sound_waits{0};  // the speaker feed found no sound for AUDIO_WAIT
  } stream_stats_;
  std::atomic<bool> stream_stats_log_{false};  // set_stream_statistics()
  bool stream_stats_sender_{false};            // the connected sender asked for 'M' (network task)
  bool stream_stats_running_{false};           // network task
  uint32_t stream_stats_ms_{0};                // the last report (network task)
#endif
#ifdef SVP_SPICE
  static constexpr size_t SPICE_SCRATCH_BYTES = 65536;
  struct SpiceState {
    char host[64];
    char password[61];  // SPICE_MAX_PASSWORD_LENGTH + 1
    uint16_t port;
  };
  SpiceState *spice_{nullptr};          // PSRAM pool
  uint8_t *spice_scratch_{nullptr};
  SemaphoreHandle_t spice_lock_{nullptr};  // spice_->host/password/port
  TaskHandle_t spice_task_{nullptr};
  int spice_wake_fd_{-1};
  std::atomic<uint8_t> spice_request_{0};  // 1 connect, 2 disconnect
  std::atomic<bool> spice_on_{false};
  std::atomic<bool> spice_connecting_{false};
  std::atomic<bool> spice_failed_{false};
  std::atomic<bool> spice_browser_{false};  // the connect asked for last is the Browser's
  std::atomic<bool> spice_inputs_on_{false};    // the session's inputs channel takes keys, mouse, touches
  std::atomic<bool> spice_sound_on_{false};     // the session's playback channel is open
  std::atomic<bool> spice_sound_claim_{false};  // loop(): what plays on the session's sound device stops
  std::atomic<bool> spice_direct_out_{true};    // the session draws onto the display, else the canvas
  bool spice_sound_shares_files_{false};
  bool spice_sound_shares_media_player_{false};
  // Cursor: the session sets shape and place (under spice_cursor_lock_), the draw task shows it.
  // Shapes as straight ARGB; SPICE_CURSOR_INVERT (alpha 0, not black) inverts what is under it.
  static constexpr uint16_t SPICE_CURSOR_MAX = 64;
  static constexpr uint8_t SPICE_CURSOR_CACHE = 16;
  static constexpr uint32_t SPICE_CURSOR_INVERT = 0x00FFFFFF;
  struct SpiceCursorShape {
    uint64_t id;
    uint32_t used;  // last use, for the cache
    uint16_t w, h, hot_x, hot_y;
    bool valid;
    uint32_t px[SPICE_CURSOR_MAX * SPICE_CURSOR_MAX];
  };
  struct SpiceCursor {
    SpiceCursorShape shape;
    int32_t x, y;  // the hot spot on the output
    bool visible;
  };
  SpiceCursor *spice_cursor_{nullptr};             // PSRAM pool, while a session has them
  SpiceCursorShape *spice_cursor_cache_{nullptr};  // PSRAM pool, SPICE_CURSOR_CACHE (spice task)
  SemaphoreHandle_t spice_cursor_lock_{nullptr};
  std::atomic<bool> spice_cursor_dirty_{false};
  // Draw task: what is under the drawn cursor, and the cursor composed over it (PSRAM pool).
  uint8_t *spice_cursor_under_{nullptr};
  uint8_t *spice_cursor_patch_{nullptr};
  bool spice_cursor_drawn_{false};
  int32_t spice_cursor_rx_{0}, spice_cursor_ry_{0}, spice_cursor_rw_{0}, spice_cursor_rh_{0};
  struct SpiceSaved {
    char name[32];
    char host[64];
    uint16_t port;
    bool used;
  };
  uint8_t spice_saved_count_{8};
  SpiceSaved *spice_saved_{nullptr};  // PSRAM pool, spice_saved_count_
  ESPPreferenceObject *spice_saved_prefs_{nullptr};
  CallbackManager<void()> spice_callbacks_;
  std::atomic<bool> spice_changed_{false};  // loop(): spice_callbacks_
  std::atomic<uint8_t> spice_close_keys_{1};
  std::atomic<uint32_t> spice_close_hold_ms_{1000};
  std::atomic<uint32_t> spice_touch_hold_ms_{1000};
  bool desktop_took_{false};  // spice task: the session of this connect took the panel
  // The relay's sender of the desktop's picture: let in from its address while wanted; gone: it
  // disconnected (the session ends with it). relay_inputs: keys, mouse and touches go to it (VNC).
  std::atomic<uint32_t> desktop_relay_addr_{0};
  std::atomic<bool> desktop_sender_wanted_{false};
  std::atomic<bool> desktop_sender_gone_{false};
  std::atomic<bool> desktop_relay_inputs_{false};
  std::atomic<uint8_t> usbip_attached_{0};  // bit per USB slot (spice task)
  // Network task: the pointer's shape from the relay (udisp 0x23) as it arrives.
  uint32_t desktop_cursor_left_{0};
  size_t desktop_cursor_at_{0};
  bool desktop_cursor_ok_{false};
  // Network task: the desktop's sender gets only DESKTOP_FRAMES of the stream frames (one received, one
  // drawn), the others are held here meanwhile -- no picture of the past waits on the panel.
  static constexpr uint8_t DESKTOP_FRAMES = 2;
  bool stream_desktop_sender_{false};
  StreamFrame *stream_held_[STREAM_FRAME_COUNT]{};
  uint8_t stream_held_count_{0};
  void spice_saved_setup_();
  // The connections file (spice_file_path_): one line per connection, read and written whole through
  // the storage worker (main loop); its text in spice_file_buf_ (PSRAM pool).
  static constexpr uint8_t SPICE_SAVED_USB = 4;  // USB devices kept per connection
  static constexpr size_t SPICE_FILE_LINE = 512;
  struct SpiceSavedUsb {
    uint16_t vid, pid;  // vid 0: none
    char serial[64];
  };
  std::string spice_file_path_;
  SpiceSavedUsb *spice_saved_usb_{nullptr};  // PSRAM pool, spice_saved_count_ * SPICE_SAVED_USB
  char *spice_file_buf_{nullptr};
  size_t spice_file_len_{0};
  size_t spice_file_got_{0};
  storage::StreamHandle spice_file_handle_{};
  bool spice_file_loaded_{false};  // read (or found missing): written from now on
  bool spice_file_busy_{false};
  bool spice_file_dirty_{false};
  int8_t spice_active_saved_{-1};  // the saved connection connected last, -1: none
  bool spice_usb_applying_{false};
  size_t spice_file_bytes_() const { return static_cast<size_t>(this->spice_saved_count_) * SPICE_FILE_LINE; }
  void spice_file_load_();
  void spice_file_read_();
  void spice_file_read_done_(storage::StorageError err);
  void spice_file_parse_();
  void spice_file_save_();
  void spice_file_write_();
  void spice_saved_changed_(uint8_t index);
#ifdef SVP_SPICE_USB
  static constexpr uint8_t SPICE_USB_SLOTS = 4;
  SpiceUsbSlot *spice_usb_[SPICE_USB_SLOTS]{};
  uint8_t spice_usb_count_{0};
#endif
#endif
#ifdef SVP_CANVAS_OUTPUT
  // Canvas output: the part of the page the udisp rectangles changed since loop() last told LVGL.
  portMUX_TYPE stream_dirty_lock_ = portMUX_INITIALIZER_UNLOCKED;
  bool stream_dirty_{false};
  int32_t stream_dirty_x1_{0}, stream_dirty_y1_{0}, stream_dirty_x2_{0}, stream_dirty_y2_{0};
#endif
#ifdef SVP_STREAM_TOUCH
  static constexpr uint8_t STREAM_TOUCH_MAX = 5;
  struct StreamTouch {
    uint8_t count;
    uint8_t id[STREAM_TOUCH_MAX];
    uint16_t x[STREAM_TOUCH_MAX];
    uint16_t y[STREAM_TOUCH_MAX];
  };
  touchscreen::Touchscreen *stream_touchscreen_{nullptr};
  StreamTouchListener stream_touch_listener_{this};
  QueueHandle_t stream_touch_q_{nullptr};
  StreamTouch stream_last_touch_{};
  bool stream_last_touch_valid_{false};
#endif
#ifdef SVP_STREAM_INPUT
  static constexpr uint8_t STREAM_INPUT_MAX = 13;
  static constexpr uint8_t STREAM_INPUT_QUEUE = 32;
  struct StreamInput {
    uint8_t length;
    uint8_t data[STREAM_INPUT_MAX];
  };
  StreamInputTaker stream_input_taker_{this};
  StaticQueue_t stream_input_q_store_{};
  QueueHandle_t stream_input_q_{nullptr};
  std::atomic<bool> stream_input_wanted_{false};  // the connected sender asked for input (udisp 0x21)
#endif
#ifdef USE_AUDIO
  uint32_t stream_audio_left_{0};  // PCM payload bytes still to come (network task)
  uint8_t stream_audio_src_ch_{1};
  uint32_t stream_audio_rate_{0};
  uint8_t stream_audio_carry_[4]{};  // a sample frame split across two reads
  size_t stream_audio_carry_len_{0};
  uint8_t *stream_audio_block_{nullptr};
  size_t stream_audio_block_size_{0};
  size_t stream_audio_block_used_{0};
  uint8_t stream_audio_out_ch_{1};
  bool stream_logged_audio_format_{false};
  std::atomic<bool> stream_rate_pending_{false};
  uint32_t stream_speaker_rate_{0};  // the rate the speaker was started in for a sender's sound
  std::atomic<bool> stream_audio_on_{false};
  std::atomic<uint32_t> stream_last_audio_ms_{0};
#endif
#endif

#ifdef SVP_KEYBOARD
  // The list (svp_relay's "name\tdescription\n" lines, cut into strings in place), the entries into
  // it, the layouts read (a cache of KBD_SLOTS, a layout each) and what is given whom (pool).
  static constexpr size_t KBD_LIST_BYTES = 32 * 1024;
  static constexpr size_t KBD_MAX = 1024;
  static constexpr size_t KBD_LAYOUT_BYTES = 8 * 1024;
  static constexpr size_t KBD_SLOTS = 6;
  static constexpr size_t KBD_ASSIGN_MAX = 24;
  struct KbdEntry {
    uint16_t name;  // offsets into kbd_list_
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
  // Kept in preferences: a device's key (truncated) or "@" and a keyboard's name, the layout given
  // it ("" none: a USB device remembered), the device's name.
  struct KbdAssign {
    char key[48];
    char layout[32];
    char name[32];
    uint8_t input;  // the keyboard it was seen behind
  };
  struct KbdAssignTable {
    KbdAssign entries[KBD_ASSIGN_MAX];
  };
  struct KbdInput {
    hid::HIDInput *input;
    std::string name;
    std::string layout;  // the configuration's standard
    const hid::KeyboardLayout *own;
  };
  void keyboard_setup_();
  void keyboard_loop_();
  /// The directory's layouts: the list of them (build), else which of the list's are there.
  void keyboard_list_local_(bool build);
  void keyboard_listed_();
  /// The layouts wanted read (or failed), then put where they are wanted; the next read started.
  void keyboard_resolve_();
  /// The layout a keyboard's standard / a device has: given it, else ... ("" its own).
  std::string keyboard_standard_(size_t input) const;
  const KbdAssign *keyboard_assigned_(const std::string &key) const;
  KbdSlot *keyboard_slot_(const std::string &name);
  void keyboard_next_();
  void keyboard_read_();
  void keyboard_read_end_(bool ok);
  void keyboard_fetch_();
  /// The read slot's layout, if it is one (a download: kept only).
  bool keyboard_loaded_(size_t len);
  void keyboard_keep_(size_t len);
  void keyboard_write_();
  void keyboard_done_(bool ok);
  void keyboard_save_();
  std::string kbd_dir_, kbd_dir_default_;
  PsramVector<KbdInput> kbd_inputs_;
  char *kbd_list_{nullptr};
  KbdEntry *kbd_entries_{nullptr};
  size_t kbd_count_{0};
  bool kbd_listed_{false};
  bool kbd_list_remote_{false};
  bool kbd_list_busy_{false};
  KbdSlot kbd_slots_[KBD_SLOTS]{};
  KbdAssignTable *kbd_assign_{nullptr};
  ESPPreferenceObject kbd_pref_;  // the table's place before keyboards.bin, taken over from there once
  // The table kept in the configured directory (keyboards.bin) through the storage worker: read
  // once, written anew (a .tmp moved over it, from its copy in the pool) after each change.
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
  PoolList<FixedString<32>> kbd_downloads_;
  static constexpr size_t KBD_DEVICES_MAX = KBD_ASSIGN_MAX + 8;
  mutable PoolList<KeyboardDevice> kbd_devices_;  // keyboard_devices()'
  int8_t kbd_loading_{-1};      // the slot being read into
  bool kbd_downloading_{false};  // ... for a download (kept, not put on a keyboard)
  KbdStep kbd_step_{KBD_IDLE};
  bool kbd_ready_{false};
  bool kbd_kept_{false};  // its file written so far
  FixedString<32> kbd_loading_name_;
  size_t kbd_len_{0};
  size_t kbd_got_{0};
  storage::StreamHandle kbd_handle_{};
  CallbackManager<void()> kbd_callbacks_;
  // From the channel task: the list (its length) / the layout into the slot being read.
  std::atomic<bool> kbd_list_done_{false}, kbd_get_done_{false};
  bool kbd_list_ok_{false}, kbd_get_ok_{false};
  size_t kbd_list_len_{0}, kbd_get_len_{0};
#ifdef SVP_KEYBOARD_ENTITIES
  // The device select's options (labels, the devices' keys) in the pool, the one picked.
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
#endif

#ifdef SVP_CHANNEL_LIST
  std::string channel_url_;
  std::string channel_token_;
  http_request::HttpRequestComponent *channel_http_request_{nullptr};
  lvgl::LvDropdownType *channel_widget_{nullptr};
  QueueHandle_t channel_q_{nullptr};
  SemaphoreHandle_t channel_lock_{nullptr};
  // Shown in the dropdown (LVGL holds pointers into channel_names_) / next list from the task.
  static constexpr size_t CHANNEL_MAX = 300;
  PoolList<FixedString<64>> channel_names_, channel_new_names_;
  PoolList<FixedString<256>> channel_refs_, channel_new_refs_;
  std::atomic<bool> channel_new_{false};
  bool channel_requested_once_{false};
  uint32_t channel_version_{0};
  int channel_index_{-1};
  // Home Assistant media (channel_lock_ for what the task and the main loop share).
  static constexpr size_t HA_ITEMS_MAX = 500;  // as the relay sends at most
  static constexpr size_t HA_SOURCES_MAX = 32;
  static constexpr size_t HA_REQS_MAX = 8;
  using HaType = FixedString<16>;
  // A folder's listing, besides its items.
  struct HaListing {
    HaType type;
    SvpId id;
    SvpTitle title;
    SvpPath path;
    SvpId parent;
  };
  // The texts shared with the channel task (a pool piece). listings[shown]: the main loop's; the other
  // the task's to write while ha_new_ is clear.
  struct HaTexts {
    HaListing listings[2];
    uint8_t shown{0};
    const HaListing &listing() const { return this->listings[this->shown]; }
    HaType sources_type;
    SvpId play_id;
    SvpId picture_id;  // the picture the task fetches into pic_read_.file
  };
  PsramPtr<HaTexts> ha_texts_;
  PoolList<std::pair<HaType, SvpId>> ha_reqs_;  // type, id: listings asked for, in turn
  uint16_t ha_browse_waiting_{0};                // of them, not asked from the task yet: no network
  PoolList<HaItem> ha_src_items_;
  bool ha_src_ok_{false}, ha_last_sources_{false};
  // The sources of each type listed by the task since the main loop last took them, in turn: one a
  // type was asked for right after another's must not take its place.
  // A ring of HA_SOURCE_LISTS; the task fills the one after the last listed, outside the lock.
  struct HaSources {
    HaType type;
    PoolList<HaItem> items;
    bool ok{false};
  };
  static constexpr size_t HA_SOURCE_LISTS = 3;
  HaSources ha_new_srcs_[HA_SOURCE_LISTS];
  uint8_t ha_srcs_head_{0}, ha_srcs_count_{0};
  std::atomic<bool> ha_new_src_{false};
  PoolList<HaItem> ha_items_, ha_new_items_;
  bool ha_ok_{false}, ha_new_ok_{false};
  std::atomic<bool> ha_new_{false};
  CallbackManager<void()> ha_callbacks_;
  int32_t pic_remote_w_{0}, pic_remote_h_{0};  // no larger than the display (the relay fits it)
  // svp_relay settings: key and value, key and choices (one per line); sets waiting for the task.
  static constexpr size_t OPT_VALUES_MAX = 32;
  static constexpr size_t OPT_CHOICES_MAX = 16;
  static constexpr size_t OPT_SETS_MAX = 8;
  using OptPair = std::pair<FixedString<32>, FixedString<256>>;
  PoolList<OptPair> opt_values_, opt_new_values_, opt_choices_, opt_new_choices_, opt_sets_;
  bool opt_ok_{false}, opt_new_ok_{false};
  std::atomic<bool> opt_new_{false};
  CallbackManager<void()> opt_callbacks_;
#endif

  jpeg_decoder_handle_t hw_jpeg_decoder_{nullptr};

  // ONE compressed "storage-load" buffer: the reader task (Core 0) demuxes the next compressed
  // frame into it, the playback task (Core 1) decodes from it, cycled through two FreeRTOS queues.
  // This is NOT where pre-buffering/read-jitter absorption happens -- that is the PSRAM ring in the
  // storage interface (BufferedFileReader), which sits in front of this. The decoded render
  // double-buffer is output_buffers_[2] (the two frame buffers); this is the single video buffer.
  // Allocated once in setup(), reused every play().
  static constexpr uint8_t FRAME_SLOT_COUNT = 1;
  struct FrameSlot {
    uint8_t *data{nullptr};
    size_t capacity{0};
    int size{0};  // payload bytes (> 0); 0 = EOF sentinel; -1 = read-error sentinel
    uint32_t gen{0};         // frame_gen_ when read: a seek since makes it stale
    uint32_t file_frame{0};  // frame number in the file
  };
  FrameSlot frame_slots_[FRAME_SLOT_COUNT]{};
  QueueHandle_t empty_queue_{nullptr};   // slots free for the reader to fill
  QueueHandle_t filled_queue_{nullptr};  // slots filled, waiting for decode
  /// The playback task gives a frame slot back to the reader.
  void return_slot_(FrameSlot *slot);
  TaskHandle_t reader_task_handle_{nullptr};  // Core 0, made once in setup()
  std::atomic<bool> reader_busy_{false};      // a session's reading runs on it
  volatile bool reader_task_stop_{false};
  // Absolute presentation index of the last frame read (demux order, monotonic across a loop
  // rewind). Playback-task-local.
  uint32_t video_frame_index_{0};
  // Set true in the teardown of playback_loop_() (and wired to file_reader_'s abort flag) so an
  // in-flight storage wait returns promptly at end of playback.
  volatile bool playback_task_stop_{false};

  // Playlist (main loop only). The playback task asks loop() for the next item at the end of a file.
  PoolList<SvpPath> playlist_;
  PoolList<uint16_t> playlist_order_;  // sorting playlist_ (sort_by_order)
  size_t playlist_index_{0};
  RepeatMode repeat_{RepeatMode::OFF};
  std::atomic<bool> advance_req_{false};
  static constexpr size_t MAX_PLAYLIST = 500;
  // What is to play next (main loop): a file (start_path_, its folder as the playlist with
  // start_folder_) or, start_path_ empty, playlist item start_index_. Started by
  // run_pending_start_() once the session before has ended.
  std::atomic<bool> start_pending_{false};
  bool start_folder_{false};
  std::string start_path_;
  size_t start_index_{0};
  // Held from "no session runs" to the new playback task: the main loop and the network task.
  SemaphoreHandle_t session_lock_{nullptr};

  // Seeking: seek() (main loop) -> playback task (handle_seek_) -> reader task (reader_seek_).
  std::atomic<bool> seek_pending_{false};
  std::atomic<uint32_t> seek_target_{0};       // video frame
  std::atomic<bool> reader_seek_req_{false};   // set by the playback task, cleared by the reader when done
  std::atomic<uint32_t> reader_seek_frame_{0};
  std::atomic<bool> reader_seek_ok_{false};
  std::atomic<uint32_t> frame_gen_{0};         // reader task: bumped by each seek
  uint32_t expected_gen_{0};                   // playback task: frames of an older gen are dropped
  std::atomic<uint32_t> position_frame_{0};    // file frame shown last (an audio file: ms played)
  std::atomic<uint32_t> duration_frames_{0};   // video frames of the file (an audio file: ms); 0 = no seeking

  // FreeRTOS task (decode/playback, Core 1 -- alongside the main loop/LVGL, see play()'s
  // xTaskCreatePinnedToCore comment for why decode specifically needs to share that core)
  TaskHandle_t task_handle_{nullptr};       // Core 1, made once in setup()
  std::atomic<bool> playback_busy_{false};  // a session runs on it

  // A task blocks on its notification until done() holds; whoever changes what it waits for calls
  // wake() afterwards. waiting gates the wake, so the task is not woken where it waits for something
  // else. guard only bounds a lost wake; feed: the task is on the task watchdog (resetting it
  // otherwise logs an error).
  struct Waiter {
    TaskHandle_t task{nullptr};
    std::atomic<bool> waiting{false};
    void wake() {
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (this->waiting.load(std::memory_order_relaxed) && this->task != nullptr)
        xTaskNotifyGive(this->task);
    }
    template<typename F> void wait(F done, TickType_t guard, bool feed = false) {
      this->waiting.store(true, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      while (!done()) {
        if (feed)
          esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, guard);
      }
      this->waiting.store(false, std::memory_order_relaxed);
    }
  };
  static constexpr TickType_t WAIT_GUARD = pdMS_TO_TICKS(1000);
  Waiter playback_wait_;
  // One-shot at a frame's presentation time: wakes the playback task blocked until then.
  esp_timer_handle_t pace_timer_{nullptr};
  static void pace_timer_cb_(void *arg) { static_cast<SimpleVideoPlayer *>(arg)->playback_wait_.wake(); }
  bool playback_wdt_on_{false};  // playback task: on the task watchdog
  /// Playback task: blocks until done() holds (see Waiter).
  template<typename F> void playback_wait_for_(F done) {
    this->playback_wait_.wait(done, WAIT_GUARD, this->playback_wdt_on_);
  }
  void set_playback_wdt_(bool on) {
    if (on == this->playback_wdt_on_)
      return;
    this->playback_wdt_on_ = on;
    if (on) {
      esp_task_wdt_add(nullptr);
    } else {
      esp_task_wdt_delete(nullptr);
    }
  }
  Waiter reader_wait_;
  Waiter audio_wait_;
  // The tasks waiting in pool_request_() for the main loop to hand back pool memory (playback, stream
  // draw, spice), a slot each: the groups asked for, 0 once answered or taken back.
  struct PoolRequest {
    std::atomic<TaskHandle_t> task{nullptr};
    std::atomic<uint8_t> user{0};
    std::atomic<uint8_t> groups{0};
  };
  static constexpr size_t POOL_REQUESTS = 4;
  PoolRequest pool_requests_[POOL_REQUESTS];
  /// Main loop: this request answered, if it still asks for these groups.
  static void pool_answered_(PoolRequest &request, uint8_t groups) {
    if (request.groups.compare_exchange_strong(groups, 0, std::memory_order_acq_rel)) {
      if (TaskHandle_t task = request.task.load(std::memory_order_acquire))
        xTaskNotifyGive(task);
    }
  }

  // Frame pacing -- wall clock is the master timeline. Each frame's target instant is
  // playback_start_time_us_ + paused_accum_us_ + index * frame_duration_us_. The task blocks until
  // that instant (pace_timer_), then presents; it never drops a frame.
  // Audio free-runs on the speaker clock on its own Core 0 task -- no explicit A/V re-sync.
  int64_t playback_start_time_us_{0};  // esp_timer_get_time() at frame 0
  int64_t paused_accum_us_{0};         // total wall time spent PAUSED, excluded from media_us
  float frame_duration_us_{0};         // duration of one frame in microseconds (1000000/fps)

  // Automation callbacks
  CallbackManager<void()> on_started_callbacks_;
  CallbackManager<void()> on_finished_callbacks_;
  CallbackManager<void()> on_paused_callbacks_;
  CallbackManager<void(uint8_t)> on_error_callbacks_;
  // Set by the playback task, the automations then run from loop(): -1 none, else the PlaybackError.
  std::atomic<bool> finished_pending_{false};
  std::atomic<int16_t> error_pending_{-1};
  CallbackManager<void()> state_callbacks_;
  std::atomic<bool> state_changed_{false};  // set wherever state_ changes, state_callbacks_ run from loop()
#ifdef SVP_CHANNEL_LIST
  CallbackManager<void()> channels_callbacks_;
#endif
#ifdef SVP_PICTURES
  CallbackManager<void()> picture_callbacks_;
#endif
  CallbackManager<void()> status_callbacks_;
  bool video_shown_{false};  // local_video_active() as the status last reported it
#ifdef SVP_MEDIA_PLAYER
  media_player::MediaPlayer *media_player_{nullptr};
  void setup_media_player_();
  void build_media_traits_();
  std::string media_traits_json_;  // "pause":..,"formats":[..] of the status
#endif

#ifdef SVP_CAPTURE
  // ---- Screen capture (screen_capture.cpp) ---------------------------------------------------------
  // LVGL's drawing goes into the back one of two pictures (the screen's size and colour format); once
  // a refresh is complete and the encoder does not read the front one, they swap and the capture task
  // copies what changed into the new back one before LVGL draws into it again (it waits for that).
  // The capture task encodes the front picture once per tick when it changed, into the JPEG buffer
  // not written out; a tick's picture goes out as a batch (CastAvi::Batch: the newest JPEG, or an
  // empty chunk for the same picture, and the sound since) to the file (main loop, storage worker)
  // and/or svp_relay (the cast sender task), one batch at a time.
  class CaptureSink : public lvgl::LvglComponent::FrameCapture {
   public:
    explicit CaptureSink(SimpleVideoPlayer *player) : player_(player) {}
    uint8_t *capture_target() override { return this->player_->cap_target_(); }
    void capture_area(const lv_area_t &area) override { this->player_->cap_area_(area); }
    void capture_done() override { this->player_->cap_done_(); }

   protected:
    SimpleVideoPlayer *player_;
  };
  static constexpr size_t CAP_JPEG_BYTES = 512 * 1024;
  static constexpr size_t CAP_PREFIX = 128;  // the picture's chunk header before its JPEG (cache-line aligned)
  static constexpr size_t CAP_PCM_BYTES = 32 * 1024;
  static constexpr size_t CAP_INDEX_BYTES = 128 * 1024;
  static constexpr uint16_t CAP_MAX_PICTURES = 250;  // in one batch at most
  // After the JPEG: the batch's further chunk headers and its sound.
  static constexpr size_t CAP_TAIL = CastAvi::CHUNK_HEADER * (CAP_MAX_PICTURES + 2) + CAP_PCM_BYTES;
  static constexpr size_t CAP_JPEG_ROOM = CAP_JPEG_BYTES - CAP_PREFIX - CAP_TAIL;
  static constexpr uint8_t CAP_RECTS = 16;
  static constexpr uint32_t CAP_SYNC_WAIT_MS = 100;
  static constexpr uint32_t CAP_ENCODE_TIMEOUT_MS = 200;
  static constexpr uint32_t CAP_RELAY_WAIT_MS = 10000;
  static constexpr uint32_t CAP_TX_TIMEOUT_MS = 5000;
  static constexpr size_t CAP_TASK_STACK = 4096;
  enum : uint32_t {
    CAPN_TICK = 1 << 0,   // the screencast's tick
    CAPN_SYNC = 1 << 1,   // the pictures swapped: the changed areas into the new back one
    CAPN_SHOT = 1 << 2,   // a screenshot's picture (or, casting, the newest JPEG) wanted
    CAPN_SINK = 1 << 3,   // a batch or a screenshot was written out
    CAPN_STOP = 1 << 4,   // no batches any more
    CAPN_CAST_SHOT = 1 << 5,  // casting: a screenshot of the newest JPEG
    CAPN_CAM = 1 << 6,
  };
  enum : uint8_t { CAP_OWNER_FREE, CAP_OWNER_ENCODING, CAP_OWNER_SWAPPING };
  enum : uint8_t { CAP_SINK_FILE = 1, CAP_SINK_RELAY = 2 };
  enum : uint8_t { CAP_RELAY_NONE, CAP_RELAY_ASKED, CAP_RELAY_CONNECTING, CAP_RELAY_READY, CAP_RELAY_FAILED };
  // The file's steps (main loop).
  enum : uint8_t {
    CF_NONE,
    CF_OPENING,
    CF_HEADER,
    CF_READY,
    CF_BUSY,  // an operation in flight
    CF_BATCH,
    CF_IX_VIDEO,
    CF_IX_AUDIO,
    CF_SEEK_HEADER,
    CF_HEADER_AGAIN,
    CF_SEEK_PATCH,
    CF_PATCH,
    CF_SEEK_END,
    CF_SEGMENT,
    CF_CLOSING,
    CF_DONE,
    CF_FAILED,
  };

  void capture_setup_();
  /// Main loop: what the screen capture has to do (sinks, its start and end, sessions taking the panel).
  void capture_loop_();
  /// The task, the encoder, the PPA client and the tick, made the first time and kept.
  bool capture_ready_();
  /// A screenshot's / the screencast's buffers from the pool; all or none.
  bool capture_take_(bool cast);
  void capture_free_frames_();
  void capture_free_rest_();
  /// Ends the screencast (reason "" when asked to); its file is completed after.
  void capture_end_(const char *reason);
  /// Main loop, a session's request for POOL_CAPTURE: true once the pictures and JPEGs are back.
  bool capture_give_back_(uint8_t user);
  void capture_finished_();
  void capture_notify_(uint32_t bits);
  std::string capture_name_(const std::string &dir, const char *prefix, const char *suffix);

  // LVGL thread (FrameCapture).
  uint8_t *cap_target_();
  void cap_area_(const lv_area_t &area);
  void cap_done_();

  // Capture task.
  static void capture_task_entry_(void *param);
  void capture_task_loop_();
  static void capture_tick_cb_(void *arg);
  void capture_sync_();
  bool capture_encode_(const uint8_t *picture, uint8_t *out, uint32_t *len);
  void capture_camera_();
  bool capture_cast_begin_(uint8_t target);
  void capture_cast_encode_();
  void capture_compose_();
  void capture_shot_from_cast_();
  /// A sink is done with the batch (any task).
  void capture_sink_done_(uint8_t sink);

  // Main loop: the screenshot's file, the screencast's file.
  void shot_write_();
  void shot_done_(bool ok);
  bool cast_file_open_();
  void cast_file_run_();
  void cast_file_op_(storage::StorageError err);
  void cast_file_result_(storage::StorageError err);
  void cast_file_write_(const uint8_t *data, size_t len, uint8_t next);
  void cast_file_seek_(uint64_t pos, uint8_t next);
  void cast_file_fail_();

  // svp_relay: asked on the channel task, sent on the cast sender task.
#ifdef SVP_CHANNEL_LIST
  void cast_relay_task_(bool start);
#endif
  static void cast_tx_entry_(void *param);
  void cast_tx_loop_();
  bool cast_tx_send_(const uint8_t *data, size_t len);
  void cast_tx_close_();

  std::string shot_dir_;
  std::string cast_dir_;
  std::atomic<uint8_t> cap_quality_{80};
  uint8_t cast_fps_{25};
  uint8_t cast_target_{CAST_LOCAL};
  bool cast_sound_{true};
#ifdef SVP_CAPTURE_MIC
  microphone::MicrophoneSource *cap_mic_{nullptr};
#endif
  Trigger<std::string> shot_trigger_;
  Trigger<> cast_started_trigger_;
  Trigger<std::string> cast_stopped_trigger_;
  CallbackManager<void()> cap_callbacks_;
  CaptureSink cap_sink_{this};

  CastState cast_state_{CAST_IDLE};
  std::string cast_reason_;  // why it ends
  TaskHandle_t cap_task_{nullptr};
  esp_timer_handle_t cap_timer_{nullptr};
  jpeg_encoder_handle_t cap_encoder_{nullptr};
  ppa_client_handle_t cap_ppa_{nullptr};
  StaticSemaphore_t cap_sync_sem_buf_;
  SemaphoreHandle_t cap_sync_sem_{nullptr};
  // The pictures (2 for a screencast, 1 for a screenshot), the JPEG buffers, the file's index and the
  // sound ring; from the pool.
  uint8_t *cap_frame_[2]{};
  uint8_t *cap_jpeg_[2]{};
  uint8_t *cap_index_{nullptr};
  uint8_t *cap_pcm_{nullptr};
  size_t cap_frame_bytes_{0};  // a piece (whole cache lines)
  uint16_t cap_w_{0};
  uint16_t cap_h_{0};
  uint8_t cap_bpp_{2};
  // LVGL thread.
  uint8_t cap_back_{1};
  lv_area_t cap_rects_[CAP_RECTS];
  uint8_t cap_rects_n_{0};
  bool cap_need_take_{false};
  bool cap_single_{false};  // a screenshot drawn by LVGL anew into cap_frame_[0]
  // Set by the LVGL thread at a swap, read by the capture task while cap_sync_busy_.
  lv_area_t cap_sync_rects_[CAP_RECTS];
  uint8_t cap_sync_n_{0};
  std::atomic<bool> cap_sync_busy_{false};
  std::atomic<uint8_t> cap_front_{0};
  std::atomic<uint32_t> cap_swaps_{0};  // complete pictures in the front one so far
  std::atomic<uint8_t> cap_owner_{CAP_OWNER_FREE};
  std::atomic<bool> cap_broken_{false};
  // Capture task.
  uint32_t cap_encoded_swaps_{0};
  uint8_t cap_wjpeg_{0};       // the JPEG buffer written out (the other one is encoded into)
  uint32_t cap_wlen_{0};       // its JPEG, 0 none yet
  bool cap_ejpeg_ready_{false};  // the other one has a newer JPEG
  uint32_t cap_elen_{0};
  uint32_t cap_ticks_{0};
  uint32_t cap_enc_errors_{0};
  bool cap_shot_wait_{false};  // casting: a screenshot waits for the first JPEG
  std::atomic<uint8_t> cap_jpeg_users_[2]{};  // screenshots still writing them
  std::atomic<bool> cap_casting_{false};
  std::atomic<bool> cap_stopping_{false};
  std::atomic<bool> cap_drained_{false};
  std::atomic<bool> cap_shot_cast_{false};  // a screenshot of the screencast's JPEG wanted
  // The batch out (written by the capture task before cap_batch_sinks_).
  CastAvi::Batch cap_batch_{};
  const uint8_t *cap_batch_data_{nullptr};
  size_t cap_batch_len_{0};
  std::atomic<uint8_t> cap_batch_sinks_{0};
  std::atomic<bool> cap_file_batch_{false};
  std::atomic<uint8_t> cap_sinks_{0};  // the screencast's sinks still up
  bool cap_relay_asked_any_{false};    // svp_relay was asked to take it: told when it ends
  // The screenshot (main loop; the data from the capture task).
  std::atomic<bool> shot_ready_{false};
  const uint8_t *shot_data_{nullptr};
  uint32_t shot_len_{0};
  int8_t shot_jpeg_{-1};  // the screencast's JPEG buffer it writes, -1 its own buffers
  bool shot_busy_{false};
  uint8_t cam_cap_{0};
  bool cap_camera_{false};
  bool cam_cap_shot_{false};
  std::atomic<bool> cam_cap_busy_{false};
  std::atomic<bool> cam_cap_finished_{false};
  const uint8_t *cam_cap_data_{nullptr};
  size_t cam_cap_len_{0};
  CameraFrameFormat cam_cap_format_{CameraFrameFormat::RGB565};
  uint16_t cam_cap_w_{0};
  uint16_t cam_cap_h_{0};
  uint32_t cam_cap_last_{0};
  std::function<void()> cam_cap_done_;
  const char *cast_prefix_{"screencast_"};
  uint8_t shot_step_{0};
  bool shot_ok_{false};
  std::string shot_path_;
  storage::StreamHandle shot_handle_{};
  std::string shot_last_stamp_;
  uint8_t shot_same_stamp_{0};
  // The sound: the microphone's PCM into the ring (any task while cap_ring_live_).
#ifdef SVP_CAPTURE_MIC
  std::shared_ptr<ring_buffer::RingBuffer> cap_ring_;
  std::atomic<ring_buffer::RingBuffer *> cap_ring_raw_{nullptr};
  std::atomic<uint8_t> cap_mic_in_{0};
  bool cap_mic_on_{false};
#endif
  uint32_t cap_rate_{0};
  uint8_t cap_channels_{0};
  // The screencast's file.
  CastAvi cap_avi_;
  std::string cast_path_;
  storage::StreamHandle cast_handle_{};
  uint8_t cast_file_{CF_NONE};
  uint8_t cast_file_next_{CF_NONE};
  bool cast_file_final_{false};
  bool cast_file_pending_{false};  // the batch waits for the segment's end
  // The operation in flight, for a retry when the worker was not ready for it.
  bool cast_op_seek_{false};
  const uint8_t *cast_op_data_{nullptr};
  size_t cast_op_len_{0};
  uint64_t cast_op_pos_{0};
  // svp_relay.
  std::atomic<uint8_t> cap_relay_{CAP_RELAY_NONE};
  std::atomic<uint16_t> cap_relay_port_{0};
  char cap_relay_token_[48]{};
  uint32_t cap_relay_asked_{0};
  TaskHandle_t cap_tx_task_{nullptr};
  int cap_sock_{-1};
  std::atomic<bool> cap_tx_connect_{false};
  std::atomic<bool> cap_tx_send_{false};
  std::atomic<bool> cap_tx_close_{false};
#endif
};

//========================================================================
// Automation Actions
//========================================================================

template<typename... Ts> class PlayAction : public Action<Ts...> {
 public:
  explicit PlayAction(SimpleVideoPlayer *player) : player_(player) {}

  TEMPLATABLE_VALUE(std::string, path)
  void set_folder_playlist(bool folder) { this->folder_playlist_ = folder; }

  void play(const Ts &...x) override {
    auto path = this->path_.value(x...);
    this->player_->play(path, this->folder_playlist_);
  }

 protected:
  SimpleVideoPlayer *player_;
  bool folder_playlist_{false};
};

template<typename... Ts> class PauseAction : public Action<Ts...> {
 public:
  explicit PauseAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(const Ts &...x) override { this->player_->pause(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class TogglePauseAction : public Action<Ts...> {
 public:
  explicit TogglePauseAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->toggle_pause(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class NextAction : public Action<Ts...> {
 public:
  explicit NextAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->next(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class PreviousAction : public Action<Ts...> {
 public:
  explicit PreviousAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->previous(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class RepeatAction : public Action<Ts...> {
 public:
  explicit RepeatAction(SimpleVideoPlayer *player) : player_(player) {}
  void set_mode(RepeatMode mode) { this->mode_ = mode; }
  /// Instead of a mode: off -> playlist -> file -> off.
  void set_cycle(bool cycle) { this->cycle_ = cycle; }
  void play(const Ts &...x) override {
    if (!this->cycle_) {
      this->player_->set_repeat(this->mode_);
      return;
    }
    switch (this->player_->get_repeat()) {
      case RepeatMode::OFF:
        this->player_->set_repeat(RepeatMode::PLAYLIST);
        break;
      case RepeatMode::PLAYLIST:
        this->player_->set_repeat(RepeatMode::FILE);
        break;
      default:
        this->player_->set_repeat(RepeatMode::OFF);
        break;
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  RepeatMode mode_{RepeatMode::OFF};
  bool cycle_{false};
};

/// The volume by a step (negative: down), within 0..1.
template<typename... Ts> class VolumeStepAction : public Action<Ts...> {
 public:
  explicit VolumeStepAction(SimpleVideoPlayer *player) : player_(player) {}
  void set_step(float step) { this->step_ = step; }
  void play(const Ts &...x) override {
    float volume = this->player_->get_volume() + this->step_;
    this->player_->set_volume(volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume));
  }

 protected:
  SimpleVideoPlayer *player_;
  float step_{0.05f};
};

template<typename... Ts> class SeekAction : public Action<Ts...> {
 public:
  explicit SeekAction(SimpleVideoPlayer *player) : player_(player) {}

  TEMPLATABLE_VALUE(float, position)
  void set_relative(bool relative) { this->relative_ = relative; }

  void play(const Ts &...x) override {
    const float seconds = this->position_.value(x...);
    if (this->relative_) {
      this->player_->seek_relative(seconds);
    } else {
      this->player_->seek(seconds);
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  bool relative_{false};
};

template<typename... Ts> class ResumeAction : public Action<Ts...> {
 public:
  explicit ResumeAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(const Ts &...x) override { this->player_->resume(); }

 protected:
  SimpleVideoPlayer *player_;
};

#ifdef SVP_CHANNEL_LIST
template<typename... Ts> class ChannelRefreshAction : public Action<Ts...> {
 public:
  explicit ChannelRefreshAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->channel_list_refresh(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class ChannelPlayAction : public Action<Ts...> {
 public:
  explicit ChannelPlayAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(int, index)
  void play(const Ts &...x) override {
    if (this->index_.has_value()) {
      this->player_->channel_play(this->index_.value(x...));
    } else {
      this->player_->channel_play();
    }
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class ChannelStopAction : public Action<Ts...> {
 public:
  explicit ChannelStopAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->channel_stop(); }

 protected:
  SimpleVideoPlayer *player_;
};
#endif

#ifdef SVP_STREAM
template<typename... Ts> class StreamSleepAction : public Action<Ts...> {
 public:
  explicit StreamSleepAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->stream_sleep(true); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class StreamWakeAction : public Action<Ts...> {
 public:
  explicit StreamWakeAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->stream_sleep(false); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class StreamOutputAction : public Action<Ts...> {
 public:
  explicit StreamOutputAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(bool, fullscreen)
  void play(const Ts &...x) override { this->player_->set_stream_fullscreen(this->fullscreen_.value(x...)); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class StreamStatisticsAction : public Action<Ts...> {
 public:
  explicit StreamStatisticsAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(bool, enable)
  void play(const Ts &...x) override { this->player_->set_stream_statistics(this->enable_.value(x...)); }

 protected:
  SimpleVideoPlayer *player_;
};

#ifdef SVP_SPICE
template<typename... Ts> class SpiceConnectAction : public Action<Ts...> {
 public:
  explicit SpiceConnectAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(std::string, host)
  TEMPLATABLE_VALUE(uint16_t, port)
  TEMPLATABLE_VALUE(std::string, password)
  void play(const Ts &...x) override {
    this->player_->spice_connect(this->host_.value(x...), this->port_.value(x...), this->password_.value(x...));
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class SpiceConnectSavedAction : public Action<Ts...> {
 public:
  explicit SpiceConnectSavedAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(uint8_t, index)
  TEMPLATABLE_VALUE(std::string, password)
  void play(const Ts &...x) override {
    this->player_->spice_connect_saved(this->index_.value(x...), this->password_.value(x...));
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class SpiceSaveAction : public Action<Ts...> {
 public:
  explicit SpiceSaveAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(uint8_t, index)
  TEMPLATABLE_VALUE(std::string, name)
  TEMPLATABLE_VALUE(std::string, host)
  TEMPLATABLE_VALUE(uint16_t, port)
  void play(const Ts &...x) override {
    this->player_->spice_save(this->index_.value(x...), this->name_.value(x...), this->host_.value(x...),
                              this->port_.value(x...));
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class SpiceForgetAction : public Action<Ts...> {
 public:
  explicit SpiceForgetAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(uint8_t, index)
  void play(const Ts &...x) override { this->player_->spice_forget(this->index_.value(x...)); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class SpiceDisconnectAction : public Action<Ts...> {
 public:
  explicit SpiceDisconnectAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->spice_disconnect(); }

 protected:
  SimpleVideoPlayer *player_;
};
#endif
#endif

template<typename... Ts> class SetVolumeAction : public Action<Ts...> {
 public:
  explicit SetVolumeAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(float, volume)
  void play(const Ts &...x) override { this->player_->set_volume(this->volume_.value(x...)); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(const Ts &...x) override { this->player_->stop(); }

 protected:
  SimpleVideoPlayer *player_;
};

#ifdef SVP_CAPTURE
template<typename... Ts> class ScreenshotAction : public Action<Ts...> {
 public:
  explicit ScreenshotAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->screenshot(); }

 protected:
  SimpleVideoPlayer *player_;
};

/// 0 start, 1 stop, 2 the one or the other.
template<typename... Ts> class ScreencastAction : public Action<Ts...> {
 public:
  explicit ScreencastAction(SimpleVideoPlayer *player) : player_(player) {}
  void set_command(uint8_t command) { this->command_ = command; }
  void play(const Ts &...x) override {
    if (this->command_ == 0) {
      this->player_->screencast_start();
    } else if (this->command_ == 1) {
      this->player_->screencast_stop();
    } else {
      this->player_->screencast_toggle();
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  uint8_t command_{2};
};
#endif

//========================================================================
// Trigger Implementations (after SimpleVideoPlayer is complete)
//========================================================================

inline PlaybackStartedTrigger::PlaybackStartedTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_started_callback([this]() { this->trigger(); });
}

inline PlaybackFinishedTrigger::PlaybackFinishedTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_finished_callback([this]() { this->trigger(); });
}

inline PlaybackPausedTrigger::PlaybackPausedTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_paused_callback([this]() { this->trigger(); });
}

inline PlaybackErrorTrigger::PlaybackErrorTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_error_callback([this](uint8_t error) { this->trigger(error); });
}

#ifdef SVP_PICTURES
inline ThumbnailClickTrigger::ThumbnailClickTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_thumbnail_click_callback([this](size_t index) { this->trigger(index); });
}

template<typename... Ts> class ShowImageAction : public Action<Ts...> {
 public:
  explicit ShowImageAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(int, index)
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override {
    if (this->path_.has_value()) {
      this->player_->show_image(this->path_.value(x...));
    } else {
      const int index = this->index_.value(x...);
      if (index >= 0)
        this->player_->show_image(static_cast<size_t>(index));
    }
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class PictureDirectoryAction : public Action<Ts...> {
 public:
  explicit PictureDirectoryAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(int, index)
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override {
    if (this->path_.has_value()) {
      this->player_->open_directory(this->path_.value(x...));
    } else {
      const int index = this->index_.value(x...);
      if (index >= 0)
        this->player_->set_current_directory(static_cast<size_t>(index));
    }
  }

 protected:
  SimpleVideoPlayer *player_;
};

enum class PictureCommand : uint8_t { NEXT, PREVIOUS, START, STOP, PAUSE, TOGGLE, REFRESH };

/// next_image, previous_image, the slideshow, refresh_images.
template<typename... Ts> class PictureCommandAction : public Action<Ts...> {
 public:
  PictureCommandAction(SimpleVideoPlayer *player, PictureCommand command) : player_(player), command_(command) {}
  void play(const Ts &...x) override {
    switch (this->command_) {
      case PictureCommand::NEXT:
        this->player_->next_image();
        break;
      case PictureCommand::PREVIOUS:
        this->player_->previous_image();
        break;
      case PictureCommand::START:
        this->player_->start_slideshow();
        break;
      case PictureCommand::STOP:
        this->player_->stop_slideshow();
        break;
      case PictureCommand::PAUSE:
        this->player_->pause_slideshow();
        break;
      case PictureCommand::TOGGLE:
        this->player_->toggle_slideshow();
        break;
      case PictureCommand::REFRESH:
        this->player_->refresh_images();
        break;
    }
  }

 protected:
  SimpleVideoPlayer *player_;
  PictureCommand command_;
};

template<typename... Ts> class FullscreenAction : public Action<Ts...> {
 public:
  explicit FullscreenAction(SimpleVideoPlayer *player) : player_(player) {}
  TEMPLATABLE_VALUE(bool, fullscreen)
  void play(const Ts &...x) override { this->player_->set_fullscreen(this->fullscreen_.value(x...)); }

 protected:
  SimpleVideoPlayer *player_;
};
#endif

}  // namespace esphome::simple_video_player
