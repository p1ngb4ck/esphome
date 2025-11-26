# Dual USB Host Support for ESP32-P4 - Implementation Summary

## Executive Summary

This document explains the implementation of dual USB Host support for ESP32-P4, enabling simultaneous operation of both Full-Speed (FS) and High-Speed (HS) USB OTG controllers. This work spans three architectural layers: TinyUSB core, ESP-IDF USB class drivers, and ESPHome integration.

## Problem Statement

### Hardware Constraint: ESP32-P4 Cannot Translate USB Speeds

The ESP32-P4 has a unique hardware architecture:
- **USB0**: Full-Speed (FS) only - 12 Mbps
- **USB1**: High-Speed (HS) capable - 480 Mbps
- **Critical Limitation**: ESP32-P4 **cannot perform speed translation** between FS and HS

### Real-World Impact

Different USB device classes typically operate at different speeds:

| Device Class | Typical Speed | Reason |
|-------------|---------------|---------|
| USB Audio (UAC) | Full-Speed (12 Mbps) | Audio bandwidth doesn't require HS |
| USB HID (keyboard/mouse) | Full-Speed (12 Mbps) | Low data rate devices |
| USB Mass Storage (MSC) | Either FS or HS | Depends on device (thumb drives often HS) |
| USB Video (UVC) | High-Speed (480 Mbps) | High bandwidth for video streams |

### Problem Without Dual USB Host Support

**Single Controller Configuration:**
- **USB0 (FS) only**: Can use audio + HID, but NOT HS video cameras or fast storage
- **USB1 (HS) only**: Can use video + fast storage, but some FS-only devices may not enumerate properly

**Consequence**: Cannot mix device classes that require different USB speeds on ESP32-P4

### Solution: Dual USB Host Support

**With dual USB Host support:**
- **USB0 (FS)**: Connect USB audio interface, HID devices (keyboard/mouse)
- **USB1 (HS)**: Connect USB webcam, high-speed flash drives
- **Result**: Use all device classes simultaneously without speed translation issues

This is a **hardware limitation workaround** that requires software support for dual instances.

---

## Implementation Architecture

### The Software Stack (Bottom to Top)

```
┌─────────────────────────────────────────────────────────┐
│ ESPHome Components (usb_storage, usb_audio, usb_uart)  │ ← Layer 4: Application
├─────────────────────────────────────────────────────────┤
│ ESP-IDF USB Class Drivers (MSC, UAC, CDC-ACM, HID, UVC)│ ← Layer 3: Protocol
├─────────────────────────────────────────────────────────┤
│ TinyUSB Stack                                           │ ← Layer 2: USB Core
├─────────────────────────────────────────────────────────┤
│ ESP32-P4 Hardware (USB0-FS, USB1-HS)                   │ ← Layer 1: Hardware
└─────────────────────────────────────────────────────────┘
```

### Why This Layered Approach is Necessary

1. **TinyUSB** directly interfaces with hardware - must support multiple instances first
2. **ESP-IDF drivers** sit on top of TinyUSB - must propagate instance handles
3. **ESPHome** uses ESP-IDF drivers - must configure which controller to use

**Critical**: Implementation must start at the lowest layer and work upward. Any other approach would fail.

---

## Implementation Details

### Phase 1: TinyUSB Core Modifications (Foundation Layer)

**Repository**: https://github.com/hathach/tinyusb (upstream) → Modified in ESP-IDF fork

**What was changed:**
- Converted from singleton pattern to instance-based pattern
- Introduced `tuh_instance_t` opaque pointer type
- Each USB Host instance can be bound to a specific hardware controller
- Modified all `tuh_*` APIs to accept instance parameter

**Key changes:**
```c
// Before (Singleton):
void tuh_init(uint8_t rhport);
bool tuh_mounted(uint8_t dev_addr);

// After (Multi-Instance):
typedef struct usbh_instance* tuh_instance_t;
tuh_instance_t tuh_init(uint8_t rhport);
bool tuh_instance_mounted(tuh_instance_t inst, uint8_t dev_addr);
```

**Why this is critical:**
- TinyUSB is the foundation - it MUST support multiple instances first
- Without this, no upper layers can support dual controllers
- Each instance maintains separate device tables, transfer queues, and state machines

### Phase 2: ESP-IDF USB Class Drivers (Protocol Layer)

**Repository**: https://github.com/p1ngb4ck/esp-usb (modified drivers)
**Branch**: `dual-host-support`

**Drivers modified:**
1. **MSC (Mass Storage Class)** - `host/class/msc/usb_host_msc/`
2. **UAC (USB Audio Class)** - `host/class/uac/usb_host_uac/`
3. **CDC-ACM (Communication Device Class)** - `host/class/cdc/usb_host_cdc_acm/`
4. **HID (Human Interface Device)** - `host/class/hid/usb_host_hid/`
5. **UVC (USB Video Class)** - `host/class/uvc/usb_host_uvc/`

**What was changed:**
- Converted from singleton pattern to instance-based pattern
- Each driver now uses `*_host_driver_handle_t` that encapsulates TinyUSB instance
- Driver initialization functions return handles instead of using global state
- All device operations require driver handle parameter

**Example: MSC Driver Changes**
```c
// Before (Singleton API):
esp_err_t msc_host_install(const msc_host_driver_config_t *config);
esp_err_t msc_host_install_device(uint8_t dev_addr, msc_host_device_handle_t *msc_dev_handle);

// After (Multi-Instance API):
msc_host_driver_handle_t msc_host_driver_init(tuh_instance_t usb_inst, const msc_host_driver_config_t *config);
esp_err_t msc_host_driver_install_device(msc_host_driver_handle_t driver, uint8_t dev_addr, msc_host_device_handle_t *msc_dev_handle);
```

**Why this is critical:**
- Class drivers sit between TinyUSB and applications
- They MUST propagate the TinyUSB instance handle up to applications
- Without this, applications cannot specify which controller to use
- Maintains backward compatibility by keeping original singleton API alongside new multi-instance API

**Pattern used in all drivers:**
1. Store TinyUSB instance handle in driver structure
2. Pass driver handle to all API functions
3. Forward instance handle to TinyUSB calls
4. Support both singleton and multi-instance APIs via conditional compilation

### Phase 3: ESPHome Integration (Application Layer)

**Repository**: https://github.com/esphome/esphome (ESPHome core)

**Components modified:**
1. **usb_host** - Base USB Host infrastructure
2. **usb_storage** - Mass Storage devices
3. **usb_audio** - Audio devices (speakers/microphones)
4. **usb_uart** - Serial UART adapters (no changes needed)

#### 3.1: usb_host Configuration

**File**: `esphome/components/usb_host/__init__.py`

**Added configuration options:**
```yaml
usb_host:
  dual_host_support: true  # ESP32-P4 only
  instances:
    - id: usb_fs
      controller: fs  # Full-Speed controller (USB0)
    - id: usb_hs
      controller: hs  # High-Speed controller (USB1)
```

**What happens:**
- Sets `USE_USB_HOST_DUAL_INSTANCE` define when dual_host_support is enabled
- Stores configuration in `CORE.data` for other components to access
- Creates separate USBHost component instances for each controller

#### 3.2: usb_storage Updates

**Files modified:**
- `esphome/components/usb_storage/__init__.py` - Conditional IDF component loading
- `esphome/components/usb_storage/usb_storage.h` - Add driver handle field
- `esphome/components/usb_storage/usb_storage.cpp` - Implement dual API support

**Python changes:**
```python
# Conditional IDF component loading based on dual_host_support
dual_host_support = CORE.data.get("usb_host_dual_instance", False)
if dual_host_support:
    # Load modified multi-instance MSC driver
    add_idf_component(
        name="usb_host_msc",
        repo="https://github.com/p1ngb4ck/esp-usb.git",
        ref="dual-host-support",
        path="host/class/msc/usb_host_msc",
    )
else:
    # Load original singleton MSC driver
    add_idf_component(name="espressif/usb_host_msc", ref="1.1.4")
```

**C++ changes:**
```cpp
// Header: Add driver handle field
class USBStorageHost : public Component {
#ifdef USE_USB_HOST_DUAL_INSTANCE
  msc_host_driver_handle_t msc_driver_{nullptr};
#endif
};

// Implementation: Support both APIs
#ifdef USE_USB_HOST_DUAL_INSTANCE
  // Multi-instance API
  this->msc_driver_ = msc_host_driver_init(nullptr, &msc_config);
  // ... device operations with driver handle
  msc_host_driver_install_device(this->msc_driver_, addr, &handle);
#else
  // Singleton API
  esp_err_t err = msc_host_install(&msc_config);
  // ... device operations without driver handle
  msc_host_install_device(addr, &handle);
#endif
```

#### 3.3: usb_audio Updates

**Files modified:**
- `esphome/components/usb_audio/__init__.py` - Conditional IDF component loading
- `esphome/components/usb_audio/usb_audio.h` - Add driver handle field
- `esphome/components/usb_audio/usb_audio.cpp` - Implement dual API support

**Same pattern as usb_storage:**
- Conditional IDF component loading (p1ngb4ck/esp-usb vs espressif/usb_host_uac)
- `#ifdef USE_USB_HOST_DUAL_INSTANCE` guards for driver handle
- Support both singleton and multi-instance APIs

#### 3.4: usb_uart - No Changes Needed

**Why**: usb_uart uses the USBClient system (VID/PID based) which operates at a lower level than ESP-IDF class drivers. It implements USB protocols directly using USB transfer APIs from usb_host, not ESP-IDF managed components. Dual instance support will come from usb_host infrastructure itself.

---

## Design Principles

### 1. Backward Compatibility

**Goal**: ESP32-S2 and ESP32-S3 continue to work without any changes

**Implementation**:
- All dual-instance code is guarded by `#ifdef USE_USB_HOST_DUAL_INSTANCE`
- Define is only set on ESP32-P4 when `dual_host_support: true` is configured
- Conditional IDF component loading ensures correct driver versions are used
- No runtime overhead for single-instance platforms

### 2. Clean Layer Separation

**Each layer has a clear responsibility:**

| Layer | Responsibility | Instance Handle Type |
|-------|----------------|---------------------|
| TinyUSB | Hardware abstraction | `tuh_instance_t` |
| ESP-IDF Drivers | USB protocol implementation | `*_host_driver_handle_t` |
| ESPHome | User-friendly YAML configuration | Config-based selection |

**Handle propagation:**
```
YAML Config → Python Codegen → C++ Component → ESP-IDF Driver → TinyUSB → Hardware
```

### 3. Fail-Safe Instance Selection

**Current implementation** (TODO markers in code):
```cpp
// Temporarily passing nullptr - needs proper instance selection
this->msc_driver_ = msc_host_driver_init(nullptr, &msc_config);
```

**Future implementation**:
- ESPHome components will need a `usb_host_id` configuration option
- Python codegen will pass correct USBHost instance based on config
- C++ components will store and use the correct instance handle

---

## Why This Approach is Optimal

### Alternative Approaches (and why they wouldn't work):

❌ **Start at ESPHome level**: Can't work - ESPHome depends on ESP-IDF drivers which depend on TinyUSB

❌ **Only modify ESP-IDF drivers**: Can't work - TinyUSB underneath is still singleton

❌ **Fork entire ESP-IDF**: Too invasive - targeted approach is much cleaner and more maintainable

### Benefits of This Approach:

✅ **Correct layer ordering**: Started at the lowest layer (TinyUSB) and worked upward - the ONLY correct way

✅ **Backward compatibility**: Existing platforms (ESP32-S2, ESP32-S3) continue to work without changes

✅ **Clean abstraction**: Each layer properly abstracts the instance concept

✅ **Separation of concerns**: TinyUSB handles hardware, ESP-IDF handles protocols, ESPHome handles configuration

✅ **Maintainable**: Changes are localized to specific components with clear boundaries

✅ **Upstreamable**: Clean enough to potentially be contributed back to official repositories

---

## Use Cases

### Example Configuration

```yaml
esp32:
  board: esp32-p4-function-ev-board
  variant: esp32p4
  framework:
    type: esp-idf

# Enable dual USB Host support
usb_host:
  dual_host_support: true
  instances:
    - id: usb_fs_host
      controller: fs  # Full-Speed controller (USB0)
    - id: usb_hs_host
      controller: hs  # High-Speed controller (USB1)

# USB Audio on Full-Speed controller
usb_audio:
  id: audio_device
  connect_timeout: 5s
  # TODO: Add usb_host_id: usb_fs_host

# USB Storage on High-Speed controller
usb_storage:
  id: storage_host
  # TODO: Add usb_host_id: usb_hs_host
  devices:
    - id: usb_thumb_drive
      mount_path: /usb
```

### Typical Device Assignment by Speed

**Full-Speed Controller (USB0):**
- USB Audio interfaces (microphones, speakers, audio adapters)
- USB HID devices (keyboards, mice, gamepads)
- Low-speed storage devices
- USB serial adapters (FTDI, CH340, CP210x)

**High-Speed Controller (USB1):**
- USB Video cameras (UVC)
- High-speed flash drives
- USB 2.0 storage devices requiring fast transfers

---

## Testing Status

| Component | Singleton API | Multi-Instance API | Status |
|-----------|--------------|-------------------|---------|
| TinyUSB Core | ✅ Working | 🔧 Implemented | Needs hardware testing |
| MSC Driver | ✅ Working | 🔧 Implemented | Needs hardware testing |
| UAC Driver | ✅ Working | 🔧 Implemented | Needs hardware testing |
| CDC-ACM Driver | ✅ Working | 🔧 Implemented | Needs hardware testing |
| HID Driver | ✅ Working | 🔧 Implemented | Needs hardware testing |
| UVC Driver | ✅ Working | 🔧 Implemented | Needs hardware testing |
| ESPHome usb_storage | ✅ Working | 🔧 Implemented | Needs hardware testing |
| ESPHome usb_audio | ✅ Working | 🔧 Implemented | Needs hardware testing |
| ESPHome usb_uart | ✅ Working | ⚪ No changes needed | Needs hardware testing |

---

## Future Work

### Phase 4: Instance Handle Wiring (TODO)

**Current state**: All multi-instance driver initialization calls pass `nullptr` for instance handle

**What needs to be done:**

1. **Add `usb_host_id` configuration to USB components**:
```yaml
usb_storage:
  usb_host_id: usb_hs_host  # Explicitly bind to HS controller
  devices:
    - id: fast_storage
      mount_path: /usb
```

2. **Update Python codegen to pass instance handle**:
```python
# Get the correct USBHost instance from config
if dual_host_support and CONF_USB_HOST_ID in config:
    usb_host_var = await cg.get_variable(config[CONF_USB_HOST_ID])
    # Pass to C++ component for driver initialization
```

3. **Update C++ components to use instance handle**:
```cpp
// Instead of nullptr, use actual instance
this->msc_driver_ = msc_host_driver_init(this->usb_host_->get_tuh_instance(), &msc_config);
```

### Phase 5: Hardware Validation

**Required testing:**
- [ ] ESP32-P4 with FS device on USB0 and HS device on USB1 simultaneously
- [ ] USB Audio on USB0 + USB Storage on USB1
- [ ] USB HID on USB0 + USB Video on USB1
- [ ] Performance benchmarking of dual-controller operation
- [ ] Stress testing with high data rates on both controllers

### Phase 6: Upstream Contribution

**Potential upstreaming targets:**
1. **TinyUSB**: Multi-instance support for USB Host
2. **ESP-IDF (esp-usb)**: Multi-instance class drivers
3. **ESPHome**: Dual USB Host support for ESP32-P4

**Requirements before upstreaming:**
- Complete hardware validation
- Comprehensive testing across all supported platforms
- Documentation and examples
- Code review and refactoring as needed

---

## References

### Repositories

- **TinyUSB Upstream**: https://github.com/hathach/tinyusb
- **ESP-IDF Fork (TinyUSB)**: https://github.com/espressif/esp-idf (components/tinyusb)
- **ESP-USB (Modified Drivers)**: https://github.com/p1ngb4ck/esp-usb (branch: dual-host-support)
- **ESPHome**: https://github.com/esphome/esphome

### Technical Documentation

- ESP32-P4 Technical Reference Manual - USB OTG Controllers
- USB 2.0 Specification - Speed Classes
- TinyUSB Documentation - Host Mode
- ESP-IDF USB Host Programming Guide

---

## Summary

This implementation enables ESP32-P4 to use both USB OTG controllers simultaneously, working around the hardware limitation that prevents speed translation. The three-phase approach (TinyUSB → ESP-IDF drivers → ESPHome) follows proper software architecture principles and maintains backward compatibility while enabling new functionality for ESP32-P4.

The work is architecturally sound, properly layered, and ready for hardware testing and eventual upstream contribution.

---

**Document Version**: 1.0
**Last Updated**: 2025-01-26
**Author**: p1ngb4ck
**License**: Same as respective component repositories
