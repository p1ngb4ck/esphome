import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    VARIANT_ESP32S31,
)
from esphome.components.network import get_network_priority, ip_address_literal
from esphome.components.usb_host import (
    CONF_PID,
    CONF_VID,
    USB_BLACKLIST_SCHEMA,
    USBClient,
    require_bulk_transfers,
    require_control_transfers,
    setup_usb_match,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_DNS1,
    CONF_DNS2,
    CONF_GATEWAY,
    CONF_ID,
    CONF_MANUAL_IP,
    CONF_STATIC_IP,
    CONF_SUBNET,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["usb_host", "network"]

usb_ethernet_ns = cg.esphome_ns.namespace("usb_ethernet")
USBEthernet = usb_ethernet_ns.class_("USBEthernet", USBClient)
ManualIP = usb_ethernet_ns.struct("ManualIP")

MANUAL_IP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_STATIC_IP): cv.ipv4address,
        cv.Required(CONF_GATEWAY): cv.ipv4address,
        cv.Required(CONF_SUBNET): cv.ipv4address,
        cv.Optional(CONF_DNS1, default="0.0.0.0"): cv.ipv4address,
        cv.Optional(CONF_DNS2, default="0.0.0.0"): cv.ipv4address,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBEthernet),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            **USB_BLACKLIST_SCHEMA,
            cv.Optional(CONF_MANUAL_IP): MANUAL_IP_SCHEMA,
        }
    ),
    esp32.only_on_variant(
        supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3, VARIANT_ESP32S31]
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    prio = get_network_priority("usb_ethernet")
    if prio is not None:
        cg.set_setup_priority(var, prio)
    await cg.register_component(var, config)
    await setup_usb_match(var, config)
    require_control_transfers()
    require_bulk_transfers()
    cg.add_define("USE_USB_ETHERNET")
    if CONF_MANUAL_IP in config:
        conf = config[CONF_MANUAL_IP]
        cg.add_define("USE_USB_ETHERNET_MANUAL_IP")
        cg.add(
            var.set_manual_ip(
                cg.StructInitializer(
                    ManualIP,
                    ("static_ip", ip_address_literal(conf[CONF_STATIC_IP])),
                    ("gateway", ip_address_literal(conf[CONF_GATEWAY])),
                    ("subnet", ip_address_literal(conf[CONF_SUBNET])),
                    ("dns1", ip_address_literal(conf[CONF_DNS1])),
                    ("dns2", ip_address_literal(conf[CONF_DNS2])),
                )
            )
        )
