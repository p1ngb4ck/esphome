from __future__ import annotations

from collections.abc import Callable, Iterator
import re

_SKIP = re.compile(
    r'R"(?P<delim>[^(\s"\\]{0,16})\((?:.|\n)*?\)(?P=delim)"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'"
    r"|//[^\n]*"
    r"|/\*(?:.|\n)*?\*/"
    r"|^[ \t]*#[^\n]*",
    re.M,
)
_IDENT = re.compile(r"[A-Za-z_]\w*")
_DECL = re.compile(
    r"^\s*(?:alignas\([^()]*(?:\([^()]*\))?[^()]*\)\s*)?static\s+(?P<type>[^=;{}]*?[\s*&])"
    r"(?P<name>[A-Za-z_]\w*)\s*(?P<array>(?:\[[^\]]*\])*)\s*(?:PROGMEM\s*)?(?:=[^;]*)?;\s*$",
    re.S,
)
_FUNCTION = re.compile(r"^\s*(?:static\s+)?(?:inline\s+)?[\w:<>,*&\s]+?\b(?P<name>[A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const\s*)?\{", re.S)
PSTORAGE = "__pstorage"


def segments(text: str) -> Iterator[tuple[str, bool]]:
    pos = 0
    for match in _SKIP.finditer(text):
        if match.start() > pos:
            yield text[pos : match.start()], True
        yield match.group(0), False
        pos = match.end()
    if pos < len(text):
        yield text[pos:], True


def identifiers(text: str) -> set[str]:
    found: set[str] = set()
    for part, code in segments(text):
        if code:
            found.update(_IDENT.findall(part))
    return found


def storage_id(token: str) -> str | None:
    if not token.endswith(PSTORAGE):
        return None
    parts = token[: -len(PSTORAGE)].split("__", 1)
    return parts[1] if len(parts) == 2 else None


def replace_identifiers(text: str, replace: Callable[[str], str | None]) -> str:
    out: list[str] = []
    for part, code in segments(text):
        if not code:
            out.append(part)
            continue
        pieces: list[str] = []
        pos = 0
        for match in _IDENT.finditer(part):
            before = part[: match.start()].rstrip()
            after = part[match.end() :].lstrip()
            start = match.start()
            if before.endswith("::"):
                qualifier = before[:-2].rstrip()
                if qualifier and (qualifier[-1].isalnum() or qualifier[-1] in "_>"):
                    continue
                start = len(before) - 2
            if before.endswith((".", "->")) or after.startswith("::"):
                continue
            new = replace(match.group(0))
            if new is None:
                continue
            pieces.append(part[pos:start])
            pieces.append(new)
            pos = match.end()
        pieces.append(part[pos:])
        out.append("".join(pieces))
    return "".join(out)


def declaration(text: str) -> re.Match | None:
    if "\n" in text.strip() and "{" in text:
        return None
    return _DECL.match(text)


def defined_name(text: str) -> str | None:
    decl = declaration(text)
    if decl is not None:
        return decl.group("name")
    function = _FUNCTION.match(text)
    return function.group("name") if function is not None else None


def pointer_declaration(text: str, name: str, pointer: str) -> str | None:
    decl = declaration(text)
    if decl is None or decl.group("name") != name:
        return None
    kind = decl.group("type").replace("PROGMEM", "").strip()
    return f"static {kind} (*{pointer}){decl.group('array')};"
