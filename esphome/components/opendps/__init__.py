"""OpenDPS Power Supply Component for ESPHome"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import time as time_component, uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_BAUD_RATE,
    CONF_BRIGHTNESS,
    CONF_BUFFER_SIZE,
    CONF_CURRENT,
    CONF_FORMAT,
    CONF_ID,
    CONF_KEY,
    CONF_ON_CONNECT,
    CONF_PATH,
    CONF_TIME_ID,
    CONF_TRIGGER_ID,
    CONF_UPDATE_INTERVAL,
    CONF_VALUE,
    CONF_VOLTAGE,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor", "switch"]

# Datalogger configuration keys
CONF_DATALOGGER = "datalogger"
CONF_STORAGE_PATH = "storage_path"
CONF_FILENAME_FORMAT = "filename_format"
CONF_FILENAME_ID = "filename_id"
CONF_COLUMNS = "columns"
CONF_FLUSH_INTERVAL = "flush_interval"
CONF_FILENAME = "filename"

DATALOG_FORMAT_CSV = "csv"
DATALOG_FORMAT_BINARY = "binary"

CONF_OPENDPS_ID = "opendps_id"
CONF_ENABLE = "enable"
CONF_FUNCTION = "function"
CONF_LOCKED = "locked"
CONF_FIRMWARE_PATH = "firmware_path"
CONF_DEFAULT_BRIGHTNESS = "default_brightness"
CONF_CALIBRATION_NAME = "calibration_name"
CONF_CALIBRATION_VALUE = "calibration_value"

# TCP Bridge for dpsctl.py access
CONF_TCP_BRIDGE = "tcp_bridge"
CONF_TCP_BRIDGE_PORT = "tcp_bridge_port"
CONF_TCP_BRIDGE_DISCONNECT_DELAY = "tcp_bridge_disconnect_delay"
CONF_TCP_BRIDGE_FRAME_TIMEOUT = "tcp_bridge_frame_timeout"

# Bootloader baud rate for firmware upgrades (dpsboot may use different rate than main firmware)
CONF_BOOTLOADER_BAUD_RATE = "bootloader_baud_rate"

VALID_BAUD_RATES = [9600, 19200, 38400, 57600, 115200]


def validate_uart_baud(value):
    value = cv.positive_int(value)
    if value not in VALID_BAUD_RATES:
        raise cv.Invalid(f"Invalid baud rate {value}. Valid values: {VALID_BAUD_RATES}")
    return value


# Calibration backup/restore
CONF_CALIBRATION_BACKUP = "calibration_backup"
CONF_CALIBRATION_BACKUP_PATH = "calibration_backup_path"
CONF_CALIBRATION_AUTO_RESTORE = "calibration_auto_restore"

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
SetUartBaudAction = opendps_ns.class_("SetUartBaudAction", automation.Action)
PingAction = opendps_ns.class_("PingAction", automation.Action)
RequestVersionAction = opendps_ns.class_("RequestVersionAction", automation.Action)
UpgradeFirmwareAction = opendps_ns.class_("UpgradeFirmwareAction", automation.Action)
RequestCalibrationReportAction = opendps_ns.class_(
    "RequestCalibrationReportAction", automation.Action
)
SetCalibrationAction = opendps_ns.class_("SetCalibrationAction", automation.Action)
ClearCalibrationAction = opendps_ns.class_("ClearCalibrationAction", automation.Action)

# Calibration backup/restore actions
SaveCalibrationAction = opendps_ns.class_("SaveCalibrationAction", automation.Action)
RestoreCalibrationAction = opendps_ns.class_(
    "RestoreCalibrationAction", automation.Action
)

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

DATALOG_FORMATS = {
    "csv": DATALOG_FORMAT_CSV,
    "bin": DATALOG_FORMAT_BINARY,
}

DataloggerConfig = opendps_ns.struct("DataloggerConfig")

# Datalogger column flags - must match C++ enum DatalogColumn in opendps.h
DATALOG_COL_ELAPSED_MS = 1 << 0  # Time since log start (milliseconds)
DATALOG_COL_SYSTEM_TIME = 1 << 1  # System time (from time component, ISO8601 format)
DATALOG_COL_VOLTAGE_IN = 1 << 2
DATALOG_COL_VOLTAGE_OUT = 1 << 3
DATALOG_COL_CURRENT_OUT = 1 << 4
DATALOG_COL_POWER_OUT = 1 << 5
DATALOG_COL_OUTPUT_ENABLED = 1 << 6
DATALOG_COL_TEMP1 = 1 << 7
DATALOG_COL_TEMP2 = 1 << 8
# Legacy alias
DATALOG_COL_TIMESTAMP = DATALOG_COL_ELAPSED_MS

DATALOG_COLUMNS = {
    "elapsed_ms": DATALOG_COL_ELAPSED_MS,
    "system_time": DATALOG_COL_SYSTEM_TIME,
    "timestamp": DATALOG_COL_TIMESTAMP,  # Legacy alias for elapsed_ms
    "voltage_in": DATALOG_COL_VOLTAGE_IN,
    "voltage_out": DATALOG_COL_VOLTAGE_OUT,
    "current_out": DATALOG_COL_CURRENT_OUT,
    "power_out": DATALOG_COL_POWER_OUT,
    "output_enabled": DATALOG_COL_OUTPUT_ENABLED,
    "temp1": DATALOG_COL_TEMP1,
    "temp2": DATALOG_COL_TEMP2,
}

# Datalogger actions
StartDatalogAction = opendps_ns.class_("StartDatalogAction", automation.Action)
StopDatalogAction = opendps_ns.class_("StopDatalogAction", automation.Action)
FlushDatalogAction = opendps_ns.class_("FlushDatalogAction", automation.Action)

# Config keys for calibration assistant
CONF_MEASURED_VALUE = "measured_value"

TCP_BRIDGE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TCP_BRIDGE_PORT, default=5005): cv.port,
        # Grace period after client disconnect before resuming normal operation
        # Allows tools like dpsctl.py to reconnect between commands (e.g., during calibration)
        cv.Optional(
            CONF_TCP_BRIDGE_DISCONNECT_DELAY, default="500ms"
        ): cv.positive_time_period_milliseconds,
        # Timeout for incomplete UART frames - clears buffer if no EOF received
        cv.Optional(
            CONF_TCP_BRIDGE_FRAME_TIMEOUT, default="500ms"
        ): cv.positive_time_period_milliseconds,
    }
)

CALIBRATION_BACKUP_SCHEMA = cv.Schema(
    {
        # Path to store calibration backup file (text format, human readable)
        cv.Optional(
            CONF_CALIBRATION_BACKUP_PATH, default="/sd/opendps_calibration.cfg"
        ): cv.string,
        # Automatically restore calibration after successful firmware upgrade
        cv.Optional(CONF_CALIBRATION_AUTO_RESTORE, default=False): cv.boolean,
    }
)


def validate_format(value):
    """Validate datalogger forma"""
    if value not in DATALOG_FORMATS:
        raise cv.Invalid(f"Unknown value '{value}'. Valid formats are csv and binary.")
    return value


def validate_columns(value):
    """Validate and convert column list to bitmask."""
    if isinstance(value, list):
        result = 0
        for col in value:
            if col not in DATALOG_COLUMNS:
                raise cv.Invalid(
                    f"Unknown column '{col}'. Valid columns: {list(DATALOG_COLUMNS.keys())}"
                )
            result |= DATALOG_COLUMNS[col]
        return result
    raise cv.Invalid("columns must be a list")


DATALOGGER_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_STORAGE_PATH, default="/sd/logs"): cv.string,
        cv.Optional(CONF_FILENAME_FORMAT): cv.string,
        cv.Optional(CONF_FILENAME_ID): cv.string,
        cv.Optional(CONF_BUFFER_SIZE, default=65536): cv.int_range(
            min=1024, max=1048576
        ),
        cv.Optional(CONF_FORMAT, default="csv"): validate_format,
        cv.Optional(CONF_COLUMNS): validate_columns,
        cv.Optional(
            CONF_FLUSH_INTERVAL, default="5s"
        ): cv.positive_time_period_milliseconds,
        # Time component for system_time column (sntp, homeassistant, ds1307, etc.)
        cv.Optional(CONF_TIME_ID): cv.use_id(time_component.RealTimeClock),
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
            # Datalogger for high-speed data logging to storage with PSRAM buffering
            cv.Optional(CONF_DATALOGGER): DATALOGGER_SCHEMA,
            # Bootloader baud rate - dpsboot may use different rate than main firmware
            # Set to 0 (default) to use same rate as UART, or specify explicit rate (e.g., 9600)
            cv.Optional(CONF_BOOTLOADER_BAUD_RATE, default=0): cv.int_range(
                min=0, max=921600
            ),
            # Calibration backup/restore configuration
            cv.Optional(CONF_CALIBRATION_BACKUP): CALIBRATION_BACKUP_SCHEMA,
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

    # Bootloader baud rate for firmware upgrades
    if config[CONF_BOOTLOADER_BAUD_RATE] > 0:
        cg.add(var.set_bootloader_baud_rate(config[CONF_BOOTLOADER_BAUD_RATE]))

    # TCP bridge configuration for dpsctl.py access
    if CONF_TCP_BRIDGE in config:
        tcp_bridge_config = config[CONF_TCP_BRIDGE]
        cg.add(var.set_tcp_bridge_enabled(True))
        cg.add(var.set_tcp_bridge_port(tcp_bridge_config[CONF_TCP_BRIDGE_PORT]))
        cg.add(
            var.set_tcp_bridge_disconnect_delay(
                tcp_bridge_config[CONF_TCP_BRIDGE_DISCONNECT_DELAY]
            )
        )
        cg.add(
            var.set_tcp_bridge_frame_timeout(
                tcp_bridge_config[CONF_TCP_BRIDGE_FRAME_TIMEOUT]
            )
        )

    for conf in config.get(CONF_ON_CONNECT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    # Calibration backup configuration
    if CONF_CALIBRATION_BACKUP in config:
        cal_backup_config = config[CONF_CALIBRATION_BACKUP]
        cg.add(
            var.set_calibration_backup_path(
                cal_backup_config[CONF_CALIBRATION_BACKUP_PATH]
            )
        )
        cg.add(
            var.set_auto_restore_calibration(
                cal_backup_config[CONF_CALIBRATION_AUTO_RESTORE]
            )
        )

    # Datalogger configuration
    if CONF_DATALOGGER in config:
        datalog_config = config[CONF_DATALOGGER]
        # Create DataloggerConfig struct
        config_struct = cg.StructInitializer(
            DataloggerConfig,
            ("storage_path", datalog_config[CONF_STORAGE_PATH]),
            (
                "filename_format",
                datalog_config.get(CONF_FILENAME_FORMAT, ""),
            ),
            ("filename_id", datalog_config.get(CONF_FILENAME_ID, "")),
            ("buffer_size", datalog_config[CONF_BUFFER_SIZE]),
            ("format", datalog_config[CONF_FORMAT]),
            (
                "columns",
                datalog_config.get(
                    CONF_COLUMNS,
                    DATALOG_COL_ELAPSED_MS
                    | DATALOG_COL_SYSTEM_TIME
                    | DATALOG_COL_VOLTAGE_IN
                    | DATALOG_COL_VOLTAGE_OUT
                    | DATALOG_COL_CURRENT_OUT
                    | DATALOG_COL_POWER_OUT
                    | DATALOG_COL_OUTPUT_ENABLED,
                ),
            ),
            ("flush_interval_ms", datalog_config[CONF_FLUSH_INTERVAL]),
        )
        cg.add(var.set_datalogger_config(config_struct))

        # Time component for system_time column
        if CONF_TIME_ID in datalog_config:
            time_var = await cg.get_variable(datalog_config[CONF_TIME_ID])
            cg.add(var.set_datalog_time(time_var))


# Automation Actions
OPENDPS_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(OpenDPS)})


@automation.register_action(
    "opendps.enable_output",
    EnableOutputAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_ENABLE): cv.templatable(cv.boolean)}
    ),
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
)
async def opendps_set_brightness_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_BRIGHTNESS], args, cg.uint8)
    cg.add(var.set_brightness(template_))
    return var


@automation.register_action(
    "opendps.set_uart_baud",
    SetUartBaudAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {cv.Required(CONF_BAUD_RATE): cv.templatable(validate_uart_baud)}
    ),
    synchronous=True,
)
async def opendps_set_uart_baud_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_BAUD_RATE], args, cg.uint32)
    cg.add(var.set_baud_rate(template_))
    return var


@automation.register_action(
    "opendps.ping", PingAction, OPENDPS_ACTION_SCHEMA, synchronous=True
)
async def opendps_ping_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.request_version",
    RequestVersionAction,
    OPENDPS_ACTION_SCHEMA,
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
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
    synchronous=True,
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
    "opendps.clear_calibration",
    ClearCalibrationAction,
    OPENDPS_ACTION_SCHEMA,
    synchronous=True,
)
async def opendps_clear_calibration_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.save_calibration",
    SaveCalibrationAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def opendps_save_calibration_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    if CONF_PATH in config:
        template_path = await cg.templatable(config[CONF_PATH], args, cg.std_string)
        cg.add(var.set_path(template_path))
    return var


@automation.register_action(
    "opendps.restore_calibration",
    RestoreCalibrationAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_PATH): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def opendps_restore_calibration_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    if CONF_PATH in config:
        template_path = await cg.templatable(config[CONF_PATH], args, cg.std_string)
        cg.add(var.set_path(template_path))
    return var


# Calibration Assistant Actions
@automation.register_action(
    "opendps.start_calibration_assistant",
    StartCalibrationAssistantAction,
    OPENDPS_ACTION_SCHEMA,
    synchronous=True,
)
async def opendps_start_calibration_assistant_to_code(
    config, action_id, template_arg, args
):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.calibration_assistant_step",
    CalibrationAssistantStepAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_MEASURED_VALUE, default=0): cv.templatable(cv.float_),
        }
    ),
    synchronous=True,
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
    synchronous=True,
)
async def opendps_cancel_calibration_assistant_to_code(
    config, action_id, template_arg, args
):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


# Datalogger Actions
@automation.register_action(
    "opendps.start_datalog",
    StartDatalogAction,
    OPENDPS_ACTION_SCHEMA.extend(
        {
            cv.Optional(CONF_FILENAME, default=""): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def opendps_start_datalog_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_FILENAME], args, cg.std_string)
    cg.add(var.set_filename(template_))
    return var


@automation.register_action(
    "opendps.stop_datalog",
    StopDatalogAction,
    OPENDPS_ACTION_SCHEMA,
    synchronous=True,
)
async def opendps_stop_datalog_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "opendps.flush_datalog",
    FlushDatalogAction,
    OPENDPS_ACTION_SCHEMA,
    synchronous=True,
)
async def opendps_flush_datalog_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
