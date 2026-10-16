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

#ifdef SVP_PICTURES
#include "esphome/components/runtime_image/runtime_image.h"
#include "esphome/components/storage/storage_worker.h"
#endif

#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#ifdef USE_AUDIO
#include "esphome/components/audio/audio_decoder.h"
#endif
#ifdef SVP_AUDIO_ROUTER
#include "esphome/components/audio_router/speaker/audio_router.h"
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
// ring_buffer::RingBuffer is used for the audio input/decoded rings (see USE_AUDIO members).
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "buffered_file_reader.h"
#include "avi_parser.h"
#include "psram_pool.h"
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

#ifdef USE_AUDIO
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
                  bool level3) override;
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
/// An object of the player's in PSRAM only; empty when PSRAM has no room for it.
template<class T> using PsramPtr = RAMUniquePtr<T>;
template<class T, class... Args> PsramPtr<T> psram_make(Args &&...args) {
  return RAMAllocator<T>(RAMAllocator<T>::ALLOC_EXTERNAL).make_unique(std::forward<Args>(args)...);
}

struct ImageEntry {
  PsramString path;
  PsramString filename;
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
  /// The most the sound may lead with the prebuffer set (ms): ring and sound fit in their share.
  uint32_t max_audio_latency() const { return this->max_audio_latency_ms_; }
  void set_speaker_channel_mode(SpeakerChannelMode mode) { this->speaker_channel_mode_ = mode; }
#endif
#ifdef SVP_AUDIO_ROUTER
  /// The speaker is this router: sound is wanted in its selected output's rate and channel count.
  void set_audio_router(audio_router::AudioRouter *router) { this->audio_router_ = router; }
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
  const PsramString &audio_queue_item(size_t index) const { return this->audio_queue_[index]; }
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
  const PsramVector<PsramString> &get_channel_names() const { return this->channel_names_; }
  uint32_t get_channel_list_version() const { return this->channel_version_; }
  // The channel played last, -1 for none.
  int get_channel_index() const { return this->channel_index_; }

  // Home Assistant media through svp_relay: the media sources that have a type ("audio", "video",
  // "image"; id empty) or a folder of one, listed for the main loop (add_on_ha_browse_callback); a
  // sound or video played through the relay, pictures shown on the picture canvas like local ones.
  struct HaItem {
    PsramString title;
    PsramString id;
    bool dir{false};
  };
  void ha_browse(const std::string &type, const std::string &id);
  // The sources of a type, listed last; whether the callback is for them (else for a folder).
  const PsramVector<HaItem> &ha_source_items() const { return this->ha_src_items_; }
  const PsramString &ha_sources_type() const { return this->ha_src_type_; }
  bool ha_sources_listed() const { return this->ha_src_ok_; }
  bool ha_last_sources() const { return this->ha_last_sources_; }
  // The folder listed last.
  const PsramVector<HaItem> &ha_items() const { return this->ha_items_; }
  const PsramString &ha_type() const { return this->ha_type_; }
  const PsramString &ha_id() const { return this->ha_id_; }
  const PsramString &ha_title() const { return this->ha_title_; }
  const PsramString &ha_path() const { return this->ha_path_; }
  const PsramString &ha_parent() const { return this->ha_parent_; }  // empty: none
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
    std::string key;   // hid::InputDevice's; "@" and the keyboard's name for a keyboard's standard
    std::string name;
    uint8_t input;     // which keyboard
    bool connected;
    std::string layout;  // given it: "" none (the keyboard's standard / for a standard: its own)
  };
  size_t keyboard_input_count() const { return this->kbd_inputs_.size(); }
  const std::string &keyboard_input_name(size_t index) const { return this->kbd_inputs_[index].name; }
  /// Each keyboard's standard (key "@name") and every device of theirs: those behind them now, and
  /// the USB ones seen before.
  PsramVector<KeyboardDevice> keyboard_devices() const;
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

  /// Requests an image of the current folder; it shows once read and decoded. False when out of range.
  bool show_image(size_t index);
  /// An image by full path or file name; a path outside the current folder opens its folder.
  bool show_image(const std::string &path);
  bool next_image();
  bool previous_image();
  void start_slideshow();
  void stop_slideshow();
  void pause_slideshow();
  void toggle_slideshow();
  SlideshowMode get_slideshow_mode() const { return this->slideshow_mode_; }
  size_t get_image_count() const { return this->images_.size(); }
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

  /// Main loop, no session running: the playlist becomes the videos of path's folder.
  void load_folder_playlist_(const std::string &path);
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
    return this->stream_fullscreen_.load(std::memory_order_acquire);
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
  /// The panel to the session: a playing video ends, a stream sender is let go, no cursor yet.
  /// false: they did not let go in time.
  bool desktop_take_panel_(bool direct, bool &wifi_held);
  /// Spice task, as the session takes the panel: pictures' and a video's buffers back to the pool (main
  /// loop), the session's cursor buffers from it.
  bool desktop_buffers_take_();
  /// The desktop's buffers back to the pool while no session runs (media wants the room); any task.
  bool release_desktop_();
  SemaphoreHandle_t desktop_lock_{nullptr};  // its buffers: taken, or given back
  std::atomic<bool> media_release_req_{false};  // main loop: release_media_() for a desktop session
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
  /// A VNC session: all of it through the relay, the panel's keys, mouse and touches too.
  void desktop_vnc_(const char *host, uint16_t port, const char *password);
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
  };
  struct ChannelCmd {
    uint8_t cmd;
    uint16_t index;
  };
  bool setup_channel_list_();
  bool channel_request_(uint8_t cmd, uint16_t index);
  void channel_loop_();
  void ha_loop_();
  static void channel_task_entry_(void *param);
  void channel_task_loop_();
  /// Blocking HTTP to the relay (channel task only). response: PSRAM buffer, caller frees.
  bool channel_http_(const char *path, const char *body, char **response, size_t *response_len,
                     uint8_t *into = nullptr, size_t into_cap = 0, uint32_t timeout_ms = 0);
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
  void image_task_();
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
  // The channel task fetches into cover_file_ (cover_fetch_w_ x cover_fetch_h_), the picture task
  // decodes it; each hands over with its flag (release / acquire).
  int32_t cover_fetch_w_{0}, cover_fetch_h_{0};
  size_t cover_len_{0};
  bool cover_fetch_ok_{false};
  std::atomic<bool> cover_fetched_{false};
  std::atomic<bool> cover_job_{false};
  std::atomic<bool> cover_decoded_{false};
  bool cover_decode_ok_{false};
  size_t cover_bytes_();
  size_t cover_reserve_{0};  // the views' buffers and the file: their share of the picture + audio mode
  uint8_t *cover_alloc_(size_t size);
  bool cover_buffers_();
  void release_cover_buffers_();
  void cover_loop_();
  void cover_task_();     // channel task
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
  // side's idle buffers handed back.
  bool plan_pool_();
  /// From any task but the main loop: a piece for the media side; asks the main loop for the
  /// pictures' if needed.
  uint8_t *media_alloc_(size_t size);
  /// Playback task: the read-ahead ring, the sound block and the compressed-frame slot of a session
  /// (the index is taken in open_file_(), only for a file's AVI). Ring and sound block are taken
  /// anew only when the size this session needs differs from the kept one's.
  bool acquire_session_buffers_();
  /// The display's decode buffer (and the controls) / the canvas output pair, kept once taken. Only
  /// when it does not fit beside the other output, the main loop gives that back first.
  bool ensure_direct_target_(bool with_target);
  bool ensure_canvas_outputs_();
  uint8_t *output_alloc_(size_t size, uint8_t other);
  /// Main loop: the media side's buffers back to the pool, when nothing plays or streams.
  bool release_media_();
  /// Main loop: what output_alloc_() asked for (an idle output back to the pool).
  void handle_output_release_();
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
  std::atomic<bool> pool_reclaim_req_{false};  // media side waits for the pictures' buffers
  /// Any task but the main loop: the pictures' buffers back to the pool, waited for at most 2 s.
  void reclaim_pictures_();
  enum : uint8_t { RELEASE_CANVAS = 1, RELEASE_DIRECT = 2 };
  std::atomic<uint8_t> output_release_req_{0};  // an output waits for the other one's memory
#ifdef SVP_STREAM
  std::atomic<bool> output_release_stream_{false};  // the stream's draw task asked for it
#endif
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
  bool sound_block_for_(uint32_t lead_ms);
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
  audio_router::AudioRouter *audio_router_{nullptr};
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
  PsramVector<PsramString> audio_queue_;
  PsramVector<PsramString> audio_titles_;  // per track; empty: its file's name
  PsramVector<uint16_t> audio_order_;
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
  };

  bool pic_ready_{false};
  lv_obj_t *pic_canvas_{nullptr};
  PsramVector<PsramString> pic_dirs_;
  std::string pic_dir_;                  // folder whose images_ are listed
  size_t pic_dir_index_{SIZE_MAX};       // in pic_dirs_, SIZE_MAX for another folder
  std::string pic_select_;               // shown once the folder is listed
  bool pic_list_pending_{false};
  uint32_t pic_list_retry_ms_{0};
  uint32_t pic_generation_{0};           // bumped with every listing; older reads are dropped
  PsramVector<ImageEntry> images_;
  int current_image_{-1};
  int pic_wanted_{-1};                   // requested, not read yet
  PictureRead pic_read_;
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
  PsramVector<ThumbSlot> thumb_slots_;
  uint8_t *thumb_buffer_{nullptr};  // PSRAM, all slots
  uint32_t thumb_stride_{0};
  size_t thumb_window_{0};               // image shown by slot 0
  PsramVector<size_t> thumb_queue_;      // slots to load, first first
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
  static constexpr size_t SPICE_SCRATCH_BYTES = 20480;  // internal RAM: a 64x64 cursor's message fits
  struct SpiceState {
    char host[64];
    char password[61];  // SPICE_MAX_PASSWORD_LENGTH + 1
    uint16_t port;
  };
  SpiceState *spice_{nullptr};          // PSRAM pool
  uint8_t *spice_scratch_{nullptr};     // internal RAM: the session's messages
  SemaphoreHandle_t spice_lock_{nullptr};  // spice_->host/password/port
  TaskHandle_t spice_task_{nullptr};
  int spice_wake_fd_{-1};
  std::atomic<uint8_t> spice_request_{0};  // 1 connect, 2 disconnect
  std::atomic<bool> spice_on_{false};
  std::atomic<bool> spice_connecting_{false};
  std::atomic<bool> spice_failed_{false};
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
  PsramVector<PsramString> kbd_downloads_;
  int8_t kbd_loading_{-1};      // the slot being read into
  bool kbd_downloading_{false};  // ... for a download (kept, not put on a keyboard)
  KbdStep kbd_step_{KBD_IDLE};
  bool kbd_ready_{false};
  bool kbd_kept_{false};  // its file written so far
  PsramString kbd_loading_name_;
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
  PsramVector<PsramString> channel_names_, channel_refs_;
  PsramVector<PsramString> channel_new_names_, channel_new_refs_;
  std::atomic<bool> channel_new_{false};
  bool channel_requested_once_{false};
  uint32_t channel_version_{0};
  int channel_index_{-1};
  // Home Assistant media (channel_lock_ for what the task and the main loop share).
  PsramVector<std::pair<PsramString, PsramString>> ha_reqs_;  // type, id: listings asked for, in turn
  PsramString ha_play_id_;
  PsramVector<HaItem> ha_src_items_;
  PsramString ha_src_type_;
  bool ha_src_ok_{false}, ha_last_sources_{false};
  // The sources of each type listed by the task since the main loop last took them, in turn: one a
  // type was asked for right after another's must not take its place.
  struct HaSources {
    PsramString type;
    PsramVector<HaItem> items;
    bool ok;
  };
  PsramVector<HaSources> ha_new_srcs_;
  std::atomic<bool> ha_new_src_{false};
  PsramVector<HaItem> ha_items_, ha_new_items_;
  PsramString ha_type_, ha_id_, ha_title_, ha_path_, ha_parent_;
  PsramString ha_new_type_, ha_new_id_, ha_new_title_, ha_new_path_, ha_new_parent_;
  bool ha_ok_{false}, ha_new_ok_{false};
  std::atomic<bool> ha_new_{false};
  CallbackManager<void()> ha_callbacks_;
  PsramString pic_remote_id_;     // the picture the task fetches into pic_read_.file
  int32_t pic_remote_w_{0}, pic_remote_h_{0};  // no larger than the display (the relay fits it)
  // svp_relay settings: key and value, key and choices (one per line); sets waiting for the task.
  PsramVector<std::pair<PsramString, PsramString>> opt_values_, opt_new_values_, opt_choices_, opt_new_choices_,
      opt_sets_;
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
  PsramVector<PsramString> playlist_;
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
  std::atomic<uint32_t> position_frame_{0};    // file frame shown last
  std::atomic<uint32_t> duration_frames_{0};   // video frames of the file; 0 = no seeking

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
  // The tasks waiting in media_alloc_()/output_alloc_()/desktop_buffers_take_() for the main loop to
  // free pool memory (playback, stream draw, spice): each one is woken.
  static constexpr size_t ALLOC_WAITERS = 4;
  std::atomic<TaskHandle_t> alloc_waiters_[ALLOC_WAITERS]{};
  /// The requesting task blocks until the main loop has answered (done() true), at most 2 s.
  template<typename F> void wait_alloc_answer_(F done) {
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    std::atomic<TaskHandle_t> *slot = nullptr;
    for (auto &waiter : this->alloc_waiters_) {
      TaskHandle_t none = nullptr;
      if (waiter.compare_exchange_strong(none, self, std::memory_order_acq_rel)) {
        slot = &waiter;
        break;
      }
    }
    App.wake_loop_threadsafe();
    const uint32_t start = millis();
    for (uint32_t waited = 0; !done() && waited < 2000; waited = millis() - start)
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000 - waited));
    if (slot != nullptr)
      slot->store(nullptr, std::memory_order_release);
  }
  /// Main loop: a pool request answered.
  void answer_alloc_() {
    for (auto &waiter : this->alloc_waiters_) {
      if (TaskHandle_t task = waiter.load(std::memory_order_acquire))
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
#ifdef SVP_MEDIA_PLAYER
  media_player::MediaPlayer *media_player_{nullptr};
  void setup_media_player_();
  void build_media_traits_();
  std::string media_traits_json_;  // "pause":..,"formats":[..] of the status
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
