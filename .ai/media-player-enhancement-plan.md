# Media Player Enhancement Plan: File Stream Playback from Local Storage

## Executive Summary

This plan extends ESPHome's existing `media_player` component to support audio/video playback from local storage devices (USB, SD) with efficient memory management for resource-constrained MCUs. The current `video_player` component was misguided - `media_player` is the correct architectural home for media playback.

## 1. Current State Analysis

### 1.1 Existing Media Player Component

**Location:** `esphome/components/media_player/`

**Current Capabilities:**
- **Base Class:** Abstract `MediaPlayer` with `control()` method for playback commands
- **State Management:** States (IDLE, PLAYING, PAUSED, ANNOUNCING, ON, OFF)
- **Commands:** PLAY, PAUSE, STOP, MUTE, UNMUTE, TOGGLE, VOLUME_UP/DOWN, TURN_ON/OFF
- **URL Playback:** `play_media(media_url)` action supports HTTP URLs
- **Format Negotiation:** `MediaPlayerSupportedFormat` struct (format, sample_rate, num_channels, purpose, sample_bytes)
- **Feature Flags:** Entity features (PAUSE, SEEK, VOLUME_SET, PLAY_MEDIA, etc.)

**Limitations:**
- ✗ No support for local file paths (only HTTP URLs)
- ✗ No integration with storage component
- ✗ No streaming from file descriptors
- ✗ No memory-efficient buffering for large files

### 1.2 USB Audio PR (#11728) Analysis

**Location:** `/tmp/esphome-pr/esphome/components/usb_audio/`

**Components:**
1. **USBAudioComponent** - Main USB audio host driver
   - Uses ESP-IDF component `espressif/usb_host_uac` v1.3.1
   - Supports ESP32-S2, ESP32-S3, ESP32-P4
   - Buffer sizes: 6400 bytes default (microphone + speaker)

2. **USBAudioSpeaker** - Speaker implementation
   - Extends `speaker::Speaker` base class
   - Configurable: sample_rate (48000), bits_per_sample (16), channels (2)
   - **Supports stereo output** ✓
   - Write timeout: 20ms default
   - Chunk-based adaptive writing

3. **USBAudioMediaPlayer** - Media player bridge
   - Extends `speaker::SpeakerMediaPlayer`
   - Monitors USB connection state
   - Handles disconnect gracefully

4. **SpeakerMediaPlayer** - Generic speaker-based media player
   - **Location:** `esphome/components/speaker/media_player/speaker_media_player.h`
   - **Architecture:** Dual-pipeline (announcement + media)
   - **Key Features:**
     - AudioPipeline abstraction (reader → decoder → speaker)
     - FreeRTOS task-based processing
     - Ring buffer between reader and decoder (24KB default)
     - Playlist support with repeat and enqueue
     - Volume persistence to flash
     - Mute state management

5. **AudioPipeline** - Stream processing pipeline
   - **Location:** `esphome/components/speaker/media_player/audio_pipeline.h`
   - **Components:**
     - `AudioReader`: Reads from HTTP URL or `AudioFile*` struct
     - `AudioDecoder`: Decodes MP3/FLAC/WAV/AAC
     - Ring buffer: Connects reader → decoder (configurable size)
     - Speaker output: Direct to `speaker::Speaker`
   - **FreeRTOS Tasks:**
     - `read_task`: Reads file/URL data
     - `decode_task`: Decodes audio
     - Event groups + queues for coordination
   - **States:** STARTING_FILE, STARTING_URL, PLAYING, STOPPING, STOPPED, PAUSED, ERROR_READING, ERROR_DECODING

**Key Finding:** The USB audio PR already provides a **complete media player implementation** with speaker integration. It uses `AudioPipeline` which reads from `AudioFile*` (flash) or HTTP URLs.

**Gap:** `AudioFile` struct only supports in-memory data (`const uint8_t *data, size_t length`). No support for file descriptors or streaming from local storage.

### 1.3 Current Video Player Implementation

**Location:** `esphome/components/video_player/`

**Architecture:**
- Separate component, NOT extending media_player
- Supports MP4/MKV containers with H264 video + FLAC/MP3/AAC audio
- PSRam-based buffering for frames
- Integration with LVGL canvas for video rendering
- Integration with `speaker::Speaker` for audio output
- File access: Direct POSIX file I/O (no storage component integration)

**Key Features to Preserve:**
1. **Triple Buffering Architecture:**
   ```cpp
   std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> h264_frame_buffer_;    // Compressed H264
   std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> annexb_frame_buffer_;  // Annex-B converted
   std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> yuv_frame_buffer_;     // Decoded YUV420
   uint16_t *rgb_frame_buffer_;  // RGB565 for LVGL
   ```

2. **Audio Decoder Integration:**
   ```cpp
   std::unique_ptr<audio::AudioDecoder> audio_decoder_;
   std::shared_ptr<RingBuffer> audio_input_ring_buffer_;  // Raw audio data
   std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> audio_sample_buffer_;
   ```

3. **A/V Synchronization:**
   - PTS timestamp tracking
   - `audio_start_time_ms_` and `video_start_time_ms_` for sync

4. **Container Demuxing:**
   - MP4 demuxer (AVCC format)
   - MKV demuxer (Annex-B format)
   - Sample-by-sample reading API

**Issues:**
- Video player is a separate component (architectural mistake)
- No media_player integration
- Direct file I/O instead of storage component
- Cannot leverage existing `SpeakerMediaPlayer` infrastructure

### 1.4 Storage Component

**Location:** `esphome/components/storage/`

**Key APIs:**
```cpp
class Storage : public Component {
  // File operations
  bool file_exists(const std::string &path);
  std::string read_file(const std::string &path);  // Not suitable for large files!
  bool write_file(const std::string &path, const std::string &data);
  std::vector<std::string> list_files(const std::string &path);

  // Mount management
  void register_mount(const std::string &path, const std::string &platform, MountSpaceProvider *space_provider);
  std::string find_mount_for_path(const std::string &path);

  // StorageDevice registry
  void register_device(StorageDevice *device);
  std::vector<StorageInfo> get_available_storages();
  StorageDevice *get_device_by_mount_path(const std::string &mount_path);
};

class StorageDevice {
  StorageInfo get_info();
  bool is_available();
  std::string get_mount_path();
  // ... but no file stream API!
};
```

**Gap:** Storage component provides device registry and basic file ops, but no **streaming file read** API suitable for large media files.

### 1.5 HTTP File Browser Component

**Location:** `esphome/components/http_file_browser/`

**Relevant Patterns:**
```cpp
// Buffer pool allocation (PSRAM or heap)
#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
  this->buffer_pool_ = (uint8_t *) heap_caps_malloc(
    this->buffer_pool_size_,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
#else
  this->buffer_pool_ = (uint8_t *) malloc(this->buffer_pool_size_);
#endif

// File operations use storage component
if (this->storage_ != nullptr) {
  storage_device = this->storage_->get_device_by_mount_path(mount_point);
}
```

**Key Pattern:** Use `storage::global_storage` for device access, then use POSIX file I/O on mount paths.

### 1.6 Audio Component

**Location:** `esphome/components/audio/`

**Key Classes:**

1. **AudioDecoder** (`audio_decoder.h`)
   - Input: `RingBuffer` with raw file data
   - Output: `RingBuffer` or `speaker::Speaker*`
   - Supports: WAV, FLAC, MP3, AAC
   - Buffer sizes: Constructor takes `input_buffer_size`, `output_buffer_size`

2. **AudioReader** (`audio_reader.h`)
   - Reads from `AudioFile*` (flash) or HTTP URL
   - Output: `RingBuffer`
   - **File reading pattern:**
     ```cpp
     esp_err_t start(AudioFile *audio_file, AudioFileType &file_type);
     AudioReaderState read();  // Streams data to ring buffer
     ```
   - HTTP uses `esp_http_client`

3. **AudioFile** struct (`audio.h`)
   ```cpp
   struct AudioFile {
     const uint8_t *data;  // In-memory pointer only!
     size_t length;
     AudioFileType file_type;
   };
   ```

**Gap:** `AudioFile` assumes in-memory data. No file descriptor or stream-based reading.

## 2. Goals & Requirements

### 2.1 Primary Goals

1. ✅ **USB Audio Support:** Use PR #11728's USB audio component for stereo speakers on ESP32-P4
2. ✅ **Local Storage Playback:** Play audio/video files from USB/SD storage devices
3. ✅ **Memory Efficiency:** Stream large files without loading into RAM (use PSRAM buffers)
4. ✅ **Unified Architecture:** Use `media_player` component (not separate video_player)
5. ✅ **Format Support:** MP3, FLAC, WAV audio; optional H264 video (MP4/MKV containers)

### 2.2 Technical Requirements

**Memory Management:**
- Heap usage: Minimize (ESP32-P4 has 512KB internal SRAM)
- PSRAM usage: Leverage (ESP32-P4 has 32MB @ 200MHz)
- Streaming: Read files in chunks, never load entire file
- Buffer sizing: Configurable based on platform

**File Access:**
- Use storage component for device enumeration
- Stream from POSIX file descriptors
- Support local paths: `/usb/music.mp3`, `/sd/video.mp4`
- Support network paths if available

**Audio Quality:**
- Stereo output (2 channels)
- Sample rates: 44.1kHz, 48kHz (configurable)
- Bit depth: 16-bit (extendable to 24-bit)
- Low-latency playback

**Integration:**
- Extend existing `media_player` component
- Reuse `SpeakerMediaPlayer` infrastructure
- Integrate with storage component
- Optional LVGL integration for video

### 2.3 Out of Scope (Phase 1)

- Video decoding (H264) - focus on audio first
- Advanced seeking (simple stop/start acceptable)
- Playlist management beyond what `SpeakerMediaPlayer` provides
- Real-time transcoding
- Network streaming (NFS/SMB) - HTTP URLs already supported

## 3. Architecture Design

### 3.1 Component Hierarchy

```
media_player (base)
├── USBAudioMediaPlayer (PR #11728)
│   └── SpeakerMediaPlayer
│       └── AudioPipeline (reader → decoder → speaker)
└── [Future] VideoMediaPlayer
    └── MediaPlayer with video extensions
```

### 3.2 File Stream Architecture

**Problem:** Current `AudioReader` only supports:
1. HTTP URLs (via `esp_http_client`)
2. Flash memory (`AudioFile*` with in-memory pointer)

**Solution:** Add file descriptor streaming to `AudioReader`

#### Option A: Extend AudioFile struct (Minimal Change)
```cpp
// In audio.h
enum class AudioSourceType : uint8_t {
  MEMORY,     // const uint8_t *data
  FILE,       // FILE *file_handle
};

struct AudioFile {
  AudioSourceType source_type{AudioSourceType::MEMORY};
  union {
    struct {
      const uint8_t *data;
      size_t length;
    } memory;
    struct {
      FILE *file_handle;  // Caller manages lifetime
      size_t length;      // Total file size (for progress)
      size_t position;    // Current read position
    } file;
  };
  AudioFileType file_type;
};
```

**Pros:**
- Minimal API changes
- Backward compatible
- Reuses existing `AudioReader::start(AudioFile *, AudioFileType &)`

**Cons:**
- Union increases struct size
- Caller must manage FILE* lifetime
- Less clean API

#### Option B: New file path API (Clean Design)
```cpp
// In audio_reader.h
class AudioReader {
public:
  // Existing methods
  esp_err_t start(const std::string &uri, AudioFileType &file_type);  // HTTP
  esp_err_t start(AudioFile *audio_file, AudioFileType &file_type);   // Flash

  // NEW: File path support
  esp_err_t start_file_path(const std::string &file_path, AudioFileType &file_type);

protected:
  AudioReaderState file_read_();      // Existing: AudioFile* reading
  AudioReaderState file_path_read_(); // NEW: FILE* reading

  FILE *file_handle_{nullptr};  // NEW: For file path reading
  size_t file_size_{0};
  size_t file_position_{0};
};
```

**Pros:**
- Clean API separation
- AudioReader manages FILE* lifecycle
- No struct changes
- Easy to add video container support later

**Cons:**
- New public method (API expansion)
- Slight code duplication in reading logic

**Recommendation:** **Option B** - cleaner, more maintainable, easier video integration later.

### 3.3 Integration with SpeakerMediaPlayer

`SpeakerMediaPlayer` already has dual input mode:
```cpp
void start_url(const std::string &uri);   // HTTP URLs
void start_file(audio::AudioFile *audio_file);  // Flash memory
```

**Add third input mode:**
```cpp
class AudioPipeline {
public:
  // Existing
  void start_url(const std::string &uri);
  void start_file(audio::AudioFile *audio_file);

  // NEW
  void start_file_path(const std::string &file_path);
};
```

### 3.4 Media Player Call Extensions

**Current:**
```yaml
media_player.play_media:
  media_url: "http://example.com/audio.mp3"
  announcement: false
```

**Enhanced:**
```yaml
# HTTP URL (existing)
media_player.play_media:
  media_url: "http://example.com/audio.mp3"

# Local storage file (NEW)
media_player.play_media:
  media_url: "/usb/music.mp3"

# Local storage file with explicit platform (NEW)
media_player.play_media:
  media_url: "file:///usb/music.mp3"
```

**Implementation:**
```cpp
void SpeakerMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (call.get_media_url().has_value()) {
    std::string url = call.get_media_url().value();

    // Check if local file path (starts with /)
    if (url[0] == '/' || url.find("file://") == 0) {
      // Strip file:// prefix if present
      if (url.find("file://") == 0) {
        url = url.substr(7);
      }

      // Determine file type from extension
      audio::AudioFileType file_type = detect_audio_type_from_path_(url);

      // Queue command to start local file
      this->queue_file_path_playback_(url, file_type, is_announcement);
    } else {
      // HTTP URL (existing path)
      this->queue_url_playback_(url, is_announcement);
    }
  }
}
```

### 3.5 Buffer Management Strategy

**Constraints:**
- ESP32-P4: 512KB internal SRAM, 32MB PSRAM @ 200MHz
- File chunks should fit L2 cache where possible
- Balance between memory usage and seek performance

**Recommended Buffer Sizes:**

| Component | Size | Location | Rationale |
|-----------|------|----------|-----------|
| AudioReader chunk | 8-16KB | PSRAM | One read() call chunk |
| Reader→Decoder ring buffer | 24-48KB | PSRAM | ~500ms @ 48kHz stereo 16-bit |
| AudioDecoder input | 24KB | PSRAM | Default in AudioPipeline |
| AudioDecoder output | 24KB | PSRAM | Default in AudioPipeline |
| Speaker internal buffer | 6.4KB | SRAM | USBAudio default |
| **Total** | **~80-110KB** | PSRAM | Acceptable for 32MB PSRAM |

**Configuration:**
```yaml
media_player:
  platform: usb_audio_media_player
  speaker: usb_speaker
  buffer_size: 32768  # Reader→Decoder ring buffer (default: 24KB)
```

### 3.6 File Type Detection

**Method 1: Extension-based (Fast, simple)**
```cpp
audio::AudioFileType detect_audio_type_from_path(const std::string &path) {
  if (path.ends_with(".mp3")) return audio::AudioFileType::MP3;
  if (path.ends_with(".flac")) return audio::AudioFileType::FLAC;
  if (path.ends_with(".wav")) return audio::AudioFileType::WAV;
  if (path.ends_with(".aac") || path.ends_with(".m4a")) return audio::AudioFileType::AAC;
  return audio::AudioFileType::NONE;
}
```

**Method 2: Magic number detection (Robust, slower)**
```cpp
audio::AudioFileType detect_audio_type_from_file(FILE *file) {
  uint8_t header[12];
  fread(header, 1, 12, file);
  rewind(file);

  // MP3: 0xFF 0xFB or ID3
  if ((header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) ||
      (header[0] == 'I' && header[1] == 'D' && header[2] == '3')) {
    return audio::AudioFileType::MP3;
  }

  // FLAC: "fLaC"
  if (header[0] == 'f' && header[1] == 'L' &&
      header[2] == 'a' && header[3] == 'C') {
    return audio::AudioFileType::FLAC;
  }

  // WAV: "RIFF" + "WAVE"
  if (header[0] == 'R' && header[1] == 'I' &&
      header[2] == 'F' && header[3] == 'F' &&
      header[8] == 'W' && header[9] == 'A' &&
      header[10] == 'V' && header[11] == 'E') {
    return audio::AudioFileType::WAV;
  }

  return audio::AudioFileType::NONE;
}
```

**Recommendation:** Use **Method 1** (extension) with **Method 2** (magic) as fallback.

## 4. Implementation Plan

### 4.1 Phase 1: USB Audio Integration (Week 1)

**Goal:** Get USB audio working with existing infrastructure

**Tasks:**
1. ✅ Merge/apply PR #11728 to codebase
2. ✅ Test basic USB speaker functionality
   - Connect USB stereo speaker to ESP32-P4
   - Play test tone from flash memory
   - Verify stereo output
3. ✅ Test USBAudioMediaPlayer with HTTP URLs
   - Stream MP3 from HTTP server
   - Verify playback quality
   - Test volume/mute controls
4. ✅ Document USB audio setup in YAML

**Expected YAML:**
```yaml
usb_audio:
  connect_timeout: 5s
  speaker_buffer_size: 6400

usb_audio.speaker:
  id: usb_speaker
  sample_rate: 48000
  bits_per_sample: 16
  channels: 2

usb_audio.media_player:
  id: media_player_1
  speaker: usb_speaker
```

**Success Criteria:**
- USB speaker plays HTTP-streamed MP3
- Stereo output confirmed
- Volume control works
- No audio glitches

### 4.2 Phase 2: Audio File Streaming (Week 2-3)

**Goal:** Add local file path support to AudioReader

**Tasks:**

#### Task 2.1: Extend AudioReader with file path support
**Files:** `esphome/components/audio/audio_reader.h`, `audio_reader.cpp`

**Changes:**
```cpp
// audio_reader.h
class AudioReader {
public:
  // Add new method
  esp_err_t start_file_path(const std::string &file_path, AudioFileType &file_type);

protected:
  AudioReaderState file_path_read_();

  // New members
  FILE *file_handle_{nullptr};
  size_t file_size_{0};
  size_t file_position_{0};
  std::string current_file_path_;
};

// audio_reader.cpp
esp_err_t AudioReader::start_file_path(const std::string &file_path, AudioFileType &file_type) {
  // Open file
  this->file_handle_ = fopen(file_path.c_str(), "rb");
  if (this->file_handle_ == nullptr) {
    return ESP_ERR_NOT_FOUND;
  }

  // Get file size
  fseek(this->file_handle_, 0, SEEK_END);
  this->file_size_ = ftell(this->file_handle_);
  fseek(this->file_handle_, 0, SEEK_SET);
  this->file_position_ = 0;

  // Detect file type (extension first, magic fallback)
  file_type = detect_audio_type_from_path(file_path);
  if (file_type == AudioFileType::NONE) {
    file_type = detect_audio_type_from_file(this->file_handle_);
  }

  this->audio_file_type_ = file_type;
  this->current_file_path_ = file_path;
  return ESP_OK;
}

AudioReaderState AudioReader::file_path_read_() {
  if (this->file_handle_ == nullptr) {
    return AudioReaderState::FAILED;
  }

  // Get available space in ring buffer
  size_t available = this->output_transfer_buffer_->get_available();
  if (available == 0) {
    return AudioReaderState::READING;  // Buffer full, try again later
  }

  // Read chunk (min of available space and buffer_size_)
  size_t to_read = std::min(available, this->buffer_size_);
  uint8_t *buffer = new uint8_t[to_read];

  size_t bytes_read = fread(buffer, 1, to_read, this->file_handle_);
  if (bytes_read > 0) {
    this->output_transfer_buffer_->write(buffer, bytes_read);
    this->file_position_ += bytes_read;
  }

  delete[] buffer;

  // Check if done
  if (bytes_read == 0 || this->file_position_ >= this->file_size_) {
    fclose(this->file_handle_);
    this->file_handle_ = nullptr;
    return AudioReaderState::FINISHED;
  }

  return AudioReaderState::READING;
}

// Update AudioReader::read() to dispatch to file_path_read_()
AudioReaderState AudioReader::read() {
  if (this->file_handle_ != nullptr) {
    return this->file_path_read_();
  } else if (this->current_audio_file_ != nullptr) {
    return this->file_read_();
  } else if (this->client_ != nullptr) {
    return this->http_read_();
  }
  return AudioReaderState::FAILED;
}
```

#### Task 2.2: Extend AudioPipeline with file path support
**Files:** `esphome/components/speaker/media_player/audio_pipeline.h`, `audio_pipeline.cpp`

**Changes:**
```cpp
// audio_pipeline.h
class AudioPipeline {
public:
  void start_file_path(const std::string &file_path);

protected:
  bool pending_file_path_{false};
  std::string pending_file_path_value_;
};

// audio_pipeline.cpp
void AudioPipeline::start_file_path(const std::string &file_path) {
  if (this->is_playing_) {
    // Queue for later
    this->pending_file_path_ = true;
    this->pending_file_path_value_ = file_path;
    this->stop();
  } else {
    this->current_file_path_ = file_path;
    this->start_tasks_();
  }
}

// Update read_task to handle file paths
void AudioPipeline::read_task(void *params) {
  // ...
  if (!pipeline->current_file_path_.empty()) {
    esp_err_t err = reader->start_file_path(
      pipeline->current_file_path_,
      pipeline->current_audio_file_type_
    );
    // ... error handling
  }
  // ... existing URL and AudioFile handling
}
```

#### Task 2.3: Update SpeakerMediaPlayer to route local paths
**Files:** `esphome/components/speaker/media_player/speaker_media_player.h`, `speaker_media_player.cpp`

**Changes:**
```cpp
void SpeakerMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (call.get_media_url().has_value()) {
    std::string url = call.get_media_url().value();
    bool is_announcement = call.get_announcement().value_or(false);

    // Detect local file path
    bool is_local_file = (url[0] == '/') || (url.find("file://") == 0);

    if (is_local_file) {
      // Strip file:// prefix
      if (url.find("file://") == 0) {
        url = url.substr(7);
      }

      // Add to playlist
      PlaylistItem item;
      item.file_path = url;

      if (is_announcement) {
        this->announcement_playlist_.push_back(item);
      } else {
        this->media_playlist_.push_back(item);
      }

      // Start playback if idle
      if (this->state == MEDIA_PLAYER_STATE_IDLE) {
        this->process_next_playlist_item_();
      }
    } else {
      // HTTP URL (existing code path)
      // ...
    }
  }
}
```

#### Task 2.4: Add file type detection utilities
**Files:** `esphome/components/audio/audio.h`, `audio.cpp`

**Changes:**
```cpp
// audio.h
namespace audio {
  AudioFileType detect_audio_type_from_path(const std::string &path);
  AudioFileType detect_audio_type_from_file(FILE *file);
}
```

#### Task 2.5: Testing
**Test Cases:**
1. Play MP3 from USB storage: `/usb/test.mp3`
2. Play FLAC from SD card: `/sd/music.flac`
3. Play WAV from internal flash: `/data/sound.wav`
4. Handle missing files gracefully
5. Handle unsupported formats gracefully
6. Verify memory usage stays under 150KB PSRAM
7. Test seeking (stop → play at different position)

**Success Criteria:**
- All test cases pass
- No heap fragmentation
- Playback quality equal to HTTP streaming
- File handle cleanup on errors

### 4.3 Phase 3: Storage Component Integration (Week 4)

**Goal:** Properly integrate with storage component for device awareness

**Tasks:**

#### Task 3.1: Add storage awareness to SpeakerMediaPlayer
**Files:** `esphome/components/speaker/media_player/speaker_media_player.h`, `speaker_media_player.cpp`

**Changes:**
```cpp
// speaker_media_player.h
#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

class SpeakerMediaPlayer {
protected:
#ifdef USE_STORAGE
  storage::Storage *storage_{nullptr};
#endif

  bool validate_file_path_(const std::string &path);
  std::string resolve_file_path_(const std::string &path);
};

// speaker_media_player.cpp
void SpeakerMediaPlayer::setup() {
#ifdef USE_STORAGE
  this->storage_ = storage::global_storage;
#endif
}

bool SpeakerMediaPlayer::validate_file_path_(const std::string &path) {
#ifdef USE_STORAGE
  if (this->storage_ == nullptr) {
    ESP_LOGW(TAG, "Storage component not available");
    return false;
  }

  // Get device for mount point
  std::string mount = this->storage_->find_mount_for_path(path);
  if (mount.empty()) {
    ESP_LOGW(TAG, "No mount point for path: %s", path.c_str());
    return false;
  }

  auto *device = this->storage_->get_device_by_mount_path(mount);
  if (device == nullptr || !device->is_available()) {
    ESP_LOGW(TAG, "Storage device not available for: %s", path.c_str());
    return false;
  }
#endif

  // Check file exists (POSIX)
  struct stat st;
  return (stat(path.c_str(), &st) == 0);
}
```

#### Task 3.2: Add storage availability monitoring
**Purpose:** Detect USB disconnect, SD card removal during playback

**Changes:**
```cpp
void SpeakerMediaPlayer::setup() {
#ifdef USE_STORAGE
  if (storage::global_storage != nullptr) {
    storage::global_storage->add_on_device_removed_callback(
      [this](storage::StorageDevice *device) {
        // Check if currently playing from this device
        if (!this->current_file_path_.empty()) {
          std::string mount = device->get_mount_path();
          if (this->current_file_path_.find(mount) == 0) {
            ESP_LOGW(TAG, "Storage device removed during playback");
            this->stop_due_to_storage_removal_();
          }
        }
      }
    );
  }
#endif
}
```

#### Task 3.3: Update configuration schema
**Files:** `esphome/components/speaker/media_player/__init__.py`

**Changes:**
```python
# Add optional storage dependency
DEPENDENCIES = []  # ESP-IDF only

def to_code(config):
    # Auto-detect storage component
    if "storage" in CORE.loaded_integrations:
        cg.add_define("USE_STORAGE")
```

#### Task 3.4: Testing
**Test Cases:**
1. Play file from USB, disconnect during playback → graceful stop
2. Play file from SD, remove card during playback → graceful stop
3. Attempt to play from unmounted device → error logged
4. Device reconnects → able to resume playback

### 4.4 Phase 4: Configuration & Documentation (Week 5)

**Goal:** User-friendly configuration and comprehensive docs

#### Task 4.1: Create example configurations

**Example 1: USB Audio with Local Files**
```yaml
# USB storage for media files
usb_storage:
  id: usb_drive

# USB audio output device
usb_audio:
  connect_timeout: 5s

usb_audio.speaker:
  id: usb_speaker
  sample_rate: 48000
  channels: 2

usb_audio.media_player:
  id: media_player_1
  speaker: usb_speaker
  buffer_size: 32768
  volume_initial: 0.7
  volume_increment: 0.05

# Automations
button:
  - platform: template
    name: "Play USB Music"
    on_press:
      - media_player.play_media:
          id: media_player_1
          media_url: "/usb/music.mp3"

  - platform: template
    name: "Play Announcement"
    on_press:
      - media_player.play_media:
          id: media_player_1
          media_url: "/usb/doorbell.wav"
          announcement: true
```

**Example 2: I2S Audio with SD Card**
```yaml
sd_storage:
  id: sd_card
  miso_pin: GPIO2
  mosi_pin: GPIO15
  clk_pin: GPIO14
  cs_pin: GPIO13

i2s_audio:
  - id: i2s_out
    i2s_lrclk_pin: GPIO26
    i2s_bclk_pin: GPIO27
    i2s_dout_pin: GPIO25

speaker:
  - platform: i2s_audio
    id: i2s_speaker
    i2s_audio_id: i2s_out
    mode: stereo
    sample_rate: 48000

speaker.media_player:
  id: media_player_1
  media_speaker: i2s_speaker

button:
  - platform: template
    name: "Play SD Music"
    on_press:
      - media_player.play_media:
          id: media_player_1
          media_url: "/sd/playlist/song.flac"
```

#### Task 4.2: Update component documentation

**Documentation Topics:**
1. **USB Audio Setup**
   - Hardware requirements
   - Driver installation (none needed, ESP-IDF native)
   - Stereo vs mono configuration

2. **File Playback**
   - Supported formats (MP3, FLAC, WAV, AAC)
   - File path conventions (`/usb/`, `/sd/`, `/data/`)
   - Storage component integration

3. **Memory Requirements**
   - Buffer sizing guidelines
   - PSRAM usage expectations
   - Platform-specific considerations

4. **Automation Examples**
   - Playlists
   - Announcements
   - Volume control
   - Seek/skip functionality

#### Task 4.3: Add component tests

**Test YAML:** `tests/components/speaker_media_player/test.esp32-p4.yaml`
```yaml
usb_audio:
  connect_timeout: 5s

usb_audio.speaker:
  id: usb_speaker

usb_audio.media_player:
  id: media_player_1
  speaker: usb_speaker
```

## 5. Video Support (Future Phase)

**Note:** Audio-only playback is Phase 1 priority. Video can be added later.

### 5.1 Architecture for Video

**Extend MediaPlayer with video traits:**
```cpp
// media_player.h
class MediaPlayerTraits {
  void set_supports_video(bool supports_video);
  bool get_supports_video() const;

  void set_video_formats(std::vector<VideoFormat> formats);
};

struct VideoFormat {
  std::string codec;  // "h264", "h265"
  uint16_t max_width;
  uint16_t max_height;
  uint8_t max_fps;
};
```

**Video rendering integration:**
```cpp
class MediaPlayer {
  void set_video_renderer(VideoRenderer *renderer);

protected:
  VideoRenderer *video_renderer_{nullptr};
};

class VideoRenderer {
public:
  virtual void render_frame(const uint8_t *yuv_data, uint16_t width, uint16_t height) = 0;
};

// LVGL implementation
class LVGLVideoRenderer : public VideoRenderer {
  void render_frame(const uint8_t *yuv_data, uint16_t width, uint16_t height) override;
  void set_canvas(lv_obj_t *canvas);
};
```

### 5.2 Video Pipeline

**Extend AudioPipeline to VideoPipeline:**
```
Reader → Demuxer → [Video Decoder → Renderer]
                   [Audio Decoder → Speaker]
                   Sync Controller
```

**Components:**
- **Container Demuxer:** MP4/MKV parsing (reuse from video_player)
- **Video Decoder:** H264 hardware decoder (ESP32-P4)
- **Audio Decoder:** Existing AudioDecoder
- **Sync Controller:** PTS-based A/V sync
- **Renderer:** LVGL canvas or DMA2D

### 5.3 Video Configuration
```yaml
media_player:
  platform: video_media_player
  speaker: usb_speaker
  video_renderer: lvgl_canvas_1
  supported_codecs:
    video: [h264]
    audio: [mp3, flac, aac]
  buffer_size: 65536  # Larger for video
```

## 6. Memory Budget Analysis

### 6.1 ESP32-P4 Memory Layout

**Internal SRAM:** 512KB (fast, limited)
**PSRAM:** 32MB @ 200MHz (abundant, slightly slower)

### 6.2 Media Player Memory Usage

| Component | Size | Location | Notes |
|-----------|------|----------|-------|
| **FreeRTOS Tasks** | | | |
| Read task stack | 4-8KB | SRAM/PSRAM | Configurable |
| Decode task stack | 4-8KB | SRAM/PSRAM | Configurable |
| **Ring Buffers** | | | |
| Reader→Decoder | 24-48KB | PSRAM | Configurable via `buffer_size` |
| **Transfer Buffers** | | | |
| AudioReader input | 24KB | PSRAM | `DEFAULT_TRANSFER_BUFFER_SIZE` |
| AudioDecoder output | 24KB | PSRAM | `DEFAULT_TRANSFER_BUFFER_SIZE` |
| **Decoder State** | | | |
| FLAC decoder | ~50KB | PSRAM | Codec-dependent |
| MP3 decoder | ~30KB | PSRAM | Codec-dependent |
| **Speaker Buffer** | | | |
| USB audio buffer | 6.4KB | SRAM | USB host requirement |
| I2S DMA buffer | 4-8KB | SRAM | Platform-dependent |
| **Total (Audio)** | **~150-200KB** | Mostly PSRAM | Acceptable |

### 6.3 Video Extension Budget (Future)

| Component | Size | Location | Notes |
|-----------|------|----------|-------|
| H264 frame (compressed) | 10-50KB | PSRAM | Depends on resolution/quality |
| YUV420 frame buffer | 153KB | PSRAM | 480x272 resolution |
| RGB565 frame buffer | 255KB | PSRAM | For LVGL canvas |
| **Total (Video)** | **~420-500KB** | PSRAM | Still acceptable for 32MB |

## 7. Risk Assessment & Mitigation

### 7.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| **File I/O bottleneck** | Medium | High | Buffer tuning, PSRAM optimization |
| **Memory fragmentation** | Low | High | Fixed-size buffers, no dynamic alloc in loop |
| **USB disconnect during playback** | High | Medium | Graceful error handling, state machine |
| **Incompatible audio formats** | Medium | Low | Clear error messages, format detection |
| **PSRAM bandwidth limits** | Low | Medium | Profiling, buffer size optimization |
| **Breaking existing media_player API** | Low | High | Careful extension, no breaking changes |

### 7.2 Mitigation Strategies

**File I/O Optimization:**
- Read ahead buffering (24-48KB ring buffer)
- Aligned reads for flash/SD performance
- Async file operations (FreeRTOS task-based)

**Memory Management:**
- Allocate all buffers in setup(), never in loop()
- Use PSRAM for large buffers (>4KB)
- Monitor `heap_caps_get_free_size()` during testing

**Error Handling:**
- Detect missing files before starting playback
- Handle mid-playback storage removal gracefully
- Provide user-friendly error states

**API Compatibility:**
- All changes are additive (new methods, no signature changes)
- Existing HTTP URL playback unaffected
- Flash memory playback (AudioFile*) unaffected

## 8. Testing Strategy

### 8.1 Unit Tests

**AudioReader:**
- File path opening/closing
- Chunk-based reading
- File type detection
- Error handling (missing file, permissions, corrupt file)

**AudioPipeline:**
- Task lifecycle (start, stop, pause)
- File path vs URL vs AudioFile* routing
- State transitions
- Error propagation

### 8.2 Integration Tests

**SpeakerMediaPlayer:**
- Local file playback vs HTTP URL
- Playlist management with mixed sources
- Volume/mute with file playback
- Announcement interrupts media file

**Storage Integration:**
- Device availability checks
- Mount point resolution
- Hot-plug/unplug handling
- Multi-device scenarios (USB + SD simultaneously)

### 8.3 System Tests

**Hardware Setup:**
- ESP32-P4 DevKit
- USB audio stereo speaker
- USB flash drive with test files (MP3, FLAC, WAV)
- SD card with test files

**Test Scenarios:**
1. **Basic Playback:**
   - Play MP3 from USB
   - Play FLAC from SD
   - Play WAV from internal flash
   - Verify audio quality (no glitches, correct tempo)

2. **Memory Stress:**
   - Play 2-hour FLAC file (large)
   - Monitor PSRAM usage (should stay flat)
   - Monitor heap fragmentation
   - No crashes or audio dropouts

3. **Error Conditions:**
   - Play non-existent file → log error, stay idle
   - Play corrupt file → log error, stop gracefully
   - Disconnect USB during playback → stop, log warning
   - Reconnect USB → able to play again

4. **Mixed Playback:**
   - Queue multiple files from different sources
   - Play HTTP URL, then local file, then HTTP again
   - Verify seamless transitions

5. **Volume/Mute:**
   - Change volume during playback
   - Mute during playback
   - Volume persists after reboot

### 8.4 Performance Benchmarks

**Metrics:**
| Metric | Target | Measurement |
|--------|--------|-------------|
| Time to start playback | < 500ms | Time from `play_media` to first audio |
| PSRAM usage (steady state) | < 200KB | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` |
| Heap fragmentation | < 10% | Monitor over 24h playback test |
| CPU usage | < 30% | FreeRTOS task monitor |
| Buffer underruns | 0 | Count over 1h playback |

## 9. Migration Path

### 9.1 Existing Users (HTTP URLs)

**No changes required.** Existing configurations will continue to work:
```yaml
media_player.play_media:
  media_url: "http://example.com/audio.mp3"
```

### 9.2 video_player Users

**video_player component will remain separate** for now. Future migration:
1. Add `#warning` deprecation notice to video_player
2. Provide migration guide to media_player with video support
3. Set sunset date (e.g., 6 months)

**Migration example:**
```yaml
# OLD (video_player)
video_player:
  canvas: my_canvas
  speaker: my_speaker
  video_file: "/usb/video.mp4"

# NEW (media_player with video - future)
media_player:
  platform: video_media_player
  video_renderer: my_canvas
  speaker: my_speaker

automation:
  - media_player.play_media:
      media_url: "/usb/video.mp4"
```

## 10. Success Criteria

### 10.1 Phase 1 (Audio from USB/SD)

- [ ] USB audio speaker works with stereo output
- [ ] MP3, FLAC, WAV playback from USB storage
- [ ] MP3, FLAC, WAV playback from SD card
- [ ] Memory usage under 200KB PSRAM
- [ ] No audio glitches or dropouts
- [ ] Graceful handling of storage removal
- [ ] Documentation complete with examples
- [ ] All tests passing

### 10.2 Future Phases

- [ ] Video playback with H264 decoder
- [ ] LVGL rendering integration
- [ ] A/V synchronization
- [ ] Playlist management enhancements
- [ ] Network storage (NFS/SMB) support

## 11. Timeline

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| **Phase 1: USB Audio** | Week 1 | USB speaker working, HTTP streaming tested |
| **Phase 2: File Streaming** | Weeks 2-3 | AudioReader file support, local playback working |
| **Phase 3: Storage Integration** | Week 4 | Device awareness, hot-plug handling |
| **Phase 4: Polish & Docs** | Week 5 | Examples, tests, documentation |
| **Total (Audio-only)** | **5 weeks** | Production-ready audio playback from storage |
| **Future: Video Support** | TBD | Video playback, A/V sync, LVGL rendering |

## 12. Appendix

### 12.1 Key Files to Modify

| File | Changes | Reason |
|------|---------|--------|
| `audio/audio_reader.h/.cpp` | Add `start_file_path()` | File streaming support |
| `speaker/media_player/audio_pipeline.h/.cpp` | Add `start_file_path()` | Pipeline routing |
| `speaker/media_player/speaker_media_player.h/.cpp` | Add local path handling | Media player integration |
| `audio/audio.h/.cpp` | Add file type detection | Format detection |
| `speaker/media_player/__init__.py` | Storage dependency detection | Config validation |

### 12.2 New Files to Create

| File | Purpose |
|------|---------|
| `.ai/media-player-enhancement-plan.md` | This document |
| `tests/components/speaker_media_player/test.esp32-p4.yaml` | Component test |
| (Future) `components/media_player/video_renderer.h` | Video rendering abstraction |

### 12.3 Dependencies

**Required ESP-IDF Components:**
- `espressif/usb_host_uac` v1.3.1 (USB audio)
- `esp-idf/vfs` (File system abstraction)
- `esp-idf/fatfs` (FAT file system for USB/SD)

**Required ESPHome Components:**
- `audio` (decoder infrastructure)
- `speaker` (audio output abstraction)
- `storage` (optional, for device management)
- `usb_audio` (from PR #11728)

### 12.4 References

- **USB Audio PR:** https://github.com/esphome/esphome/pull/11728
- **ESPHome Media Player:** https://esphome.io/components/media_player/
- **ESP-IDF USB UAC:** https://components.espressif.com/components/espressif/usb_host_uac
- **esp-audio-libs:** https://github.com/espressif/esp-adf-libs
- **ESP32-P4 TRM:** https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf
