import esphome.codegen as cg
from esphome.components import select as select_domain, text_sensor as text_sensor_domain
from esphome.components.hid import layouts as hid_layouts
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage", "hid"]

CONF_KEYBOARDS = "keyboards"
CONF_LAYOUT = "layout"
CONF_DIRECTORY = "directory"
CONF_DEVICE_SELECT = "device_select"
CONF_LAYOUT_SENSOR = "layout_sensor"
CONF_RELAY = "relay"
CONF_HTTP_SOURCES = "http_sources"


def AUTO_LOAD(config=None) -> list[str]:
    load = ["fixed_containers"]
    if config and (config.get(CONF_RELAY) or config.get(CONF_HTTP_SOURCES)):
        load.append("relay_link")
    if config and CONF_DEVICE_SELECT in config:
        load.append("select")
    if config and CONF_LAYOUT_SENSOR in config:
        load.append("text_sensor")
    return load


keyboard_layouts_ns = cg.esphome_ns.namespace("keyboard_layouts")
KeyboardLayouts = keyboard_layouts_ns.class_("KeyboardLayouts", cg.Component)
KeyboardDeviceSelect = keyboard_layouts_ns.class_("KeyboardDeviceSelect", select_domain.Select)
HIDInput = cg.esphome_ns.namespace("hid").class_("HIDInput")


def _layout_name(value):
    value = cv.string_strict(value)
    if value:
        try:
            hid_layouts.resolve(value)
        except KeyError as err:
            raise cv.Invalid(f"{value} is not a keyboard layout (an XKB name as de or de(nodeadkeys))") from err
    return value


def _keyboard(value):
    if not isinstance(value, dict):
        value = {CONF_ID: value}
    return cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(HIDInput),
            cv.Optional(CONF_NAME): cv.string_strict,
            cv.Optional(CONF_LAYOUT, default=""): _layout_name,
        }
    )(value)


def _storage_dir(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a directory's storage path, as /sd/keyboard")
    return value


def layouts_schema(required=cv.Required):
    return cv.Schema(
        {
            required(CONF_KEYBOARDS): cv.ensure_list(_keyboard),
            cv.Optional(CONF_DIRECTORY): _storage_dir,
            cv.Optional(CONF_DEVICE_SELECT): select_domain.select_schema(KeyboardDeviceSelect, icon="mdi:keyboard"),
            cv.Optional(CONF_LAYOUT_SENSOR): text_sensor_domain.text_sensor_schema(icon="mdi:keyboard-settings"),
        }
    )


def _standalone(config):
    sources = CONF_DIRECTORY in config or config.get(CONF_RELAY) or config.get(CONF_HTTP_SOURCES)
    if CONF_KEYBOARDS in config and not sources:
        raise cv.Invalid(f"a source of layouts: {CONF_DIRECTORY}, {CONF_RELAY} and / or {CONF_HTTP_SOURCES}")
    if CONF_KEYBOARDS not in config and (
        sources or any(k in config for k in (CONF_DEVICE_SELECT, CONF_LAYOUT_SENSOR))
    ):
        raise cv.Invalid(f"{CONF_KEYBOARDS} is required")
    return config


CONFIG_SCHEMA = cv.All(
    layouts_schema(cv.Optional)
    .extend(
        {
            cv.GenerateID(): cv.declare_id(KeyboardLayouts),
            cv.Optional(CONF_RELAY, default=False): cv.boolean,
            cv.Optional(CONF_HTTP_SOURCES, default=[]): cv.All(cv.ensure_list(cv.url), cv.Length(max=4)),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _standalone,
)


async def new_layouts(config, layouts_id, relay=None):
    request_storage_worker()
    cg.add_define("USE_KEYBOARD_LAYOUTS")
    var = cg.Pvariable(layouts_id, cg.MockObj(str(KeyboardLayouts), "::").create())
    await cg.register_component(var, config)
    sources = config.get(CONF_HTTP_SOURCES, [])
    if relay is not None or sources:
        cg.add_define("USE_KEYBOARD_LAYOUTS_LINK")
    if relay is not None:
        cg.add(var.set_relay(relay))
    if sources:
        from esphome.components import relay_link

        link = await relay_link.get_relay_link()
        for base in sources:
            cg.add(var.add_http_source(link, base.rstrip("/")))
    if CONF_DIRECTORY in config:
        cg.add(var.set_keyboard_directory(config[CONF_DIRECTORY]))
    for keyboard in config[CONF_KEYBOARDS]:
        input_var = await cg.get_variable(keyboard[CONF_ID])
        name = keyboard.get(CONF_NAME, str(keyboard[CONF_ID]))
        cg.add(var.add_keyboard(input_var, name, keyboard[CONF_LAYOUT]))
    if device_select := config.get(CONF_DEVICE_SELECT):
        cg.add_define("USE_KEYBOARD_LAYOUTS_ENTITIES")
        sel = await select_domain.new_select(device_select, options=[])
        cg.add(sel.set_parent(var))
        cg.add(var.set_keyboard_select(sel))
    if layout_sensor := config.get(CONF_LAYOUT_SENSOR):
        cg.add_define("USE_KEYBOARD_LAYOUTS_ENTITIES")
        sensor = await text_sensor_domain.new_text_sensor(layout_sensor)
        cg.add(var.set_keyboard_layout_sensor(sensor))
    return var


async def to_code(config):
    if CONF_KEYBOARDS not in config:
        return
    relay = None
    if config[CONF_RELAY]:
        from esphome.components import relay_link

        relay = await relay_link.get_relay_link()
    await new_layouts(config, config[CONF_ID], relay)
