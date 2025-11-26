# ESP-USB Multi-Instance Pattern

This document describes the pattern for converting ESP-IDF USB class drivers from singleton to multi-instance for dual USB Host support.

## Pattern Applied to MSC Driver

### 1. Header Changes (`include/usb/xxx_host.h`)

**Add TinyUSB instance forward declaration:**
```c
#ifndef TUH_INSTANCE_T_DEFINED
#define TUH_INSTANCE_T_DEFINED
typedef struct usbh_instance* tuh_instance_t;
#endif

typedef struct xxx_host_driver *xxx_host_driver_handle_t;
```

**Replace singleton API with instance API:**
```c
// OLD (remove):
esp_err_t xxx_host_install(const xxx_host_driver_config_t *config);
esp_err_t xxx_host_uninstall(void);
esp_err_t xxx_host_handle_events(uint32_t timeout);

// NEW (add):
xxx_host_driver_handle_t xxx_host_driver_init(tuh_instance_t usb_inst, const xxx_host_driver_config_t *config);
esp_err_t xxx_host_driver_deinit(xxx_host_driver_handle_t driver);
esp_err_t xxx_host_driver_handle_events(xxx_host_driver_handle_t driver, uint32_t timeout);
```

**Device operations get driver parameter:**
```c
// OLD:
esp_err_t xxx_host_install_device(uint8_t device_address, xxx_host_device_handle_t *device);

// NEW:
esp_err_t xxx_host_driver_install_device(xxx_host_driver_handle_t driver, uint8_t device_address, xxx_host_device_handle_t *device);
```

### 2. Driver Structure Changes

**Add USB instance to driver structure:**
```c
typedef struct xxx_host_driver {
    tuh_instance_t usb_host_inst;  // NEW
    usb_host_client_handle_t client_handle;
    xxx_host_event_cb_t user_cb;
    void *user_arg;
    // ... rest of fields
} xxx_driver_t;

// Remove global singleton:
// static xxx_driver_t *s_xxx_driver;  // DELETE THIS
```

**Add driver pointer to device structure:**
```c
typedef struct xxx_host_device {
    // ... existing fields
    xxx_driver_t *driver;  // NEW
} xxx_device_t;
```

### 3. Implementation Changes (`src/xxx_host.c`)

**Init function returns handle:**
```c
xxx_host_driver_handle_t xxx_host_driver_init(tuh_instance_t usb_inst, const xxx_host_driver_config_t *config)
{
    xxx_driver_t *driver = calloc(1, sizeof(xxx_driver_t));
    if (!driver) return NULL;

    driver->usb_host_inst = usb_inst;
    driver->user_cb = config->callback;
    driver->user_arg = config->callback_arg;

    usb_host_client_config_t client_config = {
        .async.client_event_callback = client_event_cb,
        .async.callback_arg = driver,  // Pass driver, not NULL
        .max_num_event_msg = 10,
    };

    if (usb_host_client_register(&client_config, &driver->client_handle) != ESP_OK) {
        free(driver);
        return NULL;
    }

    // Initialize device list, semaphores, task, etc.

    return driver;
}
```

**All callbacks receive driver from arg:**
```c
static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    xxx_driver_t *driver = (xxx_driver_t *)arg;  // Get driver from arg

    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (is_xxx_device(driver, event->new_dev.address)) {  // Pass driver
            driver->user_cb(&xxx_event, driver->user_arg);
        }
        break;
    }
}

static void event_handler_task(void *arg)
{
    xxx_driver_t *driver = (xxx_driver_t *)arg;  // Get driver from arg
    while (xxx_host_driver_handle_events(driver, portMAX_DELAY) == ESP_OK) {}
    vTaskDelete(NULL);
}
```

**Device operations use driver handle:**
```c
esp_err_t xxx_host_driver_install_device(xxx_host_driver_handle_t driver, uint8_t device_address, xxx_host_device_handle_t *device_handle)
{
    xxx_driver_t *drv = (xxx_driver_t *)driver;
    xxx_device_t *device = calloc(1, sizeof(xxx_device_t));

    device->driver = drv;  // Link device to driver

    STAILQ_INSERT_TAIL(&drv->devices_tailq, device, tailq_entry);

    usb_host_device_open(drv->client_handle, device_address, &device->handle);

    // ... rest of device initialization

    *device_handle = device;
    return ESP_OK;
}
```

**Device uninstall uses driver from device:**
```c
esp_err_t xxx_host_uninstall_device(xxx_host_device_handle_t device)
{
    xxx_device_t *dev = (xxx_device_t *)device;
    return xxx_deinit_device(dev->driver, dev, false);  // Use dev->driver
}
```

**Control transfers use driver from device:**
```c
esp_err_t xxx_control_transfer(xxx_device_t *device, size_t len)
{
    usb_host_transfer_submit_control(device->driver->client_handle, xfer);  // Use device->driver
    return ESP_OK;
}
```

**Helper functions receive driver parameter:**
```c
// OLD:
static bool is_xxx_device(uint8_t dev_addr) {
    usb_host_device_open(s_xxx_driver->client_handle, dev_addr, &device);  // BAD
}

// NEW:
static bool is_xxx_device(xxx_driver_t *driver, uint8_t dev_addr) {
    usb_host_device_open(driver->client_handle, dev_addr, &device);  // GOOD
}
```

## Application Pattern

**Dual USB Host usage:**
```c
// Initialize TinyUSB instances
tuh_instance_t usb_fs = tuh_instance_init(0, &rh_init_fs);  // Full-Speed controller
tuh_instance_t usb_hs = tuh_instance_init(1, &rh_init_hs);  // High-Speed controller

// Create MSC drivers for each controller
msc_host_driver_handle_t msc_fs = msc_host_driver_init(usb_fs, &msc_config);
msc_host_driver_handle_t msc_hs = msc_host_driver_init(usb_hs, &msc_config);

// When devices connect, install them on the correct driver
msc_host_driver_install_device(msc_fs, fs_device_addr, &fs_device);
msc_host_driver_install_device(msc_hs, hs_device_addr, &hs_device);
```

## Drivers to Convert

1. ✅ **MSC** (Mass Storage) - COMPLETED
2. ⏳ **UAC** (USB Audio) - Same pattern
3. ⏳ **CDC-ACM** (Serial) - Same pattern
4. ⏳ **HID** (Human Interface) - Same pattern
5. ⏳ **UVC** (USB Video) - Same pattern

## Key Principles

1. **No global singletons** - Every driver instance is independent
2. **Driver handle everywhere** - All operations need driver reference
3. **Device-to-driver linkage** - Devices store pointer to their driver
4. **Callback context** - Pass driver through `callback_arg`
5. **No backward compatibility** - Clean break, no legacy API
