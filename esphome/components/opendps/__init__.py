"""OpenDPS Power Supply Component for ESPHome"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "switch"]

CONF_UPDATE_INTERVAL = "update_interval"
CONF_OPENDPS_ID = "opendps_id"
CONF_ENABLE = "enable"
CONF_VOLTAGE = "voltage"
CONF_CURRENT = "current"
CONF_FUNCTION = "function"
CONF_KEY = "key"
CONF_VALUE = "value"
CONF_LOCKED = "locked"
CONF_BRIGHTNESS = "brightness"
CONF_FIRMWARE_PATH = "firmware_path"
CONF_DEFAULT_BRIGHTNESS = "default_brightness"
CONF_CALIBRATION_NAME = "calibration_name"
CONF_CALIBRATION_VALUE = "calibration_value"

CONF_ON_CONNECT = "on_connect"

# TCP Bridge for dpsctl.py access
CONF_TCP_BRIDGE = "tcp_bridge"
CONF_TCP_BRIDGE_PORT = "port"

opendps_ns = cg.esphome_ns.namespace("opendps")
OpenDPS = opendps_ns.class_("OpenDPS", cg.Component, uart.UARTDevice)
OpenDPSConnectTrigger = opendps_ns.class_(
    "OpenDPSConnectTrigger", automation.Trigger.template()
)

# Automation actions
EnableOutputAction = opendps_ns.class_("EnableOutputAction", automation.Action)
SetVoltageAction = opendps_ns.class_("SetVoltageAction", automation.Action)
SetCurrentAction = opendps_ns.class_("SetCurrentAction", automation.Action)
SetFunctionAction = opendps_ns.class_("SetFunctionAction", automation.Action)
SetParameterAction = opendps_ns.class_("SetParameterAction", automation.Action)
LockAction = opendps_ns.class_("LockAction", automation.Action)
SetBrightnessAction = opendps_ns.class_("SetBrightnessAction", automation.Action)
PingAction = opendps_ns.class_("PingAction", automation.Action)
RequestVersionAction = opendps_ns.class_("RequestVersionAction", automation.Action)
UpgradeFirmwareAction = opendps_ns.class_("UpgradeFirmwareAction", automation.Action)
RequestCalibrationReportAction = opendps_ns.class_(
    "RequestCalibrationReportAction", automation.Action
)
SetCalibrationAction = opendps_ns.class_("SetCalibrationAction", automation.Action)
ClearCalibrationAction = opendps_ns.class_("ClearCalibrationAction", automation.Action)

# Calibration Assistant actions
StartCalibrationAssistantAction = opendps_ns.class_(
    "StartCalibrationAssistantAction", automation.Action
)
CalibrationAssistantStepAction = opendps_ns.class_(
    "CalibrationAssistantStepAction", automation.Action
)
CancelCalibrationAssistantAction = opendps_ns.class_(
    "CancelCalibrationAssistantAction", automation.Action
)

# Calibration assistant parameters struct
CalibrationAssistantParams = opendps_ns.struct("CalibrationAssistantParams")

# Config keys for calibration assistant
CONF_VIN_LOW_MV = "vin_low_mv"
CONF_VIN_HIGH_MV = "vin_high_mv"
CONF_VOUT_LOW_MV = "vout_low_mv"
CONF_VOUT_HIGH_MV = "vout_high_mv"
CONF_LOAD_RESISTANCE = "load_resistance"
CONF_LOAD_MAX_WATTAGE = "load_max_wattage"
CONF_MAX_DPS_CURRENT = "max_dps_current"
CONF_MEASURED_VALUE = "measured_value"

TCP_BRIDGE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TCP_BRIDGE_PORT, default=5005): cv.port,
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OpenDPS),
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="1s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_DEFAULT_BRIGHTNESS, default=50): cv.int_range(
                min=0, max=100
            ),
            cv.Optional(CONF_ON_CONNECT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        OpenDPSConnectTrigger
                    ),
                }
            ),
            # TCP bridge for dpsctl.py access - allows external tools to communicate
            # directly with OpenDPS via TCP->UART bridge (default port 5005)
            cv.Optional(CONF_TCP_BRIDGE): TCP_BRIDGE_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_default_brightness(config[CONF_DEFAULT_BRIGHTNESS]))

    # TCP bridge configuration for dpsctl.py access
    if CONF_TCP_BRIDGE in config:
        tcp_bridge_config = config[CONF_TCP_BRIDGE]
        cg.add(var.set_tcp_bridge_enabled(True))
        cg.add(var.set_tcp_bridge_port(tcp_bridge_config[CONF_TCP_BRIDGE_PORT]))

    for conf in config.get(CONF_ON_CONNECT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


# Automation Actions
OPENDPS_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(OpenDPS)})


@automation.register_action(
    "opendps.enable_output",
    EnableOutputAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_ENABLE): cv.templatable(cv.boolean)}
    ),
)
async def opendps_enable_output_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_ENABLE], args, bool)
    cg.add(var.set_enable(template_))
    return var


@automation.register_action(
    "opendps.set_voltage",
    SetVoltageAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_VOLTAGE): cv.templatable(cv.float_)}
    ),
)
async def opendps_set_voltage_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_VOLTAGE], args, float)
    cg.add(var.set_voltage(template_))
    return var


@automation.register_action(
    "opendps.set_current",
    SetCurrentAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_CURRENT): cv.templatable(cv.float_)}
    ),
)
async def opendps_set_current_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_CURRENT], args, float)
    cg.add(var.set_current(template_))
    return var


@automation.register_action(
    "opendps.set_function",
    SetFunctionAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_FUNCTION): cv.templatable(cv.string)}
    ),
)
async def opendps_set_function_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_FUNCTION], args, cg.std_string)
    cg.add(var.set_function(template_))
    return var


@automation.register_action(
    "opendps.set_parameter",
    SetParameterAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Required(CONF_KEY): cv.templatable(cv.string),
            cv.Required(CONF_VALUE): cv.templatable(cv.string),
        }
    ),
)
async def opendps_set_parameter_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_key = await cg.templatable(config[CONF_KEY], args, cg.std_string)
    template_value = await cg.templatable(config[CONF_VALUE], args, cg.std_string)
    cg.add(var.set_key(template_key))
    cg.add(var.set_value(template_value))
    return var


@automation.register_action(
    "opendps.lock",
    LockAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_LOCKED): cv.templatable(cv.boolean)}
    ),
)
async def opendps_lock_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_LOCKED], args, bool)
    cg.add(var.set_locked(template_))
    return var


@automation.register_action(
    "opendps.set_brightness",
    SetBrightnessAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_BRIGHTNESS): cv.templatable(cv.uint8_t)}
    ),
)
async def opendps_set_brightness_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_BRIGHTNESS], args, cg.uint8)
    cg.add(var.set_brightness(template_))
    return var


@automation.register_action("opendps.ping", PingAction, OPENDPS_ACTION_SCHEMA)
async def opendps_ping_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.request_version", RequestVersionAction, OPENDPS_ACTION_SCHEMA
)
async def opendps_request_version_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.upgrade_firmware",
    UpgradeFirmwareAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_FIRMWARE_PATH): cv.templatable(cv.string)}
    ),
)
async def opendps_upgrade_firmware_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_FIRMWARE_PATH], args, cg.std_string)
    cg.add(var.set_firmware_path(template_))
    return var


# Calibration Actions
@automation.register_action(
    "opendps.request_calibration_report",
    RequestCalibrationReportAction,
    OPENDPS_ACTION_SCHEMA,
)
async def opendps_request_calibration_report_to_code(
    config, action_id, template_arg, args
):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.set_calibration",
    SetCalibrationAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Required(CONF_CALIBRATION_NAME): cv.templatable(cv.string),
            cv.Required(CONF_CALIBRATION_VALUE): cv.templatable(cv.float_),
        }
    ),
)
async def opendps_set_calibration_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_name = await cg.templatable(
        config[CONF_CALIBRATION_NAME], args, cg.std_string
    )
    template_value = await cg.templatable(config[CONF_CALIBRATION_VALUE], args, float)
    cg.add(var.set_name(template_name))
    cg.add(var.set_value(template_value))
    return var


@automation.register_action(
    "opendps.clear_calibration", ClearCalibrationAction, OPENDPS_ACTION_SCHEMA
)
async def opendps_clear_calibration_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


# Calibration Assistant Actions
@automation.register_action(
    "opendps.start_calibration_assistant",
    StartCalibrationAssistantAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Required(CONF_VIN_LOW_MV): cv.templatable(cv.float_),
            cv.Required(CONF_VIN_HIGH_MV): cv.templatable(cv.float_),
            cv.Optional(CONF_VOUT_LOW_MV, default=0): cv.templatable(cv.float_),
            cv.Optional(CONF_VOUT_HIGH_MV, default=0): cv.templatable(cv.float_),
            cv.Optional(CONF_LOAD_RESISTANCE, default=0): cv.templatable(cv.float_),
            cv.Optional(CONF_LOAD_MAX_WATTAGE, default=0): cv.templatable(cv.float_),
            cv.Optional(CONF_MAX_DPS_CURRENT, default=5.0): cv.templatable(cv.float_),
        }
    ),
)
async def opendps_start_calibration_assistant_to_code(
    config, action_id, template_arg, args
):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    vin_low = await cg.templatable(config[CONF_VIN_LOW_MV], args, float)
    vin_high = await cg.templatable(config[CONF_VIN_HIGH_MV], args, float)
    vout_low = await cg.templatable(config[CONF_VOUT_LOW_MV], args, float)
    vout_high = await cg.templatable(config[CONF_VOUT_HIGH_MV], args, float)
    load_r = await cg.templatable(config[CONF_LOAD_RESISTANCE], args, float)
    load_w = await cg.templatable(config[CONF_LOAD_MAX_WATTAGE], args, float)
    max_i = await cg.templatable(config[CONF_MAX_DPS_CURRENT], args, float)
    cg.add(var.set_vin_low_mv(vin_low))
    cg.add(var.set_vin_high_mv(vin_high))
    cg.add(var.set_vout_low_mv(vout_low))
    cg.add(var.set_vout_high_mv(vout_high))
    cg.add(var.set_load_resistance(load_r))
    cg.add(var.set_load_max_wattage(load_w))
    cg.add(var.set_max_dps_current(max_i))
    return var


@automation.register_action(
    "opendps.calibration_assistant_step",
    CalibrationAssistantStepAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_MEASURED_VALUE, default=0): cv.templatable(cv.float_),
        }
    ),
)
async def opendps_calibration_assistant_step_to_code(
    config, action_id, template_arg, args
):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    measured = await cg.templatable(config[CONF_MEASURED_VALUE], args, float)
    cg.add(var.set_measured_value(measured))
    return var


@automation.register_action(
    "opendps.cancel_calibration_assistant",
    CancelCalibrationAssistantAction,
    OPENDPS_ACTION_SCHEMA,
)
async def opendps_cancel_calibration_assistant_to_code(
    config, action_id, template_arg, args
):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
