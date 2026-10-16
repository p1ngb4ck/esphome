#!/usr/bin/env python3
"""Write every keyboard layout of esphome/components/hid/layouts/ as a file.

    script/export_hid_layouts.py <directory>

<directory> gets one <name>.kbl per layout (hid::parse_layout()) and index.tsv, a line
"name<TAB>description" per layout, sorted by description (those without one last).
"""

from __future__ import annotations

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

# pylint: disable=wrong-import-position
from esphome.components.hid import layouts  # noqa: E402


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    entries = layouts.names()
    for name, _ in entries:
        (out / f"{name}.kbl").write_bytes(layouts.kbl(layouts.resolve(name)[1]))
    lines = [
        f"{name}\t{description}"
        for name, description in sorted(entries, key=lambda e: (not e[1], e[1].casefold(), e[0]))
    ]
    (out / "index.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
