import esphome.codegen as cg
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv

DEPENDENCIES = ["esp32", "psram"]

CONF_RECORDS = "records"
CONF_DEPTH = "depth"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_RECORDS, default=40000): cv.int_range(min=1000, max=200000),
        cv.Optional(CONF_DEPTH, default=10): cv.int_range(min=1, max=32),
    }
)


async def to_code(config):
    add_idf_sdkconfig_option("CONFIG_HEAP_TRACING_STANDALONE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_SYSTEM_USE_FRAME_POINTER", True)
    add_idf_sdkconfig_option("CONFIG_HEAP_TRACING_STACK_DEPTH", config[CONF_DEPTH])
    add_idf_sdkconfig_option("CONFIG_HEAP_TRACE_HASH_MAP", True)
    add_idf_sdkconfig_option("CONFIG_HEAP_TRACE_HASH_MAP_IN_EXT_RAM", True)
    add_idf_sdkconfig_option("CONFIG_HEAP_TRACE_HASH_MAP_SIZE", 2000)
    cg.add_define("USE_HEAP_TRACING_RECORDS", config[CONF_RECORDS])
