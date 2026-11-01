from collections.abc import Callable
from pathlib import Path
import re

ATTINY_START = "69, 254, 133, 0, 30, 255, 69, 254, 134, 0, 8, 255, 69, 254, 131, 0, 8, 255, 69, 254, 129, "


def init_sequence(main_cpp: str, display_id: str) -> str:
    name = re.search(rf"{display_id}->set_init_sequence\((\w+), \d+\);", main_cpp)
    assert name is not None
    values = re.search(
        rf"static constexpr uint8_t {name.group(1)}\[\] PROGMEM = \{{([^}}]*)\}};",
        main_cpp,
    )
    assert values is not None
    return values.group(1)


def test_rpi7_code_generation(
    generate_main: Callable[[str | Path], str],
    component_fixture_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_fixture_path("mipi_dsi_rpi7.yaml"))
    for display_id in ("rpi7_normal", "rpi7_rotated"):
        for line in (
            "set_lanes(1);",
            "set_lane_bit_rate(720.0f);",
            "set_frame_ack(false);",
            "set_continuous_clock(true);",
            "set_eotp(false);",
            "set_lp_horizontal_blanking(false);",
            "set_lp_vertical_active(false);",
            "set_generic_commands(true);",
            "set_i2c_bus(display_bus);",
        ):
            assert f"{display_id}->{line}" in main_cpp
    assert init_sequence(main_cpp, "rpi7_normal").startswith(ATTINY_START + "4, ")
    assert init_sequence(main_cpp, "rpi7_rotated").startswith(ATTINY_START + "8, ")
    assert init_sequence(main_cpp, "rpi7_normal").endswith(
        "4, 5, 1, 1, 0, 0, 0, 4, 5, 2, 1, 0, 0, 0, 100, 255"
    )
