import esphome.codegen as cg
from esphome.components import light, spi
import esphome.config_validation as cv
from esphome.const import CONF_CHIPSET, CONF_NUM_LEDS, CONF_OUTPUT_ID, CONF_RGB_ORDER
from esphome.types import ConfigType

spi_led_strip_ns = cg.esphome_ns.namespace("spi_led_strip")
SpiLedStrip = spi_led_strip_ns.class_(
    "SpiLedStrip", light.AddressableLight, spi.SPIDevice
)

CHIPSETS = {
    "APA102": False,
    "SK9822": False,
    "WS2801": True,
}

RGB_ORDERS = {
    "RGB": (0, 1, 2),
    "RBG": (0, 2, 1),
    "GRB": (1, 0, 2),
    "GBR": (1, 2, 0),
    "BRG": (2, 0, 1),
    "BGR": (2, 1, 0),
}

CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(SpiLedStrip),
        cv.Optional(CONF_NUM_LEDS, default=1): cv.positive_not_null_int,
        cv.Optional(CONF_CHIPSET, default="APA102"): cv.one_of(*CHIPSETS, upper=True),
        cv.Optional(CONF_RGB_ORDER): cv.one_of(*RGB_ORDERS, upper=True),
    }
).extend(spi.spi_device_schema(False, "1MHz"))


async def to_code(config: ConfigType) -> None:
    ws2801 = CHIPSETS[config[CONF_CHIPSET]]
    default_order = "RGB" if ws2801 else "BGR"
    r, g, b = RGB_ORDERS[config.get(CONF_RGB_ORDER, default_order)]
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID], config[CONF_NUM_LEDS], ws2801, r, g, b)
    await light.register_light(var, config)
    await spi.register_spi_device(var, config)
    await cg.register_component(var, config)
