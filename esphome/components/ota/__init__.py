import logging
from typing import Any

from esphome import automation
import esphome.codegen as cg
from esphome.components.const import CONF_ON_STATE_CHANGE
from esphome.config_helpers import (
    filter_source_files_from_defines,
    filter_source_files_from_platform,
)
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import (
    CONF_ADVANCED,
    CONF_API,
    CONF_ESPHOME,
    CONF_FRAMEWORK,
    CONF_ID,
    CONF_INCLUDES,
    CONF_ON_ERROR,
    CONF_OTA,
    CONF_PLATFORM,
    CONF_RAW_DATA_ID,
    CONF_TRIGGER_ID,
    PLATFORM_ESP32,
    PlatformFramework,
)
from esphome.core import CORE, ID, coroutine_with_priority
from esphome.coroutine import CoroPriority
from esphome.cpp_generator import MockObj, MockObjClass
from esphome.helpers import fnv1_hash
from esphome.types import ConfigType

from .ota_system import (
    CONF_KEEP_COMPONENTS,
    CONF_KEEP_IDS,
    CONF_OTA_SYSTEM,
    CONF_SYSTEM_BUILD,
)
from .ota_system import write as write_ota_system

_LOGGER = logging.getLogger(__name__)

OTA_STATE_LISTENER_KEY = "ota_state_listener"

CODEOWNERS = ["@esphome/core"]
DOMAIN = "ota"


def AUTO_LOAD() -> list[str]:
    components = ["safe_mode"]
    if not CORE.using_zephyr:
        components.extend(["md5"])
    if CORE.is_esp32:
        components.extend(["watchdog"])
    return components


IS_PLATFORM_COMPONENT = True

CONF_ON_ABORT = "on_abort"
CONF_ON_BEGIN = "on_begin"
CONF_ON_END = "on_end"
CONF_ON_PROGRESS = "on_progress"


ota_ns = cg.esphome_ns.namespace("ota")
OTAComponent = ota_ns.class_("OTAComponent", cg.Component)
OTAState = ota_ns.enum("OTAState")
OTAAbortTrigger = ota_ns.class_("OTAAbortTrigger", automation.Trigger.template())
OTAEndTrigger = ota_ns.class_("OTAEndTrigger", automation.Trigger.template())
OTAErrorTrigger = ota_ns.class_("OTAErrorTrigger", automation.Trigger.template())
OTAProgressTrigger = ota_ns.class_("OTAProgressTrigger", automation.Trigger.template())
OTAStartTrigger = ota_ns.class_("OTAStartTrigger", automation.Trigger.template())
OTAStateChangeTrigger = ota_ns.class_(
    "OTAStateChangeTrigger", automation.Trigger.template()
)
RebootToSystemAction = ota_ns.class_("RebootToSystemAction", automation.Action)
RebootToAppAction = ota_ns.class_("RebootToAppAction", automation.Action)
RebootToSystemService = ota_ns.class_("RebootToSystemService")
SystemRunningService = ota_ns.class_("SystemRunningService")
RebootToAppService = ota_ns.class_("RebootToAppService")
OTA_SYSTEM_IMAGE_MARKER = "ota-system"
OtaSystemElfAction = ota_ns.class_("OtaSystemElfAction")
OTA_SYSTEM_ELF_ACTION = "ota_system_elf"
MainYamlRecord = ota_ns.struct("MainYamlRecord")
MAIN_YAML_RECORD = "ota_main_yaml_record"
MAIN_YAML_RECORD_SECTION = ".rodata_custom_desc"
STORE_YAML = "store_yaml"



def _ota_final_validate(config: ConfigType) -> None:
    if len(config) < 1:
        raise cv.Invalid(
            f"At least one platform must be specified for '{CONF_OTA}'; add '{CONF_PLATFORM}: {CONF_ESPHOME}' for original OTA functionality"
        )
    if sum(1 for platform in config if CONF_OTA_SYSTEM in platform) > 1:
        raise cv.Invalid(f"'{CONF_OTA_SYSTEM}' may be set on one '{CONF_OTA}' platform only")
    for platform in config:
        if CONF_OTA_SYSTEM in platform:
            system = platform[CONF_OTA_SYSTEM]
            write_ota_system(
                system[CONF_KEEP_COMPONENTS],
                system[CONF_KEEP_IDS],
                system[CONF_INCLUDES],
                fv.full_config.get(),
            )


FINAL_VALIDATE_SCHEMA = _ota_final_validate

OTA_SYSTEM_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_KEEP_COMPONENTS, default=[]): cv.ensure_list(cv.string_strict),
            cv.Optional(CONF_KEEP_IDS, default=[]): cv.ensure_list(cv.string_strict),
            cv.Optional(CONF_INCLUDES, default=[]): cv.ensure_list(cv.file_),
        }
    ),
    cv.only_on_esp32,
)

BASE_OTA_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_OTA_SYSTEM): OTA_SYSTEM_SCHEMA,
        cv.Optional(CONF_SYSTEM_BUILD, default=False): cv.boolean,
        cv.Optional(CONF_ON_STATE_CHANGE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OTAStateChangeTrigger),
            }
        ),
        cv.Optional(CONF_ON_ABORT): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OTAAbortTrigger),
            }
        ),
        cv.Optional(CONF_ON_BEGIN): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OTAStartTrigger),
            }
        ),
        cv.Optional(CONF_ON_END): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OTAEndTrigger),
            }
        ),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OTAErrorTrigger),
            }
        ),
        cv.Optional(CONF_ON_PROGRESS): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OTAProgressTrigger),
            }
        ),
    }
)


@coroutine_with_priority(CoroPriority.OTA_UPDATES)
async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_OTA")
    CORE.add_job(final_step)

    if CORE.is_rp2 and CORE.using_arduino:
        cg.add_library("Updater", None)


async def ota_to_code(var: MockObj, config: ConfigType) -> None:
    await cg.past_safe_mode()
    use_state_callback = False
    for conf in config.get(CONF_ON_STATE_CHANGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(OTAState, "state")], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_ABORT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_BEGIN, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_PROGRESS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(float, "x")], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_END, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        use_state_callback = True
    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint8, "x")], conf)
        use_state_callback = True
    if use_state_callback:
        request_ota_state_listeners()


def request_ota_state_listeners() -> None:
    """Request that OTA state listeners be compiled in.

    Components that need to be notified about OTA state changes (start, progress,
    complete, error) should call this function during their code generation.
    This enables the add_state_listener() API on OTAComponent.
    """
    CORE.data[OTA_STATE_LISTENER_KEY] = True


@coroutine_with_priority(CoroPriority.FINAL)
async def final_step() -> None:
    """Final code generation step to configure optional OTA features."""
    if CORE.data.get(OTA_STATE_LISTENER_KEY, False):
        cg.add_define("USE_OTA_STATE_LISTENER")
    for platform in CORE.config.get(CONF_OTA, []):
        if CONF_OTA_SYSTEM in platform:
            cg.add_define("USE_OTA_REBOOT_TO_SYSTEM")
            await _register_api_service("ota_reboot_to_system_service", RebootToSystemService)
            if STORE_YAML in CORE.config:
                await _embed_main_yaml_record()
        if platform.get(CONF_SYSTEM_BUILD):
            cg.add_define("USE_OTA_SYSTEM_BUILD")
            cg.add_define("USE_OTA_REBOOT_TO_APP")
            await _register_api_service("ota_system_running_service", SystemRunningService)
            await _register_api_service("ota_reboot_to_app_service", RebootToAppService)
            _mark_system_image()
            if STORE_YAML in CORE.config:
                await _map_main_yaml()
        if (CONF_OTA_SYSTEM in platform or platform.get(CONF_SYSTEM_BUILD)) and CONF_API in CORE.config:
            cg.add_define("USE_OTA_SYSTEM_ELF_ACTION")
            cg.add_define("USE_API_USER_DEFINED_ACTION_RESPONSES")
            cg.add_define("USE_API_USER_DEFINED_ACTION_RESPONSES_JSON")
            await _register_api_service(
                "ota_system_elf_action", OtaSystemElfAction, fnv1_hash(OTA_SYSTEM_ELF_ACTION)
            )


async def _embed_main_yaml_record() -> None:
    raw = await cg.get_variable(CORE.config[STORE_YAML][CONF_RAW_DATA_ID])
    cg.add(
        cg.RawStatement(
            f"static const {MainYamlRecord} {MAIN_YAML_RECORD} "
            f'__attribute__((section("{MAIN_YAML_RECORD_SECTION}"), used)) = '
            f"{{{ota_ns.MAIN_YAML_RECORD_MAGIC}, {raw}, sizeof({raw})}};"
        )
    )
    cg.add(ota_ns.keep_main_yaml_record(cg.RawExpression(f"&{MAIN_YAML_RECORD}")))


async def _map_main_yaml() -> None:
    cg.add_define("USE_OTA_MAIN_YAML")
    cg.add_define("USE_STORE_YAML_EXPORT")
    cg.add(ota_ns.map_main_yaml())


def _mark_system_image() -> None:
    from esphome.components.const import CONF_ENABLE_OTA_DOWNGRADE_PROTECTION
    from esphome.components.esp32 import add_idf_sdkconfig_option

    advanced = CORE.config.get(PLATFORM_ESP32, {}).get(CONF_FRAMEWORK, {}).get(CONF_ADVANCED, {})
    if advanced.get(CONF_ENABLE_OTA_DOWNGRADE_PROTECTION):
        _LOGGER.warning(
            "OTA system: downgrade protection keeps the project version in the image, so the OTA "
            "system cannot mark its image and cannot update itself over OTA"
        )
        return
    cg.add_define("USE_OTA_SYSTEM_SELF_UPDATE")
    add_idf_sdkconfig_option("CONFIG_APP_PROJECT_VER_FROM_CONFIG", True)
    add_idf_sdkconfig_option("CONFIG_APP_PROJECT_VER", OTA_SYSTEM_IMAGE_MARKER)


async def _register_api_service(name: str, service_class: MockObjClass, *args: Any) -> None:
    if CONF_API not in CORE.config:
        return
    cg.add_define("USE_API_USER_DEFINED_ACTIONS")
    cg.add_define("USE_API_CUSTOM_SERVICES")
    api = await cg.get_variable(CORE.config[CONF_API][CONF_ID])
    service = cg.new_Pvariable(ID(name, True, service_class), *args)
    cg.add(api.register_user_service(service))


@automation.register_action(
    "ota.reboot_to_system",
    RebootToSystemAction,
    cv.All(cv.Schema({}), cv.only_on_esp32),
    synchronous=True,
)
async def reboot_to_system_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: list,
) -> MockObj:
    cg.add_define("USE_OTA_REBOOT_TO_SYSTEM")
    return cg.new_Pvariable(action_id, template_arg)


@automation.register_action(
    "ota.reboot_to_app",
    RebootToAppAction,
    cv.All(cv.Schema({}), cv.only_on_esp32),
    synchronous=True,
)
async def reboot_to_app_to_code(
    config: ConfigType,
    action_id: ID,
    template_arg: cg.TemplateArguments,
    args: list,
) -> MockObj:
    cg.add_define("USE_OTA_REBOOT_TO_APP")
    return cg.new_Pvariable(action_id, template_arg)


_filter_backend_source_files = filter_source_files_from_platform(
    {
        "ota_backend_esp_idf.cpp": {
            PlatformFramework.ESP32_ARDUINO,
            PlatformFramework.ESP32_IDF,
        },
        "ota_backend_esp8266.cpp": {PlatformFramework.ESP8266_ARDUINO},
        "ota_backend_arduino_rp2.cpp": {PlatformFramework.RP2_ARDUINO},
        "ota_backend_arduino_libretiny.cpp": {
            PlatformFramework.BK72XX_ARDUINO,
            PlatformFramework.RTL87XX_ARDUINO,
            PlatformFramework.LN882X_ARDUINO,
        },
        "ota_backend_host.cpp": {PlatformFramework.HOST_NATIVE},
    }
)


# USE_OTA_SIGNED_VERIFICATION_MULTI_KEY is set only on ESP32/IDF;
# USE_OTA_PARTITIONS is set by the esphome OTA platform when
# allow_partition_access is enabled.
_filter_define_source_files = filter_source_files_from_defines(
    {
        "ota_signature_esp_idf.cpp": "USE_OTA_SIGNED_VERIFICATION_MULTI_KEY",
        "ota_bootloader_esp_idf.cpp": "USE_OTA_PARTITIONS",
        "ota_partitions_esp_idf.cpp": "USE_OTA_PARTITIONS",
        "ota_system_esp_idf.cpp": ("USE_OTA_REBOOT_TO_SYSTEM", "USE_OTA_REBOOT_TO_APP"),
        "ota_system_update_esp_idf.cpp": "USE_OTA_SYSTEM_SELF_UPDATE",
        "main_yaml_esp_idf.cpp": "USE_OTA_MAIN_YAML",
    }
)


def FILTER_SOURCE_FILES() -> list[str]:
    return _filter_backend_source_files() + _filter_define_source_files()
