"""Names of HID keys and buttons, by (usage page, usage).

Keyboard keys follow the Linux key names without KEY_; consumer controls (remote and media
keys) carry the names of their HID usages. The one table is emitted into the firmware too,
so a name means the same in YAML and in on_key.
"""

from __future__ import annotations

import re

import esphome.config_validation as cv

KEYBOARD = 0x07
CONSUMER = 0x0C
BUTTON = 0x09
GENERIC_DESKTOP = 0x01


def _keyboard() -> dict[str, int]:
    names: dict[str, int] = {}
    for i, letter in enumerate("ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
        names[letter] = 0x04 + i
    for i, digit in enumerate("1234567890"):
        names[digit] = 0x1E + i
    names.update(
        {
            "ENTER": 0x28,
            "ESCAPE": 0x29,
            "BACKSPACE": 0x2A,
            "TAB": 0x2B,
            "SPACE": 0x2C,
            "MINUS": 0x2D,
            "EQUAL": 0x2E,
            "LEFT_BRACE": 0x2F,
            "RIGHT_BRACE": 0x30,
            "BACKSLASH": 0x31,
            "NON_US_HASH": 0x32,
            "SEMICOLON": 0x33,
            "APOSTROPHE": 0x34,
            "GRAVE": 0x35,
            "COMMA": 0x36,
            "DOT": 0x37,
            "SLASH": 0x38,
            "CAPS_LOCK": 0x39,
            "PRINT_SCREEN": 0x46,
            "SCROLL_LOCK": 0x47,
            "PAUSE": 0x48,
            "INSERT": 0x49,
            "HOME": 0x4A,
            "PAGE_UP": 0x4B,
            "DELETE": 0x4C,
            "END": 0x4D,
            "PAGE_DOWN": 0x4E,
            "RIGHT": 0x4F,
            "LEFT": 0x50,
            "DOWN": 0x51,
            "UP": 0x52,
            "NUM_LOCK": 0x53,
            "KP_SLASH": 0x54,
            "KP_ASTERISK": 0x55,
            "KP_MINUS": 0x56,
            "KP_PLUS": 0x57,
            "KP_ENTER": 0x58,
            "KP_0": 0x62,
            "KP_DOT": 0x63,
            "NON_US_BACKSLASH": 0x64,
            "COMPOSE": 0x65,
            "KEYBOARD_POWER": 0x66,
            "KP_EQUAL": 0x67,
            "RO": 0x87,
            "KATAKANA_HIRAGANA": 0x88,
            "YEN": 0x89,
            "HENKAN": 0x8A,
            "MUHENKAN": 0x8B,
            "LEFT_CTRL": 0xE0,
            "LEFT_SHIFT": 0xE1,
            "LEFT_ALT": 0xE2,
            "LEFT_META": 0xE3,
            "RIGHT_CTRL": 0xE4,
            "RIGHT_SHIFT": 0xE5,
            "RIGHT_ALT": 0xE6,
            "RIGHT_META": 0xE7,
        }
    )
    for i in range(12):
        names[f"F{i + 1}"] = 0x3A + i
    for i in range(12):
        names[f"F{i + 13}"] = 0x68 + i
    for i in range(9):
        names[f"KP_{i + 1}"] = 0x59 + i
    return names


CONSUMER_NAMES = {
    "POWER": 0x30,
    "SLEEP": 0x32,
    "DATA_ON_SCREEN": 0x60,
    "BRIGHTNESS_UP": 0x6F,
    "BRIGHTNESS_DOWN": 0x70,
    "MENU": 0x40,
    "SELECT": 0x41,
    "MENU_UP": 0x42,
    "MENU_DOWN": 0x43,
    "MENU_LEFT": 0x44,
    "MENU_RIGHT": 0x45,
    "MENU_ESCAPE": 0x46,
    "CHANNEL_UP": 0x9C,
    "CHANNEL_DOWN": 0x9D,
    "MEDIA_PLAY": 0xB0,
    "MEDIA_PAUSE": 0xB1,
    "MEDIA_RECORD": 0xB2,
    "MEDIA_FAST_FORWARD": 0xB3,
    "MEDIA_REWIND": 0xB4,
    "MEDIA_NEXT": 0xB5,
    "MEDIA_PREVIOUS": 0xB6,
    "MEDIA_STOP": 0xB7,
    "MEDIA_EJECT": 0xB8,
    "MEDIA_PLAY_PAUSE": 0xCD,
    "VOICE_COMMAND": 0xCF,
    "MUTE": 0xE2,
    "VOLUME_UP": 0xE9,
    "VOLUME_DOWN": 0xEA,
    "AL_CONFIG": 0x183,
    "AL_EMAIL": 0x18A,
    "AL_CALCULATOR": 0x192,
    "AL_FILE_BROWSER": 0x194,
    "AL_WWW": 0x196,
    "AC_SEARCH": 0x221,
    "AC_HOME": 0x223,
    "AC_BACK": 0x224,
    "AC_FORWARD": 0x225,
    "AC_STOP": 0x226,
    "AC_REFRESH": 0x227,
    "AC_BOOKMARKS": 0x22A,
    "AC_ZOOM_IN": 0x22D,
    "AC_ZOOM_OUT": 0x22E,
}

SYSTEM_NAMES = {
    "SYSTEM_POWER": 0x81,
    "SYSTEM_SLEEP": 0x82,
    "SYSTEM_WAKE": 0x83,
    # Controls rather than keys: what on_control names them by.
    "AXIS_X": 0x30,
    "AXIS_Y": 0x31,
    "AXIS_Z": 0x32,
    "AXIS_RX": 0x33,
    "AXIS_RY": 0x34,
    "AXIS_RZ": 0x35,
    "SLIDER": 0x36,
    "DIAL": 0x37,
    "WHEEL": 0x38,
    "HAT_SWITCH": 0x39,
}

KEY_NAMES: dict[str, tuple[int, int]] = {
    **{name: (KEYBOARD, usage) for name, usage in _keyboard().items()},
    **{name: (CONSUMER, usage) for name, usage in CONSUMER_NAMES.items()},
    **{name: (GENERIC_DESKTOP, usage) for name, usage in SYSTEM_NAMES.items()},
    **{f"BUTTON_{i}": (BUTTON, i) for i in range(1, 33)},
}

# Mouse buttons by what they are called on a mouse.
ALIASES = {
    "MOUSE_LEFT": "BUTTON_1",
    "MOUSE_RIGHT": "BUTTON_2",
    "MOUSE_MIDDLE": "BUTTON_3",
}


def validate_key(value) -> tuple[int, int]:
    """A key by name, by "page:usage", or by keyboard usage number."""
    if isinstance(value, int):
        return (KEYBOARD, cv.int_range(min=0, max=0xFFFF)(value))
    value = cv.string_strict(value).strip()
    name = value.upper()
    name = ALIASES.get(name, name)
    if name in KEY_NAMES:
        return KEY_NAMES[name]
    if m := re.fullmatch(r"(0x[0-9a-fA-F]+|\d+):(0x[0-9a-fA-F]+|\d+)", value):
        return (int(m.group(1), 0), int(m.group(2), 0))
    raise cv.Invalid(
        f"Unknown key '{value}': use a key name such as ENTER or VOLUME_UP, or page:usage"
    )
