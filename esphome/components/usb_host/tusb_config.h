/*
 * TinyUSB Configuration for ESPHome USB Host
 *
 * This configuration enables all available USB host classes for maximum
 * device compatibility.
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

// MCU type - defined by build system
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// No RTOS for now (ESPHome runs on Arduino/ESP-IDF event loop)
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_ESP32
#endif

// Debug level (0 = no debug, 3 = verbose)
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

// Memory alignment for DMA transfers
#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif

#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// Host Stack Configuration
//--------------------------------------------------------------------

// Enable USB Host stack
#define CFG_TUH_ENABLED 1

// Default max speed (let hardware decide)
#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

#define CFG_TUH_MAX_SPEED BOARD_TUH_MAX_SPEED

// Root hub port (default to 0 for built-in USB controller)
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

//--------------------------------------------------------------------
// Host Driver Configuration
//--------------------------------------------------------------------

// Buffer for enumeration and descriptor reading
#define CFG_TUH_ENUMERATION_BUFSIZE 512

// Maximum number of USB hubs supported
#define CFG_TUH_HUB 1

// Maximum number of devices (excluding hubs)
// 1 hub typically has 4 ports, so allow 4 devices
#define CFG_TUH_DEVICE_MAX (4 * CFG_TUH_HUB + 1)

//--------------------------------------------------------------------
// USB Host Class Drivers - Enable ALL available classes
//--------------------------------------------------------------------

//------------- Mass Storage (USB drives, card readers) -------------//
#define CFG_TUH_MSC 1
#define CFG_TUH_MSC_MAXLUN 4  // Max LUNs per device (typical for card readers)

//------------- HID (keyboards, mice, game controllers) -------------//
#define CFG_TUH_HID (3 * CFG_TUH_DEVICE_MAX)  // Allow multiple HID interfaces per device
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

//------------- CDC (serial devices) -------------//
#define CFG_TUH_CDC 2  // Number of CDC ACM devices

// CDC buffer sizes
#define CFG_TUH_CDC_RX_BUFSIZE 256
#define CFG_TUH_CDC_TX_BUFSIZE 256

// CDC line control on enumeration: DTR + RTS
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM (CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS)

// CDC line coding: 115200 baud, 8N1
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM \
  { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

//------------- CDC Serial Adapters (FTDI, CP210x, CH34x, PL2303) -------------//
#define CFG_TUH_CDC_FTDI 1    // FTDI FT232, FT2232, etc.
#define CFG_TUH_CDC_CP210X 1  // SiLabs CP2102, CP2104, etc.
#define CFG_TUH_CDC_CH34X 1   // WCH CH340, CH341
#define CFG_TUH_CDC_PL2303 1  // Prolific PL2303

//------------- MIDI -------------//
#define CFG_TUH_MIDI 1
#define CFG_TUH_MIDI_RX_BUFSIZE 128
#define CFG_TUH_MIDI_TX_BUFSIZE 128

//------------- Vendor-specific devices -------------//
#define CFG_TUH_VENDOR 1

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
