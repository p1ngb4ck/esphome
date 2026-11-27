"""Speaker Media Player Base - Shared implementation."""

from esphome.components import ota
import esphome.config_validation as cv

CODEOWNERS = ["@kahrendt", "@synesthesiam"]

# This component only provides C++ implementation files
# It has no configuration schema - it's purely a code library

CONFIG_SCHEMA = cv.All(
    cv.Schema({}),
)


async def to_code(config):
    ota.request_ota_state_listeners()
