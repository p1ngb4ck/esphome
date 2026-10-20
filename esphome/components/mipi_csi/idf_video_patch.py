"""esp_video without its USB UVC device (Olli 2026-10-20: "get rid of his uvc" - "just remove it entirely").

USB cameras are this fork's usb_uvc's (on usb_host); esp_video's own UVC device and the usb_host_uvc component it
pulls in on the P4 (its manifest, unconditionally) leave the build: the manifest dependency, the CMake source
block, the Kconfig menu and the source file. Its remaining references sit under
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE, a symbol that no longer exists.

Exact-string patches (as idf_usb_patch.py): a changed upstream line fails the build naming the file.
"""

from __future__ import annotations

import hashlib

ESP_VIDEO_VERSION = "2.5.0"
ESP_VIDEO_URL = (
    "https://components-file.espressif.com/components/espressif/esp_video/"
    f"{ESP_VIDEO_VERSION}/espressif__esp_video-v{ESP_VIDEO_VERSION}.zip"
)

# Files removed from the copy (the UVC device and its example)
REMOVED = ("src/device/esp_video_usb_uvc_device.c", "examples/uvc")

PATCHES = (
    ("idf_component.yml", '  usb_host_uvc:\n    rules:\n    - if: target in [esp32p4, esp32s3, esp32s31]\n    version: 2.5.*\n', ""),
    ("CMakeLists.txt", 'if(CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE)\n    list(APPEND srcs "src/device/esp_video_usb_uvc_device.c")\nendif()\n\n', ""),
    ("Kconfig", '    menuconfig ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE\n        bool "Enable USB-UVC based Video Device"\n        depends on SOC_USB_OTG_SUPPORTED\n        default n\n        help\n            Enable USB Video Class (UVC) video device support.\n\n            UVC is a standard USB device class that provides a universal\n            interface for video devices like USB cameras, webcams, and\n            video capture devices.\n\n            Features:\n            - Plug-and-play compatibility with standard USB cameras\n            - No driver installation required for UVC-compliant devices\n            - Support for multiple video formats and resolutions\n            - Standard USB power delivery for connected devices\n            - Hot-plug detection and device enumeration\n\n            Supported device types:\n            - USB webcams and external cameras\n            - USB video capture devices\n            - Professional USB cameras\n            - Streaming devices with UVC support\n\n            Best for: Applications requiring external camera connectivity,\n            surveillance systems with USB cameras, or scenarios where\n            flexibility in camera selection is important.\n\n            Note: Requires USB OTG host functionality and sufficient\n            power supply for connected USB devices.\n\n\n    if ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE\n\n        config USB_UVC_VIDEO_DEVICE_URB_SIZE\n            int "USB UVC Video Device URB Size"\n            default 10240\n            help\n                Size in bytes of 1 URB, 10kB should be enough for start.\n                Larger value results in less frequent interrupts at the\n                cost of memory consumption\n\n        config USB_UVC_INIT_TIMEOUT_MS\n            int "UVC device initialization timeout (ms)"\n            default 10000\n            range 500 60000\n            help\n                Timeout in milliseconds for waiting for UVC device enumeration.\n                Default is 10000ms (10 seconds).\n    endif\n', ""),
)

PATCH_STAMP = hashlib.sha256(
    repr((ESP_VIDEO_URL, REMOVED, PATCHES)).encode()
).hexdigest()
