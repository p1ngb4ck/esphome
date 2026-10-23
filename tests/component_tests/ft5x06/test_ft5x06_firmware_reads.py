from collections.abc import Callable
from pathlib import Path


def test_firmware_reads_code_generation(
    generate_main: Callable[[str | Path], str],
    component_fixture_path: Callable[[str], Path],
) -> None:
    main_cpp = generate_main(component_fixture_path("ft5x06_firmware_reads.yaml"))
    assert "touch_firmware_reads->set_firmware_reads(true);" in main_cpp
    assert "touch_default->set_firmware_reads(" not in main_cpp
