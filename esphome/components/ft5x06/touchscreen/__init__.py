from esphome import pins
import esphome.codegen as cg
from esphome.components import i2c, touchscreen
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_INTERRUPT_PIN
from esphome.types import ConfigType

from .. import ft5x06_ns

CONF_REGISTER_COPY = "register_copy"
CONF_BYTE_READS = "byte_reads"
CONF_EDT_READS = "edt_reads"

FT5x06ButtonListener = ft5x06_ns.class_("FT5x06ButtonListener")
FT5x06Touchscreen = ft5x06_ns.class_(
    "FT5x06Touchscreen",
    touchscreen.Touchscreen,
    cg.Component,
    i2c.I2CDevice,
)

CONFIG_SCHEMA = touchscreen.TOUCHSCREEN_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(FT5x06Touchscreen),
        cv.Optional(CONF_INTERRUPT_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_REGISTER_COPY, default=False): cv.boolean,
        cv.Optional(CONF_BYTE_READS, default=False): cv.boolean,
        cv.Optional(CONF_EDT_READS, default=False): cv.boolean,
    }
).extend(i2c.i2c_device_schema(0x48))


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await i2c.register_i2c_device(var, config)
    await touchscreen.register_touchscreen(var, config)

    await cg.add_pin(var.set_interrupt_pin, config.get(CONF_INTERRUPT_PIN))
    cg.add(var.set_register_copy(config[CONF_REGISTER_COPY]))
    cg.add(var.set_byte_reads(config[CONF_BYTE_READS]))
    cg.add(var.set_edt_reads(config[CONF_EDT_READS]))
