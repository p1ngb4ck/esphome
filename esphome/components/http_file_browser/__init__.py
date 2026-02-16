import esphome.codegen as cg
from esphome.components import storage, web_server_base
from esphome.components.esp32 import (
    add_idf_component,
    include_builtin_idf_component,
    require_vfs_dir,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, coroutine_with_priority

CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["storage", "web_server_base"]

# http_file_browser uses VFS directory functions (opendir, readdir, mkdir, rmdir)
# Ensure CONFIG_VFS_SUPPORT_DIR is enabled
require_vfs_dir()
# http_file_browser uses esp_vfs_fat_info() for disk space queries
include_builtin_idf_component("fatfs")
AUTO_LOAD = []

http_file_browser_ns = cg.esphome_ns.namespace("http_file_browser")
HttpFileBrowser = http_file_browser_ns.class_("HttpFileBrowser", cg.Component)

CONF_ROOT_PATH = "root_path"
CONF_URL_PREFIX = "url_prefix"
CONF_STORAGE_ID = "storage_id"
CONF_ENABLE_UPLOAD = "enable_upload"
CONF_ENABLE_DOWNLOAD = "enable_download"
CONF_ENABLE_DELETION = "enable_deletion"
CONF_AUTH_ENABLED = "auth_enabled"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"


def validate_auth(config):
    """Validate authentication configuration."""
    auth_enabled = config.get(CONF_AUTH_ENABLED, False)
    has_username = CONF_USERNAME in config
    has_password = CONF_PASSWORD in config

    if auth_enabled:
        # If auth is enabled, username and password are required
        if not has_username or not has_password:
            raise cv.Invalid(
                f"When {CONF_AUTH_ENABLED} is true, both {CONF_USERNAME} and {CONF_PASSWORD} are required"
            )
    # If auth is disabled, username and password must not be set
    elif has_username or has_password:
        raise cv.Invalid(
            f"{CONF_USERNAME} and {CONF_PASSWORD} can only be set when {CONF_AUTH_ENABLED} is true"
        )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HttpFileBrowser),
            cv.GenerateID(web_server_base.CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.Required(CONF_STORAGE_ID): cv.use_id(
                storage.Storage
            ),  # Reference to storage (REQUIRED)
            cv.Optional(CONF_ROOT_PATH, default="/"): cv.string,
            cv.Optional(CONF_URL_PREFIX, default="/files"): cv.string,
            cv.Optional(CONF_ENABLE_UPLOAD, default=False): cv.boolean,
            cv.Optional(CONF_ENABLE_DOWNLOAD, default=True): cv.boolean,
            cv.Optional(CONF_ENABLE_DELETION, default=False): cv.boolean,
            cv.Optional(CONF_AUTH_ENABLED, default=False): cv.boolean,
            cv.Optional(CONF_USERNAME): cv.string,
            cv.Optional(CONF_PASSWORD): cv.string,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_auth,
)


@coroutine_with_priority(45.0)
async def to_code(config):
    cg.add_define("USE_HTTP_FILE_BROWSER")
    cg.add_define("USE_WEBSERVER_OTA")  # Enable multipart upload support
    if CORE.is_esp32:
        add_idf_component(name="zorxx/multipart-parser", ref="1.0.1")

        # Check if PSRAM is guaranteed to be available for larger buffers
        # Import here to avoid circular dependency
        from esphome.components import psram

        if psram.is_guaranteed():
            cg.add_define("HTTP_FILE_BROWSER_USE_PSRAM")

    # Get web_server_base instance
    web_server_base_var = await cg.get_variable(
        config[web_server_base.CONF_WEB_SERVER_BASE_ID]
    )

    # Create HttpFileBrowser with web_server_base as constructor parameter
    var = cg.new_Pvariable(config[CONF_ID], web_server_base_var)
    await cg.register_component(var, config)

    # Get storage instance
    storage_var = await cg.get_variable(config[CONF_STORAGE_ID])
    cg.add(var.set_storage(storage_var))

    # Set configuration
    root_path = config[CONF_ROOT_PATH]
    url_prefix = config[CONF_URL_PREFIX]
    enable_upload = config[CONF_ENABLE_UPLOAD]
    enable_download = config[CONF_ENABLE_DOWNLOAD]
    enable_deletion = config[CONF_ENABLE_DELETION]

    cg.add(var.set_root_path(root_path))
    cg.add(var.set_url_prefix(url_prefix))
    cg.add(var.set_upload_enabled(enable_upload))
    cg.add(var.set_download_enabled(enable_download))
    cg.add(var.set_deletion_enabled(enable_deletion))

    # Optional authentication
    if config.get(CONF_AUTH_ENABLED, False):
        cg.add_define("USE_WEBSERVER_AUTH")
        cg.add(var.set_auth(config[CONF_USERNAME], config[CONF_PASSWORD]))
