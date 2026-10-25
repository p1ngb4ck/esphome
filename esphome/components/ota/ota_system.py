from __future__ import annotations

import os
from pathlib import Path
from typing import Any

from esphome import loader, yaml_util
import esphome.config_validation as cv
from esphome.config_helpers import merge_config
from esphome.const import (
    CONF_BUILD_PATH,
    CONF_ESPHOME,
    CONF_ID,
    CONF_INCLUDES,
    CONF_OTA,
    CONF_PLATFORM,
)
from esphome.core import CORE, ID, Lambda

CONF_OTA_SYSTEM = "ota_system"
CONF_KEEP_COMPONENTS = "keep_components"
CONF_MIGRATE_VIA_OTA = "migrate_via_ota"
CONF_ALLOW_PARTITION_ACCESS = "allow_partition_access"
CONF_SYSTEM_BUILD = "system_build"
BUILD_PATH_SUFFIX = "-ota-system"

BASE_COMPONENTS = {
    "esphome",
    "esp32",
    "psram",
    "logger",
    "network",
    "wifi",
    "ethernet",
    "esp32_hosted",
    "api",
    "ota",
    "http_request",
    "preferences",
    "storage",
    "external_components",
    "display",
    "touchscreen",
    "lvgl",
}

LVGL_KEEP_KEYS = {
    CONF_ID,
    "displays",
    "touchscreens",
    "buffer_size",
    "color_depth",
    "byte_order",
    "draw_rounding",
    "full_refresh",
    "rotation",
    "log_level",
    "update_interval",
    "bg_color",
    "disp_bg_color",
}


def _units(config: dict[str, Any]) -> list[tuple[str, int | None]]:
    units = []
    for domain, value in config.items():
        if isinstance(value, list):
            units.extend((domain, i) for i in range(len(value)))
        else:
            units.append((domain, None))
    return units


def _unit_value(config: dict[str, Any], unit: tuple[str, int | None]) -> Any:
    domain, index = unit
    value = config.get(domain)
    if index is None:
        return value
    return value[index] if isinstance(value, list) and index < len(value) else None


def _is_automation_key(key: Any) -> bool:
    return isinstance(key, str) and key.startswith("on_")


def _walk_ids(value: Any, declared: set[str], referenced: set[str]) -> None:
    if isinstance(value, ID):
        (declared if value.is_declaration else referenced).add(str(value.id))
    elif isinstance(value, Lambda):
        referenced.update(str(i.id) for i in value.requires_ids)
    elif isinstance(value, dict):
        for key, item in value.items():
            if _is_automation_key(key):
                continue
            _walk_ids(item, declared, referenced)
    elif isinstance(value, (list, tuple)):
        for item in value:
            _walk_ids(item, declared, referenced)


def _reduce_value(domain: str, value: Any) -> Any:
    if not isinstance(value, dict):
        return value
    if domain == "lvgl":
        return {k: v for k, v in value.items() if k in LVGL_KEEP_KEYS}
    return {k: v for k, v in value.items() if not _is_automation_key(k) and k != CONF_OTA_SYSTEM}


def _declared_dependencies(domain: str, value: Any) -> list[str]:
    deps = []
    component = loader.get_component(domain)
    if component is not None:
        deps.extend(component.dependencies)
    if isinstance(value, dict) and CONF_PLATFORM in value:
        platform = loader.get_platform(domain, str(value[CONF_PLATFORM]))
        if platform is not None:
            deps.extend(platform.dependencies)
    return [d.split(".")[0] for d in deps]


def _relocate(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: _relocate(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_relocate(v) for v in value]
    if not isinstance(value, str) or os.path.isabs(value):
        return value
    try:
        path = cv.file_(value)
    except (cv.Invalid, OSError, ValueError):
        return value
    return path.absolute().as_posix()


def _target() -> Path:
    return CORE.relative_config_path(CORE.config_path.stem, "ota", "ota_system.yaml")


def derive(
    keep_components: list[str],
    includes: list[Path],
    migrate_via_ota: bool,
    validated: dict[str, Any],
) -> dict[str, Any]:
    raw = CORE.raw_config
    units = _units(raw)
    reduced = {u: _reduce_value(u[0], _unit_value(validated, u)) for u in units}

    declares: dict[str, tuple[str, int | None]] = {}
    references: dict[tuple[str, int | None], set[str]] = {}
    for unit in units:
        declared: set[str] = set()
        referenced: set[str] = set()
        _walk_ids(reduced[unit], declared, referenced)
        for name in declared:
            declares.setdefault(name, unit)
        references[unit] = referenced - declared

    wanted_domains = set(BASE_COMPONENTS) | set(keep_components)
    kept = {u for u in units if u[0] in wanted_domains}
    pending = list(kept)
    while pending:
        unit = pending.pop()
        needed = set()
        for domain in _declared_dependencies(unit[0], _unit_value(raw, unit)):
            needed.update(u for u in units if u[0] == domain)
        needed.update(declares[name] for name in references[unit] if name in declares)
        for other in needed - kept:
            kept.add(other)
            pending.append(other)

    derived: dict[str, Any] = {}
    for unit in units:
        if unit not in kept:
            continue
        domain, index = unit
        value = _relocate(_reduce_value(domain, _unit_value(raw, unit)))
        if index is None:
            derived[domain] = value
        else:
            derived.setdefault(domain, []).append(value)

    esphome_conf = derived.get(CONF_ESPHOME)
    if isinstance(esphome_conf, dict):
        esphome_conf[CONF_BUILD_PATH] = (
            f"{validated[CONF_ESPHOME][CONF_BUILD_PATH]}{BUILD_PATH_SUFFIX}"
        )

    for platform in derived.get(CONF_OTA, []):
        if isinstance(platform, dict) and platform.get(CONF_PLATFORM) == CONF_ESPHOME:
            platform[CONF_SYSTEM_BUILD] = True
            if migrate_via_ota:
                platform[CONF_ALLOW_PARTITION_ACCESS] = True

    for include in includes:
        derived = merge_config(derived, yaml_util.load_yaml(include))
    return derived


def write(
    keep_components: list[str],
    includes: list[Path],
    migrate_via_ota: bool,
    validated: dict[str, Any],
) -> Path:
    derived = derive(keep_components, includes, migrate_via_ota, validated)
    target = _target()
    target.parent.mkdir(parents=True, exist_ok=True)
    _write_if_changed(target, yaml_util.dump(derived, show_secrets=False))
    secrets = CORE.relative_config_path(yaml_util.SECRET_YAML)
    if secrets.is_file():
        include = Path(os.path.relpath(secrets.absolute(), target.parent)).as_posix()
        _write_if_changed(target.parent / yaml_util.SECRET_YAML, f"<<: !include {include}\n")
    return target


def _write_if_changed(path: Path, text: str) -> None:
    data = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return
    path.write_bytes(data)
