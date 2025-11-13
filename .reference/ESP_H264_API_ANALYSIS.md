# ESP H.264 Component API Analysis

**Repository:** https://github.com/espressif/esp-h264-component
**Version:** 1.1.2 (used in ESPHome transcoder)
**Last Updated:** 2025-01-13

## Overview

The esp-h264-component provides H.264 video encoding and decoding capabilities for ESP32 chips. It supports:
- **Software encoder/decoder** (ESP32-S3, ESP32-P4) using OpenH264 and TinyH264
- **Hardware encoder** (ESP32-P4 only) using dedicated H.264 hardware
- **Hardware-accelerated decoder** (ESP32-P4 only) - Note: ESP32-P4 has hardware encoder but software decoder

## Critical Architecture Understanding

### Platform Support Matrix

| Platform | Decoder | Encoder |
|----------|---------|---------|
| ESP32-P4 | Software (TinyH264) | Hardware |
| ESP32-S3 | Software (TinyH264) | Software (OpenH264) |
| Others   | Not supported | Not supported |

**IMPORTANT:** ESP32-P4 has H.264 hardware at 0x50084000, but it's **ONLY for encoding**. The decoder is software-based (TinyH264). However, ESP32-P4's powerful dual-core CPU @ 400MHz can achieve **31 fps at 640x480** with dual-task decoder optimization.

### Performance Reality Check

**ESP32-P4 Decoder Performance (with dual-task optimization):**
- 640x480: **31 fps** ✅ Smooth video playback capability!
- 1280x720: **10 fps** ⚠️ Acceptable for some use cases
- Memory: Uses PSRam @ 200MHz (faster than ESP32-S3's 80MHz)

**THIS IS A MICROCONTROLLER, NOT A CPU:**
- Software H.264 decoding at 31fps @ 640x480 is actually impressive for an MCU
- Dual-task decoder splits workload across both CPU cores
- Must enable `ESP_H264_DUAL_TASK=1` and `ESP_H264_DECODER_IRAM=1` in menuconfig
- Critical for video playback: Without dual-task, only 25 fps @ 640x480

## Decoder API

### Core Interface Pattern

The esp_h264 library uses an interface-based pattern where all operations go through function pointers:

```c
typedef struct esp_h264_dec_if {
    esp_h264_err_t (*open)(esp_h264_dec_handle_t dec);
    esp_h264_err_t (*process)(esp_h264_dec_handle_t dec,
                              esp_h264_dec_in_frame_t *in_frame,
                              esp_h264_dec_out_frame_t *out_frame);
    esp_h264_err_t (*close)(esp_h264_dec_handle_t dec);
    esp_h264_err_t (*del)(esp_h264_dec_handle_t dec);
} esp_h264_dec_t;
```

### Decoder Lifecycle

#### 1. Create Decoder Instance

**Software Decoder (ESP32-P4, ESP32-S3):**

```c
#include "esp_h264_dec_sw.h"

// Configuration - only pic_type field exists
esp_h264_dec_cfg_sw_t cfg = {
    .pic_type = ESP_H264_RAW_FMT_I420  // YUV420 planar format
};

esp_h264_dec_handle_t decoder;
esp_h264_err_t ret = esp_h264_dec_sw_new(&cfg, &decoder);
if (ret != ESP_H264_ERR_OK) {
    // Handle error
}
```

**CRITICAL:** The config struct ONLY has `pic_type` field. There are NO `pic_num`, `timeout`, or other fields.

#### 2. Open Decoder

```c
ret = esp_h264_dec_open(decoder);  // Takes only handle, NO config parameter
if (ret != ESP_H264_ERR_OK) {
    esp_h264_dec_del(decoder);
    // Handle error
}
```

**CRITICAL:** `esp_h264_dec_open()` takes ONLY the decoder handle. There is no config parameter.

#### 3. Process Frames

```c
// Input frame structure
esp_h264_dec_in_frame_t in_frame = {
    .raw_data = {
        .buffer = h264_data,
        .len = h264_size
    },
    .consume = 0,  // Will be filled by decoder
    .pts = 0,
    .dts = 0
};

// Output frame structure
esp_h264_dec_out_frame_t out_frame = {
    .outbuf = NULL,      // Decoder allocates internally
    .out_size = 0,       // Will be filled by decoder
    .frame_type = 0,
    .pts = 0,
    .dts = 0
};

// Decode
ret = esp_h264_dec_process(decoder, &in_frame, &out_frame);
if (ret == ESP_H264_ERR_OK && out_frame.out_size > 0) {
    // Copy decoded YUV data from out_frame.outbuf
    // Buffer is valid until next esp_h264_dec_process() call
    memcpy(yuv_buffer, out_frame.outbuf, out_frame.out_size);
}
```

**CRITICAL:** The decoder manages output buffer internally. You must copy data before next `process()` call.

#### 4. Close and Delete

```c
esp_h264_dec_close(decoder);
esp_h264_dec_del(decoder);
```

### YUV Format Details

**ESP_H264_RAW_FMT_I420 (YUV420 Planar):**
- Also known as IYUV
- Layout: All Y samples first, then all U samples, then all V samples
- Size calculation: `width * height * 1.5` bytes
  - Y plane: `width * height` bytes
  - U plane: `(width/2) * (height/2)` bytes
  - V plane: `(width/2) * (height/2)` bytes

## Encoder API

### Software Encoder (ESP32-S3, ESP32-P4)

```c
#include "esp_h264_enc_single_sw.h"

esp_h264_enc_cfg_sw_t cfg = {
    .pic_type = ESP_H264_RAW_FMT_YUYV,  // Input format
    .gop = 30,
    .fps = 30,
    .res = {
        .width = 640,
        .height = 480
    },
    .rc = {
        .bitrate = 1000000,
        .qp_min = 10,
        .qp_max = 51
    }
};

esp_h264_enc_handle_t encoder;
esp_h264_err_t ret = esp_h264_enc_sw_new(&cfg, &encoder);
```

### Hardware Encoder (ESP32-P4 Only)

```c
#include "esp_h264_enc_single_hw.h"

esp_h264_enc_cfg_hw_t cfg = {
    .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,  // Hardware-specific format
    .gop = 30,
    .fps = 30,
    .res = {
        .width = 640,
        .height = 480
    },
    .rc = {
        .bitrate = 1000000,
        .qp_min = 10,
        .qp_max = 51
    }
};

esp_h264_enc_handle_t encoder;
esp_h264_err_t ret = esp_h264_enc_hw_new(&cfg, &encoder);
```

## Common Mistakes to Avoid

### ❌ WRONG: Invented API

```cpp
// DOES NOT EXIST IN esp_h264 v1.1.2
esp_h264_dec_cfg_t dec_config = {};
dec_config.pic_num = 3;      // ❌ Field doesn't exist
dec_config.timeout = 200;    // ❌ Field doesn't exist

ret = esp_h264_dec_open(&dec_config, &decoder);  // ❌ Wrong signature
```

### ✅ CORRECT: Actual API

```cpp
// Use software decoder API
esp_h264_dec_cfg_sw_t cfg = {};
cfg.pic_type = ESP_H264_RAW_FMT_I420;  // ✅ Only field that exists

esp_h264_dec_handle_t decoder;
ret = esp_h264_dec_sw_new(&cfg, &decoder);  // ✅ Create decoder
if (ret == ESP_H264_ERR_OK) {
    ret = esp_h264_dec_open(decoder);  // ✅ Correct signature
}
```

## ESPHome Integration Issues

### Problem in transcoder.cpp (Lines 49-67)

The transcoder component incorrectly initializes H.264 decoder:

**Current (WRONG) Code:**
```cpp
#ifdef USE_ESP_H264_DECODER
  esp_h264_dec_cfg_t dec_config = {};
  dec_config.pic_num = 3;    // ❌ Field doesn't exist
  dec_config.timeout = 200;  // ❌ Field doesn't exist

  ret = esp_h264_dec_open(&dec_config, &this->h264_decoder_);  // ❌ Wrong API
```

**Should Be (CORRECT):**
```cpp
#ifdef USE_ESP_H264_DECODER
  esp_h264_dec_cfg_sw_t cfg = {};
  cfg.pic_type = ESP_H264_RAW_FMT_I420;

  esp_h264_err_t ret = esp_h264_dec_sw_new(&cfg, &this->h264_decoder_);
  if (ret != ESP_H264_OK) {
    ESP_LOGE(TAG, "Failed to create H.264 decoder: %s", esp_err_to_name(ret));
  } else {
    ret = esp_h264_dec_open(this->h264_decoder_);
    if (ret != ESP_H264_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open H.264 decoder: %s", esp_err_to_name(ret));
      esp_h264_dec_del(this->h264_decoder_);
      this->h264_decoder_ = nullptr;
    } else {
      ESP_LOGI(TAG, "H.264 decoder initialized");
    }
  }
#endif
```

### Reference Implementation

The video_player component (lines 316-330 in video_player.cpp) has the CORRECT implementation:

```cpp
esp_h264_dec_cfg_sw_t cfg = {};
cfg.pic_type = ESP_H264_RAW_FMT_I420;

esp_h264_err_t ret = esp_h264_dec_sw_new(&cfg, &this->decoder_);
if (ret != ESP_H264_ERR_OK) {
  ESP_LOGE(TAG, "Failed to create H.264 decoder: %d", ret);
  return false;
}

ret = esp_h264_dec_open(this->decoder_);
if (ret != ESP_H264_ERR_OK) {
  ESP_LOGE(TAG, "Failed to open H.264 decoder: %d", ret);
  esp_h264_dec_del(this->decoder_);
  this->decoder_ = nullptr;
  return false;
}
```

## Required Headers

### For Software Decoder:
```cpp
#include "esp_h264_dec.h"          // Common decoder interface
#include "esp_h264_dec_sw.h"       // Software decoder API
#include "esp_h264_dec_param.h"    // Parameter interface
#include "esp_h264_types.h"        // Common types
```

### For Software Encoder:
```cpp
#include "esp_h264_enc_single.h"      // Common encoder interface
#include "esp_h264_enc_single_sw.h"   // Software encoder API
#include "esp_h264_types.h"           // Common types
```

### For Hardware Encoder (ESP32-P4):
```cpp
#include "esp_h264_enc_single.h"      // Common encoder interface
#include "esp_h264_enc_single_hw.h"   // Hardware encoder API
#include "esp_h264_types.h"           // Common types
```

## Memory Considerations for ESP32-P4

The esp_h264 decoder allocates buffers internally. For ESP32-P4 with limited internal SRAM (512KB) but abundant PSRam (32MB @ 200MHz):

- Decoder manages its own buffers (typically PSRam)
- Application must copy decoded frames promptly
- Original video_player uses `ExternalRAMAllocator` for frame buffers
- PSRam on P4 is FASTER (200MHz) than S2/S3 PSRam (80MHz)

## Error Codes

```c
typedef enum {
    ESP_H264_ERR_OK          = 0,   // Success
    ESP_H264_ERR_FAIL        = -1,  // General failure
    ESP_H264_ERR_ARG         = -2,  // Invalid arguments
    ESP_H264_ERR_MEM         = -3,  // Out of memory
    ESP_H264_ERR_UNSUPPORTED = -5,  // Feature not supported
    ESP_H264_ERR_TIMEOUT     = -6,  // Operation timeout
    ESP_H264_ERR_OVERFLOW    = -7,  // Buffer overflow
} esp_h264_err_t;
```

## Testing Reference

The repository contains test applications in `esp_h264/test_apps/` that demonstrate correct API usage:
- `main/esp_h264_sw_dec_test.h` - Software decoder test
- `main/esp_h264_sw_enc_test.h` - Software encoder test
- `main/esp_h264_hw_enc_test.h` - Hardware encoder test

## Summary

**Key Takeaways:**
1. ESP32-P4 uses SOFTWARE decoder (TinyH264), not hardware
2. Decoder config struct has ONLY `pic_type` field
3. `esp_h264_dec_open()` takes ONLY the handle parameter
4. Must call `esp_h264_dec_sw_new()` before `esp_h264_dec_open()`
5. Decoder manages output buffers internally - copy data promptly
6. video_player.cpp has correct implementation to reference
7. transcoder.cpp needs fixing to match video_player pattern
