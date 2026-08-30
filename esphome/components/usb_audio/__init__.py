import logging

import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.usb_host import (
    fs_peripheral_index,
    per_port_fifo_bias_available,
    set_port_fifo_bias,
    usb_host_ns,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_MICROPHONE,
    CONF_NUM_CHANNELS,
    CONF_PLATFORM,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["usb_host", "esp32"]
AUTO_LOAD = ["audio"]
MULTI_CONF = False

usb_audio_ns = cg.esphome_ns.namespace("usb_audio")

USBAudioClient = usb_audio_ns.class_(
    "USBAudioClient",
    usb_host_ns.class_("USBClient"),
    cg.Component,
)

CONF_USB_AUDIO_ID = "usb_audio_id"
CONF_MICROPHONE_BUFFER_SIZE = "microphone_buffer_size"
CONF_SPEAKER_BUFFER_SIZE = "speaker_buffer_size"
CONF_FEEDBACK = "feedback"
CONF_VOLUME_CURVE = "volume_curve"
CONF_CHANNEL_PAIR = "channel_pair"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_DEFAULT_BUFFER_SIZE = 6400

UacChannelPair = usb_audio_ns.enum("UacChannelPair", is_class=True)
# ESPHome has no surround path, so a multichannel card is driven as one stereo pair. This
# picks which pair when the device offers nothing but multichannel alt-settings; a card that
# also describes plain stereo uses that and ignores this option.
CHANNEL_PAIRS = {
    "front": UacChannelPair.FRONT,
    "side": UacChannelPair.SIDE,
    "back": UacChannelPair.BACK,
}

VolumeCurve = usb_audio_ns.enum("VolumeCurve", is_class=True)
VOLUME_CURVES = {
    "linear": VolumeCurve.LINEAR,
    "logarithmic": VolumeCurve.LOGARITHMIC,
}

SUPPORTED_VARIANTS = [
    esp32.const.VARIANT_ESP32S2,
    esp32.const.VARIANT_ESP32S3,
    esp32.const.VARIANT_ESP32P4,
]

# -- USB host FIFO biasing ----------------------------------------------------
# The USB controller caches packets in one FIFO that is split three ways: received packets,
# non-periodic OUT (bulk and control) and periodic OUT (interrupt and isochronous). How it
# is split decides the largest packet each direction can carry, and ESP-IDF offers three
# fixed splits. Audio runs on isochronous endpoints, so a stream that needs a larger packet
# than the split allows cannot be submitted at all.
#
# On the full-speed controller in the ESP32-S2 and S3 the split is tight enough that the
# direction has to be chosen. The numbers below follow _calculate_fifo_from_bias() in
# hcd_dwc.c together with usb_dwc_hal_get_mps_limits(), for a FIFO of 200 lines and an
# OTG_DFIFO_DEPTH of 256. They are not the ones in the ESP-IDF Kconfig help text, which is
# out of date for the balanced split.
#
# The ESP32-P4 has two host controllers and dual host runs them at the same time, so the
# split is chosen per controller rather than for the chip. Only the full-speed one is moved.
# The high-speed one keeps ESP-IDF's balanced split, where it has 1024 bytes of non-periodic
# OUT for bulk and 512 bytes of periodic OUT for isochronous and needs nothing from here;
# moving it would set nptx_fifo_lines to depth / 16, which usb_dwc_hal_get_mps_limits()
# reports as 256 bytes of non-periodic OUT, below the 512 byte wMaxPacketSize every
# high-speed bulk OUT endpoint has, and hcd_pipe_alloc() would then reject every bulk pipe
# on it. usb_host makes the per-controller choice possible; see its idf_usb_patch.py.
#
# The line counts below are for a full-speed controller with 200 FIFO lines, confirmed for
# the S2 and S3. The P4's full-speed controller reports its own count from hardware, so
# there the numbers bound the validation slightly rather than exactly.
BIAS_BALANCED = "balanced"
BIAS_IN = "in"
BIAS_PERIODIC_OUT = "periodic_out"


def _needs_fifo_bias() -> bool:
    """Whether a full-speed controller is in play and its packet limits therefore apply.

    An isochronous stream only meets the limits below where a full-speed controller serves
    it. Which controller that is, and whether there is one at all, is usb_host's to say.
    """
    return fs_peripheral_index() is not None


# Largest isochronous packet each direction can carry, per split.
FS_MPS_LIMITS = {
    BIAS_BALANCED: {"in": 408, "out": 128},
    BIAS_IN: {"in": 600, "out": 128},
    BIAS_PERIODIC_OUT: {"in": 128, "out": 600},
}

FS_BIAS_SDKCONFIG = {
    BIAS_BALANCED: "CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED",
    BIAS_IN: "CONFIG_USB_HOST_HW_BUFFER_BIAS_IN",
    BIAS_PERIODIC_OUT: "CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT",
}


def _endpoint_mps(stream) -> int:
    """Largest isochronous packet a stream of this format needs.

    A full-speed device is serviced once per 1 ms frame, so a packet carries one
    millisecond of audio rounded up to a whole audio frame. Devices declare one further
    frame on top: a rate that is not a multiple of 1000 sends a longer packet whenever its
    accumulator wraps, and an asynchronous endpoint does the same when its clock runs fast.
    """
    frame = int(stream[CONF_NUM_CHANNELS]) * (int(stream[CONF_BITS_PER_SAMPLE]) // 8)
    per_interval = -(-int(stream[CONF_SAMPLE_RATE]) // 1000) * frame
    return per_interval + frame


def _usb_audio_streams(full_config):
    """The configured playback and capture stream, either of which may be absent."""
    streams = {}
    for domain in (CONF_SPEAKER, CONF_MICROPHONE):
        for entry in full_config.get(domain) or []:
            if entry.get(CONF_PLATFORM) == "usb_audio":
                streams[domain] = entry
    return streams.get(CONF_SPEAKER), streams.get(CONF_MICROPHONE)


def _select_fifo_bias(out_mps: int, in_mps: int):
    """First split that carries both directions, or None when none of them does.

    Playback gets the room whenever a speaker is configured: a speaker endpoint is the
    larger of the two by a wide margin, its format is what a listener notices, and most
    microphones are mono and fit into what is left either way. Taking that split even where
    the default would do also covers a device whose endpoint asks for more than the format
    alone suggests. Capture on its own keeps the default split as long as that carries it,
    since moving away from the default changes the USB host for everything sharing it.
    """
    if out_mps:
        order = (BIAS_PERIODIC_OUT, BIAS_BALANCED, BIAS_IN)
    else:
        order = (BIAS_BALANCED, BIAS_IN, BIAS_PERIODIC_OUT)
    for bias in order:
        limits = FS_MPS_LIMITS[bias]
        if out_mps <= limits["out"] and in_mps <= limits["in"]:
            return bias
    return None


def _final_validate(config):
    variant = esp32.get_esp32_variant()
    if not _needs_fifo_bias():
        return config

    speaker, microphone = _usb_audio_streams(fv.full_config.get())
    out_mps = _endpoint_mps(speaker) if speaker else 0
    in_mps = _endpoint_mps(microphone) if microphone else 0
    if _select_fifo_bias(out_mps, in_mps) is not None:
        return config

    # Say which stream is over which limit and what would bring it inside, rather than only
    # that the combination does not work.
    widest_out = max(limits["out"] for limits in FS_MPS_LIMITS.values())
    widest_in = max(limits["in"] for limits in FS_MPS_LIMITS.values())
    if out_mps > widest_out:
        raise cv.Invalid(
            f"This speaker format does not fit the USB controller of the {variant}: it "
            f"needs {out_mps} bytes per isochronous packet and at most {widest_out} are "
            f"available. Use fewer channels or a lower sample rate."
        )
    if in_mps > widest_in:
        raise cv.Invalid(
            f"This microphone format does not fit the USB controller of the {variant}: it "
            f"needs {in_mps} bytes per isochronous packet and at most {widest_in} are "
            f"available. Use fewer channels or a lower sample rate."
        )
    # Each fits on its own, so it is the combination. Playback is what gets the room, so
    # capture is what has to give.
    capture_left = FS_MPS_LIMITS[BIAS_PERIODIC_OUT]["in"]
    raise cv.Invalid(
        f"Playback and capture do not fit together on the {variant}. The USB controller "
        f"splits one packet buffer between the two directions, and the split that carries "
        f"this speaker's {out_mps} bytes per isochronous packet leaves {capture_left} "
        f"bytes for capture. This microphone needs {in_mps}. Bring it within "
        f"{capture_left} bytes -- one channel, or a lower sample rate -- or configure only "
        f"one of the two."
    )


FINAL_VALIDATE_SCHEMA = _final_validate


def _apply_fifo_bias() -> None:
    variant = esp32.get_esp32_variant()
    index = fs_peripheral_index()
    if index is None:
        return

    speaker, microphone = _usb_audio_streams(CORE.config)
    out_mps = _endpoint_mps(speaker) if speaker else 0
    in_mps = _endpoint_mps(microphone) if microphone else 0
    bias = _select_fifo_bias(out_mps, in_mps)
    if bias is None:
        # Rejected during validation; nothing sensible to set.
        return

    if per_port_fifo_bias_available():
        # One controller, not all of them. On the P4 this is what keeps the high-speed
        # controller on the balanced division, where its bulk endpoints have the 512 bytes
        # of non-periodic OUT they need while this controller carries the audio.
        set_port_fifo_bias(index, bias)
        return

    # Without the per-port path this is a single option reaching every controller. That is
    # only reached where the full-speed controller is the only one, so nothing else loses
    # anything by it.
    for name, option in FS_BIAS_SDKCONFIG.items():
        esp32.add_idf_sdkconfig_option(option, name == bias)

    if bias == BIAS_BALANCED:
        return
    # The division is a property of the USB host, not of this component, so anything else
    # sharing it is affected. That is worth saying out loud.
    limits = FS_MPS_LIMITS[bias]
    reason = (
        f"playback needs {out_mps} bytes per isochronous packet"
        if out_mps
        else f"capture needs {in_mps} bytes per isochronous packet"
    )
    _LOGGER.warning(
        "usb_audio: the USB host packet buffer on the %s is split towards '%s' because %s. "
        "While this is set, isochronous endpoints are limited to %d bytes out and %d bytes "
        "in, for every component sharing this USB host.",
        variant,
        bias,
        reason,
        limits["out"],
        limits["in"],
    )


CONFIG_SCHEMA = cv.All(
    cv.COMPONENT_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(USBAudioClient),
            cv.Optional(
                CONF_MICROPHONE_BUFFER_SIZE, default=CONF_DEFAULT_BUFFER_SIZE
            ): cv.positive_int,
            cv.Optional(
                CONF_SPEAKER_BUFFER_SIZE, default=CONF_DEFAULT_BUFFER_SIZE
            ): cv.positive_int,
            cv.Optional(CONF_CHANNEL_PAIR, default="front"): cv.enum(
                CHANNEL_PAIRS, lower=True
            ),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_FEEDBACK, default=True): cv.boolean,
            cv.Optional(CONF_VOLUME_CURVE, default="linear"): cv.enum(
                VOLUME_CURVES, lower=True
            ),
        }
    ),
    esp32.only_on_variant(supported=SUPPORTED_VARIANTS),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_microphone_buffer_size(config[CONF_MICROPHONE_BUFFER_SIZE]))
    cg.add(var.set_speaker_buffer_size(config[CONF_SPEAKER_BUFFER_SIZE]))
    cg.add(var.set_vid(config[CONF_VID]))
    cg.add(var.set_pid(config[CONF_PID]))
    cg.add(var.set_feedback_enabled(config[CONF_FEEDBACK]))
    cg.add(var.set_volume_curve(config[CONF_VOLUME_CURVE]))
    cg.add(var.set_channel_pair(config[CONF_CHANNEL_PAIR]))

    cg.add_define("USE_USB_ISOC_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")

    _apply_fifo_bias()
