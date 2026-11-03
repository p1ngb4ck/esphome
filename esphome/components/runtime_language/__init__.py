import re

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME, CONF_PATH, CONF_PLATFORM
from esphome.core import CORE, CoroPriority, Lambda, coroutine_with_priority
from esphome.cpp_generator import StaticConstAssignmentExpression

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]


def AUTO_LOAD(config=None) -> list[str]:
    if config and (config.get("relay") or config.get("http_sources")):
        return ["relay_link"]
    return []
DOMAIN = "runtime_language"

CONF_MAX_FILE_SIZE = "max_file_size"
KEYS_SYMBOL = "esphome_runtime_language_keys"
SORTED_SYMBOL = "esphome_runtime_language_sorted"
CONF_DIRECTORY = "directory"
CONF_RELAY = "relay"
CONF_HTTP_SOURCES = "http_sources"

runtime_language_ns = cg.esphome_ns.namespace("runtime_language")
char_ptr = cg.global_ns.namespace("char *")
RuntimeLanguage = runtime_language_ns.class_("RuntimeLanguage", cg.Component)
LoadAction = runtime_language_ns.class_("LoadAction", automation.Action)
UnloadAction = runtime_language_ns.class_("UnloadAction", automation.Action)
ListAction = runtime_language_ns.class_("ListAction", automation.Action)
WriteTemplateAction = runtime_language_ns.class_("WriteTemplateAction", automation.Action)

def _storage_dir(value):
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a directory's storage path, as /sd/languages")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RuntimeLanguage),
        cv.Optional(CONF_MAX_FILE_SIZE, default="524288"): cv.All(
            cv.validate_bytes, cv.int_range(min=1024, max=4 * 1024 * 1024)
        ),
        cv.Optional(CONF_DIRECTORY): cv.All(cv.string_strict, _storage_dir),
        cv.Optional(CONF_RELAY, default=False): cv.boolean,
        cv.Optional(CONF_HTTP_SOURCES, default=[]): cv.All(cv.ensure_list(cv.url), cv.Length(max=4)),
    }
).extend(cv.COMPONENT_SCHEMA)

_TR_CALL = re.compile(r'(?<![A-Za-z0-9_])runtime_language::tr\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'"}


def _data():
    return CORE.data.setdefault(DOMAIN, {"keys": [], "index": {}})


def enabled() -> bool:
    return DOMAIN in CORE.config


def register_text(text: str) -> int | None:
    if not enabled() or not isinstance(text, str) or not any(c.isalpha() for c in text):
        return None
    data = _data()
    key = data["index"].get(text)
    if key is None:
        key = len(data["keys"])
        data["keys"].append(text)
        data["index"][text] = key
    return key


def key_expression(key: int):
    from esphome.components.package_manager.build import package_pass

    if package_pass():
        return cg.MockObj(str(runtime_language_ns.global_runtime_language), "->").english(key)
    return cg.MockObj(KEYS_SYMBOL)[key]


def bind_expression(label, key: int, copy: bool = False):
    if copy:
        return runtime_language_ns.bind_label_copy(label, key)
    return runtime_language_ns.bind_label(label, key)


def text_expression(key: int):
    return cg.MockObj(str(runtime_language_ns.global_runtime_language), "->").text(key)


def _unescape(literal: str) -> str | None:
    out = []
    i = 0
    while i < len(literal):
        c = literal[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        if i + 1 >= len(literal):
            return None
        c = literal[i + 1]
        width = {"u": 4, "U": 8, "x": 2}.get(c)
        if width is not None:
            digits = literal[i + 2 : i + 2 + width]
            if len(digits) != width or any(d not in "0123456789abcdefABCDEF" for d in digits):
                return None
            out.append(chr(int(digits, 16)))
            i += 2 + width
            continue
        if c not in _ESCAPES:
            return None
        out.append(_ESCAPES[c])
        i += 2
    return "".join(out)


def _scan(value):
    if isinstance(value, Lambda):
        for match in _TR_CALL.finditer(value.value):
            text = _unescape(match.group(1))
            if text is not None:
                register_text(text)
    elif isinstance(value, dict):
        if isinstance(value.get(CONF_NAME), str) and CONF_PLATFORM in value:
            register_text(value[CONF_NAME])
        for item in value.values():
            _scan(item)
    elif isinstance(value, (list, tuple)):
        for item in value:
            _scan(item)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_RUNTIME_LANGUAGE")
    cg.add(var.set_max_file_size(config[CONF_MAX_FILE_SIZE]))
    if CONF_DIRECTORY in config:
        cg.add(var.set_directory(config[CONF_DIRECTORY]))
    if config[CONF_RELAY] or config[CONF_HTTP_SOURCES]:
        from esphome.components import relay_link

        cg.add_define("USE_RUNTIME_LANGUAGE_LINK")
        link = await relay_link.get_relay_link()
        if config[CONF_RELAY]:
            cg.add(var.set_relay(link))
        for base in config[CONF_HTTP_SOURCES]:
            cg.add(var.add_http_source(link, base.rstrip("/")))
    if "lvgl" in CORE.config:
        lvgl_conf = CORE.config["lvgl"]
        lvgl_conf = lvgl_conf[0] if isinstance(lvgl_conf, list) else lvgl_conf
        cg.add(var.set_lvgl(await cg.get_variable(lvgl_conf[CONF_ID])))

    @coroutine_with_priority(CoroPriority.FINAL)
    async def _tables():
        _scan(CORE.config)
        found = _data()["keys"]
        keys = found or [""]
        order = sorted(range(len(keys)), key=lambda k: keys[k].encode("utf-8"))
        cg.add_global(
            StaticConstAssignmentExpression(
                char_ptr, KEYS_SYMBOL, cg.ArrayInitializer(*keys)
            )
        )
        cg.add_global(
            StaticConstAssignmentExpression(
                cg.uint16, SORTED_SYMBOL, cg.ArrayInitializer(*order)
            )
        )
        cg.add(
            var.set_keys(
                cg.MockObj(KEYS_SYMBOL), cg.MockObj(SORTED_SYMBOL), len(found)
            )
        )

    CORE.add_job(_tables)


LANGUAGE_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(RuntimeLanguage)})


@automation.register_action(
    "runtime_language.load",
    LoadAction,
    cv.All(
        LANGUAGE_ACTION_SCHEMA.extend(
            {
                cv.Exclusive(CONF_PATH, "source"): cv.templatable(cv.string),
                cv.Exclusive(CONF_NAME, "source"): cv.templatable(cv.string),
            }
        ),
        cv.has_exactly_one_key(CONF_PATH, CONF_NAME),
    ),
    synchronous=True,
)
async def load_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_NAME in config:
        cg.add(var.set_by_name(True))
        cg.add(var.set_name(await cg.templatable(config[CONF_NAME], args, cg.std_string)))
    else:
        cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


@automation.register_action("runtime_language.list", ListAction, LANGUAGE_ACTION_SCHEMA, synchronous=True)
async def list_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "runtime_language.unload", UnloadAction, LANGUAGE_ACTION_SCHEMA, synchronous=True
)
async def unload_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "runtime_language.write_template",
    WriteTemplateAction,
    LANGUAGE_ACTION_SCHEMA.extend({cv.Required(CONF_PATH): cv.templatable(cv.string)}),
    synchronous=True,
)
async def write_template_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var
