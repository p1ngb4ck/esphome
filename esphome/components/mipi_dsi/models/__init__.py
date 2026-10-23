from esphome.components.mipi import DriverChip, flatten_sequence
from esphome.const import CONF_INIT_SEQUENCE, CONF_SWAP_XY
from esphome.types import ConfigType

CONF_COMMAND_MODE = "command_mode"
COMMAND_MODE_DCS = "dcs"
COMMAND_MODE_GENERIC = "generic"


class DsiDriverChip(DriverChip):
    """A driver chip for MIPI DSI displays."""

    def get_sequence(
        self, config: ConfigType, add_madctl: bool = True, add_reset: bool = False
    ) -> tuple[int, ...]:
        if config.get(CONF_COMMAND_MODE) != COMMAND_MODE_GENERIC:
            return super().get_sequence(config, add_madctl, add_reset)
        sequence = list(self.initsequence or ())
        sequence.extend(config.get(CONF_INIT_SEQUENCE, []))
        sequence = [x if isinstance(x, tuple) else (x,) for x in sequence]
        return flatten_sequence(sequence)

    @property
    def transforms(self) -> set[str]:
        """
        Return the set of transformations supported by this driver chip.
        DSI displays do not support axis swapping, so this method removes CONF_SWAP_XY
        """
        return super().transforms - {CONF_SWAP_XY}
