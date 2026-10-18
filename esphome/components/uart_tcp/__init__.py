import esphome.codegen as cg
from esphome.components import noise, socket, uart
from esphome.components.const import (
    CONF_ALLOWED_IPS,
    CONF_HOST,
    CONF_RECONNECT_INTERVAL,
    CONF_ROLE,
)
from esphome.components.tcp_uart import DOMAIN as TCP_UART_DOMAIN, TcpUart
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENCRYPTION,
    CONF_ID,
    CONF_KEY,
    CONF_PORT,
    CONF_TIMEOUT,
    CONF_UART_ID,
)
from esphome.core import CORE, ID
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash
from esphome.types import ConfigType

CODEOWNERS = ["@Bascht74"]
DOMAIN = "uart_tcp"
DEPENDENCIES = ["network", "uart"]
AUTO_LOAD = noise.stream_auto_load(DOMAIN, ["socket"])
MULTI_CONF = True

CONF_UART_TCP_ID = "uart_tcp_id"

uart_tcp_ns = cg.esphome_ns.namespace("uart_tcp")
UartTcp = uart_tcp_ns.class_("UartTcp", cg.Component, uart.UARTDevice)
UartTcpForwards = uart_tcp_ns.class_("UartTcpForwards", cg.Component)

CONF_RUNNERS = "runners"
CONF_SETTINGS_FILE = "settings_file"
ROLE_FORWARDS = "forwards"
# Sockets are reserved for the most runners, so the count can change at runtime.
MAX_RUNNERS = 12
noise.register_set_key_action(DOMAIN, UartTcp)


BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UartTcp),
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_PORT): cv.port,
        cv.Optional(
            CONF_RECONNECT_INTERVAL, default="5s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TIMEOUT, default="0s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ENCRYPTION): noise.STREAM_ENCRYPTION_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


def _consume_sockets(config: ConfigType) -> ConfigType:
    # The role can change at runtime, so every entry holds a listen socket;
    # forwards hold one per runner at the most.
    count = MAX_RUNNERS if config[CONF_ROLE] == ROLE_FORWARDS else 1
    socket.consume_sockets(count, DOMAIN, socket.SocketType.TCP_LISTEN)(config)
    return socket.consume_sockets(count, DOMAIN)(config)


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            "client": BASE_SCHEMA.extend({cv.Required(CONF_HOST): socket.ipv4_host}),
            "server": BASE_SCHEMA.extend(
                {cv.Optional(CONF_ALLOWED_IPS): socket.IPV4_ALLOW_SCHEMA}
            ),
            # UARTs forwarded at runtime, from a settings file on a storage
            ROLE_FORWARDS: cv.Schema(
                {
                    cv.GenerateID(): cv.declare_id(UartTcpForwards),
                    cv.Optional(CONF_RUNNERS, default=4): cv.int_range(
                        min=2, max=MAX_RUNNERS
                    ),
                    cv.Optional(
                        CONF_SETTINGS_FILE, default="/sdcard/serial_ports.txt"
                    ): cv.All(cv.string, cv.Length(min=1, max=127)),
                }
            )
            .extend(cv.COMPONENT_SCHEMA)
            .add_extra(cv.requires_component("storage")),
        },
        key=CONF_ROLE,
        default_type="client",
        lower=True,
    ),
    _consume_sockets,
)


def _final_validate(config: ConfigType) -> ConfigType:
    # A second reader would split the bytes with this one, and every connect
    # discards what the other reader has not read yet.
    full_config = fv.full_config.get()
    data = full_config.data.setdefault(DOMAIN, {})
    uart.claim_exclusive(config, DOMAIN)

    if config[CONF_ROLE] == "server":
        # Two listeners on one port cannot both serve it. Only uart_tcp and
        # tcp_uart servers are compared here, not other listeners such as api.
        port = config[CONF_PORT]
        ports = data.setdefault(CONF_PORT, set())
        if port in ports or any(
            conf[CONF_ROLE] == "server" and conf[CONF_PORT] == port
            for conf in full_config.get(TCP_UART_DOMAIN, [])
        ):
            raise cv.Invalid(
                f"Port {port} is already the listen port of another uart_tcp "
                "server or of a tcp_uart server.",
                [CONF_PORT],
            )
        ports.add(port)
    return config


_FINAL_VALIDATE_LINK = cv.All(
    socket.final_validate_idle_timeout,
    noise.final_validate_reconnect_interval,
    noise.final_validate_stream_key(DOMAIN),
    _final_validate,
)


def _final_validate_entry(config: ConfigType) -> ConfigType:
    if config[CONF_ROLE] != ROLE_FORWARDS:
        return _FINAL_VALIDATE_LINK(config)
    data = fv.full_config.get().data.setdefault(DOMAIN, {})
    if data.get(ROLE_FORWARDS):
        raise cv.Invalid(
            "Only one uart_tcp entry can have the role forwards.", [CONF_ROLE]
        )
    data[ROLE_FORWARDS] = True
    return config


FINAL_VALIDATE_SCHEMA = _final_validate_entry


def _declared_ids(node: object):
    if isinstance(node, ID):
        if node.is_declaration:
            yield node
    elif isinstance(node, dict):
        for value in node.values():
            yield from _declared_ids(value)
    elif isinstance(node, list):
        for item in node:
            yield from _declared_ids(item)


def _forwardable_uarts() -> list[ID]:
    """Every UART the yaml declares that nothing else reads: hardware UARTs and each
    usb_uart channel, not the log UART, not tcp_uart (already a TCP link)."""
    full_config = CORE.config
    found = []
    for id_ in _declared_ids(full_config):
        if (
            id_.type is None
            or not id_.type.inherits_from(uart.UARTComponent)
            or id_.type.inherits_from(uart.VirtualUARTComponent)
            or id_.type.inherits_from(TcpUart)
        ):
            continue
        uart_id = str(id_)
        if any(
            uart.subtree_references_uart(conf, uart_id)
            for conf in full_config.values()
        ):
            continue
        found.append(id_)
    return found


async def _forwards_to_code(config: ConfigType) -> None:
    from esphome.components.storage import request_storage_worker

    # In PSRAM, as its runners
    create = cg.MockObj(str(UartTcpForwards), "::").create()
    var = cg.Pvariable(config[CONF_ID], create)
    await cg.register_component(var, config)
    cg.add_define("USE_UART_TCP_FORWARDS")
    socket.require_tcp_listener()
    cg.add_define("USE_SOCKET_IPV4_ALLOW")
    request_storage_worker()
    cg.add(var.set_runners_default(config[CONF_RUNNERS]))
    cg.add(var.set_settings_file_default(config[CONF_SETTINGS_FILE]))
    settings_key = fnv1_hash(f"uart_tcp_forwards:{config[CONF_ID].id}")
    cg.add(var.set_settings_key(settings_key))
    uarts = _forwardable_uarts()
    cg.add(var.init_uarts(len(uarts)))
    for uart_id in uarts:
        uart_var = await cg.get_variable(uart_id)
        cg.add(var.add_uart(uart_var, uart_id.id))


async def to_code(config: ConfigType) -> None:
    if config[CONF_ROLE] == ROLE_FORWARDS:
        await _forwards_to_code(config)
        return
    # In PSRAM: the object holds the link's TX buffer
    create = cg.MockObj(str(UartTcp), "::").create()
    var = cg.Pvariable(config[CONF_ID], create)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    uart_id, uart_var = await cg.get_variable_with_full_id(config[CONF_UART_ID])
    if uart_id.type.inherits_from(uart.VirtualUARTComponent):
        cg.add(var.set_virtual_uart(uart_var))
    # Both roles and the allow list are compiled; the runtime settings switch them.
    socket.require_tcp_listener()
    cg.add_define("USE_SOCKET_IPV4_ALLOW")
    if config[CONF_ROLE] == "server":
        cg.add(var.set_server(True))
        socket.add_ipv4_allow(
            var.set_allow, config.get(CONF_ALLOWED_IPS), config[CONF_ID]
        )
    settings_key = fnv1_hash(f"uart_tcp_settings:{config[CONF_ID].id}")
    cg.add(var.set_settings_key(settings_key))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_reconnect_interval(config[CONF_RECONNECT_INTERVAL]))
    if (timeout := config[CONF_TIMEOUT]).total_milliseconds > 0:
        cg.add(var.set_timeout(timeout))
    if (host := config.get(CONF_HOST)) is not None:
        cg.add(var.set_host(host))
    if (encryption := config.get(CONF_ENCRYPTION)) is not None:
        # The TCP client starts the handshake
        stream = noise.new_stream(
            config[CONF_ID], encryption.get(CONF_KEY), config[CONF_ROLE] == "client"
        )
        cg.add(var.set_noise_stream(stream))
