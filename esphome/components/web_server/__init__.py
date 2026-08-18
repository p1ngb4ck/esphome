from __future__ import annotations

import base64
import gzip
import json
import logging
from pathlib import Path
import re

import esphome.codegen as cg
from esphome.components import storage, web_server_base
from esphome.components.esp32 import add_idf_component
from esphome.components.logger import request_log_listener
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import (
    CONF_AUTH,
    CONF_COMPRESSION,
    CONF_CSS_INCLUDE,
    CONF_CSS_URL,
    CONF_ENABLE_PRIVATE_NETWORK_ACCESS,
    CONF_ID,
    CONF_INCLUDE_INTERNAL,
    CONF_JS_INCLUDE,
    CONF_JS_URL,
    CONF_LOCAL,
    CONF_LOG,
    CONF_NAME,
    CONF_OTA,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_TYPE,
    CONF_USERNAME,
    CONF_VERSION,
    CONF_WEB_SERVER,
    CONF_WEB_SERVER_ID,
    PLATFORM_BK72XX,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_LN882X,
    PLATFORM_RP2,
    PLATFORM_RTL87XX,
)
from esphome.core import CORE, ID, CoroPriority, coroutine_with_priority
import esphome.final_validate as fv
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

AUTH_TYPE_BASIC = "basic"
AUTH_TYPE_DIGEST = "digest"

CONF_SORTING_GROUP_ID = "sorting_group_id"
CONF_SORTING_GROUPS = "sorting_groups"
CONF_SORTING_WEIGHT = "sorting_weight"
CONF_ALLOWED_ORIGINS = "allowed_origins"

# Not yet in esphome/const.py
CONF_FILE_API = "file_api"

FILE_API_DEFAULTS = {
    "max_dir_entries": 64,
    "enable_list": True,
    "enable_read": True,
    "enable_write": True,
    "enable_delete": True,
    "enable_mount": True,
    "enable_unmount": True,
    "enable_format": False,
}
CONF_FILE_BROWSER = "file_browser"
CONF_ACTIONS_AS_ICONS = "actions_as_icons"
CONF_CHANGE_POLL_INTERVAL = "change_poll_interval"
CONF_VARIANT = "variant"
CONF_ASSET_SOURCE = "asset_source"
CONF_ASSET_PATH = "asset_path"
CONF_TEXT_FILE_FORMATS = "text_file_formats"

BROWSER_SIMPLE = "simple"
BROWSER_ADVANCED = "advanced"
ASSETS_FLASH = "flash"
ASSETS_STORAGE = "storage"

# The adapter is ours and versioned with the component: it talks to whatever /files/* this
# firmware exposes. Putting it on removable media would let a firmware update move the API
# while the adapter stayed behind, so it is always compiled in -- roughly 4 kB gzipped.
# (file in this directory, URL, content type, compress)
_ADAPTER_ASSETS = (
    ("file_explorer/adapter.js", "/file-explorer/adapter.js", "text/javascript", True),
    ("file_explorer/adapter.css", "/file-explorer/adapter.css", "text/css", True),
)

# The widget itself (cubiclesoft/js-fileexplorer, unmodified) is what asset_source decides
# about. With asset_source: storage these files are not read at codegen time at all -- they
# live on the medium and the paths below are just strings -- so a user who does not want ~56 kB
# of third-party code in the repository does not need it there. Either way the widget is MIT,
# and its terms are in file_explorer/LICENSE.txt -- upstream ships no LICENSE file, so that
# copy is the one that travels with the code.
_WIDGET_ASSETS = (
    (
        "file_explorer/file-explorer.js",
        "/file-explorer/file-explorer.js",
        "text/javascript",
        True,
    ),
    (
        "file_explorer/file-explorer.css",
        "/file-explorer/file-explorer.css",
        "text/css",
        True,
    ),
    (
        "file_explorer/fileexplorer_sprites.png",
        "/file-explorer/fileexplorer_sprites.png",
        "image/png",
        False,
    ),
    (
        "file_explorer/fileexplorer_actions.woff",
        "/file-explorer/fileexplorer_actions.woff",
        "font/woff",
        False,
    ),
)
CONF_MAX_DIR_ENTRIES = "max_dir_entries"
CONF_STORAGE_ID = "storage_id"
# file_api per-operation access options. Each gates a group of endpoints server-side (a
# disallowed call is rejected with 403) and is advertised to the browser so it never renders a
# button that could only fail. All default to True (no behaviour change for existing configs).
CONF_ENABLE_LIST = "enable_list"  # /files/list, /files/stat
CONF_ENABLE_READ = "enable_read"  # /files/download, /files/copy (copy is a read)
CONF_ENABLE_WRITE = "enable_write"  # /files/upload, /files/mkdir (shared with raw_api)
CONF_ENABLE_DELETE = "enable_delete"  # /files/delete, /files/move (move deletes source)
CONF_ENABLE_MOUNT = "enable_mount"  # /files/mount
CONF_ENABLE_UNMOUNT = "enable_unmount"  # /files/unmount
CONF_ENABLE_FORMAT = "enable_format"  # /files/format (destructive; default off)
CONF_RAW_API = "raw_api"
CONF_DEVICE_ID = "device_id"


def AUTO_LOAD(config: ConfigType) -> list[str]:
    """web_server always needs json/web_server_base; storage is pulled in only when the
    optional file_api:/file_browser: sub-blocks are actually present in this instance's
    config — a web_server without them pays nothing for the feature."""
    base = ["json", "web_server_base"]
    if config and (
        CONF_FILE_API in config or CONF_FILE_BROWSER in config or CONF_RAW_API in config
    ):
        return base + ["storage"]
    return base


web_server_ns = cg.esphome_ns.namespace("web_server")
WebServer = web_server_ns.class_("WebServer", cg.Component, cg.Controller)

WebServerFileApi = web_server_ns.class_("WebServerFileApi", cg.Component)
WebServerRawApi = web_server_ns.class_("WebServerRawApi", cg.Component)
FileExplorerAssets = web_server_ns.class_("FileExplorerAssets", cg.Component)

sorting_groups = {}


def default_url(config: ConfigType) -> ConfigType:
    config = config.copy()
    if config[CONF_VERSION] == 1:
        if CONF_CSS_URL not in config:
            config[CONF_CSS_URL] = "https://oi.esphome.io/v1/webserver-v1.min.css"
        if CONF_JS_URL not in config:
            config[CONF_JS_URL] = "https://oi.esphome.io/v1/webserver-v1.min.js"
    if config[CONF_VERSION] == 2:
        if CONF_CSS_URL not in config:
            config[CONF_CSS_URL] = ""
        if CONF_JS_URL not in config:
            config[CONF_JS_URL] = "https://oi.esphome.io/v2/www.js"
    if config[CONF_VERSION] == 3:
        if CONF_CSS_URL not in config:
            config[CONF_CSS_URL] = ""
        if CONF_JS_URL not in config:
            config[CONF_JS_URL] = "https://oi.esphome.io/v3/www.js"
    return config


def validate_version_deprecated(config: ConfigType) -> ConfigType:
    if config[CONF_VERSION] == 1:
        _LOGGER.warning(
            "Version 1 of 'web_server' is deprecated and will be removed in "
            "2027.1.0. Please migrate to version 2 (the default) or version 3."
        )
    return config


def validate_auth_type_deprecated(auth: ConfigType) -> ConfigType:
    # Remove before 2027.1.0: the default auth scheme changes from basic to digest.
    if CONF_TYPE not in auth:
        _LOGGER.warning(
            "The 'web_server' 'auth' scheme currently defaults to 'basic', which sends the "
            "password over the network in an easily reversible form. The default will change "
            "to 'digest' in ESPHome 2027.1.0. To keep using basic authentication, set "
            "'type: basic' under 'auth:' explicitly; otherwise set 'type: digest' now to "
            "adopt the more secure scheme."
        )
    return auth


def validate_local(config: ConfigType) -> ConfigType:
    if CONF_LOCAL in config and config[CONF_VERSION] == 1:
        raise cv.Invalid("'local' is not supported in version 1")
    return config


def validate_ota(config: ConfigType) -> ConfigType:
    # The OTA option only accepts False to explicitly disable OTA for web_server
    # IMPORTANT: Setting ota: false ONLY affects the web_server component
    # The captive_portal component will still be able to perform OTA updates
    if CONF_OTA in config and config[CONF_OTA] is not False:
        raise cv.Invalid(
            f"The '{CONF_OTA}' option in 'web_server' only accepts 'false' to disable OTA. "
            f"To enable OTA, please use the new OTA platform structure instead:\n\n"
            f"ota:\n"
            f"  - platform: web_server\n\n"
            f"See https://esphome.io/components/ota for more information."
        )
    return config


# An Origin header is always "scheme://host[:port]" with no path or trailing slash.
_ORIGIN_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*://[^/\s]+$")


def validate_origin(value: str) -> str:
    # "*" is the wildcard that allows any origin.
    if value == "*":
        return value
    value = cv.string_strict(value)
    if not _ORIGIN_RE.match(value):
        raise cv.Invalid(
            f"'{value}' is not a valid origin. An origin must be 'scheme://host[:port]' with no "
            f"path or trailing slash (e.g. 'https://example.com'), or '*' to allow any origin."
        )
    # Browsers send the scheme and host lowercased in the Origin header, so normalize to match.
    return value.lower()


def validate_private_network_access(config: ConfigType) -> ConfigType:
    # PNA preflights are always cross-origin, so they can only be authorized against the
    # allowed_origins list. Enabling PNA without any origins would deny every PNA request.
    if (
        config[CONF_ENABLE_PRIVATE_NETWORK_ACCESS]
        and config.get(CONF_ALLOWED_ORIGINS) is None
    ):
        raise cv.Invalid(
            f"'{CONF_ALLOWED_ORIGINS}' must be set when "
            f"'{CONF_ENABLE_PRIVATE_NETWORK_ACCESS}' is enabled. List each origin that is "
            f"allowed to reach the device (e.g. 'https://example.com'). '*' allows any origin "
            f"but is not recommended.",
            path=[CONF_ENABLE_PRIVATE_NETWORK_ACCESS],
        )
    return config


def validate_sorting_groups(config: ConfigType) -> ConfigType:
    if CONF_SORTING_GROUPS in config and config[CONF_VERSION] != 3:
        raise cv.Invalid(
            f"'{CONF_SORTING_GROUPS}' is only supported in 'web_server' version 3"
        )
    return config


def _validate_no_sorting_component(
    sorting_component: str,
    webserver_version: int,
    config: ConfigType,
    path: list[str] | None = None,
) -> None:
    if path is None:
        path = []
    if CONF_WEB_SERVER in config and sorting_component in config[CONF_WEB_SERVER]:
        raise cv.FinalExternalInvalid(
            f"{sorting_component} on entities is not supported in web_server version {webserver_version}",
            path=path + [sorting_component],
        )
    for p, value in config.items():
        if isinstance(value, dict):
            _validate_no_sorting_component(
                sorting_component, webserver_version, value, path + [p]
            )
        elif isinstance(value, list):
            for i, item in enumerate(value):
                if isinstance(item, dict):
                    _validate_no_sorting_component(
                        sorting_component, webserver_version, item, path + [p, i]
                    )


def _final_validate_sorting(config: ConfigType) -> None:
    if (webserver_version := config.get(CONF_VERSION)) != 3:
        _validate_no_sorting_component(
            CONF_SORTING_WEIGHT, webserver_version, fv.full_config.get()
        )
        _validate_no_sorting_component(
            CONF_SORTING_GROUP_ID, webserver_version, fv.full_config.get()
        )


def _final_validate_file_explorer(config: ConfigType) -> None:
    """The advanced browser serves its assets out of external RAM in both modes.

    There is no variant that trades PSRAM away -- flash and storage differ in where the bytes
    come from, not in where they are served from -- so psram is required either way. Same shape
    of check as storage's transfer_buffer, and for the same reason: the option promises
    something only the hardware can deliver.
    """
    browser = config.get(CONF_FILE_BROWSER)
    if browser is None or browser.get(CONF_VARIANT) != BROWSER_ADVANCED:
        return config
    full = fv.full_config.get()
    if "psram" not in full:
        raise cv.Invalid(
            f"'{CONF_FILE_BROWSER}: {CONF_VARIANT}: {BROWSER_ADVANCED}' serves its assets from "
            f"external RAM and needs the 'psram' component configured",
            path=[CONF_FILE_BROWSER, CONF_VARIANT],
        )
    if browser.get(CONF_ASSET_SOURCE) == ASSETS_STORAGE and "storage" not in full:
        raise cv.Invalid(
            f"'{CONF_ASSET_SOURCE}: {ASSETS_STORAGE}' needs the 'storage' component to read the "
            f"assets from",
            path=[CONF_FILE_BROWSER, CONF_ASSET_SOURCE],
        )
    return config


FINAL_VALIDATE_SCHEMA = cv.All(
    _final_validate_sorting,
    _final_validate_file_explorer,
)


def _consume_web_server_sockets(config: ConfigType) -> ConfigType:
    """Register socket needs for web_server component."""
    from esphome.components import socket

    # Web server needs typically 5 concurrent client connections
    # (browser opens connections for page resources, SSE event stream, and POST
    # requests for entity control which may linger before closing)
    # The listening socket is registered by web_server_base (shared with captive_portal)
    socket.consume_sockets(5, "web_server")(config)
    return config


sorting_group = {
    cv.Required(CONF_ID): cv.declare_id(cg.int_),
    cv.Required(CONF_NAME): cv.string,
    cv.Optional(CONF_SORTING_WEIGHT): cv.float_,
}

WEBSERVER_SORTING_SCHEMA = cv.Schema(
    {
        # The per-entity web_server block is cosmetic dashboard ordering —
        # mark the whole block advanced; the children inherit via the cascade.
        cv.Optional(CONF_WEB_SERVER, visibility=cv.Visibility.ADVANCED): cv.Schema(
            {
                cv.OnlyWith(CONF_WEB_SERVER_ID, "web_server"): cv.use_id(WebServer),
                cv.Optional(CONF_SORTING_WEIGHT): cv.All(
                    cv.requires_component("web_server"),
                    cv.float_,
                ),
                cv.Optional(CONF_SORTING_GROUP_ID): cv.All(
                    cv.requires_component("web_server"),
                    cv.use_id(cg.int_),
                ),
            }
        )
    }
)


def _text_file_format(value):
    value = cv.string(value).strip().lower()
    return value if value.startswith(".") else "." + value


def _validate_file_browser(config):
    # text_file_formats decides which files the advanced widget treats as text (its editor
    # and the "monitor" tail button). It has no effect on the simple browser, so it is
    # advanced-only.
    if config[CONF_VARIANT] == BROWSER_ADVANCED:
        config.setdefault(CONF_TEXT_FILE_FORMATS, [".txt", ".log"])
    elif CONF_TEXT_FILE_FORMATS in config:
        raise cv.Invalid(
            f"'{CONF_TEXT_FILE_FORMATS}' only applies to '{CONF_VARIANT}: {BROWSER_ADVANCED}'",
            path=[CONF_TEXT_FILE_FORMATS],
        )
    return config


def _validate_file_api(config):
    """file_api/file_browser are ESP-IDF + web_server version 3 only: the REST handlers use
    the IDF httpd backend directly (chunked responses, multipart upload), and the browser
    module hooks into the v3 page."""
    if CONF_FILE_API not in config and CONF_FILE_BROWSER not in config:
        return config
    if CORE.target_framework != "esp-idf":
        raise cv.Invalid("file_api/file_browser require the esp-idf framework")
    if config[CONF_VERSION] != 3:
        raise cv.Invalid("file_api/file_browser require web_server version: 3")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebServer),
            cv.Optional(CONF_PORT, default=80): cv.port,
            cv.Optional(CONF_VERSION, default=2): cv.one_of(1, 2, 3, int=True),
            cv.Optional(CONF_CSS_URL): cv.string,
            cv.Optional(CONF_CSS_INCLUDE): cv.file_,
            cv.Optional(CONF_JS_URL): cv.string,
            cv.Optional(CONF_JS_INCLUDE): cv.file_,
            cv.Optional(CONF_ENABLE_PRIVATE_NETWORK_ACCESS, default=False): cv.boolean,
            cv.Optional(CONF_ALLOWED_ORIGINS): cv.All(
                cv.ensure_list(validate_origin), cv.Length(min=1)
            ),
            cv.Optional(CONF_AUTH): cv.All(
                cv.Schema(
                    {
                        cv.Required(CONF_USERNAME): cv.All(
                            cv.string_strict, cv.Length(min=1)
                        ),
                        cv.Required(CONF_PASSWORD): cv.sensitive(
                            cv.All(cv.string_strict, cv.Length(min=1))
                        ),
                        cv.Optional(CONF_TYPE): cv.one_of(
                            AUTH_TYPE_BASIC, AUTH_TYPE_DIGEST, lower=True
                        ),
                    }
                ),
                validate_auth_type_deprecated,
            ),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.Optional(CONF_INCLUDE_INTERNAL, default=False): cv.boolean,
            cv.Optional(CONF_OTA): cv.boolean,
            cv.Optional(CONF_LOG, default=True): cv.boolean,
            cv.Optional(CONF_LOCAL): cv.boolean,
            cv.Optional(CONF_COMPRESSION, default="gzip"): cv.one_of("gzip", "br"),
            cv.Optional(CONF_SORTING_GROUPS): cv.ensure_list(sorting_group),
            # Optional compile-time opt-in blocks. Presence alone drives AUTO_LOAD() above
            # to pull in storage; absence costs nothing (zero-cost gating via defines).
            # ESP-IDF + web_server version 3 only — enforced in _validate_file_api below.
            # Address-based counterpart to file_api for raw media. Reading is what a
            # browser needs; writing and erasing are opt-in because neither is undoable.
            cv.Optional(CONF_RAW_API): cv.Schema(
                {
                    cv.Optional(CONF_DEVICE_ID): cv.use_id(storage.RawStorage),
                    # One flag: erase is a technical necessity of some media (NOR flash
                    # erases before it writes), not a feature of its own — a device you may
                    # write is a device you may erase.
                    cv.Optional(CONF_ENABLE_WRITE, default=True): cv.boolean,
                }
            ),
            cv.Optional(CONF_FILE_API): cv.Schema(
                {
                    cv.Optional(CONF_STORAGE_ID): cv.use_id(storage.PathStorage),
                    cv.Optional(CONF_MAX_DIR_ENTRIES, default=64): cv.int_range(
                        min=1, max=1024
                    ),
                    # Per-operation access. Each defaults to True (no change for existing
                    # configs); set to False to have the API reject that group with 403 and
                    # the browser hide the corresponding buttons.
                    cv.Optional(CONF_ENABLE_LIST, default=True): cv.boolean,
                    cv.Optional(CONF_ENABLE_READ, default=True): cv.boolean,
                    cv.Optional(CONF_ENABLE_WRITE, default=True): cv.boolean,
                    cv.Optional(CONF_ENABLE_DELETE, default=True): cv.boolean,
                    cv.Optional(CONF_ENABLE_MOUNT, default=True): cv.boolean,
                    cv.Optional(CONF_ENABLE_UNMOUNT, default=True): cv.boolean,
                    cv.Optional(CONF_ENABLE_FORMAT, default=False): cv.boolean,
                }
            ),
            # The browser is a module INSIDE the existing v3 page (a card next to <esp-app>),
            # not a separate page; it implies file_api (defaults applied when absent).
            cv.Optional(CONF_FILE_BROWSER): cv.All(
                lambda value: {} if value is None else value,
                cv.Schema(
                    {
                        # Compile-time choice: the embedded browser JS contains exactly one
                        # action-button variant — flat uppercase v3 text buttons (default)
                        # or icons. Type/medium icons are identification, not actions, and
                        # ship in both variants.
                        cv.Optional(CONF_ACTIONS_AS_ICONS, default=False): cv.boolean,
                        # How often the browser polls /files/changes to auto-refresh
                        # directories that changed behind its back (other clients, plain
                        # API calls). 0s disables the poll.
                        cv.Optional(
                            CONF_CHANGE_POLL_INTERVAL, default="5s"
                        ): cv.positive_time_period_milliseconds,
                        # simple   the browser embedded in this component, ~13 kB
                        # advanced the vendored js-fileexplorer widget with an image
                        #          preview, a plain-textarea editor and a tail -f style
                        #          follower, ~60 kB. Needs psram either way; see
                        #          _final_validate_file_explorer().
                        cv.Optional(CONF_VARIANT, default=BROWSER_SIMPLE): cv.one_of(
                            BROWSER_SIMPLE, BROWSER_ADVANCED, lower=True
                        ),
                        # Where the widget's assets come from before they are copied into
                        # PSRAM. flash compiles them in (~56 kB) and they are there from the
                        # first boot; storage reads them off the medium instead, costs no
                        # flash, and answers 503 until that storage is mounted. The adapter
                        # is compiled in either way. With storage the widget files are not
                        # needed in the repository at all.
                        cv.Optional(CONF_ASSET_SOURCE, default=ASSETS_FLASH): cv.one_of(
                            ASSETS_FLASH, ASSETS_STORAGE, lower=True
                        ),
                        # Directory the widget files are read from with
                        # asset_source: storage. The widget files live on the medium RAW
                        # (no gzip, no .gz suffix) and are served as-is; flash compresses.
                        cv.Optional(
                            CONF_ASSET_PATH, default="/sdcard/file-explorer"
                        ): cv.string_strict,
                        # Which files the advanced widget opens in its text editor and
                        # offers the "monitor" (tail) button for. advanced-only; the
                        # default is applied in _validate_file_browser().
                        cv.Optional(CONF_TEXT_FILE_FORMATS): cv.ensure_list(
                            _text_file_format
                        ),
                    }
                ),
                _validate_file_browser,
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on(
        [
            PLATFORM_ESP32,
            PLATFORM_ESP8266,
            PLATFORM_BK72XX,
            PLATFORM_LN882X,
            PLATFORM_RP2,
            PLATFORM_RTL87XX,
        ]
    ),
    default_url,
    validate_version_deprecated,
    validate_local,
    validate_sorting_groups,
    validate_ota,
    validate_private_network_access,
    _consume_web_server_sockets,
    _validate_file_api,
)


def add_sorting_groups(web_server_var, config):
    for group in config:
        sorting_groups[group[CONF_ID]] = group[CONF_NAME]
        group_sorting_weight = group.get(CONF_SORTING_WEIGHT, 50)
        cg.add(
            web_server_var.add_sorting_group(
                hash(group[CONF_ID]), group[CONF_NAME], group_sorting_weight
            )
        )


async def add_entity_config(entity, config):
    web_server = await cg.get_variable(config[CONF_WEB_SERVER_ID])
    sorting_weight = config.get(CONF_SORTING_WEIGHT, 50)
    sorting_group_hash = hash(config.get(CONF_SORTING_GROUP_ID))

    cg.add_define("USE_WEBSERVER_SORTING")
    cg.add(
        web_server.add_entity_config(
            entity,
            sorting_weight,
            sorting_group_hash,
        )
    )


def build_index_html(config) -> str:
    html = "<!DOCTYPE html><html><head><meta charset=UTF-8><link rel=icon href=data:>"
    css_include = config.get(CONF_CSS_INCLUDE)
    js_include = config.get(CONF_JS_INCLUDE)
    if css_include:
        html += "<link rel=stylesheet href=/0.css>"
    if config[CONF_CSS_URL]:
        html += f'<link rel=stylesheet href="{config[CONF_CSS_URL]}">'
    html += "</head><body>"
    if js_include:
        html += "<script type=module src=/0.js></script>"
    html += "<esp-app></esp-app>"
    if (browser := config.get(CONF_FILE_BROWSER)) is not None:
        if browser.get(CONF_VARIANT) == BROWSER_ADVANCED:
            # Only the scripts go here. The adapter builds the card and mounts it into the v3
            # app's shadow root, so both stylesheets have to be linked from INSIDE that card --
            # document-level <link>s do not cross a shadow boundary. The adapter does that.
            # text_file_formats is handed to the adapter as a global it reads before running.
            html += (
                "<script>window.ESPHFE={textFormats:"
                + json.dumps(browser[CONF_TEXT_FILE_FORMATS])
                + ",changePollMs:"
                + str(int(browser[CONF_CHANGE_POLL_INTERVAL].total_milliseconds))
                + "}</script>"
            )
            html += '<script src="/file-explorer/file-explorer.js"></script>'
            html += '<script src="/file-explorer/adapter.js"></script>'
        else:
            # Injected module renders a "Files" card inside this same page (next to
            # <esp-app>).
            html += "<script src=/file_browser.js></script>"
    if config[CONF_JS_URL]:
        html += f'<script src="{config[CONF_JS_URL]}"></script>'
    html += "</body></html>"
    return html


def add_resource_as_progmem(
    resource_name: str, content: str, compress: bool = True
) -> None:
    """Add a resource to progmem."""
    content_encoded = content.encode("utf-8")
    if compress:
        content_encoded = gzip.compress(content_encoded)
    content_encoded_size = len(content_encoded)
    bytes_as_int = ", ".join(str(x) for x in content_encoded)
    uint8_t = f"constexpr uint8_t ESPHOME_WEBSERVER_{resource_name}[{content_encoded_size}] PROGMEM = {{{bytes_as_int}}}"
    size_t = f"constexpr size_t ESPHOME_WEBSERVER_{resource_name}_SIZE = {content_encoded_size}"
    cg.add_global(cg.RawExpression(uint8_t))
    cg.add_global(cg.RawExpression(size_t))


def add_bytes_as_progmem(resource_name: str, data: bytes) -> None:
    """Like add_resource_as_progmem(), for content that is already bytes.

    The widget's sprite sheet and icon font are binary and must not go through a utf-8 encode,
    and the gzipping is decided per asset rather than for all of them.
    """
    bytes_as_int = ", ".join(str(x) for x in data)
    cg.add_global(
        cg.RawExpression(
            f"constexpr uint8_t ESPHOME_WEBSERVER_{resource_name}[{len(data)}] "
            f"PROGMEM = {{{bytes_as_int}}}"
        )
    )
    cg.add_global(
        cg.RawExpression(
            f"constexpr size_t ESPHOME_WEBSERVER_{resource_name}_SIZE = {len(data)}"
        )
    )


def _asset_symbol(name: str) -> str:
    return "FE_" + name.replace(".", "_").replace("-", "_").upper()


def _flash_row(here: Path, rel: str, url: str, ctype: str, compress: bool) -> str:
    """Compiles one asset in and returns its table row."""
    data = (here / rel).read_bytes()
    if compress:
        data = gzip.compress(data, 9)
    symbol = _asset_symbol(Path(rel).name)
    add_bytes_as_progmem(symbol, data)
    return (
        f'{{"{url}", "{ctype}", ESPHOME_WEBSERVER_{symbol}, '
        f"ESPHOME_WEBSERVER_{symbol}_SIZE, nullptr, "
        f"{'true' if compress else 'false'}, nullptr, 0}}"
    )


async def _add_file_explorer(config, var, api_var) -> None:
    """Emits the advanced browser: an asset table plus the component that serves it.

    Serving is always out of PSRAM. asset_source only decides how the widget's bytes get
    there -- compiled in gzipped and copied at setup(), or read off a PathStorage RAW once
    it mounts. Either way the browser receives the same widget. The adapter is always
    compiled in.
    """
    browser = config[CONF_FILE_BROWSER]
    source = browser[CONF_ASSET_SOURCE]
    base_dir = browser[CONF_ASSET_PATH].rstrip("/")
    here = Path(__file__).parent

    cg.add_define("USE_WEBSERVER_FILE_EXPLORER")

    if source == ASSETS_STORAGE:
        # Reading the assets off the medium goes through the worker's stream API (chunked, no
        # max_blocking_transfer_size ceiling), so the worker has to be compiled in. A path-based
        # driver normally requests it; asking here as well makes it independent of which driver
        # the user happened to configure.
        from esphome.components.storage import request_storage_worker

        request_storage_worker()

    assets_id = ID(f"{var}_file_explorer", is_declaration=True, type=FileExplorerAssets)
    CORE.component_ids.add(str(assets_id))
    assets_var = cg.new_Pvariable(assets_id)
    await cg.register_component(assets_var, {})
    # The assets are served by the file_api handler, not by a handler of their own -- see
    # WebServerFileApi::set_file_explorer(). This is the only wire between the two.
    cg.add(api_var.set_file_explorer(assets_var))

    rows = [_flash_row(here, *entry) for entry in _ADAPTER_ASSETS]

    missing = []
    for rel, url, ctype, compress in _WIDGET_ASSETS:
        name = Path(rel).name
        if source == ASSETS_FLASH:
            if not (here / rel).is_file():
                missing.append(name)
                continue
            rows.append(_flash_row(here, rel, url, ctype, compress))
        else:
            # External storage has room to spare, so the widget files live there RAW -- no
            # gzip and no .gz suffix -- and are served as-is out of PSRAM. Flash still
            # compresses itself in, where the KB matter; storage does not bother. The URL, the
            # on-disk name and the served bytes are all the same plain file.
            disk = f"{base_dir}/{name}"
            rows.append(
                f'{{"{url}", "{ctype}", nullptr, 0, "{disk}", false, nullptr, 0}}'
            )

    if missing:
        raise cv.Invalid(
            f"'{CONF_ASSET_SOURCE}: {ASSETS_FLASH}' compiles the file explorer widget in, but "
            f"{', '.join(missing)} is not in {here / 'file_explorer'}. Either place the "
            f"widget files there, or use '{CONF_ASSET_SOURCE}: {ASSETS_STORAGE}' and put them "
            f"on the medium instead."
        )

    table = ",\n    ".join(rows)
    cg.add_global(
        cg.RawExpression(
            "static esphome::web_server::FileExplorerAssets::Asset "
            f"ESPHOME_WEBSERVER_FE_ASSETS[] = {{\n    {table}\n}}"
        )
    )
    cg.add(
        assets_var.set_assets(
            cg.RawExpression("ESPHOME_WEBSERVER_FE_ASSETS"), len(rows)
        )
    )


@coroutine_with_priority(CoroPriority.WEB)
async def to_code(config):
    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])

    var = cg.new_Pvariable(config[CONF_ID], paren)
    await cg.register_component(var, config)

    # Track controller registration for StaticVector sizing
    CORE.register_controller()

    version = config[CONF_VERSION]

    cg.add(paren.set_port(config[CONF_PORT]))
    cg.add_define("USE_WEBSERVER")
    cg.add_define("USE_WEBSERVER_PORT", config[CONF_PORT])
    cg.add_define("USE_WEBSERVER_VERSION", version)
    if version >= 2:
        # Don't compress the index HTML as the data sizes are almost the same.
        add_resource_as_progmem("INDEX_HTML", build_index_html(config), compress=False)
    else:
        cg.add(var.set_css_url(config[CONF_CSS_URL]))
        cg.add(var.set_js_url(config[CONF_JS_URL]))
    # OTA is now handled by the web_server OTA platform
    # The CONF_OTA option is kept to allow explicitly disabling OTA for web_server
    # IMPORTANT: This ONLY affects the web_server component, NOT captive_portal
    # Captive portal will still be able to perform OTA updates even when this is set
    if config.get(CONF_OTA) is False:
        cg.add_define("USE_WEBSERVER_OTA_DISABLED")
    cg.add(var.set_expose_log(config[CONF_LOG]))
    if config[CONF_LOG]:
        request_log_listener()  # Request a log listener slot for web server log streaming
    if config[CONF_ENABLE_PRIVATE_NETWORK_ACCESS]:
        cg.add_define("USE_WEBSERVER_PRIVATE_NETWORK_ACCESS")
    if (allowed_origins := config.get(CONF_ALLOWED_ORIGINS)) is not None:
        cg.add_define("USE_WEBSERVER_ALLOWED_ORIGINS")
        cg.add(var.set_allowed_origins(allowed_origins))
    if (auth := config.get(CONF_AUTH)) is not None:
        cg.add_define("USE_WEBSERVER_AUTH")
        # The scheme is fixed at build time so the unused Basic/Digest code path is compiled
        # out. Basic is the current default (the absence of this define); an explicit
        # 'type: digest' opts in early. Default changes to digest in 2027.1.0.
        is_digest = auth.get(CONF_TYPE) == AUTH_TYPE_DIGEST
        if is_digest:
            cg.add_define("USE_WEBSERVER_AUTH_DIGEST")
        if is_digest or CORE.is_esp32:
            cg.add(paren.set_auth_username(auth[CONF_USERNAME]))
            cg.add(paren.set_auth_password(auth[CONF_PASSWORD]))
        else:
            # Every non-ESP32 basic auth build takes this path. The ESP8266 and RP2040
            # core base64 encoders wrap output every 72 chars, which breaks
            # ESPAsyncWebServer's basic auth compare for long credentials.
            # Precompute the hash here and let C++ compare the raw header payload.
            basic_hash = base64.b64encode(
                f"{auth[CONF_USERNAME]}:{auth[CONF_PASSWORD]}".encode()
            ).decode()
            cg.add(paren.set_auth_basic_hash(basic_hash))
    if CONF_CSS_INCLUDE in config:
        cg.add_define("USE_WEBSERVER_CSS_INCLUDE")
        path = CORE.relative_config_path(config[CONF_CSS_INCLUDE])
        with path.open(encoding="utf-8") as css_file:
            add_resource_as_progmem("CSS_INCLUDE", css_file.read())
    if CONF_JS_INCLUDE in config:
        cg.add_define("USE_WEBSERVER_JS_INCLUDE")
        path = CORE.relative_config_path(config[CONF_JS_INCLUDE])
        with path.open(encoding="utf-8") as js_file:
            add_resource_as_progmem("JS_INCLUDE", js_file.read())
    cg.add(var.set_include_internal(config[CONF_INCLUDE_INTERNAL]))
    if CONF_LOCAL in config and config[CONF_LOCAL]:
        cg.add_define("USE_WEBSERVER_LOCAL")
    if config[CONF_COMPRESSION] == "gzip":
        cg.add_define("USE_WEBSERVER_GZIP")

    if (sorting_group_config := config.get(CONF_SORTING_GROUPS)) is not None:
        cg.add_define("USE_WEBSERVER_SORTING")
        add_sorting_groups(var, sorting_group_config)

    file_api_config = config.get(CONF_FILE_API)
    file_browser = config.get(CONF_FILE_BROWSER) is not None
    if file_browser and file_api_config is None:
        # file_browser implies file_api with defaults
        file_api_config = FILE_API_DEFAULTS
    if file_api_config is not None:
        cg.add_define("USE_WEBSERVER_FILE_API")
        # The registry-level directory-change feed behind /files/changes: fed by the
        # storage worker and the raw API too, but only worth its RAM when something
        # serves it — and that something is the file_api.
        cg.add_define("USE_STORAGE_CHANGE_FEED")
        # /files/upload uses the same multipart machinery as web_server OTA; without OTA
        # configured nothing else pulls the parser library in. file_api is validated as
        # esp-idf-only, so no framework check is needed here.
        add_idf_component(name="zorxx/multipart-parser", ref="1.0.1")
        # No YAML-visible id — this instance exists purely to back this web_server's
        # file_api sub-block. It is a real Component (setup() registers the handler).
        api_id = ID(f"{var}_file_api", is_declaration=True, type=WebServerFileApi)
        # register_component() consumes the id from CORE.component_ids — manually created
        # IDs (no cv.declare_id) must be added there first (same pattern as the storage
        # worker instantiation).
        CORE.component_ids.add(str(api_id))
        api_var = cg.new_Pvariable(api_id)
        await cg.register_component(api_var, {})
        cg.add(api_var.set_web_server_base(paren))
        cg.add(api_var.set_max_dir_entries(file_api_config[CONF_MAX_DIR_ENTRIES]))
        cg.add(api_var.set_enable_list(file_api_config[CONF_ENABLE_LIST]))
        cg.add(api_var.set_enable_read(file_api_config[CONF_ENABLE_READ]))
        cg.add(api_var.set_enable_write(file_api_config[CONF_ENABLE_WRITE]))
        cg.add(api_var.set_enable_delete(file_api_config[CONF_ENABLE_DELETE]))
        cg.add(api_var.set_enable_mount(file_api_config[CONF_ENABLE_MOUNT]))
        cg.add(api_var.set_enable_unmount(file_api_config[CONF_ENABLE_UNMOUNT]))
        cg.add(api_var.set_enable_format(file_api_config[CONF_ENABLE_FORMAT]))
        if storage_id := file_api_config.get(CONF_STORAGE_ID):
            cg.add(api_var.set_scoped_storage(await cg.get_variable(storage_id)))
        if file_browser and config[CONF_FILE_BROWSER][CONF_VARIANT] == BROWSER_ADVANCED:
            await _add_file_explorer(config, var, api_var)
        elif file_browser:
            cg.add_define("USE_WEBSERVER_FILE_BROWSER")
            # Two prebuilt variants differing only in action-button rendering; the option
            # decides at codegen which one is embedded — the other never reaches the build.
            fb_conf = config[CONF_FILE_BROWSER]
            js_name = (
                "file_browser_icons.js"
                if fb_conf[CONF_ACTIONS_AS_ICONS]
                else "file_browser.js"
            )
            js_path = Path(__file__).parent / js_name
            with js_path.open(encoding="utf-8") as js_file:
                js = js_file.read()
            # Both variants carry the schema default; only a deviation needs the rewrite.
            poll_ms = int(fb_conf[CONF_CHANGE_POLL_INTERVAL].total_milliseconds)
            if poll_ms != 5000:
                js = js.replace(
                    "const CHANGE_POLL_MS = 5000;",
                    f"const CHANGE_POLL_MS = {poll_ms};",
                    1,
                )
            add_resource_as_progmem("FILE_BROWSER_JS", js)

    raw_api_config = config.get(CONF_RAW_API)
    # A configured device node needs an API to talk to, exactly like file_browser implies
    # file_api above. Read-only: showing a node is not consent to write or erase.
    # binary_storage's final validation resolves the node name into its config, so the
    # presence of that key is the reliable signal here — and needs no import of a component
    # that may not even be vendored.
    if (
        raw_api_config is None
        and file_browser
        and any(
            "device_node_name" in device
            for device in CORE.config.get("binary_storage", [])
        )
    ):
        raw_api_config = {CONF_ENABLE_WRITE: True}
    if raw_api_config is not None:
        cg.add_define("USE_WEBSERVER_RAW_API")
        # Same pattern as the file api above: no YAML-visible id, a real Component whose
        # setup() registers the handler.
        raw_id = ID(f"{var}_raw_api", is_declaration=True, type=WebServerRawApi)
        CORE.component_ids.add(str(raw_id))
        raw_var = cg.new_Pvariable(raw_id)
        await cg.register_component(raw_var, {})
        cg.add(raw_var.set_web_server_base(paren))
        cg.add(raw_var.set_enable_write(raw_api_config[CONF_ENABLE_WRITE]))
        if device_id := raw_api_config.get(CONF_DEVICE_ID):
            cg.add(raw_var.set_scoped_device(await cg.get_variable(device_id)))


def FILTER_SOURCE_FILES() -> list[str]:
    """Filter out web_server_v1.cpp when version is not 1."""
    files_to_filter: list[str] = []

    # web_server_v1.cpp is only needed when version is 1
    config = CORE.config.get("web_server", {})
    if config.get(CONF_VERSION, 2) != 1:
        files_to_filter.append("web_server_v1.cpp")

    return files_to_filter
