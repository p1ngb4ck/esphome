#pragma once

// USB HID class constants
static constexpr uint8_t USB_CLASS_HID = 0x03;

// HID interface subclass
static constexpr uint8_t HID_SC_NONE = 0x00;
static constexpr uint8_t HID_SC_BOOT = 0x01;

// HID interface protocol (boot protocol)
static constexpr uint8_t HID_PROTO_NONE = 0x00;
static constexpr uint8_t HID_PROTO_KEYBOARD = 0x01;
static constexpr uint8_t HID_PROTO_MOUSE = 0x02;

// HID class-specific requests
static constexpr uint8_t HID_REQ_GET_REPORT = 0x01;
static constexpr uint8_t HID_REQ_GET_IDLE = 0x02;
static constexpr uint8_t HID_REQ_GET_PROTOCOL = 0x03;
static constexpr uint8_t HID_REQ_SET_REPORT = 0x09;
static constexpr uint8_t HID_REQ_SET_IDLE = 0x0A;
static constexpr uint8_t HID_REQ_SET_PROTOCOL = 0x0B;

// HID report types
static constexpr uint8_t HID_REPORT_TYPE_INPUT = 0x01;
static constexpr uint8_t HID_REPORT_TYPE_OUTPUT = 0x02;
static constexpr uint8_t HID_REPORT_TYPE_FEATURE = 0x03;

// bmRequestType values for HID
static constexpr uint8_t HID_REQ_TYPE_HOST_TO_DEV = 0x21;  // Class | Interface | Host-to-device
static constexpr uint8_t HID_REQ_TYPE_DEV_TO_HOST = 0xA1;  // Class | Interface | Device-to-host

// Keyboard LED report bits
static constexpr uint8_t HID_KB_LED_NUM_LOCK = 0x01;
static constexpr uint8_t HID_KB_LED_CAPS_LOCK = 0x02;
static constexpr uint8_t HID_KB_LED_SCROLL_LOCK = 0x04;

// Xbox 360 VID/PID list (vendor-specific class, not HID 0x03)
static constexpr uint16_t XBOX360_VID = 0x045E;
static constexpr uint16_t XBOX360_PID_WIRED = 0x028E;
static constexpr uint16_t XBOX360_PID_WIRELESS = 0x0719;
static constexpr uint16_t BITDO_VID = 0x2DC8;
static constexpr uint16_t BITDO_PID_XBOX = 0x310B;

// Endpoint direction
static constexpr uint8_t EP_DIR_IN = 0x80;
static constexpr uint8_t EP_DIR_MASK = 0x80;

// Endpoint transfer type
static constexpr uint8_t EP_TYPE_MASK = 0x03;
static constexpr uint8_t EP_TYPE_INTERRUPT = 0x03;

// Maximum number of HID devices we support simultaneously
static constexpr uint8_t HID_MAX_DEVICES = 4;

// Media interface marker byte (prepended to reports from media/secondary interface)
static constexpr uint8_t HID_MEDIA_REPORT_MARKER = 0xFF;
