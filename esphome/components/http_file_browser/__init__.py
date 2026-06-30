import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.esp32 import add_idf_component
from esphome.components.storage import request_storage_device
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_USERNAME

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["web_server_base", "network"]
AUTO_LOAD = ["storage"]

CONF_ROOT_PATH = "root_path"
CONF_URL_PREFIX = "url_prefix"
CONF_UPLOAD_ENABLED = "upload_enabled"
CONF_DOWNLOAD_ENABLED = "download_enabled"
CONF_DELETION_ENABLED = "deletion_enabled"
CONF_AUTH_ENABLED = "auth_enabled"

http_file_browser_ns = cg.esphome_ns.namespace("http_file_browser")
HttpFileBrowser = http_file_browser_ns.class_("HttpFileBrowser", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HttpFileBrowser),
        cv.GenerateID(web_server_base.CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.Optional(CONF_ROOT_PATH, default="/"): cv.string,
        cv.Optional(CONF_URL_PREFIX, default="/files"): cv.string,
        cv.Optional(CONF_UPLOAD_ENABLED, default=False): cv.boolean,
        cv.Optional(CONF_DOWNLOAD_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_DELETION_ENABLED, default=False): cv.boolean,
        cv.Optional(CONF_AUTH_ENABLED, default=False): cv.boolean,
        cv.Optional(CONF_USERNAME): cv.string,
        cv.Optional(CONF_PASSWORD): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    web_server_base_var = await cg.get_variable(
        config[web_server_base.CONF_WEB_SERVER_BASE_ID]
    )
    var = cg.new_Pvariable(config[CONF_ID], web_server_base_var)
    await cg.register_component(var, config)

    cg.add(var.set_root_path(config[CONF_ROOT_PATH]))
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    cg.add(var.set_upload_enabled(config[CONF_UPLOAD_ENABLED]))
    cg.add(var.set_download_enabled(config[CONF_DOWNLOAD_ENABLED]))
    cg.add(var.set_deletion_enabled(config[CONF_DELETION_ENABLED]))

    if config[CONF_AUTH_ENABLED]:
        if (username := config.get(CONF_USERNAME)) is not None and (
            password := config.get(CONF_PASSWORD)
        ) is not None:
            cg.add(var.set_auth(username, password))

    cg.add_define("USE_HTTP_FILE_BROWSER")

    add_idf_component(name="zorxx/multipart-parser", ref="1.0.1")

    request_storage_device()
