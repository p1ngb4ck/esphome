import esphome.codegen as cg
from esphome.components import sensor, voltage_sampler
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_VOLTAGE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    ICON_CURRENT_AC,
    ICON_FLASH,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_WATT,
)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = []
MULTI_CONF = True

CONF_VOLTAGE_SENSOR = "voltage_sensor"
CONF_CURRENT_SENSOR = "current_sensor"
CONF_POWER_SENSOR = "power_sensor"
CONF_SENSITIVITY = "sensitivity"
CONF_ZERO_POINT = "zero_point"
CONF_LINE_VOLTAGE = "line_voltage"
CONF_SAMPLES = "samples"
CONF_SAMPLE_DURATION = "sample_duration"

acs712_ns = cg.esphome_ns.namespace("acs712")
ACS712Component = acs712_ns.class_("ACS712Component", cg.PollingComponent)

# Sensitivity values for different ACS712 models (in V/A)
ACS712Model = acs712_ns.enum("ACS712Model")
ACS712_MODELS = {
    "5A": ACS712Model.ACS712_5A,
    "20A": ACS712Model.ACS712_20A,
    "30A": ACS712Model.ACS712_30A,
}

CONF_MODEL = "model"


def validate_samples(value):
    """Validate that samples is a reasonable number for RMS calculation."""
    if value < 10:
        raise cv.Invalid("samples must be at least 10 for accurate RMS calculation")
    if value > 1000:
        raise cv.Invalid("samples should not exceed 1000 to avoid excessive processing")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ACS712Component),
        cv.Required(CONF_VOLTAGE_SENSOR): cv.use_id(voltage_sampler.VoltageSampler),
        cv.Optional(CONF_MODEL, default="20A"): cv.enum(ACS712_MODELS, upper=True),
        cv.Optional(CONF_SENSITIVITY): cv.float_range(min=0.001, max=1.0),
        cv.Optional(CONF_ZERO_POINT, default=2.5): cv.float_range(min=0.0, max=5.0),
        cv.Optional(CONF_LINE_VOLTAGE, default=230.0): cv.positive_float,
        cv.Optional(CONF_SAMPLES, default=100): cv.All(
            cv.positive_int, validate_samples
        ),
        cv.Optional(CONF_SAMPLE_DURATION, default=20): cv.int_range(min=10, max=100),
        cv.Optional(CONF_CURRENT_SENSOR): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_CURRENT_AC,
        ),
        cv.Optional(CONF_POWER_SENSOR): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_FLASH,
        ),
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement="V",
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
).extend(cv.polling_component_schema("60s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Get the voltage sampler (ADS1115 sensor)
    voltage_sensor = await cg.get_variable(config[CONF_VOLTAGE_SENSOR])
    cg.add(var.set_voltage_source(voltage_sensor))

    # Set model (which determines sensitivity)
    cg.add(var.set_model(config[CONF_MODEL]))

    # Allow manual sensitivity override
    if CONF_SENSITIVITY in config:
        cg.add(var.set_sensitivity(config[CONF_SENSITIVITY]))

    # Configuration parameters
    cg.add(var.set_zero_point(config[CONF_ZERO_POINT]))
    cg.add(var.set_line_voltage(config[CONF_LINE_VOLTAGE]))
    cg.add(var.set_samples(config[CONF_SAMPLES]))
    cg.add(var.set_sample_duration(config[CONF_SAMPLE_DURATION]))

    # Register optional sensors
    if CONF_CURRENT_SENSOR in config:
        current_sensor = await sensor.new_sensor(config[CONF_CURRENT_SENSOR])
        cg.add(var.set_current_sensor(current_sensor))

    if CONF_POWER_SENSOR in config:
        power_sensor = await sensor.new_sensor(config[CONF_POWER_SENSOR])
        cg.add(var.set_power_sensor(power_sensor))

    if CONF_VOLTAGE in config:
        voltage_out_sensor = await sensor.new_sensor(config[CONF_VOLTAGE])
        cg.add(var.set_voltage_sensor(voltage_out_sensor))
