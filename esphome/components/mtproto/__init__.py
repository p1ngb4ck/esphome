import json
from pathlib import Path
import re

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, network
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_STATE, CONF_PASSWORD, PLATFORM_ESP32
from esphome.core import CORE
from esphome.types import ConfigType

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["sha256"]

CONF_MTPROTO_ID = "mtproto_id"
CONF_API_ID = "api_id"
CONF_API_HASH = "api_hash"
CONF_DEVICE_MODEL = "device_model"
CONF_APP_VERSION = "app_version"
CONF_LANG_CODE = "lang_code"

LAYER = 225
SCHEMA_FILES = ("mtproto.json", "api.json")

KIND_INT = 0
KIND_LONG = 1
KIND_DOUBLE = 2
KIND_INT128 = 3
KIND_INT256 = 4
KIND_BYTES = 5
KIND_TRUE = 6
KIND_FLAGS = 7
KIND_OBJECT = 8
KIND_VECTOR = 9
KIND_BARE = 10
KIND_BARE_VECTOR = 11
NO_FLAG = 0xFF
MAX_FLAGS = 4

SIMPLE_KINDS = {
    "int": KIND_INT,
    "long": KIND_LONG,
    "double": KIND_DOUBLE,
    "int128": KIND_INT128,
    "int256": KIND_INT256,
    "string": KIND_BYTES,
    "bytes": KIND_BYTES,
    "true": KIND_TRUE,
    "#": KIND_FLAGS,
}
FLAG_RE = re.compile(r"^(\w+)\.(\d+)\?(.+)$")

mtproto_ns = cg.esphome_ns.namespace("mtproto")
Mtproto = mtproto_ns.class_("Mtproto", cg.Component)
LogoutAction = mtproto_ns.class_("LogoutAction", automation.Action)
CheckPasswordAction = mtproto_ns.class_("CheckPasswordAction", automation.Action)
SetApiAction = mtproto_ns.class_("SetApiAction", automation.Action)


def _fnv1a(text: str) -> int:
    value = 2166136261
    for byte in text.encode():
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def _is_boxed(name: str) -> bool:
    return name.rsplit(".", 1)[-1][:1].isupper()


def _element(type_name: str, bare_ids: dict[str, int]) -> tuple[int, int]:
    if type_name in SIMPLE_KINDS:
        return SIMPLE_KINDS[type_name], 0
    bare = type_name[1:] if type_name.startswith("%") else type_name
    if type_name.startswith("%") or not _is_boxed(type_name):
        if bare not in bare_ids:
            raise cv.Invalid(f"mtproto schema: unknown bare type {type_name}")
        return KIND_BARE, bare_ids[bare]
    return KIND_OBJECT, 0


def _schema_table() -> list[int]:
    folder = Path(__file__).parent / "schema"
    constructors: dict[int, list[tuple[int, int, int, int, int, int]]] = {}
    for file_name in SCHEMA_FILES:
        data = json.loads((folder / file_name).read_text(encoding="utf-8"))
        bare_ids: dict[str, int] = {}
        for item in data["constructors"]:
            ident = int(item["id"]) & 0xFFFFFFFF
            bare_ids.setdefault(item["predicate"], ident)
            bare_ids.setdefault(item["type"], ident)
        for item in data["constructors"]:
            ident = int(item["id"]) & 0xFFFFFFFF
            if ident in constructors:
                continue
            slots = [param["name"] for param in item["params"] if param["type"] == "#"]
            if len(slots) > MAX_FLAGS:
                raise cv.Invalid(f"mtproto schema: {item['predicate']} has more than {MAX_FLAGS} flag fields")
            params = []
            for param in item["params"]:
                type_name = param["type"]
                flag_field, flag_bit = NO_FLAG, 0
                if type_name == "#":
                    flag_bit = slots.index(param["name"])
                elif match := FLAG_RE.match(type_name):
                    flag_field = slots.index(match.group(1))
                    flag_bit = int(match.group(2))
                    type_name = match.group(3)
                elem, ref = 0, 0
                if type_name.startswith(("Vector<", "vector<")):
                    kind = KIND_VECTOR if type_name[0] == "V" else KIND_BARE_VECTOR
                    elem, ref = _element(type_name[type_name.index("<") + 1 : -1], bare_ids)
                else:
                    kind, ref = _element(type_name, bare_ids)
                params.append((_fnv1a(param["name"]), kind, flag_field, flag_bit, elem, ref))
            constructors[ident] = params
    order = sorted(constructors)
    index = bytearray(len(order).to_bytes(4, "little"))
    blob = bytearray()
    base = 4 + 8 * len(order)
    for ident in order:
        index += ident.to_bytes(4, "little") + (base + len(blob)).to_bytes(4, "little")
        params = constructors[ident]
        blob.append(len(params))
        for name_hash, kind, flag_field, flag_bit, elem, ref in params:
            blob += name_hash.to_bytes(4, "little")
            blob += bytes((kind, flag_field, flag_bit, elem))
            blob += ref.to_bytes(4, "little")
    return list(index + blob)


def _api_hash(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9a-f]{32}", value):
        raise cv.Invalid("api_hash is the 32 hex characters from my.telegram.org")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Mtproto),
            cv.Optional(CONF_API_ID): cv.int_range(min=1, max=2**31 - 1),
            cv.Optional(CONF_API_HASH): _api_hash,
            cv.Optional(CONF_DEVICE_MODEL): cv.All(cv.string_strict, cv.Length(max=63)),
            cv.Optional(CONF_APP_VERSION, default="1.0"): cv.All(cv.string_strict, cv.Length(max=31)),
            cv.Optional(CONF_LANG_CODE, default="en"): cv.All(cv.string_strict, cv.Length(min=2, max=7)),
            cv.Optional(CONF_ON_STATE): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    cv.has_none_or_all_keys(CONF_API_ID, CONF_API_HASH),
)


async def to_code(config: ConfigType) -> None:
    create = cg.MockObj(str(Mtproto), "::").create()
    var = cg.Pvariable(config[CONF_ID], create)
    await cg.register_component(var, config)

    table = _schema_table()
    cg.extern_progmem_array("esphome::mtproto::TL_SCHEMA", cg.uint8, table)
    cg.add_define("MTPROTO_TL_SCHEMA_SIZE", len(table))
    cg.add_define("MTPROTO_LAYER", LAYER)

    if CONF_API_ID in config:
        cg.add(var.set_default_api(config[CONF_API_ID], config[CONF_API_HASH]))
    cg.add(
        var.set_client_info(
            config.get(CONF_DEVICE_MODEL, CORE.friendly_name or CORE.name),
            config[CONF_APP_VERSION],
            config[CONF_LANG_CODE],
        )
    )

    esp32.require_vfs_select()
    await network.register_active_interface_listener(var)

    if CONF_ON_STATE in config:
        await automation.build_automation(
            var.get_state_trigger(), [(cg.std_string, "state")], config[CONF_ON_STATE]
        )


MTPROTO_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(Mtproto)})


@automation.register_action("mtproto.logout", LogoutAction, MTPROTO_ACTION_SCHEMA, synchronous=True)
async def logout_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "mtproto.check_password",
    CheckPasswordAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(Mtproto),
            cv.Required(CONF_PASSWORD): cv.templatable(cv.string),
        },
        key=CONF_PASSWORD,
    ),
    synchronous=True,
)
async def check_password_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_password(await cg.templatable(config[CONF_PASSWORD], args, cg.std_string)))
    return var


@automation.register_action(
    "mtproto.set_api",
    SetApiAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(Mtproto),
            cv.Required(CONF_API_ID): cv.templatable(cv.int_range(min=1, max=2**31 - 1)),
            cv.Required(CONF_API_HASH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def set_api_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_api_id(await cg.templatable(config[CONF_API_ID], args, cg.int32)))
    cg.add(var.set_api_hash(await cg.templatable(config[CONF_API_HASH], args, cg.std_string)))
    return var
