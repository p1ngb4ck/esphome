# Picture Viewer Component

A sophisticated picture viewer for ESPHome with LVGL integration, supporting dynamic image loading, slideshow functionality, and thumbnails.

## Features

- ✅ **Dynamic Image Loading** - FileManager integration for automatic updates
- ✅ **Multiple JPEG Decoders** - Optimized for each platform:
  - ESP32-S2/S3: `esp_jpeg` (faster than JPEGDec)
  - ESP32-P4: Hardware JPEG decoder
  - Other platforms: JPEGDec library
- ✅ **PSRAM Optimization** - Images stored in PSRAM when available
- ✅ **Pre-loading** - Next image pre-loaded during slideshow for smooth transitions
- ✅ **Slideshow** - Configurable interval with play/pause/stop
- ✅ **Manual Selection** - Navigate with next/previous buttons
- ✅ **Thumbnails** - ESP32-P4 hardware-accelerated thumbnail generation
- ✅ **Fullscreen Mode** - Dynamic resizing
- ✅ **LVGL Integration** - Native canvas and UI support

## Hardware Requirements

- **ESP32-S2/S3/P4** recommended (hardware acceleration)
- **PSRAM** highly recommended for smooth operation
- **SD card or network storage** for photos
- **Display** with LVGL support
- **Optional touchscreen** for UI interaction

## Installation

### Option 1: Copy to ESPHome components directory

```bash
cp -r picture_viewer /path/to/esphome/esphome/components/
```

### Option 2: Use as external component

```yaml
external_components:
  - source:
      type: local
      path: /path/to/picture_viewer
    components: [picture_viewer]
```

## Configuration

### Basic Example (Single Directory with Initial File List)

This example shows how file_manager sends the complete file list to picture_viewer after initial scan. The callback fires **once per directory** with all matching files.

```yaml
# Configure storage
storage:
  id: storage_id
  sd_card:
    cs_pin: GPIO10
    spi_id: spi_bus

  # File manager monitors directory and sends file list to picture_viewer
  file_managers:
    - id: photo_monitor
      watch_directory: /sd/photos
      scan_interval: 5s
      patterns:
        - "*.jpg"
        - "*.jpeg"

      # Initial scan callback - fires ONCE per directory with complete file list
      on_directory_changed:
        - lambda: |-
            // Send complete file list to picture_viewer (once per directory)
            id(viewer)->update_directory_files(x.path, x.files);

# Picture viewer displays images from monitored directory
picture_viewer:
  id: viewer
  canvas_id: photo_canvas
  display_id: main_display

  # Directory configuration with JPEG decoder settings
  directories:
    - path: /sd/photos
      jpeg_rgb_order: RGB      # RGB or BGR
      jpeg_color_space: BT601  # BT601 or BT709
      jpeg_output_format: RGB565  # RGB565, RGB888, or GRAY

  # Slideshow and display settings
  slideshow_interval: 5s
  default_image_index: 0  # Show first image on startup (or "off" to disable)
  fit_mode: SCALE_TO_FIT  # SCALE_TO_FIT, SCALE_TO_FILL, STRETCH, or CENTER

  # Thumbnail configuration (optional)
  enable_thumbnails: true
  thumbnail_width: 120
  thumbnail_height: 90
  thumbnail_max_count: 10
  thumbnail_layout: HORIZONTAL  # HORIZONTAL, VERTICAL, or GRID
  thumbnail_container_id: thumb_list  # LVGL container widget
```

### Dynamic File Change Detection

This example shows how to handle real-time file changes (add/remove files while running). The callback triggers on any directory change and sends the updated file list.

```yaml
storage:
  id: storage_id
  sd_card:
    cs_pin: GPIO10
    spi_id: spi_bus

  # File manager with change detection
  file_managers:
    - id: photo_monitor
      watch_directory: /sd/photos
      scan_interval: 2s  # Scan more frequently for quicker updates
      patterns: ["*.jpg", "*.jpeg"]

      # Fires on directory changes (files added, modified, or deleted)
      on_directory_changed:
        - lambda: |-
            // Log changes for debugging
            ESP_LOGI("FileManager", "Directory changed: %zu files total", x.files.size());
            if (!x.added.empty()) {
              ESP_LOGI("FileManager", "  Added: %zu files", x.added.size());
            }
            if (!x.deleted.empty()) {
              ESP_LOGI("FileManager", "  Deleted: %zu files", x.deleted.size());
            }

            // Send updated complete file list to picture_viewer
            id(viewer)->update_directory_files(x.path, x.files);

picture_viewer:
  id: viewer
  canvas_id: photo_canvas

  directories:
    - path: /sd/photos
      jpeg_rgb_order: RGB
      jpeg_color_space: BT601
      jpeg_output_format: RGB565

  slideshow_interval: 5s
  default_image_index: 0
```

### Multiple Directories Example

This example shows monitoring multiple directories with per-directory JPEG decoder settings.

```yaml
storage:
  id: storage_id
  sd_card:
    cs_pin: GPIO10
    spi_id: spi_bus

  # Monitor multiple directories
  file_managers:
    - id: photo_monitor
      watch_directory: /sd/photos
      scan_interval: 5s
      patterns: ["*.jpg", "*.jpeg"]
      on_directory_changed:
        - lambda: |-
            id(viewer)->update_directory_files(x.path, x.files);

    - id: vacation_monitor
      watch_directory: /sd/vacation
      scan_interval: 5s
      patterns: ["*.jpg"]
      on_directory_changed:
        - lambda: |-
            id(viewer)->update_directory_files(x.path, x.files);

    - id: family_monitor
      watch_directory: /sd/family
      scan_interval: 5s
      patterns: ["*.jpg", "*.jpeg"]
      on_directory_changed:
        - lambda: |-
            id(viewer)->update_directory_files(x.path, x.files);

# Picture viewer with per-directory JPEG settings
picture_viewer:
  id: viewer
  canvas_id: photo_canvas

  # Each directory can have different JPEG decoder settings
  directories:
    - path: /sd/photos
      jpeg_rgb_order: RGB
      jpeg_color_space: BT601
      jpeg_output_format: RGB565

    - path: /sd/vacation
      jpeg_rgb_order: BGR
      jpeg_color_space: BT709
      jpeg_output_format: RGB888

    - path: /sd/family
      jpeg_rgb_order: RGB
      jpeg_color_space: BT601
      jpeg_output_format: RGB565

  slideshow_interval: 5s

# Switch between directories at runtime
button:
  - platform: gpio
    pin: GPIO5
    on_press:
      - lambda: |-
          // Cycle through directories
          size_t current = id(viewer)->get_current_directory_index();
          size_t count = id(viewer)->get_directory_count();
          id(viewer)->set_current_directory((current + 1) % count);
```

### Advanced LVGL Integration with Thumbnails

```yaml
# Picture viewer with custom LVGL styles and thumbnail labels
picture_viewer:
  id: viewer
  file_manager_id: photo_monitor
  canvas_id: photo_canvas
  thumbnail_container_id: thumb_list

  directories:
    - path: /sd/photos

  # Custom LVGL styles (optional)
  thumbnail_style_id: my_thumb_style
  thumbnail_active_style_id: my_active_thumb_style
  thumbnail_container_style_id: my_container_style

  # Thumbnail labels (optional)
  thumbnail_label_pattern: "{index}"  # Options: {index}, {filename}, {name}
  thumbnail_label_style_id: my_label_style

  # Overlay icons for slideshow feedback
  overlay_icon_size: 128
  overlay_icon_color: 0xFFFFFF
  overlay_duration: 1500

# Define custom LVGL styles
lvgl:
  styles:
    - id: my_thumb_style
      pad_all: 0
      border_width: 0
      radius: 0

    - id: my_active_thumb_style
      pad_all: 0
      border_width: 2
      border_color: 0x00FF00
      radius: 0

    - id: my_label_style
      text_color: 0xFFFFFF
      text_font: montserrat_12
```

### Custom Thumbnail Click Actions

By default, clicking a thumbnail shows that image in the main canvas. You can override this behavior with custom automation.

**IMPORTANT:** When you add `on_thumbnail_click` automation, the default behavior (showing the image) is **disabled**. You must explicitly call `show_image()` if you want that functionality.

```yaml
picture_viewer:
  id: viewer
  canvas_id: photo_canvas

  directories:
    - path: /sd/photos

  # Custom thumbnail click behavior
  on_thumbnail_click:
    - logger.log:
        format: "Thumbnail clicked: image index %d"
        args: ['x']

    # You MUST call show_image() if you want the default behavior
    - lambda: |-
        id(viewer)->show_image(x);

    # Example: Also trigger other actions
    - light.turn_on: status_led

# Alternative: Log only, don't show image
picture_viewer:
  id: viewer2
  canvas_id: photo_canvas2

  directories:
    - path: /sd/photos

  on_thumbnail_click:
    # Image NOT shown - full custom control
    - logger.log:
        format: "Just logging, not showing image %d"
        args: ['x']
```

### Complete Example

See `examples/picture_viewer_example.yaml` for a full LVGL UI setup with:
- Thumbnail panel (right side)
- Main image canvas (left side)
- Control buttons (play, pause, next, previous, fullscreen)
- Settings page for slideshow configuration
- Home Assistant integration

## API Reference

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `file_manager_id` | ID | Required | FileManager to monitor for image changes |
| `canvas_id` | ID | Required | LVGL canvas for displaying images |
| `display_id` | ID | Required | Display component reference |
| `watch_directory` | string | Required | Directory containing photos |
| `slideshow_interval` | time | 5s | Time between slideshow images |
| `enable_thumbnails` | bool | true | Enable thumbnail generation (P4 only) |
| `thumbnail_width` | int | 120 | Thumbnail width in pixels |
| `thumbnail_height` | int | 90 | Thumbnail height in pixels |

### Methods (available in lambdas)

```cpp
// Show specific image
id(viewer)->show_image(5);              // By index
id(viewer)->show_image("/sd/photo.jpg"); // By path

// Navigation
id(viewer)->next_image();
id(viewer)->previous_image();

// Slideshow control
id(viewer)->start_slideshow();
id(viewer)->stop_slideshow();
id(viewer)->pause_slideshow();
id(viewer)->toggle_slideshow();

// Runtime configuration
id(viewer)->set_slideshow_interval_runtime(10000);  // 10 seconds
id(viewer)->set_fullscreen(true);

// Status
size_t count = id(viewer)->get_image_count();
int index = id(viewer)->get_current_index();
auto mode = id(viewer)->get_slideshow_mode();
bool fs = id(viewer)->is_fullscreen();

// Refresh image list
id(viewer)->refresh_images();
```

## Implementation Details

### PSRAM Allocation

Images are automatically allocated in PSRAM when available for better performance:

```cpp
uint8_t *PictureViewer::allocate_image_buffer_(size_t size) {
#ifdef USE_ESP_IDF
  #if CONFIG_SPIRAM
    // Allocate in PSRAM (external RAM)
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (buffer != nullptr) {
      ESP_LOGD(TAG, "Allocated %zu bytes in PSRAM", size);
      return buffer;
    }
    ESP_LOGW(TAG, "PSRAM allocation failed, falling back to internal RAM");
  #endif
#endif
  // Fallback to regular heap
  uint8_t *buffer = (uint8_t *)malloc(size);
  ESP_LOGD(TAG, "Allocated %zu bytes in heap", size);
  return buffer;
}
```

### Pre-loading Next Image

During slideshow, the next image is pre-loaded in the background:

```cpp
void PictureViewer::preload_next_image_() {
  int next_index = (this->current_index_ + 1) % this->images_.size();

  if (next_index == this->next_image_index_) {
    return;  // Already pre-loaded
  }

  // Free previous pre-loaded image
  if (this->next_image_data_ != nullptr) {
    this->free_image_buffer_(this->next_image_data_);
  }

  // Load next image into PSRAM
  const auto &next_image = this->images_[next_index];
  std::vector<uint8_t> jpeg_data;
  if (!this->read_file_(next_image.path, jpeg_data)) {
    return;
  }

  // Decode to temporary buffer
  std::vector<uint8_t> rgb565_temp;
  int width, height;
  if (!this->decode_jpeg_xxx_(jpeg_data, rgb565_temp, width, height,
                               this->canvas_width_, this->canvas_height_)) {
    return;
  }

  // Allocate in PSRAM and copy
  size_t size = rgb565_temp.size();
  this->next_image_data_ = this->allocate_image_buffer_(size);
  if (this->next_image_data_ != nullptr) {
    memcpy(this->next_image_data_, rgb565_temp.data(), size);
    this->next_image_width_ = width;
    this->next_image_height_ = height;
    this->next_image_size_ = size;
    this->next_image_index_ = next_index;
  }
}
```

### FileManager Integration

The picture viewer registers a callback with FileManager:

```cpp
void PictureViewer::setup() {
  // Register callback with FileManager
  if (this->file_manager_ != nullptr) {
    this->file_manager_->add_on_directory_changed_callback(
      [this](const storage::DirectoryChangeInfo &info) {
        this->on_directory_changed_(info);
      }
    );
  }
}

void PictureViewer::on_directory_changed_(const storage::DirectoryChangeInfo &info) {
  ESP_LOGI(TAG, "Directory changed: %zu files", info.file_count);

  // Refresh image list
  this->refresh_images();

  // If current image was deleted, show next
  if (this->current_index_ >= static_cast<int>(this->images_.size())) {
    if (!this->images_.empty()) {
      this->show_image(0);
    } else {
      this->current_index_ = -1;
    }
  }
}
```

### JPEG Decoder Selection

The component automatically selects the best decoder:

```cpp
bool PictureViewer::load_jpeg_(...) {
#ifdef USE_HARDWARE_JPEG_DECODER
  // ESP32-P4: Hardware decoder
  return this->decode_jpeg_hardware_(...);
#elif defined(USE_ESP_JPEG_DECODER)
  // ESP32-S2/S3: esp_jpeg decoder
  return this->decode_jpeg_esp_(...);
#elif defined(USE_JPEGDEC)
  // Other platforms: JPEGDec library
  return this->decode_jpeg_jpegdec_(...);
#else
  #error "No JPEG decoder available"
#endif
}
```

## Platform-Specific Optimizations

### ESP32-S2/S3 (esp_jpeg)

```cpp
bool PictureViewer::decode_jpeg_esp_(...) {
  esp_jpeg_image_cfg_t jpeg_cfg = {
    .indata = jpeg_data.data(),
    .indata_size = jpeg_data.size(),
    .outbuf = (uint8_t *)output_buffer,
    .outbuf_size = output_size,
    .out_format = JPEG_IMAGE_FORMAT_RGB565,
    .out_scale = JPEG_IMAGE_SCALE_0,  // Or calculate scale factor
    .flags = {
      .swap_color_bytes = 0,
    }
  };

  esp_jpeg_image_output_t outimg;
  esp_err_t ret = esp_jpeg_decode(&jpeg_cfg, &outimg);

  if (ret == ESP_OK) {
    width = outimg.width;
    height = outimg.height;
    return true;
  }
  return false;
}
```

### ESP32-P4 (Hardware Decoder)

```cpp
bool PictureViewer::decode_jpeg_hardware_(...) {
  jpeg_decode_cfg_t decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
  };

  jpeg_decode_picture_info_t picture_info;
  ESP_ERROR_CHECK(jpeg_decoder_get_info(jpeg_data.data(), jpeg_data.size(), &picture_info));

  // Allocate output in PSRAM
  size_t output_size = picture_info.width * picture_info.height * 2;  // RGB565
  uint8_t *output = this->allocate_image_buffer_(output_size);

  ESP_ERROR_CHECK(jpeg_decoder_process(this->hw_decoder_, &decode_cfg,
                                        jpeg_data.data(), jpeg_data.size(),
                                        output, output_size, &out_size));

  width = picture_info.width;
  height = picture_info.height;
  return true;
}
```

## Home Assistant Integration

The example YAML includes Home Assistant services:

```yaml
# In Home Assistant
service: esphome.picture_viewer_show_image
data:
  index: 5

service: esphome.picture_viewer_start_slideshow

service: esphome.picture_viewer_set_interval
data:
  seconds: 10
```

## Performance Tips

1. **Use PSRAM** - Essential for smooth operation with large images
2. **Enable esp_jpeg** - Much faster than JPEGDec on S2/S3
3. **Pre-size images** - Resize photos to display resolution for faster loading
4. **Limit resolution** - 800x600 is usually sufficient for embedded displays
5. **Use SPI mode 6** - Fastest SPI clock for SD cards

## Troubleshooting

### Images not loading

- Check file patterns in FileManager configuration
- Verify SD card is mounted (check logs)
- Ensure JPEG files are valid (not corrupted)
- Check PSRAM is enabled in platformio_options

### Slideshow stuttering

- Enable PSRAM for pre-loading
- Reduce image file sizes
- Increase slideshow interval
- Check SD card speed (use fast cards)

### Out of memory errors

- Enable PSRAM
- Reduce canvas size
- Disable thumbnails
- Use lower resolution images

## License

Part of ESPHome - https://esphome.io
