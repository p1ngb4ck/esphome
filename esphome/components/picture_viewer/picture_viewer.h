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

// Thumbnail configuration
struct ThumbnailConfig {
  bool enabled{true};
  int width{200};
  int height{150};
  size_t max_count{20};        // Maximum number of thumbnails in memory
  size_t max_memory{2097152};  // Maximum memory budget (default: 2MB)
  bool lazy_load{true};        // Load thumbnails on-demand
  size_t preload_count{10};    // Number of thumbnails to preload initially
};

// Thumbnail cache entry with LRU tracking
struct ThumbnailCacheEntry {
  int image_index{-1};           // Index of image this thumbnail belongs to
  std::vector<uint8_t> data;     // RGB565 thumbnail data
  uint32_t last_access_time{0};  // Last access timestamp (millis)
  size_t memory_usage{0};        // Memory used by this entry
  bool loaded{false};            // Whether thumbnail is loaded
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
  void set_thumbnail_max_memory(size_t bytes) { this->thumbnail_config_.max_memory = bytes; }
  void set_thumbnail_lazy_load(bool lazy) { this->thumbnail_config_.lazy_load = lazy; }
  void set_thumbnail_preload_count(size_t count) { this->thumbnail_config_.preload_count = count; }
  void set_fit_mode(ImageFitMode mode) { this->fit_mode_ = mode; }

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

  // State
  std::vector<ImageEntry> images_;
  int current_index_{-1};
  SlideshowMode slideshow_mode_{SlideshowMode::STOPPED};
  uint32_t last_slideshow_time_{0};
  bool fullscreen_{false};
  int canvas_width_{0};
  int canvas_height_{0};

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
  size_t current_memory_usage_{0};                        // Total memory used by thumbnails in cache
  std::function<void(size_t)> thumbnail_ready_callback_;  // Called when thumbnail is loaded

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
  void on_directory_changed_(const storage_host::DirectoryChangeInfo &info);
};

}  // namespace picture_viewer
}  // namespace esphome
