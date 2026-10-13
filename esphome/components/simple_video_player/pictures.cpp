#include "simple_video_player.h"

#ifdef SVP_PICTURES

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.pictures";

static constexpr size_t PICTURE_STEP = 1024 * 1024;  // picture buffers grow in these
static constexpr uint32_t LIST_RETRY_MS = 1000;
static constexpr size_t MAX_IMAGES = 1000;
static constexpr int32_t LONG_PRESS_SLOP = 20;  // px the finger may move and still long-press

static std::string lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return text;
}

static bool picture_format(const std::string &name, runtime_image::ImageFormat *format) {
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

void SimpleVideoPlayer::pictures_setup_() {
  if (this->pic_canvas_ == nullptr)
    return;
  if (!this->adopt_canvas_buffer_())
    return;
  this->update_picture_size_();
  lv_display_t *display = lv_obj_get_display(this->pic_canvas_);
  const int32_t dw = std::max(lv_display_get_horizontal_resolution(display), this->pic_w_);
  const int32_t dh = std::max(lv_display_get_vertical_resolution(display), this->pic_h_);
  this->pic_page_bytes_ =
      align_up(static_cast<size_t>(lv_draw_buf_width_to_stride(dw, LV_COLOR_FORMAT_RGB565)) * dh, PsramPool::ALIGN);
#if defined(USE_RUNTIME_IMAGE_PNG) || defined(USE_RUNTIME_IMAGE_BMP)
  this->pic_image_ = std::make_unique<runtime_image::RuntimeImage>(runtime_image::AUTO, image::IMAGE_TYPE_RGB565,
                                                                   image::TRANSPARENCY_OPAQUE);
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
  if (xTaskCreatePinnedToCore(picture_task_entry_, "svp_pictures", 8192, this, 1, &this->pic_task_, 1) != pdPASS) {
    ESP_LOGE(TAG, "Could not create the picture task -- pictures disabled");
    return;
  }

  if (!this->pic_dirs_.empty()) {
    this->pic_dir_index_ = 0;
    this->pic_dir_ = this->pic_dirs_[0];
    this->pic_list_pending_ = true;
  }
  this->pic_ready_ = true;
}

bool SimpleVideoPlayer::pictures_held_() const {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  return state == PlayerState::PLAYING || state == PlayerState::PAUSED;
}

void SimpleVideoPlayer::pictures_loop_() {
  if (!this->pic_ready_)
    return;
  const uint32_t now = millis();
  if (this->overlay_visible_ && now - this->overlay_shown_ms_ >= this->overlay_duration_ms_)
    this->hide_overlay_icon_();
  if (this->thumb_auto_hide_ms_ > 0 && this->thumbs_visible_ && this->thumb_slide_enabled_ &&
      now - this->thumb_touched_ms_ >= this->thumb_auto_hide_ms_)
    this->slide_thumbnails_(false);

  // A starting video needs the pool: what is in flight finishes, then the pictures' buffers go.
  if (this->pool_reclaim_req_.load(std::memory_order_acquire)) {
    if (this->pic_read_.active) {
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
    this->release_picture_buffers_();
    this->pool_reclaim_req_.store(false, std::memory_order_release);
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
    std::string configured = this->pic_dirs_[i];
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
  return this->open_directory(this->pic_dirs_[index]);
}

void SimpleVideoPlayer::refresh_images() {
  if (this->pic_dir_.empty())
    return;
  if (this->pic_select_.empty()) {
    if (const ImageEntry *entry = this->get_current_image())
      this->pic_select_ = entry->path;
  }
  this->pic_list_pending_ = true;
  this->pic_list_retry_ms_ = millis() - LIST_RETRY_MS;
}

void SimpleVideoPlayer::list_directory_() {
  this->pic_list_retry_ms_ = millis();
  if (storage::global_storage_registry == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *storage = storage::global_storage_registry->resolve_path(this->pic_dir_.c_str(), &rel);
  // Not mounted yet, or the worker reads from it on its task: tried again.
  if (storage == nullptr ||
      (storage::global_storage_worker != nullptr && storage::global_storage_worker->has_active_task_io(storage)))
    return;

  struct Context {
    std::vector<ImageEntry> found;
    const std::string *dir;
  } context{{}, &this->pic_dir_};
  const storage::StorageError err =
      storage->list_dir((rel == nullptr || rel[0] == '\0') ? "/" : rel,
                        [](const storage::FileStat *entry, void *ctx) -> bool {
                          auto *context = static_cast<Context *>(ctx);
                          runtime_image::ImageFormat format;
                          if (entry->is_dir || entry->name[0] == '.' || !picture_format(entry->name, &format))
                            return true;
                          context->found.push_back({*context->dir + "/" + entry->name, entry->name, entry->size});
                          return context->found.size() < MAX_IMAGES;
                        },
                        &context);
  this->pic_list_pending_ = false;
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGW(TAG, "Cannot list %s: %s", this->pic_dir_.c_str(), storage::error_to_string(err));
    return;
  }
  std::sort(context.found.begin(), context.found.end(),
            [](const ImageEntry &a, const ImageEntry &b) { return lower(a.filename) < lower(b.filename); });

  this->pic_generation_++;  // a read in flight belongs to the old list
  this->images_ = std::move(context.found);
  for (ThumbSlot &slot : this->thumb_slots_) {
    slot.image = -1;  // nothing carries over into the new list
    slot.loaded = false;
  }
  this->current_image_ = -1;
  this->pic_wanted_ = -1;
  ESP_LOGI(TAG, "%u pictures in %s", static_cast<unsigned>(this->images_.size()), this->pic_dir_.c_str());

  int show = -1;
  if (!this->pic_select_.empty()) {
    for (size_t i = 0; i < this->images_.size(); i++) {
      if (this->images_[i].path == this->pic_select_)
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
  this->pic_wanted_ = static_cast<int>(index);
  return true;
}

bool SimpleVideoPlayer::show_image(const std::string &path) {
  for (size_t i = 0; i < this->images_.size(); i++) {
    if (this->images_[i].path == path || this->images_[i].filename == path)
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

bool SimpleVideoPlayer::next_image() {
  if (this->images_.empty())
    return false;
  this->pic_direction_ = 1;
  const int base = this->pic_wanted_ >= 0 ? this->pic_wanted_ : this->current_image_;
  return this->show_image(static_cast<size_t>((base + 1) % static_cast<int>(this->images_.size())));
}

bool SimpleVideoPlayer::previous_image() {
  if (this->images_.empty())
    return false;
  this->pic_direction_ = -1;
  const int base = this->pic_wanted_ >= 0 ? this->pic_wanted_ : this->current_image_;
  const int count = static_cast<int>(this->images_.size());
  return this->show_image(static_cast<size_t>(base <= 0 ? count - 1 : base - 1));
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
}

void SimpleVideoPlayer::stop_slideshow() {
  const bool was_playing = this->slideshow_mode_ == SlideshowMode::PLAYING;
  this->slideshow_mode_ = SlideshowMode::STOPPED;
  if (was_playing)
    this->show_overlay_icon_(false);
}

void SimpleVideoPlayer::pause_slideshow() { this->slideshow_mode_ = SlideshowMode::PAUSED; }

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

// A file to read into: an empty or stale one, else the least recently used one that is neither
// shown nor asked for.
SimpleVideoPlayer::PicFile *SimpleVideoPlayer::free_file_() {
  PicFile *best = nullptr;
  for (PicFile &file : this->pic_files_) {
    if (file.index < 0 || file.generation != this->pic_generation_ || file.length == 0)
      return &file;
    if (file.index == this->current_image_ || file.index == this->pic_wanted_)
      continue;
    if (best == nullptr || static_cast<int32_t>(file.used_ms - best->used_ms) < 0)
      best = &file;
  }
  return best;
}

// From the pool; when it is full, the video's idle buffers make room (taken again by the next video).
uint8_t *SimpleVideoPlayer::picture_alloc_(size_t size) {
  uint8_t *piece = this->pool_.alloc(size);
  if (piece == nullptr && this->release_media_()) {
    ESP_LOGI(TAG, "Video buffers handed back to the pool for pictures");
    piece = this->pool_.alloc(size);
  }
  return piece;
}

// The pictures' buffers, all at once when pictures are first wanted (after boot or a video): the
// pages, the decode buffer for a picture larger than the canvas (up to the display's pixels) and the
// files (a quarter of that). Only a larger file or picture takes more, when it comes.
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
  if (ok && this->pic_decode_buf_ == nullptr) {
    this->pic_decode_buf_ = this->picture_alloc_(this->pic_page_bytes_);
    this->pic_decode_cap_ = this->pic_decode_buf_ != nullptr ? this->pic_page_bytes_ : 0;
  }
  ok = ok && this->pic_decode_buf_ != nullptr;
  const size_t file_bytes = align_up(this->pic_page_bytes_ / 4, PsramPool::ALIGN);
  auto take = [this, &ok, file_bytes](PicFile &file) {
    if (ok && file.data == nullptr) {
      file.data = this->picture_alloc_(file_bytes);
      file.capacity = file.data != nullptr ? file_bytes : 0;
    }
    ok = ok && file.data != nullptr;
  };
  for (PicFile &file : this->pic_files_)
    take(file);
  take(this->thumb_file_);
  if (ok)
    return true;
  ESP_LOGE(TAG, "No room in the PSRAM pool for the pictures (%u bytes)",
           static_cast<unsigned>((PIC_PAGE_COUNT + 1) * this->pic_page_bytes_ + (PIC_FILE_COUNT + 1) * file_bytes));
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

// Main loop, nothing read or decoded meanwhile: the pictures' pool buffers for the video.
void SimpleVideoPlayer::release_picture_buffers_() {
  this->pic_buffers_failed_ = false;
  if (this->pic_shown_ != nullptr) {
    // The canvas shows its own buffer meanwhile, black; the picture comes back after the video.
    lv_canvas_set_draw_buf(this->pic_canvas_, this->pic_lvgl_buf_);
    lv_canvas_fill_bg(this->pic_canvas_, lv_color_black(), LV_OPA_COVER);
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
  this->pool_.free(this->thumb_file_.data);
  this->thumb_file_ = PicFile{};
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
  const char *why = nullptr;
  const char *rel = nullptr;
  storage::PathStorage *storage = nullptr;
  PicFile *file = thumbnail ? &this->thumb_file_ : this->free_file_();
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
  if (read.preload) {
    this->start_decode_(false, read.index, 0, file, true);
    return;
  }
  if (read.thumbnail) {
    if (read.slot < this->thumb_slots_.size() && this->thumb_slots_[read.slot].image == static_cast<int>(read.index))
      this->start_decode_(true, read.index, read.slot, file);
    return;
  }
  // Shown unless another picture was asked for meanwhile (this one stays read).
  if (this->pic_wanted_ == static_cast<int>(read.index))
    this->start_decode_(false, read.index, 0, file);
}

// The pictures next to the shown one, decoded ahead while nothing else is to do: the one in the
// last step's direction first.
void SimpleVideoPlayer::preload_neighbours_() {
  const int count = static_cast<int>(this->images_.size());
  if (this->current_image_ < 0 || count < 2)
    return;
  while (this->pic_preload_tries_ < 2) {
    const int step = this->pic_preload_tries_++ == 0 ? this->pic_direction_ : -this->pic_direction_;
    const int index = (this->current_image_ + step + count) % count;
    if (this->page_of_(index) != nullptr)
      continue;
    if (PicFile *file = this->cached_file_(static_cast<size_t>(index))) {
      if (this->start_decode_(false, static_cast<size_t>(index), 0, file, true))
        return;
      continue;
    }
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
  job.path = this->images_[index].path;
  if (thumbnail) {
    job.target = this->thumb_slots_[slot].data;
    job.target_capacity = static_cast<size_t>(this->thumb_stride_) * this->thumb_h_;
    job.target_w = this->thumb_w_;
    job.target_h = this->thumb_h_;
    job.target_stride = this->thumb_stride_;
    job.fit = ImageFitMode::SCALE_TO_FIT;
  } else {
    PicPage *page = this->pic_lvgl_buf_ != nullptr ? this->free_page_() : nullptr;
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
      written_by_cpu(job.page != nullptr ? job.target : nullptr, job.target_capacity);
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
    written_by_cpu(job.page != nullptr ? job.target : nullptr, job.target_capacity);
    job.ok = true;
  } else {
    ESP_LOGW(TAG, "%s: incomplete picture, or not in PSRAM", job.path.c_str());
  }
  image->release();
}

void SimpleVideoPlayer::picture_decoded_() {
  const PictureRead read = this->pic_read_;
  this->pic_read_ = PictureRead{};
  this->pic_decoded_.store(false, std::memory_order_relaxed);
  const PictureJob &job = this->pic_job_;
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
    this->set_fullscreen(fullscreen);
  }
}

//========================================================================
// Canvas
//========================================================================

bool SimpleVideoPlayer::adopt_canvas_buffer_() {
  lv_draw_buf_t *buf = lv_canvas_get_draw_buf(this->pic_canvas_);
  if (buf == nullptr || buf->data == nullptr || buf->header.w == 0 || buf->header.h == 0 ||
      buf->header.cf != LV_COLOR_FORMAT_RGB565) {
    ESP_LOGE(TAG, "The picture canvas needs an RGB565 buffer (a canvas without transparency)");
    return false;
  }
  this->pic_lvgl_buf_ = buf;
  return true;
}

// The canvas' own size, or the widget's in full screen.
void SimpleVideoPlayer::update_picture_size_() {
  int32_t w = static_cast<int32_t>(this->pic_lvgl_buf_->header.w);
  int32_t h = static_cast<int32_t>(this->pic_lvgl_buf_->header.h);
  if (this->fullscreen_) {
    lv_obj_update_layout(this->pic_canvas_);
    const int32_t ow = lv_obj_get_width(this->pic_canvas_);
    const int32_t oh = lv_obj_get_height(this->pic_canvas_);
    if (ow > 0 && oh > 0) {
      w = ow;
      h = oh;
    }
  }
  this->pic_w_ = w;
  this->pic_h_ = h;
  this->pic_stride_ = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
}

// Main loop: the canvas shows a decoded page.
void SimpleVideoPlayer::show_page_(PicPage *page) {
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
  if (this->pic_canvas_ == nullptr || this->pic_lvgl_buf_ == nullptr)
    return;
  if (this->pic_read_.decoding && !this->pic_read_.thumbnail) {
    this->pic_fullscreen_pending_ = fullscreen ? 1 : 0;  // for the size being decoded
    return;
  }
  if (this->fullscreen_ == fullscreen)
    return;
  this->fullscreen_ = fullscreen;
  this->update_picture_size_();
  // Into a page of the new size; the old one shows until then.
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
      break;
    }
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
  // At the start of the pool for good (plan_pool_()).
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
  lv_style_init(&this->thumb_default_active_style_);
  lv_style_set_border_width(&this->thumb_default_active_style_, 2);
  lv_style_set_border_color(&this->thumb_default_active_style_, lv_color_hex(0x00FF00));

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

  this->thumb_slots_.resize(this->thumb_count_);
  for (size_t i = 0; i < this->thumb_count_; i++) {
    ThumbSlot &slot = this->thumb_slots_[i];
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
  const std::string &filename = this->images_[image].filename;
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

}  // namespace esphome::simple_video_player

#endif  // SVP_PICTURES
