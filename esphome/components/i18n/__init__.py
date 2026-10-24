import re

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PATH
from esphome.core import CORE, CoroPriority, Lambda, coroutine_with_priority
from esphome.cpp_generator import RawStatement
from esphome.helpers import cpp_string_escape

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]
DOMAIN = "i18n"

CONF_MAX_FILE_SIZE = "max_file_size"
KEYS_SYMBOL = "esphome_i18n_keys"
SORTED_SYMBOL = "esphome_i18n_sorted"

i18n_ns = cg.esphome_ns.namespace("i18n")
I18nComponent = i18n_ns.class_("I18nComponent", cg.Component)
LoadAction = i18n_ns.class_("LoadAction", automation.Action)
UnloadAction = i18n_ns.class_("UnloadAction", automation.Action)
WriteTemplateAction = i18n_ns.class_("WriteTemplateAction", automation.Action)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(I18nComponent),
        cv.Optional(CONF_MAX_FILE_SIZE, default="512KB"): cv.All(
            cv.validate_bytes, cv.int_range(min=1024, max=4 * 1024 * 1024)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

_TR_CALL = re.compile(r'(?<![A-Za-z0-9_])i18n::tr\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
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


def key_expression(key: int) -> str:
    return f"{KEYS_SYMBOL}[{key}]"


def _unescape(literal: str) -> str | None:
    out = []
    i = 0
    while i < len(literal):
        c = literal[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        if i + 1 >= len(literal) or literal[i + 1] not in _ESCAPES:
            return None
        out.append(_ESCAPES[literal[i + 1]])
        i += 2
    return "".join(out)


def _scan(value):
    if isinstance(value, Lambda):
        for match in _TR_CALL.finditer(value.value):
            text = _unescape(match.group(1))
            if text is not None:
                register_text(text)
    elif isinstance(value, dict):
        for item in value.values():
            _scan(item)
    elif isinstance(value, (list, tuple)):
        for item in value:
            _scan(item)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_I18N")
    cg.add(var.set_max_file_size(config[CONF_MAX_FILE_SIZE]))
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
            RawStatement(
                f"const char *const {KEYS_SYMBOL}[] = {{"
                + ", ".join(cpp_string_escape(k) for k in keys)
                + "};"
            )
        )
        cg.add_global(
            RawStatement(
                f"const uint16_t {SORTED_SYMBOL}[] = {{"
                + ", ".join(str(k) for k in order)
                + "};"
            )
        )
        cg.add(
            var.set_keys(
                cg.RawExpression(KEYS_SYMBOL),
                cg.RawExpression(SORTED_SYMBOL),
                len(found),
            )
        )

    CORE.add_job(_tables)


I18N_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(I18nComponent)})


@automation.register_action(
    "i18n.load",
    LoadAction,
    I18N_ACTION_SCHEMA.extend({cv.Required(CONF_PATH): cv.templatable(cv.string)}),
    synchronous=True,
)
async def load_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


@automation.register_action(
    "i18n.unload", UnloadAction, I18N_ACTION_SCHEMA, synchronous=True
)
async def unload_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "i18n.write_template",
    WriteTemplateAction,
    I18N_ACTION_SCHEMA.extend({cv.Required(CONF_PATH): cv.templatable(cv.string)}),
    synchronous=True,
)
async def write_template_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var
