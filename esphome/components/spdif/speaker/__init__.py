from esphome import pins
import esphome.codegen as cg
from esphome.components import audio, speaker
from esphome.components.esp32 import add_idf_component
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_PIN,
    CONF_SAMPLE_RATE,
)

from .. import spdif_ns

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["audio", "ring_buffer"]

CONF_TASK_CORE = "task_core"

SpdifSpeaker = spdif_ns.class_("SpdifSpeaker", speaker.Speaker, cg.Component)

# IEC 60958 defines the consumer frame rates, and the channel status word has a code for
# each of them. Anything else has no code to send, so a receiver would be told the rate is
# unknown and would have to guess it from the transitions.
SAMPLE_RATES = [32000, 44100, 48000]


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=2,
        max_channels=2,
        min_sample_rate=config[CONF_SAMPLE_RATE],
        max_sample_rate=config[CONF_SAMPLE_RATE],
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(SpdifSpeaker),
                cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
                cv.Optional(CONF_SAMPLE_RATE, default=48000): cv.one_of(
                    *SAMPLE_RATES, int=True
                ),
                # A subframe carries the sample in time slots 12 to 27, so 16 bit is what
                # fits without the encoder having to requantise on the way out.
                cv.Optional(CONF_BITS_PER_SAMPLE, default="16bit"): cv.All(
                    cv.float_with_unit("bits", "bit"), cv.one_of(16)
                ),
                # A frame is one sample of each channel and there are exactly two of them.
                # More channels need an IEC 61937 burst, which is a payload question rather
                # than a transport one.
                cv.Optional(CONF_NUM_CHANNELS, default=2): cv.one_of(2, int=True),
                cv.Optional(
                    CONF_BUFFER_DURATION, default="500ms"
                ): cv.positive_time_period_milliseconds,
                # Encoding runs continuously and the DMA must never be left waiting, so a
                # build that does other real-time work wants the two on separate cores.
                cv.Optional(CONF_TASK_CORE, default=-1): cv.int_range(min=-1, max=1),
            }
        ).extend(cv.COMPONENT_SCHEMA)
    ),
    _set_stream_limits,
    # Gapless output needs loop_transmission on the parallel IO transmit config,
    # which arrived in IDF 6.0. Without it the DMA goes idle between buffers and a
    # receiver loses lock at every seam.
    cv.require_framework_version(esp_idf=cv.Version(6, 0, 0)),
)


async def to_code(config):
    add_idf_component(
            name="esphome/esp-hub75",
            ref="0.3.6",
        )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
    cg.add(var.set_buffer_duration_ms(config[CONF_BUFFER_DURATION].total_milliseconds))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))
