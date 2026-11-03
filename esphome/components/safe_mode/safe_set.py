from __future__ import annotations

import inspect
from typing import Any

from esphome import loader
from esphome.config_helpers import defined_in
from esphome.const import CONF_API, CONF_ID, CONF_PLATFORM, CONF_TRIGGER_ID
from esphome.core import ID, Lambda

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
    "safe_mode",
    "http_request",
    "preferences",
    "storage",
    "store_yaml",
    "external_components",
    "display",
    "touchscreen",
    "lvgl",
}

LVGL = "lvgl"
LVGL_LISTS = ("pages", "msgboxes", "animations")
LVGL_WIDGETS = "widgets"
LVGL_LAYERS = ("top_layer", "bottom_layer")
LVGL_ROOT_TRIGGERS = ("on_idle", "on_pause", "on_resume", "on_draw_start", "on_draw_end")
API_AUTOMATIONS = ("actions", "services")
AUTOMATION_ID = "automation_id"


def _automation_key(domain: str, key: Any) -> bool:
    return isinstance(key, str) and (
        key.startswith("on_") or (domain == CONF_API and key in API_AUTOMATIONS)
    )


class SafeSet:
    def __init__(self, includes: set[str]) -> None:
        self.includes = includes
        self.units: dict[Any, Any] = {}
        self.kept: set[Any] = set()
        self.automations: set[str] = set()
        self.rescue: set[Any] = set()
        self.rescue_automations: set[str] = set()
        self.joined: set[str] = set()
        self.declares: dict[str, Any] = {}

    def in_include(self, node: Any) -> bool:
        return bool(defined_in(node) & self.includes)

    def only_include(self, node: Any) -> bool:
        found = defined_in(node)
        return bool(found) and found <= self.includes

    def walk(
        self, domain: str, value: Any, declared: set[str], referenced: set[str], automations: bool, nested: bool = False
    ) -> None:
        if isinstance(value, ID):
            (declared if value.is_declaration else referenced).add(str(value.id))
        elif isinstance(value, Lambda):
            referenced.update(str(i.id) for i in value.requires_ids)
        elif isinstance(value, dict):
            if AUTOMATION_ID in value and nested and not automations:
                if not self.in_include(value):
                    return
                automations = True
            for key, item in value.items():
                if _automation_key(domain, key) and not automations:
                    for entry in item if isinstance(item, list) else [item]:
                        if isinstance(entry, dict) and self.in_include(entry):
                            self.walk(domain, entry, declared, referenced, True, True)
                    continue
                self.walk(domain, item, declared, referenced, automations, True)
        elif isinstance(value, (list, tuple)):
            for item in value:
                self.walk(domain, item, declared, referenced, automations, nested)

    def uses_safe_only(self, domain: str, value: Any) -> bool:
        declared: set[str] = set()
        referenced: set[str] = set()
        self.walk(domain, value, declared, referenced, True)
        return all(self.declares.get(name) in self.kept for name in referenced - declared if name in self.declares)

    def mark_automations(self, domain: str, value: Any, inside: bool, top: bool = True) -> None:
        if isinstance(value, dict):
            if AUTOMATION_ID in value:
                if self.in_include(value):
                    inside = True
                    self.rescue_automations.add(str(value[AUTOMATION_ID]))
                elif not top or not inside:
                    inside = self.uses_safe_only(domain, value)
                if inside:
                    self.automations.add(str(value[AUTOMATION_ID]))
            for key, item in value.items():
                self.mark_automations(domain, item, inside and not _automation_key(domain, key), False)
        elif isinstance(value, (list, tuple)):
            for item in value:
                self.mark_automations(domain, item, inside, top)


def _lvgl_parts(conf: dict[str, Any]) -> tuple[dict[str, Any], list[tuple[str, Any]]]:
    parts: list[tuple[str, Any]] = []
    base = dict(conf)
    for key in LVGL_LISTS:
        for node in base.pop(key, None) or []:
            parts.append((str(node[CONF_ID]), node))
    for widget in base.pop(LVGL_WIDGETS, None) or []:
        node = next(iter(widget.values()))
        parts.append((str(node[CONF_ID]), node))
    for layer in LVGL_LAYERS:
        if isinstance(base.get(layer), dict):
            layer_conf = dict(base[layer])
            for widget in layer_conf.pop(LVGL_WIDGETS, None) or []:
                node = next(iter(widget.values()))
                parts.append((str(node[CONF_ID]), node))
            base[layer] = layer_conf
    for key in LVGL_ROOT_TRIGGERS:
        value = base.pop(key, None)
        for node in value if isinstance(value, list) else [value] if value else []:
            parts.append((str(node[CONF_TRIGGER_ID]), node))
    return base, parts


def _manifest_loads(manifest: Any, validated_value: Any) -> list[str]:
    loads = list(manifest.dependencies)
    auto_load = manifest.auto_load
    if callable(auto_load):
        try:
            auto_load = auto_load(validated_value) if inspect.signature(auto_load).parameters else auto_load()
        except (KeyError, TypeError, AttributeError):
            auto_load = []
    loads.extend(auto_load)
    return loads


def _dependencies(domain: str, value: Any) -> list[str]:
    deps = []
    component = loader.get_component(domain)
    if component is not None:
        deps.extend(_manifest_loads(component, value))
    if isinstance(value, dict) and CONF_PLATFORM in value:
        platform = loader.get_platform(domain, str(value[CONF_PLATFORM]))
        if platform is not None:
            deps.extend(_manifest_loads(platform, value))
    return [d.split(".")[0] for d in deps]


def _per_item(domain: str) -> bool:
    component = loader.get_component(domain)
    return component is not None and (
        component.multi_conf
        or component.is_platform_component
        or getattr(component.module, "CODEGEN_PER_ITEM", False)
    )


def compute(config: dict[str, Any], keep_components: list[str], keep_ids: list[str], includes: set[str]) -> SafeSet:
    safe = SafeSet(includes)
    units = safe.units
    for domain, value in config.items():
        if isinstance(value, list):
            for index, item in enumerate(value):
                units[(domain, index)] = item
            if not _per_item(domain):
                safe.joined.add(domain)
        else:
            units[(domain, None)] = value
    for unit in [u for u in units if u[0] == LVGL]:
        base, parts = _lvgl_parts(units[unit])
        units[unit] = base
        for part_id, node in parts:
            units[(LVGL, part_id)] = node

    declares = safe.declares
    references: dict[Any, set[str]] = {}
    for unit, value in units.items():
        declared: set[str] = set()
        referenced: set[str] = set()
        safe.walk(unit[0], value, declared, referenced, False)
        for name in declared:
            declares.setdefault(name, unit)
        references[unit] = referenced - declared

    wanted = BASE_COMPONENTS | set(keep_components)
    kept = {u for u in units if (u[0] in wanted and not isinstance(u[1], str)) or safe.in_include(units[u])}
    for name in keep_ids:
        if name not in declares:
            raise ValueError(name)
        kept.add(declares[name])
    pending = list(kept)
    while pending:
        unit = pending.pop()
        needed = set()
        for domain in _dependencies(unit[0], units[unit]):
            needed.update(u for u in units if u[0] == domain and not isinstance(u[1], str))
        if unit[0] in safe.joined:
            needed.update(u for u in units if u[0] == unit[0])
        needed.update(declares[name] for name in references[unit] if name in declares)
        for other in needed - kept:
            kept.add(other)
            pending.append(other)
    safe.kept = kept
    rescue = {u for u in kept if safe.only_include(units[u])}
    pending = [u for u in units if u not in rescue]
    while pending:
        unit = pending.pop()
        needed = {declares[name] for name in references[unit] if name in declares}
        for domain in _dependencies(unit[0], units[unit]):
            needed.update(u for u in units if u[0] == domain and not isinstance(u[1], str))
        if unit[0] in safe.joined:
            needed.update(u for u in units if u[0] == unit[0])
        for other in needed & rescue:
            rescue.discard(other)
            pending.append(other)
    safe.rescue = rescue
    for unit in kept:
        safe.mark_automations(unit[0], units[unit], True)
    return safe


def classify(safe: SafeSet, unit: Any) -> int:
    domain, key = unit
    if domain == "automation":
        if key in safe.rescue_automations:
            return 2
        return 0 if key in safe.automations else 1
    if unit in safe.rescue:
        return 2
    return 0 if is_safe(safe, unit) else 1


def is_safe(safe: SafeSet, unit: Any) -> bool:
    domain, key = unit
    if domain == "automation":
        return key in safe.automations
    if unit in safe.kept:
        return True
    if key is None:
        if (domain, None) in safe.units:
            return False
        component = loader.get_component(domain)
        return any(u[0] == domain for u in safe.kept) or (component is not None and component.is_platform_component)
    return False
