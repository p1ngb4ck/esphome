# ESP-IDF USB Class Driver Modifications for Multi-Instance Support

## Current State

All ESP-IDF USB class drivers (MSC, UAC, CDC, etc.) are currently **singletons** that assume only one USB Host controller exists in the system.

## Files Downloaded

Repository: `esp-usb` (https://github.com/espressif/esp-usb)
Location: `/workspaces/esphome/reference/esp-idf-usb-components/esp-usb/`

### Class Drivers Found:
1. **MSC** (Mass Storage Class) - `host/class/msc/usb_host_msc/`
   - Used by ESPHome's `usb_storage` component
   - Version referenced: 1.1.4

2. **UAC** (USB Audio Class) - `host/class/uac/usb_host_uac/`
   - Used by ESPHome's `usb_audio` component
   - Version referenced: 1.3.1

3. **CDC-ACM** (Serial Communication) - `host/class/cdc/usb_host_cdc_acm/`
   - Used by ESPHome's `usb_uart` component (if applicable)

4. **HID** (Human Interface Device) - `host/class/hid/usb_host_hid/`
5. **UVC** (USB Video Class) - `host/class/uvc/usb_host_uvc/`

## Problem Pattern Found in MSC Driver

### Current Singleton Pattern
```c
// In msc_host.c
static msc_driver_t *s_msc_driver = NULL;  // GLOBAL SINGLETON

esp_err_t msc_host_install(const msc_host_driver_config_t *config) {
    MSC_RETURN_ON_FALSE(!s_msc_driver, ESP_ERR_INVALID_STATE);  // Only one instance allowed

    s_msc_driver = calloc(1, sizeof(msc_driver_t));
    // ... register with USB Host (single controller assumed)
}

esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *handle) {
    MSC_GOTO_ON_FALSE_CRITICAL(s_msc_driver, ESP_ERR_INVALID_STATE);  // Uses global
    // ... uses s_msc_driver->client_handle
}
```

### Issue for Dual USB Host
- **Cannot support two USB controllers simultaneously**
- A USB drive on FS controller and another on HS controller would conflict
- The second `msc_host_install()` call would fail with `ESP_ERR_INVALID_STATE`

## Required Modifications

### Pattern to Implement

Following our TinyUSB multi-instance pattern:

```c
// New API in msc_host.h
typedef struct msc_host_driver *msc_host_driver_handle_t;

// Instance-based initialization (replaces msc_host_install)
msc_host_driver_handle_t msc_host_driver_init(
    tuh_instance_t usb_host_inst,  // Which USB controller
    const msc_host_driver_config_t *config
);

// Instance-based cleanup
esp_err_t msc_host_driver_deinit(msc_host_driver_handle_t driver);

// Device operations now need driver handle
esp_err_t msc_host_install_device(
    msc_host_driver_handle_t driver,  // Which MSC driver instance
    uint8_t device_address,
    msc_host_device_handle_t *handle
);

// Legacy API for backward compatibility (uses default instance)
#define msc_host_install(config) \
    msc_host_driver_init(tuh_get_default_instance(), config)
```

### Implementation Changes Needed

1. **Remove global singleton**:
   ```c
   // OLD
   static msc_driver_t *s_msc_driver = NULL;

   // NEW - allow multiple instances
   // Each instance associated with a tuh_instance_t
   ```

2. **Store TinyUSB instance handle**:
   ```c
   struct msc_driver {
       tuh_instance_t usb_host_inst;  // Which USB controller this driver uses
       usb_host_client_handle_t client_handle;
       // ... rest of driver state
   };
   ```

3. **Register with specific USB controller**:
   ```c
   msc_host_driver_handle_t msc_host_driver_init(
       tuh_instance_t usb_host_inst,
       const msc_host_driver_config_t *config
   ) {
       msc_driver_t *driver = calloc(1, sizeof(msc_driver_t));
       driver->usb_host_inst = usb_host_inst;

       // Register with this specific USB Host instance
       usb_host_client_register_instance(usb_host_inst, &client_config, &driver->client_handle);

       return driver;
   }
   ```

4. **All device operations accept driver handle**:
   ```c
   esp_err_t msc_host_install_device(
       msc_host_driver_handle_t driver,  // Not global anymore
       uint8_t device_address,
       msc_host_device_handle_t *handle
   ) {
       // Use driver->client_handle instead of s_msc_driver->client_handle
       usb_host_device_open(driver->client_handle, device_address, ...);
   }
   ```

## Files That Need Modification

### MSC Driver
- `host/class/msc/usb_host_msc/include/usb/msc_host.h` - API changes
- `host/class/msc/usb_host_msc/src/msc_host.c` - Implementation

### UAC Driver
- `host/class/uac/usb_host_uac/include/usb/uac_host.h` - API changes
- `host/class/uac/usb_host_uac/uac_host.c` - Implementation

### Similar Pattern for Other Drivers
- CDC-ACM
- HID
- UVC

## Integration with ESPHome

After modifying ESP-IDF components, ESPHome components need updates:

### usb_storage component
```python
# In __init__.py, instead of:
add_idf_component(name="espressif/usb_host_msc", ref="1.1.4")

# Use our modified version:
add_idf_component(name="p1ngb4ck/usb_host_msc", ref="dual-host-support")
```

### C++ code
```cpp
// OLD (single instance)
msc_host_install(&config);

// NEW (specify which USB controller)
auto usb_inst = storage::global_storage->get_usb_instance();  // FS or HS
msc_host_driver_init(usb_inst, &config);
```

## Testing Strategy

1. Modify MSC driver first (most commonly used)
2. Test with single USB controller (backward compatibility)
3. Test with dual controllers (ESP32-P4 FS + HS)
4. Verify no device conflicts between controllers
5. Repeat for UAC and other drivers

## Completion Status

1. ✅ Downloaded ESP-USB repository
2. ✅ Analyzed all driver structures
3. ✅ Modified MSC driver for multi-instance support
4. ✅ Modified UAC driver for multi-instance support
5. ✅ Modified CDC-ACM driver for multi-instance support
6. ✅ Modified HID driver for multi-instance support
7. ✅ Modified UVC driver for multi-instance support
8. ⏳ Test modifications on hardware
9. ⏳ Update ESPHome components to use modified drivers
10. ⏳ Create GitHub repositories for modified drivers

## All Drivers Converted ✅

All ESP-IDF USB class drivers have been successfully converted from singleton to multi-instance pattern:

### ✅ MSC (Mass Storage Class)
- **Location:** `host/class/msc/usb_host_msc/`
- **New API:** `msc_host_driver_init()`, `msc_host_driver_deinit()`, `msc_host_driver_install_device()`, `msc_host_driver_handle_events()`
- **Pattern:** Driver handle + device back-reference via `device->driver`
- **Used by:** ESPHome `usb_storage` component (v1.1.4)
- **Status:** Complete - all singleton code removed

### ✅ UAC (USB Audio Class)
- **Location:** `host/class/uac/usb_host_uac/`
- **New API:** `uac_host_driver_init()`, `uac_host_driver_deinit()`, `uac_host_driver_device_open()`, `uac_host_driver_handle_events()`
- **Pattern:** Driver handle + device/interface back-reference
- **Used by:** ESPHome `usb_audio` component (v1.3.1)
- **Status:** Complete - all singleton code removed

### ✅ CDC-ACM (USB Serial)
- **Location:** `host/class/cdc/usb_host_cdc_acm/`
- **New API:** `cdc_acm_host_driver_init()`, `cdc_acm_host_driver_deinit()`, `cdc_acm_host_driver_open()`, `cdc_acm_host_driver_handle_events()`
- **Pattern:** Driver handle + device back-reference
- **Used by:** ESPHome `usb_uart` component (if applicable)
- **Status:** Complete - all singleton code removed, C++ wrapper class removed

### ✅ HID (Human Interface Device)
- **Location:** `host/class/hid/usb_host_hid/`
- **New API:** `hid_host_driver_init()`, `hid_host_driver_deinit()`, `hid_host_driver_handle_events()`
- **Pattern:** Driver handle + device/interface back-reference via `parent->driver`
- **Used by:** Potential ESPHome HID components
- **Status:** Complete - all singleton code removed

### ✅ UVC (USB Video Class)
- **Location:** `host/class/uvc/usb_host_uvc/`
- **New API:** `uvc_host_install_ex()`, `uvc_host_uninstall_ex()`, `uvc_host_stream_open_ex()`, `uvc_host_handle_events_ex()`
- **Pattern:** Driver handle + stream back-reference
- **Used by:** Potential ESPHome camera components
- **Status:** Complete - all singleton code removed

### Additional CDC Variants (No Changes Needed)
The following CDC variants are thin wrappers around CDC-ACM and inherit multi-instance support:
- `usb_host_ch34x_vcp` - CH340/CH341 USB-to-serial chips
- `usb_host_cp210x_vcp` - CP210x USB-to-UART bridge
- `usb_host_ftdi_vcp` - FTDI USB-to-serial chips
- `usb_host_vcp` - Generic VCP wrapper
