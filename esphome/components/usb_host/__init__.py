from pathlib import Path

import esphome.codegen as cg
from esphome.components.esp32 import (
    VARIANT_ESP32H4,
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
    add_idf_component,
    add_idf_sdkconfig_option,
    get_esp32_variant,
    idf_version,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_DEVICES, CONF_ID
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.cpp_generator import MockObj
from esphome.cpp_types import Component
from esphome.types import ConfigType

AUTO_LOAD = ["bytebuffer"]
CODEOWNERS = ["@clydebarrow"]
DEPENDENCIES = ["esp32"]
usb_host_ns = cg.esphome_ns.namespace("usb_host")
USBHost = usb_host_ns.class_("USBHost", Component)
USBClient = usb_host_ns.class_("USBClient", Component)
DOMAIN = "usb_host"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_ENABLE_HUBS = "enable_hubs"
CONF_MAX_TRANSFER_REQUESTS = "max_transfer_requests"
CONF_MAX_PACKET_SIZE = "max_packet_size"
CONF_DUAL_HOST = "dual_host"
CONF_FS_PINS = "fs_pins"

# GPIO pair the ESP32-P4 full-speed OTG controller is wired to on the board. The chip has
# two internal FSLS PHYs: PHY 0 on GPIO24 (D-) / GPIO25 (D+), PHY 1 on GPIO26 / GPIO27.
# Out of reset OTG_FS gets PHY 1 and USB-Serial-JTAG gets PHY 0; selecting "24_25" swaps
# them, which also moves USB-Serial-JTAG to GPIO26/27.
FS_PIN_CHOICES = {"26_27": 1, "24_25": 0}

# Transfer-class requirement tracking. Consumer components call the require_*()
# functions below; the FINAL-priority job turns the union into defines.
KEY_TRANSFERS_REQUIRED = "transfers_required"
TRANSFER_BULK = "bulk"
TRANSFER_CONTROL = "control"
TRANSFER_ISOC = "isoc"
_TRANSFER_DEFINES = {
    TRANSFER_BULK: "USE_USB_BULK_TRANSFERS",
    TRANSFER_CONTROL: "USE_USB_CONTROL_TRANSFERS",
    TRANSFER_ISOC: "USE_USB_ISOC_TRANSFERS",
}


def usb_device_schema(
    cls=USBClient, vid: int | None = None, pid: int | None = None
) -> cv.Schema:
    schema = cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(cls),
        }
    )
    if vid:
        schema = schema.extend({cv.Optional(CONF_VID, default=vid): cv.hex_uint16_t})
    else:
        schema = schema.extend({cv.Required(CONF_VID): cv.hex_uint16_t})
    if pid:
        schema = schema.extend({cv.Optional(CONF_PID, default=pid): cv.hex_uint16_t})
    else:
        schema = schema.extend({cv.Required(CONF_PID): cv.hex_uint16_t})
    return schema


def _store_host_options(config: dict) -> dict:
    """Publish the host-wide sizing options so consumer components can read them
    from their own validation and codegen."""
    domain_data = CORE.data.setdefault(DOMAIN, {})
    domain_data[CONF_MAX_PACKET_SIZE] = config[CONF_MAX_PACKET_SIZE]
    domain_data[CONF_MAX_TRANSFER_REQUESTS] = config[CONF_MAX_TRANSFER_REQUESTS]
    domain_data[CONF_DUAL_HOST] = config[CONF_DUAL_HOST]
    return config


def _require_transfers(*kinds: str) -> None:
    """Record transfer classes a consumer needs.

    Recording rather than emitting keeps this callable from any consumer at any
    codegen priority, and lets the reconcile job below see the union of every
    request instead of whatever the first caller happened to ask for.
    """
    required = CORE.data.setdefault(DOMAIN, {}).setdefault(
        KEY_TRANSFERS_REQUIRED, set()
    )
    required.update(kinds)


def require_bulk_transfers() -> None:
    """Request the bulk/interrupt transfer API in USBClient.

    A consumer component calls this from its own to_code(). The transfer paths are
    compiled per request so a build only carries the ones some driver actually uses;
    isochronous in particular is dead weight for a serial adapter.
    """
    _require_transfers(TRANSFER_BULK)


def require_control_transfers() -> None:
    """Request the control transfer API, including set_interface()."""
    _require_transfers(TRANSFER_CONTROL)


def require_isoc_transfers() -> None:
    """Request the isochronous stream API.

    Selecting an alt-setting is a control transfer, so isochronous cannot stand on
    its own. The implication is resolved in the reconcile job rather than here, so
    a consumer only has to state what it actually uses.
    """
    _require_transfers(TRANSFER_ISOC)


@coroutine_with_priority(CoroPriority.FINAL)
async def _emit_transfer_defines() -> None:
    """Emit the transfer-class defines once, after every require_*() call.

    Consumer to_code() runs at a higher priority than FINAL, so by the time this
    job runs every request has been recorded. Reading the set inline from
    usb_host's own to_code() instead would depend on component iteration order and
    would silently drop the requests of any consumer that had not run yet.
    """
    required = set(CORE.data.get(DOMAIN, {}).get(KEY_TRANSFERS_REQUIRED, ()))
    if TRANSFER_ISOC in required:
        # Alt-setting selection is a control transfer, so isochronous cannot stand
        # alone; usb_host.h enforces the same dependency with an #error for anyone
        # defining the macros by hand.
        required.add(TRANSFER_CONTROL)
    for kind in sorted(required):
        cg.add_define(_TRANSFER_DEFINES[kind])


# Per-port hardware FIFO biasing. The DWC controller divides one FIFO between received
# data, non-periodic OUT and periodic OUT, and which division a port uses caps the largest
# packet each of its directions can carry. ESP-IDF exposes that choice as a single Kconfig
# option reaching every port, which cannot serve a chip with two controllers: a full-speed
# port carrying an isochronous OUT stream wants the periodic-out division, and that same
# division on a high-speed port leaves 256 bytes of non-periodic OUT, under the 512 byte
# wMaxPacketSize every high-speed bulk OUT endpoint has, so every bulk pipe is rejected.
#
# idf_usb_patch.py adds a per-port bias to the espressif/usb component and the component is
# built from a patched copy, so each port takes the division it needs and the other port is
# untouched. Consumers record what they need through set_port_fifo_bias(); the FINAL job
# below emits it once every consumer has run.
KEY_PORT_FIFO_BIAS = "port_fifo_bias"

FIFO_BIAS_DEFAULT = "default"
FIFO_BIAS_BALANCED = "balanced"
FIFO_BIAS_IN = "in"
FIFO_BIAS_PERIODIC_OUT = "periodic_out"

# Numeric values of usb_host_fifo_bias_t as the patch defines it.
_FIFO_BIAS_VALUES = {
    FIFO_BIAS_DEFAULT: 0,
    FIFO_BIAS_BALANCED: 1,
    FIFO_BIAS_IN: 2,
    FIFO_BIAS_PERIODIC_OUT: 3,
}


def per_port_fifo_bias_available() -> bool:
    """Whether the build can bias a single port rather than all of them.

    The patched component replaces the one pulled from the registry, and that only happens
    where the registry component is used at all: from IDF 6.0 the USB host left the IDF
    tree. On an older IDF the host is an IDF built-in and the Kconfig option is the only
    lever there is.
    """
    return idf_version() >= cv.Version(6, 0, 0)


def fs_peripheral_index() -> int | None:
    """peripheral_map index of the full-speed host controller, or None if none is running.

    Per USB_DWC_LL_GET_HW() the ESP32-P4 has the high-speed controller at index 0 and the
    full-speed one at index 1, and the latter is only brought up under dual host. On the
    variants whose full-speed controller is the only controller it is index 0.
    """
    variant = get_esp32_variant()
    if variant == VARIANT_ESP32P4:
        return 1 if dual_host_enabled() else None
    if variant in (VARIANT_ESP32S2, VARIANT_ESP32S3):
        return 0
    return None


def set_port_fifo_bias(peripheral_index: int, bias: str) -> None:
    """Ask for a FIFO division on one host controller.

    Recorded rather than emitted so this is callable from any consumer at any codegen
    priority. Two consumers asking for different divisions on the same port is a real
    conflict rather than something to silently resolve, so it is raised.
    """
    if bias not in _FIFO_BIAS_VALUES:
        raise ValueError(f"Unknown FIFO bias {bias}")
    biases = CORE.data.setdefault(DOMAIN, {}).setdefault(KEY_PORT_FIFO_BIAS, {})
    previous = biases.get(peripheral_index, FIFO_BIAS_DEFAULT)
    if previous not in (FIFO_BIAS_DEFAULT, bias):
        raise cv.Invalid(
            f"Two components need different USB FIFO divisions on host controller "
            f"{peripheral_index}: {previous} and {bias}. They cannot share that "
            f"controller; move one of the devices to the other controller."
        )
    biases[peripheral_index] = bias


@coroutine_with_priority(CoroPriority.FINAL)
async def _emit_port_fifo_bias(var: MockObj) -> None:
    """Emit the recorded per-port biases, after every set_port_fifo_bias() call."""
    biases = CORE.data.get(DOMAIN, {}).get(KEY_PORT_FIFO_BIAS, {})
    for index, bias in sorted(biases.items()):
        if bias == FIFO_BIAS_DEFAULT:
            continue
        cg.add(var.set_port_fifo_bias(index, _FIFO_BIAS_VALUES[bias]))


def get_max_packet_size() -> int:
    return CORE.data.get(DOMAIN, {}).get(CONF_MAX_PACKET_SIZE, 64)


def get_max_transfer_requests() -> int:
    return CORE.data.get(DOMAIN, {}).get(CONF_MAX_TRANSFER_REQUESTS, 16)


def dual_host_enabled() -> bool:
    """Whether both USB controllers are brought up.

    Matters to consumers because the two controllers are not equivalent: with dual host a
    device can land on the full-speed one, whose FIFO and packet limits are those of a
    full-speed controller no matter how capable the variant is overall.
    """
    return bool(CORE.data.get(DOMAIN, {}).get(CONF_DUAL_HOST, False))


def _default_max_packet_size() -> int:
    """Largest bulk/interrupt packet the controller in this variant moves at once.

    USB_HOST_MAX_PACKET_SIZE sizes every transfer buffer the client pool allocates, and a
    request larger than one buffer is rejected. The ESP32-P4's high-speed controller uses
    512 byte bulk packets, so a flat 64 leaves mass storage transfers unable to carry a
    sector. Full-speed variants stay at 64.
    """
    return 512 if get_esp32_variant() == VARIANT_ESP32P4 else 64


def _dual_host_validator(value):
    """dual_host requires ESP32-P4 and IDF >= 6.0 (needs espressif/usb 1.4.1)."""
    value = cv.boolean(value)
    if value:
        if get_esp32_variant() != VARIANT_ESP32P4:
            raise cv.Invalid("dual_host is only supported on ESP32-P4")
        if idf_version() < cv.Version(6, 0, 0):
            raise cv.Invalid("dual_host requires IDF >= 6.0.0")
    return value


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBHost),
            cv.Optional(CONF_ENABLE_HUBS, default=False): cv.boolean,
            cv.Optional(CONF_MAX_TRANSFER_REQUESTS, default=16): cv.int_range(
                min=1, max=32
            ),
            cv.Optional(
                CONF_MAX_PACKET_SIZE, default=_default_max_packet_size
            ): cv.one_of(64, 128, 256, 512, 1024, int=True),
            cv.Optional(CONF_DUAL_HOST, default=False): _dual_host_validator,
            cv.Optional(CONF_FS_PINS, default="26_27"): cv.enum(FS_PIN_CHOICES),
            cv.Optional(CONF_DEVICES): cv.ensure_list(usb_device_schema()),
        }
    ),
    only_on_variant(
        supported=[
            VARIANT_ESP32H4,
            VARIANT_ESP32P4,
            VARIANT_ESP32S2,
            VARIANT_ESP32S3,
            VARIANT_ESP32S31,
        ]
    ),
    _store_host_options,
)


_USB_OVERRIDE_MARKER = ".esphome_usb_patch"


def _sync_usb_component_override() -> str:
    """Build a patched copy of the espressif/usb component and return its path.

    ESP-IDF's component manager resolves a dependency to a local directory when the manifest
    gives it an override_path, so a patched copy replaces the registry one with zero cmake
    and nothing outside this build directory touched. The pristine source is the published
    release archive, fetched through ESPHome's own URL cache, so the copy is byte-for-byte
    the version the manifest pins and no git clone is involved.

    Synced on every codegen and stamped with the component version and a digest of the
    patches, so a copy left from an earlier revision is replaced rather than trusted. There
    is no removal path, unlike the exFAT FatFs override: that one lives in the project
    components directory, which ESP-IDF scans on its own, so a copy left behind after the
    option is turned off would keep being compiled. This one sits outside every component
    search path -- the generated CMakeLists only adds src/ -- and is reachable solely through
    the override_path this function's caller writes into the manifest, so a copy nobody
    points at is inert.
    """
    import io
    import shutil
    import zipfile

    from esphome.external_files import compute_local_file_path, download_content

    from . import idf_usb_patch as patch

    dest = Path(CORE.build_path) / "components-src" / "espressif__usb"
    marker = dest / _USB_OVERRIDE_MARKER
    if marker.is_file() and marker.read_text() == patch.PATCH_STAMP:
        return str(dest)

    archive = download_content(
        patch.USB_COMPONENT_URL,
        compute_local_file_path(DOMAIN, patch.USB_COMPONENT_URL),
    )
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as zf:
        zf.extractall(dest)

    for rel, old, new in patch.PATCHES:
        target = dest / rel
        if not target.is_file():
            raise cv.Invalid(
                f"usb_host: {rel} is missing from espressif/usb "
                f"{patch.USB_COMPONENT_VERSION}; the per-port FIFO patch does not fit "
                f"this version"
            )
        text = target.read_text()
        if text.count(old) != 1:
            raise cv.Invalid(
                f"usb_host: the per-port FIFO patch no longer applies to {rel} in "
                f"espressif/usb {patch.USB_COMPONENT_VERSION} -- the code it anchors on "
                f"is missing or appears more than once"
            )
        target.write_text(text.replace(old, new, 1))

    marker.write_text(patch.PATCH_STAMP)
    return str(dest)


async def register_usb_client(config: ConfigType) -> MockObj:
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
    return var


async def to_code(config: ConfigType) -> None:
    # IDF 6.0 moved USB host to an external component; 1.4.1 requires IDF >= 6.0.
    # It is built from a patched copy so each host controller can take its own FIFO
    # division -- see idf_usb_patch.py for why one division cannot serve two controllers.
    if per_port_fifo_bias_available():
        from . import idf_usb_patch as idf_usb_patch_mod

        add_idf_component(
            name="espressif/usb",
            ref=idf_usb_patch_mod.USB_COMPONENT_VERSION,
            override_path=_sync_usb_component_override(),
        )
        cg.add_define("USE_USB_HOST_PER_PORT_FIFO_BIAS")

    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
    if config.get(CONF_ENABLE_HUBS):
        add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)

    cg.add_define("USB_HOST_MAX_REQUESTS", config[CONF_MAX_TRANSFER_REQUESTS])
    cg.add_define("USB_HOST_MAX_PACKET_SIZE", config[CONF_MAX_PACKET_SIZE])

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if config.get(CONF_DUAL_HOST):
        cg.add(var.set_dual_host(True))
    cg.add(var.set_fs_phy_index(config[CONF_FS_PINS]))

    devices = config.get(CONF_DEVICES)
    if devices:
        # A bare devices: entry has no driver component behind it to request transfer
        # classes, so it is only useful through lambdas. Give it the standard pair rather
        # than a USBClient that cannot transfer anything at all.
        require_bulk_transfers()
        require_control_transfers()

    # FINAL: require_*() calls arrive from consumer to_code() at higher priorities, so
    # turn the collected set into defines once after every job ran. Emitting per call
    # instead would make the result depend on component iteration order.
    CORE.add_job(_emit_transfer_defines)

    # FINAL for the same reason: a consumer's set_port_fifo_bias() runs at its own
    # priority, and reading the record here would depend on component iteration order.
    if per_port_fifo_bias_available():
        CORE.add_job(_emit_port_fifo_bias, var)

    for device in devices or ():
        await register_usb_client(device)
