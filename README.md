# mipi, mipi_dsi: I2C register writes in the init sequence

Some DSI displays need register writes to an I2C controller (power, resets) as part of their start-up. This adds
them as entries of the existing init sequence, next to the existing delay entries:

- `i2c_write(address, register, value)` in `esphome/components/mipi`, stored with the length byte 0xFE
  (0xFF is the delay).
- mipi_dsi writes such an entry with `i2c::I2CDevice::write_registers()`, in its place in the sequence.
- The display option `i2c_id` exists only for models whose init sequence contains I2C entries; with a single I2C bus
  it is selected automatically.

Model example:

```python
from esphome.components.mipi import delay, i2c_write

initsequence=[
    i2c_write(0x45, 0x85, 0x00), delay(30),
    i2c_write(0x45, 0x83, 0x0D), delay(8),
    (0x10, 0x02, 0x03, 0x00, 0x00, 0x00),
]
```

```yaml
display:
  - platform: mipi_dsi
    model: ...
    i2c_id: bus_a
```

Part of [rpi7-v1-upstream](https://github.com/p1ngb4ck/esphome/tree/rpi7-v1-upstream).
