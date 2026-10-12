import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    add_idf_sdkconfig_option,
    request_bluetooth,
)
from esphome.components.usb_host import (
    CONF_PID,
    CONF_VID,
    USBClient,
    get_max_packet_size,
    require_bulk_transfers,
    require_control_transfers,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import ID
import esphome.final_validate as fv

from . import realtek

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]

CONF_BLUEDROID = "bluedroid"
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
# Without vid/pid it takes any device with a Bluetooth controller interface (class E0). A
# dongle that hides that interface behind the vendor class (FF/01/01, as Broadcom's do) is
# named by vid/pid.
CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBBluetooth),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            # ESP-IDF's Bluedroid host stack, with the dongle as its controller.
            cv.Optional(CONF_BLUEDROID, default=False): cv.boolean,
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
    # Bluedroid takes one controller; esp32_ble would bring up another stack around a different one.
    if config[CONF_BLUEDROID] and "esp32_ble" in fv.full_config.get():
        raise cv.Invalid(
            f"{CONF_BLUEDROID}: Bluedroid takes the USB dongle as its controller; "
            "esp32_ble cannot be used beside it"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
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

    if config[CONF_BLUEDROID]:
        request_bluetooth()
        add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
        # Host only: the controller is the dongle.
        add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_DISABLED", True)
        add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
        # Espressif's vendor commands for coexistence, on by default with the controller
        # disabled; a USB dongle does not know them.
        add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ESP_COEX_VSC", False)
        cg.add_define("USE_USB_BLUETOOTH_BLUEDROID")
