from collections.abc import Callable

import esphome.codegen as cg
from esphome.components.const import CONF_DATA_BITS, CONF_PARITY, CONF_STOP_BITS
from esphome.components.esp32 import VARIANT_ESP32P4, get_esp32_variant
from esphome.components.uart import (
    CONF_DEBUG_PREFIX,
    CONF_FLUSH_TIMEOUT,
    UARTComponent,
    UARTParityOptions,
    mark_unclocked,
    restore_line_settings,
)
from esphome.components.usb_host import (
    CONF_PID,
    CONF_VID,
    get_max_packet_size,
    get_max_transfer_requests,
    register_usb_client,
    usb_device_schema,
    validate_usb_clients,
)
from esphome.config_helpers import filter_source_files_from_defines
import esphome.config_validation as cv
from esphome.const import (
    CONF_BAUD_RATE,
    CONF_BUFFER_SIZE,
    CONF_CHANNELS,
    CONF_DEBUG,
    CONF_DEVICES,
    CONF_DUMMY_RECEIVER,
    CONF_ID,
    CONF_TYPE,
)
from esphome.core import CORE, ID
from esphome.cpp_types import Component
from esphome.types import ConfigType

AUTO_LOAD = ["uart", "usb_host", "bytebuffer"]
CODEOWNERS = ["@clydebarrow"]
DOMAIN = "usb_uart"

usb_uart_ns = cg.esphome_ns.namespace("usb_uart")
# One client for every adapter; the type classes are the per-adapter drivers it makes on plug
USBUartClient = usb_uart_ns.class_("USBUartClient", Component)
USBUartComponent = usb_uart_ns.class_("USBUartComponent")
UartType = usb_uart_ns.enum("UartType", is_class=True)
UART_TYPES = {
    "CdcAcm": UartType.CDC_ACM,
    "CH34X": UartType.CH34X,
    "CH934X": UartType.CH934X,
    "CP210X": UartType.CP210X,
    "FT23XX": UartType.FT23XX,
    "PL2303": UartType.PL2303,
}
USBUartChannel = usb_uart_ns.class_("USBUartChannel", UARTComponent)
CH934XChannel = usb_uart_ns.class_("CH934XChannel", USBUartChannel)


def is_usb_uart_channel(uart_id: ID, full_config: ConfigType) -> bool:
    return any(
        channel[CONF_ID] == uart_id
        for device in (full_config.get(DOMAIN) or {}).get(CONF_DEVICES) or []
        for channel in device[CONF_CHANNELS]
    )


UART_PARITY_OPTIONS = UARTParityOptions.options(
    "NONE", "EVEN", "ODD", "MARK", "SPACE", prefix="UART_CONFIG_PARITY_"
)

UARTStopBitsOptions = usb_uart_ns.enum("UARTStopBitsOptions")
UART_STOP_BITS_OPTIONS = UARTStopBitsOptions.options(
    "1", "1.5", "2", prefix="UART_CONFIG_STOP_BITS_"
)

DEFAULT_BAUD_RATE = 9600
# CH934x TX packets carry a 3-byte header ([port, len_lo, len_hi]) inside every chunk, so the
# usable payload per chunk is that much smaller. Kept in sync with CH934XChannel::TX_HEADER_SIZE.
CH934X_TX_HEADER_SIZE = 3
CONF_DEBUG_ADD_UART_SETTINGS = "debug_add_uart_settings"
CONF_CLAIM_NOTIFICATION_EP = "claim_notification_ep"
CONF_CLAIM_COMM_INTERFACE = "claim_comm_interface"


class Type:
    def __init__(
        self,
        name: str,
        vid: int,
        pid: int,
        cls: str | None,
        max_channels: int = 1,
        baud_rate_required: bool = True,
        channel_cls=None,
        max_baud: int = 1_000_000,
        has_comm_interface: bool = False,
    ) -> None:
        self.name = name
        cls = cls or name
        self.cls_name = cls
        self.vid = vid
        self.pid = pid
        self.cls = usb_uart_ns.class_(f"USBUartType{cls}", USBUartComponent)
        # CDC ACM lives in usb_uart.cpp; each vendor driver has its own <cls>.cpp
        self.driver = None if cls == "CdcAcm" else cls
        self._max_channels = max_channels
        self.baud_rate_required = baud_rate_required
        self.channel_cls = channel_cls or USBUartChannel
        self.max_baud = max_baud
        # True for types that claim the CDC comm (interrupt) interface; only these
        # accept the claim_comm_interface option.
        self.has_comm_interface = has_comm_interface

    @property
    def max_channels(self):
        return (
            3
            if CORE.is_esp32
            and get_esp32_variant() != VARIANT_ESP32P4
            and self._max_channels > 3
            else self._max_channels
        )


class MpxType(Type):
    @property
    def max_channels(self):
        # Multiplexed devices aren't restricted by the number of available USB endpoints
        return self._max_channels


uart_types = (
    MpxType("CH9344", 0x1A86, 0xE018, "CH934X", 4, channel_cls=CH934XChannel),
    MpxType("CH9344L", 0x1A86, 0xE018, "CH934X", 4, channel_cls=CH934XChannel),
    MpxType("CH9344Q", 0x1A86, 0xE018, "CH934X", 4, channel_cls=CH934XChannel),
    MpxType("CH348", 0x1A86, 0x55D9, "CH934X", 8, channel_cls=CH934XChannel),
    MpxType("CH348L", 0x1A86, 0x55D9, "CH934X", 8, channel_cls=CH934XChannel),
    MpxType("CH348Q", 0x1A86, 0x55D9, "CH934X", 8, channel_cls=CH934XChannel),
    Type(
        "CDC_ACM", 0, 0, "CdcAcm", 1, baud_rate_required=False, has_comm_interface=True
    ),
    Type("CH34X", 0x1A86, 0x55D5, "CH34X", 4, max_baud=2_000_000),
    Type("CH340", 0x1A86, 0x7523, "CH34X", 1, max_baud=2_000_000),
    Type("CP210X", 0x10C4, 0xEA60, "CP210X", 3, max_baud=2_000_000),
    Type(
        "ESP_JTAG",
        0x303A,
        0x1001,
        "CdcAcm",
        1,
        baud_rate_required=False,
        has_comm_interface=True,
    ),
    Type("FT232", 0x0403, 0x6001, "FT23XX", 1, max_baud=3_000_000),
    Type("FT2232", 0x0403, 0x6010, "FT23XX", 2, max_baud=12_000_000),
    Type("FT4232", 0x0403, 0x6011, "FT23XX", 4, max_baud=12_000_000),
    Type("PL2303", 0x067B, 0x2303, "PL2303", 1, max_baud=6_000_000),
    Type("PL2303GB", 0x067B, 0x23B3, "PL2303", 1, max_baud=6_000_000),
    Type("PL2303GC", 0x067B, 0x23A3, "PL2303", 1, max_baud=6_000_000),
    Type("PL2303GE", 0x067B, 0x23E3, "PL2303", 1, max_baud=6_000_000),
    Type("PL2303GL", 0x067B, 0x23D3, "PL2303", 1, max_baud=6_000_000),
    Type("PL2303GS", 0x067B, 0x23F3, "PL2303", 1, max_baud=6_000_000),
    Type("PL2303GT", 0x067B, 0x23C3, "PL2303", 1, max_baud=6_000_000),
    Type(
        "STM32_VCP",
        0x0483,
        0x5740,
        "CdcAcm",
        1,
        baud_rate_required=False,
        has_comm_interface=True,
    ),
)

_TYPES_BY_NAME = {it.name: it for it in uart_types}


def _driver_define(driver: str) -> str:
    return f"USE_USB_UART_{driver}"


FILTER_SOURCE_FILES = filter_source_files_from_defines(
    {
        f"{it.driver.lower()}.cpp": _driver_define(it.driver)
        for it in uart_types
        if it.driver is not None
    }
)


def _validate_debug_add_uart_settings(config):
    if config.get(CONF_DEBUG_ADD_UART_SETTINGS) and not config.get(CONF_DEBUG):
        raise cv.Invalid(
            f"'{CONF_DEBUG_ADD_UART_SETTINGS}' requires '{CONF_DEBUG}' to be true"
        )
    return config


def _channel_limit(type_: Type) -> Callable[[list], list]:
    """Check the channel count against the type's limit, which depends on the target."""

    def validator(value: list) -> list:
        if len(value) > (limit := type_.max_channels):
            raise cv.Invalid(
                f"Device type {type_.name} supports a maximum of {limit} channels"
            )
        return value

    return validator


def channel_schema(type_: "Type") -> cv.Schema:
    max_channels = type_.max_channels
    schema = cv.Schema(
        {
            cv.Required(CONF_CHANNELS): cv.All(
                cv.ensure_list(
                    cv.All(
                        cv.Schema(
                            {
                                cv.GenerateID(): cv.All(
                                    cv.declare_id(type_.channel_cls), mark_unclocked
                                ),
                                cv.Optional(
                                    CONF_BUFFER_SIZE, default=256
                                ): cv.int_range(min=64, max=8192),
                                (
                                    cv.Required(CONF_BAUD_RATE)
                                    if type_.baud_rate_required
                                    else cv.Optional(
                                        CONF_BAUD_RATE, default=DEFAULT_BAUD_RATE
                                    )
                                ): cv.int_range(min=300, max=type_.max_baud),
                                cv.Optional(CONF_STOP_BITS, default="1"): cv.enum(
                                    UART_STOP_BITS_OPTIONS, upper=True
                                ),
                                cv.Optional(CONF_PARITY, default="NONE"): cv.enum(
                                    UART_PARITY_OPTIONS, upper=True
                                ),
                                cv.Optional(CONF_DATA_BITS, default=8): cv.int_range(
                                    min=5, max=8
                                ),
                                cv.Optional(
                                    CONF_DUMMY_RECEIVER, default=False
                                ): cv.boolean,
                                cv.Optional(
                                    CONF_CLAIM_NOTIFICATION_EP, default=False
                                ): cv.boolean,
                                cv.Optional(CONF_DEBUG, default=False): cv.boolean,
                                cv.Optional(CONF_DEBUG_PREFIX, default=""): cv.string,
                                cv.Optional(
                                    CONF_DEBUG_ADD_UART_SETTINGS, default=False
                                ): cv.boolean,
                                cv.Optional(
                                    CONF_FLUSH_TIMEOUT, default="100ms"
                                ): cv.positive_time_period_milliseconds,
                            }
                        ),
                        _validate_debug_add_uart_settings,
                    )
                ),
                _channel_limit(type_),
            ),
        }
    )
    if type_.has_comm_interface:
        # The comm (interrupt) interface pins a host hardware channel per device;
        # disable to save one on channel-poor hosts (some devices may need it
        # claimed before enabling data flow).
        schema = schema.extend(
            {cv.Optional(CONF_CLAIM_COMM_INTERFACE, default=True): cv.boolean}
        )
    else:
        schema = schema.extend(
            {
                cv.Optional(CONF_CLAIM_COMM_INTERFACE): cv.invalid(
                    f"'{CONF_CLAIM_COMM_INTERFACE}' is only supported on device types "
                    f"that claim the CDC comm interface; {type_.name} never claims it"
                )
            }
        )
    return schema


def _entry_schema(type_: "Type") -> cv.Schema:
    # An entry names its adapter by vid / pid (the type's by default); no object of its own
    schema = cv.Schema({cv.GenerateID(): cv.declare_id(type_.cls)})
    if type_.vid:
        schema = schema.extend(
            {cv.Optional(CONF_VID, default=type_.vid): cv.hex_uint16_t}
        )
    else:
        schema = schema.extend({cv.Optional(CONF_VID): cv.hex_uint16_t})
    if type_.pid:
        schema = schema.extend(
            {cv.Optional(CONF_PID, default=type_.pid): cv.hex_uint16_t}
        )
    else:
        schema = schema.extend({cv.Optional(CONF_PID): cv.hex_uint16_t})
    return schema.extend(channel_schema(type_))


def _entries_block(value):
    # The former form, a list of entries, is the block's devices
    if isinstance(value, list):
        return {CONF_DEVICES: value}
    return value if value is not None else {}


# One client for every serial adapter. An adapter without an entry gets its type from its vid:pid
# (else CDC ACM) and channels with 115200 8N1 when it is plugged in; entries are optional.
CONFIG_SCHEMA = cv.All(
    _entries_block,
    usb_device_schema(USBUartClient).extend(
        {
            cv.Optional(CONF_DEVICES): cv.All(
                cv.ensure_list(
                    cv.typed_schema(
                        {it.name: _entry_schema(it) for it in uart_types},
                        upper=True,
                    )
                ),
                validate_usb_clients,
            ),
        }
    ),
)


async def to_code(config: ConfigType) -> None:
    devices = config.get(CONF_DEVICES) or []
    # The output chunk pool/queue are compile-time-sized templates shared by all channel
    # instances. Add one extra slot because LockFreeQueue<T,N> is a ring buffer that wastes
    # one entry.
    type_by_name = {t.name: t for t in uart_types}
    mps = get_max_packet_size()
    payload = mps - CH934X_TX_HEADER_SIZE

    output_chunk_count = 3
    for device in devices:
        device_type = type_by_name.get(device[CONF_TYPE])
        if isinstance(device_type, MpxType) and payload > 0:
            # A multiplexed device routes every channel's TX through the shared queue on
            # channel 0, and that queue only drains between loops -- so the pool has to hold
            # ceil(buffer / payload) chunks for EVERY channel at once, not just the largest.
            need = (
                sum(
                    -(-channel[CONF_BUFFER_SIZE] // payload)
                    for channel in device[CONF_CHANNELS]
                )
                + 1
            )
        else:
            device_max = max(
                channel[CONF_BUFFER_SIZE] for channel in device[CONF_CHANNELS]
            )
            need = max(device_max // mps, 2) + 1
        output_chunk_count = max(output_chunk_count, need)
    # LockFreeQueue/EventPool index slots with a uint8_t, so the count cannot exceed 255.
    output_chunk_count = min(output_chunk_count, 255)
    cg.add_define("USB_UART_OUTPUT_CHUNK_COUNT", output_chunk_count)

    # Multiplexed (CH934x) drivers initialise their channels in parallel over a single shared
    # command endpoint. Cap the concurrent init "lanes" at the transfer-request pool size so a
    # wide device cannot exhaust it during setup.
    max_init_lanes = get_max_transfer_requests()
    cg.add_define("USE_USB_BULK_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")

    # Every driver: an adapter of any type may be plugged in without an entry
    for driver in {it.driver for it in uart_types if it.driver is not None}:
        cg.add_define(_driver_define(driver))
    var = await register_usb_client(config)
    for device in devices:
        device_type = type_by_name[device[CONF_TYPE]]
        lanes = 1
        if isinstance(device_type, MpxType):
            lanes = min(len(device[CONF_CHANNELS]), max_init_lanes)
        cg.add(
            var.add_entry(
                device.get(CONF_VID, 0),
                device.get(CONF_PID, 0),
                UART_TYPES[device_type.cls_name],
                lanes,
                device.get(CONF_CLAIM_COMM_INTERFACE, True),
            )
        )
        for index, channel in enumerate(device[CONF_CHANNELS]):
            # The adapter it belongs to is its parent while it is plugged in
            chvar = cg.new_Pvariable(channel[CONF_ID], index, channel[CONF_BUFFER_SIZE])
            cg.add(chvar.set_stop_bits(channel[CONF_STOP_BITS]))
            cg.add(chvar.set_data_bits(channel[CONF_DATA_BITS]))
            cg.add(chvar.set_parity(channel[CONF_PARITY]))
            cg.add(chvar.set_baud_rate(channel[CONF_BAUD_RATE]))
            restore_line_settings(chvar, channel[CONF_ID])
            cg.add(chvar.set_dummy_receiver(channel[CONF_DUMMY_RECEIVER]))
            cg.add(chvar.set_claim_notification_ep(channel[CONF_CLAIM_NOTIFICATION_EP]))
            cg.add(chvar.set_flush_timeout(channel[CONF_FLUSH_TIMEOUT]))
            cg.add(chvar.set_debug(channel[CONF_DEBUG]))
            if channel[CONF_DEBUG_PREFIX]:
                cg.add(chvar.set_debug_prefix(channel[CONF_DEBUG_PREFIX]))
            cg.add(var.add_entry_channel(chvar))
            if channel[CONF_DEBUG]:
                cg.add_define("USE_UART_DEBUGGER")
                if channel[CONF_DEBUG_ADD_UART_SETTINGS]:
                    cg.add(chvar.set_debug_add_settings(True))
                    cg.add_define("UART_DEBUGGER_ADD_SETTINGS")
