import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3
from esphome.components.usb_host import (
    CONF_PID,
    CONF_VID,
    USBClient,
    require_bulk_transfers,
    require_control_transfers,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]

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
        }
    ),
    esp32.only_on_variant(
        supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)
    # HCI commands on endpoint 0, events on an interrupt IN endpoint, ACL data on bulk.
    require_control_transfers()
    require_bulk_transfers()
