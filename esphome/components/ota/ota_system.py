from __future__ import annotations

import hashlib
import inspect
import os
from pathlib import Path
from typing import Any

from esphome import const, loader, yaml_util
import esphome.core as esphome_core
import esphome.config_validation as cv
from esphome.config_helpers import merge_config
from esphome.const import (
    CONF_API,
    CONF_BUILD_PATH,
    CONF_ESPHOME,
    CONF_ID,
    CONF_INCLUDES,
    CONF_OTA,
    CONF_PACKAGES,
    CONF_PLATFORM,
    CONF_SUBSTITUTIONS,
)
from esphome.core import CORE, ID, Lambda

CONF_OTA_SYSTEM = "ota_system"
CONF_KEEP_COMPONENTS = "keep_components"
CONF_KEEP_IDS = "keep_ids"
CONF_ALLOW_PARTITION_ACCESS = "allow_partition_access"
CONF_SYSTEM_BUILD = "system_build"
BUILD_PATH_SUFFIX = "-ota-system"
FINGERPRINT_FILENAME = "ota_system.fingerprint"
MAIN_FINGERPRINT_FILENAME = "main.fingerprint"
CHIP_REVISION_ENV = "ESPHOME_CHIP_REVISION"
FINGERPRINT_SKIP_DOMAINS = {CONF_ESPHOME, CONF_PACKAGES, CONF_SUBSTITUTIONS, "external_components"}
API_DROP_KEYS = ("actions", "services")

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
        if isinstance(value, list) and len(value) == 1:
            return value[0]
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
    dropped = API_DROP_KEYS if domain == CONF_API else ()
    return {
        k: v
        for k, v in value.items()
        if not _is_automation_key(k) and k != CONF_OTA_SYSTEM and k not in dropped
    }


def _manifest_loads(manifest: Any, validated_value: Any) -> list[str]:
    loads = list(manifest.dependencies)
    auto_load = manifest.auto_load
    if callable(auto_load):
        try:
            if inspect.signature(auto_load).parameters:
                auto_load = auto_load(validated_value)
            else:
                auto_load = auto_load()
        except (KeyError, TypeError, AttributeError):
            auto_load = []
    loads.extend(auto_load)
    return loads


def _declared_dependencies(domain: str, value: Any, validated_value: Any) -> list[str]:
    deps = []
    component = loader.get_component(domain)
    if component is not None:
        deps.extend(_manifest_loads(component, validated_value))
    if isinstance(value, dict) and CONF_PLATFORM in value:
        platform = loader.get_platform(domain, str(value[CONF_PLATFORM]))
        if platform is not None:
            deps.extend(_manifest_loads(platform, validated_value))
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


def _user_config() -> dict[str, Any]:
    from esphome import config as esphome_config
    from esphome.components.packages import do_packages_pass, merge_packages
    from esphome.components.substitutions import do_substitution_pass

    config = yaml_util.load_yaml(CORE.config_path)
    if CONF_PACKAGES in config:
        config = do_packages_pass(config)
    config = do_substitution_pass(config, None)
    if CONF_PACKAGES in config:
        config = merge_packages(config)
    config.pop(CONF_SUBSTITUTIONS, None)
    esphome_config._resolve_component_aliases(config)
    esphome_config.resolve_extend_remove(config)
    return config


def _target() -> Path:
    return CORE.relative_config_path(CORE.config_path.stem, "ota", "ota_system.yaml")


def derive(
    keep_components: list[str],
    keep_ids: list[str],
    includes: list[Path],
    validated: dict[str, Any],
    raw: dict[str, Any] | None = None,
) -> dict[str, Any]:
    if raw is None:
        raw = _user_config()
    units = _units(raw)
    reduced = {u: _reduce_value(u[0], _unit_value(validated, u)) for u in units}

    declares: dict[str, tuple[str, int | None]] = {}
    references: dict[tuple[str, int | None], set[str]] = {}
    for unit in units:
        value = reduced[unit]
        own = value.get(CONF_ID) if isinstance(value, dict) else None
        if isinstance(own, ID):
            declares[str(own.id)] = unit
    for unit in units:
        declared: set[str] = set()
        referenced: set[str] = set()
        _walk_ids(reduced[unit], declared, referenced)
        for name in declared:
            declares.setdefault(name, unit)
        references[unit] = referenced - declared

    wanted_domains = set(BASE_COMPONENTS) | set(keep_components)
    kept = {u for u in units if u[0] in wanted_domains}
    for name in keep_ids:
        if name not in declares:
            raise cv.Invalid(f"{CONF_KEEP_IDS}: '{name}' is not declared in this configuration")
        kept.add(declares[name])
    pending = list(kept)
    while pending:
        unit = pending.pop()
        needed = set()
        for domain in _declared_dependencies(
            unit[0], _unit_value(raw, unit), _unit_value(validated, unit)
        ):
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
            platform[CONF_ALLOW_PARTITION_ACCESS] = True

    for include in includes:
        derived = merge_config(derived, yaml_util.load_yaml(include))
    return derived


def write(
    keep_components: list[str],
    keep_ids: list[str],
    includes: list[Path],
    validated: dict[str, Any],
) -> Path:
    raw = _user_config()
    derived = derive(keep_components, keep_ids, includes, validated, raw)
    target = _target()
    target.parent.mkdir(parents=True, exist_ok=True)
    text = yaml_util.dump(derived, show_secrets=False)
    _write_if_changed(target, text)
    secrets = CORE.relative_config_path(yaml_util.SECRET_YAML)
    if secrets.is_file():
        include = Path(os.path.relpath(secrets.absolute(), target.parent)).as_posix()
        _write_if_changed(target.parent / yaml_util.SECRET_YAML, f"<<: !include {include}\n")
    _write_if_changed(target.parent / FINGERPRINT_FILENAME, _fingerprint(text, derived, secrets) + "\n")
    main_text = yaml_util.dump(raw, show_secrets=False)
    _write_if_changed(
        target.parent / MAIN_FINGERPRINT_FILENAME, _fingerprint(main_text, validated, secrets) + "\n"
    )
    return target


def _fingerprint(text: str, derived: dict[str, Any], secrets: Path) -> str:
    digest = hashlib.sha256()
    digest.update(const.__version__.encode())
    digest.update(os.environ.get(CHIP_REVISION_ENV, "").encode())
    digest.update(_host_independent(text).encode("utf-8"))
    for directory in sorted({Path(esphome_core.__file__).parent, *_component_dirs(derived)}):
        _hash_tree(digest, directory)
    for path in sorted(_referenced_files(derived)):
        digest.update(_host_independent(path.as_posix()).encode("utf-8"))
        digest.update(path.read_bytes())
    if secrets.is_file():
        digest.update(secrets.read_bytes())
    return digest.hexdigest()


def _host_independent(text: str) -> str:
    for prefix in (Path(CORE.data_dir).absolute().as_posix(), Path(CORE.config_dir).absolute().as_posix()):
        text = text.replace(prefix, "")
    return text


def _component_dirs(derived: dict[str, Any]) -> set[Path]:
    pending = []
    for domain, value in derived.items():
        if domain in FINGERPRINT_SKIP_DOMAINS:
            continue
        pending.append(loader.get_component(domain))
        for item in value if isinstance(value, list) else [value]:
            if isinstance(item, dict) and CONF_PLATFORM in item:
                pending.append(loader.get_platform(domain, str(item[CONF_PLATFORM])))
    dirs: set[Path] = set()
    seen: set[str] = set()
    while pending:
        manifest = pending.pop()
        if manifest is None:
            continue
        parts = manifest.module.__name__.split(".")
        if len(parts) < 3 or parts[1] != "components" or parts[2] in seen:
            continue
        seen.add(parts[2])
        module_file = Path(manifest.module.__file__).absolute()
        dirs.update(p for p in module_file.parents if p.name == parts[2] and p.parent.name == "components")
        for name in _manifest_loads(manifest, None):
            pending.append(loader.get_component(name.split(".")[0]))
    return dirs


def _hash_tree(digest: Any, directory: Path) -> None:
    for path in sorted(directory.rglob("*")):
        if "__pycache__" in path.parts or not path.is_file():
            continue
        digest.update(path.relative_to(directory.parent).as_posix().encode("utf-8"))
        digest.update(path.read_bytes())


def _referenced_files(value: Any) -> set[Path]:
    if isinstance(value, dict):
        return set().union(*(_referenced_files(v) for v in value.values()))
    if isinstance(value, list):
        return set().union(*(_referenced_files(v) for v in value))
    if isinstance(value, Path) and value.is_absolute() and value.is_file():
        return {value}
    if isinstance(value, str) and "\n" not in value and os.path.isabs(value) and os.path.isfile(value):
        return {Path(value)}
    return set()


def _write_if_changed(path: Path, text: str) -> None:
    data = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == data:
        return
    path.write_bytes(data)
