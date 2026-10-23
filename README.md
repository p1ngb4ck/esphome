# i2c: bus_recovery option

Adds the option `bus_recovery` to the `i2c` bus (ESP-IDF and Arduino, default `true` = unchanged). With `false` the bus
recovery at setup (nine SCL pulses and a STOP) is skipped and the log shows `Recovery: disabled`.

Needed for devices that stall the bus after a recovery: the DFRobot DFR0550's STM32 keeps SCL low from its first
addressing on when the recovery ran while it powered up.

```yaml
i2c:
  sda: GPIO7
  scl: GPIO8
  bus_recovery: false
```

All changes for the DFR0550 together: [dfrobot-dfr0550-upstream](https://github.com/p1ngb4ck/esphome/tree/dfrobot-dfr0550-upstream).
