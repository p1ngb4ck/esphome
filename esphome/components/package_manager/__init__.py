import base64
import binascii
import logging
from pathlib import Path
import re

from esphome import automation
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component
from esphome.components.storage import request_storage_worker
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import (
    CONF_API,
    CONF_FILE,
    CONF_ID,
    CONF_MQTT,
    CONF_NAME,
    CONF_PACKAGES,
    CONF_SOURCE,
    CONF_TARGET,
    CONF_URL,
    CONF_VERSION,
)
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority

from . import build

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]

LIB_BUILDER_NAME = "package_manager_lib_builder"
LIB_BUILDER_PATH = Path(__file__).parent / LIB_BUILDER_NAME


def AUTO_LOAD(config):
    loads = ["sha256"]
    if not config or config.get(CONF_API, True) or config.get(CONF_MQTT, True):
        loads += ["json"]
    if config and config.get(CONF_PACKAGES):
        loads += build.entity_domains()
    if config and config.get(CONF_HTTP_REQUEST_ID):
        loads += ["noise"]
    return loads


package_manager_ns = cg.esphome_ns.namespace("package_manager")
PackageManager = package_manager_ns.class_("PackageManager", cg.Component)
SettingsTarget = package_manager_ns.enum("SettingsTarget", is_class=True)

LoadAction = package_manager_ns.class_("LoadAction", automation.Action, cg.Parented.template(PackageManager))
InstallAction = package_manager_ns.class_("InstallAction", automation.Action, cg.Parented.template(PackageManager))
EnableAction = package_manager_ns.class_("EnableAction", automation.Action, cg.Parented.template(PackageManager))
RecoveryAction = package_manager_ns.class_("RecoveryAction", automation.Action, cg.Parented.template(PackageManager))
RemoveAction = package_manager_ns.class_("RemoveAction", automation.Action, cg.Parented.template(PackageManager))

http_request_ns = cg.esphome_ns.namespace("http_request")
HttpRequestComponent = http_request_ns.class_("HttpRequestComponent", cg.Component)

CONF_ABI = "abi"
CONF_DIRECTORY = "directory"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_MAX_PACKAGES = "max_packages"
CONF_PUBLIC_KEY = "public_key"
CONF_REPOSITORIES = "repositories"
CONF_SETTINGS = "settings"
REPO_MAX = 8

SETTINGS_TARGETS = {
    "preferences": SettingsTarget.PREFERENCES,
    "storage": SettingsTarget.STORAGE,
}


def _directory(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/") or len(value) > 63:
        raise cv.Invalid("directory must be a full path without a trailing '/', at most 63 characters")
    return value


PACKAGE_NAME = cv.All(cv.string_strict, cv.Length(min=1, max=23))
_BUILD_NAME = re.compile(r"^[a-z][a-z0-9_]{0,22}$")


def _build_name(value):
    value = cv.string_strict(value)
    if not _BUILD_NAME.match(value):
        raise cv.Invalid("a package name starts with a-z and has only a-z, 0-9 and _ (at most 23 characters)")
    return value


def _package_entry(value):
    if isinstance(value, dict):
        return cv.Schema(
            {
                cv.Required(CONF_FILE): cv.file_,
                cv.Optional(build.CONF_DEPENDS, default=[]): cv.ensure_list(_build_name),
            }
        )(value)
    return {CONF_FILE: cv.file_(value), build.CONF_DEPENDS: []}


def _validate_packages(config):
    for name, entry in config[CONF_PACKAGES].items():
        for dep in entry[build.CONF_DEPENDS]:
            if dep not in config[CONF_PACKAGES] or dep == name:
                raise cv.Invalid(
                    f"'{name}' depends on '{dep}', which is not another package",
                    path=[CONF_PACKAGES, name, build.CONF_DEPENDS],
                )
    return config


def _repo_url(value):
    value = cv.url(value).rstrip("/")
    if not value.startswith(("http://", "https://")) or len(value) > 127:
        raise cv.Invalid("a repository URL starts with http:// or https://, at most 127 characters")
    return value


def _public_key(value):
    value = cv.string_strict(value).strip()
    line = value.splitlines()[-1].strip() if value else ""
    try:
        raw = base64.b64decode(line, validate=True)
    except binascii.Error as err:
        raise cv.Invalid("public_key must be a usign / signify Ed25519 public key") from err
    if len(raw) != 42 or raw[:2] != b"Ed":
        raise cv.Invalid("public_key must be a usign / signify Ed25519 public key")
    return raw[2:].hex()


REPOSITORY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=23)),
        cv.Required(CONF_URL): _repo_url,
        cv.Optional(CONF_PUBLIC_KEY): _public_key,
    }
)


def _validate_repositories(config):
    repos = config.get(CONF_REPOSITORIES, [])
    if repos and CONF_HTTP_REQUEST_ID not in config:
        raise cv.Invalid("repositories need http_request_id", path=[CONF_REPOSITORIES])
    names = [repo[CONF_NAME] for repo in repos]
    for index, name in enumerate(names):
        if name in names[:index]:
            raise cv.Invalid(f"repository '{name}' is listed twice", path=[CONF_REPOSITORIES, index])
    return config

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PackageManager),
            cv.Optional(CONF_DIRECTORY): _directory,
            cv.Optional(CONF_MAX_PACKAGES, default=8): cv.int_range(min=1, max=15),
            cv.Optional(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
            cv.Optional(CONF_PACKAGES, default={}): cv.Schema({_build_name: _package_entry}),
            cv.Optional(CONF_REPOSITORIES): cv.All(cv.ensure_list(REPOSITORY_SCHEMA), cv.Length(max=REPO_MAX)),
            cv.Optional(CONF_API): cv.boolean,
            cv.Optional(CONF_MQTT): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_packages,
    _validate_repositories,
    cv.only_with_framework("esp-idf"),
)


def _api_enabled(config):
    return config.get(CONF_API, CONF_API in CORE.config)


def _mqtt_enabled(config):
    return config.get(CONF_MQTT, CONF_MQTT in CORE.config)


def _final_validate(config):
    if config.get(CONF_API) and CONF_API not in fv.full_config.get():
        raise cv.Invalid("api: true needs the api component", path=[CONF_API])
    if config.get(CONF_MQTT) and CONF_MQTT not in fv.full_config.get():
        raise cv.Invalid("mqtt: true needs the mqtt component", path=[CONF_MQTT])
    if config[CONF_PACKAGES] and not build.package_pass():
        documents = [Path(entry[CONF_FILE]).resolve() for entry in config[CONF_PACKAGES].values()]
        fv.full_config.get().add_validation_step(build.removal_step(documents))
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_PACKAGE_MANAGER")
    if CONF_DIRECTORY in config:
        cg.add(var.set_directory(config[CONF_DIRECTORY]))
    cg.add(var.set_max_packages(config[CONF_MAX_PACKAGES]))
    if CONF_HTTP_REQUEST_ID in config:
        cg.add_define("USE_PACKAGE_MANAGER_HTTP")
        cg.add(var.set_http_request_parent(await cg.get_variable(config[CONF_HTTP_REQUEST_ID])))
        for repo in config.get(CONF_REPOSITORIES, []):
            cg.add(var.add_repository(repo[CONF_NAME], repo[CONF_URL], repo.get(CONF_PUBLIC_KEY, "")))
    request_storage_worker()
    add_idf_component(name=LIB_BUILDER_NAME, path=str(LIB_BUILDER_PATH.resolve()))
    if _api_enabled(config) and not build.package_pass():
        CORE.add_job(_add_api_actions, var)
    if _mqtt_enabled(config) and not build.package_pass():
        cg.add_define("USE_PACKAGE_MANAGER_MQTT")
    CORE.add_job(_finish, config)


@coroutine_with_priority(CoroPriority.FINAL)
async def _add_api_actions(var):
    cg.add_define("USE_PACKAGE_MANAGER_API")
    cg.add_define("USE_API_USER_DEFINED_ACTIONS")
    cg.add_define("USE_API_CUSTOM_SERVICES")
    cg.add_define("USE_API_USER_DEFINED_ACTION_RESPONSES")
    cg.add_define("USE_API_USER_DEFINED_ACTION_RESPONSES_JSON")
    await cg.get_variable(CORE.config[CONF_API][CONF_ID])
    cg.add(var.register_api_actions())


@coroutine_with_priority(CoroPriority.FINAL + 0.5)
async def _finish(config):
    manifest = Path(CORE.relative_build_path(build.GLUE_DIR, "manifest.json"))
    if config[CONF_PACKAGES]:
        for item in build.build_packages(config[CONF_PACKAGES]):
            _LOGGER.info("package_manager: package '%s' %s built", item["package"], item["version"])
        return
    if manifest.is_file():
        manifest.unlink()
    from . import exports

    exports.emit(exports.core_ids())


PM_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(PackageManager)})
PM_NAME_SCHEMA = PM_ACTION_SCHEMA.extend({cv.Required(CONF_NAME): cv.templatable(PACKAGE_NAME)})


@automation.register_action("package_manager.load", LoadAction, PM_ACTION_SCHEMA, synchronous=True)
async def load_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "package_manager.install",
    InstallAction,
    PM_NAME_SCHEMA.extend(
        {
            cv.Required(CONF_SOURCE): cv.templatable(cv.string_strict),
            cv.Required(CONF_VERSION): cv.templatable(cv.All(cv.string_strict, cv.Length(max=15))),
            cv.Optional(CONF_ABI): cv.templatable(cv.int_range(min=1, max=0xFFFFFFFF)),
            cv.Optional(CONF_TARGET): cv.templatable(_directory),
            cv.Optional(CONF_SETTINGS, default="preferences"): cv.enum(SETTINGS_TARGETS, lower=True),
        }
    ),
    synchronous=True,
)
async def install_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_source(await cg.templatable(config[CONF_SOURCE], args, cg.std_string)))
    cg.add(var.set_name(await cg.templatable(config[CONF_NAME], args, cg.std_string)))
    cg.add(var.set_version(await cg.templatable(config[CONF_VERSION], args, cg.std_string)))
    if CONF_ABI in config:
        cg.add(var.set_abi(await cg.templatable(config[CONF_ABI], args, cg.uint32)))
    if CONF_TARGET in config:
        cg.add(var.set_target(await cg.templatable(config[CONF_TARGET], args, cg.std_string)))
    cg.add(var.set_settings(config[CONF_SETTINGS]))
    return var


async def _enable_action(config, action_id, template_arg, args, enabled):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_name(await cg.templatable(config[CONF_NAME], args, cg.std_string)))
    cg.add(var.set_enabled(enabled))
    return var


@automation.register_action("package_manager.enable", EnableAction, PM_NAME_SCHEMA, synchronous=True)
async def enable_to_code(config, action_id, template_arg, args):
    return await _enable_action(config, action_id, template_arg, args, True)


@automation.register_action("package_manager.disable", EnableAction, PM_NAME_SCHEMA, synchronous=True)
async def disable_to_code(config, action_id, template_arg, args):
    return await _enable_action(config, action_id, template_arg, args, False)


async def _recovery_action(config, action_id, template_arg, args, recovery):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_name(await cg.templatable(config[CONF_NAME], args, cg.std_string)))
    cg.add(var.set_recovery(recovery))
    return var


@automation.register_action("package_manager.recovery_enable", RecoveryAction, PM_NAME_SCHEMA, synchronous=True)
async def recovery_enable_to_code(config, action_id, template_arg, args):
    return await _recovery_action(config, action_id, template_arg, args, True)


@automation.register_action("package_manager.recovery_disable", RecoveryAction, PM_NAME_SCHEMA, synchronous=True)
async def recovery_disable_to_code(config, action_id, template_arg, args):
    return await _recovery_action(config, action_id, template_arg, args, False)


@automation.register_action("package_manager.remove", RemoveAction, PM_NAME_SCHEMA, synchronous=True)
async def remove_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_name(await cg.templatable(config[CONF_NAME], args, cg.std_string)))
    return var
