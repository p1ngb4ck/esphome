#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#ifdef USE_STORAGE_HOST
#include "esphome/components/storage_host/file_manager.h"
#endif

#ifdef USE_TRANSCODER
#include "esphome/components/transcoder/transcoder.h"
#endif

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace esphome {
namespace picture_viewer {

// Forward declarations
class PictureViewer;

// Thumbnail click trigger
class ThumbnailClickTrigger : public Trigger<size_t> {
 public:
  explicit ThumbnailClickTrigger(PictureViewer *parent) : parent_(parent) {}

 protected:
  PictureViewer *parent_;
};

// Directory configuration with per-directory JPEG decoder settings
struct DirectoryConfig {
  std::string path;
  int jpeg_rgb_order{0};                    // Default: RGB (JPEG_DEC_RGB_ELEMENT_ORDER_RGB)
  int jpeg_color_space{0};                  // Default: BT601
  uint32_t jpeg_output_format{0x02000002};  // Default: RGB565
};

// Image information
struct ImageEntry {
  std::string path;
  std::string filename;
  uint64_t size{0};
  int width{0};
  int height{0};
  bool thumbnail_loaded{false};
  std::vector<uint8_t> thumbnail_data;  // RGB565 thumbnail data
};

// Slideshow mode
enum class SlideshowMode {
  STOPPED,
  PLAYING,
  PAUSED,
};

// Image fit mode for canvas display
enum class ImageFitMode {
  SCALE_TO_FIT,   // Scale to fit canvas, maintain aspect ratio (may have black bars)
  SCALE_TO_FILL,  // Scale to fill canvas, maintain aspect ratio (may crop)
  STRETCH,        // Stretch to fill canvas, ignore aspect ratio
  CENTER,         // Center image, no scaling
};

// Thumbnail layout mode for dynamic LVGL generation
enum class ThumbnailLayout {
  HORIZONTAL,  // Horizontal strip/ribbon
  VERTICAL,    // Vertical strip/ribbon
  GRID,        // Grid layout with wrapping
};

// Thumbnail configuration
struct ThumbnailConfig {
  bool enabled{true};
  int width{120};                           // Thumbnail width in pixels
  int height{90};                           // Thumbnail height in pixels
  size_t max_count{10};                     // Maximum number of thumbnails in memory
  size_t decode_work_buffer_size{2097152};  // 2MB work buffer for JPEG decode (reused)
  bool lazy_load{true};                     // Load thumbnails on-demand
  size_t preload_count{5};                  // Number of thumbnails to preload initially

  // LVGL dynamic generation options
  ThumbnailLayout layout{ThumbnailLayout::HORIZONTAL};  // Layout mode
  int spacing{5};                                       // Pixels between thumbnails
  int grid_columns{3};                                  // Columns for GRID layout
};

// Thumbnail cache entry with LRU tracking
struct ThumbnailCacheEntry {
  int image_index{-1};           // Index of image this thumbnail belongs to
  uint8_t *data{nullptr};        // Points into thumbnail_buffer_array_ (not owned)
  uint32_t last_access_time{0};  // Last access timestamp (millis)
  bool loaded{false};            // Whether thumbnail is loaded
#ifdef USE_LVGL
  lv_obj_t *thumb_btn_{nullptr};     // LVGL thumbnail button object pointer
  lv_obj_t *thumb_canvas_{nullptr};  // LVGL canvas widget pointer
  lv_obj_t *thumb_label_{nullptr};   // LVGL label widget pointer (optional)
#endif
};

// =====================================================
// PictureViewer Component
// =====================================================

class PictureViewer : public Component {
 public:
  PictureViewer() = default;
  ~PictureViewer();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // =====================================================
  // Configuration
  // =====================================================

#ifdef USE_STORAGE_HOST
  void set_file_manager(storage_host::FileManager *fm) { this->file_manager_ = fm; }
#endif

#ifdef USE_TRANSCODER
  void set_transcoder(transcoder::Transcoder *tc) { this->transcoder_ = tc; }
#endif

#ifdef USE_LVGL
  void set_canvas_id(const std::string &canvas_id) { this->canvas_id_ = canvas_id; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  void set_display(display::Display *display) { this->display_ = display; }
#endif

  void set_slideshow_interval(uint32_t interval_ms) { this->slideshow_interval_ms_ = interval_ms; }
  void set_thumbnail_width(int width) {
    this->thumbnail_width_ = width;
    this->thumbnail_config_.width = width;
  }
  void set_thumbnail_height(int height) {
    this->thumbnail_height_ = height;
    this->thumbnail_config_.height = height;
  }
  void set_enable_thumbnails(bool enable) {
    this->enable_thumbnails_ = enable;
    this->thumbnail_config_.enabled = enable;
  }
  void set_thumbnail_max_count(size_t count) { this->thumbnail_config_.max_count = count; }
  void set_thumbnail_decode_work_buffer_size(size_t bytes) { this->thumbnail_config_.decode_work_buffer_size = bytes; }
  void set_thumbnail_lazy_load(bool lazy) { this->thumbnail_config_.lazy_load = lazy; }
  void set_thumbnail_preload_count(size_t count) { this->thumbnail_config_.preload_count = count; }
  void set_thumbnail_layout(ThumbnailLayout layout) { this->thumbnail_config_.layout = layout; }
  void set_thumbnail_spacing(int spacing) { this->thumbnail_config_.spacing = spacing; }
  void set_thumbnail_grid_columns(int columns) { this->thumbnail_config_.grid_columns = columns; }
  void set_fit_mode(ImageFitMode mode) { this->fit_mode_ = mode; }
  void set_overlay_icon_size(int size) { this->overlay_icon_size_ = size; }
  void set_overlay_icon_color(uint32_t color) { this->overlay_icon_color_ = color; }
  void set_overlay_duration(uint32_t duration_ms) { this->overlay_duration_ms_ = duration_ms; }
  void set_default_image_index(int index) { this->default_image_index_ = index; }

#ifdef USE_LVGL
  void set_thumbnail_container(lv_obj_t *container) { this->thumbnail_container_ = container; }
  void set_thumbnail_style(lv_style_t *style) { this->thumbnail_style_ = style; }
  void set_thumbnail_active_style(lv_style_t *style) { this->thumbnail_active_style_ = style; }
  void set_thumbnail_container_style(lv_style_t *style) { this->thumbnail_container_style_ = style; }
  void set_thumbnail_label_pattern(const std::string &pattern) { this->thumbnail_label_pattern_ = pattern; }
  void set_thumbnail_label_style(lv_style_t *style) { this->thumbnail_label_style_ = style; }
#endif

  // Directory configuration
  void add_directory(const std::string &path, int rgb_order, int color_space, uint32_t output_format) {
    DirectoryConfig dir;
    dir.path = path;
    dir.jpeg_rgb_order = rgb_order;
    dir.jpeg_color_space = color_space;
    dir.jpeg_output_format = output_format;
    this->directories_.push_back(dir);
  }

  // Switch to a different directory by index
  bool set_current_directory(size_t index);

  // Get current directory
  const DirectoryConfig *get_current_directory() const {
    if (this->current_directory_index_ < this->directories_.size()) {
      return &this->directories_[this->current_directory_index_];
    }
    return nullptr;
  }

  // Get directory count
  size_t get_directory_count() const { return this->directories_.size(); }

#ifdef USE_STORAGE_HOST
  // =====================================================
  // FileManager Integration
  // =====================================================

  /// Called by file_manager to provide updated file list for a directory
  /// This is the main entry point for file_manager to push directory updates
  /// @param directory_path The directory path (must match one of the configured directories)
  /// @param files The complete file list for that directory
  void update_directory_files(const std::string &directory_path, const std::vector<storage_host::FileInfo> &files) {
    this->update_directory_files_(directory_path, files);
  }
#endif

  // Get current directory index
  size_t get_current_directory_index() const { return this->current_directory_index_; }

  // =====================================================
  // Picture Control API
  // =====================================================

  /// Load and display a specific image by index
  bool show_image(size_t index);

  /// Load and display a specific image by path
  bool show_image(const std::string &path);

  /// Show next image
  bool next_image();

  /// Show previous image
  bool previous_image();

  /// Start slideshow
  void start_slideshow();

  /// Stop slideshow
  void stop_slideshow();

  /// Pause slideshow
  void pause_slideshow();

  /// Toggle slideshow play/pause
  void toggle_slideshow();

  /// Set slideshow interval
  void set_slideshow_interval_runtime(uint32_t interval_ms) { this->slideshow_interval_ms_ = interval_ms; }

  /// Get current slideshow state
  SlideshowMode get_slideshow_mode() const { return this->slideshow_mode_; }

  /// Get total image count
  size_t get_image_count() const { return this->images_.size(); }

  /// Get current image index
  int get_current_index() const { return this->current_index_; }

  /// Get current image info
  const ImageEntry *get_current_image() const {
    if (this->current_index_ >= 0 && this->current_index_ < static_cast<int>(this->images_.size())) {
      return &this->images_[this->current_index_];
    }
    return nullptr;
  }

  /// Refresh image list from directory
  void refresh_images();

  /// Enable/disable fullscreen mode
  void set_fullscreen(bool fullscreen);
  bool is_fullscreen() const { return this->fullscreen_; }

  // =====================================================
  // Thumbnail API
  // =====================================================

  /// Get thumbnail data for a specific image index
  /// Returns pointer to RGB565 thumbnail data, or nullptr if not available
  /// Updates width and height parameters with thumbnail dimensions
  const uint8_t *get_thumbnail_data(size_t index, int &width, int &height);

  /// Get the pointer to a LVGL thumbnail button object for a specific image index
  /// Returns nullptr if not available
#ifdef USE_LVGL
  lv_obj_t *get_thumbnail_button(size_t index);
  void set_thumbnail_button(size_t index, lv_obj_t *btn) {
    int cache_index = this->find_thumbnail_in_cache_(index);
    if (cache_index >= 0) {
      this->thumbnail_cache_[cache_index].thumb_btn_ = btn;
    }
  }
#endif

  /// Get total number of thumbnails that can be generated
  size_t get_thumbnail_count() const { return this->images_.size(); }

  /// Request thumbnail to be loaded (for lazy loading)
  /// Returns true if thumbnail is already loaded, false if it needs to be generated
  bool request_thumbnail(size_t index);

  /// Configure thumbnail system at runtime
  void set_thumbnail_config(const ThumbnailConfig &config) { this->thumbnail_config_ = config; }

  /// Get current thumbnail configuration
  const ThumbnailConfig &get_thumbnail_config() const { return this->thumbnail_config_; }

  /// Set callback for thumbnail ready notifications
  /// Callback receives image index when thumbnail is loaded
  void set_thumbnail_ready_callback(std::function<void(size_t)> callback) {
    this->thumbnail_ready_callback_ = callback;
  }

  // =====================================================
  // Thumbnail Click Automation
  // =====================================================

  /// Register thumbnail click trigger
  void add_on_thumbnail_click_callback(ThumbnailClickTrigger *trigger) {
    this->thumbnail_click_triggers_.push_back(trigger);
  }

  /// Register thumbnail click lambda callback
  void add_on_thumbnail_click_callback(std::function<void(size_t)> &&callback) {
    this->thumbnail_click_callbacks_.add(std::move(callback));
  }

 protected:
  // Configuration
#ifdef USE_STORAGE_HOST
  storage_host::FileManager *file_manager_{nullptr};
#endif

#ifdef USE_TRANSCODER
  transcoder::Transcoder *transcoder_{nullptr};
#endif

#ifdef USE_LVGL
  std::string canvas_id_;      // LVGL canvas widget ID (for debugging)
  lv_obj_t *canvas_{nullptr};  // LVGL canvas object pointer
  display::Display *display_{nullptr};
#endif

  // Directories with per-directory JPEG decoder settings
  std::vector<DirectoryConfig> directories_;
  size_t current_directory_index_{0};

  uint32_t slideshow_interval_ms_{5000};  // Default: 5 seconds
  int thumbnail_width_{120};
  int thumbnail_height_{90};
  bool enable_thumbnails_{true};
  ImageFitMode fit_mode_{ImageFitMode::SCALE_TO_FIT};  // Default: scale to fit

  // Overlay icon configuration
  int overlay_icon_size_{128};             // Size of play/pause icon in pixels
  uint32_t overlay_icon_color_{0xFFFFFF};  // RGB color (white default)
  uint32_t overlay_duration_ms_{1500};     // Duration to show overlay (1.5s default)
  int default_image_index_{0};             // Default image to show on startup (0=first, -1=disabled)

  // State
  std::vector<ImageEntry> images_;
  int current_index_{-1};
  SlideshowMode slideshow_mode_{SlideshowMode::STOPPED};
  uint32_t last_slideshow_time_{0};
  bool fullscreen_{false};
  int canvas_width_{0};
  int canvas_height_{0};

  // Overlay state
  bool overlay_visible_{false};
  uint32_t overlay_show_time_{0};
  bool overlay_is_playing_{true};  // true = play icon, false = pause icon

  // Scroll tracking for smart thumbnail preloading
  int32_t last_scroll_pos_{0};
  uint32_t last_scroll_time_{0};
  float scroll_velocity_{0.0f};  // pixels per millisecond

  // Current displayed image (allocated in PSRAM if available)
  uint8_t *current_image_data_{nullptr};  // RGB565 data in PSRAM
  int current_image_width_{0};
  int current_image_height_{0};
  size_t current_image_size_{0};

  // Pre-loaded next image for smooth slideshow transitions (PSRAM)
  uint8_t *next_image_data_{nullptr};  // Pre-loaded next image in PSRAM
  int next_image_width_{0};
  int next_image_height_{0};
  size_t next_image_size_{0};
  int next_image_index_{-1};  // Index of pre-loaded image

  // Canvas buffer management (adopted from LVGL canvas - not owned by us)
  uint16_t *canvas_buffer_{nullptr};  // Points to canvas's buffer (owned by LVGL)
  int canvas_buffer_width_{0};
  int canvas_buffer_height_{0};
  size_t canvas_buffer_pixels_{0};
  bool canvas_buffer_ready_{false};

  // Thumbnail management
  ThumbnailConfig thumbnail_config_;
  std::vector<ThumbnailCacheEntry> thumbnail_cache_;
  std::function<void(size_t)> thumbnail_ready_callback_;  // Called when thumbnail is loaded

  // Thumbnail click automation
  std::vector<ThumbnailClickTrigger *> thumbnail_click_triggers_;
  CallbackManager<void(size_t)> thumbnail_click_callbacks_;

  // Work buffer for JPEG decoding (reused for every decode operation)
  uint8_t *decode_work_buffer_{nullptr};
  size_t decode_work_buffer_size_{0};

  // Thumbnail buffer array (single PSRAM allocation for all thumbnails)
  uint8_t *thumbnail_buffer_array_{nullptr};
  size_t thumbnail_buffer_size_per_slot_{0};  // Size per thumbnail: width * height * 2 (RGB565)

#ifdef USE_LVGL
  lv_obj_t *thumbnail_container_{nullptr};          // Parent container for dynamic thumbnail widgets
  lv_style_t *thumbnail_style_{nullptr};            // User-provided style for thumbnails
  lv_style_t *thumbnail_active_style_{nullptr};     // User-provided style for active thumbnail
  lv_style_t *thumbnail_container_style_{nullptr};  // User-provided style for container
  lv_style_t *thumbnail_label_style_{nullptr};      // User-provided style for thumbnail labels

  // Thumbnail label configuration
  std::string thumbnail_label_pattern_;  // Label text pattern (e.g., "{index}", "{filename}")

  // Default styles (created if user doesn't provide custom styles)
  lv_style_t default_thumbnail_style_;
  lv_style_t default_thumbnail_active_style_;
  lv_style_t default_container_style_;
  bool default_styles_initialized_{false};
#endif

  // =====================================================
  // Internal Methods
  // =====================================================

  /// Scan directory for images from provided file list
  void scan_directory_(const std::vector<storage_host::FileInfo> &files);

  /// Load JPEG file and decode
  bool load_jpeg_(const std::string &path, std::vector<uint8_t> &rgb565_data, int &width, int &height,
                  int target_width = 0, int target_height = 0);

  /// Decode JPEG using esp_jpeg (ESP32-S2/S3)
#ifdef USE_ESP_JPEG_DECODER
  bool decode_jpeg_esp_(const std::vector<uint8_t> &jpeg_data, std::vector<uint8_t> &rgb565_data, int &width,
                        int &height, int target_width = 0, int target_height = 0);
#endif

  /// Decode JPEG using hardware decoder (ESP32-P4)
#ifdef USE_HARDWARE_JPEG_DECODER
  bool decode_jpeg_hardware_(const std::vector<uint8_t> &jpeg_data, std::vector<uint8_t> &rgb565_data, int &width,
                             int &height, int target_width = 0, int target_height = 0);
#endif

  /// Decode JPEG using JPEGDec library (fallback)
#ifdef USE_JPEGDEC
  bool decode_jpeg_jpegdec_(const std::vector<uint8_t> &jpeg_data, std::vector<uint8_t> &rgb565_data, int &width,
                            int &height, int target_width = 0, int target_height = 0);
  static int jpeg_decode_callback_(JPEGDRAW *draw);
  JPEGDEC *jpeg_decoder_{nullptr};
  std::vector<uint8_t> *decode_target_{nullptr};
  int decode_width_{0};
#endif

  /// Resize RGB565 image
  void resize_image_(const std::vector<uint8_t> &src_data, int src_width, int src_height,
                     std::vector<uint8_t> &dst_data, int dst_width, int dst_height);

  /// Ensure canvas buffer is properly initialized and adopted from LVGL
  void ensure_canvas_buffer_();

  /// Resize canvas buffer to match current canvas dimensions
  void resize_canvas_buffer_();

  /// Write RGB565 image data directly to canvas buffer (with scaling/positioning)
  void write_to_canvas_buffer_(const uint8_t *rgb565_data, int img_width, int img_height);

  /// Update canvas with current image
  void update_canvas_();

  /// Generate thumbnail for image
  bool generate_thumbnail_(ImageEntry &entry);

  /// Thumbnail management internal methods
  /// Load thumbnail for specific image index into cache
  bool load_thumbnail_(size_t image_index);

  /// Evict oldest (least recently used) thumbnail from cache
  void evict_oldest_thumbnail_();

  /// Update LRU timestamp for a cache entry
  void update_lru_timestamp_(size_t cache_index);

  /// Find thumbnail in cache by image index (returns cache index or -1 if not found)
  int find_thumbnail_in_cache_(size_t image_index) const;

  /// Allocate work buffer for JPEG decoding
  bool allocate_work_buffer_();

  /// Allocate thumbnail buffer array
  bool allocate_thumbnail_buffer_array_();

  /// Free work buffer
  void free_work_buffer_();

  /// Free thumbnail buffer array
  void free_thumbnail_buffer_array_();

#ifdef USE_LVGL
  /// Create dynamic LVGL thumbnail widgets
  void create_thumbnail_widgets_();

  /// Update thumbnail widget at cache index with image data
  void update_thumbnail_widget_(size_t cache_index);

  /// Highlight active thumbnail and unhighlight others
  void update_thumbnail_highlighting_(int active_image_index);

  /// Handle scroll events for smart thumbnail preloading
  void on_thumbnail_scroll_(lv_event_t *event);

  /// Preload thumbnails based on scroll position and velocity
  void preload_thumbnails_for_viewport_();

  /// Initialize default thumbnail styles if user didn't provide any
  void init_default_thumbnail_styles_();

  /// Apply thumbnail style (default or user-provided)
  void apply_thumbnail_style_(lv_obj_t *obj, bool is_active);

  /// Apply container style (default or user-provided)
  void apply_container_style_(lv_obj_t *obj);

  /// Format label text from pattern (e.g., "{index}" -> "1", "{filename}" -> "photo.jpg")
  std::string format_thumbnail_label_(size_t image_index);
#endif

  /// Show play/pause overlay icon
  void show_overlay_icon_(bool is_playing);

  /// Hide overlay icon
  void hide_overlay_icon_();

  /// Draw play icon (triangle) on canvas
  void draw_play_icon_(int center_x, int center_y, int size, uint32_t color);

  /// Draw pause icon (two rectangles) on canvas
  void draw_pause_icon_(int center_x, int center_y, int size, uint32_t color);

  /// Get canvas dimensions
  void update_canvas_dimensions_();

  /// Read file from storage
  bool read_file_(const std::string &path, std::vector<uint8_t> &data);

  /// Allocate image buffer in PSRAM if available, otherwise in heap
  uint8_t *allocate_image_buffer_(size_t size);

  /// Free image buffer (PSRAM or heap)
  void free_image_buffer_(uint8_t *buffer);

  /// Pre-load next image for smooth slideshow
  void preload_next_image_();

  /// Swap current and next image buffers (for slideshow)
  void swap_to_preloaded_image_();

  /// FileManager callback - called when directory changes
  /// @param directory_path The directory path that changed
  /// @param files The updated file list for that directory
  void update_directory_files_(const std::string &directory_path, const std::vector<storage_host::FileInfo> &files);
};

}  // namespace picture_viewer
}  // namespace esphome
