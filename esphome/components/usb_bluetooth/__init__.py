import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import (
    CONF_SDKCONFIG_OPTIONS,
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_sdkconfig_option,
    request_bluetooth,
)
from esphome.components.usb_host import (
    USB_BLACKLIST_SCHEMA,
    setup_usb_match,
    CONF_PID,
    CONF_VID,
    USBClient,
    get_max_packet_size,
    require_bulk_transfers,
    require_control_transfers,
)
import esphome.config_validation as cv
from esphome.const import CONF_FRAMEWORK, CONF_ID, PLATFORM_ESP32
from esphome.core import CORE, ID
import esphome.final_validate as fv

from . import realtek

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["usb_host"]
MULTI_CONF = True

CONF_BLUEDROID = "bluedroid"
CONF_C6_OFFLOAD = "c6_offload"
CONF_FIRMWARE = "firmware"
CONF_FIRMWARE_CONFIG = "firmware_config"
# An HCI command is up to 3 + 255 bytes, sent as one control transfer.
MIN_TRANSFER_BUFFER = 256

usb_bluetooth_ns = cg.esphome_ns.namespace("usb_bluetooth")
USBBluetooth = usb_bluetooth_ns.class_("USBBluetooth", USBClient)


def _validate_firmware(config):
    if CONF_FIRMWARE_CONFIG in config and CONF_FIRMWARE not in config:
        raise cv.Invalid(f"{CONF_FIRMWARE_CONFIG} needs {CONF_FIRMWARE}")
    if CONF_FIRMWARE in config:
        fw = config[CONF_FIRMWARE].read_bytes()
        cfg = (
            config[CONF_FIRMWARE_CONFIG].read_bytes()
            if CONF_FIRMWARE_CONFIG in config
            else b""
        )
        try:
            realtek.images(fw, cfg)
        except realtek.NotFirmware as err:
            raise cv.Invalid(f"{CONF_FIRMWARE}: {err}") from err
    return config


# A USB Bluetooth dongle (HCI over USB) as a usb_host client, next to every other client.
# Routed by class E0/01/01, and Broadcom/Apple controllers behind the vendor class FF/01/01.
# vid/pid name one dongle instead, whatever its class.
CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBBluetooth),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            **USB_BLACKLIST_SCHEMA,
            # ESP-IDF's Bluedroid host stack, with the dongle as its controller.
            cv.Optional(CONF_BLUEDROID, default=False): cv.boolean,
            cv.Optional(CONF_C6_OFFLOAD, default=False): cv.boolean,
            # Realtek firmware (linux-firmware rtl_bt/<chip>_fw.bin and, where the chip has one,
            # <chip>_config.bin), downloaded into a Realtek controller still on its ROM.
            cv.Optional(CONF_FIRMWARE): cv.file_,
            cv.Optional(CONF_FIRMWARE_CONFIG): cv.file_,
        }
    ),
    _validate_firmware,
    esp32.only_on_variant(
        supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]
    ),
)


def _final_validate(config):
    if get_max_packet_size() < MIN_TRANSFER_BUFFER:
        raise cv.Invalid(
            f"usb_bluetooth sends HCI commands of up to 258 bytes in one control transfer: set "
            f"usb_host max_packet_size to {MIN_TRANSFER_BUFFER} or more"
        )
    full = fv.full_config.get()
    if config[CONF_C6_OFFLOAD]:
        if config[CONF_BLUEDROID]:
            raise cv.Invalid(f"{CONF_C6_OFFLOAD} and {CONF_BLUEDROID} are exclusive: one host stack per dongle")
        if "esp32_hosted" not in full:
            raise cv.Invalid(f"{CONF_C6_OFFLOAD} needs esp32_hosted (the C6 runs the host stack)")
        if "esp32_ble" in full:
            raise cv.Invalid(
                f"{CONF_C6_OFFLOAD}: the C6's offload firmware runs no controller for esp32_ble; "
                "Bluetooth LE then also goes through the dongle on the C6"
            )
    # Bluedroid is one stack with one controller.
    instances = full.get("usb_bluetooth") or []
    if config[CONF_BLUEDROID] and sum(1 for c in instances if c[CONF_BLUEDROID]) > 1:
        raise cv.Invalid(f"{CONF_BLUEDROID}: only one usb_bluetooth can run Bluedroid")
    if config[CONF_BLUEDROID] and _splits_with_esp32_ble(fv.full_config.get()):
        full = fv.full_config.get()
        if "esp32_hosted" not in full:
            raise cv.Invalid(
                f"{CONF_BLUEDROID} beside esp32_ble shares Bluedroid with the co-processor's "
                "controller (esp32_hosted), which this configuration does not have"
            )
        # Bluedroid reads the controllers once, when esp32_ble brings it up: it runs on the
        # co-processor alone until the dongle is ready, then usb_bluetooth starts it again on both
        # (with enable_on_boot: false, only then).


def _splits_with_esp32_ble(full_config) -> bool:
    """Bluedroid on both controllers: LE on the co-processor, BR/EDR on the dongle."""
    return "esp32_ble" in full_config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
    await setup_usb_match(var, config)
    # HCI commands on endpoint 0, events on an interrupt IN endpoint, ACL data on bulk.
    require_control_transfers()
    require_bulk_transfers()

    if CONF_FIRMWARE in config:
        fw = config[CONF_FIRMWARE].read_bytes()
        cfg = (
            config[CONF_FIRMWARE_CONFIG].read_bytes()
            if CONF_FIRMWARE_CONFIG in config
            else b""
        )
        images, lmp_subver = realtek.images(fw, cfg)
        for rom, data in sorted(images.items()):
            arr = cg.progmem_array(
                ID(
                    f"{config[CONF_ID].id}_rtl_rom{rom}",
                    is_declaration=True,
                    type=cg.uint8,
                ),
                list(data),
            )
            cg.add(var.add_realtek_firmware(lmp_subver, rom, arr, len(data)))

    if config[CONF_BLUEDROID] or config[CONF_C6_OFFLOAD]:
        cg.add_define("USE_USB_BLUETOOTH_H4")
    if config[CONF_C6_OFFLOAD]:
        cg.add_define("USE_USB_BLUETOOTH_C6_OFFLOAD")
    if config[CONF_BLUEDROID]:
        request_bluetooth()
        add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
        # Host only: the controllers are the dongle (and the co-processor).
        add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_DISABLED", True)
        # After esp32_ble's final validation, which turns it off for esp_hosted.
        add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
        cg.add_define("USE_USB_BLUETOOTH_BLUEDROID")
        # Bluedroid traces every ACL fragment it cannot place as an HCI error, many a second on
        # a busy link; compiled out, nothing of it is formatted or logged. A level set in
        # sdkconfig_options wins.
        user_options = CORE.config[PLATFORM_ESP32][CONF_FRAMEWORK].get(
            CONF_SDKCONFIG_OPTIONS, {}
        )
        if not any(
            name.startswith("CONFIG_BT_LOG_HCI_TRACE_LEVEL") for name in user_options
        ):
            add_idf_sdkconfig_option("CONFIG_BT_LOG_HCI_TRACE_LEVEL_NONE", True)
        if _splits_with_esp32_ble(CORE.config):
            cg.add_define("USE_USB_BLUETOOTH_HCI_SPLIT")
            cg.add_build_flag("-Wl,--wrap=esp_bluedroid_attach_hci_driver")
        else:
            # Espressif's vendor commands for coexistence, on by default with the controller
            # disabled; a USB dongle does not know them.
            add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ESP_COEX_VSC", False)
