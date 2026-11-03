from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any

from esphome.const import CONF_FILE, CONF_PLATFORM
from esphome.config_helpers import defined_in as _defined_in, range_document as _range_document
from esphome.core import CORE, ID, EsphomeError, Lambda

GLUE_DIR = "component_libs"
PASS_ENV = "ESPHOME_PACKAGE_PASS"
CORE_ENV = "ESPHOME_PACKAGE_CORE"
TOOLCHAIN_ENV = "ESPHOME_PACKAGE_TOOLCHAIN"
CONF_DEPENDS = "depends"
CORE_SUFFIX = "__core"
EXTEND = {"api", "esphome", "lvgl"}
LOOKUP = {"script"}

_REGISTER_COMPONENT = re.compile(r"\bApp\.register_component_\(\s*(\w+)\s*(?:,\s*(\d+)\s*)?\);")
_REGISTER_ENTITY = re.compile(r"\bApp\.register_(\w+)\(\s*(\w+)\s*(,.*)?\);\s*$", re.S)
_CONFIGURE_ENTITY = re.compile(r"\b(\w+)->configure_entity_\((.*)\);\s*$", re.S)
_ENTITY_HASH = re.compile(
    r'(\s*(?:ESPHOME_PSTR\(\s*"(?:[^"\\]|\\.)*"\s*\)|"(?:[^"\\]|\\.)*"|[\w:]+)\s*,\s*)(\d+)(?:[uU][lL]?|[lL][uU]?)?(\s*,)',
    re.S,
)
_PACKED = re.compile(r"(,\s*)(\d+)(?:[uU][lL]?|[lL][uU]?)?(\s*\)\s*;\s*)$", re.S)
_TRAILING_COMMENT = re.compile(r";[ \t]*//[^\n]*$")
_ENTITY_TYPE = re.compile(r"ENTITY_(?:CONTROLLER_)?TYPE_\(\s*[^,]+,\s*(\w+)\s*,")
_ENTITY_COMPONENT = re.compile(r"ENTITY_(?:CONTROLLER_)?TYPE_\(\s*(\w+)::")
_CATEGORIES = (("device_classes", 0, 0), ("units", 1, 8), ("icons", 2, 16))
_INCLUDE = re.compile(r"^\s*(#include\b|using\s+namespace\b|using\s+[\w:]+\s*;)")
_CORE_ONLY_INCLUDES = ("esphome/core/controller_dispatch.h",)
_LOCAL_NAMES = re.compile(
    r"(?:^[ \t]*|[;{(,]\s*)(?:const\s+)?(?:auto|[A-Za-z_][\w:]*(?:<[^;={}]*?>)?)\s*(?:const\s*)?[*&]*\s+([A-Za-z_]\w*)\s*(?=[=;{(\[,)])",
    re.M,
)
_LV_CREATE = re.compile(r"^\s*(\w+)\s*=\s*lv_\w+_create\((.*)\)\s*;\s*$", re.S)


def package_pass() -> bool:
    return PASS_ENV in os.environ


def _entity_header() -> str:
    import esphome.core as esphome_core

    return (Path(esphome_core.__file__).parent / "entity_types.h").read_text(encoding="utf-8")


def entity_domains() -> list[str]:
    components = Path(__file__).resolve().parent.parent
    names = {name.rstrip("_") for name in _ENTITY_COMPONENT.findall(_entity_header())}
    return sorted(name for name in names if (components / name / "__init__.py").is_file())


def _entity_singulars() -> set[str]:
    return set(_ENTITY_TYPE.findall(_entity_header()))


def dependencies(packages: dict[str, Any], name: str) -> list[str]:
    ordered: list[str] = []

    def visit(current: str) -> None:
        for dep in packages[current][CONF_DEPENDS]:
            if dep not in ordered:
                visit(dep)
                ordered.append(dep)

    visit(name)
    return ordered


PACKAGE_DOCUMENTS: set[str] = set()


def _mine(found: set[str], target: str) -> bool:
    return target in found and not (found & (PACKAGE_DOCUMENTS - {target}))


def _owned(declared: Any, path: list[Any], target: str) -> bool:
    if declared.is_manual:
        document = _range_document(declared.id)
        if document is not None:
            return document == target
    for depth in range(len(path) - 1, 0, -1):
        if depth == 1 and path[0] in EXTEND:
            return False
        try:
            node = CORE.config.get_nested_item(path[:depth])
        except Exception:
            return False
        documents = _defined_in(node)
        if documents:
            return _mine(documents, target)
    return False


def _order(packages: dict[str, Any]) -> list[str]:
    order: list[str] = []
    for name in packages:
        for dep in [*dependencies(packages, name), name]:
            if dep not in order:
                order.append(dep)
    return order


def lib_package(component: str) -> str:
    return "lib_" + re.sub(r"\W", "_", component.split("/")[-1])[:19]


def build_packages(packages: dict[str, Any]) -> list[dict[str, Any]]:
    from esphome.components.esp32.const import KEY_COMPONENTS, KEY_ESP32

    from . import exports

    base_components = set(CORE.data[KEY_ESP32].get(KEY_COMPONENTS, {}))
    libs = Path(CORE.relative_build_path(GLUE_DIR))
    for stale in [*libs.glob("*.item.json"), *libs.glob("*.exports.json")]:
        stale.unlink()
    ids = exports.core_ids()
    exports.manifest(ids)
    language = CORE.data.get("runtime_language", {}).get("keys", [])
    (libs / "language_keys.json").write_text(json.dumps(language), encoding="utf-8")
    plan = [
        {"name": name, "document": str(Path(packages[name][CONF_FILE]).resolve()), "depends": dependencies(packages, name)}
        for name in _order(packages)
    ]
    (libs / "passes.json").write_text(json.dumps(plan), encoding="utf-8")
    if hasattr(os, "fork"):
        _run_pass(libs, None)
    else:
        for index in range(len(plan)):
            _run_pass(libs, index)
    items = []
    lib_items: dict[str, dict[str, Any]] = {}
    for entry in plan:
        name = entry["name"]
        item = json.loads((libs / f"{name}.item.json").read_text(encoding="utf-8"))
        properties = item.pop("properties")
        needed = sorted(set(properties.get("idf_components", {})) - base_components)
        for component in needed:
            spec = properties["idf_components"][component]
            lib_items.setdefault(component, {
                "package": lib_package(component),
                "version": "0+" + hashlib.sha256(json.dumps([component, spec], sort_keys=True).encode()).hexdigest()[:8],
                "lib": component,
                "components": [],
                "ids": [],
                "depends": [],
            })
        exports.apply_properties(properties)
        texts = item.pop("language_keys")
        if texts:
            from esphome.components import runtime_language

            for text in texts:
                runtime_language.register_text(text)
        for platform, count in item.pop("platform_counts").items():
            CORE.platform_counts[platform] += count
        item["depends"] = [dep.replace("_", "-") for dep in dependencies(packages, name)] + [
            lib_package(component).replace("_", "-") for component in needed
        ]
        items.append(item)
    items = [*lib_items.values(), *items]
    for platform in _entity_singulars():
        if platform != "singular" and not CORE.platform_counts[platform]:
            CORE.platform_counts[platform] = 1
    abi = exports.emit(ids)
    for item in items:
        item["abi"] = abi
    (libs / "manifest.json").write_text(json.dumps(items, indent=1) + "\n", encoding="utf-8")
    return items


def _filter_list(config: Any, path: list[Any], items: list[Any], owned) -> list[Any]:
    removed = [index for index, item in enumerate(items) if owned(item)]
    if not removed:
        return items
    gone = set(removed)
    size = len(path)
    declared = []
    for declared_id, id_path in config.declare_ids:
        if len(id_path) > size and list(id_path[:size]) == path and isinstance(id_path[size], int):
            index = id_path[size]
            if index in gone:
                continue
            shift = sum(1 for item in removed if item < index)
            if shift:
                id_path = [*id_path[:size], index - shift, *id_path[size + 1 :]]
        declared.append((declared_id, id_path))
    config.declare_ids[:] = declared
    return [item for index, item in enumerate(items) if index not in gone]


def remove_package_nodes(config: Any, documents: list[Path]) -> None:
    targets = {str(document) for document in documents}

    def owned(node: Any) -> bool:
        found = _defined_in(node)
        return bool(found & targets)

    before = {declared.id for declared, _path in config.declare_ids}
    _remove_nodes(config, owned)
    removed = before - {declared.id for declared, _path in config.declare_ids}
    mqtt = config.get("mqtt")
    if isinstance(mqtt, dict) and mqtt.get("_publish"):
        mqtt["_publish"] = [entry for entry in mqtt["_publish"] if str(entry[0]) not in removed]
    references: list[str] = []
    stack: list[tuple[Any, list[Any]]] = [(config, [])]
    while stack:
        node, path = stack.pop()
        if isinstance(node, ID):
            if not node.is_declaration and node.id in removed:
                references.append(f"{node.id} at {'->'.join(str(part) for part in path)}")
        elif isinstance(node, dict):
            stack.extend((value, [*path, key]) for key, value in node.items())
        elif isinstance(node, (list, tuple)):
            stack.extend((value, [*path, index]) for index, value in enumerate(node))
    if references:
        raise EsphomeError(
            "package_manager: the core config names package objects (the core may not use them): "
            + "; ".join(sorted(references))
        )


def _remove_nodes(config: Any, owned) -> None:
    for domain in list(config):
        conf = config[domain]
        if domain == "api" and isinstance(conf, dict) and "actions" in conf:
            conf["actions"] = _filter_list(config, [domain, "actions"], conf["actions"], owned)
        elif domain == "esphome":
            for key in ("on_boot", "on_shutdown"):
                if key in conf:
                    conf[key] = _filter_list(config, [domain, key], conf[key], owned)
        elif domain == "lvgl" and isinstance(conf, list):
            for index, entry in enumerate(conf):
                if "pages" in entry:
                    entry["pages"] = _filter_list(config, [domain, index, "pages"], entry["pages"], owned)
                top = entry.get("top_layer")
                if top and "widgets" in top:
                    top["widgets"] = _filter_list(
                        config, [domain, index, "top_layer", "widgets"], top["widgets"], owned
                    )
        elif isinstance(conf, list):
            config[domain] = _filter_list(config, [domain], conf, owned)
        elif owned(conf):
            config.declare_ids[:] = [item for item in config.declare_ids if not item[1] or item[1][0] != domain]
            del config[domain]


def removal_step(documents: list[Path]) -> Any:
    from esphome.config import ConfigValidationStep

    class RemovePackagesStep(ConfigValidationStep):
        priority = -30.0

        def run(self, result: Any) -> None:
            remove_package_nodes(result, documents)

    return RemovePackagesStep()


def _run_pass(libs: Path, index: int | None) -> None:
    env = {
        **os.environ,
        PASS_ENV: "1",
        CORE_ENV: str(Path(CORE.build_path).resolve()),
        TOOLCHAIN_ENV: str(CORE.toolchain or ""),
    }
    args = [sys.executable, "-m", __name__, str(Path(CORE.config_path).resolve())]
    if index is not None:
        args.append(str(index))
    result = subprocess.run(args, env=env, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise EsphomeError(f"package_manager: package pass failed:\n{(result.stdout + result.stderr)[-8000:]}")


def _extend_part(domain: str, conf: Any, target: str) -> Any:
    def own(item: Any) -> bool:
        return _mine(_defined_in(item), target)

    if domain == "api":
        actions = [action for action in conf.get("actions", []) if own(action)]
        return {**conf, "actions": actions} if actions else None
    if domain == "esphome":
        parts = {key: [entry for entry in conf.get(key, []) if own(entry)] for key in ("on_boot", "on_shutdown")}
        return parts if any(parts.values()) else None
    configs = []
    for config in conf:
        pages = [page for page in config.get("pages", []) if own(page)]
        top = config.get("top_layer") or {}
        widgets = [widget for widget in top.get("widgets", []) if own(widget)]
        if not pages and not widgets:
            continue
        part = {key: value for key, value in config.items() if key not in ("pages", "top_layer", "bottom_layer", "widgets")}
        part["pages"] = pages
        if widgets:
            part["top_layer"] = {**top, "widgets": widgets}
        configs.append(part)
    return configs or None


def _select(target: str, core_domains: set[str]) -> tuple[set[str], dict[str, Any], list[tuple[str, Any, Any]]]:
    from esphome.config import iter_component_configs

    domains = {
        domain
        for domain, conf in CORE.config.items()
        if domain not in core_domains and domain not in EXTEND and (not _defined_in(conf) or _mine(_defined_in(conf), target))
    }
    parts = {domain: _extend_part(domain, CORE.config[domain], target) for domain in EXTEND if domain in CORE.config}
    for domain in LOOKUP:
        if isinstance(CORE.config.get(domain), list):
            parts[domain] = [entry for entry in CORE.config[domain] if _mine(_defined_in(entry), target)] or None
    jobs: list[tuple[str, Any, Any]] = []
    for name, component, conf in iter_component_configs(CORE.config):
        if component.to_code is None:
            continue
        domain = name.split(".")[0]
        if domain == "esphome":
            continue
        if name in parts:
            if parts[name] is not None:
                jobs.append((name, component, parts[name]))
            continue
        if domain in parts:
            continue
        if domain in domains or _mine(_defined_in(conf), target):
            jobs.append((name, component, conf))
    return domains, parts.get("esphome") or {}, jobs


async def _triggers_to_code(parts: dict[str, Any]) -> None:
    from esphome import automation
    import esphome.codegen as cg

    for key in ("on_boot", "on_shutdown"):
        for conf in parts.get(key, []):
            trigger = cg.new_Pvariable(conf["trigger_id"], conf.get("priority"))
            await cg.register_component(trigger, conf)
            await automation.build_automation(trigger, [], conf)


async def _publish_to_code(client_id: Any, entries: list[tuple[Any, Any, str]]) -> None:
    from esphome.components.mqtt import PUBLISH_WAY_ENUM
    import esphome.codegen as cg

    client = await cg.get_variable(client_id)
    for entity_id, mqtt_id, way in entries:
        entity = await cg.get_variable(entity_id)
        component = await cg.get_variable(mqtt_id)
        cg.add(client.add_publish_entry(entity, component, PUBLISH_WAY_ENUM[way]))


def _remap_packed(packed: int, core_pools: dict[str, list[str]], strings: list[tuple[int, str]]) -> str:
    from esphome.core.entity_helpers import _get_pool

    pool = _get_pool()
    expression = str(packed & ~0xFFFFFF)
    for attr, category, shift in _CATEGORIES:
        index = (packed >> shift) & 0xFF
        if index == 0:
            continue
        reverse = {value: text for text, value in getattr(pool, attr).items()}
        text = reverse.get(index)
        if text is None:
            continue
        core_list = core_pools.get(attr, [])
        if text in core_list:
            expression += f" | ({core_list.index(text) + 1}u << {shift})"
            continue
        slot = len(strings)
        strings.append((category, text))
        expression += f" | (uint32_t(package_strings_[{slot}]) << {shift})"
    return expression


def _entity(text: str, core_pools, strings, entities: list[tuple[str, str]], singulars: set[str]) -> tuple[str, str | None]:
    register = _REGISTER_ENTITY.search(text)
    if register is not None and register.group(1) not in singulars:
        register = None
    configure = _CONFIGURE_ENTITY.search(text)
    if register is not None:
        entities.append((register.group(1), register.group(2)))
    if register is not None and register.group(3):
        start = register.start(3) + 1
        target = register.group(2)
    elif configure is not None:
        start = configure.start(2)
        target = configure.group(1)
    else:
        return text, None
    hashed = _ENTITY_HASH.match(text, start)
    if hashed is not None:
        text = text[: hashed.start(2)] + "0" + text[hashed.end(2) :]
    packed = _PACKED.search(text)
    if packed is not None:
        remapped = _remap_packed(int(packed.group(2)), core_pools, strings)
        text = text[: packed.start()] + packed.group(1) + remapped + packed.group(3)
    return text, target


def _copy_sources(name: str, domains: set[str], libs: Path) -> list[str]:
    from esphome.config import iter_components

    source_root = libs / name / "src"
    if source_root.exists():
        shutil.rmtree(source_root)
    headers: list[str] = []
    for component_name, component in iter_components(CORE.config):
        if component_name.split(".")[0] not in domains and component_name.split(".")[-1] not in domains:
            continue
        for resource in component.resources:
            target = Path(resource.package.replace(".", "/")) / resource.resource
            destination = source_root / target
            destination.parent.mkdir(parents=True, exist_ok=True)
            with resource.path() as source:
                destination.write_bytes(Path(source).read_bytes())
            if target.suffix in (".h", ".hpp"):
                headers.append(f'#include "{target.as_posix()}"')
    return headers


def _write_package(name: str, target: str, owned: set[str], external: dict[str, str], core: dict[str, Any],
                   domains: set[str], depends: list[str], core_language: list[str]) -> None:
    from esphome.cpp_generator import LineComment

    from . import cpp, exports

    libs = Path(os.environ[CORE_ENV]) / GLUE_DIR
    global_texts = [str(statement) for statement in CORE.global_statements if not isinstance(statement, LineComment)]
    main_texts = [
        _TRAILING_COMMENT.sub(";", str(statement))
        for statement in CORE.main_statements
        if not isinstance(statement, LineComment)
    ]
    head = [
        text
        for text in global_texts
        if _INCLUDE.match(text) and "\n" not in text.strip() and not any(i in text for i in _CORE_ONLY_INCLUDES)
    ]
    definitions = [text for text in global_texts if not _INCLUDE.match(text)]
    definitions += [
        str(statement)
        for statement in CORE.global_statements_after_includes
        if not isinstance(statement, LineComment)
    ]
    declared: dict[str, str] = {}
    for text in definitions:
        decl = cpp.declaration(text)
        if decl is not None:
            declared.setdefault(decl.group("name"), text)
    used: set[str] = set()
    for text in [*definitions, *main_texts]:
        used |= cpp.identifiers(text)
    imports = sorted(item for item in used if item in external and item not in declared)
    shared = {item: declared[item] for item in sorted(owned) if item in declared}
    replace = {item: f"(*{item}{CORE_SUFFIX})" for item in imports}

    def rename(text: str) -> str:
        local = set(_LOCAL_NAMES.findall(text))
        return cpp.replace_identifiers(text, lambda found: None if found in local else replace.get(found))

    pointers = [cpp.pointer_declaration(external[item], item, f"{item}{CORE_SUFFIX}") for item in imports]
    if None in pointers:
        bad = [imports[i] for i, value in enumerate(pointers) if value is None]
        raise EsphomeError(f"package_manager: package '{name}' cannot import {', '.join(bad)}")
    singulars = _entity_singulars()
    pool = CORE.data.get("component_source_pool")
    source_names = {index: source for source, index in pool.sources.items()} if pool is not None else {}
    strings: list[tuple[int, str]] = []
    entities: list[tuple[str, str]] = []
    widgets: list[str] = []
    sourced: list[str] = []
    construct: list[str] = []
    components = 0
    for text in main_texts:
        component = _REGISTER_COMPONENT.search(text)
        if component is not None:
            source = source_names.get(int(component.group(2) or 0))
            if source is not None:
                construct.append(f"  esphome::component_source_add({rename(component.group(1))}, {json.dumps(source)});")
                sourced.append(component.group(1))
            construct.append(f"  package_add_component_({component.group(1)});")
            components += 1
            continue
        created = _LV_CREATE.match(text)
        if created is not None and not (cpp.identifiers(created.group(2)) & set(declared)):
            widgets.append(created.group(1))
        text, entity = _entity(text, core["pools"], strings, entities, singulars)
        construct.append(rename(text))
        if entity is not None:
            construct.append(f"  if (!esphome::package_manager::entity_key_free({rename(entity)}))\n    return false;")
    objects = []
    for text in main_texts:
        placed = re.match(r"^\s*new\s*\(\s*(\w+)\s*\)", text)
        if placed is None or placed.group(1) not in declared:
            continue
        decl = cpp.declaration(declared[placed.group(1)])
        kind = re.sub(r"\s*\*\s*(const\s*)?$", "", decl.group("type").strip()) if decl is not None else ""
        if kind:
            objects.append((placed.group(1), kind))
    imports_code = "\n".join(
        f"  {item}{CORE_SUFFIX} = reinterpret_cast<decltype({item}{CORE_SUFFIX})>("
        f'package_manager_query_id("{item}", {exports.type_hash(external[item])}u));\n'
        f"  if ({item}{CORE_SUFFIX} == nullptr)\n    return false;"
        for item in imports
    )
    strings_code = "\n".join(
        f"  package_strings_[{slot}] = esphome::entity_string_add({category}, {json.dumps(text)});"
        for slot, (category, text) in enumerate(strings)
    )
    release_code = "\n".join(
        [f"  esphome::component_source_release({rename(var)});" for var in reversed(sourced)]
        + [
            f"  esphome::entity_string_release({category}, package_strings_[{slot}]);"
            for slot, (category, _text) in enumerate(strings)
        ]
    )
    unregister_code = "\n".join(f"  App.unregister_{singular}({rename(var)});" for singular, var in reversed(entities))
    destroy_code = "\n".join(
        f"  std::destroy_at(reinterpret_cast<{kind} *>({var}));" for var, kind in reversed(objects)
    )
    ids_table = (
        [
            "static const esphome::package_manager::IdEntry package_ids_[] = {",
            *(
                f'    {{"{item}", (void *) &{item}, {exports.type_hash(text)}u}},'
                for item, text in sorted(shared.items())
            ),
            "};",
        ]
        if shared
        else ["static const esphome::package_manager::IdEntry *const package_ids_ = nullptr;"]
    )
    glue = "\n".join(
        [
            '#include "esphome.h"',
            "#include <memory>",
            "using namespace esphome;",
            *dict.fromkeys([*core.get("head", []), *head]),
            '#include "esphome/components/package_manager/ids.h"',
            '#include "esphome/components/package_manager/module_abi.h"',
            "",
            '__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;',
            *pointers,
            "",
            *(rename(text) for text in definitions),
            "",
            f"static esphome::Component *package_components_[{max(1, components)}];",
            "static size_t package_component_count_ = 0;",
            f"static uint8_t package_strings_[{max(1, len(strings))}];",
            *ids_table,
            "static void package_add_component_(esphome::Component *component) {",
            "  package_components_[package_component_count_++] = component;",
            "}",
            "static bool package_import_() {",
            imports_code,
            "  return true;",
            "}",
            "static bool package_construct_() {",
            strings_code,
            *construct,
            "  return true;",
            "}",
            'extern "C" MODULE_EXPORT uint32_t module_init(const module_host_api_t *host) {',
            "  if (host == nullptr || host->abi_version != ESPHOME_MODULE_ABI_VERSION || !package_import_())",
            "    return 0;",
            "  if (!package_construct_())",
            "    return 0;",
            f"  if (!esphome::package_manager::register_ids(package_ids_, package_ids_, {len(shared)}))",
            "    return 0;",
            "  for (size_t i = 1; i < package_component_count_; i++) {",
            "    esphome::Component *component = package_components_[i];",
            "    size_t j = i;",
            "    for (; j > 0 && package_components_[j - 1]->get_actual_setup_priority() <",
            "                       component->get_actual_setup_priority();",
            "         j--)",
            "      package_components_[j] = package_components_[j - 1];",
            "    package_components_[j] = component;",
            "  }",
            "  return ESPHOME_MODULE_ABI_VERSION;",
            "}",
            'extern "C" MODULE_EXPORT void module_loop(void) {',
            "  for (size_t i = 0; i < package_component_count_; i++) {",
            "    package_components_[i]->call();",
            "    if (!package_components_[i]->can_proceed())",
            "      break;",
            "  }",
            "}",
            "static bool package_shut_down_ = false;",
            'extern "C" MODULE_EXPORT uint32_t module_stop(void) {',
            "  if (!package_shut_down_) {",
            "    for (size_t i = package_component_count_; i > 0; i--)",
            "      package_components_[i - 1]->on_shutdown();",
            "    package_shut_down_ = true;",
            "  }",
            "  bool done = true;",
            "  for (size_t i = package_component_count_; i > 0; i--)",
            "    done = package_components_[i - 1]->teardown() && done;",
            "  if (!done)",
            "    return 0;",
            "  for (size_t i = 0; i < package_component_count_; i++)",
            "    App.scheduler.cancel_component(package_components_[i]);",
            "  package_shut_down_ = false;",
            "  return 1;",
            "}",
            'extern "C" MODULE_EXPORT void module_deinit(void) {',
            "  esphome::package_manager::unregister_ids(package_ids_);",
            unregister_code,
            *(f"  lv_obj_delete({widget});" for widget in reversed(widgets)),
            release_code,
            destroy_code,
            "  package_component_count_ = 0;",
            "}",
            "",
        ]
    )
    (libs / f"{name}.lib.cpp").write_text(glue, encoding="utf-8")
    headers = _copy_sources(name, domains, libs)
    for dep in depends:
        dep_root = libs / dep / "src"
        for header in dep_root.rglob("*"):
            if header.suffix in (".h", ".hpp"):
                destination = libs / name / "src" / header.relative_to(dep_root)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(header, destination)
                headers.append(f'#include "{header.relative_to(dep_root).as_posix()}"')
    include = libs / name / "include"
    (include / "esphome" / "core").mkdir(parents=True, exist_ok=True)
    core_defines = set(core["defines"])
    core_defines_h = (Path(os.environ[CORE_ENV]) / "src" / "esphome" / "core" / "defines.h").as_posix()
    extra = []
    for line in exports.define_lines():
        if line in core_defines:
            continue
        macro = line.split()[1].split("(")[0]
        extra += [f"#ifndef {macro}", line, "#endif"]
    (include / "esphome" / "core" / "defines.h").write_text(
        "\n".join(["#pragma once", f'#include "{core_defines_h}"', *extra, ""]), encoding="utf-8"
    )
    core_header = (Path(os.environ[CORE_ENV]) / "src" / "esphome.h").as_posix()
    (include / "esphome.h").write_text(
        "#pragma once\n" + f'#include "{core_header}"\n' + "\n".join(dict.fromkeys(headers)) + "\n",
        encoding="utf-8",
    )
    (libs / f"{name}.exports.json").write_text(json.dumps(shared, indent=1) + "\n", encoding="utf-8")
    item = {
        "package": name,
        "version": "0+" + hashlib.sha256(Path(target).read_bytes()).hexdigest()[:8],
        "glue": str(libs / f"{name}.lib.cpp"),
        "include": str(include),
        "sources": str(libs / name / "src"),
        "components": sorted(domains),
        "ids": [f"{item} {exports.type_hash(external[item])}" for item in imports],
        "properties": exports.properties(),
        "language_keys": CORE.data["runtime_language"]["keys"][len(core_language):],
        "platform_counts": dict(CORE.platform_counts),
    }
    (libs / f"{name}.item.json").write_text(json.dumps(item, indent=1) + "\n", encoding="utf-8")


def _pass_main(pass_yaml: str, only: str | None) -> int:
    import logging

    from esphome.config import read_config
    from esphome.const import Toolchain

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    libs = Path(os.environ[CORE_ENV]) / GLUE_DIR
    CORE.config_path = Path(pass_yaml)
    if os.environ.get(TOOLCHAIN_ENV):
        CORE.toolchain = Toolchain(os.environ[TOOLCHAIN_ENV])
    config = read_config({}, skip_external_update=True)
    if config is None:
        return 2
    CORE.config = config
    plan = json.loads((libs / "passes.json").read_text(encoding="utf-8"))
    if only is not None:
        entry = plan[int(only)]
        return _generate(entry["name"], entry["document"], entry["depends"], libs)
    for entry in plan:
        sys.stdout.flush()
        sys.stderr.flush()
        pid = os.fork()
        if pid == 0:
            try:
                rc = _generate(entry["name"], entry["document"], entry["depends"], libs)
            except BaseException:
                import traceback

                traceback.print_exc()
                rc = 1
            sys.stdout.flush()
            sys.stderr.flush()
            os._exit(rc)
        _, status = os.waitpid(pid, 0)
        rc = os.waitstatus_to_exitcode(status)
        if rc != 0:
            return rc
        item = json.loads((libs / f"{entry['name']}.item.json").read_text(encoding="utf-8"))
        language = json.loads((libs / "language_keys.json").read_text(encoding="utf-8"))
        language.extend(item["language_keys"])
        (libs / "language_keys.json").write_text(json.dumps(language), encoding="utf-8")
    return 0


def _generate(name: str, document: str, depends: list[str], libs: Path) -> int:
    import logging

    from esphome import yaml_util
    from esphome.__main__ import _wrap_to_code
    from esphome.cpp_generator import MockObj

    from . import cpp

    core = json.loads((libs / "core.json").read_text(encoding="utf-8"))
    external: dict[str, str] = dict(core["ids"])
    for dep in depends:
        external.update(json.loads((libs / f"{dep}.exports.json").read_text(encoding="utf-8")))
    target = str(Path(document).resolve())
    PACKAGE_DOCUMENTS.update(
        str(Path(entry[CONF_FILE]).resolve())
        for entry in (CORE.config.get("package_manager") or {}).get("packages", {}).values()
    )
    owned = {declared.id for declared, path in CORE.config.declare_ids if _owned(declared, path, target)}
    if not owned:
        logging.error("package '%s' declares nothing (%s)", name, target)
        return 3
    mqtt = CORE.config.get("mqtt") or {}
    publish = [entry for entry in mqtt.get("_publish", []) if entry[0].id in owned]
    CORE.data["package_manager_owned"] = {ID(item) for item in owned}
    language = json.loads((libs / "language_keys.json").read_text(encoding="utf-8"))
    CORE.data["runtime_language"] = {"keys": list(language), "index": {text: i for i, text in enumerate(language)}}
    domains, triggers, jobs = _select(target, set(core["domains"]))
    CORE.build_path = libs / name / "pass"
    for declared, _path in CORE.config.declare_ids:
        if declared.id in owned or declared.id not in external:
            continue
        decl = cpp.declaration(external[declared.id])
        pointer = decl is not None and decl.group("type").replace("const", "").strip().endswith("*")
        CORE.register_variable(declared, MockObj(declared.id, "->" if pointer else "."))
    if triggers:
        CORE.add_job(_triggers_to_code, triggers)
    if publish:
        CORE.add_job(_publish_to_code, mqtt["id"], publish)
    waited: set[Any] = set()
    generators = {}
    for attribute in ("_get_variable_generator", "_get_variable_with_full_id_generator"):
        generators[attribute] = getattr(CORE, attribute)

        def tracking(id_, original=generators[attribute]):
            waited.add(id_)
            return original(id_)

        setattr(CORE, attribute, tracking)
    try:
        for job_name, component, conf in jobs:
            CORE.add_job(_wrap_to_code(job_name, component, yaml_util), conf)
        CORE.flush_tasks()
    except EsphomeError:
        missing = sorted({item.id for item in waited if item not in CORE.variables})
        for item in missing:
            reason = "owned by the package, never created" if item in owned else (
                "declared in the core without an exported declaration" if item not in external else "external"
            )
            logging.error("package '%s': id '%s' never resolved (%s)", name, item, reason)
        raise
    _write_package(name, target, owned, external, core, domains, depends, language)
    return 0


if __name__ == "__main__":
    sys.exit(_pass_main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
