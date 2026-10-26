#include "simple_video_player.h"

#ifdef SVP_PICTURES

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.pictures";

static constexpr size_t PICTURE_STEP = 1024 * 1024;  // picture buffers grow in these
static constexpr uint32_t LIST_RETRY_MS = 1000;
static constexpr int32_t LONG_PRESS_SLOP = 20;  // px the finger may move and still long-press

static std::string lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return text;
}

static bool picture_format(const std::string &name, runtime_image::ImageFormat *format) {
  if (name.rfind("media-source://", 0) == 0) {
    *format = runtime_image::JPEG;  // Home Assistant's: the relay sends a JPEG
    return true;
  }
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos)
    return false;
  const std::string extension = lower(name.substr(dot));
  if (extension == ".jpg" || extension == ".jpeg") {
    *format = runtime_image::JPEG;  // the JPEG unit, not runtime_image
    return true;
  }
#ifdef USE_RUNTIME_IMAGE_PNG
  if (extension == ".png") {
    *format = runtime_image::PNG;
    return true;
  }
#endif
#ifdef USE_RUNTIME_IMAGE_BMP
  if (extension == ".bmp") {
    *format = runtime_image::BMP;
    return true;
  }
#endif
  return false;
}

static constexpr size_t align_up(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }


static uint16_t to_rgb565(uint32_t color) {
  return ((color >> 8) & 0xF800) | ((color >> 5) & 0x07E0) | ((color >> 3) & 0x001F);
}

//========================================================================
// Setup, loop
//========================================================================

size_t SimpleVideoPlayer::pictures_bytes_() const {
  return this->pic_canvas_ != nullptr ? PIC_PAGE_COUNT * this->pic_page_bytes_ + PIC_FILE_COUNT * PICTURE_STEP : 0;
}

void SimpleVideoPlayer::pictures_setup_() {
  if (this->pic_canvas_ == nullptr)
    return;
  // At the pool's end for good (plan_pool_()).
  if (!this->images_.reserve(this->pool_, MAX_IMAGES) || !this->images_order_.reserve(this->pool_, MAX_IMAGES)) {
    ESP_LOGE(TAG, "Pictures disabled: no room in the PSRAM pool for their list");
    return;
  }
  this->pic_lvgl_buf_ = lv_canvas_get_draw_buf(this->pic_canvas_);
  this->update_picture_size_();
  lv_display_t *display = lv_obj_get_display(this->pic_canvas_);
  const int32_t dw = std::max(lv_display_get_horizontal_resolution(display), this->pic_w_);
  const int32_t dh = std::max(lv_display_get_vertical_resolution(display), this->pic_h_);
  this->pic_page_bytes_ =
      align_up(static_cast<size_t>(lv_draw_buf_width_to_stride(dw, LV_COLOR_FORMAT_RGB565)) * dh, PsramPool::ALIGN);
#ifdef SVP_CHANNEL_LIST
  this->pic_remote_w_ = dw;
  this->pic_remote_h_ = dh;
#endif
#if defined(USE_RUNTIME_IMAGE_PNG) || defined(USE_RUNTIME_IMAGE_BMP)
  this->pic_image_ = psram_make<runtime_image::RuntimeImage>(runtime_image::AUTO, image::IMAGE_TYPE_RGB565,
                                                             image::TRANSPARENCY_OPAQUE);
  if (!this->pic_image_)
    ESP_LOGE(TAG, "No PSRAM for the PNG/BMP decoder");
#endif
  if (this->thumbs_enabled_ && this->thumb_container_ != nullptr && this->thumb_count_ > 0)
    this->setup_thumbnails_();

  lv_obj_t *canvas = this->pic_canvas_;
  lv_obj_set_clickable(canvas, true);
  lv_obj_set_scrollable(canvas, false);
  lv_obj_set_gesture_bubble(canvas, false);  // swipes are ours, not the page's
  lv_obj_add_event_cb(canvas, &SimpleVideoPlayer::canvas_event_, LV_EVENT_ALL, this);
  // Leaving the page ends the slideshow.
  lv_obj_t *screen = lv_obj_get_screen(canvas);
  if (screen != nullptr) {
    lv_obj_add_event_cb(
        screen,
        [](lv_event_t *event) {
          auto *self = static_cast<SimpleVideoPlayer *>(lv_event_get_user_data(event));
          if (self->slideshow_mode_ == SlideshowMode::PLAYING)
            self->stop_slideshow();
        },
        LV_EVENT_SCREEN_UNLOAD_START, this);
  }

  // Decoding (and scaling a large photo) off the main loop: on Core 1 at the loop task's priority
  // like the video's decode (DMA2D with LVGL's PPA, esp-idf#18999, see play()).
  if (!create_psram_task(picture_task_entry_, "svp_pictures", 8192, this, 1, &this->pic_task_, 0)) {
    ESP_LOGE(TAG, "Could not create the picture task -- pictures disabled");
    return;
  }

  if (!this->pic_dirs_.empty()) {
    this->pic_dir_index_ = 0;
    this->pic_dir_ = this->pic_dirs_[0].c_str();
    this->pic_list_pending_ = true;
  }
  this->pic_ready_ = true;
}

bool SimpleVideoPlayer::pictures_held_() const {
  // A stream, a dashboard or a desktop session has the panel: nothing interrupts it, a picture
  // asked for meanwhile waits.
  if (this->is_network_session())
    return true;
#ifdef SVP_AUDIO_FILES
  if (this->audio_session_.load(std::memory_order_acquire))
    return false;  // music plays on with the pictures
#endif
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  return state == PlayerState::PLAYING || state == PlayerState::PAUSED;
}

void SimpleVideoPlayer::pictures_loop_() {
  if (!this->pic_ready_)
    return;
  const uint32_t now = millis();
  if (this->overlay_visible_ && now - this->overlay_shown_ms_ >= this->overlay_duration_ms_)
    this->hide_overlay_icon_();
  // The strip's hide changes LVGL objects: not while LVGL is paused (a video or a sender has the
  // panel); it follows once LVGL runs again.
  this->view_loop_();
  bool lvgl_runs = true;
#ifdef SVP_USE_LVGL
  lvgl_runs = this->lvgl_component_ == nullptr || !this->lvgl_component_->is_paused();
#endif
  if (lvgl_runs && this->thumb_auto_hide_ms_ > 0 && this->thumbs_visible_ && this->thumb_slide_enabled_ &&
      now - this->thumb_touched_ms_ >= this->thumb_auto_hide_ms_)
    this->slide_thumbnails_(false);
#ifdef SVP_CHANNEL_LIST
  // Home Assistant's picture fetched (or not): the read is done.
  if (this->pic_read_.active && this->pic_read_.remote && !this->pic_read_.done &&
      this->fetch_done_(this->pic_fetch_)) {
    PictureRead &read = this->pic_read_;
    read.length = this->pic_fetch_.ok ? this->pic_fetch_.len : 0;
    read.failed = !this->pic_fetch_.ok || read.length == 0 || read.length >= read.limit;
    read.done = true;
  }
#endif

#ifdef SVP_AUDIO_COVER
  // The picture task decodes cover art: no picture meanwhile, and the video waits for it.
  if (this->cover_step_ == CoverStep::DECODING)
    return;
#endif
  // Another user lacks room (the owner table): what is in flight finishes, a fetch of Home
  // Assistant's is dropped or stopped, then the pictures' and the cover art's buffers go.
  if (this->pool_wanted_(POOL_PICTURES)) {
    if (this->pic_read_.active) {
#ifdef SVP_CHANNEL_LIST
      if (this->pic_read_.remote && !this->pic_read_.decoding) {
        if (!this->fetch_drop_(this->pic_fetch_))
          return;  // stops after its block
        this->pic_read_.done = true;  // dropped below
      }
#endif
      const bool finished =
          this->pic_read_.decoding ? this->pic_decoded_.load(std::memory_order_acquire) : this->pic_read_.done;
      if (!finished)
        return;
      if (this->pic_read_.decoding && !this->pic_read_.thumbnail) {
        this->picture_decoded_();  // shown or kept
      } else {
        this->pic_read_ = PictureRead{};
        this->pic_decoded_.store(false, std::memory_order_relaxed);
      }
    }
#ifdef SVP_BACKGROUND
    // The page background: its read or decode finishes, then its pieces go (shown again later).
    if (this->bg_busy_())
      return;
    this->release_background_();
#endif
    if (this->view_step_ == ViewStep::DECODING)
      return;
    this->release_view_();
#ifdef SVP_AUDIO_COVER
    // Cover art being fetched into its buffer: dropped, or stopped after its block.
    if (this->cover_step_ == CoverStep::FETCHING && !this->fetch_drop_(this->cover_fetch_))
      return;
    this->release_cover_buffers_();
#endif
    this->release_picture_buffers_();
    this->pool_answer_(POOL_PICTURES);
  }

  // Closed: what is in flight finishes and is dropped, then the canvas empties and the buffers go
  // (LVGL running).
  if (this->pic_close_req_ && lvgl_runs) {
    if (this->pic_read_.active) {
      const bool finished =
          this->pic_read_.decoding ? this->pic_decoded_.load(std::memory_order_acquire) : this->pic_read_.done;
      if (!finished)
        return;
      this->pic_read_ = PictureRead{};
      this->pic_decoded_.store(false, std::memory_order_relaxed);
    }
    this->pic_close_req_ = false;
    this->release_picture_buffers_();
    this->pic_restore_ = false;
    this->highlight_thumbnail_(false);
  }

  // While a video plays nothing is read or decoded: the JPEG unit and the storage are its.
  if (this->pictures_held_())
    return;
  if (this->pic_restore_ && !this->pic_read_.active) {
    this->pic_restore_ = false;
    this->render_current_();
  }

  if (this->pic_read_.active) {
    if (this->pic_read_.decoding) {
      if (this->pic_decoded_.load(std::memory_order_acquire))
        this->picture_decoded_();
    } else if (this->pic_read_.done) {
      this->picture_read_done_();
    }
    return;
  }
  if (this->pic_shown_fullscreen_ != this->fullscreen_ && this->pic_wanted_ < 0) {
    // The picture of the new size did not come (not read or not decoded): the size on its own.
    this->apply_picture_size_();
    this->picture_callbacks_.call();
  }
  if (this->pic_list_pending_) {
    if (now - this->pic_list_retry_ms_ >= LIST_RETRY_MS)
      this->list_directory_();
    return;
  }
  if (this->slideshow_mode_ == SlideshowMode::PLAYING && this->pic_wanted_ < 0 &&
      now - this->last_slide_ms_ >= this->slideshow_interval_ms_) {
    this->last_slide_ms_ = now;
    this->next_image();
  }
  if (this->pic_wanted_ >= 0) {
    const size_t index = static_cast<size_t>(this->pic_wanted_);
    if (PicPage *page = this->page_of_(this->pic_wanted_)) {
      this->pic_wanted_ = -1;
      this->show_page_(page);
    } else if (PicFile *file = this->cached_file_(index)) {
      if (!this->start_decode_(false, index, 0, file))
        this->pic_wanted_ = -1;
    } else {
      this->start_read_(false, index, 0);
    }
    return;
  }
  while (!this->thumb_queue_.empty()) {
    const size_t slot = this->thumb_queue_.front();
    this->thumb_queue_.erase(this->thumb_queue_.begin());
    if (slot < this->thumb_slots_.size() && !this->thumb_slots_[slot].loaded && this->thumb_slots_[slot].image >= 0) {
      const size_t index = static_cast<size_t>(this->thumb_slots_[slot].image);
      if (PicFile *file = this->cached_file_(index)) {
        this->start_decode_(true, index, slot, file);
      } else {
        this->start_read_(true, index, slot);
      }
      return;
    }
  }
  this->preload_neighbours_();
}

//========================================================================
// Folders
//========================================================================

bool SimpleVideoPlayer::open_directory(const std::string &path) {
  std::string dir = path;
  while (dir.size() > 1 && dir.back() == '/')
    dir.pop_back();
  if (dir.empty() || dir == "/") {
    ESP_LOGW(TAG, "Not a picture folder: '%s'", path.c_str());
    return false;
  }
  this->pic_dir_ = dir;
  this->pic_dir_index_ = SIZE_MAX;
  for (size_t i = 0; i < this->pic_dirs_.size(); i++) {
    std::string configured = this->pic_dirs_[i].c_str();
    while (configured.size() > 1 && configured.back() == '/')
      configured.pop_back();
    if (configured == dir)
      this->pic_dir_index_ = i;
  }
  this->pic_list_pending_ = true;
  this->pic_list_retry_ms_ = millis() - LIST_RETRY_MS;
  return true;
}

bool SimpleVideoPlayer::set_current_directory(size_t index) {
  if (index >= this->pic_dirs_.size()) {
    ESP_LOGW(TAG, "No picture directory %u", static_cast<unsigned>(index));
    return false;
  }
  return this->open_directory(this->pic_dirs_[index].c_str());
}

void SimpleVideoPlayer::refresh_images() {
  if (this->pic_dir_.empty())
    return;
#ifdef SVP_CHANNEL_LIST
  if (this->pic_dir_.rfind("media-source://", 0) == 0)
    return;  // a Home Assistant folder: listed by ha_browse()
#endif
  if (this->pic_select_.empty()) {
    if (const ImageEntry *entry = this->get_current_image())
      this->pic_select_ = entry->path.c_str();
  }
  this->pic_list_pending_ = true;
  this->pic_list_retry_ms_ = millis() - LIST_RETRY_MS;
}

void SimpleVideoPlayer::list_directory_() {
  this->pic_list_retry_ms_ = millis();
  FolderListing &job = this->listings_->pictures;
  if (job.active || storage::global_storage_registry == nullptr)
    return;  // the one on its way first (its result is dropped: pic_list_pending_)
  const char *rel = nullptr;
  storage::PathStorage *storage = storage::global_storage_registry->resolve_path(this->pic_dir_.c_str(), &rel);
  if (storage == nullptr)
    return;  // not mounted yet: tried again
  const char *path = (rel == nullptr || rel[0] == '\0') ? "/" : rel;

  // Listed straight into images_ (the old list goes; a failed listing leaves none, pic_list_failed_).
  this->pic_generation_++;  // a read in flight belongs to the old list
  this->images_.clear();
  for (ThumbSlot &slot : this->thumb_slots_) {
    slot.image = -1;  // nothing carries over into the new list
    slot.loaded = false;
  }
  this->current_image_ = -1;
  this->pic_wanted_ = -1;
  job.player = this;
  job.dir = this->pic_dir_;
  job.first = 0;
  job.count = 0;
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (worker != nullptr &&
      worker->async_list_dir(storage, path, &SimpleVideoPlayer::picture_entry_, &job,
                             [this](storage::StorageError err) { this->pictures_listed_(err); }) ==
          storage::StorageError::STORAGE_ERROR_OK) {
    job.active = true;
    this->pic_list_pending_ = false;
    return;
  }
  // No worker, or its queue full: read here, unless the worker reads from it on its task.
  if (worker != nullptr && worker->has_active_task_io(storage))
    return;  // tried again
  job.active = true;
  this->pic_list_pending_ = false;
  this->pictures_listed_(storage->list_dir(path, &SimpleVideoPlayer::picture_entry_, &job));
}

// The worker's task (or here): one entry of the folder, into images_' next place.
bool SimpleVideoPlayer::picture_entry_(const storage::FileStat *entry, void *ctx) {
  auto *job = static_cast<FolderListing *>(ctx);
  PoolList<ImageEntry> &list = job->player->images_;
  runtime_image::ImageFormat format;
  if (entry->is_dir || entry->name[0] == '.' || !picture_format(entry->name, &format))
    return true;
  if (!SvpId::fits(job->dir.size() + 1 + std::strlen(entry->name))) {
    ESP_LOGW(TAG, "%s left out: its path is too long", entry->name);
    return true;
  }
  ImageEntry *at = list.place(job->first + job->count);
  if (at == nullptr)
    return false;
  ImageEntry *image = ::new (at) ImageEntry();
  image->path = job->dir;
  image->path += '/';
  image->path += entry->name;
  image->filename = entry->name;
  image->size = entry->size;
  job->count++;
  return job->first + job->count < list.capacity();
}

// Main loop: the folder listed.
void SimpleVideoPlayer::pictures_listed_(storage::StorageError err) {
  FolderListing &job = this->listings_->pictures;
  job.active = false;
#ifdef SVP_CHANNEL_LIST
  if (this->pic_ha_show_ >= 0) {
    // Home Assistant's pictures were asked for meanwhile: they take the list.
    const size_t index = static_cast<size_t>(this->pic_ha_show_);
    this->pic_ha_show_ = -1;
    this->ha_show_pictures(index);
    return;
  }
#endif
  if (this->pic_list_pending_)
    return;  // another folder (or this one anew) asked for meanwhile: listed next
  this->images_.adopt(job.count);
  this->pic_list_failed_ = err != storage::StorageError::STORAGE_ERROR_OK;
  if (this->pic_list_failed_) {
    this->images_.clear();
    ESP_LOGW(TAG, "Cannot list %s: %s", this->pic_dir_.c_str(), storage::error_to_string(err));
    this->picture_callbacks_.call();
    return;
  }
  sort_by_order(this->images_, this->images_order_, [](const ImageEntry &a, const ImageEntry &b) {
    return strcasecmp(a.filename.c_str(), b.filename.c_str()) < 0;
  });
  ESP_LOGI(TAG, "%u pictures in %s", static_cast<unsigned>(this->images_.size()), this->pic_dir_.c_str());
  this->picture_callbacks_.call();

  int show = -1;
  if (!this->pic_select_.empty()) {
    for (size_t i = 0; i < this->images_.size(); i++) {
      if (this->pic_select_ == this->images_[i].path.c_str())
        show = static_cast<int>(i);
    }
    this->pic_select_.clear();
  } else if (this->default_image_index_ >= 0 &&
             static_cast<size_t>(this->default_image_index_) < this->images_.size()) {
    show = this->default_image_index_;
  }
  this->place_thumbnail_window_(show >= 0 ? static_cast<size_t>(show) : 0);
  if (show >= 0)
    this->show_image(static_cast<size_t>(show));
}

//========================================================================
// Navigation, slideshow
//========================================================================

bool SimpleVideoPlayer::show_image(size_t index) {
  if (index >= this->images_.size()) {
    ESP_LOGW(TAG, "No picture %u (of %u)", static_cast<unsigned>(index), static_cast<unsigned>(this->images_.size()));
    return false;
  }
  if (this->app_ != AppKind::PICTURES && !this->app_take(AppKind::PICTURES))  // a video, a desktop session, the camera end
    return false;
  this->pic_wanted_ = static_cast<int>(index);
  return true;
}

bool SimpleVideoPlayer::show_image(const std::string &path) {
  for (size_t i = 0; i < this->images_.size(); i++) {
    if (path == this->images_[i].path.c_str() || path == this->images_[i].filename.c_str())
      return this->show_image(i);
  }
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) {
    ESP_LOGW(TAG, "Picture not found: %s", path.c_str());
    return false;
  }
  this->pic_select_ = path;
  return this->open_directory(path.substr(0, slash));
}

#ifdef SVP_CHANNEL_LIST
bool SimpleVideoPlayer::ha_show_pictures(size_t index) {
  if (!this->pic_ready_ || index >= this->ha_items_.size() || this->ha_items_[index].dir)
    return false;
  if (this->listings_->pictures.active) {
    // A local folder is listed into images_ meanwhile: these take it once that is done.
    this->pic_ha_show_ = static_cast<int>(index);
    return true;
  }
  size_t before = 0;  // the pictures listed before it
  for (size_t i = 0; i < index; i++)
    before += this->ha_items_[i].dir ? 0 : 1;
  if (before >= MAX_IMAGES)
    return false;
  this->pic_generation_++;  // a read in flight belongs to the old list
  this->images_.clear();
  int show = -1;
  for (size_t i = 0; i < this->ha_items_.size() && !this->images_.full(); i++) {
    const HaItem &item = this->ha_items_[i];
    if (item.dir)
      continue;
    if (i == index)
      show = static_cast<int>(this->images_.size());
    this->images_.push_back(ImageEntry{item.id, item.title, 0});
  }
  this->pic_dir_ = this->ha_id().c_str();
  this->pic_dir_index_ = SIZE_MAX;
  this->pic_list_pending_ = false;
  this->pic_list_failed_ = false;
  this->pic_select_.clear();
  for (ThumbSlot &slot : this->thumb_slots_) {
    slot.image = -1;
    slot.loaded = false;
  }
  this->current_image_ = -1;
  this->pic_wanted_ = -1;
  this->picture_callbacks_.call();
  this->place_thumbnail_window_(static_cast<size_t>(show));
  return this->show_image(static_cast<size_t>(show));
}
#endif

bool SimpleVideoPlayer::next_image() {
  if (this->images_.empty())
    return false;
  this->pic_direction_ = 1;
  const int base = this->pic_wanted_ >= 0 ? this->pic_wanted_ : this->current_image_;
  return this->show_image(static_cast<size_t>((base + 1) % static_cast<int>(this->images_.size())));
}

void SimpleVideoPlayer::close_picture() {
  if (!this->pic_ready_)
    return;
  this->app_release(AppKind::PICTURES);
  this->slideshow_mode_ = SlideshowMode::STOPPED;
  this->overlay_visible_ = false;
  this->pic_wanted_ = -1;
  this->current_image_ = -1;
  this->pic_restore_ = false;
  this->pic_close_req_ = true;
  this->picture_callbacks_.call();
}

bool SimpleVideoPlayer::previous_image() {
  if (this->images_.empty())
    return false;
  this->pic_direction_ = -1;
  const int base = this->pic_wanted_ >= 0 ? this->pic_wanted_ : this->current_image_;
  const int count = static_cast<int>(this->images_.size());
  return this->show_image(static_cast<size_t>(base <= 0 ? count - 1 : base - 1));
}

size_t SimpleVideoPlayer::slideshow_step_index(uint32_t ms) {
  size_t best = 0;
  for (size_t i = 1; i < SLIDESHOW_STEP_COUNT; i++) {
    const auto diff = [ms](size_t index) {
      const int64_t d = static_cast<int64_t>(SLIDESHOW_STEPS_S[index]) * 1000 - ms;
      return d < 0 ? -d : d;
    };
    if (diff(i) < diff(best))
      best = i;
  }
  return best;
}

const char *SimpleVideoPlayer::slideshow_step_text(size_t index) {
  static const char *const TEXTS[] = {"3 s", "5 s", "8 s", "10 s", "15 s", "20 s", "30 s", "60 s"};
  static_assert(sizeof(TEXTS) / sizeof(TEXTS[0]) == SLIDESHOW_STEP_COUNT, "a text per step");
  return index < SLIDESHOW_STEP_COUNT ? TEXTS[index] : "";
}

std::string SimpleVideoPlayer::slideshow_step_options() {
  std::string options;
  for (size_t i = 0; i < SLIDESHOW_STEP_COUNT; i++) {
    if (i != 0)
      options += '\n';
    options += slideshow_step_text(i);
  }
  return options;
}

void SimpleVideoPlayer::start_slideshow() {
  if (this->images_.empty()) {
    ESP_LOGW(TAG, "No pictures for a slideshow");
    return;
  }
  this->slideshow_mode_ = SlideshowMode::PLAYING;
  this->last_slide_ms_ = millis();
  if (this->current_image_ < 0 && this->pic_wanted_ < 0)
    this->show_image(static_cast<size_t>(0));
  this->show_overlay_icon_(true);
  this->picture_callbacks_.call();
}

void SimpleVideoPlayer::stop_slideshow() {
  const bool was_playing = this->slideshow_mode_ == SlideshowMode::PLAYING;
  this->slideshow_mode_ = SlideshowMode::STOPPED;
  if (was_playing)
    this->show_overlay_icon_(false);
  this->picture_callbacks_.call();
}

void SimpleVideoPlayer::pause_slideshow() {
  this->slideshow_mode_ = SlideshowMode::PAUSED;
  this->picture_callbacks_.call();
}

void SimpleVideoPlayer::toggle_slideshow() {
  if (this->slideshow_mode_ == SlideshowMode::PLAYING) {
    this->pause_slideshow();
    this->show_overlay_icon_(false);
  } else {
    this->start_slideshow();
  }
}

//========================================================================
// Reading through the storage worker: each file whole, in one request
//========================================================================

SimpleVideoPlayer::PicFile *SimpleVideoPlayer::cached_file_(size_t index) {
  for (PicFile &file : this->pic_files_) {
    if (file.index == static_cast<int>(index) && file.generation == this->pic_generation_ && file.length != 0)
      return &file;
  }
  return nullptr;
}

// A file to read into: an empty or stale one, else the one farthest from the shown picture that is
// neither shown nor asked for.
SimpleVideoPlayer::PicFile *SimpleVideoPlayer::free_file_() {
  const int count = static_cast<int>(this->images_.size());
  PicFile *best = nullptr;
  int best_distance = -1;
  for (PicFile &file : this->pic_files_) {
    int distance = INT32_MAX;
    if (file.index >= 0 && file.generation == this->pic_generation_ && file.length != 0) {
      if (file.index == this->current_image_ || file.index == this->pic_wanted_)
        continue;
      if (this->current_image_ >= 0 && count > 0) {
        const int d = std::abs(file.index - this->current_image_);
        distance = std::min(d, count - d);
      }
    }
    if (distance > best_distance) {
      best = &file;
      best_distance = distance;
    }
  }
  return best;
}

// From the pool; when it is full, the video's idle buffers make room (taken again by the next video).
uint8_t *SimpleVideoPlayer::picture_alloc_(size_t size) { return this->pool_take_(size, POOL_USER_PICTURES); }

// The pictures' buffers, all at once when pictures are first wanted (after boot or a video): the
// pages and the files (undecoded). A larger file takes more when it comes; the decode buffer for a
// picture larger than its page only while one is decoded (reserve_decode_()).
bool SimpleVideoPlayer::acquire_picture_buffers_() {
  if (this->pic_pages_[0].data != nullptr)
    return true;
  if (this->pic_buffers_failed_)
    return false;
  bool ok = true;
  for (PicPage &page : this->pic_pages_) {
    page = PicPage{};
    page.data = ok ? this->picture_alloc_(this->pic_page_bytes_) : nullptr;
    page.capacity = page.data != nullptr ? this->pic_page_bytes_ : 0;
    ok = ok && page.data != nullptr;
  }
  for (PicFile &file : this->pic_files_) {
    if (ok && file.data == nullptr) {
      file.data = this->picture_alloc_(PICTURE_STEP);
      file.capacity = file.data != nullptr ? PICTURE_STEP : 0;
    }
    ok = ok && file.data != nullptr;
  }
  if (ok)
    return true;
  ESP_LOGE(TAG, "No room in the PSRAM pool for the pictures (%u bytes)",
           static_cast<unsigned>(PIC_PAGE_COUNT * this->pic_page_bytes_ + PIC_FILE_COUNT * PICTURE_STEP));
  this->release_picture_buffers_();
  this->pic_buffers_failed_ = true;
  return false;
}

// Kept for the next picture; replaced only by a larger one (in whole PICTURE_STEPs).
bool SimpleVideoPlayer::reserve_file_(PicFile *file, size_t size) {
  if (!this->acquire_picture_buffers_())
    return false;
  if (file->capacity >= size)
    return true;
  this->pool_.free(file->data);
  file->data = nullptr;
  file->capacity = 0;
  file->index = -1;
  file->length = 0;
  const size_t capacity = align_up(size, PICTURE_STEP);
  file->data = this->picture_alloc_(capacity);
  if (file->data == nullptr)
    return false;
  file->capacity = capacity;
  return true;
}

bool SimpleVideoPlayer::reserve_decode_(size_t size) {
  if (!this->acquire_picture_buffers_())
    return false;
  if (this->pic_decode_cap_ >= size)
    return true;
  this->pool_.free(this->pic_decode_buf_);
  this->pic_decode_buf_ = nullptr;
  this->pic_decode_cap_ = 0;
  const size_t capacity = align_up(size, PICTURE_STEP);
  this->pic_decode_buf_ = this->picture_alloc_(capacity);
  if (this->pic_decode_buf_ == nullptr)
    return false;
  this->pic_decode_cap_ = capacity;
  return true;
}

// Main loop, nothing decoded meanwhile.
void SimpleVideoPlayer::release_decode_() {
  this->pool_.free(this->pic_decode_buf_);
  this->pic_decode_buf_ = nullptr;
  this->pic_decode_cap_ = 0;
}

// Main loop, nothing read or decoded meanwhile: the pictures' pool buffers for the video.
void SimpleVideoPlayer::release_picture_buffers_() {
  this->pic_buffers_failed_ = false;
  if (this->pic_shown_ != nullptr) {
    // Meanwhile the canvas shows its own buffer, black, or nothing (external_buffer); the picture
    // comes back after the video.
    if (this->pic_lvgl_buf_ != nullptr) {
      lv_canvas_set_draw_buf(this->pic_canvas_, this->pic_lvgl_buf_);
      lv_canvas_fill_bg(this->pic_canvas_, lv_color_black(), LV_OPA_COVER);
    } else {
      lv_image_set_src(this->pic_canvas_, nullptr);
    }
    this->pic_shown_ = nullptr;
    this->pic_restore_ = this->current_image_ >= 0;
  }
  for (PicPage &page : this->pic_pages_) {
    this->pool_.free(page.data);
    page = PicPage{};
  }
  for (PicFile &file : this->pic_files_) {
    this->pool_.free(file.data);
    file = PicFile{};
  }
  this->pool_.free(this->pic_decode_buf_);
  this->pic_decode_buf_ = nullptr;
  this->pic_decode_cap_ = 0;
}

void SimpleVideoPlayer::start_read_(bool thumbnail, size_t index, size_t slot, bool preload) {
  if (index >= this->images_.size()) {
    if (!thumbnail && !preload)
      this->pic_wanted_ = -1;
    return;
  }
  const ImageEntry &entry = this->images_[index];
#ifdef SVP_CHANNEL_LIST
  if (remote_image_(entry)) {
    // Home Assistant's: fetched by the channel task, the relay fitting it into the file's room.
    PicFile *file = this->free_file_();
    if (file == nullptr || !this->reserve_file_(file, PICTURE_STEP)) {
      ESP_LOGW(TAG, "%s: no room in the PSRAM pool for the file", entry.filename.c_str());
      if (!thumbnail && !preload)
        this->pic_wanted_ = -1;
      return;
    }
    file->index = -1;
    file->length = 0;
    PictureRead &read = this->pic_read_;
    read = PictureRead{};
    read.active = true;
    read.thumbnail = thumbnail;
    read.preload = preload;
    read.index = index;
    read.slot = slot;
    read.generation = this->pic_generation_;
    read.file = file;
    read.remote = true;
    read.limit = file->capacity - 16;  // room past it for the decoder, as with a file
    xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
    this->ha_texts_->picture_id = entry.path;
    xSemaphoreGive(this->channel_lock_);
    if (!this->fetch_start_(this->pic_fetch_, CHANNEL_CMD_IMAGE, file->data, read.limit)) {
      read.failed = true;  // the task's queue is full: not read this time
      read.done = true;
    }
    return;
  }
#endif
  const char *why = nullptr;
  const char *rel = nullptr;
  storage::PathStorage *storage = nullptr;
  PicFile *file = this->free_file_();
  // Room up to a multiple of 16 past the file: the decoder is given that much (as the video's).
  const size_t size = static_cast<size_t>(entry.size) + 16;
  if (storage::global_storage_registry == nullptr || storage::global_storage_worker == nullptr) {
    why = "no storage";
  } else if ((storage = storage::global_storage_registry->resolve_path(entry.path.c_str(), &rel)) == nullptr ||
             rel == nullptr || rel[0] == '\0') {
    why = "storage not mounted";
  } else if (file == nullptr || !this->reserve_file_(file, size)) {
    why = "no room in the PSRAM pool for the file";
  }
  if (why != nullptr) {
    ESP_LOGW(TAG, "%s: %s", entry.path.c_str(), why);
    if (!thumbnail && !preload)
      this->pic_wanted_ = -1;
    return;
  }

  file->index = -1;  // being read
  file->length = 0;
  PictureRead &read = this->pic_read_;
  read = PictureRead{};
  read.active = true;
  read.thumbnail = thumbnail;
  read.preload = preload;
  read.index = index;
  read.slot = slot;
  read.generation = this->pic_generation_;
  read.file = file;
  read.limit = static_cast<size_t>(entry.size) + 1;
  const storage::StorageError err =
      storage::global_storage_worker->begin_read(storage, rel, &read.handle, [this](storage::StorageError result) {
        if (result != storage::StorageError::STORAGE_ERROR_OK) {
          this->pic_read_.failed = true;
          this->finish_read_();
          return;
        }
        this->read_next_chunk_();
      });
  if (err == storage::StorageError::STORAGE_ERROR_OK)
    return;
  read.active = false;
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    // All streams busy: the same file next loop.
    if (thumbnail)
      this->thumb_queue_.insert(this->thumb_queue_.begin(), slot);
    return;
  }
  ESP_LOGW(TAG, "%s: %s", entry.path.c_str(), storage::error_to_string(err));
  if (!thumbnail && !preload)
    this->pic_wanted_ = -1;
}

// The rest of the file in one request; another only when the storage returned less.
void SimpleVideoPlayer::read_next_chunk_() {
  PictureRead &read = this->pic_read_;
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      read.handle, read.file->data + read.length, read.limit - read.length, &read.chunk,
      [this](storage::StorageError result) {
        PictureRead &read = this->pic_read_;
        if (result != storage::StorageError::STORAGE_ERROR_OK) {
          read.failed = true;
          this->finish_read_();
          return;
        }
        read.length += read.chunk;
        if (read.chunk == 0 || read.length + 1 == read.limit) {
          this->finish_read_();  // end of file, or all of it
        } else if (read.length == read.limit) {
          read.failed = true;  // larger than when it was listed
          this->finish_read_();
        } else {
          this->read_next_chunk_();
        }
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    read.failed = true;
    this->finish_read_();
  }
}

void SimpleVideoPlayer::finish_read_() {
  const storage::StorageError err = storage::global_storage_worker->end_read(
      this->pic_read_.handle, [this](storage::StorageError) { this->pic_read_.done = true; });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->pic_read_.done = true;
}

void SimpleVideoPlayer::picture_read_done_() {
  const PictureRead read = this->pic_read_;
  this->pic_read_ = PictureRead{};
  if (read.generation != this->pic_generation_ || read.index >= this->images_.size())
    return;
  const ImageEntry &entry = this->images_[read.index];
  if (read.failed || read.length == 0) {
    ESP_LOGW(TAG, "Cannot read %s", entry.path.c_str());
    if (!read.thumbnail && !read.preload && this->pic_wanted_ == static_cast<int>(read.index))
      this->pic_wanted_ = -1;
    return;
  }
  PicFile *file = read.file;
  file->index = static_cast<int>(read.index);
  file->generation = read.generation;
  file->length = read.length;
  file->used_ms = millis();
  if (read.preload)
    return;  // kept undecoded until it is shown
  if (read.thumbnail) {
    if (read.slot < this->thumb_slots_.size() && this->thumb_slots_[read.slot].image == static_cast<int>(read.index))
      this->start_decode_(true, read.index, read.slot, file);
    return;
  }
  // Shown unless another picture was asked for meanwhile (this one stays read).
  if (this->pic_wanted_ == static_cast<int>(read.index))
    this->start_decode_(false, read.index, 0, file);
}

// The files of the pictures next to the shown one, read ahead (undecoded) while nothing else is to
// do: the one in the last step's direction first.
void SimpleVideoPlayer::preload_neighbours_() {
  const int count = static_cast<int>(this->images_.size());
  if (this->current_image_ < 0 || count < 2)
    return;
  while (this->pic_preload_tries_ < 2) {
    const int step = this->pic_preload_tries_++ == 0 ? this->pic_direction_ : -this->pic_direction_;
    const int index = (this->current_image_ + step + count) % count;
    if (this->page_of_(index) != nullptr)
      continue;
    if (this->cached_file_(static_cast<size_t>(index)) != nullptr)
      continue;
    this->start_read_(false, static_cast<size_t>(index), 0, true);
    return;
  }
}

// The page holding a picture at the canvas' current size.
SimpleVideoPlayer::PicPage *SimpleVideoPlayer::page_of_(int index) {
  for (PicPage &page : this->pic_pages_) {
    if (page.index == index && page.generation == this->pic_generation_ && page.w == this->pic_w_ &&
        page.h == this->pic_h_)
      return &page;
  }
  return nullptr;
}

// A page to decode into, at the canvas' current size: never the shown one; an empty or outdated one
// first, else the one farthest from the shown picture.
SimpleVideoPlayer::PicPage *SimpleVideoPlayer::free_page_() {
  if (!this->acquire_picture_buffers_())
    return nullptr;
  const int count = static_cast<int>(this->images_.size());
  PicPage *best = nullptr;
  int best_distance = -1;
  for (PicPage &page : this->pic_pages_) {
    if (&page == this->pic_shown_)
      continue;
    int distance = INT32_MAX;
    if (page.index >= 0 && page.generation == this->pic_generation_ && page.w == this->pic_w_ &&
        page.h == this->pic_h_ && this->current_image_ >= 0 && count > 0) {
      const int d = std::abs(page.index - this->current_image_);
      distance = std::min(d, count - d);
    }
    if (distance > best_distance) {
      best = &page;
      best_distance = distance;
    }
  }
  // At the display's size: any canvas fits.
  if (best == nullptr || best->capacity < static_cast<size_t>(this->pic_stride_) * this->pic_h_)
    return nullptr;
  best->index = -1;
  best->w = this->pic_w_;
  best->h = this->pic_h_;
  best->stride = this->pic_stride_;
  return best;
}

//========================================================================
// Decoding, on the picture task
//========================================================================

// The decoder writes whole MCUs (esp_driver_jpeg jpeg_parse_marker.c, process_h/process_v): 16
// pixels wide for 4:2:2 and 4:2:0, 16 high for 4:2:0, else 8. RGB565.
static size_t jpeg_decoded_size(const jpeg_decode_picture_info_t &info, uint32_t *padded_w) {
  const bool wide = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 || info.sample_method == JPEG_DOWN_SAMPLING_YUV420;
  const uint32_t pw = align_up(info.width, wide ? 16 : 8);
  const uint32_t ph = align_up(info.height, info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8);
  if (padded_w != nullptr)
    *padded_w = pw;
  return static_cast<size_t>(pw) * ph * 2;
}

// Output buffers by DMA: start and size on whole cache lines (the pool's ALIGN covers the P4's).
static size_t dma_usable(size_t capacity) { return capacity & ~(PsramPool::ALIGN - 1); }

bool SimpleVideoPlayer::start_decode_(bool thumbnail, size_t index, size_t slot, PicFile *file, bool preload) {
  PictureJob &job = this->pic_job_;
  job = PictureJob{};
  job.thumbnail = thumbnail;
  job.data = file->data;
  job.length = file->length;
  job.path = this->images_[index].path.c_str();
  if (thumbnail) {
    job.target = this->thumb_slots_[slot].data;
    job.target_capacity = static_cast<size_t>(this->thumb_stride_) * this->thumb_h_;
    job.target_w = this->thumb_w_;
    job.target_h = this->thumb_h_;
    job.target_stride = this->thumb_stride_;
    job.fit = ImageFitMode::SCALE_TO_FIT;
  } else {
    PicPage *page = this->free_page_();
    if (page == nullptr) {
      if (!preload)
        ESP_LOGW(TAG, "%s: no room in the PSRAM pool for a %dx%d picture", job.path.c_str(),
                 static_cast<int>(this->pic_w_), static_cast<int>(this->pic_h_));
      return false;
    }
    if (!preload && this->pic_wanted_ == static_cast<int>(index))
      this->pic_wanted_ = -1;
    job.page = page;
    job.target = page->data;
    job.target_capacity = page->capacity;
    job.target_w = page->w;
    job.target_h = page->h;
    job.target_stride = page->stride;
    job.fit = this->fit_mode_;
  }
  // A JPEG that does not go straight into its target (a thumbnail never does) is decoded whole
  // first: the buffer for that is reserved here, on the main loop.
  runtime_image::ImageFormat format;
  jpeg_decode_picture_info_t info{};
  if (picture_format(job.path, &format) && format == runtime_image::JPEG &&
      jpeg_decoder_get_info(job.data, static_cast<uint32_t>(job.length), &info) == ESP_OK && info.width != 0 &&
      info.height != 0) {
    const size_t bytes = jpeg_decoded_size(info, nullptr);
    const bool straight = !thumbnail && static_cast<int32_t>(info.width) <= job.target_w &&
                          static_cast<int32_t>(info.height) <= job.target_h && bytes <= dma_usable(job.target_capacity);
    if (!straight) {
      if (!this->reserve_decode_(bytes)) {
        ESP_LOGW(TAG, "%s: %ux%u needs %u bytes of the PSRAM pool to decode", job.path.c_str(),
                 static_cast<unsigned>(info.width), static_cast<unsigned>(info.height), static_cast<unsigned>(bytes));
        return false;
      }
      job.scratch = this->pic_decode_buf_;
      job.scratch_capacity = this->pic_decode_cap_;
    }
  }
  file->used_ms = millis();
  PictureRead &read = this->pic_read_;
  read = PictureRead{};
  read.active = true;
  read.decoding = true;
  read.thumbnail = thumbnail;
  read.preload = preload;
  read.index = index;
  read.slot = slot;
  read.generation = this->pic_generation_;
  read.file = file;
  this->pic_decoded_.store(false, std::memory_order_relaxed);
  xTaskNotifyGive(this->pic_task_);
  return true;
}

void SimpleVideoPlayer::picture_task_entry_(void *arg) {
  auto *self = static_cast<SimpleVideoPlayer *>(arg);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (self->view_job_.exchange(false, std::memory_order_acq_rel)) {
      self->decode_job_(self->view_job_data_);
      self->view_decoded_.store(true, std::memory_order_release);
      continue;
    }
#ifdef SVP_BACKGROUND
    if (self->bg_job_.exchange(false, std::memory_order_acq_rel)) {
      self->decode_job_(self->bg_job_data_);
      self->bg_decoded_.store(true, std::memory_order_release);
      continue;
    }
#endif
#ifdef SVP_AUDIO_COVER
    if (self->cover_job_.exchange(false, std::memory_order_acq_rel)) {
      self->cover_decode_();
      self->cover_decoded_.store(true, std::memory_order_release);
      continue;
    }
#endif
    if (self->cam_job_.exchange(false, std::memory_order_acq_rel)) {
      self->camera_decode_();
      self->cam_decoded_.store(true, std::memory_order_release);
      self->enable_loop_soon_any_context();  // done() on the main loop
      continue;
    }
    self->decode_job_(self->pic_job_);
    self->pic_decoded_.store(true, std::memory_order_release);
  }
}

// Nearest neighbour, 16.16 fixed point, into all of the target as the fit mode says; black around.
static void scale_rgb565(const uint8_t *src, int32_t iw, int32_t ih, uint32_t src_stride, uint8_t *dst, int32_t cw,
                         int32_t ch, uint32_t stride, ImageFitMode fit) {
  int32_t dw = iw;
  int32_t dh = ih;
  switch (fit) {
    case ImageFitMode::SCALE_TO_FIT:
    case ImageFitMode::SCALE_TO_FILL: {
      const float sx = static_cast<float>(cw) / static_cast<float>(iw);
      const float sy = static_cast<float>(ch) / static_cast<float>(ih);
      const float scale = fit == ImageFitMode::SCALE_TO_FIT ? std::min(sx, sy) : std::max(sx, sy);
      dw = std::max<int32_t>(1, static_cast<int32_t>(static_cast<float>(iw) * scale));
      dh = std::max<int32_t>(1, static_cast<int32_t>(static_cast<float>(ih) * scale));
      break;
    }
    case ImageFitMode::STRETCH:
      dw = cw;
      dh = ch;
      break;
    case ImageFitMode::CENTER:
      break;
  }
  const int32_t dx = (cw - dw) / 2;
  const int32_t dy = (ch - dh) / 2;
  if (fit != ImageFitMode::STRETCH) {
    for (int32_t y = 0; y < ch; y++)
      memset(dst + static_cast<size_t>(y) * stride, 0, static_cast<size_t>(cw) * 2);
  }
  const int32_t x0 = std::max<int32_t>(dx, 0);
  const int32_t x1 = std::min<int32_t>(dx + dw, cw);
  const int32_t y0 = std::max<int32_t>(dy, 0);
  const int32_t y1 = std::min<int32_t>(dy + dh, ch);
  const uint64_t step_x = (static_cast<uint64_t>(iw) << 16) / static_cast<uint64_t>(dw);
  const uint64_t step_y = (static_cast<uint64_t>(ih) << 16) / static_cast<uint64_t>(dh);
  for (int32_t y = y0; y < y1; y++) {
    const int32_t sy = std::min<int32_t>(static_cast<int32_t>((static_cast<uint64_t>(y - dy) * step_y) >> 16), ih - 1);
    const auto *src_row = reinterpret_cast<const uint16_t *>(src + static_cast<size_t>(sy) * src_stride);
    auto *dst_row = reinterpret_cast<uint16_t *>(dst + static_cast<size_t>(y) * stride);
    if (dw == iw) {
      memcpy(dst_row + x0, src_row + (x0 - dx), static_cast<size_t>(x1 - x0) * 2);
      continue;
    }
    uint64_t acc = static_cast<uint64_t>(x0 - dx) * step_x;
    for (int32_t x = x0; x < x1; x++, acc += step_x)
      dst_row[x] = src_row[std::min<int32_t>(static_cast<int32_t>(acc >> 16), iw - 1)];
  }
}

// A page drawn by the CPU goes out to PSRAM, as LVGL may blit it by DMA.
static void written_by_cpu(uint8_t *page, size_t capacity) {
  if (page != nullptr)
    esp_cache_msync(page, dma_usable(capacity), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

bool SimpleVideoPlayer::show_camera_frame(const uint8_t *data, size_t length, CameraFrameFormat format,
                                          uint16_t width, uint16_t height, uint8_t *target, size_t target_capacity,
                                          int32_t target_w, int32_t target_h, std::function<void(bool)> &&done) {
  if (this->pic_task_ == nullptr || this->cam_busy_ || data == nullptr || length == 0 ||
      (target == nullptr && format != CameraFrameFormat::MJPEG))
    return false;
#ifndef SVP_CAPTURE
  if (format != CameraFrameFormat::MJPEG)
    return false;  // the PPA client is screen capture's
#endif
  uint8_t slot = 0;
  if (format == CameraFrameFormat::MJPEG) {
    // A frame larger than the canvas needs the scratch piece of its decoded size (kept while the picture shows)
    jpeg_decode_picture_info_t info{};
    if (this->hw_jpeg_decoder_ == nullptr ||
        jpeg_decoder_get_info(data, static_cast<uint32_t>(length), &info) != ESP_OK || info.width == 0 ||
        info.height == 0)
      return false;
    uint32_t pw = 0;
    const size_t bytes = jpeg_decoded_size(info, &pw);
    const bool direct = target != nullptr && info.width <= static_cast<uint32_t>(target_w) &&
                        info.height <= static_cast<uint32_t>(target_h) && pw == static_cast<uint32_t>(target_w) &&
                        bytes <= dma_usable(target_capacity);
    // No target: the piece the reader does not hold (both held: the frame is not taken); a target: the first
    slot = 0;
    if (target == nullptr) {
      if ((this->cam_held_ & 1u) == 0) {
        slot = 0;
      } else if ((this->cam_held_ & 2u) == 0) {
        slot = 1;
      } else {
        return false;
      }
    }
    if (!direct && bytes > dma_usable(this->cam_scratch_capacity_[slot])) {
      if (this->cam_scratch_[slot] != nullptr)
        this->pool_.free(this->cam_scratch_[slot]);
      this->cam_scratch_capacity_[slot] = align_up(bytes, PsramPool::ALIGN);
      this->cam_scratch_[slot] = this->pool_.alloc(this->cam_scratch_capacity_[slot]);
      if (this->cam_scratch_[slot] == nullptr) {
        ESP_LOGW(TAG, "No room in the pool for the camera's %u byte picture", static_cast<unsigned>(bytes));
        this->cam_scratch_capacity_[slot] = 0;
        return false;
      }
    }
  }
  CameraJob &job = this->cam_job_data_;
  job.data = data;
  job.length = length;
  job.format = format;
  job.width = width;
  job.height = height;
  job.target = target;
  job.target_capacity = target_capacity;
  job.target_w = target_w;
  job.target_h = target_h;
  job.ok = false;
  job.slot = slot;
  this->cam_done_ = std::move(done);
  this->cam_busy_ = true;
  this->cam_decoded_.store(false, std::memory_order_relaxed);
  this->cam_job_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->pic_task_);
  return true;
}

#ifdef SVP_DSI_OUTPUT
bool SimpleVideoPlayer::camera_direct_start() {
  if (this->dsi_ == nullptr || !this->direct_flips_() || this->pic_task_ == nullptr)
    return false;
  this->cam_direct_touch_ = false;
  this->cam_direct_.store(true, std::memory_order_release);
  return true;
}

void SimpleVideoPlayer::camera_direct_stop() {
  this->cam_direct_.store(false, std::memory_order_release);
  this->cam_direct_ready_.store(false, std::memory_order_release);
}

bool SimpleVideoPlayer::show_camera_frame_direct(const uint8_t *data, size_t length, CameraFrameFormat format,
                                                 uint16_t width, uint16_t height, std::function<void(bool)> &&done) {
  if (!this->cam_direct_ready_.load(std::memory_order_acquire) || this->pic_task_ == nullptr || this->cam_busy_ ||
      data == nullptr || length == 0)
    return false;
#ifndef SVP_CAPTURE
  if (format != CameraFrameFormat::MJPEG)
    return false;  // copied by the PPA: screen capture's client
#endif
  uint16_t w = width, h = height;
  if (format == CameraFrameFormat::MJPEG) {
    jpeg_decode_picture_info_t info{};
    if (this->hw_jpeg_decoder_ == nullptr ||
        jpeg_decoder_get_info(data, static_cast<uint32_t>(length), &info) != ESP_OK || info.width == 0 ||
        info.height == 0)
      return false;
    uint32_t pw = 0;
    const size_t bytes = jpeg_decoded_size(info, &pw);
    w = static_cast<uint16_t>(info.width);
    h = static_cast<uint16_t>(info.height);
    // Rows the panel's own width and no padded rows: decoded straight into the scanout buffer; else via the scratch
    const bool straight = pw == this->dsi_out_w_ && h <= this->dsi_out_h_ && bytes == static_cast<size_t>(pw) * h * 2;
    if (!straight && bytes > dma_usable(this->cam_scratch_capacity_[0])) {
      if (this->cam_scratch_[0] != nullptr)
        this->pool_.free(this->cam_scratch_[0]);
      this->cam_scratch_capacity_[0] = align_up(bytes, PsramPool::ALIGN);
      this->cam_scratch_[0] = this->pool_.alloc(this->cam_scratch_capacity_[0]);
      if (this->cam_scratch_[0] == nullptr) {
        ESP_LOGW(TAG, "No room in the pool for the camera's %u byte picture", static_cast<unsigned>(bytes));
        this->cam_scratch_capacity_[0] = 0;
        return false;
      }
    }
  }
  CameraJob &job = this->cam_job_data_;
  job = CameraJob{};
  job.data = data;
  job.length = length;
  job.format = format;
  job.width = w;
  job.height = h;
  job.direct = true;
  this->cam_done_ = std::move(done);
  this->cam_busy_ = true;
  this->cam_decoded_.store(false, std::memory_order_relaxed);
  this->cam_job_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->pic_task_);
  return true;
}

// Picture task (Core 1, as the video's decode): the picture 1:1, centred, black around, in the buffer not shown
bool SimpleVideoPlayer::camera_direct_draw_() {
  CameraJob &job = this->cam_job_data_;
  if (!this->cam_direct_ready_.load(std::memory_order_acquire))
    return false;
  size_t len = 0;
  uint8_t *fb = this->direct_target_(true, &len);  // after the last flip
  void *fbs[2]{};
  display::ColorBitness bitness{};
  if (fb == nullptr || fb == this->decode_target_ || this->dsi_->get_scanout_buffers(fbs, 2, &bitness) != 2)
    return false;
  const uint8_t index = fb == fbs[0] ? 0 : 1;
  const uint32_t W = this->dsi_out_w_, H = this->dsi_out_h_;
  const size_t bpp = this->dsi_fb_bpp_;
  const size_t stride = static_cast<size_t>(W) * bpp;
  const size_t fb_size = stride * H;
  // Black once per buffer and picture size (the picture covers the same place in every frame)
  const uint32_t key = (static_cast<uint32_t>(job.width) << 16) | job.height;
  if (this->cam_direct_key_ != key) {
    this->cam_direct_key_ = key;
    this->cam_direct_black_ = 0;
  }
  if ((this->cam_direct_black_ & (1u << index)) == 0) {
    memset(fb, 0, fb_size);
    esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                     ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    this->cam_direct_black_ |= static_cast<uint8_t>(1u << index);
  }
  // 1:1 in the panel's orientation, centred, cut when larger; even (YUV pixel pairs, decoder rows on cache lines)
  const uint32_t step = std::max<uint32_t>(
      1, std::min<uint32_t>(16, std::min<uint32_t>(W * 16 / job.width, H * 16 / job.height)));
  const bool scaled = step < 16;
  const float scale = static_cast<float>(step) / 16.0f;
  const uint32_t ow = (scaled ? job.width * step / 16 : std::min<uint32_t>(job.width, W)) & ~1u;
  const uint32_t oh = scaled ? job.height * step / 16 : std::min<uint32_t>(job.height, H);
  const uint32_t sx = scaled ? 0 : ((job.width - ow) / 2) & ~1u;
  const uint32_t sy = scaled ? 0 : (job.height - oh) / 2;
  const uint32_t in_w = scaled ? job.width & ~1u : ow;
  const uint32_t in_h = scaled ? job.height : oh;
  const uint32_t dx = ((W - ow) / 2) & ~1u;
  const uint32_t dy = ((H - oh) / 2) & ~1u;
  bool ok = false;
  if (job.format == CameraFrameFormat::MJPEG) {
    jpeg_decode_picture_info_t info{};
    if (jpeg_decoder_get_info(job.data, static_cast<uint32_t>(job.length), &info) != ESP_OK)
      return false;
    uint32_t pw = 0;
    const size_t bytes = jpeg_decoded_size(info, &pw);
    jpeg_decode_cfg_t cfg{};
    cfg.output_format = bpp == 3 ? JPEG_DECODE_OUT_FORMAT_RGB888 : JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian, as the panel's buffers
    cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    const uint32_t stream = static_cast<uint32_t>(align_up(job.length, 16));
    uint32_t out = 0;
    if (!scaled && pw == W && bpp == 2 && bytes == static_cast<size_t>(pw) * info.height * 2 && info.height <= H) {
      // The rows are the panel's: straight in at the picture's first row (on whole cache lines: dy even)
      uint8_t *dest = fb + static_cast<size_t>(dy) * stride;
      const size_t room = dma_usable(fb_size - static_cast<size_t>(dy) * stride);
      ok = jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, job.data, stream, dest, static_cast<uint32_t>(room),
                                &out) == ESP_OK &&
           out != 0;
    } else {
      uint8_t *const scratch = this->cam_scratch_[0];
      if (scratch == nullptr || bytes > dma_usable(this->cam_scratch_capacity_[0]))
        return false;
      if (jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, job.data, stream, scratch,
                               static_cast<uint32_t>(dma_usable(this->cam_scratch_capacity_[0])), &out) != ESP_OK ||
          out == 0)
        return false;
#ifdef SVP_CAPTURE
      ok = this->camera_ppa_copy_(scratch, bpp == 3 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565, pw,
                                  static_cast<uint32_t>(bytes / (static_cast<size_t>(pw) * bpp)), sx, sy, in_w, in_h,
                                  fb, bpp == 3 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565, W, H, fb_size,
                                  dx, dy, scale);
#endif
      if (!ok && !scaled) {
        // No PPA: the rows by the CPU
        for (uint32_t r = 0; r < oh; r++)
          memcpy(fb + (static_cast<size_t>(dy + r) * W + dx) * bpp,
                 scratch + (static_cast<size_t>(sy + r) * pw + sx) * bpp, static_cast<size_t>(ow) * bpp);
        esp_cache_msync(fb + static_cast<size_t>(dy) * stride, static_cast<size_t>(oh) * stride,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        ok = true;
      }
    }
  } else {
#ifdef SVP_CAPTURE
    const uint32_t in_cm = job.format == CameraFrameFormat::YUYV   ? PPA_SRM_COLOR_MODE_YUV422_YUYV
                           : job.format == CameraFrameFormat::UYVY ? PPA_SRM_COLOR_MODE_YUV422_UYVY
                                                                   : PPA_SRM_COLOR_MODE_RGB565;
    ok = this->camera_ppa_copy_(job.data, in_cm, job.width, job.height, sx, sy, in_w, in_h, fb,
                                bpp == 3 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565, W, H, fb_size, dx,
                                dy, scale);
#endif
  }
  if (ok)
    this->direct_show_(fb, 0, 0, static_cast<int>(W), static_cast<int>(H), 0);  // flipped to
  return ok;
}
#endif

void SimpleVideoPlayer::camera_preview_end() {
  if (this->cam_busy_) {
    this->cam_end_pending_ = true;
    return;
  }
  for (int i = 0; i < 2; i++) {
    if (this->cam_scratch_[i] != nullptr)
      this->pool_.free(this->cam_scratch_[i]);
    this->cam_scratch_[i] = nullptr;
    this->cam_scratch_capacity_[i] = 0;
  }
  this->cam_held_ = 0;  // the reader let go first (its frames dropped before the end)
  this->cam_cleared_key_ = 0;
}

void SimpleVideoPlayer::camera_service_() {
  if (!this->cam_busy_ || !this->cam_decoded_.exchange(false, std::memory_order_acq_rel))
    return;
  this->cam_busy_ = false;
  auto done = std::move(this->cam_done_);
  this->cam_done_ = nullptr;
  if (this->cam_end_pending_) {
    this->cam_end_pending_ = false;
    this->camera_preview_end();
  }
  if (done)
    done(this->cam_job_data_.ok);
}

// Picture task: the frame fit into the canvas piece, black around it
#ifdef SVP_CAPTURE
bool SimpleVideoPlayer::camera_ppa_copy_(const void *in, uint32_t in_cm, uint32_t pic_w, uint32_t pic_h, uint32_t sx,
                                         uint32_t sy, uint32_t bw, uint32_t bh, uint8_t *out, uint32_t out_cm,
                                         uint32_t out_w, uint32_t out_h, size_t out_size, uint32_t dx, uint32_t dy,
                                         float scale) {
  if (this->cap_ppa_ == nullptr) {
    ppa_client_config_t pcfg{};
    pcfg.oper_type = PPA_OPERATION_SRM;
    pcfg.max_pending_trans_num = 1;
    if (ppa_register_client(&pcfg, &this->cap_ppa_) != ESP_OK) {
      this->cap_ppa_ = nullptr;
      return false;
    }
  }
  ppa_srm_oper_config_t cfg{};
  cfg.in.buffer = in;
  cfg.in.pic_w = pic_w;
  cfg.in.pic_h = pic_h;
  cfg.in.block_w = bw;
  cfg.in.block_h = bh;
  cfg.in.block_offset_x = sx;
  cfg.in.block_offset_y = sy;
  cfg.in.srm_cm = static_cast<ppa_srm_color_mode_t>(in_cm);
  cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
  cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  cfg.out.buffer = out;
  cfg.out.buffer_size = out_size & ~(PsramPool::ALIGN - 1);
  cfg.out.pic_w = out_w;
  cfg.out.pic_h = out_h;
  cfg.out.block_offset_x = dx;
  cfg.out.block_offset_y = dy;
  cfg.out.srm_cm = static_cast<ppa_srm_color_mode_t>(out_cm);
  cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  cfg.scale_x = scale;
  cfg.scale_y = scale;
  cfg.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(this->cap_ppa_, &cfg) == ESP_OK;
}

bool SimpleVideoPlayer::camera_ppa_fit_(const void *in, uint32_t in_cm, uint32_t pic_w, uint32_t pic_h,
                                        uint32_t block_w, uint32_t block_h) {
  CameraJob &job = this->cam_job_data_;
  if (this->cap_ppa_ == nullptr) {
    ppa_client_config_t pcfg{};
    pcfg.oper_type = PPA_OPERATION_SRM;
    pcfg.max_pending_trans_num = 1;
    if (ppa_register_client(&pcfg, &this->cap_ppa_) != ESP_OK) {
      this->cap_ppa_ = nullptr;
      return false;
    }
  }
  const float sx = static_cast<float>(job.target_w) / static_cast<float>(block_w);
  const float sy = static_cast<float>(job.target_h) / static_cast<float>(block_h);
  // The PPA scales in 1/16 steps: the fit rounded down to one, so the result never exceeds the target
  const float scale = std::floor(std::min(sx, sy) * 16.0f) / 16.0f;
  if (scale <= 0.0f)
    return false;
  const int32_t ow = static_cast<int32_t>(static_cast<float>(block_w) * scale) & ~1;
  const int32_t oh = static_cast<int32_t>(static_cast<float>(block_h) * scale);
  const int32_t dx = ((job.target_w - ow) / 2) & ~1;
  const int32_t dy = (job.target_h - oh) / 2;
  // The black around the picture: once per frame size
  const uint32_t key = (block_w << 16) | block_h;
  if (this->cam_cleared_key_ != key) {
    memset(job.target, 0, static_cast<size_t>(job.target_w) * job.target_h * 2);
    written_by_cpu(job.target, job.target_capacity);
    this->cam_cleared_key_ = key;
    job.out_x = 0;  // the black is new too: all of it redrawn once
    job.out_y = 0;
    job.out_w = job.target_w;
    job.out_h = job.target_h;
  } else {
    job.out_x = dx;
    job.out_y = dy;
    job.out_w = ow;
    job.out_h = oh;
  }
  ppa_srm_oper_config_t cfg{};
  cfg.in.buffer = in;
  cfg.in.pic_w = pic_w;
  cfg.in.pic_h = pic_h;
  cfg.in.block_w = block_w;
  cfg.in.block_h = block_h;
  cfg.in.srm_cm = static_cast<ppa_srm_color_mode_t>(in_cm);
  cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
  cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  cfg.out.buffer = job.target;
  cfg.out.buffer_size = dma_usable(job.target_capacity);
  cfg.out.pic_w = static_cast<uint32_t>(job.target_w);
  cfg.out.pic_h = static_cast<uint32_t>(job.target_h);
  cfg.out.block_offset_x = static_cast<uint32_t>(dx);
  cfg.out.block_offset_y = static_cast<uint32_t>(dy);
  cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  cfg.scale_x = scale;
  cfg.scale_y = scale;
  cfg.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(this->cap_ppa_, &cfg) == ESP_OK;
}
#endif

void SimpleVideoPlayer::camera_decode_() {
  CameraJob &job = this->cam_job_data_;
#ifdef SVP_DSI_OUTPUT
  if (job.direct) {
    job.ok = this->camera_direct_draw_();
    return;
  }
#endif
  if (job.format == CameraFrameFormat::MJPEG) {
    jpeg_decode_picture_info_t info{};
    if (jpeg_decoder_get_info(job.data, static_cast<uint32_t>(job.length), &info) != ESP_OK || info.width == 0 ||
        info.height == 0)
      return;
    uint32_t pw = 0;
    const size_t bytes = jpeg_decoded_size(info, &pw);
    jpeg_decode_cfg_t cfg{};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian RGB565, as LVGL's
    cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    const uint32_t stream = static_cast<uint32_t>(align_up(job.length, 16));
    uint32_t out = 0;
    const bool direct = job.target != nullptr && info.width <= static_cast<uint32_t>(job.target_w) &&
                        info.height <= static_cast<uint32_t>(job.target_h) &&
                        pw == static_cast<uint32_t>(job.target_w) && bytes <= dma_usable(job.target_capacity);
    if (direct) {
      esp_cache_msync(job.target, dma_usable(job.target_capacity),
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
      job.ok = jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, job.data, stream, job.target,
                                    static_cast<uint32_t>(dma_usable(job.target_capacity)), &out) == ESP_OK &&
               out != 0;
      job.out_x = 0;
      job.out_y = 0;
      job.out_w = static_cast<int32_t>(info.width);
      job.out_h = static_cast<int32_t>(info.height);
      return;
    }
    uint8_t *const scratch = this->cam_scratch_[job.slot];
    if (scratch == nullptr || bytes > dma_usable(this->cam_scratch_capacity_[job.slot]))
      return;
    if (jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, job.data, stream, scratch,
                             static_cast<uint32_t>(dma_usable(this->cam_scratch_capacity_[job.slot])), &out) != ESP_OK ||
        out == 0)
      return;
    if (job.target == nullptr) {  // the reader takes it from the scratch (camera_decoded)
      job.pic_w = static_cast<uint16_t>(pw);
      job.pic_h = static_cast<uint16_t>(bytes / (static_cast<size_t>(pw) * 2));
      job.out_x = 0;
      job.out_y = 0;
      job.out_w = static_cast<int32_t>(info.width);
      job.out_h = static_cast<int32_t>(info.height);
      job.ok = true;
      return;
    }
#ifdef SVP_CAPTURE
    // Scaled by the PPA (DMA): the decoder's rows are pw wide, the picture info.width of them
    if (this->camera_ppa_fit_(scratch, PPA_SRM_COLOR_MODE_RGB565, pw, info.height, info.width,
                              info.height)) {
      job.ok = true;
      return;
    }
#endif
    scale_rgb565(scratch, static_cast<int32_t>(info.width), static_cast<int32_t>(info.height), pw * 2,
                 job.target, job.target_w, job.target_h, static_cast<uint32_t>(job.target_w) * 2,
                 ImageFitMode::SCALE_TO_FIT);
    written_by_cpu(job.target, job.target_capacity);
    this->cam_cleared_key_ = 0;  // the CPU drew the black itself
    job.out_x = 0;
    job.out_y = 0;
    job.out_w = job.target_w;
    job.out_h = job.target_h;
    job.ok = true;
    return;
  }
#ifdef SVP_CAPTURE
  // YUV422: the PPA converts and scales it straight into the canvas (its driver writes the input's cache lines back,
  // ppa_srm.c)
  if (job.width == 0 || job.height == 0)
    return;
  job.ok = this->camera_ppa_fit_(
      job.data, job.format == CameraFrameFormat::YUYV ? PPA_SRM_COLOR_MODE_YUV422_YUYV : PPA_SRM_COLOR_MODE_YUV422_UYVY,
      job.width, job.height, job.width, job.height);
#endif
}

// As the old picture_viewer: the whole file, the hardware decoder's RGB565, the canvas. A picture
// no larger than the canvas goes straight into its memory; a larger one (a thumbnail always) is
// decoded once into a buffer of its own and scaled from there.
void SimpleVideoPlayer::decode_job_(PictureJob &job) {
  runtime_image::ImageFormat format;
  if (!picture_format(job.path, &format))
    return;
  if (format == runtime_image::JPEG) {
    jpeg_decode_picture_info_t info{};
    if (this->hw_jpeg_decoder_ == nullptr ||
        jpeg_decoder_get_info(job.data, static_cast<uint32_t>(job.length), &info) != ESP_OK || info.width == 0 ||
        info.height == 0) {
      ESP_LOGW(TAG, "%s: not a JPEG the decoder takes", job.path.c_str());
      return;
    }
    uint32_t pw = 0;
    const size_t bytes = jpeg_decoded_size(info, &pw);
    jpeg_decode_cfg_t cfg{};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;  // little-endian RGB565, as LVGL's
    cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    const uint32_t stream = static_cast<uint32_t>(align_up(job.length, 16));
    const size_t usable = dma_usable(job.target_capacity);
    uint32_t out = 0;
    if (!job.thumbnail && static_cast<int32_t>(info.width) <= job.target_w &&
        static_cast<int32_t>(info.height) <= job.target_h && bytes <= usable) {
      // Lines the CPU wrote go out first, or one written back later would land on the picture.
      esp_cache_msync(job.target, usable, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
      if (jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, job.data, stream, job.target,
                               static_cast<uint32_t>(usable), &out) != ESP_OK ||
          out == 0) {
        ESP_LOGW(TAG, "%s: decoding failed", job.path.c_str());
        return;
      }
      job.direct = true;
      job.width = static_cast<int32_t>(info.width);
      job.height = static_cast<int32_t>(info.height);
      job.stride = pw * 2;
      job.ok = true;
      return;
    }
    if (job.scratch == nullptr || bytes > dma_usable(job.scratch_capacity))
      return;  // reserved by start_decode_(), which said why not
    if (jpeg_decoder_process(this->hw_jpeg_decoder_, &cfg, job.data, stream, job.scratch,
                             static_cast<uint32_t>(dma_usable(job.scratch_capacity)), &out) == ESP_OK &&
        out != 0) {
      scale_rgb565(job.scratch, static_cast<int32_t>(info.width), static_cast<int32_t>(info.height), pw * 2,
                   job.target, job.target_w, job.target_h, job.target_stride, job.fit);
      written_by_cpu(job.page != nullptr || job.background ? job.target : nullptr, job.target_capacity);
      job.ok = true;
    } else {
      ESP_LOGW(TAG, "%s: decoding failed", job.path.c_str());
    }
    return;
  }

  // PNG, BMP: runtime_image, then scaled; its buffer goes again at once.
  runtime_image::RuntimeImage *image = this->pic_image_.get();
  if (image == nullptr || !image->begin_decode(job.length, format))
    return;
  size_t offset = 0;
  auto *data = const_cast<uint8_t *>(job.data);
  while (offset < job.length) {
    const int used = image->feed_data(data + offset, job.length - offset);
    if (used < 0) {
      ESP_LOGW(TAG, "%s: %s", job.path.c_str(), runtime_image::decode_error_to_string(used));
      image->release();
      return;
    }
    if (used == 0)
      break;
    offset += static_cast<size_t>(used);
  }
  image->end_decode();
  const uint8_t *pixels = image->get_data_start();
  if (pixels != nullptr && image->get_width() > 0 && image->get_height() > 0 && esp_ptr_external_ram(pixels)) {
    scale_rgb565(pixels, image->get_width(), image->get_height(), image->get_width_stride(), job.target, job.target_w,
                 job.target_h, job.target_stride, job.fit);
    written_by_cpu(job.page != nullptr || job.background ? job.target : nullptr, job.target_capacity);
    job.ok = true;
  } else {
    ESP_LOGW(TAG, "%s: incomplete picture, or not in PSRAM", job.path.c_str());
  }
  image->release();
}

bool SimpleVideoPlayer::show_canvas_picture(lv_obj_t *canvas, const uint8_t *data, size_t length, const char *name,
                                            std::function<void(bool)> &&done) {
  if (canvas == nullptr || data == nullptr || length == 0 || name == nullptr || !this->pic_ready_ ||
      this->pic_task_ == nullptr || this->view_step_ != ViewStep::IDLE || this->lvgl_component_ == nullptr ||
      this->lvgl_component_->is_paused() || this->pictures_held_() || this->pool_wanted_(POOL_PICTURES))
    return false;
  runtime_image::ImageFormat format;
  if (!picture_format(name, &format))
    return false;
  this->release_view_();
  int32_t w = lv_obj_get_style_width(canvas, LV_PART_MAIN);
  int32_t h = lv_obj_get_style_height(canvas, LV_PART_MAIN);
  if (LV_COORD_IS_SPEC(w) || LV_COORD_IS_SPEC(h)) {
    lv_obj_update_layout(canvas);
    w = lv_obj_get_width(canvas);
    h = lv_obj_get_height(canvas);
  }
  if (w <= 0 || h <= 0)
    return false;
  PictureJob &job = this->view_job_data_;
  job = PictureJob{};
  job.data = data;
  job.length = length;
  job.path = name;
  job.target_w = w;
  job.target_h = h;
  job.target_stride = static_cast<uint32_t>(align_up(w, 16) * 2);
  job.fit = this->fit_mode_;
  job.background = true;
  this->view_capacity_ = align_up(align_up(w, 16) * align_up(h, 16) * 2, PsramPool::ALIGN);
  this->view_data_ = this->pool_take_(this->view_capacity_, POOL_USER_PICTURES);
  if (this->view_data_ == nullptr) {
    ESP_LOGW(TAG, "%s: no room in the PSRAM pool", name);
    this->view_capacity_ = 0;
    return false;
  }
  job.target = this->view_data_;
  job.target_capacity = this->view_capacity_;
  jpeg_decode_picture_info_t info{};
  if (format == runtime_image::JPEG && jpeg_decoder_get_info(data, static_cast<uint32_t>(length), &info) == ESP_OK &&
      info.width != 0 && info.height != 0) {
    const size_t bytes = jpeg_decoded_size(info, nullptr);
    const bool straight = static_cast<int32_t>(info.width) <= w && static_cast<int32_t>(info.height) <= h &&
                          bytes <= dma_usable(this->view_capacity_);
    if (!straight) {
      const size_t scratch = align_up(bytes, PsramPool::ALIGN);
      this->view_scratch_ = this->pool_take_(scratch, POOL_USER_PICTURES);
      if (this->view_scratch_ == nullptr) {
        ESP_LOGW(TAG, "%s: %ux%u needs %u bytes of the PSRAM pool to decode", name, static_cast<unsigned>(info.width),
                 static_cast<unsigned>(info.height), static_cast<unsigned>(bytes));
        this->release_view_();
        return false;
      }
      job.scratch = this->view_scratch_;
      job.scratch_capacity = scratch;
    }
  }
  this->view_canvas_ = canvas;
  this->view_done_ = std::move(done);
  this->view_close_ = false;
  this->view_step_ = ViewStep::WAITING;
  return true;
}

void SimpleVideoPlayer::close_canvas_picture() {
  this->view_close_ = true;
  if (this->view_step_ == ViewStep::IDLE && this->lvgl_component_ != nullptr && !this->lvgl_component_->is_paused()) {
    this->view_close_ = false;
    this->release_view_();
  }
}

void SimpleVideoPlayer::release_view_() {
  if (this->view_attached_ && this->view_canvas_ != nullptr) {
    if (this->lvgl_component_ == nullptr || this->lvgl_component_->is_paused()) {
      this->view_detach_pending_ = true;
    } else {
      lv_image_set_src(this->view_canvas_, nullptr);
    }
  }
  this->view_attached_ = false;
  this->pool_.free(this->view_data_);
  this->view_data_ = nullptr;
  this->view_capacity_ = 0;
  this->pool_.free(this->view_scratch_);
  this->view_scratch_ = nullptr;
}

void SimpleVideoPlayer::view_loop_() {
  if (this->lvgl_component_ == nullptr || this->lvgl_component_->is_paused())
    return;
  if (this->view_detach_pending_) {
    this->view_detach_pending_ = false;
    lv_image_set_src(this->view_canvas_, nullptr);
  }
  switch (this->view_step_) {
    case ViewStep::IDLE:
      if (this->view_close_) {
        this->view_close_ = false;
        this->release_view_();
      }
      return;
    case ViewStep::WAITING: {
      if (!this->view_close_) {
        if (this->pic_read_.decoding || this->cam_busy_)
          return;
#ifdef SVP_BACKGROUND
        if (this->bg_step_ == BgStep::DECODING)
          return;
#endif
#ifdef SVP_AUDIO_COVER
        if (this->cover_step_ == CoverStep::DECODING)
          return;
#endif
        this->view_decoded_.store(false, std::memory_order_relaxed);
        this->view_job_.store(true, std::memory_order_release);
        this->view_step_ = ViewStep::DECODING;
        xTaskNotifyGive(this->pic_task_);
        return;
      }
      break;
    }
    case ViewStep::DECODING:
      if (!this->view_decoded_.load(std::memory_order_acquire))
        return;
      break;
  }
  this->view_step_ = ViewStep::IDLE;
  this->pool_.free(this->view_scratch_);
  this->view_scratch_ = nullptr;
  const PictureJob &job = this->view_job_data_;
  const bool ok = job.ok && !this->view_close_;
  if (ok) {
    const int32_t sw = job.direct ? job.width : job.target_w;
    const int32_t sh = job.direct ? job.height : job.target_h;
    const uint32_t stride = job.direct ? job.stride : job.target_stride;
    this->view_detach_pending_ = false;
    lv_draw_buf_init(&this->view_buf_, sw, sh, LV_COLOR_FORMAT_RGB565, stride, this->view_data_, this->view_capacity_);
    lv_canvas_set_draw_buf(this->view_canvas_, &this->view_buf_);
    lv_obj_invalidate(this->view_canvas_);
    this->view_attached_ = true;
  } else {
    this->release_view_();
  }
  this->view_close_ = false;
  auto done = std::move(this->view_done_);
  this->view_done_ = nullptr;
  if (done)
    done(ok);
}

void SimpleVideoPlayer::picture_decoded_() {
  const PictureRead read = this->pic_read_;
  this->pic_read_ = PictureRead{};
  this->pic_decoded_.store(false, std::memory_order_relaxed);
  const PictureJob &job = this->pic_job_;
  // The decode buffer only while pictures larger than their target are decoded.
  if (job.scratch != nullptr && this->thumb_queue_.empty() && this->pic_wanted_ < 0)
    this->release_decode_();
  const bool current = read.generation == this->pic_generation_ && read.index < this->images_.size();
  if (read.thumbnail) {
    if (!job.ok || !current || read.slot >= this->thumb_slots_.size())
      return;
    ThumbSlot &slot = this->thumb_slots_[read.slot];
    if (slot.image != static_cast<int>(read.index))
      return;
    slot.loaded = true;
    this->show_slot_(read.slot);
    if (this->thumb_ready_callback_)
      this->thumb_ready_callback_(read.index);
    return;
  }

  PicPage *page = job.page;
  if (!current) {
    // Of an older listing: the page stays empty.
  } else if (job.ok) {
    // Straight in: the page at the picture's own size, LVGL centres it in the canvas.
    if (job.direct) {
      lv_draw_buf_init(&page->buf, job.width, job.height, LV_COLOR_FORMAT_RGB565, job.stride, page->data,
                       page->capacity);
    } else {
      lv_draw_buf_init(&page->buf, page->w, page->h, LV_COLOR_FORMAT_RGB565, page->stride, page->data,
                       page->capacity);
    }
    page->index = static_cast<int>(read.index);
    page->generation = read.generation;
    if (!read.preload || this->pic_wanted_ == static_cast<int>(read.index)) {
      if (this->pic_wanted_ == static_cast<int>(read.index))
        this->pic_wanted_ = -1;
      this->show_page_(page);
    }
  } else if (!read.preload) {
    // What it was is gone.
    memset(page->data, 0, page->capacity);
    esp_cache_msync(page->data, page->capacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    lv_draw_buf_init(&page->buf, page->w, page->h, LV_COLOR_FORMAT_RGB565, page->stride, page->data, page->capacity);
    this->current_image_ = -1;
    this->show_page_(page);
  }
  if (this->pic_fullscreen_pending_ >= 0) {
    const bool fullscreen = this->pic_fullscreen_pending_ != 0;
    this->pic_fullscreen_pending_ = -1;
    if (this->pic_pending_w_ > 0 && this->pic_pending_h_ > 0) {
      this->set_fullscreen(fullscreen, this->pic_pending_w_, this->pic_pending_h_);
    } else {
      this->set_fullscreen(fullscreen);
    }
  }
}

//========================================================================
// Canvas
//========================================================================

// The canvas widget's size (YAML width/height, or what full screen set).
void SimpleVideoPlayer::update_picture_size_() {
  lv_obj_update_layout(this->pic_canvas_);
  int32_t w = lv_obj_get_width(this->pic_canvas_);
  int32_t h = lv_obj_get_height(this->pic_canvas_);
  if ((w <= 0 || h <= 0) && this->pic_lvgl_buf_ != nullptr) {
    w = static_cast<int32_t>(this->pic_lvgl_buf_->header.w);
    h = static_cast<int32_t>(this->pic_lvgl_buf_->header.h);
  }
  this->pic_w_ = w;
  this->pic_h_ = h;
  this->pic_stride_ = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
}

// Main loop: the canvas takes the size pictures are decoded for (set_fullscreen() with a size).
void SimpleVideoPlayer::apply_picture_size_() {
  if (lv_obj_get_style_width(this->pic_canvas_, LV_PART_MAIN) != this->pic_w_ ||
      lv_obj_get_style_height(this->pic_canvas_, LV_PART_MAIN) != this->pic_h_)
    lv_obj_set_size(this->pic_canvas_, this->pic_w_, this->pic_h_);
  this->pic_shown_fullscreen_ = this->fullscreen_;
}

// Main loop: the canvas shows a decoded page.
void SimpleVideoPlayer::show_page_(PicPage *page) {
  // A page of a new size goes in together with that size: no step between.
  if (this->pic_shown_fullscreen_ != this->fullscreen_ && page->w == this->pic_w_ && page->h == this->pic_h_)
    this->apply_picture_size_();
  lv_canvas_set_draw_buf(this->pic_canvas_, &page->buf);
  this->pic_shown_ = page;
  this->pic_preload_tries_ = 0;  // the neighbours again: one page may have gone to this one
  if (page->index >= 0) {
    const bool changed = this->current_image_ != page->index;
    this->current_image_ = page->index;
    if (changed) {
      const size_t index = static_cast<size_t>(page->index);
      const size_t slots = this->thumb_slots_.size();
      if (slots != 0 && (index < this->thumb_window_ || index >= this->thumb_window_ + slots))
        this->place_thumbnail_window_(index > slots / 2 ? index - slots / 2 : 0);
      this->highlight_thumbnail_(true);
      ESP_LOGD(TAG, "Showing %s (%ux%u)", this->images_[index].filename.c_str(),
               static_cast<unsigned>(page->buf.header.w), static_cast<unsigned>(page->buf.header.h));
    }
  }
  this->finish_picture_();
  this->picture_callbacks_.call();
}

// The shown picture once more (overlay gone, canvas resized): decoded again from its file.
void SimpleVideoPlayer::render_current_() {
  if (this->current_image_ >= 0 && this->pic_wanted_ < 0)
    this->pic_wanted_ = this->current_image_;
}

// The overlay over what shows, and LVGL told.
void SimpleVideoPlayer::finish_picture_() {
  if (this->overlay_visible_ && this->pic_shown_ != nullptr) {
    this->pic_shown_->index = -1;  // drawn over: decoded again without it
    const int cx = static_cast<int>(this->pic_shown_->buf.header.w) / 2;
    const int cy = static_cast<int>(this->pic_shown_->buf.header.h) / 2;
    const uint16_t color = to_rgb565(this->overlay_icon_color_);
    if (this->overlay_playing_) {
      this->draw_play_icon_(cx, cy, this->overlay_icon_size_, color);
    } else {
      this->draw_pause_icon_(cx, cy, this->overlay_icon_size_, color);
    }
    written_by_cpu(this->pic_shown_->data, this->pic_shown_->capacity);
  }
  lv_obj_invalidate(this->pic_canvas_);
}

void SimpleVideoPlayer::set_fullscreen(bool fullscreen) {
  if (this->pic_canvas_ == nullptr || !this->pic_ready_)
    return;
  if (this->pic_read_.decoding && !this->pic_read_.thumbnail) {
    this->pic_fullscreen_pending_ = fullscreen ? 1 : 0;  // for the size being decoded
    return;
  }
  this->pic_pending_w_ = 0;
  this->pic_pending_h_ = 0;
  if (this->fullscreen_ == fullscreen)
    return;
  this->fullscreen_ = fullscreen;
  this->pic_shown_fullscreen_ = fullscreen;
  this->update_picture_size_();
  // Into a page of the new size; the old one shows until then.
  this->render_current_();
}

void SimpleVideoPlayer::set_fullscreen(bool fullscreen, int32_t width, int32_t height) {
  if (this->pic_canvas_ == nullptr || !this->pic_ready_ || width <= 0 || height <= 0)
    return;
  if (this->pic_read_.decoding && !this->pic_read_.thumbnail) {
    this->pic_fullscreen_pending_ = fullscreen ? 1 : 0;  // for the size being decoded
    this->pic_pending_w_ = width;
    this->pic_pending_h_ = height;
    return;
  }
  this->pic_pending_w_ = 0;
  this->pic_pending_h_ = 0;
  if (this->fullscreen_ == fullscreen && this->pic_shown_fullscreen_ == fullscreen && this->pic_w_ == width &&
      this->pic_h_ == height)
    return;
  this->fullscreen_ = fullscreen;
  this->pic_w_ = width;
  this->pic_h_ = height;
  this->pic_stride_ = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
  if (this->current_image_ < 0 || this->pictures_held_()) {
    // Nothing to fit first.
    this->apply_picture_size_();
    this->picture_callbacks_.call();
    return;
  }
  // The shown picture stays as it is until it is there in the new size (show_page_()).
  this->render_current_();
}

//========================================================================
// Play/pause overlay
//========================================================================

void SimpleVideoPlayer::show_overlay_icon_(bool playing) {
  this->overlay_visible_ = true;
  this->overlay_playing_ = playing;
  this->overlay_shown_ms_ = millis();
  if (this->pic_read_.decoding && !this->pic_read_.thumbnail)
    return;  // drawn once the picture is there
  if (this->current_image_ >= 0)
    this->finish_picture_();
}

void SimpleVideoPlayer::hide_overlay_icon_() {
  if (!this->overlay_visible_)
    return;
  this->overlay_visible_ = false;
  this->render_current_();  // the picture without it
}

void SimpleVideoPlayer::fill_rect_(int x0, int y0, int x1, int y1, uint16_t color) {
  const lv_draw_buf_t *buf = &this->pic_shown_->buf;
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, static_cast<int>(buf->header.w) - 1);
  y1 = std::min(y1, static_cast<int>(buf->header.h) - 1);
  for (int y = y0; y <= y1; y++) {
    auto *row = reinterpret_cast<uint16_t *>(buf->data + static_cast<size_t>(y) * buf->header.stride);
    for (int x = x0; x <= x1; x++)
      row[x] = color;
  }
}

void SimpleVideoPlayer::draw_play_icon_(int cx, int cy, int size, uint16_t color) {
  // A triangle pointing right, 3:4 wide.
  const int height = size;
  const int width = size * 3 / 4;
  const int left = cx - width / 2;
  const int top = cy - height / 2;
  for (int y = 0; y <= height; y++) {
    const int distance = y <= height / 2 ? y : height - y;
    this->fill_rect_(left, top + y, left + distance * width * 2 / height, top + y, color);
  }
}

void SimpleVideoPlayer::draw_pause_icon_(int cx, int cy, int size, uint16_t color) {
  const int bar = size / 5;
  const int gap = size / 6;
  const int top = cy - size / 2;
  const int bottom = cy + size / 2;
  this->fill_rect_(cx - gap / 2 - bar, top, cx - gap / 2, bottom, color);
  this->fill_rect_(cx + gap / 2, top, cx + gap / 2 + bar, bottom, color);
}

//========================================================================
// Touch on the picture
//========================================================================

void SimpleVideoPlayer::canvas_event_(lv_event_t *event) {
  auto *self = static_cast<SimpleVideoPlayer *>(lv_event_get_user_data(event));
  lv_indev_t *indev = lv_indev_active();
  switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED: {
      lv_point_t point{};
      if (indev != nullptr)
        lv_indev_get_point(indev, &point);
      self->press_x_ = point.x;
      self->press_y_ = point.y;
      self->press_ms_ = millis();
      self->press_handled_ = false;
      self->gesture_seen_ = false;
#ifdef SVP_CONTROLS
      self->picture_controls_touched_();
#endif
      break;
    }
#ifdef SVP_CONTROLS
    case LV_EVENT_SHORT_CLICKED:
      // In full screen a tap shows the controls (or hides them again).
      if (self->fullscreen_ && !self->press_handled_)
        self->picture_controls_tap_();
      break;
#endif
    case LV_EVENT_PRESSING: {
      // Held still for long_press_time: the slideshow starts or stops.
      if (self->press_handled_ || self->gesture_seen_ || millis() - self->press_ms_ < self->long_press_time_ms_)
        break;
      lv_point_t point{};
      if (indev != nullptr)
        lv_indev_get_point(indev, &point);
      if (std::abs(point.x - self->press_x_) > LONG_PRESS_SLOP || std::abs(point.y - self->press_y_) > LONG_PRESS_SLOP)
        break;
      self->press_handled_ = true;
      if (self->slideshow_mode_ == SlideshowMode::PLAYING) {
        self->stop_slideshow();
      } else {
        self->start_slideshow();
      }
      break;
    }
    case LV_EVENT_GESTURE:
      self->gesture_seen_ = true;
      if (!self->press_handled_ && indev != nullptr) {
        self->press_handled_ = true;
        self->canvas_gesture_(lv_indev_get_gesture_dir(indev));
      }
      break;
    default:
      break;
  }
}

void SimpleVideoPlayer::canvas_gesture_(lv_dir_t dir) {
  if (dir == LV_DIR_NONE)
    return;
  // A swipe from the strip's edge slides the thumbnails in; one towards it slides them out.
  if (this->thumb_slide_enabled_ && !this->thumb_slots_.empty()) {
    lv_obj_t *screen = lv_screen_active();
    const int32_t sw = lv_obj_get_width(screen);
    const int32_t sh = lv_obj_get_height(screen);
    const int32_t edge_h = this->thumb_layout_ == ThumbnailLayout::VERTICAL ? this->thumb_w_ : sw / 10;
    const int32_t edge_v = this->thumb_layout_ == ThumbnailLayout::HORIZONTAL ? this->thumb_h_ : sh / 10;
    bool in_from_edge = false;
    lv_dir_t inward = LV_DIR_NONE;
    lv_dir_t outward = LV_DIR_NONE;
    switch (this->thumb_slide_edge_) {
      case ThumbnailSlideEdge::RIGHT:
        in_from_edge = this->press_x_ > sw - edge_h;
        inward = LV_DIR_LEFT;
        outward = LV_DIR_RIGHT;
        break;
      case ThumbnailSlideEdge::LEFT:
        in_from_edge = this->press_x_ < edge_h;
        inward = LV_DIR_RIGHT;
        outward = LV_DIR_LEFT;
        break;
      case ThumbnailSlideEdge::TOP:
        in_from_edge = this->press_y_ < edge_v;
        inward = LV_DIR_BOTTOM;
        outward = LV_DIR_TOP;
        break;
      case ThumbnailSlideEdge::BOTTOM:
        in_from_edge = this->press_y_ > sh - edge_v;
        inward = LV_DIR_TOP;
        outward = LV_DIR_BOTTOM;
        break;
    }
    if (!this->thumbs_visible_ && in_from_edge && dir == inward) {
      this->slide_thumbnails_(true);
      return;
    }
    if (this->thumbs_visible_ && dir == outward) {
      this->slide_thumbnails_(false);
      return;
    }
  }
  // Up and down change the folder (with more than one configured).
  if (this->pic_dirs_.size() > 1 && (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM)) {
    const size_t count = this->pic_dirs_.size();
    const size_t current = this->pic_dir_index_ < count ? this->pic_dir_index_ : 0;
    this->set_current_directory(dir == LV_DIR_TOP ? (current + 1) % count : (current + count - 1) % count);
    return;
  }
  // Left and right: next and previous picture; a running slideshow stops.
  if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
    this->stop_slideshow();
    if (dir == LV_DIR_LEFT) {
      this->next_image();
    } else {
      this->previous_image();
    }
  }
}

//========================================================================
// Thumbnails
//========================================================================

bool SimpleVideoPlayer::setup_thumbnails_() {
  this->thumb_stride_ = lv_draw_buf_width_to_stride(this->thumb_w_, LV_COLOR_FORMAT_RGB565);
  const size_t slot_bytes = static_cast<size_t>(this->thumb_stride_) * this->thumb_h_;
  const size_t total = slot_bytes * this->thumb_count_;
  // The slots' list at the pool's end, their pictures at its start, for good (plan_pool_()).
  if (!this->thumb_slots_.reserve(this->pool_, this->thumb_count_) ||
      !this->thumb_queue_.reserve(this->pool_, this->thumb_count_)) {
    ESP_LOGE(TAG, "Thumbnails disabled: no room in the PSRAM pool for their slots");
    return false;
  }
  this->thumb_buffer_ = this->pool_.alloc(total);
  if (this->thumb_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Thumbnails need %u bytes of the PSRAM pool -- disabled", static_cast<unsigned>(total));
    return false;
  }
  memset(this->thumb_buffer_, 0, total);

  lv_style_init(&this->thumb_default_style_);
  lv_style_set_pad_all(&this->thumb_default_style_, 0);
  lv_style_set_border_width(&this->thumb_default_style_, 0);
  lv_style_set_radius(&this->thumb_default_style_, 0);
  // The shown picture's thumbnail: a colour over it (recolor reaches the canvas inside), no border.
  lv_style_init(&this->thumb_default_active_style_);
  lv_style_set_recolor(&this->thumb_default_active_style_, lv_color_hex(0x00FF00));
  lv_style_set_recolor_opa(&this->thumb_default_active_style_, LV_OPA_30);

  lv_obj_t *box = this->thumb_container_;
  if (this->thumb_container_style_ != nullptr)
    lv_obj_add_style(box, this->thumb_container_style_, LV_PART_MAIN);
  lv_obj_set_flex_flow(box, this->thumb_layout_ == ThumbnailLayout::HORIZONTAL ? LV_FLEX_FLOW_ROW
                            : this->thumb_layout_ == ThumbnailLayout::VERTICAL ? LV_FLEX_FLOW_COLUMN
                                                                               : LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(box, this->thumb_spacing_, LV_PART_MAIN);
  lv_obj_set_style_pad_row(box, this->thumb_spacing_, LV_PART_MAIN);
  if (this->thumb_layout_ == ThumbnailLayout::GRID) {
    const int32_t columns = std::max(this->thumb_columns_, 1);
    lv_obj_set_width(box, columns * this->thumb_w_ + (columns - 1) * this->thumb_spacing_ +
                              lv_obj_get_style_pad_left(box, LV_PART_MAIN) +
                              lv_obj_get_style_pad_right(box, LV_PART_MAIN) +
                              2 * lv_obj_get_style_border_width(box, LV_PART_MAIN));
  }
  lv_obj_set_scrollable(box, true);
  lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(box, this->thumb_layout_ == ThumbnailLayout::HORIZONTAL ? LV_DIR_HOR : LV_DIR_VER);
  lv_obj_add_event_cb(box, &SimpleVideoPlayer::thumbnail_event_, LV_EVENT_SCROLL, this);

  for (size_t i = 0; i < this->thumb_count_; i++) {
    ThumbSlot &slot = *this->thumb_slots_.emplace_back();
    slot.data = this->thumb_buffer_ + i * slot_bytes;
    lv_obj_t *button = lv_obj_create(box);
    lv_obj_set_size(button, this->thumb_w_, this->thumb_h_);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_clickable(button, true);
    lv_obj_add_style(button, this->thumb_style_ != nullptr ? this->thumb_style_ : &this->thumb_default_style_,
                     LV_PART_MAIN);
    lv_obj_set_hidden(button, true);
    lv_obj_set_user_data(button, reinterpret_cast<void *>(i));
    lv_obj_add_event_cb(button, &SimpleVideoPlayer::thumbnail_event_, LV_EVENT_CLICKED, this);
    lv_obj_t *canvas = lv_canvas_create(button);
    lv_canvas_set_buffer(canvas, slot.data, this->thumb_w_, this->thumb_h_, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);
    lv_obj_set_clickable(canvas, false);
    if (!this->thumb_label_pattern_.empty()) {
      slot.label = lv_label_create(button);
      lv_obj_align(slot.label, LV_ALIGN_BOTTOM_MID, 0, 0);
      lv_obj_set_clickable(slot.label, false);
      if (this->thumb_label_style_ != nullptr)
        lv_obj_add_style(slot.label, this->thumb_label_style_, LV_PART_MAIN);
    }
    slot.button = button;
    slot.canvas = canvas;
  }

  lv_obj_t *strip = lv_obj_get_parent(box);
  if (strip != nullptr && this->thumb_slide_enabled_) {
    // A swipe on the strip (bubbled up from the thumbnails) slides it out.
    lv_obj_set_clickable(strip, true);
    lv_obj_add_event_cb(strip, &SimpleVideoPlayer::thumbnail_event_, LV_EVENT_GESTURE, this);
  }
  this->slide_thumbnails_(true);
  return true;
}

void SimpleVideoPlayer::thumbnail_event_(lv_event_t *event) {
  auto *self = static_cast<SimpleVideoPlayer *>(lv_event_get_user_data(event));
  self->thumb_touched_ms_ = millis();
  switch (lv_event_get_code(event)) {
    case LV_EVENT_SCROLL:
      self->thumbnails_scrolled_();
      break;
    case LV_EVENT_GESTURE: {
      lv_indev_t *indev = lv_indev_active();
      if (!self->thumbs_visible_ || indev == nullptr)
        break;
      const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
      const ThumbnailSlideEdge edge = self->thumb_slide_edge_;
      if ((edge == ThumbnailSlideEdge::RIGHT && dir == LV_DIR_RIGHT) ||
          (edge == ThumbnailSlideEdge::LEFT && dir == LV_DIR_LEFT) ||
          (edge == ThumbnailSlideEdge::TOP && dir == LV_DIR_TOP) ||
          (edge == ThumbnailSlideEdge::BOTTOM && dir == LV_DIR_BOTTOM))
        self->slide_thumbnails_(false);
      break;
    }
    case LV_EVENT_CLICKED: {
      const auto slot = reinterpret_cast<size_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
      if (slot >= self->thumb_slots_.size() || self->thumb_slots_[slot].image < 0)
        break;
      const auto image = static_cast<size_t>(self->thumb_slots_[slot].image);
      self->stop_slideshow();
      if (self->thumb_click_handled_) {
        self->thumb_click_callbacks_.call(image);
      } else {
        self->show_image(image);
      }
      break;
    }
    default:
      break;
  }
}

std::string SimpleVideoPlayer::thumbnail_label_(size_t image) const {
  if (image >= this->images_.size())
    return "";
  const std::string filename = this->images_[image].filename.c_str();
  const size_t dot = filename.rfind('.');
  const std::string name = dot == std::string::npos ? filename : filename.substr(0, dot);
  const std::string number = std::to_string(image + 1);
  std::string text = this->thumb_label_pattern_;
  const std::pair<const char *, const std::string *> fields[] = {
      {"{index}", &number}, {"{filename}", &filename}, {"{name}", &name}};
  for (const auto &field : fields) {
    const size_t key_length = strlen(field.first);
    for (size_t pos = text.find(field.first); pos != std::string::npos;
         pos = text.find(field.first, pos + field.second->size()))
      text.replace(pos, key_length, *field.second);
  }
  return text;
}

/// The strip holds thumb_count_ slots: slot k shows image thumb_window_ + k.
void SimpleVideoPlayer::place_thumbnail_window_(size_t start) {
  if (this->thumb_slots_.empty())
    return;
  const size_t slots = this->thumb_slots_.size();
  const size_t images = this->images_.size();
  // Whole lines only, so a grid keeps its columns; the last line may have hidden slots.
  const size_t per_line =
      this->thumb_layout_ == ThumbnailLayout::GRID ? static_cast<size_t>(std::max(this->thumb_columns_, 1)) : 1;
  const size_t last_start = images > slots ? (images - slots + per_line - 1) / per_line * per_line : 0;
  start = std::min(start, last_start) / per_line * per_line;
  const size_t slot_bytes = static_cast<size_t>(this->thumb_stride_) * this->thumb_h_;

  // Thumbnails still in the window keep their pixels: moved to their new slot.
  const size_t old_start = this->thumb_window_;
  std::vector<bool> carried(slots, false);
  auto carry = [&](size_t k) {
    const int image = static_cast<int>(start + k);
    if (start + k >= old_start && start + k < old_start + slots) {
      ThumbSlot &from = this->thumb_slots_[start + k - old_start];
      if (from.loaded && from.image == image) {
        if (&from != &this->thumb_slots_[k])
          memcpy(this->thumb_slots_[k].data, from.data, slot_bytes);
        carried[k] = true;
      }
    }
  };
  if (start >= old_start) {
    for (size_t k = 0; k < slots; k++)
      carry(k);
  } else {
    for (size_t k = slots; k-- > 0;)
      carry(k);
  }

  this->thumb_window_ = start;
  this->thumb_queue_.clear();
  for (size_t k = 0; k < slots; k++) {
    ThumbSlot &slot = this->thumb_slots_[k];
    slot.image = start + k < images ? static_cast<int>(start + k) : -1;
    slot.loaded = carried[k];
    if (!slot.loaded)
      memset(slot.data, 0, slot_bytes);
    lv_obj_set_hidden(slot.button, slot.image < 0);
    if (slot.label != nullptr)
      lv_label_set_text(slot.label, slot.image >= 0 ? this->thumbnail_label_(slot.image).c_str() : "");
    lv_obj_invalidate(slot.canvas);
  }
  this->queue_thumbnails_(!this->thumb_lazy_);
  this->highlight_thumbnail_(false);
}

bool SimpleVideoPlayer::visible_slots_(size_t *first, size_t *last) const {
  if (this->thumb_slots_.empty())
    return false;
  const bool horizontal = this->thumb_layout_ == ThumbnailLayout::HORIZONTAL;
  const int32_t per_line = this->thumb_layout_ == ThumbnailLayout::GRID ? std::max(this->thumb_columns_, 1) : 1;
  const int32_t cell = (horizontal ? this->thumb_w_ : this->thumb_h_) + this->thumb_spacing_;
  const int32_t view =
      horizontal ? lv_obj_get_width(this->thumb_container_) : lv_obj_get_height(this->thumb_container_);
  const int32_t pos = std::max<int32_t>(
      0, horizontal ? lv_obj_get_scroll_x(this->thumb_container_) : lv_obj_get_scroll_y(this->thumb_container_));
  const size_t count = this->thumb_slots_.size();
  *first = std::min(count - 1, static_cast<size_t>(pos / cell * per_line));
  *last = std::min(count - 1, static_cast<size_t>(((pos + view) / cell + 1) * per_line - 1));
  return true;
}

void SimpleVideoPlayer::queue_slot_(size_t slot) {
  if (slot >= this->thumb_slots_.size() || this->thumb_slots_[slot].loaded || this->thumb_slots_[slot].image < 0)
    return;
  if (std::find(this->thumb_queue_.begin(), this->thumb_queue_.end(), slot) == this->thumb_queue_.end())
    this->thumb_queue_.push_back(slot);
}

void SimpleVideoPlayer::queue_thumbnails_(bool all) {
  size_t first;
  size_t last;
  if (!this->visible_slots_(&first, &last))
    return;
  for (size_t k = first; k <= last; k++)
    this->queue_slot_(k);
  const size_t count = this->thumb_slots_.size();
  const size_t ahead = all ? count : this->thumb_preload_;
  for (size_t k = last + 1; k < count && k <= last + ahead; k++)
    this->queue_slot_(k);
  for (size_t k = first; k-- > 0 && first - k <= ahead;)
    this->queue_slot_(k);
}

void SimpleVideoPlayer::thumbnails_scrolled_() {
  const bool horizontal = this->thumb_layout_ == ThumbnailLayout::HORIZONTAL;
  const int32_t pos =
      horizontal ? lv_obj_get_scroll_x(this->thumb_container_) : lv_obj_get_scroll_y(this->thumb_container_);
  const uint32_t now = millis();
  if (this->thumb_scroll_ms_ != 0 && now != this->thumb_scroll_ms_)
    this->thumb_scroll_velocity_ =
        static_cast<float>(pos - this->thumb_scroll_pos_) / static_cast<float>(now - this->thumb_scroll_ms_);
  this->thumb_scroll_pos_ = pos;
  this->thumb_scroll_ms_ = now;

  size_t first;
  size_t last;
  if (!this->visible_slots_(&first, &last))
    return;
  // At an end of the window with more pictures beyond it: the window moves half a strip on, the
  // scroll position back by as much, so the same thumbnails stay where they are.
  const size_t slots = this->thumb_slots_.size();
  const size_t images = this->images_.size();
  const size_t per_line =
      this->thumb_layout_ == ThumbnailLayout::GRID ? static_cast<size_t>(std::max(this->thumb_columns_, 1)) : 1;
  const int32_t cell = (horizontal ? this->thumb_w_ : this->thumb_h_) + this->thumb_spacing_;
  size_t start = this->thumb_window_;
  if (this->thumb_shifting_)
    return;  // the scroll set by the shift below
  if (last + 1 >= slots && first > 0 && start + slots < images) {
    start += std::max<size_t>(slots / 2 / per_line, 1) * per_line;
  } else if (first == 0 && last + 1 < slots && start > 0) {
    start -= std::min(start, std::max<size_t>(slots / 2 / per_line, 1) * per_line);
  }
  if (start != this->thumb_window_) {
    const size_t old_start = this->thumb_window_;
    this->place_thumbnail_window_(start);
    const int32_t shift = (static_cast<int32_t>(this->thumb_window_) - static_cast<int32_t>(old_start)) /
                          static_cast<int32_t>(per_line) * cell;
    this->thumb_shifting_ = true;
    if (horizontal) {
      lv_obj_scroll_to_x(this->thumb_container_, pos - shift, LV_ANIM_OFF);
    } else {
      lv_obj_scroll_to_y(this->thumb_container_, pos - shift, LV_ANIM_OFF);
    }
    this->thumb_shifting_ = false;
    this->queue_thumbnails_(!this->thumb_lazy_);
    return;
  }

  for (size_t k = first; k <= last; k++)
    this->queue_slot_(k);
  if (this->thumb_scroll_velocity_ > 0.1f) {
    for (size_t k = last + 1; k < slots && k <= last + this->thumb_preload_; k++)
      this->queue_slot_(k);
  } else if (this->thumb_scroll_velocity_ < -0.1f) {
    for (size_t k = first; k-- > 0 && first - k <= this->thumb_preload_;)
      this->queue_slot_(k);
  }
}

void SimpleVideoPlayer::show_slot_(size_t slot_index) {
  ThumbSlot &slot = this->thumb_slots_[slot_index];
  lv_obj_set_hidden(slot.button, slot.image < 0);
  lv_obj_invalidate(slot.canvas);
}

void SimpleVideoPlayer::highlight_thumbnail_(bool scroll) {
  lv_style_t *active =
      this->thumb_active_style_ != nullptr ? this->thumb_active_style_ : &this->thumb_default_active_style_;
  for (ThumbSlot &slot : this->thumb_slots_) {
    lv_obj_remove_style(slot.button, active, LV_PART_MAIN);
    if (slot.image >= 0 && slot.image == this->current_image_) {
      lv_obj_add_style(slot.button, active, LV_PART_MAIN);
      if (scroll && this->thumbs_visible_)
        lv_obj_scroll_to_view(slot.button, LV_ANIM_ON);
    }
  }
}

void SimpleVideoPlayer::slide_thumbnails_(bool show) {
  if (this->thumb_container_ == nullptr)
    return;
  lv_obj_t *strip = lv_obj_get_parent(this->thumb_container_);
  lv_obj_t *target = strip != nullptr ? strip : this->thumb_container_;
  if (show) {
    this->thumb_touched_ms_ = millis();
    lv_obj_set_hidden(target, false);
    lv_obj_move_foreground(target);
  } else {
    lv_obj_set_hidden(target, true);
    lv_obj_move_background(target);
  }
  // Hidden, the strip must not take the canvas's touches.
  if (strip != nullptr && this->thumb_slide_enabled_)
    lv_obj_set_clickable(strip, show);
  this->thumbs_visible_ = show;
}

const uint8_t *SimpleVideoPlayer::get_thumbnail_data(size_t index, int &width, int &height) {
  if (index < this->thumb_window_ || index >= this->thumb_window_ + this->thumb_slots_.size())
    return nullptr;
  const ThumbSlot &slot = this->thumb_slots_[index - this->thumb_window_];
  if (!slot.loaded)
    return nullptr;
  width = this->thumb_w_;
  height = this->thumb_h_;
  return slot.data;
}

lv_obj_t *SimpleVideoPlayer::get_thumbnail_button(size_t index) {
  if (index < this->thumb_window_ || index >= this->thumb_window_ + this->thumb_slots_.size())
    return nullptr;
  return this->thumb_slots_[index - this->thumb_window_].button;
}

bool SimpleVideoPlayer::request_thumbnail(size_t index) {
  if (index < this->thumb_window_ || index >= this->thumb_window_ + this->thumb_slots_.size())
    return false;
  const size_t slot = index - this->thumb_window_;
  if (this->thumb_slots_[slot].loaded)
    return true;
  this->queue_slot_(slot);
  return false;
}

//========================================================================
// The page background (SVP_BACKGROUND): a file by path through the same read and decode
//========================================================================

#ifdef SVP_BACKGROUND
static const char *const BG_TAG = "simple_video_player.background";

static constexpr size_t BG_FILE_MAX = 4 * 1024 * 1024;  // a larger file is refused
static constexpr size_t DMA_ALIGN = PsramPool::ALIGN;    // whole cache lines, as the JPEG unit needs

static constexpr size_t bg_align(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }

void SimpleVideoPlayer::set_background_path(const std::string &path) {
  if (path.size() >= sizeof(this->bg_path_)) {
    ESP_LOGW(BG_TAG, "Background path too long: %s", path.c_str());
    return;
  }
  if (path == this->bg_path_)
    return;
  snprintf(this->bg_path_, sizeof(this->bg_path_), "%s", path.c_str());
  this->bg_failed_ = false;
  this->bg_reload_ = true;
}

void SimpleVideoPlayer::set_background_wanted(bool wanted) {
  if (wanted && !this->bg_wanted_)
    this->bg_failed_ = false;  // tried again on each showing of the page
  this->bg_wanted_ = wanted;
}

// LVGL running: the canvas shows nothing (no longer reads the piece).
void SimpleVideoPlayer::bg_detach_() {
  if (!this->bg_attached_)
    return;
  lv_image_set_src(this->bg_canvas_, nullptr);
  this->bg_attached_ = false;
}

// Main loop, nothing read or decoded meanwhile: the pieces back to the pool. The canvas lets go first
// (at once while LVGL runs, else once it runs again).
void SimpleVideoPlayer::release_background_() {
  if (this->lvgl_component_ == nullptr || this->lvgl_component_->is_paused()) {
    this->bg_detach_pending_ = this->bg_detach_pending_ || this->bg_attached_;
  } else {
    this->bg_detach_();
  }
  this->bg_shown_ = false;
  this->pool_.free(this->bg_data_);
  this->bg_data_ = nullptr;
  this->bg_capacity_ = 0;
  this->pool_.free(this->bg_file_);
  this->bg_file_ = nullptr;
  this->pool_.free(this->bg_scratch_);
  this->bg_scratch_ = nullptr;
  this->bg_step_ = BgStep::IDLE;
}

// The rest of the file in one request; another only when the storage returned less.
void SimpleVideoPlayer::bg_read_next_() {
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->bg_handle_, this->bg_file_ + this->bg_len_, this->bg_limit_ - this->bg_len_, &this->bg_chunk_,
      [this](storage::StorageError result) {
        if (result != storage::StorageError::STORAGE_ERROR_OK) {
          this->bg_read_failed_ = true;
          this->bg_finish_read_();
          return;
        }
        this->bg_len_ += this->bg_chunk_;
        if (this->bg_chunk_ == 0 || this->bg_len_ + 1 == this->bg_limit_) {
          this->bg_finish_read_();  // end of file, or all of it
        } else if (this->bg_len_ == this->bg_limit_) {
          this->bg_read_failed_ = true;  // larger than when it was looked at
          this->bg_finish_read_();
        } else {
          this->bg_read_next_();
        }
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->bg_read_failed_ = true;
    this->bg_finish_read_();
  }
}

void SimpleVideoPlayer::bg_finish_read_() {
  const storage::StorageError err = storage::global_storage_worker->end_read(
      this->bg_handle_, [this](storage::StorageError) { this->bg_step_ = BgStep::READ; });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->bg_step_ = BgStep::READ;
}

void SimpleVideoPlayer::background_loop_() {
  if (this->bg_canvas_ == nullptr || !this->pic_ready_)
    return;
  // Nothing of LVGL while it is paused: everything waits for it.
  if (this->lvgl_component_ == nullptr || this->lvgl_component_->is_paused())
    return;
  if (this->bg_detach_pending_) {
    this->bg_detach_pending_ = false;
    this->bg_detach_();
  }
  switch (this->bg_step_) {
    case BgStep::IDLE: {
      // Not wanted (the page left) or another file: what shows goes back
      if ((!this->bg_wanted_ || this->bg_reload_) && (this->bg_data_ != nullptr || this->bg_attached_)) {
        this->release_background_();
        return;
      }
      this->bg_reload_ = false;
      if (!this->bg_wanted_ || this->bg_shown_ || this->bg_failed_ || this->bg_path_[0] == '\0')
        return;
      // Not while a video plays or takes the pool, nor while a picture or cover art is read or decoded
      if (this->pictures_held_() || this->pool_wanted_(POOL_PICTURES) || this->pic_read_.active)
        return;
#ifdef SVP_AUDIO_COVER
      if (this->cover_step_ != CoverStep::IDLE)
        return;
#endif
      const char *rel = nullptr;
      storage::PathStorage *storage = nullptr;
      storage::FileStat info{};
      const char *why = nullptr;
      if (storage::global_storage_registry == nullptr || storage::global_storage_worker == nullptr) {
        why = "no storage";
      } else if ((storage = storage::global_storage_registry->resolve_path(this->bg_path_, &rel)) == nullptr ||
                 rel == nullptr || rel[0] == '\0') {
        why = "storage not mounted";
      } else if (storage::global_storage_worker->has_active_task_io(storage)) {
        return;  // the worker is inside it: next loop
      } else if (storage->stat(rel, &info) != storage::StorageError::STORAGE_ERROR_OK || info.is_dir) {
        why = "not found";
      } else if (info.size == 0 || info.size > BG_FILE_MAX) {
        why = "empty or larger than 4 MB";
      }
      if (why != nullptr) {
        ESP_LOGW(BG_TAG, "%s: %s", this->bg_path_, why);
        this->bg_failed_ = true;
        return;
      }
      // The canvas' size (the configuration's) and room for the decoder's whole MCUs at it
      int32_t w = lv_obj_get_style_width(this->bg_canvas_, LV_PART_MAIN);
      int32_t h = lv_obj_get_style_height(this->bg_canvas_, LV_PART_MAIN);
      if (LV_COORD_IS_SPEC(w) || LV_COORD_IS_SPEC(h)) {
        lv_obj_update_layout(this->bg_canvas_);
        w = lv_obj_get_width(this->bg_canvas_);
        h = lv_obj_get_height(this->bg_canvas_);
      }
      if (w <= 0 || h <= 0)
        return;
      this->bg_capacity_ = bg_align(bg_align(w, 16) * bg_align(h, 16) * 2, DMA_ALIGN);
      this->bg_data_ = this->pool_take_(this->bg_capacity_, POOL_USER_PICTURES);
      this->bg_limit_ = static_cast<size_t>(info.size) + 1;
      this->bg_file_ = this->bg_data_ != nullptr
                           ? this->pool_take_(bg_align(static_cast<size_t>(info.size) + 16, DMA_ALIGN), POOL_USER_PICTURES)
                           : nullptr;
      if (this->bg_file_ == nullptr) {
        ESP_LOGW(BG_TAG, "%s: no room in the PSRAM pool", this->bg_path_);
        this->release_background_();
        this->bg_failed_ = true;
        return;
      }
      this->bg_job_data_ = PictureJob{};
      this->bg_job_data_.target_w = w;
      this->bg_job_data_.target_h = h;
      this->bg_len_ = 0;
      this->bg_read_failed_ = false;
      this->bg_step_ = BgStep::READING;
      const storage::StorageError err =
          storage::global_storage_worker->begin_read(storage, rel, &this->bg_handle_, [this](storage::StorageError result) {
            if (result != storage::StorageError::STORAGE_ERROR_OK) {
              this->bg_read_failed_ = true;
              this->bg_step_ = BgStep::READ;
              return;
            }
            this->bg_read_next_();
          });
      if (err != storage::StorageError::STORAGE_ERROR_OK) {
        this->release_background_();
        if (err != storage::StorageError::STORAGE_ERROR_NOT_READY)  // all streams busy: next loop
          this->bg_failed_ = true;
      }
      return;
    }

    case BgStep::READING:
      return;  // the worker's callbacks go on

    case BgStep::READ: {
      if (this->bg_read_failed_ || this->bg_len_ == 0 || !this->bg_wanted_ || this->bg_reload_) {
        if (this->bg_read_failed_ || this->bg_len_ == 0) {
          ESP_LOGW(BG_TAG, "Cannot read %s", this->bg_path_);
          this->bg_failed_ = true;
        }
        this->release_background_();
        return;
      }
      // The picture task: not while a picture or cover art decodes
      if (this->pic_read_.decoding || this->pictures_held_() || this->pool_wanted_(POOL_PICTURES))
        return;
#ifdef SVP_AUDIO_COVER
      if (this->cover_step_ == CoverStep::DECODING)
        return;
#endif
      PictureJob &job = this->bg_job_data_;
      job.data = this->bg_file_;
      job.length = this->bg_len_;
      job.path = this->bg_path_;
      job.target = this->bg_data_;
      job.target_capacity = this->bg_capacity_;
      job.target_stride = static_cast<uint32_t>(bg_align(job.target_w, 16) * 2);
      job.fit = this->fit_mode_;
      job.background = true;
      // A JPEG that does not go straight into the canvas' piece is decoded whole first: the pictures'
      // decode piece, for this decode only
      runtime_image::ImageFormat format;
      jpeg_decode_picture_info_t info{};
      if (picture_format(job.path, &format) && format == runtime_image::JPEG &&
          jpeg_decoder_get_info(job.data, static_cast<uint32_t>(job.length), &info) == ESP_OK && info.width != 0 &&
          info.height != 0) {
        const bool wide = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 || info.sample_method == JPEG_DOWN_SAMPLING_YUV420;
        const size_t bytes = bg_align(info.width, wide ? 16 : 8) *
                             bg_align(info.height, info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8) * 2;
        const bool straight = static_cast<int32_t>(info.width) <= job.target_w &&
                              static_cast<int32_t>(info.height) <= job.target_h &&
                              bytes <= (job.target_capacity & ~(DMA_ALIGN - 1));
        if (!straight) {
          // A piece of its own for this decode (the picture viewer's pages and files stay untouched)
          const size_t scratch = bg_align(bytes, DMA_ALIGN);
          this->bg_scratch_ = this->pool_take_(scratch, POOL_USER_PICTURES);
          if (this->bg_scratch_ == nullptr) {
            ESP_LOGW(BG_TAG, "%s: %ux%u needs %u bytes of the PSRAM pool to decode", this->bg_path_,
                     static_cast<unsigned>(info.width), static_cast<unsigned>(info.height), static_cast<unsigned>(bytes));
            this->bg_failed_ = true;
            this->release_background_();
            return;
          }
          job.scratch = this->bg_scratch_;
          job.scratch_capacity = scratch;
        }
      }
      bg_detach_();  // LVGL lets go of the piece before it is written
      this->bg_decoded_.store(false, std::memory_order_relaxed);
      this->bg_job_.store(true, std::memory_order_release);
      this->bg_step_ = BgStep::DECODING;
      xTaskNotifyGive(this->pic_task_);
      return;
    }

    case BgStep::DECODING: {
      if (!this->bg_decoded_.load(std::memory_order_acquire))
        return;
      this->bg_step_ = BgStep::IDLE;
      // The file and the decode piece go at once; the canvas' piece stays while it shows
      this->pool_.free(this->bg_file_);
      this->bg_file_ = nullptr;
      this->pool_.free(this->bg_scratch_);
      this->bg_scratch_ = nullptr;
      const PictureJob &job = this->bg_job_data_;
      if (!job.ok || !this->bg_wanted_ || this->bg_reload_) {
        if (!job.ok)
          this->bg_failed_ = true;
        this->release_background_();
        return;
      }
      const int32_t sw = job.direct ? job.width : job.target_w;
      const int32_t sh = job.direct ? job.height : job.target_h;
      const uint32_t stride = job.direct ? job.stride : job.target_stride;
      lv_draw_buf_init(&this->bg_buf_, sw, sh, LV_COLOR_FORMAT_RGB565, stride, this->bg_data_, this->bg_capacity_);
      lv_canvas_set_draw_buf(this->bg_canvas_, &this->bg_buf_);
      lv_obj_invalidate(this->bg_canvas_);
      this->bg_attached_ = true;
      this->bg_shown_ = true;
      ESP_LOGD(BG_TAG, "Showing %s (%" PRId32 "x%" PRId32 ")", this->bg_path_, sw, sh);
      return;
    }
  }
}
#endif  // SVP_BACKGROUND

}  // namespace esphome::simple_video_player

#endif  // SVP_PICTURES
