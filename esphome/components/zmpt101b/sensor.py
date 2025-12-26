import esphome.codegen as cg
from esphome.components import sensor, voltage_sampler
import esphome.config_validation as cv
from esphome.const import (
    CONF_SENSOR,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_VOLT,
)

AUTO_LOAD = ["voltage_sampler"]
CODEOWNERS = ["@p1ngb4ck"]

CONF_FREQUENCY = "frequency"
CONF_SENSITIVITY = "sensitivity"
CONF_ZERO_POINT = "zero_point"
CONF_SAMPLES = "samples"
CONF_SAMPLE_DURATION = "sample_duration"

zmpt101b_ns = cg.esphome_ns.namespace("zmpt101b")
ZMPT101BSensor = zmpt101b_ns.class_(
    "ZMPT101BSensor", sensor.Sensor, cg.PollingComponent
)


def validate_samples(value):
    """Validate that samples is a reasonable number for RMS calculation."""
    if value < 20:
        raise cv.Invalid("samples must be at least 20 for accurate RMS calculation")
    if value > 1000:
        raise cv.Invalid("samples should not exceed 1000 to avoid excessive processing")
    return value


CONFIG_SCHEMA = (
    sensor.sensor_schema(
        ZMPT101BSensor,
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    )
    .extend(
        {
            cv.Required(CONF_SENSOR): cv.use_id(voltage_sampler.VoltageSampler),
            cv.Optional(CONF_FREQUENCY, default=50): cv.int_range(min=50, max=60),
            cv.Optional(CONF_SENSITIVITY): cv.float_range(min=0.1, max=100.0),
            cv.Optional(CONF_ZERO_POINT, default=2.5): cv.float_range(min=0.0, max=5.0),
            cv.Optional(CONF_SAMPLES, default=200): cv.All(
                cv.positive_int, validate_samples
            ),
            cv.Optional(
                CONF_SAMPLE_DURATION, default="40ms"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.polling_component_schema("60s"))
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)

    # Get the voltage sampler (ADS1115 sensor)
    sens = await cg.get_variable(config[CONF_SENSOR])
    cg.add(var.set_source(sens))

    # Configuration parameters
    cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    cg.add(var.set_zero_point(config[CONF_ZERO_POINT]))
    cg.add(var.set_samples(config[CONF_SAMPLES]))
    cg.add(var.set_sample_duration(config[CONF_SAMPLE_DURATION]))

    # Allow manual sensitivity override (otherwise auto-calculated)
    if CONF_SENSITIVITY in config:
        cg.add(var.set_sensitivity(config[CONF_SENSITIVITY]))
