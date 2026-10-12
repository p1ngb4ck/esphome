#!/usr/bin/env python3
"""Generate esphome/components/hid/layouts/ from xkeyboard-config.

    script/generate_hid_layouts.py <xkeyboard-config> <xkbcommon-keysyms.h>

<xkeyboard-config> is a checkout of https://gitlab.freedesktop.org/xkeyboard-config/
xkeyboard-config, <xkbcommon-keysyms.h> is include/xkbcommon/xkbcommon-keysyms.h from
https://github.com/xkbcommon/libxkbcommon.

A HID keyboard usage reaches an XKB key name the way it does on Linux: hid-input.c's
hid_keyboard[] gives the evdev code, keycodes/evdev names the key at evdev code + 8. Every
symbols file becomes one module; its default variant is stored whole, the others as the keys
in which they differ from it. Run ruff format and ruff check --fix on the
directory afterwards.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys
import unicodedata

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

# pylint: disable=wrong-import-position
from esphome.components.hid.layouts import DEAD_KEYS  # noqa: E402

# drivers/hid/hid-input.c, hid_keyboard[]: HID keyboard usage -> evdev key code (0: none).
HID_KEYBOARD = [
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26,
    27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
    72, 73, 82, 83, 86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
    115, 114, 0, 0, 0, 121, 0, 89, 93, 124, 92, 94, 95, 0, 0, 0,
]  # fmt: skip

# The usages a layout decides: letters, digits, punctuation and the ISO and JIS extra keys.
# Enter, Tab, Space and the keypad do not change.
USAGES = [*range(0x04, 0x28), *range(0x2D, 0x39), 0x64, 0x87, 0x89]

# Not keyboard layouts: option fragments, vendor keys and the shared parts others include.
SKIP_FILES = {
    "altwin", "capslock", "caps", "compose", "ctrl", "empty", "eurosign", "fkeys",
    "group", "grp", "hyper", "inet", "keypad", "kpdl", "level2", "level3", "level5",
    "lv2", "lv3", "lv5", "meta", "nbsp", "parens", "pc", "rupeesign", "scrolllock",
    "shift", "srvr_ctrl", "srvrkeys", "terminate", "typo", "grab", "trans", "brai",
    "meson.build", "apl", "bqn", "latin", "olpc",
}  # fmt: skip

LEVELS = 4


def load_keysyms(header: Path) -> dict[str, str | None]:
    """Keysym name -> the character it types, or None."""
    pattern = re.compile(
        r"#define XKB_KEY_(\w+)\s+0x([0-9a-fA-F]+)\s*(?:/\*\s*U\+([0-9A-Fa-f]+))?"
    )
    values: dict[str, int] = {}
    chars: dict[int, str] = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        if not (m := pattern.match(line)):
            continue
        value = int(m.group(2), 16)
        values.setdefault(m.group(1), value)
        if m.group(3) is not None:
            chars.setdefault(value, chr(int(m.group(3), 16)))
    out: dict[str, str | None] = {}
    for name, value in values.items():
        if value in chars:
            out[name] = chars[value]
        elif 0x20 <= value <= 0x7E or 0xA0 <= value <= 0xFF:
            # Latin-1 keysyms are their own code points.
            out[name] = chr(value)
        elif 0x01000100 <= value <= 0x0110FFFF:
            out[name] = chr(value - 0x01000000)
        else:
            # Deprecated aliases carry no comment; their value names the character.
            out[name] = None
    return out


def load_keycodes(xkb: Path) -> dict[int, list[str]]:
    """evdev code -> XKB key names (aliases included)."""
    text = (xkb / "keycodes" / "evdev").read_text(encoding="utf-8")
    names: dict[str, int] = {}
    for m in re.finditer(r"<(\w+)>\s*=\s*(\d+)\s*;", text):
        names.setdefault(m.group(1), int(m.group(2)))
    for m in re.finditer(r"alias\s*<(\w+)>\s*=\s*<(\w+)>\s*;", text):
        if m.group(2) in names:
            names.setdefault(m.group(1), names[m.group(2)])
    by_code: dict[int, list[str]] = {}
    for name, code in names.items():
        by_code.setdefault(code - 8, []).append(name)
    return by_code


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


class Symbols:
    def __init__(self, xkb: Path, keysyms: dict[str, str | None]) -> None:
        self.dir = xkb / "symbols"
        self.keysyms = keysyms
        self.files: dict[str, dict[str, tuple[str, str]]] = {}
        self.defaults: dict[str, str] = {}
        self.cache: dict[tuple[str, str], tuple[str, dict[str, list[str]]]] = {}

    def variants(self, file: str) -> dict[str, tuple[str, str]]:
        """variant -> (body, flags) of one symbols file."""
        if file not in self.files:
            path = self.dir / file
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            found: dict[str, tuple[str, str]] = {}
            pattern = re.compile(
                r"((?:\w+\s+)*)xkb_symbols\s+\"([^\"]+)\"\s*\{", re.DOTALL
            )
            pos = 0
            while m := pattern.search(text, pos):
                depth, i = 1, m.end()
                while depth and i < len(text):
                    depth += {"{": 1, "}": -1}.get(text[i], 0)
                    i += 1
                found[m.group(2)] = (text[m.end() : i - 1], m.group(1))
                if "default" in m.group(1).split() and file not in self.defaults:
                    self.defaults[file] = m.group(2)
                pos = i
            if found and file not in self.defaults:
                self.defaults[file] = next(iter(found))
            self.files[file] = found
        return self.files[file]

    def symbol(self, name: str) -> str | None:
        name = name.strip()
        if name in DEAD_KEYS:
            return name
        if re.fullmatch(r"U[0-9A-Fa-f]{4,6}", name):
            return chr(int(name[1:], 16))
        if re.fullmatch(r"0x[0-9A-Fa-f]+", name):
            value = int(name, 16)
            if 0x01000100 <= value <= 0x0110FFFF:
                return chr(value - 0x01000000)
            return None
        if re.fullmatch(r"\d", name):
            return name
        return self.keysyms.get(name)

    def resolve(
        self, file: str, variant: str | None
    ) -> tuple[str, dict[str, list[str]]]:
        """(description, key name -> characters per level) of file(variant)."""
        variants = self.variants(file)
        variant = variant or self.defaults.get(file)
        if variant not in variants:
            return "", {}
        if (file, variant) in self.cache:
            return self.cache[(file, variant)]
        self.cache[(file, variant)] = ("", {})  # cut include loops
        body, _ = variants[variant]
        keys: dict[str, list[str]] = {}
        description = ""
        statement = re.compile(
            r"(include|augment|override|replace)\s+\"([^\"]+)\""
            r"|name\s*\[\s*group1\s*\]\s*=\s*\"([^\"]*)\""
            r"|(?:(?:replace|override|augment)\s+)?key\s*<(\w+)>\s*\{([^}]*)\}",
            re.IGNORECASE,
        )
        for m in statement.finditer(body):
            if m.group(1):
                for part in re.split(r"[+|]", m.group(2)):
                    if not (
                        pm := re.fullmatch(
                            r"\s*([\w-]+)(?:\(([^)]*)\))?(?::\d)?\s*", part
                        )
                    ):
                        continue
                    if not (self.dir / pm.group(1)).is_file():
                        continue
                    sub_desc, sub_keys = self.resolve(pm.group(1), pm.group(2))
                    for name, levels in sub_keys.items():
                        if m.group(1).lower() == "augment" and name in keys:
                            continue
                        keys[name] = list(levels)
                    description = description or sub_desc
            elif m.group(3) is not None:
                description = m.group(3)
            elif m.group(4):
                group = re.search(
                    r"(?:symbols\s*\[\s*group1\s*\]\s*=\s*)?\[([^\]]*)\]",
                    m.group(5),
                    re.IGNORECASE,
                )
                if group is None:
                    continue
                levels = [self.symbol(s) for s in group.group(1).split(",")][:LEVELS]
                current = keys.get(m.group(4), [""] * LEVELS)
                for i, value in enumerate(levels):
                    current[i] = value or ""
                for i in range(len(levels), LEVELS):
                    current[i] = ""
                keys[m.group(4)] = current
        result = (description, keys)
        self.cache[(file, variant)] = result
        return result


def by_usage(
    keys: dict[str, list[str]], keycodes: dict[int, list[str]]
) -> dict[int, tuple[str, ...]]:
    out: dict[int, tuple[str, ...]] = {}
    for usage in USAGES:
        code = HID_KEYBOARD[usage] if usage < len(HID_KEYBOARD) else 0
        for name in keycodes.get(code, []):
            if name in keys and any(keys[name]):
                out[usage] = tuple(keys[name])
                break
    return out


def write_module(
    out: Path, file: str, default: str, layouts: dict[str, tuple[str, dict]]
) -> None:
    base_desc, base = layouts[default]
    lines = [
        f'"""Keyboard layouts from xkeyboard-config symbols/{file}. Generated by',
        'script/generate_hid_layouts.py; do not edit."""',
        "",
        f"DEFAULT = {default!r}",
        "",
        "# variant: (description, {usage: (level 1, level 2, level 3, level 4)}). Variants",
        "# other than the default list only the keys in which they differ from it.",
        "LAYOUTS = {",
    ]
    for variant, (desc, keys) in layouts.items():
        diff = (
            keys
            if variant == default
            else {u: v for u, v in keys.items() if base.get(u) != v}
        )
        lines.append(f"    {variant!r}: (")
        lines.append(f"        {desc!r},")
        lines.append("        {")
        lines.extend(f"            0x{u:02X}: {v!r}," for u, v in sorted(diff.items()))
        lines.append("        },")
        lines.append("    ),")
    lines.append("}")
    (out / f"{file}.py").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    xkb, header = Path(sys.argv[1]), Path(sys.argv[2])
    out = (
        Path(__file__).resolve().parent.parent
        / "esphome"
        / "components"
        / "hid"
        / "layouts"
    )
    for name in DEAD_KEYS.values():
        unicodedata.lookup(name)  # every dead key names a real character
    keysyms = load_keysyms(header)
    keycodes = load_keycodes(xkb)
    symbols = Symbols(xkb, keysyms)
    for old in out.glob("*.py"):
        if old.name != "__init__.py":
            old.unlink()
    count = 0
    for path in sorted((xkb / "symbols").iterdir()):
        if not path.is_file() or path.name in SKIP_FILES or path.name.endswith("_vndr"):
            continue
        variants = symbols.variants(path.name)
        default = symbols.defaults.get(path.name)
        if default is None:
            continue
        layouts = {}
        for variant in variants:
            desc, keys = symbols.resolve(path.name, variant)
            mapped = by_usage(keys, keycodes)
            if len(mapped) >= 30:
                layouts[variant] = (desc, mapped)
        if default not in layouts:
            continue
        write_module(out, path.name, default, layouts)
        count += len(layouts)
    print(f"{count} layouts")


if __name__ == "__main__":
    main()
