from esphome.components.mipi import DriverChip, flatten_sequence
from esphome.const import CONF_INIT_SEQUENCE, CONF_SWAP_XY

CONF_COMMAND_MODE = "command_mode"
CONF_I2C_ADDRESS = "i2c_address"
CONF_I2C_INIT_SEQUENCE = "i2c_init_sequence"
COMMAND_MODE_DCS = "dcs"
COMMAND_MODE_GENERIC = "generic"


class DsiDriverChip(DriverChip):
    """A driver chip for MIPI DSI displays."""

    def get_sequence(self, config, add_madctl=True, add_reset=False) -> tuple[int, ...]:
        """
        In generic command mode (bridge chips such as the TC358762) the init sequence is sent as is, as generic
        writes: none of the DCS commands (PIXFMT, MADCTL, INVON/INVOFF, SLPOUT, DISPON) is added.
        """
        if config.get(CONF_COMMAND_MODE) != COMMAND_MODE_GENERIC:
            return super().get_sequence(config, add_madctl, add_reset)
        sequence = list(self.initsequence or ())
        sequence.extend(config.get(CONF_INIT_SEQUENCE, []))
        sequence = [x if isinstance(x, tuple) else (x,) for x in sequence]
        return flatten_sequence(sequence)

    def get_i2c_init_sequence(self, config) -> list | None:
        return config.get(
            CONF_I2C_INIT_SEQUENCE, self.get_default(CONF_I2C_INIT_SEQUENCE)
        )

    @property
    def transforms(self) -> set[str]:
        """
        Return the set of transformations supported by this driver chip.
        DSI displays do not support axis swapping, so this method removes CONF_SWAP_XY
        """
        return super().transforms - {CONF_SWAP_XY}
