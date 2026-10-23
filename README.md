# ft5x06: firmware_reads option

Adds the option `firmware_reads` to the `ft5x06` touchscreen (default `false` = unchanged). With `true` the controller
is read the way the Raspberry Pi firmware does it: no access at setup, the touch count from register 0x02 (0xFF: no
touch yet), then 4 bytes per point in separate reads from 0x03 + 6 * n, and only points with event type 1 or 2 are
reported.

Needed for emulated FT5x06 controllers that answer only this pattern, such as the STM32 of the DFRobot DFR0550.

```yaml
touchscreen:
  - platform: ft5x06
    address: 0x38
    firmware_reads: true
```

All changes for the DFR0550 together: [dfrobot-dfr0550-upstream](https://github.com/p1ngb4ck/esphome/tree/dfrobot-dfr0550-upstream).
