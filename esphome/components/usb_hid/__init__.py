from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, text_sensor
from esphome.components.usb_host import USBClient
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]
AUTO_LOAD = ["usb_host"]
# One instance per device: a keyboard and the HID interface of a sound card are two
# separate clients, each attached to its own device by vid and pid.
MULTI_CONF = True

SUPPORTED_VARIANTS = [
    esp32.const.VARIANT_ESP32S2,
    esp32.const.VARIANT_ESP32S3,
    esp32.const.VARIANT_ESP32P4,
]

CONF_USB_HID_ID = "usb_hid_id"
CONF_KEYBOARD = "keyboard"
CONF_MOUSE = "mouse"
CONF_GAMEPAD = "gamepad"
CONF_LAYOUT = "layout"
CONF_DEVICE_NAME = "device_name"

CONF_VID = "vid"
CONF_PID = "pid"
CONF_RAW = "raw"
CONF_LOG_REPORTS = "log_reports"
CONF_LONG_PRESS_TIME = "long_press_time"
CONF_ON_REPORT = "on_report"
CONF_BUTTONS = "buttons"
CONF_BYTE = "byte"
CONF_MASK = "mask"
CONF_ON_SHORT_PRESS = "on_short_press"
CONF_ON_LONG_PRESS = "on_long_press"

usb_hid_ns = cg.esphome_ns.namespace("usb_hid")
USBHIDClient = usb_hid_ns.class_("USBHIDClient", USBClient, cg.Component)
RawHIDDriver = usb_hid_ns.class_("RawHIDDriver")
HIDRawButton = usb_hid_ns.struct("HIDRawButton")

ReportTrigger = automation.Trigger.template(cg.std_vector.template(cg.uint8))
PressTrigger = automation.Trigger.template()

# One button, named by where its bit sits in the interrupt IN report. Turn on log_reports,
# press each button once, and read byte and mask off the log.
BUTTON_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HIDRawButton),
        cv.Required(CONF_BYTE): cv.uint8_t,
        cv.Required(CONF_MASK): cv.hex_uint8_t,
        cv.Optional(CONF_ON_SHORT_PRESS): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PressTrigger)}
        ),
        cv.Optional(CONF_ON_LONG_PRESS): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PressTrigger)}
        ),
    }
)


def _validate_button(config):
    if config[CONF_MASK] == 0:
        raise cv.Invalid("mask selects the bit of the button and cannot be 0")
    if CONF_ON_SHORT_PRESS not in config and CONF_ON_LONG_PRESS not in config:
        raise cv.Invalid(
            "a button needs at least one of on_short_press or on_long_press, "
            "otherwise nothing happens when it is pressed"
        )
    return config


RAW_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RawHIDDriver),
        # Logs each distinct report as hex at INFO. Repeats are suppressed, so holding a
        # button on a device that repeats does not bury the transitions.
        cv.Optional(CONF_LOG_REPORTS, default=False): cv.boolean,
        # Short against long is decided when the button is released, because until then it
        # is not known which of the two it will be.
        cv.Optional(
            CONF_LONG_PRESS_TIME, default="600ms"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ON_REPORT): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReportTrigger)}
        ),
        cv.Optional(CONF_BUTTONS): cv.ensure_list(
            cv.All(BUTTON_SCHEMA, _validate_button)
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBHIDClient),
            # Which device this instance attaches to. 0 matches anything, which is fine for
            # a single HID device on the bus but not once a keyboard and a sound card are
            # both attached -- give each instance the vid and pid of its own device then.
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_KEYBOARD): cv.Schema(
                {
                    cv.Optional(CONF_LAYOUT, default="us"): cv.one_of(
                        "us", "uk", "de", "fr", "es", lower=True
                    ),
                }
            ),
            cv.Optional(CONF_MOUSE): cv.Schema({}),
            cv.Optional(CONF_GAMEPAD): cv.Schema({}),
            cv.Optional(CONF_RAW): RAW_SCHEMA,
            cv.Optional(CONF_DEVICE_NAME): text_sensor.text_sensor_schema(),
        }
    ),
    esp32.only_on_variant(supported=SUPPORTED_VARIANTS),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_VID], config[CONF_PID])
    await cg.register_component(var, config)

    if CONF_KEYBOARD in config:
        cg.add_build_flag("-DUSB_HID_ENABLE_KEYBOARD")
        layout = config[CONF_KEYBOARD].get(CONF_LAYOUT, "us")
        layout_map = {
            "us": "KEYBOARD_LAYOUT_US",
            "uk": "KEYBOARD_LAYOUT_UK",
            "de": "KEYBOARD_LAYOUT_DE",
            "fr": "KEYBOARD_LAYOUT_FR",
            "es": "KEYBOARD_LAYOUT_ES",
        }
        cg.add_define(layout_map[layout])

    if CONF_MOUSE in config:
        cg.add_build_flag("-DUSB_HID_ENABLE_MOUSE")

    if CONF_GAMEPAD in config:
        cg.add_build_flag("-DUSB_HID_ENABLE_GAMEPAD")

    if (raw := config.get(CONF_RAW)) is not None:
        # The build flag makes register_all_drivers() construct the driver and hand it to
        # the client, so it is fetched back from there rather than created a second time.
        cg.add_build_flag("-DUSB_HID_ENABLE_RAW")
        driver = cg.Pvariable(raw[CONF_ID], var.get_raw_driver(), RawHIDDriver)
        cg.add(driver.set_log_reports(raw[CONF_LOG_REPORTS]))
        cg.add(driver.set_long_press_time(raw[CONF_LONG_PRESS_TIME]))

        for conf in raw.get(CONF_ON_REPORT, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
            cg.add(driver.set_on_report(trigger))
            await automation.build_automation(
                trigger, [(cg.std_vector.template(cg.uint8), "x")], conf
            )

        for conf in raw.get(CONF_BUTTONS, []):
            button = cg.new_Pvariable(conf[CONF_ID])
            cg.add(button.set_byte_index(conf[CONF_BYTE]))
            cg.add(button.set_mask(conf[CONF_MASK]))
            for key, setter in (
                (CONF_ON_SHORT_PRESS, button.set_on_short_press),
                (CONF_ON_LONG_PRESS, button.set_on_long_press),
            ):
                for press_conf in conf.get(key, []):
                    trigger = cg.new_Pvariable(press_conf[CONF_TRIGGER_ID])
                    cg.add(setter(trigger))
                    await automation.build_automation(trigger, [], press_conf)
            cg.add(driver.add_button(button))

    if (device_name_cfg := config.get(CONF_DEVICE_NAME)) is not None:
        sens = await text_sensor.new_text_sensor(device_name_cfg)
        cg.add(var.register_device_name_sensor(sens))

    # Interrupt transfers share the bulk transfer path in ESP-IDF
    cg.add_define("USE_USB_BULK_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")
