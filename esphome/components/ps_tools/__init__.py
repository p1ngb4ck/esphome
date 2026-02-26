from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import binary_storage, uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODE

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["binary_storage", "uart"]

# ── Config key constants ─────────────────────────────────────────────────────
CONF_TOOL0_UART = "tool0_uart"
CONF_PC_UART = "pc_uart"
CONF_RESET_PIN = "reset_pin"
CONF_GLITCH_PIN = "glitch_pin"
CONF_RX_PULLDOWN_PIN = "rx_pulldown_pin"
CONF_TOOL0_TX_PIN = "tool0_tx_pin"
CONF_GLITCH_DELAY_MIN = "glitch_delay_min"
CONF_GLITCH_DELAY_MAX = "glitch_delay_max"
CONF_GLITCH_WIDTH_MAX = "glitch_width_max"
CONF_MAX_ATTEMPTS = "max_attempts"
CONF_PASSCODE = "passcode"
CONF_VOLTAGE = "voltage"
CONF_DUMP_PATH = "dump_path"
CONF_WRITE_PATH = "write_path"
CONF_NOR_CHIP_ID = "nor_chip_id"
CONF_NOR_FLASH = "nor_flash"
CONF_NOR_DUMP_PATH = "nor_dump_path"

# ── Supported operating voltages for ProtoA baud-set command ────────────────
# The RL78/G13 baud-set frame includes a voltage byte (voltage * 10 in hex).
# The chip uses this to calibrate its internal oscillator.
VOLTAGE_OPTIONS = {
    "3.3v": 0x21,  # 3.3V (some boards/level-shifter setups)
    "5.0v": 0x32,  # 5.0V (native PS4 syscon voltage, Arduino Nano reference)
}

# ── Mode strings ─────────────────────────────────────────────────────────────
MODE_GLITCH_DUMP = "glitch_dump"
MODE_GLITCH_FLASHER = "glitch_flasher"
MODE_PROTO_A_WRITE = "proto_a_write"
MODE_PROTO_A_READ = "proto_a_read"
MODE_PROTO_A_BLANK_CHECK = "proto_a_blank_check"

# ── C++ namespace / class bindings ───────────────────────────────────────────
ps_tools_ns = cg.esphome_ns.namespace("ps_tools")
PsTools = ps_tools_ns.class_("PsTools", cg.Component)
PsToolsMode = ps_tools_ns.enum("PsToolsMode")

MODES = {
    MODE_GLITCH_DUMP: PsToolsMode.MODE_GLITCH_DUMP,
    MODE_GLITCH_FLASHER: PsToolsMode.MODE_GLITCH_FLASHER,
    MODE_PROTO_A_WRITE: PsToolsMode.MODE_PROTO_A_WRITE,
    MODE_PROTO_A_READ: PsToolsMode.MODE_PROTO_A_READ,
    MODE_PROTO_A_BLANK_CHECK: PsToolsMode.MODE_PROTO_A_BLANK_CHECK,
}

# ── Automation action bindings ───────────────────────────────────────────────
StartGlitchAction = ps_tools_ns.class_("StartGlitchAction", automation.Action)
StopAction = ps_tools_ns.class_("StopAction", automation.Action)
StartWriteAction = ps_tools_ns.class_("StartWriteAction", automation.Action)
StartReadAction = ps_tools_ns.class_("StartReadAction", automation.Action)
StartBlankCheckAction = ps_tools_ns.class_("StartBlankCheckAction", automation.Action)
AnalyzeNorAction = ps_tools_ns.class_("AnalyzeNorAction", automation.Action)
ProbeSysconAction = ps_tools_ns.class_("ProbeSysconAction", automation.Action)

# ── Default OCD passcode: ":Not:Used:" (unmodified PS4 syscon) ───────────────
_DEFAULT_PASSCODE = [0x3A, 0x4E, 0x6F, 0x74, 0x3A, 0x55, 0x73, 0x65, 0x64, 0x3A]


def _validate_write_path(value):
    """Validate write_path and infer file format from extension."""
    value = cv.string(value)
    lower = value.lower()
    if not (lower.endswith(".bin") or lower.endswith(".mot")):
        raise cv.Invalid(f"write_path must end in .bin or .mot, got: {value!r}")
    return value


def _validate_config(config):
    """Cross-field validation — runs after individual field validation."""
    mode = config[CONF_MODE]

    # pc_uart is required for modes that communicate with a PC tool
    if mode == MODE_GLITCH_FLASHER and CONF_PC_UART not in config:
        raise cv.Invalid(
            f"Mode '{MODE_GLITCH_FLASHER}' requires 'pc_uart' to be configured"
        )

    # write_path is required for write mode
    if mode == MODE_PROTO_A_WRITE and CONF_WRITE_PATH not in config:
        raise cv.Invalid(
            f"Mode '{MODE_PROTO_A_WRITE}' requires 'write_path' to be configured"
        )

    # glitch_pin is required for glitch modes
    if (
        mode in (MODE_GLITCH_DUMP, MODE_GLITCH_FLASHER)
        and CONF_GLITCH_PIN not in config
    ):
        raise cv.Invalid(f"Mode '{mode}' requires 'glitch_pin' to be configured")

    # tool0_tx_pin is required for ProtoA modes (needed to drive TX LOW manually)
    if (
        mode in (MODE_PROTO_A_WRITE, MODE_PROTO_A_READ, MODE_PROTO_A_BLANK_CHECK)
        and CONF_TOOL0_TX_PIN not in config
    ):
        raise cv.Invalid(f"Mode '{mode}' requires 'tool0_tx_pin' to be configured")

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PsTools),
            # ── UART channels ──
            cv.Required(CONF_TOOL0_UART): cv.use_id(uart.UARTComponent),
            cv.Optional(CONF_PC_UART): cv.use_id(uart.UARTComponent),
            # ── GPIO pins ──
            cv.Required(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_GLITCH_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RX_PULLDOWN_PIN): pins.gpio_output_pin_schema,
            # GPIO number of TOOL0 UART TX pin (int, not pin schema —
            # we need to call driver-level GPIO functions on it directly)
            cv.Optional(CONF_TOOL0_TX_PIN): cv.int_range(min=0, max=48),
            # ── Mode ──
            cv.Optional(CONF_MODE, default=MODE_GLITCH_DUMP): cv.one_of(
                *MODES, lower=True
            ),
            # ── File paths ──
            cv.Optional(CONF_DUMP_PATH, default="/sd/syscon_dump.bin"): cv.string,
            cv.Optional(CONF_WRITE_PATH): _validate_write_path,
            # ── Glitch timing ──
            cv.Optional(CONF_GLITCH_DELAY_MIN, default=1500): cv.uint32_t,
            cv.Optional(CONF_GLITCH_DELAY_MAX, default=7500): cv.uint32_t,
            cv.Optional(CONF_GLITCH_WIDTH_MAX, default=6300): cv.uint32_t,
            cv.Optional(CONF_MAX_ATTEMPTS, default=0): cv.uint32_t,
            # ── OCD passcode (10 bytes, default = :Not:Used:) ──
            cv.Optional(CONF_PASSCODE, default=_DEFAULT_PASSCODE): cv.All(
                cv.ensure_list(cv.hex_uint8_t),
                cv.Length(min=10, max=10),
            ),
            # ── ProtoA operating voltage ──
            # Must match actual hardware voltage to calibrate RL78 oscillator.
            # "5.0v" = native PS4 syscon voltage (Arduino Nano reference circuit).
            # "3.3v" = ESP32 direct connection via level shifter.
            cv.Optional(CONF_VOLTAGE, default="5.0v"): cv.one_of(
                *VOLTAGE_OPTIONS, lower=True
            ),
            # ── NOR flash chip identifier (informational) ──
            # Used for logging and future smart-repair linking syscon+NOR ops.
            # Example: "MX25L25635F"
            cv.Optional(CONF_NOR_CHIP_ID): cv.string,
            # ── NOR flash device (32MB PS4 SPI NOR via binary_storage) ──
            # Provide either nor_flash (live hardware) or nor_dump_path (offline file)
            # for analyze_nor action.
            cv.Optional(CONF_NOR_FLASH): cv.use_id(binary_storage.BinaryStorage),
            cv.Optional(CONF_NOR_DUMP_PATH): cv.string,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_config,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # ── UART channels ──
    tool0 = await cg.get_variable(config[CONF_TOOL0_UART])
    cg.add(var.set_tool0_uart(tool0))

    if CONF_PC_UART in config:
        pc = await cg.get_variable(config[CONF_PC_UART])
        cg.add(var.set_pc_uart(pc))

    # ── Pins ──
    reset_pin = await cg.gpio_pin_expression(config[CONF_RESET_PIN])
    cg.add(var.set_reset_pin(reset_pin))

    if CONF_GLITCH_PIN in config:
        glitch_pin = await cg.gpio_pin_expression(config[CONF_GLITCH_PIN])
        cg.add(var.set_glitch_pin(glitch_pin))

    if CONF_RX_PULLDOWN_PIN in config:
        rx_pd_pin = await cg.gpio_pin_expression(config[CONF_RX_PULLDOWN_PIN])
        cg.add(var.set_rx_pulldown_pin(rx_pd_pin))

    if CONF_TOOL0_TX_PIN in config:
        cg.add(var.set_tool0_tx_gpio(config[CONF_TOOL0_TX_PIN]))

    # ── Mode ──
    cg.add(var.set_mode(MODES[config[CONF_MODE]]))

    # ── Paths ──
    cg.add(var.set_dump_path(config[CONF_DUMP_PATH]))

    if CONF_WRITE_PATH in config:
        write_path = config[CONF_WRITE_PATH]
        cg.add(var.set_write_path(write_path))
        # Inform C++ whether the file is .mot or .bin
        cg.add(var.set_write_is_mot(write_path.lower().endswith(".mot")))

    # ── Glitch timing ──
    cg.add(var.set_glitch_delay_min(config[CONF_GLITCH_DELAY_MIN]))
    cg.add(var.set_glitch_delay_max(config[CONF_GLITCH_DELAY_MAX]))
    cg.add(var.set_glitch_width_max(config[CONF_GLITCH_WIDTH_MAX]))
    cg.add(var.set_max_attempts(config[CONF_MAX_ATTEMPTS]))

    # ── Passcode ──
    passcode = config[CONF_PASSCODE]
    passcode_hex = ", ".join(f"0x{x:02X}" for x in passcode)
    cg.add(
        var.set_passcode(
            cg.RawExpression(f"std::array<uint8_t, 10>{{{{{passcode_hex}}}}}")
        )
    )

    # ── Voltage byte ──
    cg.add(var.set_voltage_byte(VOLTAGE_OPTIONS[config[CONF_VOLTAGE]]))

    # ── NOR chip ID ──
    if CONF_NOR_CHIP_ID in config:
        cg.add(var.set_nor_chip_id(config[CONF_NOR_CHIP_ID]))

    # ── NOR flash device ──
    if CONF_NOR_FLASH in config:
        nor = await cg.get_variable(config[CONF_NOR_FLASH])
        cg.add(var.set_nor_flash(nor))

    # ── NOR dump path (offline analysis) ──
    if CONF_NOR_DUMP_PATH in config:
        cg.add(var.set_nor_dump_path(config[CONF_NOR_DUMP_PATH]))


# ── Automation action schemas and registration ───────────────────────────────
PS_TOOLS_ACTION_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(PsTools)})


@automation.register_action(
    "ps_tools.start_glitch", StartGlitchAction, PS_TOOLS_ACTION_SCHEMA
)
async def ps_tools_start_glitch_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action("ps_tools.stop", StopAction, PS_TOOLS_ACTION_SCHEMA)
async def ps_tools_stop_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "ps_tools.start_write", StartWriteAction, PS_TOOLS_ACTION_SCHEMA
)
async def ps_tools_start_write_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "ps_tools.start_read", StartReadAction, PS_TOOLS_ACTION_SCHEMA
)
async def ps_tools_start_read_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "ps_tools.start_blank_check", StartBlankCheckAction, PS_TOOLS_ACTION_SCHEMA
)
async def ps_tools_start_blank_check_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "ps_tools.analyze_nor", AnalyzeNorAction, PS_TOOLS_ACTION_SCHEMA
)
async def ps_tools_analyze_nor_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "ps_tools.probe_syscon", ProbeSysconAction, PS_TOOLS_ACTION_SCHEMA
)
async def ps_tools_probe_syscon_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
