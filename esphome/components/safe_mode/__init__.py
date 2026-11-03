import functools

from esphome import automation, preferences
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_DISABLED,
    CONF_ID,
    CONF_INCLUDES,
    CONF_NUM_ATTEMPTS,
    CONF_REBOOT_TIMEOUT,
    CONF_STORAGE,
    KEY_PAST_SAFE_MODE,
)
from esphome.core import CORE, KEY_CODEGEN_SPLIT, CoroPriority, coroutine_with_priority
from esphome.cpp_generator import RawExpression
import esphome.final_validate as fv
from esphome.types import ConfigType

from . import safe_set

CODEOWNERS = ["@paulmonigatti", "@jsuanet", "@kbx81"]
DOMAIN = "safe_mode"

CONF_BOOT_IS_GOOD_AFTER = "boot_is_good_after"
CONF_BOOT_IS_GOOD_ON_SHUTDOWN = "boot_is_good_on_shutdown"
CONF_ON_SAFE_MODE = "on_safe_mode"
CONF_KEEP_COMPONENTS = "keep_components"
CONF_KEEP_IDS = "keep_ids"
SAFE_MODE_ACTIVE = "esphome::safe_mode::global_safe_mode_active"

safe_mode_ns = cg.esphome_ns.namespace("safe_mode")
SafeModeComponent = safe_mode_ns.class_("SafeModeComponent", cg.Component)


def _remove_id_if_disabled(value: ConfigType) -> ConfigType:
    value = value.copy()
    if value[CONF_DISABLED]:
        value.pop(CONF_ID)
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SafeModeComponent),
            cv.Optional(
                CONF_BOOT_IS_GOOD_AFTER, default="1min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BOOT_IS_GOOD_ON_SHUTDOWN, default=True): cv.boolean,
            cv.Optional(CONF_DISABLED, default=False): cv.boolean,
            cv.Optional(CONF_NUM_ATTEMPTS, default="10"): cv.positive_not_null_int,
            cv.Optional(
                CONF_REBOOT_TIMEOUT, default="5min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_SAFE_MODE): automation.validate_automation({}),
            cv.Optional(CONF_KEEP_COMPONENTS, default=[]): cv.ensure_list(cv.string_strict),
            cv.Optional(CONF_KEEP_IDS, default=[]): cv.ensure_list(cv.string_strict),
            cv.Optional(CONF_INCLUDES, default=[]): cv.ensure_list(cv.file_),
            **preferences.storage_schema(),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _remove_id_if_disabled,
)


def _split(config: ConfigType) -> bool:
    return not config[CONF_DISABLED] and bool(
        config[CONF_KEEP_COMPONENTS] or config[CONF_KEEP_IDS] or config[CONF_INCLUDES]
    )


def _final_validate(config: ConfigType) -> None:
    if not _split(config):
        return
    try:
        safe = safe_set.compute(
            fv.full_config.get(),
            config[CONF_KEEP_COMPONENTS],
            config[CONF_KEEP_IDS],
            {str(path.resolve()) for path in config[CONF_INCLUDES]},
        )
    except ValueError as err:
        raise cv.Invalid(f"{CONF_KEEP_IDS}: '{err}' is not declared in this configuration") from err
    CORE.data[KEY_CODEGEN_SPLIT] = (
        functools.partial(safe_set.classify, safe),
        (None, f"!{SAFE_MODE_ACTIVE}", SAFE_MODE_ACTIVE),
    )


FINAL_VALIDATE_SCHEMA = _final_validate


automation.register_apply_action(
    "safe_mode.enter",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SafeModeComponent),
        }
    ),
    automation.ApplyCall("restart_in_safe_mode()"),
)


automation.register_apply_action(
    "safe_mode.leave",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SafeModeComponent),
        }
    ),
    automation.ApplyCall("leave_safe_mode()"),
)


automation.register_apply_action(
    "safe_mode.mark_successful",
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SafeModeComponent),
        }
    ),
    automation.ApplyCall("mark_successful()"),
)


_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_SAFE_MODE, "add_on_safe_mode_callback"),
)


@coroutine_with_priority(CoroPriority.APPLICATION)
async def to_code(config: ConfigType) -> None:
    if not config[CONF_DISABLED]:
        var = cg.new_Pvariable(config[CONF_ID])
        await cg.register_component(var, config)

        if config[CONF_BOOT_IS_GOOD_ON_SHUTDOWN]:
            cg.add_define("USE_SAFE_MODE_BOOT_IS_GOOD_ON_SHUTDOWN")

        if on_safe_mode := config.get(CONF_ON_SAFE_MODE):
            cg.add_define("USE_SAFE_MODE_CALLBACK")
            cg.add_define("ESPHOME_SAFE_MODE_CALLBACK_COUNT", len(on_safe_mode))
            await automation.build_callback_automations(
                var, config, _CALLBACK_AUTOMATIONS
            )

        condition = var.should_enter_safe_mode(
            config[CONF_NUM_ATTEMPTS],
            config[CONF_REBOOT_TIMEOUT],
            config[CONF_BOOT_IS_GOOD_AFTER],
            preferences.is_in_flash(config[CONF_STORAGE]),
        )
        if _split(config):
            cg.add_define("USE_SAFE_MODE_SPLIT")
            cg.add(RawExpression(f"{SAFE_MODE_ACTIVE} = {condition}"))
        else:
            cg.add(RawExpression(f"if ({condition}) return"))

    CORE.data[DOMAIN] = {}
    CORE.data[DOMAIN][KEY_PAST_SAFE_MODE] = True
