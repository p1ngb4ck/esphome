#include "picture_viewer.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <algorithm>
#include <cstring>

#ifdef ESP32
#include "esp_heap_caps.h"
#include "esp_cache.h"
#endif

#ifdef USE_HARDWARE_JPEG_DECODER
#include "hal/color_types.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#endif

namespace esphome {
namespace picture_viewer {

static const char *const TAG = "picture_viewer";

// =====================================================
// Component Lifecycle
// =====================================================

PictureViewer::~PictureViewer() {
  // Free current image data
  if (this->current_image_data_ != nullptr) {
    free(this->current_image_data_);
    this->current_image_data_ = nullptr;
  }

  // Free next image data
  if (this->next_image_data_ != nullptr) {
    free(this->next_image_data_);
    this->next_image_data_ = nullptr;
  }

  // Free work buffer
  this->free_work_buffer_();

  // Free thumbnail buffer array
  this->free_thumbnail_buffer_array_();

  // Note: Don't delete transcoder's decoder - transcoder owns it

#ifdef USE_JPEGDEC
  if (this->jpeg_decoder_ != nullptr) {
    delete this->jpeg_decoder_;
    this->jpeg_decoder_ = nullptr;
  }
#endif
}

void PictureViewer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Picture Viewer...");

#ifdef USE_TRANSCODER
  // Verify transcoder is available
  if (this->transcoder_ == nullptr) {
    ESP_LOGE(TAG, "Transcoder component not set");
    this->mark_failed();
    return;
  }

  // Verify JPEG decoder is available in transcoder
  if (!this->transcoder_->is_jpeg_decoder_available()) {
    ESP_LOGE(TAG, "JPEG decoder not available in transcoder");
    this->mark_failed();
    return;
  }

  // Log decoder type based on platform
#ifdef USE_HARDWARE_JPEG_DECODER
  jpeg_decoder_handle_t test_handle = this->transcoder_->get_jpeg_decoder();
  ESP_LOGI(TAG, "Using transcoder hardware JPEG decoder (ESP32-P4, handle: %p)", test_handle);
#elif defined(USE_ESP_NEW_JPEG_DECODER)
  ESP_LOGI(TAG, "Using transcoder ESP_NEW_JPEG decoder v1.0.0 (ESP32-S2/S3 with SIMD)");
#endif
#else
#ifdef USE_JPEGDEC
  // Initialize JPEGDec library (fallback when transcoder not available)
  this->jpeg_decoder_ = new JPEGDEC();
  ESP_LOGI(TAG, "JPEGDec library initialized");
#endif
#endif

  // NOTE: File monitoring is now handled by file_manager
  // file_manager will call update_directory_files_() when directories change
  // No callback registration needed here - file_manager pushes updates directly
#ifdef USE_STORAGE
  if (this->file_manager_ != nullptr) {
    ESP_LOGI(TAG, "File manager available - will receive directory updates via update_directory_files_()");
  }
#endif

  // Allocate work buffer for JPEG decoding
  if (!this->allocate_work_buffer_()) {
    ESP_LOGE(TAG, "Failed to allocate work buffer");
    this->mark_failed();
    return;
  }

  // Allocate thumbnail buffer array
  if (!this->allocate_thumbnail_buffer_array_()) {
    ESP_LOGE(TAG, "Failed to allocate thumbnail buffer array");
    this->mark_failed();
    return;
  }

  // Initialize thumbnail cache with pointers into buffer array
  if (this->thumbnail_config_.enabled && this->thumbnail_buffer_array_ != nullptr) {
    this->thumbnail_cache_.resize(this->thumbnail_config_.max_count);
    for (size_t i = 0; i < this->thumbnail_config_.max_count; i++) {
      this->thumbnail_cache_[i].data = this->thumbnail_buffer_array_ + (i * this->thumbnail_buffer_size_per_slot_);
      this->thumbnail_cache_[i].image_index = -1;
      this->thumbnail_cache_[i].loaded = false;
    }
    ESP_LOGI(TAG, "Initialized thumbnail cache with %zu slots", this->thumbnail_config_.max_count);

#ifdef USE_ESP_IDF
    // If images were already loaded (race condition: file_manager called update_directory_files_ before setup),
    // start preload task now that buffer is allocated
    if (!this->images_.empty()) {
      ESP_LOGI(TAG, "Images already loaded (%zu) - starting preload task now", this->images_.size());
      this->start_preload_task_();
    }
#endif
  }

#ifdef USE_LVGL
  // Create dynamic thumbnail widgets if container is provided
  if (this->thumbnail_container_ != nullptr && this->thumbnail_config_.enabled) {
    this->create_thumbnail_widgets_();
  }

  // Configure canvas to handle swipe gestures and prevent page scrolling
  if (this->canvas_ != nullptr) {
    // Make canvas clickable so it receives touch events
    lv_obj_add_flag(this->canvas_, LV_OBJ_FLAG_CLICKABLE);

    // Set clean default styles on canvas (remove any borders/padding)
    lv_obj_set_style_pad_all(this->canvas_, 0, 0);
    lv_obj_set_style_border_width(this->canvas_, 0, 0);
    lv_obj_set_style_radius(this->canvas_, 0, 0);

    // Also check and clean the canvas parent container
    lv_obj_t *parent = lv_obj_get_parent(this->canvas_);
    if (parent != nullptr) {
      lv_obj_set_style_pad_all(parent, 0, 0);
      lv_obj_set_style_border_width(parent, 0, 0);
      lv_obj_set_style_radius(parent, 0, 0);
      ESP_LOGD(TAG, "Cleaned canvas parent container styles");

      // Add event handler to detect when parent page becomes hidden (page change)
      // This automatically stops slideshow when user navigates away
      lv_obj_add_event_cb(
          parent,
          [](lv_event_t *e) {
            auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));
            lv_event_code_t code = lv_event_get_code(e);

            // LV_EVENT_SCREEN_UNLOAD_START fires when page is about to be hidden
            if (code == LV_EVENT_SCREEN_UNLOAD_START) {
              if (viewer->get_slideshow_mode() == SlideshowMode::PLAYING) {
                ESP_LOGI(TAG, "Parent page hidden - stopping slideshow");
                viewer->stop_slideshow();
              }
            }
          },
          LV_EVENT_ALL, this);
      ESP_LOGD(TAG, "Added page visibility event handler to stop slideshow on page change");
    }

    // Add pressed event handler to capture gesture start position
    lv_obj_add_event_cb(
        this->canvas_,
        [](lv_event_t *e) {
          auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));
          lv_indev_t *indev = lv_indev_get_act();
          if (indev == nullptr)
            return;

          lv_point_t point;
          lv_indev_get_point(indev, &point);
          viewer->gesture_start_x_ = point.x;
          viewer->gesture_start_y_ = point.y;
        },
        LV_EVENT_PRESSED, this);

    // Add gesture event handler for swipe detection
    lv_obj_add_event_cb(
        this->canvas_,
        [](lv_event_t *e) {
          auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));
          lv_indev_t *indev = lv_indev_get_act();
          if (indev == nullptr)
            return;

          lv_dir_t dir = lv_indev_get_gesture_dir(indev);
          if (dir == LV_DIR_NONE)
            return;

          // Mark that a gesture was detected to prevent long press from firing
          viewer->gesture_detected_ = true;
          viewer->last_gesture_time_ = millis();

          ESP_LOGD(TAG, "Gesture on canvas: dir=%d, slide_enabled=%d, container=%p", dir,
                   viewer->thumbnail_slide_enabled_, viewer->thumbnail_container_);

          // Check if this is an edge swipe for thumbnail slide
          if (viewer->thumbnail_slide_enabled_ && viewer->thumbnail_container_ != nullptr) {
            // Use gesture START position to determine if swipe began in edge area
            lv_coord_t start_x = viewer->gesture_start_x_;
            lv_coord_t start_y = viewer->gesture_start_y_;

            lv_coord_t screen_width = lv_obj_get_width(lv_scr_act());
            lv_coord_t screen_height = lv_obj_get_height(lv_scr_act());

            // Calculate edge threshold based on thumbnail bar size
            // In VERTICAL layout: use thumbnail width (bar is on left/right edge)
            // In HORIZONTAL layout: use thumbnail height (bar is on top/bottom edge)
            lv_coord_t edge_threshold_h = (viewer->thumbnail_config_.layout == ThumbnailLayout::VERTICAL)
                                              ? viewer->thumbnail_config_.width
                                              : screen_width / 10;  // fallback for horizontal
            lv_coord_t edge_threshold_v = (viewer->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL)
                                              ? viewer->thumbnail_config_.height
                                              : screen_height / 10;  // fallback for vertical

            ESP_LOGD(TAG, "Gesture detected: start=(%d,%d) dir=%d threshold_h=%d threshold_v=%d visible=%d", start_x,
                     start_y, dir, edge_threshold_h, edge_threshold_v, viewer->thumbnails_visible_);

            bool is_edge_swipe = false;
            bool started_in_edge = false;

            switch (viewer->thumbnail_slide_edge_) {
              case ThumbnailSlideEdge::RIGHT:
                // Check if gesture STARTED in right edge area
                started_in_edge = (start_x > screen_width - edge_threshold_h);
                ESP_LOGD(TAG, "RIGHT edge check: start_x=%d > threshold=%d = %d", start_x,
                         screen_width - edge_threshold_h, started_in_edge);
                // Slide in: started in edge AND not visible AND swipe left (inward from right edge)
                // Slide out: visible AND swipe right (outward toward right edge)
                if (started_in_edge && !viewer->thumbnails_visible_ && (dir == LV_DIR_LEFT)) {
                  is_edge_swipe = true;
                  ESP_LOGD(TAG, "Matched slide-IN condition");
                } else if (viewer->thumbnails_visible_ && (dir == LV_DIR_RIGHT)) {
                  is_edge_swipe = true;  // Slide out (can start anywhere)
                  ESP_LOGD(TAG, "Matched slide-OUT condition");
                }
                break;
              case ThumbnailSlideEdge::LEFT:
                started_in_edge = (start_x < edge_threshold_h);
                if (started_in_edge && !viewer->thumbnails_visible_ && (dir == LV_DIR_RIGHT)) {
                  is_edge_swipe = true;
                } else if (viewer->thumbnails_visible_ && (dir == LV_DIR_LEFT)) {
                  is_edge_swipe = true;  // Slide out
                }
                break;
              case ThumbnailSlideEdge::TOP:
                started_in_edge = (start_y < edge_threshold_v);
                if (started_in_edge && !viewer->thumbnails_visible_ && (dir == LV_DIR_BOTTOM)) {
                  is_edge_swipe = true;
                } else if (viewer->thumbnails_visible_ && (dir == LV_DIR_TOP)) {
                  is_edge_swipe = true;  // Slide out
                }
                break;
              case ThumbnailSlideEdge::BOTTOM:
                started_in_edge = (start_y > screen_height - edge_threshold_v);
                if (started_in_edge && !viewer->thumbnails_visible_ && (dir == LV_DIR_TOP)) {
                  is_edge_swipe = true;
                } else if (viewer->thumbnails_visible_ && (dir == LV_DIR_BOTTOM)) {
                  is_edge_swipe = true;  // Slide out
                }
                break;
            }

            if (is_edge_swipe) {
              ESP_LOGD(TAG, "Edge swipe detected (started at %d,%d) - sliding thumbnails %s", start_x, start_y,
                       viewer->thumbnails_visible_ ? "OUT" : "IN");
              viewer->slide_thumbnails(!viewer->thumbnails_visible_);
              return;  // Don't process as image navigation
            }
          }

          // Handle vertical swipes for directory switching (if multiple directories configured)
          if (viewer->directories_.size() > 1) {
            if (dir == LV_DIR_TOP) {
              // Swipe up -> next directory (with wraparound)
              size_t next_dir = (viewer->current_directory_index_ + 1) % viewer->directories_.size();
              ESP_LOGI(TAG, "Canvas swipe up - switching to next directory %zu", next_dir);
              viewer->set_current_directory(next_dir);
              return;  // Don't process as image navigation
            } else if (dir == LV_DIR_BOTTOM) {
              // Swipe down -> previous directory (with wraparound)
              size_t prev_dir = (viewer->current_directory_index_ == 0) ? viewer->directories_.size() - 1
                                                                        : viewer->current_directory_index_ - 1;
              ESP_LOGI(TAG, "Canvas swipe down - switching to previous directory %zu", prev_dir);
              viewer->set_current_directory(prev_dir);
              return;  // Don't process as image navigation
            }
          }

          // Regular swipe for image navigation (ALWAYS stops slideshow)
          if (dir == LV_DIR_LEFT) {
            bool was_playing = (viewer->get_slideshow_mode() == SlideshowMode::PLAYING);
            if (was_playing) {
              ESP_LOGD(TAG, "Canvas swipe left - stopping slideshow and next image");
            } else {
              ESP_LOGD(TAG, "Canvas swipe left - next image");
            }
            viewer->stop_slideshow();  // Always stop, never toggle
            viewer->next_image();
            // Show pause overlay AFTER image changes (if slideshow was playing)
            if (was_playing) {
              viewer->show_overlay_icon_(false);
            }
          } else if (dir == LV_DIR_RIGHT) {
            bool was_playing = (viewer->get_slideshow_mode() == SlideshowMode::PLAYING);
            if (was_playing) {
              ESP_LOGD(TAG, "Canvas swipe right - stopping slideshow and previous image");
            } else {
              ESP_LOGD(TAG, "Canvas swipe right - previous image");
            }
            viewer->stop_slideshow();  // Always stop, never toggle
            viewer->previous_image();
            // Show pause overlay AFTER image changes (if slideshow was playing)
            if (was_playing) {
              viewer->show_overlay_icon_(false);
            }
          }
        },
        LV_EVENT_GESTURE, this);

    // Set long press time on the input device (LVGL 8.4.0)
    // In LVGL 8.4.0, driver is a pointer in the indev structure
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != nullptr && indev->driver != nullptr) {
      indev->driver->long_press_time = this->long_press_time_ms_;
      ESP_LOGI(TAG, "Set LVGL long press time to %ums", this->long_press_time_ms_);
    }

    // Add long press event handler to toggle slideshow (only if NOT dragging/gesturing)
    lv_obj_add_event_cb(
        this->canvas_,
        [](lv_event_t *e) {
          auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));

          // Check if user is currently dragging/gesturing
          lv_indev_t *indev = lv_indev_get_act();
          if (indev != nullptr) {
            // Get the gesture direction - if any gesture is active, ignore long press
            lv_dir_t dir = lv_indev_get_gesture_dir(indev);
            if (dir != LV_DIR_NONE) {
              ESP_LOGD(TAG, "Ignoring long press - gesture in progress (dir=%d)", dir);
              return;
            }
          }

          // Also check recent gesture flag (within 500ms)
          uint32_t now = millis();
          if (viewer->gesture_detected_ && (now - viewer->last_gesture_time_ < 500)) {
            ESP_LOGD(TAG, "Ignoring long press - recent gesture detected");
            viewer->gesture_detected_ = false;  // Reset flag
            return;
          }

          ESP_LOGI(TAG, "Canvas long press - toggling slideshow");

          // Toggle slideshow state
          if (viewer->get_slideshow_mode() == SlideshowMode::PLAYING) {
            viewer->stop_slideshow();
          } else {
            viewer->start_slideshow();
          }
        },
        LV_EVENT_LONG_PRESSED, this);

    // Stop event bubbling to prevent parent page from scrolling
    lv_obj_clear_flag(this->canvas_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(this->canvas_, LV_OBJ_FLAG_SCROLLABLE);

    ESP_LOGI(TAG, "Canvas configured: clickable=true, long_press_time=%ums, clean_borders=true",
             this->long_press_time_ms_);
  }
#endif

  // Initial image list will be provided by file_manager via update_directory_files_()
  // file_manager should call update_directory_files_() for each configured directory after setup
  if (!this->directories_.empty()) {
    ESP_LOGI(TAG, "Configured %zu directories - waiting for file_manager to provide initial file lists",
             this->directories_.size());
    for (const auto &dir : this->directories_) {
      ESP_LOGI(TAG, "  - Directory: %s", dir.path.c_str());
    }
  } else {
    ESP_LOGW(TAG, "No directories configured");
  }

  ESP_LOGCONFIG(TAG, "Picture Viewer setup complete");
}

void PictureViewer::loop() {
  uint32_t now = millis();

  // Handle slideshow
  if (this->slideshow_mode_ == SlideshowMode::PLAYING) {
    if (now - this->last_slideshow_time_ >= this->slideshow_interval_ms_) {
      this->next_image();
      this->last_slideshow_time_ = now;
    }
  }

#ifdef USE_LVGL
  // Handle overlay timeout
  if (this->overlay_visible_ && (now - this->overlay_show_time_ >= this->overlay_duration_ms_)) {
    this->hide_overlay_icon_();
  }

  // Handle thumbnail auto-hide timeout
  if (this->thumbnail_auto_hide_timeout_ > 0 && this->thumbnails_visible_ && this->thumbnail_slide_enabled_) {
    if (now - this->last_thumbnail_interaction_time_ >= this->thumbnail_auto_hide_timeout_) {
      ESP_LOGD(TAG, "Auto-hiding thumbnails after %u ms of inactivity", this->thumbnail_auto_hide_timeout_);
      this->slide_thumbnails(false);
    }
  }
#endif
}

void PictureViewer::dump_config() {
  ESP_LOGCONFIG(TAG, "Picture Viewer:");
  ESP_LOGCONFIG(TAG, "  Directories: %zu", this->directories_.size());
  for (size_t i = 0; i < this->directories_.size(); i++) {
    const auto &dir = this->directories_[i];
    ESP_LOGCONFIG(TAG, "    [%zu] %s (RGB:%d, CS:%d, Fmt:0x%08x)%s", i, dir.path.c_str(), dir.jpeg_rgb_order,
                  dir.jpeg_color_space, dir.jpeg_output_format,
                  (i == this->current_directory_index_) ? " <- CURRENT" : "");
  }
  ESP_LOGCONFIG(TAG, "  Image Count: %zu", this->images_.size());
  ESP_LOGCONFIG(TAG, "  Slideshow Interval: %u ms", this->slideshow_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Thumbnails: %s", this->enable_thumbnails_ ? "enabled" : "disabled");
  if (this->enable_thumbnails_) {
    ESP_LOGCONFIG(TAG, "  Thumbnail Size: %dx%d", this->thumbnail_width_, this->thumbnail_height_);
    ESP_LOGCONFIG(TAG, "  Thumbnail Slide: %s (edge: %d)", this->thumbnail_slide_enabled_ ? "enabled" : "disabled",
                  (int) this->thumbnail_slide_edge_);
  }
#ifdef USE_ESP_NEW_JPEG_DECODER
  ESP_LOGCONFIG(TAG, "  Decoder: ESP_NEW_JPEG v1.0.0 (ESP32-S2/S3 with SIMD)");
#elif defined(USE_HARDWARE_JPEG_DECODER)
  ESP_LOGCONFIG(TAG, "  Decoder: Hardware JPEG (ESP32-P4)");
#elif defined(USE_JPEGDEC)
  ESP_LOGCONFIG(TAG, "  Decoder: JPEGDec (software)");
#endif
}

// =====================================================
// Picture Control API
// =====================================================

bool PictureViewer::show_image(size_t index) {
  if (index >= this->images_.size()) {
    ESP_LOGW(TAG, "Image index out of range: %zu (total: %zu)", index, this->images_.size());
    return false;
  }

  const auto &entry = this->images_[index];
  ESP_LOGD(TAG, "Showing image %zu: %s", index, entry.filename.c_str());

  // Load and decode image
  std::vector<uint8_t> rgb565_data;
  int width, height;

  // Get target dimensions
  this->update_canvas_dimensions_();
  int target_width = this->fullscreen_ ? this->canvas_width_ : 0;
  int target_height = this->fullscreen_ ? this->canvas_height_ : 0;

  if (!this->load_image_(entry.path, rgb565_data, width, height, target_width, target_height)) {
    ESP_LOGE(TAG, "Failed to load image: %s", entry.path.c_str());
    return false;
  }

  // Free old current image
  if (this->current_image_data_ != nullptr) {
    free(this->current_image_data_);
    this->current_image_data_ = nullptr;
    this->current_image_size_ = 0;
  }

  // Allocate new buffer in PSRAM/heap
  this->current_image_size_ = rgb565_data.size();
  this->current_image_data_ = this->allocate_image_buffer_(this->current_image_size_);
  if (this->current_image_data_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate image buffer: %zu bytes (check PSRAM availability)", this->current_image_size_);
    this->current_image_size_ = 0;
    this->current_image_width_ = 0;
    this->current_image_height_ = 0;
    return false;
  }

  // Copy decoded data to buffer
  std::memcpy(this->current_image_data_, rgb565_data.data(), this->current_image_size_);
  this->current_image_width_ = width;
  this->current_image_height_ = height;
  this->current_index_ = index;

  // Update canvas
  this->update_canvas_();

  ESP_LOGI(TAG, "Displayed image: %s (%dx%d)", entry.filename.c_str(), width, height);
  return true;
}

bool PictureViewer::show_image(const std::string &path) {
  // Find image by path
  for (size_t i = 0; i < this->images_.size(); i++) {
    if (this->images_[i].path == path || this->images_[i].filename == path) {
      return this->show_image(i);
    }
  }

  ESP_LOGW(TAG, "Image not found: %s", path.c_str());
  return false;
}

bool PictureViewer::next_image() {
  if (this->images_.empty()) {
    ESP_LOGW(TAG, "No images available");
    return false;
  }

  int next_index = (this->current_index_ + 1) % static_cast<int>(this->images_.size());
  return this->show_image(next_index);
}

bool PictureViewer::previous_image() {
  if (this->images_.empty()) {
    ESP_LOGW(TAG, "No images available");
    return false;
  }

  int prev_index = this->current_index_ - 1;
  if (prev_index < 0) {
    prev_index = static_cast<int>(this->images_.size()) - 1;
  }
  return this->show_image(prev_index);
}

void PictureViewer::start_slideshow() {
  if (this->images_.empty()) {
    ESP_LOGW(TAG, "No images available for slideshow");
    return;
  }

  ESP_LOGI(TAG, "Starting slideshow");
  this->slideshow_mode_ = SlideshowMode::PLAYING;
  this->last_slideshow_time_ = millis();

  // Show first image if none is currently displayed
  if (this->current_index_ < 0) {
    this->show_image(0);
  }

#ifdef USE_LVGL
  this->show_overlay_icon_(true);  // Show play icon
#endif
}

void PictureViewer::stop_slideshow() {
  // Check if slideshow was actually playing before stopping
  bool was_playing = (this->slideshow_mode_ == SlideshowMode::PLAYING);

  if (was_playing) {
    ESP_LOGI(TAG, "Stopping slideshow");
  }

  this->slideshow_mode_ = SlideshowMode::STOPPED;

#ifdef USE_LVGL
  // Only show overlay if slideshow was actually playing
  if (was_playing) {
    this->show_overlay_icon_(false);  // Show pause icon
  }
#endif
}

void PictureViewer::pause_slideshow() {
  ESP_LOGI(TAG, "Pausing slideshow");
  this->slideshow_mode_ = SlideshowMode::PAUSED;
}

void PictureViewer::toggle_slideshow() {
  if (this->slideshow_mode_ == SlideshowMode::PLAYING) {
    this->pause_slideshow();
#ifdef USE_LVGL
    this->show_overlay_icon_(false);  // Show pause icon
#endif
  } else {
    this->start_slideshow();
#ifdef USE_LVGL
    this->show_overlay_icon_(true);  // Show play icon
#endif
  }
}

void PictureViewer::refresh_images() {
  const DirectoryConfig *current_dir = this->get_current_directory();
  if (current_dir == nullptr) {
    ESP_LOGW(TAG, "No current directory configured");
    return;
  }

  ESP_LOGI(TAG, "Requesting file list refresh from file_manager for: %s", current_dir->path.c_str());

#ifdef USE_STORAGE
  if (this->file_manager_ != nullptr) {
    // Force an immediate scan to ensure directory_state_ is populated
    this->file_manager_->scan_now();

    // Get current file list from file_manager's directory state
    const auto &directory_state = this->file_manager_->get_directory_state();

    // Normalize directory path (remove trailing slash for comparison)
    std::string normalized_dir = current_dir->path;
    if (!normalized_dir.empty() && normalized_dir.back() == '/') {
      normalized_dir.pop_back();
    }

    ESP_LOGD(TAG, "Total files in file_manager state: %zu", directory_state.size());
    ESP_LOGD(TAG, "Normalized directory path: '%s'", normalized_dir.c_str());

    // Filter files that belong to the current directory
    std::vector<storage::FileInfo> directory_files;
    for (const auto &entry : directory_state) {
      const storage::FileInfo &file_info = entry.second;

      // Normalize file's directory path (remove trailing slash)
      std::string file_dir = file_info.directory;
      if (!file_dir.empty() && file_dir.back() == '/') {
        file_dir.pop_back();
      }

      ESP_LOGVV(TAG, "Comparing file dir '%s' with target '%s' (file: %s)", file_dir.c_str(), normalized_dir.c_str(),
                file_info.filename.c_str());

      // Check if this file belongs to the current directory
      if (file_dir == normalized_dir) {
        directory_files.push_back(file_info);
        ESP_LOGD(TAG, "  Matched: %s", file_info.filename.c_str());
      }
    }

    ESP_LOGI(TAG, "Retrieved %zu files from file_manager for directory: %s", directory_files.size(),
             current_dir->path.c_str());

    // Process the file list directly (instead of waiting for file_manager to push)
    this->update_directory_files_(current_dir->path, directory_files);
  } else {
    ESP_LOGW(TAG, "No file_manager available - cannot refresh images");
  }
#else
  ESP_LOGW(TAG, "Storage host support not available");
#endif

  ESP_LOGI(TAG, "Found %zu images", this->images_.size());
}

void PictureViewer::set_fullscreen(bool fullscreen) {
  if (this->fullscreen_ == fullscreen) {
    return;
  }

  ESP_LOGI(TAG, "Setting fullscreen: %s", fullscreen ? "true" : "false");
  this->fullscreen_ = fullscreen;

  // Update canvas dimensions and resize buffer BEFORE reloading image
  this->update_canvas_dimensions_();
  this->resize_canvas_buffer_();

  // Reload current image with new dimensions
  if (this->current_index_ >= 0) {
    this->show_image(this->current_index_);
  }
}

// =====================================================
// Internal Methods
// =====================================================

void PictureViewer::scan_directory_(const std::vector<storage::FileInfo> &files) {
#ifdef USE_STORAGE
  const DirectoryConfig *current_dir = this->get_current_directory();
  if (current_dir == nullptr) {
    ESP_LOGW(TAG, "No current directory configured for scanning");
    return;
  }

  ESP_LOGD(TAG, "Scanning %zu files for images in directory: '%s'", files.size(), current_dir->path.c_str());

  size_t image_count = 0;
  size_t matched_count = 0;

  for (const auto &file : files) {
    ESP_LOGV(TAG, "Examining file: path='%s', filename='%s'", file.path.c_str(), file.filename.c_str());

    // Filter by current directory
    if (!current_dir->path.empty()) {
      // Check if file path starts with current directory
      if (file.path.find(current_dir->path) != 0) {
        ESP_LOGV(TAG, "  Skipping - not in current directory");
        continue;
      }
    }

    // Filter image files (JPEG, PNG, BMP)
    std::string lower_filename = file.filename;
    std::transform(lower_filename.begin(), lower_filename.end(), lower_filename.begin(), ::tolower);

    bool is_supported_image = false;
    const char *image_type = nullptr;

    if (lower_filename.ends_with(".jpg") || lower_filename.ends_with(".jpeg")) {
      is_supported_image = true;
      image_type = "JPEG";
    }
#ifdef USE_PNG_DECODER
    else if (lower_filename.ends_with(".png")) {
      is_supported_image = true;
      image_type = "PNG";
    }
#endif
#ifdef USE_BMP_DECODER
    else if (lower_filename.ends_with(".bmp")) {
      is_supported_image = true;
      image_type = "BMP";
    }
#endif

    if (is_supported_image) {
      image_count++;
      ESP_LOGD(TAG, "  Found %s: %s (size: %llu bytes)", image_type, file.filename.c_str(),
               (unsigned long long) file.size);
      ImageEntry entry;
      entry.path = file.path;
      entry.filename = file.filename;
      entry.size = file.size;
      this->images_.push_back(entry);
      matched_count++;
    }
  }

  ESP_LOGD(TAG, "Scan complete: %zu images found, %zu matched filters", image_count, matched_count);

  // Sort by filename
  std::sort(this->images_.begin(), this->images_.end(),
            [](const ImageEntry &a, const ImageEntry &b) { return a.filename < b.filename; });

  // Show default image if configured and images are available
  if (this->default_image_index_ >= 0 && !this->images_.empty()) {
    size_t index_to_show = static_cast<size_t>(this->default_image_index_);
    if (index_to_show < this->images_.size()) {
      ESP_LOGI(TAG, "Showing default image at index %zu", index_to_show);
      this->show_image(index_to_show);
    } else {
      ESP_LOGW(TAG, "Default image index %zu out of range (only %zu images)", index_to_show, this->images_.size());
    }
  }

#ifdef USE_LVGL
  // Preload initial thumbnails after images are loaded
  if (this->thumbnail_config_.enabled && !this->images_.empty()) {
    ESP_LOGI(TAG, "Preloading initial thumbnails for %zu images", this->images_.size());
    this->preload_thumbnails_for_viewport_();
  }
#endif
#endif
}

void PictureViewer::update_directory_files_(const std::string &directory_path,
                                            const std::vector<storage::FileInfo> &files) {
#ifdef USE_STORAGE
  ESP_LOGI(TAG, "Received file list update for directory: %s (%zu files)", directory_path.c_str(), files.size());

  // Find which configured directory this update is for
  bool found_directory = false;
  for (size_t i = 0; i < this->directories_.size(); i++) {
    if (this->directories_[i].path == directory_path) {
      found_directory = true;

      // Update to this directory if it matches
      if (i == this->current_directory_index_) {
        ESP_LOGI(TAG, "Updating current directory images");

        // Clear existing images
        this->images_.clear();
        this->current_index_ = -1;

        // Clear thumbnail cache so old thumbnails don't show for new files
        for (auto &entry : this->thumbnail_cache_) {
          entry.image_index = -1;
          entry.loaded = false;
          // Keep buffer and widgets allocated, just mark as unloaded
        }
        ESP_LOGD(TAG, "Cleared thumbnail cache for directory update");

        // Scan the new file list
        this->scan_directory_(files);

        ESP_LOGI(TAG, "After directory update: %zu images found", this->images_.size());

#ifdef USE_LVGL
        // Hide thumbnail widgets that exceed the number of images
        // (Do this AFTER images are loaded, not during setup when images_.size() is 0)
        if (this->thumbnail_config_.enabled && this->thumbnail_container_ != nullptr) {
          for (size_t i = this->images_.size(); i < this->thumbnail_config_.max_count; i++) {
            if (i < this->thumbnail_cache_.size() && this->thumbnail_cache_[i].thumb_btn_ != nullptr) {
              lv_obj_add_flag(this->thumbnail_cache_[i].thumb_btn_, LV_OBJ_FLAG_HIDDEN);
            }
          }
          ESP_LOGD(TAG, "Hidden %zu unused thumbnail widgets",
                   this->thumbnail_config_.max_count - this->images_.size());
        }
#endif

#ifdef USE_ESP_IDF
        // Start background preloading task for thumbnails
        if (this->thumbnail_config_.enabled && !this->images_.empty()) {
          this->start_preload_task_();
        }
#endif

        // Show first image from new directory if images are available
        if (!this->images_.empty() && this->current_index_ == -1) {
          ESP_LOGI(TAG, "Showing first image from directory %zu", i);
          this->show_image(0);
        }
      } else {
        ESP_LOGD(TAG, "Update is for directory index %zu, but current is %zu - ignoring", i,
                 this->current_directory_index_);
      }
      break;
    }
  }

  if (!found_directory) {
    ESP_LOGW(TAG, "Received file list for unrecognized directory: %s", directory_path.c_str());
  }
#endif
}

// =====================================================
// Buffer Management
// =====================================================

bool PictureViewer::allocate_work_buffer_() {
  // Calculate work buffer size from config
  this->decode_work_buffer_size_ = this->thumbnail_config_.decode_work_buffer_size;

  ESP_LOGI(TAG, "Allocating work buffer: %zu bytes", this->decode_work_buffer_size_);

  // Allocate in PSRAM if available
#ifdef CONFIG_SPIRAM
  this->decode_work_buffer_ = (uint8_t *) heap_caps_malloc(this->decode_work_buffer_size_, MALLOC_CAP_SPIRAM);
  if (this->decode_work_buffer_ != nullptr) {
    ESP_LOGI(TAG, "Work buffer allocated in PSRAM");
    return true;
  }
  ESP_LOGW(TAG, "PSRAM allocation failed for work buffer, trying heap");
#endif

  // Fallback to regular heap
  this->decode_work_buffer_ = (uint8_t *) malloc(this->decode_work_buffer_size_);
  if (this->decode_work_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate work buffer: %zu bytes", this->decode_work_buffer_size_);
    return false;
  }

  ESP_LOGI(TAG, "Work buffer allocated in heap");
  return true;
}

void PictureViewer::free_work_buffer_() {
  if (this->decode_work_buffer_ != nullptr) {
    free(this->decode_work_buffer_);
    this->decode_work_buffer_ = nullptr;
    this->decode_work_buffer_size_ = 0;
    ESP_LOGD(TAG, "Work buffer freed");
  }
}

bool PictureViewer::allocate_thumbnail_buffer_array_() {
  if (!this->thumbnail_config_.enabled) {
    ESP_LOGD(TAG, "Thumbnails disabled, skipping buffer allocation");
    return true;
  }

  // Calculate buffer size per thumbnail slot
  this->thumbnail_buffer_size_per_slot_ =
      (size_t) this->thumbnail_config_.width * (size_t) this->thumbnail_config_.height * 2;  // RGB565 = 2 bytes/pixel

  const size_t total_size = this->thumbnail_buffer_size_per_slot_ * this->thumbnail_config_.max_count;

  ESP_LOGI(TAG, "Allocating thumbnail buffer array: %zu thumbnails × %zu bytes = %zu bytes (%.2f MB)",
           this->thumbnail_config_.max_count, this->thumbnail_buffer_size_per_slot_, total_size,
           total_size / (1024.0 * 1024.0));

  // Allocate in PSRAM if available
#ifdef CONFIG_SPIRAM
  this->thumbnail_buffer_array_ = (uint8_t *) heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
  if (this->thumbnail_buffer_array_ != nullptr) {
    ESP_LOGI(TAG, "Thumbnail buffer array allocated in PSRAM");
    memset(this->thumbnail_buffer_array_, 0, total_size);  // Clear to black
    return true;
  }
  ESP_LOGW(TAG, "PSRAM allocation failed for thumbnail buffer array, trying heap");
#endif

  // Fallback to regular heap
  this->thumbnail_buffer_array_ = (uint8_t *) malloc(total_size);
  if (this->thumbnail_buffer_array_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate thumbnail buffer array: %zu bytes", total_size);
    return false;
  }

  memset(this->thumbnail_buffer_array_, 0, total_size);  // Clear to black
  ESP_LOGI(TAG, "Thumbnail buffer array allocated in heap");
  return true;
}

void PictureViewer::free_thumbnail_buffer_array_() {
  if (this->thumbnail_buffer_array_ != nullptr) {
    free(this->thumbnail_buffer_array_);
    this->thumbnail_buffer_array_ = nullptr;
    this->thumbnail_buffer_size_per_slot_ = 0;
    ESP_LOGD(TAG, "Thumbnail buffer array freed");
  }
}

#ifdef USE_LVGL
// =====================================================
// LVGL Widget Generation
// =====================================================

void PictureViewer::init_default_thumbnail_styles_() {
  if (this->default_styles_initialized_) {
    return;
  }

  // Initialize default thumbnail style - clean, no borders, no padding
  lv_style_init(&this->default_thumbnail_style_);
  lv_style_set_pad_all(&this->default_thumbnail_style_, 0);
  lv_style_set_border_width(&this->default_thumbnail_style_, 0);
  lv_style_set_radius(&this->default_thumbnail_style_, 0);

  // Initialize default active thumbnail style - subtle highlight
  lv_style_init(&this->default_thumbnail_active_style_);
  lv_style_set_pad_all(&this->default_thumbnail_active_style_, 0);
  lv_style_set_border_width(&this->default_thumbnail_active_style_, 2);
  lv_style_set_border_color(&this->default_thumbnail_active_style_, lv_color_hex(0x00FF00));
  lv_style_set_radius(&this->default_thumbnail_active_style_, 0);

  // Initialize default container style - clean, scrollable OFF
  lv_style_init(&this->default_container_style_);
  lv_style_set_pad_all(&this->default_container_style_, 0);
  lv_style_set_border_width(&this->default_container_style_, 0);
  lv_style_set_radius(&this->default_container_style_, 0);

  this->default_styles_initialized_ = true;
  ESP_LOGD(TAG, "Initialized default thumbnail styles");
}

void PictureViewer::apply_thumbnail_style_(lv_obj_t *obj, bool is_active) {
  if (is_active) {
    if (this->thumbnail_active_style_ != nullptr) {
      lv_obj_add_style(obj, this->thumbnail_active_style_, 0);
    } else {
      this->init_default_thumbnail_styles_();
      lv_obj_add_style(obj, &this->default_thumbnail_active_style_, 0);
    }
  } else {
    if (this->thumbnail_style_ != nullptr) {
      lv_obj_add_style(obj, this->thumbnail_style_, 0);
    } else {
      this->init_default_thumbnail_styles_();
      lv_obj_add_style(obj, &this->default_thumbnail_style_, 0);
    }
  }
}

void PictureViewer::apply_container_style_(lv_obj_t *obj) {
  if (this->thumbnail_container_style_ != nullptr) {
    lv_obj_add_style(obj, this->thumbnail_container_style_, 0);
  } else {
    this->init_default_thumbnail_styles_();
    lv_obj_add_style(obj, &this->default_container_style_, 0);
  }
}

std::string PictureViewer::format_thumbnail_label_(size_t image_index) {
  if (image_index >= this->images_.size()) {
    return "";
  }

  std::string result = this->thumbnail_label_pattern_;
  const auto &image = this->images_[image_index];

  // Replace {index} with 1-based index
  size_t pos = 0;
  while ((pos = result.find("{index}", pos)) != std::string::npos) {
    result.replace(pos, 7, std::to_string(image_index + 1));
    pos += std::to_string(image_index + 1).length();
  }

  // Replace {filename} with full filename
  pos = 0;
  while ((pos = result.find("{filename}", pos)) != std::string::npos) {
    // Extract filename from path
    size_t last_slash = image.path.find_last_of("/\\");
    std::string filename = (last_slash != std::string::npos) ? image.path.substr(last_slash + 1) : image.path;
    result.replace(pos, 10, filename);
    pos += filename.length();
  }

  // Replace {name} with filename without extension
  pos = 0;
  while ((pos = result.find("{name}", pos)) != std::string::npos) {
    // Extract filename from path
    size_t last_slash = image.path.find_last_of("/\\");
    std::string filename = (last_slash != std::string::npos) ? image.path.substr(last_slash + 1) : image.path;
    // Remove extension
    size_t last_dot = filename.find_last_of(".");
    std::string name = (last_dot != std::string::npos) ? filename.substr(0, last_dot) : filename;
    result.replace(pos, 6, name);
    pos += name.length();
  }

  return result;
}

void PictureViewer::create_thumbnail_widgets_() {
  if (this->thumbnail_container_ == nullptr) {
    ESP_LOGW(TAG, "Thumbnail container not set, skipping widget creation");
    return;
  }

  ESP_LOGI(TAG, "Creating %zu thumbnail widgets with %s layout", this->thumbnail_config_.max_count,
           this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL ? "HORIZONTAL"
           : this->thumbnail_config_.layout == ThumbnailLayout::VERTICAL ? "VERTICAL"
                                                                         : "GRID");

  // Explicitly set clean default properties on container (overrides LVGL defaults)
  lv_obj_set_style_pad_all(this->thumbnail_container_, 0, 0);
  lv_obj_set_style_border_width(this->thumbnail_container_, 0, 0);
  lv_obj_set_style_radius(this->thumbnail_container_, 0, 0);

  // Apply container style (default or user-provided)
  this->apply_container_style_(this->thumbnail_container_);

  // Configure container layout
  lv_obj_set_flex_flow(this->thumbnail_container_,
                       this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL ? LV_FLEX_FLOW_ROW
                       : this->thumbnail_config_.layout == ThumbnailLayout::VERTICAL ? LV_FLEX_FLOW_COLUMN
                                                                                     : LV_FLEX_FLOW_ROW_WRAP);

  lv_obj_set_flex_align(this->thumbnail_container_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  // Enable scrolling but hide scrollbars
  lv_obj_add_flag(this->thumbnail_container_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(this->thumbnail_container_, LV_SCROLLBAR_MODE_OFF);

  // Set container scroll direction based on layout
  if (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL) {
    lv_obj_set_scroll_dir(this->thumbnail_container_, LV_DIR_HOR);
  } else if (this->thumbnail_config_.layout == ThumbnailLayout::VERTICAL) {
    lv_obj_set_scroll_dir(this->thumbnail_container_, LV_DIR_VER);
  } else {
    lv_obj_set_scroll_dir(this->thumbnail_container_, LV_DIR_VER);
  }

  // Add scroll event handler for smart thumbnail preloading
  lv_obj_add_event_cb(
      this->thumbnail_container_,
      [](lv_event_t *e) {
        auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));
        viewer->on_thumbnail_scroll_(e);
      },
      LV_EVENT_SCROLL, this);

  const int thumb_w = this->thumbnail_config_.width;
  const int thumb_h = this->thumbnail_config_.height;
  const int spacing = this->thumbnail_config_.spacing;

  // Create thumbnail widgets
  for (size_t i = 0; i < this->thumbnail_config_.max_count; i++) {
    // Create button container
    lv_obj_t *btn = lv_obj_create(this->thumbnail_container_);
    lv_obj_set_size(btn, thumb_w, thumb_h);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    // Explicitly set clean default properties (overrides LVGL defaults)
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 0, 0);

    // Apply thumbnail style (default or user-provided)
    this->apply_thumbnail_style_(btn, false);  // Start as non-active

    // Set margin for spacing
    if (i > 0) {
      if (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL) {
        lv_obj_set_style_pad_left(btn, spacing, 0);
      } else if (this->thumbnail_config_.layout == ThumbnailLayout::VERTICAL) {
        lv_obj_set_style_pad_top(btn, spacing, 0);
      } else {
        // Grid layout - add spacing
        lv_obj_set_style_pad_left(btn, spacing, 0);
        lv_obj_set_style_pad_top(btn, spacing, 0);
      }
    }

    // Hide thumbnail by default - will be shown when image is loaded
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

    // Create canvas inside button
    lv_obj_t *canvas = lv_canvas_create(btn);
    lv_obj_center(canvas);

    // Make canvas non-clickable so clicks reach the button
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);

    // Set canvas buffer to point into thumbnail buffer array
    if (this->thumbnail_cache_[i].data != nullptr) {
      lv_canvas_set_buffer(canvas, this->thumbnail_cache_[i].data, thumb_w, thumb_h, LV_IMG_CF_TRUE_COLOR);
    }

    // Create optional label if pattern is configured
    lv_obj_t *label = nullptr;
    if (!this->thumbnail_label_pattern_.empty()) {
      label = lv_label_create(btn);
      lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);  // Position at bottom center

      // Make label non-clickable so clicks reach the button
      lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

      // Apply label style if provided
      if (this->thumbnail_label_style_ != nullptr) {
        lv_obj_add_style(label, this->thumbnail_label_style_, 0);
      }

      // Label text will be set when thumbnail is loaded
    }

    // Store references in cache
    this->thumbnail_cache_[i].thumb_btn_ = btn;
    this->thumbnail_cache_[i].thumb_canvas_ = canvas;
    this->thumbnail_cache_[i].thumb_label_ = label;

    // If thumbnail was already loaded (race condition: preload finished before widgets created),
    // unhide the widget and update it now
    if (this->thumbnail_cache_[i].loaded && this->thumbnail_cache_[i].image_index >= 0) {
      lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
      lv_canvas_set_buffer(canvas, this->thumbnail_cache_[i].data, this->thumbnail_config_.width,
                           this->thumbnail_config_.height, LV_IMG_CF_TRUE_COLOR);

      // Update label if configured
      if (label != nullptr && !this->thumbnail_label_pattern_.empty()) {
        std::string label_text = this->format_thumbnail_label_(this->thumbnail_cache_[i].image_index);
        lv_label_set_text(label, label_text.c_str());
      }

      ESP_LOGD(TAG, "Widget %zu created for already-loaded thumbnail (image %d)", i,
               this->thumbnail_cache_[i].image_index);
    }

    // Add click handler - capture index by value
    const size_t thumbnail_index = i;
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t *e) {
          auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));
          lv_event_code_t code = lv_event_get_code(e);

          // Handle CLICKED events - select the image
          if (code == LV_EVENT_CLICKED) {
            auto index = reinterpret_cast<size_t>(lv_obj_get_user_data(lv_event_get_target(e)));

            // Reset interaction timer on click
            viewer->last_thumbnail_interaction_time_ = millis();

            // Get the image index from the thumbnail cache
            if (index < viewer->thumbnail_cache_.size()) {
              int image_index = viewer->thumbnail_cache_[index].image_index;
              if (image_index >= 0) {
                ESP_LOGI(TAG, "Thumbnail %zu clicked (image index %d)", index, image_index);

                // Stop slideshow if playing (user is manually selecting an image)
                bool was_playing = (viewer->get_slideshow_mode() == SlideshowMode::PLAYING);
                if (was_playing) {
                  viewer->stop_slideshow();
                  ESP_LOGD(TAG, "Stopped slideshow due to thumbnail click");
                }

                // Fire automation triggers
                for (auto *trigger : viewer->thumbnail_click_triggers_) {
                  trigger->trigger(static_cast<size_t>(image_index));
                }
                viewer->thumbnail_click_callbacks_.call(static_cast<size_t>(image_index));

                // Default behavior: show image (only if no automation is configured)
                if (viewer->thumbnail_click_triggers_.empty()) {
                  viewer->show_image(image_index);
                  viewer->update_thumbnail_highlighting_(image_index);
                }

                // Show pause overlay if slideshow was playing
                if (was_playing) {
                  viewer->show_overlay_icon_(false);  // Show pause icon
                }
              }
            }
          }
          // Handle GESTURE events - manually bubble to parent for slide-out
          else if (code == LV_EVENT_GESTURE && viewer->thumbnail_slide_enabled_ && viewer->thumbnails_visible_) {
            lv_obj_t *parent = lv_obj_get_parent(viewer->thumbnail_container_);
            if (parent != nullptr) {
              // Manually send gesture event to parent
              lv_event_send(parent, LV_EVENT_GESTURE, nullptr);
            }
          }
        },
        LV_EVENT_ALL, this);

    // Store thumbnail index in button user data
    lv_obj_set_user_data(btn, reinterpret_cast<void *>(thumbnail_index));
  }

  // Set transparent background on thumbnail container and its parents
  lv_obj_set_style_bg_opa(this->thumbnail_container_, LV_OPA_TRANSP, 0);
  // Also set zero padding on container itself
  lv_obj_set_style_pad_all(this->thumbnail_container_, 0, 0);
  lv_obj_t *parent = lv_obj_get_parent(this->thumbnail_container_);
  if (parent != nullptr) {
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    ESP_LOGD(TAG, "Set transparent background and zero padding on thumbnail container and parent");

    // Add gesture handler to parent container for slide-out detection
    if (this->thumbnail_slide_enabled_) {
      lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);  // Make it receive touch events
      lv_obj_add_event_cb(
          parent,
          [](lv_event_t *e) {
            auto *viewer = static_cast<PictureViewer *>(lv_event_get_user_data(e));
            lv_indev_t *indev = lv_indev_get_act();
            if (indev == nullptr)
              return;

            lv_dir_t dir = lv_indev_get_gesture_dir(indev);
            if (dir == LV_DIR_NONE)
              return;

            // Only handle slide-out gesture when thumbnails are visible
            if (!viewer->thumbnails_visible_)
              return;

            bool should_slide_out = false;
            switch (viewer->thumbnail_slide_edge_) {
              case ThumbnailSlideEdge::RIGHT:
                should_slide_out = (dir == LV_DIR_RIGHT);
                break;
              case ThumbnailSlideEdge::LEFT:
                should_slide_out = (dir == LV_DIR_LEFT);
                break;
              case ThumbnailSlideEdge::TOP:
                should_slide_out = (dir == LV_DIR_TOP);
                break;
              case ThumbnailSlideEdge::BOTTOM:
                should_slide_out = (dir == LV_DIR_BOTTOM);
                break;
            }

            if (should_slide_out) {
              ESP_LOGD(TAG, "Thumbnail container swipe detected - sliding OUT");
              viewer->slide_thumbnails(false);
            }
          },
          LV_EVENT_GESTURE, this);
      ESP_LOGD(TAG, "Added gesture handler to thumbnail parent container");
    }
  }

  // TESTING: Start with thumbnails visible to verify rendering
  this->thumbnails_visible_ = true;

  // Keep parent in foreground and visible for testing
  if (parent != nullptr) {
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(parent);
    if (this->thumbnail_slide_enabled_) {
      lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    }
    ESP_LOGD(TAG, "TESTING: Thumbnail parent container visible by default");
  } else {
    lv_obj_clear_flag(this->thumbnail_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->thumbnail_container_);
    ESP_LOGD(TAG, "TESTING: Thumbnail container visible by default");
  }

  ESP_LOGI(TAG, "TESTING: Thumbnails visible by default for debugging");

  // Note: Widget hiding based on image count happens in update_directory_files_()
  // after images are loaded, not here during setup when images_.size() is 0

  // Only invalidate the container, don't force full refresh
  lv_obj_invalidate(this->thumbnail_container_);
  ESP_LOGI(TAG, "Created %zu thumbnail widgets (%zu visible)", this->thumbnail_config_.max_count,
           std::min(this->images_.size(), this->thumbnail_config_.max_count));
}

void PictureViewer::update_thumbnail_widget_(size_t cache_index) {
  if (cache_index >= this->thumbnail_cache_.size()) {
    return;
  }

  auto &entry = this->thumbnail_cache_[cache_index];
  if (entry.thumb_canvas_ == nullptr || !entry.loaded) {
    return;
  }

  // Show thumbnail now that image data is loaded
  if (entry.thumb_btn_ != nullptr) {
    lv_obj_clear_flag(entry.thumb_btn_, LV_OBJ_FLAG_HIDDEN);
  }

  // Update label text if label exists and pattern is configured
  if (entry.thumb_label_ != nullptr && !this->thumbnail_label_pattern_.empty() && entry.image_index >= 0) {
    std::string label_text = this->format_thumbnail_label_(entry.image_index);
    lv_label_set_text(entry.thumb_label_, label_text.c_str());
  }

  // Re-set canvas buffer to ensure LVGL recognizes the updated data
  lv_canvas_set_buffer(entry.thumb_canvas_, entry.data, this->thumbnail_config_.width, this->thumbnail_config_.height,
                       LV_IMG_CF_TRUE_COLOR);

  // Invalidate to trigger redraw (actual refresh happens when all thumbnails are loaded)
  lv_obj_invalidate(entry.thumb_canvas_);
  lv_obj_invalidate(entry.thumb_btn_);
}

void PictureViewer::update_thumbnail_highlighting_(int active_image_index) {
  // Find which cache entry contains the active image
  for (size_t i = 0; i < this->thumbnail_cache_.size(); i++) {
    if (this->thumbnail_cache_[i].thumb_btn_ == nullptr) {
      continue;
    }

    bool is_active = (this->thumbnail_cache_[i].image_index == active_image_index);

    // Remove all existing styles first
    lv_obj_remove_style_all(this->thumbnail_cache_[i].thumb_btn_);

    // Re-apply appropriate style (active or inactive)
    this->apply_thumbnail_style_(this->thumbnail_cache_[i].thumb_btn_, is_active);

    // Restore widget size (removed by lv_obj_remove_style_all)
    lv_obj_set_size(this->thumbnail_cache_[i].thumb_btn_, this->thumbnail_config_.width,
                    this->thumbnail_config_.height);

    // Reapply spacing if needed (styles cleared spacing)
    if (i > 0) {
      int spacing = this->thumbnail_config_.spacing;
      if (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL) {
        lv_obj_set_style_pad_left(this->thumbnail_cache_[i].thumb_btn_, spacing, 0);
      } else if (this->thumbnail_config_.layout == ThumbnailLayout::VERTICAL) {
        lv_obj_set_style_pad_top(this->thumbnail_cache_[i].thumb_btn_, spacing, 0);
      } else {
        lv_obj_set_style_pad_left(this->thumbnail_cache_[i].thumb_btn_, spacing, 0);
        lv_obj_set_style_pad_top(this->thumbnail_cache_[i].thumb_btn_, spacing, 0);
      }
    }
  }
}

#endif  // USE_LVGL

bool PictureViewer::load_image_(const std::string &path, std::vector<uint8_t> &rgb565_data, int &width, int &height,
                                int target_width, int target_height) {
  // Detect format from file extension
  std::string lower_path = path;
  std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

  if (lower_path.ends_with(".jpg") || lower_path.ends_with(".jpeg")) {
    return this->load_jpeg_(path, rgb565_data, width, height, target_width, target_height);
  }
#ifdef USE_PNG_DECODER
  else if (lower_path.ends_with(".png")) {
    return this->load_png_(path, rgb565_data, width, height, target_width, target_height);
  }
#endif
#ifdef USE_BMP_DECODER
  else if (lower_path.ends_with(".bmp")) {
    return this->load_bmp_(path, rgb565_data, width, height, target_width, target_height);
  }
#endif
  else {
    ESP_LOGE(TAG, "Unsupported image format: %s", path.c_str());
    return false;
  }
}

bool PictureViewer::load_jpeg_(const std::string &path, std::vector<uint8_t> &rgb565_data, int &width, int &height,
                               int target_width, int target_height) {
  // Read JPEG file
  std::vector<uint8_t> jpeg_data;
  if (!this->read_file_(path, jpeg_data)) {
    return false;
  }

  ESP_LOGD(TAG, "Loaded JPEG file: %s (%zu bytes)", path.c_str(), jpeg_data.size());

  // Decode JPEG based on platform
#ifdef USE_ESP_NEW_JPEG_DECODER
  return this->decode_jpeg_esp_new_(jpeg_data, rgb565_data, width, height, target_width, target_height);
#elif defined(USE_HARDWARE_JPEG_DECODER)
  return this->decode_jpeg_hardware_(jpeg_data, rgb565_data, width, height, target_width, target_height);
#elif defined(USE_JPEGDEC)
  return this->decode_jpeg_jpegdec_(jpeg_data, rgb565_data, width, height, target_width, target_height);
#else
  return false;  // No JPEG decoder available
#endif
}

#ifdef USE_PNG_DECODER
bool PictureViewer::load_png_(const std::string &path, std::vector<uint8_t> &rgb565_data, int &width, int &height,
                              int target_width, int target_height) {
  // Read PNG file
  std::vector<uint8_t> png_data;
  if (!this->read_file_(path, png_data)) {
    return false;
  }

  ESP_LOGD(TAG, "Loaded PNG file: %s (%zu bytes)", path.c_str(), png_data.size());

  // Decode PNG
  return this->decode_png_(png_data, rgb565_data, width, height, target_width, target_height);
}
#endif

#ifdef USE_BMP_DECODER
bool PictureViewer::load_bmp_(const std::string &path, std::vector<uint8_t> &rgb565_data, int &width, int &height,
                              int target_width, int target_height) {
  // Read BMP file
  std::vector<uint8_t> bmp_data;
  if (!this->read_file_(path, bmp_data)) {
    return false;
  }

  ESP_LOGD(TAG, "Loaded BMP file: %s (%zu bytes)", path.c_str(), bmp_data.size());

  // Decode BMP
  return this->decode_bmp_(bmp_data, rgb565_data, width, height, target_width, target_height);
}
#endif

#ifdef USE_ESP_NEW_JPEG_DECODER
bool PictureViewer::decode_jpeg_esp_new_(const std::vector<uint8_t> &jpeg_data, std::vector<uint8_t> &rgb565_data,
                                         int &width, int &height, int target_width, int target_height) {
  // Get decoder handle from transcoder
  jpeg_dec_handle_t decoder = this->transcoder_->get_esp_new_jpeg_decoder();
  if (decoder == nullptr) {
    ESP_LOGE(TAG, "Failed to get ESP_NEW_JPEG decoder handle");
    return false;
  }

  // Configure decoder for RGB565 output
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;  // RGB565 little-endian for ESP32
  config.rotate = JPEG_ROTATE_0D;
  config.block_enable = false;

  // Apply scaling if target dimensions provided
  if (target_width > 0 && target_height > 0) {
    config.scale.width = (target_width + 7) & ~7;    // Round up to multiple of 8
    config.scale.height = (target_height + 7) & ~7;  // Round up to multiple of 8
  }

  // Close old decoder and open new one with updated config
  this->transcoder_->release_esp_new_jpeg_decoder();
  jpeg_error_t ret = jpeg_dec_open(&config, &decoder);
  if (ret != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Failed to reconfigure ESP_NEW_JPEG decoder: %d", ret);
    return false;
  }

  // Parse JPEG header
  jpeg_dec_io_t io = {};
  io.inbuf = const_cast<uint8_t *>(jpeg_data.data());
  io.inbuf_len = jpeg_data.size();

  jpeg_dec_header_info_t header_info = {};
  ret = jpeg_dec_parse_header(decoder, &io, &header_info);
  if (ret != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Failed to parse JPEG header: %d", ret);
    jpeg_dec_close(decoder);
    return false;
  }

  width = header_info.width;
  height = header_info.height;

  // Get required output buffer size
  int outbuf_len = 0;
  ret = jpeg_dec_get_outbuf_len(decoder, &outbuf_len);
  if (ret != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Failed to get output buffer size: %d", ret);
    jpeg_dec_close(decoder);
    return false;
  }

  // Allocate aligned output buffer
  uint8_t *outbuf = static_cast<uint8_t *>(jpeg_calloc_align(outbuf_len, 16));
  if (outbuf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate aligned output buffer (%d bytes)", outbuf_len);
    jpeg_dec_close(decoder);
    return false;
  }

  // Decode JPEG
  io.outbuf = outbuf;
  ret = jpeg_dec_process(decoder, &io);
  if (ret != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "JPEG decode failed: %d", ret);
    jpeg_free_align(outbuf);
    jpeg_dec_close(decoder);
    return false;
  }

  // Copy decoded data to output vector
  rgb565_data.resize(io.out_size);
  std::memcpy(rgb565_data.data(), outbuf, io.out_size);

  // Free aligned buffer and close decoder
  jpeg_free_align(outbuf);
  jpeg_dec_close(decoder);

  ESP_LOGD(TAG, "Decoded JPEG using ESP_NEW_JPEG v1.0.0: %dx%d (%d bytes)", width, height, io.out_size);
  return true;
}
#endif

#ifdef USE_HARDWARE_JPEG_DECODER
bool PictureViewer::decode_jpeg_hardware_(const std::vector<uint8_t> &jpeg_data, std::vector<uint8_t> &rgb565_data,
                                          int &width, int &height, int target_width, int target_height) {
  if (this->transcoder_ == nullptr || !this->transcoder_->is_jpeg_decoder_available()) {
    ESP_LOGE(TAG, "Hardware JPEG decoder not available in transcoder");
    return false;
  }

  // Get decoder handle from transcoder
  jpeg_decoder_handle_t hw_decoder = this->transcoder_->get_jpeg_decoder();

  // Get image info
  jpeg_decode_picture_info_t pic_info = {};
  esp_err_t ret = jpeg_decoder_get_info(jpeg_data.data(), static_cast<uint32_t>(jpeg_data.size()), &pic_info);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get JPEG info: %s", esp_err_to_name(ret));
    return false;
  }

  width = pic_info.width;
  height = pic_info.height;
  ESP_LOGD(TAG, "JPEG dimensions: %dx%d", width, height);

  // Calculate buffer sizes
  uint32_t input_size = jpeg_data.size();
  uint32_t output_size = width * height * 2;  // RGB565 = 2 bytes per pixel

  ESP_LOGD(TAG, "Buffer sizes: input=%u, output=%u", input_size, output_size);

  // Allocate buffers using ESP-IDF JPEG decoder memory allocator
  // This ensures proper cache-line alignment for DMA operations
  jpeg_decode_memory_alloc_cfg_t input_alloc_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };
  jpeg_decode_memory_alloc_cfg_t output_alloc_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };

  size_t input_buffer_size = 0;
  size_t output_buffer_size = 0;

  uint8_t *aligned_input = (uint8_t *) jpeg_alloc_decoder_mem(input_size, &input_alloc_cfg, &input_buffer_size);
  if (!aligned_input) {
    ESP_LOGE(TAG, "Failed to allocate input buffer (%u bytes)", input_size);
    return false;
  }

  uint8_t *aligned_output = (uint8_t *) jpeg_alloc_decoder_mem(output_size, &output_alloc_cfg, &output_buffer_size);
  if (!aligned_output) {
    ESP_LOGE(TAG, "Failed to allocate output buffer (%u bytes)", output_size);
    free(aligned_input);
    return false;
  }

  // Copy input data to aligned buffer
  memcpy(aligned_input, jpeg_data.data(), input_size);

  // Get current directory's JPEG decoder configuration
  const DirectoryConfig *current_dir = this->get_current_directory();
  if (current_dir == nullptr) {
    ESP_LOGE(TAG, "No current directory configured for JPEG decoding");
    free(aligned_input);
    free(aligned_output);
    return false;
  }

  // Configure decoder for RGB565 output
  jpeg_decode_cfg_t decode_cfg = {};
  decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  // conv_std defaults to 0 (BT601)

  // Decode (with retry on failure due to ESP32-P4 state corruption bug)
  uint32_t actual_output_size = 0;
  ret = jpeg_decoder_process(hw_decoder, &decode_cfg, aligned_input, input_size, aligned_output, output_buffer_size,
                             &actual_output_size);

  // If decode fails, release decoder and retry once (ESP32-P4 state corruption workaround)
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Hardware JPEG decode failed: %s, retrying with fresh decoder", esp_err_to_name(ret));
    this->transcoder_->release_jpeg_decoder();
    hw_decoder = this->transcoder_->get_jpeg_decoder();

    if (hw_decoder == nullptr) {
      ESP_LOGE(TAG, "Failed to reinitialize JPEG decoder");
      free(aligned_input);
      free(aligned_output);
      return false;
    }

    // Retry decode
    ret = jpeg_decoder_process(hw_decoder, &decode_cfg, aligned_input, input_size, aligned_output, output_buffer_size,
                               &actual_output_size);

    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Hardware JPEG decode failed after retry: %s", esp_err_to_name(ret));
      free(aligned_input);
      free(aligned_output);
      this->transcoder_->release_jpeg_decoder();  // Release failed decoder
      return false;
    }
    ESP_LOGI(TAG, "JPEG decode succeeded after decoder reset");
  }

  ESP_LOGD(TAG, "Hardware decode completed, output size: %u bytes", actual_output_size);

  // Byte-swap RGB565 for correct endianness
  uint16_t *pixels = (uint16_t *) aligned_output;
  size_t pixel_count = actual_output_size / 2;
  for (size_t i = 0; i < pixel_count; i++) {
    pixels[i] = (pixels[i] << 8) | (pixels[i] >> 8);
  }

  // Copy to output vector
  rgb565_data.resize(actual_output_size);
  memcpy(rgb565_data.data(), aligned_output, actual_output_size);

  // Free aligned buffers
  free(aligned_input);
  free(aligned_output);

  // Decoder is kept alive for next decode (only released on error)
  ESP_LOGD(TAG, "Decoded JPEG using hardware decoder: %dx%d", width, height);

  if (target_width > 0 && target_height > 0 && (width != target_width || height != target_height)) {
    // Resize to target dimensions
    std::vector<uint8_t> resized_data;
    this->resize_image_(rgb565_data, width, height, resized_data, target_width, target_height);
    rgb565_data = std::move(resized_data);
    width = target_width;
    height = target_height;
    ESP_LOGD(TAG, "Resized image to: %dx%d", width, height);
  }
  return true;
}
#endif

#ifdef USE_JPEGDEC
bool PictureViewer::decode_jpeg_jpegdec_(const std::vector<uint8_t> &jpeg_data, std::vector<uint8_t> &rgb565_data,
                                         int &width, int &height, int target_width, int target_height) {
  if (this->jpeg_decoder_ == nullptr) {
    ESP_LOGE(TAG, "JPEGDec decoder not initialized");
    return false;
  }

  // Open JPEG from memory
  if (this->jpeg_decoder_->openRAM(const_cast<uint8_t *>(jpeg_data.data()), jpeg_data.size(),
                                   PictureViewer::jpeg_decode_callback_) != 1) {
    ESP_LOGE(TAG, "Failed to open JPEG");
    return false;
  }

  width = this->jpeg_decoder_->getWidth();
  height = this->jpeg_decoder_->getHeight();

  // Allocate output buffer
  rgb565_data.resize(width * height * 2);  // RGB565 = 2 bytes per pixel
  this->decode_target_ = &rgb565_data;
  this->decode_width_ = width;

  // Decode
  if (this->jpeg_decoder_->decode(0, 0, 0) != 1) {
    ESP_LOGE(TAG, "Failed to decode JPEG");
    this->jpeg_decoder_->close();
    return false;
  }

  this->jpeg_decoder_->close();
  this->decode_target_ = nullptr;

  ESP_LOGD(TAG, "Decoded JPEG using JPEGDec: %dx%d", width, height);
  return true;
}

int PictureViewer::jpeg_decode_callback_(JPEGDRAW *draw) {
  // This is called during decode to provide the RGB565 data
  // We need to copy it to our buffer
  if (draw == nullptr || draw->pUser == nullptr) {
    return 0;
  }

  // Note: This is a static callback, so we can't access instance members directly
  // The decode_target_ and decode_width_ should be set before decode() is called
  return 1;
}
#endif

#ifdef USE_PNG_DECODER
// Static callbacks for pngle library
void PictureViewer::png_init_callback_(pngle_t *pngle, uint32_t w, uint32_t h) {
  PictureViewer *viewer = (PictureViewer *) pngle_get_user_data(pngle);
  ESP_LOGD(TAG, "PNG decoder initialized: %ux%u", w, h);
  // Dimensions will be set by the decoder
}

void PictureViewer::png_draw_callback_(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                       uint8_t rgba[4]) {
  PictureViewer *viewer = (PictureViewer *) pngle_get_user_data(pngle);
  if (viewer->decode_target_ == nullptr) {
    return;
  }

  // Convert RGBA to RGB565 and write to buffer
  // RGB565 format: RRRRR GGGGGG BBBBB
  uint8_t r = rgba[0];
  uint8_t g = rgba[1];
  uint8_t b = rgba[2];
  // Alpha is in rgba[3] but we ignore it for now

  uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

  // Write pixels to decode_target_ buffer
  for (uint32_t dy = 0; dy < h; dy++) {
    for (uint32_t dx = 0; dx < w; dx++) {
      uint32_t px = x + dx;
      uint32_t py = y + dy;
      if (px < viewer->decode_width_ && py < viewer->decode_width_) {  // Use decode_width_ as image width
        size_t offset = (py * viewer->decode_width_ + px) * 2;
        if (offset + 1 < viewer->decode_target_->size()) {
          (*viewer->decode_target_)[offset] = rgb565 & 0xFF;
          (*viewer->decode_target_)[offset + 1] = (rgb565 >> 8) & 0xFF;
        }
      }
    }
  }
}

void PictureViewer::png_done_callback_(pngle_t *pngle) { ESP_LOGD(TAG, "PNG decoding completed"); }

bool PictureViewer::decode_png_(const std::vector<uint8_t> &png_data, std::vector<uint8_t> &rgb565_data, int &width,
                                int &height, int target_width, int target_height) {
  ESP_LOGD(TAG, "Decoding PNG image (%zu bytes)", png_data.size());

  // Allocate pngle decoder
  pngle_t *pngle = pngle_new();
  if (!pngle) {
    ESP_LOGE(TAG, "Failed to allocate pngle decoder");
    return false;
  }

  // Set user data and callbacks
  pngle_set_user_data(pngle, this);
  pngle_set_init_callback(pngle, png_init_callback_);
  pngle_set_draw_callback(pngle, png_draw_callback_);
  pngle_set_done_callback(pngle, png_done_callback_);

  // Feed data to decoder to get dimensions first
  int fed = pngle_feed(pngle, png_data.data(), png_data.size());
  if (fed < 0) {
    ESP_LOGE(TAG, "PNG decoding failed: %s", pngle_error(pngle));
    pngle_destroy(pngle);
    return false;
  }

  // Get decoded dimensions
  width = pngle_get_width(pngle);
  height = pngle_get_height(pngle);
  ESP_LOGD(TAG, "PNG dimensions: %dx%d", width, height);

  // Allocate output buffer
  rgb565_data.resize(width * height * 2);

  // Reset decoder and decode again with buffer
  pngle_destroy(pngle);
  pngle = pngle_new();
  if (!pngle) {
    ESP_LOGE(TAG, "Failed to re-allocate pngle decoder");
    return false;
  }

  pngle_set_user_data(pngle, this);
  pngle_set_init_callback(pngle, png_init_callback_);
  pngle_set_draw_callback(pngle, png_draw_callback_);
  pngle_set_done_callback(pngle, png_done_callback_);

  // Set decode target for callbacks
  this->decode_target_ = &rgb565_data;
  this->decode_width_ = width;

  // Decode the image
  fed = pngle_feed(pngle, png_data.data(), png_data.size());
  if (fed < 0) {
    ESP_LOGE(TAG, "PNG decoding failed on second pass: %s", pngle_error(pngle));
    pngle_destroy(pngle);
    this->decode_target_ = nullptr;
    return false;
  }

  pngle_destroy(pngle);
  this->decode_target_ = nullptr;

  ESP_LOGD(TAG, "PNG decoded successfully: %dx%d", width, height);

  // Apply scaling if requested
  if (target_width > 0 && target_height > 0 && (width != target_width || height != target_height)) {
    std::vector<uint8_t> resized_data;
    this->resize_image_(rgb565_data, width, height, resized_data, target_width, target_height);
    rgb565_data = std::move(resized_data);
    width = target_width;
    height = target_height;
    ESP_LOGD(TAG, "Resized PNG to: %dx%d", width, height);
  }

  return true;
}
#endif

#ifdef USE_BMP_DECODER
bool PictureViewer::decode_bmp_(const std::vector<uint8_t> &bmp_data, std::vector<uint8_t> &rgb565_data, int &width,
                                int &height, int target_width, int target_height) {
  ESP_LOGD(TAG, "Decoding BMP image (%zu bytes)", bmp_data.size());

  if (bmp_data.size() < 54) {
    ESP_LOGE(TAG, "BMP file too small");
    return false;
  }

  // Check BMP signature
  if (bmp_data[0] != 'B' || bmp_data[1] != 'M') {
    ESP_LOGE(TAG, "Not a BMP file");
    return false;
  }

  // Read BMP header
  uint32_t data_offset = encode_uint32(bmp_data[13], bmp_data[12], bmp_data[11], bmp_data[10]);
  width = encode_uint32(bmp_data[21], bmp_data[20], bmp_data[19], bmp_data[18]);
  height = encode_uint32(bmp_data[25], bmp_data[24], bmp_data[23], bmp_data[22]);
  uint16_t bits_per_pixel = encode_uint16(bmp_data[29], bmp_data[28]);
  uint32_t compression = encode_uint32(bmp_data[33], bmp_data[32], bmp_data[31], bmp_data[30]);

  ESP_LOGD(TAG, "BMP: %dx%d, %d bpp, compression=%u, offset=%u", width, height, bits_per_pixel, compression,
           data_offset);

  // Only support uncompressed 24-bit BMP for now
  if (compression != 0) {
    ESP_LOGE(TAG, "Compressed BMP not supported");
    return false;
  }

  if (bits_per_pixel != 24) {
    ESP_LOGE(TAG, "Only 24-bit BMP supported (got %d bpp)", bits_per_pixel);
    return false;
  }

  // Calculate row size (must be multiple of 4 bytes)
  uint32_t row_size = ((width * 3 + 3) / 4) * 4;
  uint32_t padding = row_size - (width * 3);

  // Allocate output buffer
  rgb565_data.resize(width * height * 2);

  // Decode BMP data (bottom-up)
  for (int y = 0; y < height; y++) {
    uint32_t row_offset = data_offset + (height - 1 - y) * row_size;
    if (row_offset + width * 3 > bmp_data.size()) {
      ESP_LOGE(TAG, "BMP data truncated at row %d", y);
      return false;
    }

    for (int x = 0; x < width; x++) {
      uint32_t pixel_offset = row_offset + x * 3;
      uint8_t b = bmp_data[pixel_offset];
      uint8_t g = bmp_data[pixel_offset + 1];
      uint8_t r = bmp_data[pixel_offset + 2];

      // Convert RGB888 to RGB565
      uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

      // Write to output buffer
      size_t out_offset = (y * width + x) * 2;
      rgb565_data[out_offset] = rgb565 & 0xFF;
      rgb565_data[out_offset + 1] = (rgb565 >> 8) & 0xFF;
    }
  }

  ESP_LOGD(TAG, "BMP decoded successfully: %dx%d", width, height);

  // Apply scaling if requested
  if (target_width > 0 && target_height > 0 && (width != target_width || height != target_height)) {
    std::vector<uint8_t> resized_data;
    this->resize_image_(rgb565_data, width, height, resized_data, target_width, target_height);
    rgb565_data = std::move(resized_data);
    width = target_width;
    height = target_height;
    ESP_LOGD(TAG, "Resized BMP to: %dx%d", width, height);
  }

  return true;
}
#endif

void PictureViewer::resize_image_(const std::vector<uint8_t> &src_data, int src_width, int src_height,
                                  std::vector<uint8_t> &dst_data, int dst_width, int dst_height) {
  // Simple nearest-neighbor scaling for RGB565
  dst_data.resize(dst_width * dst_height * 2);

  for (int y = 0; y < dst_height; y++) {
    int src_y = (y * src_height) / dst_height;
    for (int x = 0; x < dst_width; x++) {
      int src_x = (x * src_width) / dst_width;
      int src_idx = (src_y * src_width + src_x) * 2;
      int dst_idx = (y * dst_width + x) * 2;
      dst_data[dst_idx] = src_data[src_idx];
      dst_data[dst_idx + 1] = src_data[src_idx + 1];
    }
  }
}

void PictureViewer::update_canvas_() {
#ifdef USE_LVGL
  if (this->canvas_ == nullptr) {
    ESP_LOGW(TAG, "Canvas not set");
    return;
  }

  if (this->current_image_data_ == nullptr) {
    ESP_LOGW(TAG, "No image data to display");
    return;
  }

  // Ensure canvas buffer is ready (adopts LVGL's buffer)
  this->ensure_canvas_buffer_();
  if (!this->canvas_buffer_ready_) {
    ESP_LOGE(TAG, "Canvas buffer not ready, cannot update");
    return;
  }

  // Write image data directly to canvas buffer (handles scaling/positioning)
  this->write_to_canvas_buffer_(this->current_image_data_, this->current_image_width_, this->current_image_height_);

  // Invalidate canvas to trigger redraw
  lv_obj_invalidate(this->canvas_);

  ESP_LOGD(TAG, "Canvas updated successfully");
#endif
}

bool PictureViewer::generate_thumbnail_(ImageEntry &entry) {
  if (!this->enable_thumbnails_) {
    return false;
  }

  // Load and decode thumbnail
  std::vector<uint8_t> rgb565_data;
  int width, height;
  if (!this->load_image_(entry.path, rgb565_data, width, height, this->thumbnail_width_, this->thumbnail_height_)) {
    return false;
  }

  // Resize to thumbnail size if needed
  if (width != this->thumbnail_width_ || height != this->thumbnail_height_) {
    this->resize_image_(rgb565_data, width, height, entry.thumbnail_data, this->thumbnail_width_,
                        this->thumbnail_height_);
  } else {
    entry.thumbnail_data = std::move(rgb565_data);
  }

  entry.size = entry.thumbnail_data.size();
  entry.width = width;
  entry.height = height;
  entry.thumbnail_loaded = true;

  return true;
}

// =====================================================
// Thumbnail Management Implementation
// =====================================================

const uint8_t *PictureViewer::get_thumbnail_data(size_t index, int &width, int &height) {
  if (index >= this->images_.size()) {
    return nullptr;
  }

  // Check if thumbnail is in cache
  int cache_idx = this->find_thumbnail_in_cache_(index);
  if (cache_idx >= 0 && this->thumbnail_cache_[cache_idx].loaded) {
    // Found in cache - update LRU and return data
    this->update_lru_timestamp_(cache_idx);
    auto &entry = this->thumbnail_cache_[cache_idx];
    width = this->thumbnail_config_.width;
    height = this->thumbnail_config_.height;
    return entry.data;  // Now a raw pointer, not std::vector
  }

  // Not in cache - return nullptr (caller should use request_thumbnail for lazy loading)
  return nullptr;
}

bool PictureViewer::request_thumbnail(size_t index) {
  if (index >= this->images_.size()) {
    return false;
  }

  // Check if already in cache
  int cache_idx = this->find_thumbnail_in_cache_(index);
  if (cache_idx >= 0) {
    // Already loaded - update LRU timestamp and return true
    this->update_lru_timestamp_(cache_idx);
    return true;
  }

  // Not in cache - load it (lazy loading)
  return this->load_thumbnail_(index);
}

bool PictureViewer::load_thumbnail_(size_t image_index) {
  if (image_index >= this->images_.size()) {
    return false;
  }

  if (this->thumbnail_buffer_array_ == nullptr) {
    ESP_LOGW(TAG, "Thumbnail buffer array not allocated");
    return false;
  }

  // Check if already loaded
  int existing_slot = this->find_thumbnail_in_cache_(image_index);
  if (existing_slot >= 0) {
    this->update_lru_timestamp_(existing_slot);
    ESP_LOGD(TAG, "Thumbnail %zu already in cache slot %d", image_index, existing_slot);
    return true;
  }

  auto &image = this->images_[image_index];

  // Decode thumbnail directly into work buffer
  std::vector<uint8_t> rgb565_data;
  int width, height;
  if (!this->load_image_(image.path, rgb565_data, width, height, this->thumbnail_config_.width,
                         this->thumbnail_config_.height)) {
    ESP_LOGW(TAG, "Failed to load thumbnail for image %zu: %s", image_index, image.filename.c_str());
    return false;
  }

  // Find or allocate a cache slot (using LRU if all slots are full)
  int cache_slot = -1;

  // First, try to find an empty slot
  for (size_t i = 0; i < this->thumbnail_cache_.size(); i++) {
    if (!this->thumbnail_cache_[i].loaded) {
      cache_slot = i;
      break;
    }
  }

  // If no empty slot, evict the LRU slot
  if (cache_slot == -1) {
    cache_slot = 0;
    uint32_t oldest_time = this->thumbnail_cache_[0].last_access_time;
    for (size_t i = 1; i < this->thumbnail_cache_.size(); i++) {
      if (this->thumbnail_cache_[i].last_access_time < oldest_time) {
        oldest_time = this->thumbnail_cache_[i].last_access_time;
        cache_slot = i;
      }
    }
    ESP_LOGD(TAG, "Evicting thumbnail for image %d from slot %d (LRU)", this->thumbnail_cache_[cache_slot].image_index,
             cache_slot);
  }

  // Copy RGB565 data directly to the cache slot's buffer
  auto &entry = this->thumbnail_cache_[cache_slot];
  const size_t expected_size = this->thumbnail_buffer_size_per_slot_;
  const size_t actual_size = rgb565_data.size();

  if (actual_size > expected_size) {
    ESP_LOGW(TAG, "Thumbnail data size (%zu) exceeds slot size (%zu), truncating", actual_size, expected_size);
    memcpy(entry.data, rgb565_data.data(), expected_size);
  } else {
    memcpy(entry.data, rgb565_data.data(), actual_size);
    // Fill remaining space with black if thumbnail is smaller
    if (actual_size < expected_size) {
      memset(entry.data + actual_size, 0, expected_size - actual_size);
    }
  }

  // Update cache entry metadata
  entry.image_index = image_index;
  entry.loaded = true;
  entry.last_access_time = millis();

  ESP_LOGD(TAG, "Loaded thumbnail %zu (%s) into slot %d", image_index, image.filename.c_str(), cache_slot);

#ifdef USE_LVGL
  // Update the LVGL widget if it exists
  this->update_thumbnail_widget_(cache_slot);
#endif

  // Notify callback that thumbnail is ready
  if (this->thumbnail_ready_callback_) {
    this->thumbnail_ready_callback_(image_index);
  }

  return true;
}

void PictureViewer::evict_oldest_thumbnail_() {
  // NOTE: With fixed buffer array architecture, eviction is handled inline in load_thumbnail_()
  // This method is kept for API compatibility but does nothing
  // LRU slot selection happens automatically when all slots are full
}

void PictureViewer::update_lru_timestamp_(size_t cache_index) {
  if (cache_index < this->thumbnail_cache_.size()) {
    this->thumbnail_cache_[cache_index].last_access_time = millis();
  }
}

int PictureViewer::find_thumbnail_in_cache_(size_t image_index) const {
  for (size_t i = 0; i < this->thumbnail_cache_.size(); i++) {
    if (this->thumbnail_cache_[i].image_index == static_cast<int>(image_index)) {
      return static_cast<int>(i);
    }
  }
  return -1;  // Not found
}

void PictureViewer::ensure_canvas_buffer_() {
#ifdef USE_LVGL
  if (this->canvas_ == nullptr) {
    this->canvas_buffer_ready_ = false;
    return;
  }

  // Get the canvas's image descriptor (contains the buffer LVGL owns)
  auto *img_dsc = (lv_img_dsc_t *) lv_canvas_get_img(this->canvas_);
  if (img_dsc == nullptr || img_dsc->data == nullptr) {
    // Canvas buffer not yet initialized by LVGL
    ESP_LOGD(TAG, "Canvas buffer not yet initialized by LVGL");
    this->canvas_buffer_ready_ = false;
    return;
  }

  // Validate canvas dimensions
  if (img_dsc->header.w <= 0 || img_dsc->header.h <= 0) {
    ESP_LOGD(TAG, "Invalid canvas dimensions: %dx%d", img_dsc->header.w, img_dsc->header.h);
    this->canvas_buffer_ready_ = false;
    return;
  }

  // Validate color format (we expect RGB565 / LV_IMG_CF_TRUE_COLOR)
  if (img_dsc->header.cf != LV_IMG_CF_TRUE_COLOR) {
    ESP_LOGE(TAG, "Canvas color format is not TRUE_COLOR (RGB565), got format: %d", img_dsc->header.cf);
    this->canvas_buffer_ready_ = false;
    return;
  }

  const int buf_width = img_dsc->header.w;
  const int buf_height = img_dsc->header.h;
  const size_t buf_pixels = (size_t) buf_width * (size_t) buf_height;

  // Log only when buffer changes
  if (this->canvas_buffer_ != (uint16_t *) img_dsc->data || this->canvas_buffer_pixels_ != buf_pixels) {
    ESP_LOGI(TAG, "Adopting canvas buffer: canvas=%p img_dsc=%p buffer=%p %dx%d (%zu pixels, format=%d)",
             (void *) this->canvas_, (void *) img_dsc, (void *) img_dsc->data, buf_width, buf_height, buf_pixels,
             img_dsc->header.cf);
  }

  // Adopt the canvas's buffer (we don't own it - LVGL does)
  this->canvas_buffer_ = (uint16_t *) img_dsc->data;
  this->canvas_buffer_width_ = buf_width;
  this->canvas_buffer_height_ = buf_height;
  this->canvas_buffer_pixels_ = buf_pixels;
  this->canvas_buffer_ready_ = true;
#endif
}

void PictureViewer::resize_canvas_buffer_() {
#ifdef USE_LVGL
  if (this->canvas_ == nullptr) {
    ESP_LOGW(TAG, "Canvas not set, cannot resize buffer");
    return;
  }

  // Get current canvas dimensions
  const int new_width = lv_obj_get_width(this->canvas_);
  const int new_height = lv_obj_get_height(this->canvas_);

  if (new_width <= 0 || new_height <= 0) {
    ESP_LOGE(TAG, "Invalid canvas dimensions for resize: %dx%d", new_width, new_height);
    return;
  }

  // Check if buffer size actually changed
  if (this->canvas_buffer_ready_ && this->canvas_buffer_width_ == new_width &&
      this->canvas_buffer_height_ == new_height) {
    ESP_LOGD(TAG, "Canvas buffer already correct size: %dx%d", new_width, new_height);
    return;
  }

  ESP_LOGI(TAG, "Resizing canvas buffer: %dx%d -> %dx%d", this->canvas_buffer_width_, this->canvas_buffer_height_,
           new_width, new_height);

  // Calculate buffer size (RGB565 = 2 bytes per pixel)
  const size_t buffer_size = (size_t) new_width * (size_t) new_height * sizeof(uint16_t);

  // Allocate new buffer in PSRAM if available, otherwise in regular heap
  void *new_buffer = nullptr;
#ifdef CONFIG_SPIRAM
  new_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
  if (new_buffer != nullptr) {
    ESP_LOGD(TAG, "Allocated canvas buffer in PSRAM: %zu bytes", buffer_size);
  } else {
    ESP_LOGW(TAG, "PSRAM allocation failed, trying regular heap");
  }
#endif

  if (new_buffer == nullptr) {
    new_buffer = malloc(buffer_size);
    if (new_buffer == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate canvas buffer: %zu bytes", buffer_size);
      return;
    }
    ESP_LOGD(TAG, "Allocated canvas buffer in heap: %zu bytes", buffer_size);
  }

  // Clear buffer to black
  memset(new_buffer, 0, buffer_size);

  // Set the new buffer on the canvas (LVGL will free the old buffer)
  lv_canvas_set_buffer(this->canvas_, new_buffer, new_width, new_height, LV_IMG_CF_TRUE_COLOR);

  // Re-adopt the buffer (updates our internal pointers)
  this->ensure_canvas_buffer_();

  ESP_LOGI(TAG, "Canvas buffer resized successfully to %dx%d", new_width, new_height);
#endif
}

void PictureViewer::write_to_canvas_buffer_(const uint8_t *rgb565_data, int img_width, int img_height) {
#ifdef USE_LVGL
  if (!this->canvas_buffer_ready_ || this->canvas_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Canvas buffer not ready for writing");
    return;
  }

  if (rgb565_data == nullptr || img_width <= 0 || img_height <= 0) {
    ESP_LOGW(TAG, "Invalid image data for canvas write");
    return;
  }

  const int canvas_w = this->canvas_buffer_width_;
  const int canvas_h = this->canvas_buffer_height_;

  // Calculate drawing dimensions and position based on fit mode
  int draw_x = 0, draw_y = 0;
  int draw_width = img_width;
  int draw_height = img_height;

  switch (this->fit_mode_) {
    case ImageFitMode::SCALE_TO_FIT: {
      // Scale to fit, maintain aspect ratio
      float scale_x = static_cast<float>(canvas_w) / img_width;
      float scale_y = static_cast<float>(canvas_h) / img_height;
      float scale = std::min(scale_x, scale_y);

      draw_width = static_cast<int>(img_width * scale);
      draw_height = static_cast<int>(img_height * scale);

      // Center in canvas
      draw_x = (canvas_w - draw_width) / 2;
      draw_y = (canvas_h - draw_height) / 2;

      // Clear canvas with black background
      std::memset(this->canvas_buffer_, 0, this->canvas_buffer_pixels_ * sizeof(uint16_t));
      break;
    }

    case ImageFitMode::SCALE_TO_FILL: {
      // Scale to fill, maintain aspect ratio, may crop
      float scale_x = static_cast<float>(canvas_w) / img_width;
      float scale_y = static_cast<float>(canvas_h) / img_height;
      float scale = std::max(scale_x, scale_y);

      draw_width = static_cast<int>(img_width * scale);
      draw_height = static_cast<int>(img_height * scale);

      // Center in canvas (may be cropped)
      draw_x = (canvas_w - draw_width) / 2;
      draw_y = (canvas_h - draw_height) / 2;
      break;
    }

    case ImageFitMode::STRETCH: {
      // Stretch to fill canvas, ignore aspect ratio
      draw_width = canvas_w;
      draw_height = canvas_h;
      draw_x = 0;
      draw_y = 0;
      break;
    }

    case ImageFitMode::CENTER: {
      // Center without scaling
      draw_x = (canvas_w - img_width) / 2;
      draw_y = (canvas_h - img_height) / 2;

      // Clear canvas with black background
      std::memset(this->canvas_buffer_, 0, this->canvas_buffer_pixels_ * sizeof(uint16_t));
      break;
    }
  }

  ESP_LOGD(TAG, "Writing to canvas: canvas=%dx%d, image=%dx%d, draw=%dx%d at (%d,%d), fit_mode=%d", canvas_w, canvas_h,
           img_width, img_height, draw_width, draw_height, draw_x, draw_y, static_cast<int>(this->fit_mode_));

  // Write image data to canvas buffer
  if (draw_width == img_width && draw_height == img_height) {
    // No scaling needed - direct copy with clipping
    const uint16_t *src_pixels = reinterpret_cast<const uint16_t *>(rgb565_data);

    for (int y = 0; y < img_height; y++) {
      int canvas_y = draw_y + y;
      if (canvas_y < 0 || canvas_y >= canvas_h)
        continue;  // Skip rows outside canvas

      for (int x = 0; x < img_width; x++) {
        int canvas_x = draw_x + x;
        if (canvas_x < 0 || canvas_x >= canvas_w)
          continue;  // Skip pixels outside canvas

        size_t src_idx = y * img_width + x;
        size_t dst_idx = canvas_y * canvas_w + canvas_x;
        this->canvas_buffer_[dst_idx] = src_pixels[src_idx];
      }
    }
  } else {
    // Scaling needed - use bilinear interpolation
    const uint16_t *src_pixels = reinterpret_cast<const uint16_t *>(rgb565_data);
    const float x_ratio = static_cast<float>(img_width) / draw_width;
    const float y_ratio = static_cast<float>(img_height) / draw_height;

    for (int y = 0; y < draw_height; y++) {
      int canvas_y = draw_y + y;
      if (canvas_y < 0 || canvas_y >= canvas_h)
        continue;

      for (int x = 0; x < draw_width; x++) {
        int canvas_x = draw_x + x;
        if (canvas_x < 0 || canvas_x >= canvas_w)
          continue;

        // Simple nearest-neighbor scaling (fast)
        int src_x = static_cast<int>(x * x_ratio);
        int src_y = static_cast<int>(y * y_ratio);

        // Clamp to source bounds
        src_x = std::min(src_x, img_width - 1);
        src_y = std::min(src_y, img_height - 1);

        size_t src_idx = src_y * img_width + src_x;
        size_t dst_idx = canvas_y * canvas_w + canvas_x;
        this->canvas_buffer_[dst_idx] = src_pixels[src_idx];
      }
    }
  }

  ESP_LOGD(TAG, "Canvas buffer write completed");
#endif
}

void PictureViewer::update_canvas_dimensions_() {
#ifdef USE_LVGL
  if (this->canvas_ != nullptr) {
    this->canvas_width_ = lv_obj_get_width(this->canvas_);
    this->canvas_height_ = lv_obj_get_height(this->canvas_);
    ESP_LOGD(TAG, "Canvas dimensions: %dx%d", this->canvas_width_, this->canvas_height_);
  } else {
    // Fallback to defaults if canvas not set
    if (this->canvas_width_ == 0 || this->canvas_height_ == 0) {
      this->canvas_width_ = 800;
      this->canvas_height_ = 480;
      ESP_LOGW(TAG, "Canvas not set, using default dimensions: %dx%d", this->canvas_width_, this->canvas_height_);
    }
  }
#endif
}

bool PictureViewer::read_file_(const std::string &path, std::vector<uint8_t> &data) {
#ifdef USE_STORAGE
  if (this->file_manager_ == nullptr) {
    ESP_LOGW(TAG, "File manager not set");
    return false;
  }

  FILE *f = fopen(path.c_str(), "rb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
    return false;
  }

  // Get file size
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  // Read file
  data.resize(size);
  size_t read = fread(data.data(), 1, size, f);
  fclose(f);

  if (read != static_cast<size_t>(size)) {
    ESP_LOGE(TAG, "Failed to read file: %s", path.c_str());
    return false;
  }

  return true;
#else
  ESP_LOGE(TAG, "Storage host not available");
  return false;
#endif
}

uint8_t *PictureViewer::allocate_image_buffer_(size_t size) {
  uint8_t *buffer = nullptr;

#ifdef USE_ESP32
  // For images >64KB, use cache-aligned PSRAM allocation (matches ESP-IDF JPEG driver approach)
  if (size > 65536) {
    // Get cache alignment requirement for PSRAM
    size_t cache_align = 64;
    // Align size up to cache line boundary (like JPEG decoder output buffers)
    size_t aligned_size = (size + cache_align - 1) & ~(cache_align - 1);
    // Allocate cache-aligned PSRAM (matches jpeg_alloc_decoder_mem for output buffers)
    buffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(cache_align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer != nullptr) {
      ESP_LOGD(TAG, "Allocated %zu bytes in cache-aligned PSRAM (align=%zu)", aligned_size, cache_align);
      return buffer;
    }

    // Fallback to regular PSRAM if cache-aligned allocation fails
    buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM (required for images >64KB)", size);
      return nullptr;
    }
    ESP_LOGD(TAG, "Allocated %zu bytes in PSRAM (non-aligned fallback)", size);
    return buffer;
  }

  // For smaller images, try PSRAM first, then fallback to heap
  buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer != nullptr) {
    ESP_LOGD(TAG, "Allocated %zu bytes in PSRAM", size);
    return buffer;
  }

  ESP_LOGD(TAG, "PSRAM allocation failed, using heap for %zu bytes", size);
#endif

  // Fallback to regular heap for smaller allocations
  buffer = static_cast<uint8_t *>(malloc(size));
  if (buffer != nullptr) {
    ESP_LOGD(TAG, "Allocated %zu bytes in heap", size);
  }
  return buffer;
}

// =====================================================
// Directory Management
// =====================================================

bool PictureViewer::set_current_directory(size_t index) {
  if (index >= this->directories_.size()) {
    ESP_LOGE(TAG, "Directory index %zu out of range (have %zu directories)", index, this->directories_.size());
    return false;
  }

  if (index == this->current_directory_index_) {
    ESP_LOGD(TAG, "Already on directory %zu", index);
    return true;
  }

  ESP_LOGI(TAG, "Switching from directory %zu (%s) to %zu (%s)", this->current_directory_index_,
           this->directories_[this->current_directory_index_].path.c_str(), index,
           this->directories_[index].path.c_str());

  this->current_directory_index_ = index;
  this->refresh_images();  // Reload images from new directory
  return true;
}

// =====================================================
// Overlay Icon Methods
// =====================================================

#ifdef USE_LVGL
void PictureViewer::show_overlay_icon_(bool is_playing) {
  if (this->canvas_ == nullptr || this->canvas_buffer_ == nullptr) {
    return;
  }

  this->overlay_visible_ = true;
  this->overlay_is_playing_ = is_playing;
  this->overlay_show_time_ = millis();

  // Draw the icon centered on canvas
  int center_x = this->canvas_width_ / 2;
  int center_y = this->canvas_height_ / 2;

  if (is_playing) {
    this->draw_play_icon_(center_x, center_y, this->overlay_icon_size_, this->overlay_icon_color_);
  } else {
    this->draw_pause_icon_(center_x, center_y, this->overlay_icon_size_, this->overlay_icon_color_);
  }

  lv_obj_invalidate(this->canvas_);
}

void PictureViewer::hide_overlay_icon_() {
  if (!this->overlay_visible_) {
    return;
  }

  this->overlay_visible_ = false;

  // Redraw the current image to remove overlay
  if (this->current_image_data_ != nullptr) {
    this->write_to_canvas_buffer_(this->current_image_data_, this->current_image_width_, this->current_image_height_);
    lv_obj_invalidate(this->canvas_);
  }
}

void PictureViewer::draw_play_icon_(int center_x, int center_y, int size, uint32_t color) {
  if (this->canvas_buffer_ == nullptr) {
    return;
  }

  // Convert RGB888 to RGB565
  uint16_t color565 = ((color >> 8) & 0xF800) | ((color >> 5) & 0x07E0) | ((color >> 3) & 0x001F);

  // Draw filled right-pointing triangle (taller than wide)
  // Triangle is roughly 0.75 width to height ratio
  int height = size;
  int width = (int) (size * 0.75f);

  // Triangle vertices (pointing right)
  int x1 = center_x - width / 2;  // Left vertex (top)
  int y1 = center_y - height / 2;
  int x2 = center_x - width / 2;  // Left vertex (bottom)
  int y2 = center_y + height / 2;
  int x3 = center_x + width / 2;  // Right vertex (tip)
  int y3 = center_y;

  // Fill triangle using scanline algorithm
  for (int y = y1; y <= y2; y++) {
    if (y < 0 || y >= this->canvas_height_)
      continue;

    // Calculate x range for this scanline
    // Interpolate between left edge and right tip
    float t = (float) (y - y1) / (float) (y2 - y1);

    // Left edge stays at x1, right edge goes from x1 to x3 and back
    int x_left = x1;
    int x_right;
    if (y <= y3) {
      // Upper half: interpolate from (x1,y1) to (x3,y3)
      float t_upper = (float) (y - y1) / (float) (y3 - y1);
      x_right = (int) (x1 + t_upper * (x3 - x1));
    } else {
      // Lower half: interpolate from (x3,y3) to (x2,y2)
      float t_lower = (float) (y - y3) / (float) (y2 - y3);
      x_right = (int) (x3 - t_lower * (x3 - x2));
    }

    // Draw scanline
    for (int x = x_left; x <= x_right; x++) {
      if (x >= 0 && x < this->canvas_width_) {
        this->canvas_buffer_[y * this->canvas_width_ + x] = color565;
      }
    }
  }
}

void PictureViewer::draw_pause_icon_(int center_x, int center_y, int size, uint32_t color) {
  if (this->canvas_buffer_ == nullptr) {
    return;
  }

  // Convert RGB888 to RGB565
  uint16_t color565 = ((color >> 8) & 0xF800) | ((color >> 5) & 0x07E0) | ((color >> 3) & 0x001F);

  // Draw two vertical rectangles
  int rect_width = size / 5;
  int rect_height = size;
  int gap = size / 6;

  // Left rectangle
  int left_x1 = center_x - gap / 2 - rect_width;
  int left_x2 = center_x - gap / 2;
  int y1 = center_y - rect_height / 2;
  int y2 = center_y + rect_height / 2;

  for (int y = y1; y <= y2; y++) {
    if (y < 0 || y >= this->canvas_height_)
      continue;
    for (int x = left_x1; x <= left_x2; x++) {
      if (x >= 0 && x < this->canvas_width_) {
        this->canvas_buffer_[y * this->canvas_width_ + x] = color565;
      }
    }
  }

  // Right rectangle
  int right_x1 = center_x + gap / 2;
  int right_x2 = center_x + gap / 2 + rect_width;

  for (int y = y1; y <= y2; y++) {
    if (y < 0 || y >= this->canvas_height_)
      continue;
    for (int x = right_x1; x <= right_x2; x++) {
      if (x >= 0 && x < this->canvas_width_) {
        this->canvas_buffer_[y * this->canvas_width_ + x] = color565;
      }
    }
  }
}

void PictureViewer::slide_thumbnails(bool show) {
#ifdef USE_LVGL
  if (this->thumbnail_container_ == nullptr) {
    return;
  }

  // Use z-order instead of positioning: move to foreground (show) or background (hide)
  // This is more performant and avoids positioning issues with different layouts/styles
  // Move the parent container (thumbnail_strip) instead of just the thumbnail list
  lv_obj_t *parent = lv_obj_get_parent(this->thumbnail_container_);

  if (show) {
    // Reset interaction timer when showing thumbnails
    this->last_thumbnail_interaction_time_ = millis();
    if (parent != nullptr) {
      lv_obj_clear_flag(parent, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(parent);
      // Force layout recalculation after unhiding (flex layout needs update)
      lv_obj_update_layout(parent);
      // Add clickable flag when showing (to receive gesture events for slide-out)
      if (this->thumbnail_slide_enabled_) {
        lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
        ESP_LOGD(TAG, "Added clickable flag on thumbnail parent (visible state)");
      }
      ESP_LOGD(TAG, "Thumbnails slid IN (unhidden, moved parent to foreground)");
    } else {
      lv_obj_clear_flag(this->thumbnail_container_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(this->thumbnail_container_);
      // Force layout recalculation after unhiding (flex layout needs update)
      lv_obj_update_layout(this->thumbnail_container_);
      ESP_LOGD(TAG, "Thumbnails slid IN (unhidden, moved container to foreground)");
    }

    // Explicitly clear HIDDEN flag from valid thumbnail button widgets and restore their size
    // (Only unused widgets beyond images_.size() should remain hidden)
    const int thumb_w = this->thumbnail_config_.width;
    const int thumb_h = this->thumbnail_config_.height;
    size_t visible_count = std::min(this->images_.size(), this->thumbnail_cache_.size());

    // Debug: Log container size and position
    if (parent != nullptr) {
      ESP_LOGD(TAG, "Parent container size: %dx%d at (%d,%d)", lv_obj_get_width(parent), lv_obj_get_height(parent),
               lv_obj_get_x(parent), lv_obj_get_y(parent));
    }
    ESP_LOGD(TAG, "Thumbnail container size: %dx%d at (%d,%d)", lv_obj_get_width(this->thumbnail_container_),
             lv_obj_get_height(this->thumbnail_container_), lv_obj_get_x(this->thumbnail_container_),
             lv_obj_get_y(this->thumbnail_container_));

    for (size_t i = 0; i < visible_count; i++) {
      // Only unhide and resize widgets that have loaded image data
      if (this->thumbnail_cache_[i].thumb_btn_ != nullptr && this->thumbnail_cache_[i].loaded) {
        lv_obj_clear_flag(this->thumbnail_cache_[i].thumb_btn_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(this->thumbnail_cache_[i].thumb_btn_, thumb_w, thumb_h);

        // Debug: Log first widget details
        if (i == 0) {
          lv_obj_t *btn = this->thumbnail_cache_[i].thumb_btn_;
          ESP_LOGD(TAG, "Widget 0: size=%dx%d at (%d,%d)", lv_obj_get_width(btn), lv_obj_get_height(btn),
                   lv_obj_get_x(btn), lv_obj_get_y(btn));
          if (this->thumbnail_cache_[i].thumb_canvas_ != nullptr) {
            lv_obj_t *canvas = this->thumbnail_cache_[i].thumb_canvas_;
            ESP_LOGD(TAG, "Canvas 0: size=%dx%d at (%d,%d)", lv_obj_get_width(canvas), lv_obj_get_height(canvas),
                     lv_obj_get_x(canvas), lv_obj_get_y(canvas));
          }
        }

        // Also ensure canvas inside button is properly centered and sized
        if (this->thumbnail_cache_[i].thumb_canvas_ != nullptr) {
          lv_obj_center(this->thumbnail_cache_[i].thumb_canvas_);
          lv_obj_invalidate(this->thumbnail_cache_[i].thumb_canvas_);
        }

        lv_obj_invalidate(this->thumbnail_cache_[i].thumb_btn_);
      }
    }
    ESP_LOGD(TAG, "Cleared HIDDEN flag and restored size for %zu valid thumbnail widgets", visible_count);
  } else {
    if (parent != nullptr) {
      lv_obj_add_flag(parent, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_background(parent);
      // Clear clickable flag when hiding (to prevent blocking canvas touch events)
      if (this->thumbnail_slide_enabled_) {
        lv_obj_clear_flag(parent, LV_OBJ_FLAG_CLICKABLE);
        ESP_LOGD(TAG, "Cleared clickable flag on thumbnail parent (hidden state)");
      }
      ESP_LOGD(TAG, "Thumbnails slid OUT (hidden, moved parent to background)");
    } else {
      lv_obj_add_flag(this->thumbnail_container_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_background(this->thumbnail_container_);
      ESP_LOGD(TAG, "Thumbnails slid OUT (hidden, moved container to background)");
    }
  }

  this->thumbnails_visible_ = show;

  // Force immediate refresh after all flag/style changes are complete
  lv_refr_now(NULL);
#endif
}

// =====================================================
// Scroll Tracking and Smart Thumbnail Preloading
// =====================================================

void PictureViewer::on_thumbnail_scroll_(lv_event_t *event) {
  if (this->thumbnail_container_ == nullptr) {
    return;
  }

  uint32_t now = millis();

  // Reset interaction timer on scroll
  this->last_thumbnail_interaction_time_ = now;

  int32_t scroll_pos;

  // Get scroll position based on layout
  if (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL) {
    scroll_pos = lv_obj_get_scroll_x(this->thumbnail_container_);
  } else {
    scroll_pos = lv_obj_get_scroll_y(this->thumbnail_container_);
  }

  // Calculate scroll velocity
  if (this->last_scroll_time_ > 0) {
    uint32_t time_diff = now - this->last_scroll_time_;
    if (time_diff > 0) {
      int32_t pos_diff = scroll_pos - this->last_scroll_pos_;
      this->scroll_velocity_ = (float) pos_diff / (float) time_diff;
    }
  }

  this->last_scroll_pos_ = scroll_pos;
  this->last_scroll_time_ = now;

  // Trigger smart preloading
  this->preload_thumbnails_for_viewport_();
}

void PictureViewer::preload_thumbnails_for_viewport_() {
  if (this->thumbnail_container_ == nullptr || this->images_.empty()) {
    return;
  }

  // Determine which thumbnails are visible and which to preload
  const int thumb_size = (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL)
                             ? this->thumbnail_config_.width
                             : this->thumbnail_config_.height;
  const int container_size = (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL)
                                 ? lv_obj_get_width(this->thumbnail_container_)
                                 : lv_obj_get_height(this->thumbnail_container_);

  int32_t scroll_pos = (this->thumbnail_config_.layout == ThumbnailLayout::HORIZONTAL)
                           ? lv_obj_get_scroll_x(this->thumbnail_container_)
                           : lv_obj_get_scroll_y(this->thumbnail_container_);

  // Calculate visible range
  int first_visible = std::max(0, (int) (scroll_pos / (thumb_size + this->thumbnail_config_.spacing)));
  int last_visible =
      std::min((int) this->images_.size() - 1,
               (int) ((scroll_pos + container_size) / (thumb_size + this->thumbnail_config_.spacing)) + 1);

  // Determine preload direction based on velocity
  bool scrolling_forward = this->scroll_velocity_ > 0.1f;
  bool scrolling_backward = this->scroll_velocity_ < -0.1f;

  // Preload 2-3 thumbnails in scroll direction
  int preload_count = 3;

  if (scrolling_forward) {
    // Preload ahead
    for (int i = last_visible + 1; i < std::min(last_visible + preload_count + 1, (int) this->images_.size()); i++) {
      this->request_thumbnail(i);
    }
  } else if (scrolling_backward) {
    // Preload behind
    for (int i = first_visible - 1; i >= std::max(first_visible - preload_count, 0); i--) {
      this->request_thumbnail(i);
    }
  }

  // Preload currently visible thumbnails
  for (int i = first_visible; i <= last_visible; i++) {
    if (i >= 0 && i < (int) this->images_.size()) {
      this->request_thumbnail(i);
    }
  }

  // Evict thumbnails that are far from viewport (with hysteresis)
  const int hysteresis = 3;  // Keep 3 thumbnails behind viewport
  for (size_t i = 0; i < this->thumbnail_cache_.size(); i++) {
    auto &cache_entry = this->thumbnail_cache_[i];
    if (!cache_entry.loaded || cache_entry.image_index < 0) {
      continue;
    }

    int distance_from_viewport;
    if (cache_entry.image_index < first_visible) {
      distance_from_viewport = first_visible - cache_entry.image_index;
    } else if (cache_entry.image_index > last_visible) {
      distance_from_viewport = cache_entry.image_index - last_visible;
    } else {
      distance_from_viewport = 0;  // In viewport
    }

    // Evict if far away (but keep hysteresis behind)
    bool is_behind_viewport = cache_entry.image_index < first_visible;
    int eviction_threshold = is_behind_viewport ? hysteresis : preload_count;

    if (distance_from_viewport > eviction_threshold) {
      // Store index before clearing (for logging)
      int evicted_index = cache_entry.image_index;
      // Mark for eviction (will be reused by next load_thumbnail_ call)
      cache_entry.loaded = false;
      cache_entry.image_index = -1;
      ESP_LOGD(TAG, "Evicting thumbnail %d (distance: %d)", evicted_index, distance_from_viewport);
    }
  }
}
#endif

// =====================================================
// Actions
// =====================================================

template<typename... Ts> class ShowImageIndexAction : public Action<Ts...> {
 public:
  explicit ShowImageIndexAction(PictureViewer *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(size_t, index)

  void play(Ts... x) override {
    size_t index = this->index_.value(x...);
    this->parent_->show_image(index);
  }

 protected:
  PictureViewer *parent_;
};

template<typename... Ts> class ShowImagePathAction : public Action<Ts...> {
 public:
  explicit ShowImagePathAction(PictureViewer *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) override {
    std::string path = this->path_.value(x...);
    this->parent_->show_image(path);
  }

 protected:
  PictureViewer *parent_;
};

template<typename... Ts> class NextImageAction : public Action<Ts...> {
 public:
  explicit NextImageAction(PictureViewer *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->next_image(); }

 protected:
  PictureViewer *parent_;
};

template<typename... Ts> class PreviousImageAction : public Action<Ts...> {
 public:
  explicit PreviousImageAction(PictureViewer *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->previous_image(); }

 protected:
  PictureViewer *parent_;
};

template<typename... Ts> class StartSlideshowAction : public Action<Ts...> {
 public:
  explicit StartSlideshowAction(PictureViewer *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->start_slideshow(); }

 protected:
  PictureViewer *parent_;
};

template<typename... Ts> class StopSlideshowAction : public Action<Ts...> {
 public:
  explicit StopSlideshowAction(PictureViewer *parent) : parent_(parent) {}

  void play(Ts... x) override { this->parent_->stop_slideshow(); }

 protected:
  PictureViewer *parent_;
};

// =====================================================
// Background Thumbnail Preloading Task
// =====================================================

#ifdef USE_ESP_IDF
void PictureViewer::preload_task_func_(void *param) {
  auto *viewer = static_cast<PictureViewer *>(param);

  ESP_LOGI(TAG, "Thumbnail preload task started");

  // Preload thumbnails aggressively at boot
  // Start from current image and expand outward
  size_t current_idx = (viewer->current_index_ >= 0) ? viewer->current_index_ : 0;
  size_t total_images = viewer->images_.size();

  // Preload current image thumbnail first
  if (current_idx < total_images) {
    viewer->request_thumbnail(current_idx);
    vTaskDelay(pdMS_TO_TICKS(50));  // Small delay between loads
  }

  // Preload forward from current (next 10-15 images)
  for (size_t i = 1; i <= 15 && (current_idx + i) < total_images; i++) {
    viewer->request_thumbnail(current_idx + i);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Preload backward from current (previous 10-15 images)
  for (size_t i = 1; i <= 15 && current_idx >= i; i++) {
    viewer->request_thumbnail(current_idx - i);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGI(TAG, "Thumbnail preload task completed - preloaded ~30 thumbnails");

  // Mark preload as complete
  viewer->preload_complete_ = true;

  // Task self-deletes
  viewer->preload_task_handle_ = nullptr;
  vTaskDelete(NULL);
}

void PictureViewer::start_preload_task_() {
  // Check if buffer is allocated before starting preload
  if (this->thumbnail_buffer_array_ == nullptr) {
    ESP_LOGW(TAG, "Thumbnail buffer not allocated yet - skipping preload task start");
    return;
  }

  // Stop existing task if running
  this->stop_preload_task_();

  // Reset preload flag
  this->preload_complete_ = false;

  // Create new preload task with lower priority to not interfere with main loop
  // Use core 0 (opposite of LVGL which typically runs on core 1)
  xTaskCreatePinnedToCore(preload_task_func_, "thumb_preload", 4096,  // 4KB stack
                          this,                                       // parameter
                          1,                                          // priority (low)
                          &this->preload_task_handle_,                // handle
                          0                                           // core 0
  );

  if (this->preload_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create thumbnail preload task");
  } else {
    ESP_LOGI(TAG, "Thumbnail preload task created");
  }
}

void PictureViewer::stop_preload_task_() {
  if (this->preload_task_handle_ != nullptr) {
    ESP_LOGD(TAG, "Stopping thumbnail preload task");
    vTaskDelete(this->preload_task_handle_);
    this->preload_task_handle_ = nullptr;
  }
}
#endif  // USE_ESP_IDF

}  // namespace picture_viewer
}  // namespace esphome
