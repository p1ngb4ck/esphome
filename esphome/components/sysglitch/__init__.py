"""SysGlitch - RL78/G13 OCD Voltage Glitcher for PS4/PSVITA Syscon."""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODE

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["uart"]

CONF_TOOL0_UART = "tool0_uart"
CONF_PC_UART = "pc_uart"
CONF_RESET_PIN = "reset_pin"
CONF_GLITCH_PIN = "glitch_pin"
CONF_RX_PULLDOWN_PIN = "rx_pulldown_pin"
CONF_GLITCH_DELAY_MIN = "glitch_delay_min"
CONF_GLITCH_DELAY_MAX = "glitch_delay_max"
CONF_GLITCH_WIDTH_MAX = "glitch_width_max"
CONF_PASSCODE = "passcode"
CONF_MAX_ATTEMPTS = "max_attempts"
CONF_DUMP_PATH = "dump_path"
CONF_WRITE_PATH = "write_path"

sysglitch_ns = cg.esphome_ns.namespace("sysglitch")
SysGlitch = sysglitch_ns.class_("SysGlitch", cg.Component)
SysGlitchMode = sysglitch_ns.enum("SysGlitchMode")

# Mode enum values
MODES = {
    "dump_sd": SysGlitchMode.MODE_DUMP_SD,
    "dump_uart": SysGlitchMode.MODE_DUMP_UART,
    "flasher": SysGlitchMode.MODE_FLASHER,
    "write": SysGlitchMode.MODE_WRITE,
}

# Automation actions
StartGlitchAction = sysglitch_ns.class_("StartGlitchAction", automation.Action)
StopGlitchAction = sysglitch_ns.class_("StopGlitchAction", automation.Action)
StartWriteAction = sysglitch_ns.class_("StartWriteAction", automation.Action)


def validate_mode_config(config):
    """Validate that mode-specific requirements are met."""
    mode = config[CONF_MODE]

    # pc_uart required for dump_uart and flasher modes
    needs_pc_uart = mode in (
        MODES["dump_uart"],
        MODES["flasher"],
    )
    if needs_pc_uart and CONF_PC_UART not in config:
        raise cv.Invalid(
            "Modes 'dump_uart' and 'flasher' require 'pc_uart' to be configured"
        )

    # write_path required for write mode
    if mode == MODES["write"] and CONF_WRITE_PATH not in config:
        raise cv.Invalid("Mode 'write' requires 'write_path' to be configured")

    # glitch_pin and rx_pulldown_pin required for non-write modes
    if mode != MODES["write"]:
        if CONF_GLITCH_PIN not in config:
            raise cv.Invalid(
                f"'{CONF_GLITCH_PIN}' is required for modes other than 'write'"
            )
        if CONF_RX_PULLDOWN_PIN not in config:
            raise cv.Invalid(
                f"'{CONF_RX_PULLDOWN_PIN}' is required for modes other than 'write'"
            )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SysGlitch),
            cv.Required(CONF_TOOL0_UART): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_PC_UART): cv.use_id(uart.UARTComponent),
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_GLITCH_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RX_PULLDOWN_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_MODE, default="dump_sd"): cv.enum(MODES, lower=True),
            cv.Optional(CONF_DUMP_PATH, default="/sd/syscon_dump.bin"): cv.string,
            cv.Optional(CONF_WRITE_PATH): cv.string,
            cv.Optional(CONF_GLITCH_DELAY_MIN, default=1500): cv.uint32_t,
            cv.Optional(CONF_GLITCH_DELAY_MAX, default=7500): cv.uint32_t,
            cv.Optional(CONF_GLITCH_WIDTH_MAX, default=6300): cv.uint32_t,
            cv.Optional(
                CONF_PASSCODE,
                default=[0x3A, 0x4E, 0x6F, 0x74, 0x3A, 0x55, 0x73, 0x65, 0x64, 0x3A],
            ): cv.All(
                cv.ensure_list(cv.hex_uint8_t),
                cv.Length(min=10, max=10),
            ),
            cv.Optional(CONF_MAX_ATTEMPTS, default=0): cv.uint32_t,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_mode_config,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    tool0 = await cg.get_variable(config[CONF_TOOL0_UART])
    cg.add(var.set_tool0_uart(tool0))

    if CONF_PC_UART in config:
        pc = await cg.get_variable(config[CONF_PC_UART])
        cg.add(var.set_pc_uart(pc))

    reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset_pin))

    # Glitch pin and RX pulldown pin are optional (not needed for write mode)
    if CONF_GLITCH_PIN in config:
        glitch_pin = await cg.gpio_pin_expression(config[CONF_GLITCH_PIN])
        cg.add(var.set_glitch_pin(glitch_pin))

    if CONF_RX_PULLDOWN_PIN in config:
        rx_pulldown_pin = await cg.gpio_pin_expression(config[CONF_RX_PULLDOWN_PIN])
        cg.add(var.set_rx_pulldown_pin(rx_pulldown_pin))

    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_dump_path(config[CONF_DUMP_PATH]))
    if CONF_WRITE_PATH in config:
        cg.add(var.set_write_path(config[CONF_WRITE_PATH]))
    cg.add(var.set_glitch_delay_min(config[CONF_GLITCH_DELAY_MIN]))
    cg.add(var.set_glitch_delay_max(config[CONF_GLITCH_DELAY_MAX]))
    cg.add(var.set_glitch_width_max(config[CONF_GLITCH_WIDTH_MAX]))
    cg.add(var.set_max_attempts(config[CONF_MAX_ATTEMPTS]))

    passcode = config[CONF_PASSCODE]
    passcode_hex = ", ".join(f"0x{x:02X}" for x in passcode)
    cg.add(
        var.set_passcode(
            cg.RawExpression(f"std::array<uint8_t, 10>{{{{{passcode_hex}}}}}")
        )
    )


# Automation Actions
SYSGLITCH_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(SysGlitch)})


@automation.register_action(
    "sysglitch.start_glitch", StartGlitchAction, SYSGLITCH_ACTION_SCHEMA
)
async def sysglitch_start_glitch_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("sysglitch.stop", StopGlitchAction, SYSGLITCH_ACTION_SCHEMA)
async def sysglitch_stop_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "sysglitch.start_write", StartWriteAction, SYSGLITCH_ACTION_SCHEMA
)
async def sysglitch_start_write_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
