import esphome.codegen as cg
from esphome.components import text
import esphome.config_validation as cv
from esphome.const import CONF_INITIAL_VALUE, CONF_KEY, CONF_MAX_LENGTH, CONF_MIN_LENGTH, CONF_PATTERN

from .. import nvs_ns

CONF_NAMESPACE = "namespace"

NvsText = nvs_ns.class_("NvsText", text.Text, cg.Component)

CONFIG_SCHEMA = cv.All(
    text.text_schema(NvsText)
    .extend(
        {
            cv.Required(CONF_NAMESPACE): cv.All(cv.string_strict, cv.Length(min=1, max=15)),
            cv.Required(CONF_KEY): cv.All(cv.string_strict, cv.Length(min=1, max=15)),
            cv.Optional(CONF_MIN_LENGTH, default=0): cv.uint8_t,
            cv.Optional(CONF_MAX_LENGTH, default=255): cv.uint8_t,
            cv.Optional(CONF_PATTERN): cv.string,
            cv.Optional(CONF_INITIAL_VALUE, default=""): cv.string_strict,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = await text.new_text(
        config,
        min_length=config[CONF_MIN_LENGTH],
        max_length=config[CONF_MAX_LENGTH],
        pattern=config.get(CONF_PATTERN),
    )
    await cg.register_component(var, config)
    cg.add(var.set_location(config[CONF_NAMESPACE], config[CONF_KEY]))
    cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))
