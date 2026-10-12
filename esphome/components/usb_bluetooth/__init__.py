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
    require_bulk_transfers,
    require_control_transfers,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID
import esphome.final_validate as fv

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]

CONF_BLUEDROID = "bluedroid"

usb_bluetooth_ns = cg.esphome_ns.namespace("usb_bluetooth")
USBBluetooth = usb_bluetooth_ns.class_("USBBluetooth", USBClient)

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
        }
    ),
    esp32.only_on_variant(
        supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]
    ),
)


def _final_validate(config):
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
