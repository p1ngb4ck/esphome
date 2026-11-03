from __future__ import annotations

from dataclasses import fields
import json
from pathlib import Path
import re
import time
from typing import Any
import zlib

from esphome.components.esp32 import MBEDTLS_TLS_ROLE_OPTIONS, MbedtlsSdkconfigData, get_esp32_variant
from esphome.components.esp32.const import (
    KEY_COMPONENTS,
    KEY_ESP32,
    KEY_MBEDTLS_SDKCONFIG,
    KEY_SDKCONFIG_OPTIONS,
)
from esphome.const import KEY_CORE, KEY_FRAMEWORK_VERSION, __version__
from esphome.core import CORE
from esphome.core.entity_helpers import _get_pool
import esphome.codegen as cg

from . import cpp

CORE_FILE = "core.json"
NUMBERED = re.compile(r"_\d+$")
KEY_COMPILE_STAMP = "package_manager_compile_stamp"


def compile_stamp() -> int:
    return CORE.data.setdefault(KEY_COMPILE_STAMP, time.time_ns())


def module_abi_version() -> int:
    header = Path(__file__).parent / "module_abi.h"
    match = re.search(r"#define\s+ESPHOME_MODULE_ABI_VERSION\s+(\d+)", header.read_text(encoding="utf-8"))
    return int(match.group(1))


def define_lines() -> list[str]:
    return sorted(define.as_macro for define in CORE.defines)


def sdkconfig_raw() -> dict[str, Any]:
    return dict(sorted(CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS].items()))


def sdkconfig_options() -> dict[str, str]:
    return {key: str(value) for key, value in sdkconfig_raw().items()}


def idf_component_entries() -> dict[str, dict[str, Any]]:
    return {
        str(name): {key: (str(value) if value is not None else None) for key, value in entry.items()}
        for name, entry in CORE.data[KEY_ESP32].get(KEY_COMPONENTS, {}).items()
    }


def mbedtls_requirements() -> list[str]:
    data = CORE.data[KEY_ESP32].get(KEY_MBEDTLS_SDKCONFIG)
    if data is None:
        return []
    names = []
    for item in fields(data):
        if not item.name.endswith("_required"):
            continue
        value = getattr(data, item.name)
        if isinstance(value, set):
            names += [f"{item.name}:{option}" for option in value]
        elif value is True:
            names.append(item.name)
    return sorted(names)


def properties() -> dict[str, Any]:
    return {
        "sdkconfig": {key: value for key, value in sdkconfig_raw().items() if value is not False},
        "idf_components": idf_component_entries(),
        "mbedtls": mbedtls_requirements(),
    }


def apply_properties(wanted: dict[str, Any]) -> list[str]:
    from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option

    current = CORE.data[KEY_ESP32][KEY_SDKCONFIG_OPTIONS]
    applied = []
    for key, value in wanted.get("sdkconfig", {}).items():
        if key in MBEDTLS_TLS_ROLE_OPTIONS or current.get(key) == value:
            continue
        old = current.get(key)
        if isinstance(value, int) and not isinstance(value, bool) and isinstance(old, int):
            value = max(value, old)
        add_idf_sdkconfig_option(key, value)
        applied.append(key)
    for name, entry in wanted.get("idf_components", {}).items():
        if name in CORE.data[KEY_ESP32].get(KEY_COMPONENTS, {}):
            continue
        add_idf_component(
            name=name,
            repo=entry.get("repo"),
            ref=entry.get("ref"),
            path=entry.get("path"),
            override_path=entry.get("override_path"),
        )
        applied.append(name)
    data = CORE.data[KEY_ESP32].setdefault(KEY_MBEDTLS_SDKCONFIG, MbedtlsSdkconfigData())
    for name in wanted.get("mbedtls", []):
        attribute, _, option = name.partition(":")
        value = getattr(data, attribute, None)
        if isinstance(value, set) and option not in value:
            value.add(option)
            applied.append(name)
        elif value is False:
            setattr(data, attribute, True)
            applied.append(name)
    return applied


def abi_id(defines: list[str], sdkconfig: dict[str, str]) -> int:
    text = "\n".join(
        [
            str(module_abi_version()),
            str(compile_stamp()),
            __version__,
            str(CORE.data[KEY_CORE][KEY_FRAMEWORK_VERSION]),
            str(get_esp32_variant()),
            *defines,
            *(f"{key}={value}" for key, value in sdkconfig.items()),
        ]
    )
    return zlib.crc32(text.encode("utf-8")) & 0xFFFFFFFF


def type_hash(declaration: str) -> int:
    decl = cpp.declaration(declaration)
    kind = re.sub(r"\s+", "", decl.group("type") + decl.group("array"))
    return zlib.crc32(kind.encode("utf-8")) & 0xFFFFFFFF


def declarations() -> dict[str, str]:
    found: dict[str, str] = {}
    for statement in CORE.global_statements:
        text = str(statement)
        decl = cpp.declaration(text)
        if decl is not None:
            found.setdefault(decl.group("name"), text)
    return found


def core_ids() -> dict[str, str]:
    decls = declarations()
    names = {
        declared.id
        for declared, _path in getattr(CORE.config, "declare_ids", [])
        if declared.id in decls and (declared.is_manual or not NUMBERED.search(declared.id))
    }
    return {name: decls[name] for name in sorted(names)}


def pools() -> dict[str, list[str]]:
    pool = _get_pool()
    result = {}
    for attr in ("device_classes", "units", "icons"):
        strings = getattr(pool, attr)
        result[attr] = [text for text, _index in sorted(strings.items(), key=lambda item: item[1])]
    return result


def manifest(ids: dict[str, str]) -> dict[str, Any]:
    info = {
        "build_path": str(CORE.build_path),
        "defines": define_lines(),
        "integrations": sorted(CORE.loaded_integrations),
        "domains": sorted(CORE.config),
        "ids": ids,
        "pools": pools(),
        "head": [
            text
            for text in (str(statement) for statement in CORE.global_statements)
            if "\n" not in text.strip()
            and re.match(r"^\s*(#include\b|using\s+namespace\b)", text)
            and "controller_dispatch.h" not in text
        ],
    }
    directory = Path(CORE.relative_build_path("component_libs"))
    directory.mkdir(parents=True, exist_ok=True)
    (directory / CORE_FILE).write_text(json.dumps(info, indent=1) + "\n", encoding="utf-8")
    return info


def emit(ids: dict[str, str]) -> int:
    abi = abi_id(define_lines(), sdkconfig_options())
    lines = [f'extern "C" __attribute__((used)) const uint32_t component_as_lib_abi_id = {abi}u;']
    if ids:
        entries = ",\n".join(
            f'    {{"{name}", (void *) &{name}, {type_hash(text)}u}}' for name, text in sorted(ids.items())
        )
        lines.append(f"static const esphome::package_manager::IdEntry ESPHOME_CORE_IDS[] = {{\n{entries}\n}};")
        lines.append(
            "static const bool ESPHOME_CORE_IDS_SET = "
            f"(esphome::package_manager::set_core_ids(ESPHOME_CORE_IDS, {len(ids)}), true);"
        )
    cg.add_global(cg.RawStatement("\n".join(lines)))
    return abi
