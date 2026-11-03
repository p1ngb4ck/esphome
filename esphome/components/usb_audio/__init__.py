import logging

import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.usb_host import (
    USB_BLACKLIST_SCHEMA,
    setup_usb_match,
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
    CONF_OUTPUT_SPEAKER,
    CONF_PLATFORM,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["audio", "usb_host", "speaker", "microphone"]
MULTI_CONF = True
DOMAIN = "usb_audio"
KEY_FIFO_BIAS_APPLIED = "fifo_bias_applied"

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
CONF_STREAM_IDLE_TIMEOUT = "stream_idle_timeout"
CONF_VOLUME_CURVE = "volume_curve"
CONF_CHANNEL_PAIR = "channel_pair"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_DEFAULT_BUFFER_SIZE = 6400
MAX_SINKS = 4
PREFERRED_FORMATS = {CONF_SPEAKER: (48000, 2), CONF_MICROPHONE: (16000, 1)}

BITS_PER_SAMPLE_OPTIONS = [8, 16, 24, 32]


def _format_schema(rate, channels):
    return cv.Schema(
        {
            cv.Optional(CONF_SAMPLE_RATE, default=rate): cv.int_range(min=8000, max=96000),
            cv.Optional(CONF_BITS_PER_SAMPLE, default="16bit"): cv.All(
                cv.float_with_unit("bits", "bit"), cv.one_of(*BITS_PER_SAMPLE_OPTIONS)
            ),
            cv.Optional(CONF_NUM_CHANNELS, default=channels): cv.int_range(min=1, max=8),
        }
    )

UacChannelPair = usb_audio_ns.enum("UacChannelPair", is_class=True)
# ESPHome has no surround path, so a multichannel card is driven as one stereo pair. This
# picks which pair when the device offers nothing but multichannel alt-settings; a card that
# also describes plain stereo uses that and ignores this option.
CHANNEL_PAIRS = {
    "front": UacChannelPair.FRONT,
    "center_lfe": UacChannelPair.CENTER_LFE,
    "back": UacChannelPair.BACK,
    "side": UacChannelPair.SIDE,
}

VolumeCurve = usb_audio_ns.enum("VolumeCurve", is_class=True)
VOLUME_CURVES = {
    "linear": VolumeCurve.LINEAR,
    "logarithmic": VolumeCurve.LOGARITHMIC,
}

CONF_DEVICE_QUIRKS = "device_quirks"
CONF_VOLUME_ENCODING = "volume_encoding"

VolumeEncoding = usb_audio_ns.enum("VolumeEncoding", is_class=True)
# How a volume value is put on the wire. "db" is the class definition: a signed 16-bit value
# in 1/256 dB inside the range the device reports. "byte" is for devices that do not follow
# it and take the setting as a plain 0..255 level instead. The two cannot be told apart by
# asking the device, so this is selected per VID and PID; Linux carries the equivalent in a
# per-device quirk table for the same reason.
VOLUME_ENCODINGS = {
    "db": VolumeEncoding.DB,
    "byte": VolumeEncoding.BYTE,
}

DEVICE_QUIRK_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_VID): cv.hex_uint16_t,
        cv.Required(CONF_PID): cv.hex_uint16_t,
        cv.Optional(CONF_VOLUME_ENCODING, default="db"): cv.enum(
            VOLUME_ENCODINGS, lower=True
        ),
    }
)

SUPPORTED_VARIANTS = [
    esp32.const.VARIANT_ESP32S2,
    esp32.const.VARIANT_ESP32S3,
    esp32.const.VARIANT_ESP32P4,
    esp32.const.VARIANT_ESP32S31,
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


def _service_intervals_per_second() -> int:
    """How often the controller can service an isochronous endpoint.

    A full-speed bus runs 1 ms frames, so a packet has to carry a whole millisecond of
    audio. High speed runs 125 us microframes, and an endpoint asking for every microframe
    carries an eighth of that -- which is what makes a wide format fit a packet buffer that
    a full-speed device of the same format would not.

    Which of the two applies is a property of the chip: the ESP32-P4 is the only variant
    with a high-speed controller. It is not a property of the configuration, because the
    speed and the bInterval come from the descriptor of whatever gets plugged in. This takes
    the best the hardware can do; the controller checks the endpoint it actually gets
    against its real limits in hcd_pipe_alloc(), which is where a device that asks for more
    than the buffer holds is refused, with the reason in the log.
    """
    return (
        8000
        if esp32.get_esp32_variant()
        in (esp32.const.VARIANT_ESP32P4, esp32.const.VARIANT_ESP32S31)
        else 1000
    )


def _endpoint_mps(stream) -> int:
    """Largest isochronous packet a stream of this format needs.

    A packet carries one service interval of audio rounded up to a whole audio frame.
    Devices declare one further frame on top: a rate that is not a multiple of the interval
    count sends a longer packet whenever its accumulator wraps, and an asynchronous endpoint
    does the same when its clock runs fast.
    """
    frame = int(stream[CONF_NUM_CHANNELS]) * (int(stream[CONF_BITS_PER_SAMPLE]) // 8)
    intervals = _service_intervals_per_second()
    per_interval = -(-int(stream[CONF_SAMPLE_RATE]) // intervals) * frame
    return per_interval + frame


def _stream_formats(full_config, domain, hub_id=None):
    """Every usb_audio stream format of one direction: its platforms and the blocks' own."""
    for entry in full_config.get(domain) or []:
        if entry.get(CONF_PLATFORM) == "usb_audio" and (
            hub_id is None or str(entry.get(CONF_USB_AUDIO_ID)) == hub_id
        ):
            yield entry
    for block in full_config.get(DOMAIN) or []:
        if domain in block and (hub_id is None or str(block[CONF_ID]) == hub_id):
            yield block[domain]


def _usb_audio_mps(full_config) -> tuple[int, int]:
    """Widest playback and capture packet over every usb_audio stream, 0 where there is none.

    Several usb_audio instances can share one controller, and its FIFO division has to carry
    the widest stream in each direction.
    """
    widest = {}
    for domain in (CONF_SPEAKER, CONF_MICROPHONE):
        for entry in _stream_formats(full_config, domain):
            widest[domain] = max(widest.get(domain, 0), _endpoint_mps(entry))
    return widest.get(CONF_SPEAKER, 0), widest.get(CONF_MICROPHONE, 0)


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


def _buffer_minimum(stream) -> int:
    frame = int(stream[CONF_NUM_CHANNELS]) * (int(stream[CONF_BITS_PER_SAMPLE]) // 8)
    return (-(-int(stream[CONF_SAMPLE_RATE]) // 1000) + 1) * frame


def _apply_buffer_sizes(config, full_config):
    hub_id = str(config[CONF_ID])
    for domain, key in (
        (CONF_SPEAKER, CONF_SPEAKER_BUFFER_SIZE),
        (CONF_MICROPHONE, CONF_MICROPHONE_BUFFER_SIZE),
    ):
        minimum = max(
            (_buffer_minimum(entry) for entry in _stream_formats(full_config, domain, hub_id)),
            default=0,
        )
        if key not in config:
            config[key] = max(CONF_DEFAULT_BUFFER_SIZE, minimum)
        elif config[key] < minimum:
            raise cv.Invalid(
                f"'{key}' must be at least {minimum} bytes (one 1 ms packet and one frame) "
                f"for this {domain} format",
                path=[key],
            )


def _route_count(full_config, domain, key, hub_id) -> int:
    count = 0
    for entry in full_config.get(domain) or []:
        if entry.get(CONF_PLATFORM) != "resampler" or not isinstance(entry.get(key), list):
            continue
        count += sum(1 for item in entry[key] if str(item.get(DOMAIN)) == hub_id)
    return count


def _validate_routes(config, full_config):
    hub_id = str(config[CONF_ID])
    for domain in (CONF_SPEAKER, CONF_MICROPHONE):
        platforms = [
            entry
            for entry in full_config.get(domain) or []
            if entry.get(CONF_PLATFORM) == "usb_audio" and str(entry.get(CONF_USB_AUDIO_ID)) == hub_id
        ]
        if len(platforms) > 1:
            raise cv.Invalid(f"usb_audio '{hub_id}': one {domain} platform per usb_audio block")
        if platforms and domain in config:
            own = config[domain]
            if any(int(platforms[0][key]) != int(own[key]) for key in (CONF_SAMPLE_RATE, CONF_BITS_PER_SAMPLE, CONF_NUM_CHANNELS)):
                raise cv.Invalid(
                    f"usb_audio '{hub_id}': the {domain} platform's format differs from the block's '{domain}' format",
                    path=[domain],
                )
    speakers = _route_count(full_config, CONF_SPEAKER, CONF_OUTPUT_SPEAKER, hub_id)
    fixed = any(
        entry.get(CONF_PLATFORM) == "usb_audio" and str(entry.get(CONF_USB_AUDIO_ID)) == hub_id
        for entry in full_config.get(CONF_SPEAKER) or []
    )
    if speakers + (1 if fixed else 0) > MAX_SINKS:
        raise cv.Invalid(f"usb_audio '{hub_id}': at most {MAX_SINKS} speakers per device (routes and platform)")
    microphones = _route_count(full_config, CONF_MICROPHONE, CONF_MICROPHONE, hub_id)
    fixed_mic = any(
        entry.get(CONF_PLATFORM) == "usb_audio" and str(entry.get(CONF_USB_AUDIO_ID)) == hub_id
        for entry in full_config.get(CONF_MICROPHONE) or []
    )
    if microphones and fixed_mic:
        raise cv.Invalid(
            f"usb_audio '{hub_id}': a microphone platform and resampler microphone sources cannot share a block"
        )
    for domain, routes in ((CONF_SPEAKER, speakers), (CONF_MICROPHONE, microphones)):
        if routes and domain not in config:
            config[domain] = _format_schema(*PREFERRED_FORMATS[domain])({})


def _final_validate(config):
    _validate_routes(config, fv.full_config.get())
    _apply_buffer_sizes(config, fv.full_config.get())
    variant = esp32.get_esp32_variant()
    if not _needs_fifo_bias():
        return config

    out_mps, in_mps = _usb_audio_mps(fv.full_config.get())
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

    # Once for all instances: they share the controller and its division.
    if CORE.data.setdefault(DOMAIN, {}).get(KEY_FIFO_BIAS_APPLIED):
        return
    CORE.data[DOMAIN][KEY_FIFO_BIAS_APPLIED] = True
    out_mps, in_mps = _usb_audio_mps(CORE.config)
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
            cv.Optional(CONF_MICROPHONE_BUFFER_SIZE): cv.positive_int,
            cv.Optional(CONF_SPEAKER_BUFFER_SIZE): cv.positive_int,
            cv.Optional(CONF_CHANNEL_PAIR, default="front"): cv.enum(
                CHANNEL_PAIRS, lower=True
            ),
            cv.Optional(CONF_VID, default=0x0000): cv.hex_uint16_t,
            cv.Optional(CONF_PID, default=0x0000): cv.hex_uint16_t,
            **USB_BLACKLIST_SCHEMA,
            cv.Optional(CONF_FEEDBACK, default=True): cv.boolean,
            # A stream unused this long is closed and its pipes (USB host channels, a fixed
            # number per controller) go back to other devices; reopened when used (~30 ms).
            # 0s keeps the streams open while the device is connected.
            cv.Optional(CONF_STREAM_IDLE_TIMEOUT, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_VOLUME_CURVE, default="linear"): cv.enum(
                VOLUME_CURVES, lower=True
            ),
            cv.Optional(CONF_DEVICE_QUIRKS): cv.ensure_list(DEVICE_QUIRK_SCHEMA),
            cv.Optional(CONF_SPEAKER): _format_schema(48000, 2),
            cv.Optional(CONF_MICROPHONE): _format_schema(16000, 1),
        }
    ),
    esp32.only_on_variant(supported=SUPPORTED_VARIANTS),
)


async def to_code(config):
    cg.add_define("USE_USB_AUDIO")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await setup_usb_match(var, config)

    cg.add(var.set_microphone_buffer_size(config[CONF_MICROPHONE_BUFFER_SIZE]))
    cg.add(var.set_speaker_buffer_size(config[CONF_SPEAKER_BUFFER_SIZE]))
    cg.add(var.set_vid(config[CONF_VID]))
    cg.add(var.set_pid(config[CONF_PID]))
    cg.add(var.set_feedback_enabled(config[CONF_FEEDBACK]))
    cg.add(var.set_stream_idle_timeout(config[CONF_STREAM_IDLE_TIMEOUT]))
    cg.add(var.set_volume_curve(config[CONF_VOLUME_CURVE]))
    cg.add(var.set_channel_pair(config[CONF_CHANNEL_PAIR]))
    for quirk in config.get(CONF_DEVICE_QUIRKS, []):
        cg.add(
            var.add_device_quirk(
                quirk[CONF_VID], quirk[CONF_PID], quirk[CONF_VOLUME_ENCODING]
            )
        )

    for domain, setter in ((CONF_SPEAKER, var.set_speaker_params), (CONF_MICROPHONE, var.set_microphone_params)):
        if fmt := config.get(domain):
            cg.add(setter(int(fmt[CONF_NUM_CHANNELS]), int(fmt[CONF_BITS_PER_SAMPLE]), int(fmt[CONF_SAMPLE_RATE])))

    cg.add_define("USE_USB_ISOC_TRANSFERS")
    cg.add_define("USE_USB_CONTROL_TRANSFERS")

    _apply_fifo_bias()
