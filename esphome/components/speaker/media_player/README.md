# Speaker Media Player Component

A sophisticated media player for ESPHome with support for HTTP streaming, local file playback, and flash memory audio. Features dual-pipeline architecture for announcements and media, with hardware-accelerated audio decoding.

## Features

- ✅ **HTTP/HTTPS Streaming** - Play audio from web URLs
- ✅ **Local File Playback** - Stream audio from USB/SD storage (NEW)
- ✅ **Flash Memory Audio** - Play sounds from compiled firmware
- ✅ **Multiple Audio Formats** - MP3, FLAC, WAV, AAC support
- ✅ **Dual Pipeline Architecture** - Separate announcement and media streams
- ✅ **Playlist Support** - Queue multiple files with configurable delays
- ✅ **Pause/Resume** - Full media player control
- ✅ **Volume Control** - Software volume with mute support
- ✅ **Memory Efficient** - Streaming with configurable buffer sizes (~150-200KB PSRAM)
- ✅ **FreeRTOS Task-Based** - Non-blocking audio pipeline

## Hardware Requirements

- **ESP32 (ESP-IDF only)** - Required for FreeRTOS task support
- **PSRAM** - Highly recommended for smooth playback
- **I2S Speaker** - Any I2S audio output device
- **Storage** (optional) - USB/SD card for local file playback

## Supported Platforms

- ESP32 (all variants) with ESP-IDF framework
- **Note:** Arduino framework is not supported (requires FreeRTOS features)

## Audio Format Support

| Format | Extension | Conditional Compilation | Notes |
|--------|-----------|------------------------|-------|
| WAV | `.wav` | Always enabled | Uncompressed PCM |
| MP3 | `.mp3` | `USE_AUDIO_MP3_SUPPORT` | libhelix decoder |
| FLAC | `.flac` | `USE_AUDIO_FLAC_SUPPORT` | Lossless compression |
| AAC | `.aac`, `.m4a` | `USE_AUDIO_AAC_SUPPORT` | Advanced Audio Coding |

## Installation

### Option 1: Built-in Component

Already included in ESPHome core - no installation needed!

### Option 2: External Component (Development/PR Testing)

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/esphome/esphome
      ref: dev
    components: [speaker, media_player, audio]
```

## Configuration

### Basic Example - HTTP Streaming Only

```yaml
# I2S Speaker configuration
i2s_audio:
  - id: i2s_out
    i2s_lrclk_pin: GPIO25
    i2s_bclk_pin: GPIO26

speaker:
  - platform: i2s_audio
    id: media_speaker
    dac_type: external
    i2s_dout_pin: GPIO27
    mode: stereo

# Media Player
media_player:
  - platform: speaker
    id: my_media_player
    speaker: media_speaker

    # Buffer configuration (optional)
    buffer_size: 24576       # Ring buffer size (default: 24KB)

    # Volume settings (optional)
    volume_initial: 0.5      # Initial volume on first boot
    volume_increment: 0.05   # Amount for volume up/down commands
    volume_min: 0.0         # Minimum volume (0.0 = mute)
    volume_max: 1.0         # Maximum volume

# Play HTTP stream from Home Assistant
button:
  - platform: template
    name: "Play Radio Stream"
    on_press:
      - media_player.play_media:
          id: my_media_player
          media_url: "http://stream.example.com/radio.mp3"
```

### Complete Example - Local File Playback from USB/SD

```yaml
# SPI Bus for SD/USB
spi:
  - id: spi_bus
    clk_pin: GPIO18
    mosi_pin: GPIO23
    miso_pin: GPIO19

# Storage Component with USB/SD support
storage:
  id: storage_id

  # USB Storage (ESP32-S2/S3/P4)
  usb_storage:
    - id: usb_drive
      mount_point: /usb

  # SD Card Storage
  sd_card:
    cs_pin: GPIO5
    spi_id: spi_bus
    mount_point: /sd

# I2S Speaker
i2s_audio:
  - id: i2s_out
    i2s_lrclk_pin: GPIO25
    i2s_bclk_pin: GPIO26

speaker:
  - platform: i2s_audio
    id: media_speaker
    dac_type: external
    i2s_dout_pin: GPIO27
    mode: stereo
    sample_rate: 48000
    bits_per_sample: 16bit

# Media Player with local file support
media_player:
  - platform: speaker
    id: my_media_player
    speaker: media_speaker

    # Buffer configuration for smooth playback
    buffer_size: 32768           # 32KB ring buffer
    task_stack_in_psram: true    # Use PSRAM for task stacks (recommended)

    # Volume settings
    volume_initial: 0.5
    volume_increment: 0.05
    volume_min: 0.0
    volume_max: 1.0

# Play local audio files
button:
  - platform: template
    name: "Play Local MP3"
    on_press:
      - media_player.play_media:
          id: my_media_player
          media_url: "/usb/music/song.mp3"

  - platform: template
    name: "Play from SD Card"
    on_press:
      - media_player.play_media:
          id: my_media_player
          media_url: "/sd/audio/sound.flac"

  - platform: template
    name: "Play HTTP Stream"
    on_press:
      - media_player.play_media:
          id: my_media_player
          media_url: "http://example.com/stream.mp3"
```

### Advanced Example - Dual Pipeline (Media + Announcements)

```yaml
# Two speakers: one for media, one for announcements
speaker:
  - platform: i2s_audio
    id: media_speaker
    dac_type: external
    i2s_dout_pin: GPIO27
    mode: stereo

  - platform: i2s_audio
    id: announcement_speaker
    dac_type: external
    i2s_dout_pin: GPIO14
    mode: mono

media_player:
  - platform: speaker
    id: my_media_player

    # Media pipeline
    media_speaker: media_speaker
    media_format:
      purpose: default
      sample_rate: 48000
      num_channels: 2
      format: signed_16bit
      codec: mp3

    # Announcement pipeline (interrupts media)
    announcement_speaker: announcement_speaker
    announcement_format:
      purpose: announcement
      sample_rate: 16000
      num_channels: 1
      format: signed_16bit
      codec: mp3

    # Playlist delays
    media_playlist_delay: 2s
    announcement_playlist_delay: 500ms

    buffer_size: 32768
    task_stack_in_psram: true

# Play announcement (interrupts media)
button:
  - platform: template
    name: "Play Doorbell Chime"
    on_press:
      - media_player.play_media:
          id: my_media_player
          media_url: "/usb/sounds/doorbell.mp3"
          announce: true  # Uses announcement pipeline

# Play background music
button:
  - platform: template
    name: "Play Music"
    on_press:
      - media_player.play_media:
          id: my_media_player
          media_url: "/usb/music/background.mp3"
          announce: false  # Uses media pipeline
```

### Playlist Example

```yaml
media_player:
  - platform: speaker
    id: my_media_player
    speaker: media_speaker
    media_playlist_delay: 3s  # 3 second delay between tracks

# Play multiple files in sequence
button:
  - platform: template
    name: "Play Album"
    on_press:
      # First track (starts immediately, clears previous playlist)
      - media_player.play_media:
          id: my_media_player
          media_url: "/usb/album/track1.mp3"

      # Queue additional tracks
      - media_player.play_media:
          id: my_media_player
          media_url: "/usb/album/track2.mp3"
          enqueue: true

      - media_player.play_media:
          id: my_media_player
          media_url: "/usb/album/track3.mp3"
          enqueue: true
```

## Configuration Options

### Media Player Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `speaker` | ID | - | Speaker for single-pipeline mode (media + announcements) |
| `media_speaker` | ID | - | Speaker for media pipeline (dual-pipeline mode) |
| `announcement_speaker` | ID | - | Speaker for announcement pipeline (dual-pipeline mode) |
| `buffer_size` | int | 24576 | Ring buffer size between reader and decoder (bytes) |
| `task_stack_in_psram` | bool | false | Allocate FreeRTOS task stacks in PSRAM (recommended if available) |
| `volume_initial` | float | 0.5 | Initial volume on first boot (0.0 - 1.0) |
| `volume_increment` | float | 0.05 | Volume change for up/down commands |
| `volume_min` | float | 0.0 | Minimum volume limit |
| `volume_max` | float | 1.0 | Maximum volume limit |
| `media_playlist_delay` | time | 0s | Delay between media playlist items |
| `announcement_playlist_delay` | time | 0s | Delay between announcement playlist items |
| `media_format` | object | - | Audio format specification for media pipeline |
| `announcement_format` | object | - | Audio format specification for announcement pipeline |

### Format Specification

```yaml
media_format:
  purpose: default          # default or announcement
  sample_rate: 48000       # 16000, 44100, 48000, etc.
  num_channels: 2          # 1 (mono) or 2 (stereo)
  format: signed_16bit     # signed_16bit, signed_32bit
  codec: mp3               # mp3, flac, wav, aac
```

## Media Player Actions

### play_media

Play audio from URL or local file path. Automatically detects source type.

```yaml
# HTTP/HTTPS URL
- media_player.play_media:
    id: my_media_player
    media_url: "http://example.com/audio.mp3"
    announce: false      # Optional: use announcement pipeline
    enqueue: false       # Optional: add to playlist instead of replacing

# Local file path (starts with '/')
- media_player.play_media:
    id: my_media_player
    media_url: "/usb/music/song.mp3"

# Local file path (file:// URI)
- media_player.play_media:
    id: my_media_player
    media_url: "file:///sd/audio/sound.flac"
```

### Standard Media Player Actions

```yaml
# Playback control
- media_player.play: my_media_player
- media_player.pause: my_media_player
- media_player.stop: my_media_player
- media_player.toggle: my_media_player

# Volume control
- media_player.volume_up: my_media_player
- media_player.volume_down: my_media_player
- media_player.volume_set:
    id: my_media_player
    volume: 0.75

# Mute control
- media_player.mute: my_media_player
- media_player.unmute: my_media_player
```

## URL Detection and Routing

The media player automatically detects the media source:

| URL Format | Detection | Routing |
|------------|-----------|---------|
| `http://...` | Starts with `http://` | HTTP streaming via esp_http_client |
| `https://...` | Starts with `https://` | HTTPS streaming via esp_http_client |
| `/usb/...` | Starts with `/` | Local file via FILE* streaming |
| `/sd/...` | Starts with `/` | Local file via FILE* streaming |
| `file:///...` | Starts with `file://` | Local file (strips prefix) |

**Note:** The `file://` prefix is automatically stripped before opening the file.

## Lambda API

Access media player functionality in C++ lambdas:

```yaml
# Play local file from lambda
lambda: |-
  // Play specific file
  id(my_media_player)->play("/usb/music/song.mp3");

  // Control playback
  id(my_media_player)->pause();
  id(my_media_player)->resume();
  id(my_media_player)->stop();

  // Volume control
  id(my_media_player)->set_volume(0.8f);
  float vol = id(my_media_player)->volume;

  // Mute control
  id(my_media_player)->mute();
  id(my_media_player)->unmute();
  bool muted = id(my_media_player)->is_muted();

  // Get state
  auto state = id(my_media_player)->state;
  // States: IDLE, PLAYING, PAUSED, ANNOUNCING
```

## Implementation Details

### Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                  SpeakerMediaPlayer                     │
│  (Component running in main loop, processes commands)   │
└────────────────────┬────────────────────────────────────┘
                     │
        ┌────────────┴────────────┐
        │                         │
        ▼                         ▼
┌──────────────┐          ┌──────────────┐
│ AudioPipeline│          │ AudioPipeline│
│   (Media)    │          │(Announcement)│
└──────┬───────┘          └──────┬───────┘
       │                         │
       └─────────┬───────────────┘
                 │
    ┌────────────┼────────────┐
    ▼            ▼            ▼
┌────────┐  ┌─────────┐  ┌─────────┐
│ Reader │→ │ Decoder │→ │ Speaker │
│  Task  │  │  Task   │  │Component│
└────────┘  └─────────┘  └─────────┘
 (FreeRTOS)  (FreeRTOS)   (Hardware)
```

### Audio Pipeline Flow

Each AudioPipeline consists of two FreeRTOS tasks communicating via ring buffers:

1. **Reader Task** - Reads audio data from source
   - HTTP/HTTPS: Streams via `esp_http_client`
   - Local file: Streams via `FILE*` handle (NEW)
   - Flash: Reads from compiled `AudioFile*` struct
   - Writes raw compressed audio to ring buffer

2. **Decoder Task** - Decodes audio to PCM
   - Reads compressed audio from ring buffer
   - Decodes based on file type (MP3/FLAC/WAV/AAC)
   - Outputs 16-bit PCM directly to speaker component

3. **Speaker Component** - Hardware output
   - Receives PCM audio samples
   - Handles I2S DMA transfer
   - Manages volume and mute state

### Local File Streaming Implementation

File streaming uses standard C FILE* handles with chunk-based reading:

```cpp
// AudioReader::start_file_path()
this->file_handle_ = fopen(file_path.c_str(), "rb");

// Get file size
fseek(this->file_handle_, 0, SEEK_END);
this->file_size_ = ftell(this->file_handle_);
fseek(this->file_handle_, 0, SEEK_SET);

// Detect format from extension
std::string path_lower = str_lower_case(file_path);
if (str_endswith(path_lower, ".mp3")) {
  file_type = AudioFileType::MP3;
}
// ... (FLAC, WAV, AAC)

// Stream chunks in read loop
size_t bytes_read = fread(buffer, 1, chunk_size, this->file_handle_);
```

**Memory Efficiency:**
- Only `buffer_size_` bytes held in memory (default 24KB)
- No need to load entire file
- Suitable for large audio files on constrained devices

### Memory Usage

Typical memory footprint per active pipeline:

| Component | Size | Location | Description |
|-----------|------|----------|-------------|
| Ring Buffer | 24-32KB | PSRAM/DRAM | Audio data between reader/decoder |
| Transfer Buffer | 24KB | PSRAM/DRAM | Internal reader buffering |
| Decoder Buffer | 24KB | PSRAM/DRAM | Internal decoder buffering |
| Task Stacks | ~16KB | DRAM/PSRAM | FreeRTOS task stacks |
| **Total** | **~90-110KB** | **per pipeline** | Actual usage depends on buffer_size |

**PSRAM Recommendation:** Using `task_stack_in_psram: true` moves task stacks to external RAM, reducing internal DRAM pressure.

### File Type Detection

Audio format is detected from file extension:

| Extension | Format | Requires |
|-----------|--------|----------|
| `.wav` | WAV | Always available |
| `.mp3` | MP3 | `USE_AUDIO_MP3_SUPPORT` |
| `.flac` | FLAC | `USE_AUDIO_FLAC_SUPPORT` |
| `.aac`, `.m4a` | AAC | `USE_AUDIO_AAC_SUPPORT` |

HTTP streams may also detect format from `Content-Type` header.

## Home Assistant Integration

### Automatic Discovery

The media player automatically appears in Home Assistant with full control:

```yaml
# Home Assistant service calls
service: media_player.play_media
target:
  entity_id: media_player.my_media_player
data:
  media_content_id: "http://example.com/stream.mp3"
  media_content_type: "music"

# Local file playback (if storage is configured)
service: media_player.play_media
target:
  entity_id: media_player.my_media_player
data:
  media_content_id: "/usb/music/song.mp3"
  media_content_type: "music"
```

### ESPHome Services

Custom services for advanced control:

```yaml
# ESPHome YAML
api:
  services:
    - service: play_local_file
      variables:
        file_path: string
      then:
        - media_player.play_media:
            id: my_media_player
            media_url: !lambda 'return file_path;'

    - service: play_announcement
      variables:
        sound_file: string
      then:
        - media_player.play_media:
            id: my_media_player
            media_url: !lambda 'return sound_file;'
            announce: true

# Home Assistant usage
service: esphome.my_device_play_local_file
data:
  file_path: "/usb/sounds/doorbell.mp3"
```

## Performance Tips

1. **Enable PSRAM** - Essential for smooth playback
   ```yaml
   psram:
     mode: octal
     speed: 80MHz
   ```

2. **Use task_stack_in_psram** - Reduces internal DRAM usage
   ```yaml
   media_player:
     - platform: speaker
       task_stack_in_psram: true
   ```

3. **Increase buffer_size for unreliable networks** - Helps with HTTP streaming
   ```yaml
   buffer_size: 65536  # 64KB buffer
   ```

4. **Pre-encode audio files** - Convert to supported formats with consistent settings
   ```bash
   # Optimize MP3 for embedded playback
   ffmpeg -i input.mp3 -ar 48000 -ac 2 -b:a 128k output.mp3

   # Convert to FLAC (lossless)
   ffmpeg -i input.wav -ar 48000 -ac 2 output.flac
   ```

5. **Use fast storage** - SD cards with fast SPI mode
   ```yaml
   spi:
     clk_pin: GPIO18
     frequency: 40MHz  # Maximum for SD cards
   ```

## Troubleshooting

### Audio not playing from local files

**Check storage is mounted:**
```yaml
# Add logging
storage:
  on_mount:
    - logger.log: "Storage mounted successfully"
  on_unmount:
    - logger.log: "Storage unmounted"
```

**Verify file exists:**
- Check ESPHome logs for file open errors
- Ensure file path starts with `/` or `file://`
- Verify file format is supported (.mp3, .flac, .wav, .aac)

### HTTP streaming fails

- Check network connectivity
- Verify URL is accessible from ESP32
- Some streaming servers require specific User-Agent headers
- Increase `buffer_size` for buffering

### Choppy playback / stuttering

- Enable PSRAM
- Use `task_stack_in_psram: true`
- Reduce audio bitrate (use lower quality files)
- Use faster storage (high-speed SD cards)
- Increase `buffer_size`

### Out of memory errors

- Enable PSRAM (required for most use cases)
- Reduce `buffer_size` if PSRAM not available
- Use single pipeline mode instead of dual pipeline
- Ensure no memory leaks in other components

### Announcements not interrupting media

- Verify dual pipeline configuration (media_speaker + announcement_speaker)
- Set `announce: true` in play_media action
- Check both speakers are properly configured

### Volume control not working

- Ensure speaker component supports software volume
- Check I2S DAC supports volume control
- Try software volume in speaker component

## Platform-Specific Notes

### ESP32 (Original)

- Limited internal DRAM (~320KB)
- PSRAM essential for media playback
- Use `task_stack_in_psram: true`

### ESP32-S2

- No Bluetooth (frees up RAM)
- USB support for USB storage
- Good PSRAM performance

### ESP32-S3

- Improved performance
- USB support for USB storage
- Better multitasking for dual pipelines

### ESP32-P4

- Most powerful option
- Hardware JPEG decoder (for future video support)
- Large PSRAM (32MB @ 200MHz)
- USB host for USB storage

### ESP32-C3/C6/H2

- Limited resources
- May struggle with dual pipelines
- Prefer single pipeline mode

## Future Enhancements

- [ ] Video playback support (H.264 decoding)
- [ ] Bluetooth audio source (A2DP sink)
- [ ] Audio visualization (FFT spectrum analyzer)
- [ ] Gapless playback between tracks
- [ ] Crossfade between playlist items
- [ ] Seek support for local files
- [ ] Resume playback after reboot
- [ ] Audio effects (equalizer, bass boost)

## License

Part of ESPHome - https://esphome.io

## Related Components

- [storage](../../../storage/) - USB/SD storage management
- [speaker](../../) - Base speaker component
- [i2s_audio](../../i2s_audio/) - I2S audio output
- [media_player](../../../media_player/) - Base media player component
