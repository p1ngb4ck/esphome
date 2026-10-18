"""Tests for usb_audio packet sizing, FIFO split selection and microphone channels."""

import pytest

from esphome import config_validation as cv
from esphome.components import esp32
import esphome.components.usb_audio as usb_audio
from esphome.components.usb_audio import microphone as usb_microphone
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_CHANNELS,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)


def _stream(sample_rate: int, channels: int, bits: int) -> dict:
    return {
        CONF_SAMPLE_RATE: sample_rate,
        CONF_NUM_CHANNELS: channels,
        CONF_BITS_PER_SAMPLE: float(bits),
    }


@pytest.fixture
def full_speed_only(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        usb_audio.esp32, "get_esp32_variant", lambda: esp32.const.VARIANT_ESP32S3
    )


@pytest.fixture
def high_speed_capable(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        usb_audio.esp32, "get_esp32_variant", lambda: esp32.const.VARIANT_ESP32P4
    )


def test_endpoint_mps_full_speed(full_speed_only: None) -> None:
    # 48 frames of 4 bytes per 1 ms frame, plus one frame of slack
    assert usb_audio._endpoint_mps(_stream(48000, 2, 16)) == 196
    # 44.1 kHz carries up to 45 frames per packet
    assert usb_audio._endpoint_mps(_stream(44100, 2, 16)) == 184


def test_endpoint_mps_high_speed(high_speed_capable: None) -> None:
    # 6 frames of 4 bytes per 125 us microframe, plus one frame of slack
    assert usb_audio._endpoint_mps(_stream(48000, 2, 16)) == 28


def test_fifo_bias_playback_takes_the_room() -> None:
    assert usb_audio._select_fifo_bias(196, 0) == usb_audio.BIAS_PERIODIC_OUT
    assert usb_audio._select_fifo_bias(196, 34) == usb_audio.BIAS_PERIODIC_OUT


def test_fifo_bias_capture_alone_keeps_the_default() -> None:
    assert usb_audio._select_fifo_bias(0, 196) == usb_audio.BIAS_BALANCED
    assert usb_audio._select_fifo_bias(0, 600) == usb_audio.BIAS_IN


def test_fifo_bias_no_split_fits() -> None:
    assert usb_audio._select_fifo_bias(196, 196) is None
    assert usb_audio._select_fifo_bias(601, 0) is None


def test_microphone_channels_valid() -> None:
    config = {CONF_NUM_CHANNELS: 2, CONF_CHANNELS: [1]}
    assert usb_microphone._validate_enabled_channels(config) is config


def test_microphone_channels_duplicate_rejected() -> None:
    with pytest.raises(cv.Invalid, match="must be unique"):
        usb_microphone._validate_enabled_channels(
            {CONF_NUM_CHANNELS: 2, CONF_CHANNELS: [0, 0]}
        )


def test_microphone_channels_out_of_range_rejected() -> None:
    with pytest.raises(cv.Invalid, match="out of range"):
        usb_microphone._validate_enabled_channels(
            {CONF_NUM_CHANNELS: 2, CONF_CHANNELS: [2]}
        )


def test_microphone_channels_empty_rejected() -> None:
    with pytest.raises(cv.Invalid, match="At least one channel"):
        usb_microphone._validate_enabled_channels(
            {CONF_NUM_CHANNELS: 1, CONF_CHANNELS: []}
        )
