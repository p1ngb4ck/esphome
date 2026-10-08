from esphome.components.esp32 import (
    include_builtin_idf_component,
    set_idf_sdkconfig_default,
)
from esphome.config_helpers import filter_source_files_from_defines
import esphome.config_validation as cv
from esphome.types import ConfigType

CODEOWNERS = ["@dentra"]

CONFIG_SCHEMA = cv.All(
    cv.Schema({}),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    # Increase the maximum supported size of headers section in HTTP request packet to be processed by the server
    set_idf_sdkconfig_default("CONFIG_HTTPD_MAX_REQ_HDR_LEN", 1024)
    # Re-enable ESP-IDF's HTTP server (excluded by default to save compile time).
    # Basic auth uses mbedtls_base64_encode directly, so no TLS stack is needed.
    include_builtin_idf_component("esp_http_server")


# multipart.cpp is #ifdef'd on USE_WEBSERVER_OTA (set by the web_server OTA
# platform) OR USE_WEBSERVER_FILE_API (set by web_server -> file_api, whose
# /files/upload handler drives the same parser); skip it only when neither
# consumer is configured.
FILTER_SOURCE_FILES = filter_source_files_from_defines(
    {"multipart.cpp": ("USE_WEBSERVER_OTA", "USE_WEBSERVER_FILE_API")}
)
