# Video Player Multi-Container/Codec Implementation Plan

## Overview
Transform video_player component to support multiple container formats (MP4, MKV) and audio codecs (AAC, MP3, FLAC) with user-configurable YAML options.

## CRITICAL: ESP32-P4 Memory Architecture Requirement

**ALWAYS use PSRam (SPIRAM) for buffers larger than a few bytes.**

**Memory constraints:**
- **Internal SRAM (regular heap)**: Only 512KB total, ~256KB available (already 50% used by system)
- **PSRam (external SPIRAM)**: 32MB total, ~8MB free for video_player component
- **PSRam speed**: 200MHz (FASTER than ESP32-S2/S3 PSRam @ 80MHz)

**Implementation rule:**
- Use `std::vector<T, ExternalRAMAllocator<T>>` for all buffers > few bytes
- Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` for raw pointer allocations
- ExternalRAMAllocator automatically falls back to internal RAM if PSRam is full
- This is NOT optional - the component WILL NOT FIT in internal SRAM

**Example:**
```cpp
// WRONG - uses scarce 512KB internal SRAM
std::vector<uint8_t> h264_frame_buffer_;  // 256KB buffer

// CORRECT - uses abundant 32MB PSRam @ 200MHz
std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> h264_frame_buffer_;
```

## YAML Configuration Design
```yaml
video_player:
  id: my_player
  canvas: video_canvas      # Optional for audio-only playback
  speaker: my_speaker       # Required for audio

  # User selects which formats to support (generates conditional compilation)
  containers: [mp4, mkv]              # USE_MP4_CONTAINER, USE_MKV_CONTAINER
  audio_codecs: [aac, mp3, flac]      # USE_AAC_DECODER, USE_AUDIO_MP3_SUPPORT, USE_AUDIO_FLAC_SUPPORT

  video_file: /sd/video.mp4
  auto_play: false
  loop: false
```

## Implementation Tasks

### 1. Add AAC Decoder Support to ESPHome Audio Component
**Files to modify:**
- `esphome/components/audio/audio.h`
- `esphome/components/audio/audio_decoder.h`
- `esphome/components/audio/audio_decoder.cpp`

**Changes needed:**
1. Add `AudioFileType::AAC` to enum in audio.h (conditionally compiled with `#ifdef USE_AUDIO_AAC_SUPPORT`)
2. Add AAC decoder includes and member in audio_decoder.h:
   ```cpp
   #ifdef USE_AUDIO_AAC_SUPPORT
   #include <aac_decoder.h>  // From esp_audio_codec
   #endif

   class AudioDecoder {
     #ifdef USE_AUDIO_AAC_SUPPORT
     FileDecoderState decode_aac_();
     std::unique_ptr<esp_audio_libs::aac_decoder::AACDecoder> aac_decoder_;
     #endif
   };
   ```

3. Add AAC case in `AudioDecoder::start()`:
   ```cpp
   #ifdef USE_AUDIO_AAC_SUPPORT
   case AudioFileType::AAC:
     this->aac_decoder_ = make_unique<esp_audio_libs::aac_decoder::AACDecoder>();
     // AAC typical frame size: 1024 samples per frame
     this->free_buffer_required_ = 1024 * sizeof(int16_t) * 2;
     this->output_transfer_buffer_->reallocate(this->free_buffer_required_);
     break;
   #endif
   ```

4. Implement `decode_aac_()` method following MP3/FLAC pattern

5. Add AAC to audio_file_type_to_string() in audio.cpp

**ESP-IDF Component:**
- Add `espressif/esp_audio_codec` v2.3.0 when AAC is enabled

---

### 2. Update video_player __init__.py
**File:** `esphome/components/video_player/__init__.py`

**Add configuration schema:**
```python
CONF_CONTAINERS = "containers"
CONF_AUDIO_CODECS = "audio_codecs"

CONTAINER_TYPES = ["mp4", "mkv"]
AUDIO_CODEC_TYPES = ["aac", "mp3", "flac"]

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(VideoPlayer),
    cv.Optional(CONF_CANVAS): cv.use_id(lv_canvas_t),  # Now optional!
    cv.Optional(CONF_SPEAKER): cv.use_id(...),

    # Container and codec selection
    cv.Optional(CONF_CONTAINERS, default=["mp4"]): cv.ensure_list(cv.one_of(*CONTAINER_TYPES, lower=True)),
    cv.Optional(CONF_AUDIO_CODECS, default=["mp3", "flac"]): cv.ensure_list(cv.one_of(*AUDIO_CODEC_TYPES, lower=True)),

    cv.Optional(CONF_VIDEO_FILE): cv.string,
    cv.Optional(CONF_AUTO_PLAY, default=False): cv.boolean,
    cv.Optional(CONF_LOOP, default=False): cv.boolean,
}).extend(cv.COMPONENT_SCHEMA)
```

**Validation:**
```python
def validate_video_player(config):
    # Canvas required only if video playback needed (not audio-only)
    if CONF_CANVAS not in config and CONF_VIDEO_FILE in config:
        # Will need to check file type at runtime
        _LOGGER.warning("Canvas not configured - only audio playback will be available")

    # Speaker required for audio codecs
    if config[CONF_AUDIO_CODECS] and CONF_SPEAKER not in config:
        raise cv.Invalid("Speaker required when audio codecs are configured")

    return config
```

**Code generation in to_code():**
```python
async def to_code(config):
    # Generate container defines
    for container in config[CONF_CONTAINERS]:
        cg.add_define(f"USE_{container.upper()}_CONTAINER")

    # Generate audio codec defines
    for codec in config[CONF_AUDIO_CODECS]:
        if codec == "aac":
            cg.add_define("USE_AAC_DECODER")
            cg.add_define("USE_AUDIO_AAC_SUPPORT")
            # Add esp_audio_codec component
            from esphome.components.esp32 import add_idf_component
            add_idf_component(name="espressif/esp_audio_codec", ref="2.3.0")
        elif codec == "mp3":
            cg.add_define("USE_AUDIO_MP3_SUPPORT")
        elif codec == "flac":
            cg.add_define("USE_AUDIO_FLAC_SUPPORT")
```

---

### 3. Add Codec Detection to MP4 Demuxer
**Files:** `esphome/components/video_player/mp4_demuxer.h`, `mp4_demuxer.cpp`

**Add codec type enum:**
```cpp
enum class AudioCodecType : uint8_t {
  UNKNOWN = 0,
  AAC,
  MP3,
  FLAC,
};

struct AudioTrackInfo {
  // ... existing fields ...
  AudioCodecType codec_type{AudioCodecType::UNKNOWN};
};
```

**Update parse_stsd_box() to detect codec:**
```cpp
bool MP4Demuxer::parse_stsd_box(uint32_t size, TrackType track_type) {
  // ... existing code ...

  if (track_type == TrackType::AUDIO) {
    // Detect codec from box type
    if (entry_type == BOX_TYPE_MP4A) {
      this->audio_track_.codec_type = AudioCodecType::AAC;
      ESP_LOGI(TAG, "stsd: AAC audio detected");
    } else if (entry_type == 0x2E6D7033) {  // '.mp3'
      this->audio_track_.codec_type = AudioCodecType::MP3;
      ESP_LOGI(TAG, "stsd: MP3 audio detected");
    } else if (entry_type == BOX_TYPE_FLAC) {
      this->audio_track_.codec_type = AudioCodecType::FLAC;
      ESP_LOGI(TAG, "stsd: FLAC audio detected");
    }
  }
}
```

**Add box type constants:**
```cpp
constexpr uint32_t BOX_TYPE_MP4A = 0x6D703461;  // 'mp4a' - AAC audio
```

---

### 4. Implement MKV (Matroska) Demuxer
**New files:** `esphome/components/video_player/mkv_demuxer.h`, `mkv_demuxer.cpp`

**MKV Structure:**
- EBML Header (magic: 0x1A45DFA3)
  - EBMLVersion, DocType ("matroska"), etc.
- Segment (0x18538067)
  - SeekHead (0x114D9B74) - index of top-level elements
  - Info (0x1549A966) - duration, timecode scale
  - Tracks (0x1654AE6B) - video/audio track info
    - TrackEntry
      - TrackNumber, TrackType (1=video, 2=audio)
      - CodecID ("V_MPEG4/ISO/AVC" = H264, "A_AAC", "A_MPEG/L3" = MP3, "A_FLAC")
      - Video (width, height)
      - Audio (sample rate, channels)
  - Cluster (0x1F43B675) - actual frame data
    - Timecode
    - SimpleBlock or BlockGroup with Block data

**Key classes:**
```cpp
class EBMLReader {
  // Parse EBML variable-length integers
  bool read_element_id(uint32_t &id);
  bool read_element_size(uint64_t &size);
};

class MKVDemuxer {
 public:
  bool open(const std::string &file_path);
  void close();
  bool has_video() const;
  bool has_audio() const;

  const VideoTrackInfo *get_video_track() const;
  const AudioTrackInfo *get_audio_track() const;

  bool get_next_video_sample(Sample &sample, uint8_t *buffer, size_t buffer_size);
  bool get_next_audio_sample(Sample &sample, uint8_t *buffer, size_t buffer_size);

 protected:
  bool parse_ebml_header_();
  bool parse_segment_();
  bool parse_tracks_();
  bool parse_track_entry_();
  bool skip_element_(uint64_t size);
};
```

---

### 5. Update video_player to Support Multiple Containers/Codecs

**video_player.h changes:**
```cpp
#ifdef USE_MP4_CONTAINER
#include "mp4_demuxer.h"
#endif
#ifdef USE_MKV_CONTAINER
#include "mkv_demuxer.h"
#endif

class VideoPlayer : public Component {
  // Demuxer is now polymorphic base class or variant
  #ifdef USE_MP4_CONTAINER
  std::unique_ptr<MP4Demuxer> mp4_demuxer_;
  #endif
  #ifdef USE_MKV_CONTAINER
  std::unique_ptr<MKVDemuxer> mkv_demuxer_;
  #endif

  // Active demuxer pointer (points to whichever is loaded)
  MediaDemuxer *active_demuxer_{nullptr};

  // Canvas now optional (for audio-only)
  lv_obj_t *canvas_{nullptr};  // Can be null
};
```

**Codec detection and routing:**
```cpp
bool VideoPlayer::init_audio_decoder_() {
  const AudioTrackInfo *audio = this->active_demuxer_->get_audio_track();

  // Determine AudioFileType from codec
  audio::AudioFileType file_type = audio::AudioFileType::NONE;

  switch (audio->codec_type) {
    case AudioCodecType::AAC:
      #ifdef USE_AAC_DECODER
      file_type = audio::AudioFileType::AAC;
      ESP_LOGI(TAG, "Using AAC decoder");
      #else
      ESP_LOGE(TAG, "AAC codec detected but AAC decoder not enabled");
      return false;
      #endif
      break;

    case AudioCodecType::MP3:
      #ifdef USE_AUDIO_MP3_SUPPORT
      file_type = audio::AudioFileType::MP3;
      ESP_LOGI(TAG, "Using MP3 decoder");
      #else
      ESP_LOGE(TAG, "MP3 codec detected but MP3 decoder not enabled");
      return false;
      #endif
      break;

    case AudioCodecType::FLAC:
      #ifdef USE_AUDIO_FLAC_SUPPORT
      file_type = audio::AudioFileType::FLAC;
      ESP_LOGI(TAG, "Using FLAC decoder");
      #else
      ESP_LOGE(TAG, "FLAC codec detected but FLAC decoder not enabled");
      return false;
      #endif
      break;

    default:
      ESP_LOGE(TAG, "Unknown or unsupported audio codec");
      return false;
  }

  // ... existing AudioDecoder setup ...
  err = this->audio_decoder_->start(file_type);
}
```

**Container detection from file extension:**
```cpp
bool VideoPlayer::load_video_(const std::string &file) {
  // Detect container from file extension
  std::string ext = file.substr(file.find_last_of(".") + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  #ifdef USE_MP4_CONTAINER
  if (ext == "mp4" || ext == "m4a" || ext == "m4v") {
    this->mp4_demuxer_ = std::make_unique<MP4Demuxer>();
    if (!this->mp4_demuxer_->open(file)) {
      return false;
    }
    this->active_demuxer_ = this->mp4_demuxer_.get();
    return true;
  }
  #endif

  #ifdef USE_MKV_CONTAINER
  if (ext == "mkv" || ext == "mka") {
    this->mkv_demuxer_ = std::make_unique<MKVDemuxer>();
    if (!this->mkv_demuxer_->open(file)) {
      return false;
    }
    this->active_demuxer_ = this->mkv_demuxer_.get();
    return true;
  }
  #endif

  ESP_LOGE(TAG, "Unsupported file format: %s", ext.c_str());
  return false;
}
```

**Audio-only playback support:**
```cpp
void VideoPlayer::setup() {
  // Canvas is now optional
  if (this->canvas_ == nullptr) {
    ESP_LOGW(TAG, "Canvas not configured - audio-only playback mode");
    // Don't mark_failed(), just skip video buffer allocation
  } else {
    // Allocate video frame buffers only if canvas exists
    this->h264_frame_buffer_.resize(MAX_H264_FRAME_SIZE);
    this->yuv_frame_buffer_.resize(MAX_YUV_FRAME_SIZE);
  }
}

bool VideoPlayer::load_video_(const std::string &file) {
  // ... container detection ...

  // Video track is optional now
  if (!this->active_demuxer_->has_video() && !this->active_demuxer_->has_audio()) {
    ESP_LOGE(TAG, "File has neither video nor audio tracks");
    return false;
  }

  if (this->active_demuxer_->has_video()) {
    // Allocate RGB buffer for video
  }

  if (this->active_demuxer_->has_audio()) {
    ESP_LOGI(TAG, "Audio track found");
  }

  return true;
}

void VideoPlayer::loop() {
  if (this->state_ != PlaybackState::PLAYING) {
    return;
  }

  // Process audio (always if available)
  this->process_audio_();

  // Process video only if video track exists
  if (this->active_demuxer_->has_video()) {
    this->process_video_frame_();
  }
}
```

---

## MediaDemuxer Base Class (Optional Polymorphism Approach)

Create abstract base class for common demuxer interface:

```cpp
class MediaDemuxer {
 public:
  virtual ~MediaDemuxer() = default;

  virtual bool open(const std::string &file_path) = 0;
  virtual void close() = 0;
  virtual void reset() = 0;

  virtual bool has_video() const = 0;
  virtual bool has_audio() const = 0;
  virtual bool is_open() const = 0;

  virtual const VideoTrackInfo *get_video_track() const = 0;
  virtual const AudioTrackInfo *get_audio_track() const = 0;

  virtual uint32_t get_video_duration_ms() const = 0;
  virtual uint32_t get_audio_duration_ms() const = 0;

  virtual bool get_next_video_sample(Sample &sample, uint8_t *buffer, size_t buffer_size) = 0;
  virtual bool get_next_audio_sample(Sample &sample, uint8_t *buffer, size_t buffer_size) = 0;

  virtual bool seek_video(uint32_t timestamp_ms) = 0;
  virtual bool seek_audio(uint32_t timestamp_ms) = 0;
};

class MP4Demuxer : public MediaDemuxer { /* ... */ };
class MKVDemuxer : public MediaDemuxer { /* ... */ };
```

---

## Testing Checklist

- [ ] MP4 with H264 + AAC
- [ ] MP4 with H264 + MP3
- [ ] MP4 with H264 + FLAC
- [ ] MP4 audio-only (AAC)
- [ ] MKV with H264 + AAC
- [ ] MKV with H264 + MP3
- [ ] MKV audio-only (MP3)
- [ ] Video-only file (no audio)
- [ ] Conditional compilation (only MP4, only MKV, various codec combinations)

---

## Migration Notes

**Breaking Changes:**
- `canvas` parameter is now optional (was required)
- Users must specify `containers` and `audio_codecs` lists

**Example migration:**
```yaml
# Old (no longer works - FLAC + MP4 is rare)
video_player:
  canvas: my_canvas
  video_file: /sd/video.mp4

# New (explicit codec support)
video_player:
  canvas: my_canvas
  speaker: my_speaker
  containers: [mp4]
  audio_codecs: [aac, mp3]  # Standard MP4 codecs
  video_file: /sd/video.mp4
```
