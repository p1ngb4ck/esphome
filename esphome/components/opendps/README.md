# OpenDPS Component for ESPHome

This component allows you to control and monitor OpenDPS power supplies via UART serial communication.

## Features

- **High-speed measurements**: Update rates up to loop speed (can achieve 10+ updates per second)
- **Voltage/Current monitoring**: Real-time V_in, V_out, I_out measurements
- **Power calculation**: Automatic power (W) calculation
- **Temperature monitoring**: Two temperature sensors
- **Output control**: Enable/disable power output
- **Parameter control**: Set voltage, current limits
- **Function selection**: Switch between different operating modes
- **Device control**: Lock/unlock, brightness control

## Hardware Setup

1. Connect your ESP device's UART to the OpenDPS serial interface
2. Use appropriate logic level (3.3V or 5V depending on your OpenDPS)
3. Typical connections:
   - ESP TX → OpenDPS RX
   - ESP RX → OpenDPS TX
   - GND → GND

## Example Configuration

### Basic Configuration (replaces ESP8266-proxy)

```yaml
# ESP32 or ESP8266
esphome:
  name: opendps-controller
  platform: ESP32
  board: esp32dev

# Enable logging
logger:
  level: DEBUG

# Enable WiFi
wifi:
  ssid: "your-ssid"
  password: "your-password"

# Enable Home Assistant API
api:

# UART for OpenDPS communication
uart:
  id: opendps_uart
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 115200

# OpenDPS Component
opendps:
  id: my_opendps
  uart_id: opendps_uart
  update_interval: 100ms  # Fast updates (10Hz)

# Sensors
sensor:
  - platform: opendps
    opendps_id: my_opendps
    voltage_in:
      name: "OpenDPS Input Voltage"
    voltage_out:
      name: "OpenDPS Output Voltage"
    current_out:
      name: "OpenDPS Output Current"
    power_out:
      name: "OpenDPS Output Power"
    temperature_1:
      name: "OpenDPS Temperature 1"
    temperature_2:
      name: "OpenDPS Temperature 2"

# Binary Sensor
binary_sensor:
  - platform: opendps
    opendps_id: my_opendps
    output_enabled:
      name: "OpenDPS Output Enabled"
```

### Advanced Configuration with Ethernet

```yaml
esphome:
  name: opendps-ethernet
  platform: ESP32
  board: esp32dev

logger:
  level: DEBUG

# Ethernet instead of WiFi for more reliability
ethernet:
  type: W5500
  clk_pin: GPIO18
  mosi_pin: GPIO23
  miso_pin: GPIO19
  cs_pin: GPIO5
  interrupt_pin: GPIO4
  reset_pin: GPIO14

api:

uart:
  id: opendps_uart
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 115200

opendps:
  id: my_opendps
  uart_id: opendps_uart
  update_interval: 50ms  # Very fast updates (20Hz)

sensor:
  - platform: opendps
    opendps_id: my_opendps
    voltage_in:
      name: "Input Voltage"
      filters:
        - throttle: 100ms  # Can throttle per-sensor if needed
    voltage_out:
      name: "Output Voltage"
    current_out:
      name: "Output Current"
    power_out:
      name: "Output Power"
    temperature_1:
      name: "Heatsink Temp"
    temperature_2:
      name: "Ambient Temp"

binary_sensor:
  - platform: opendps
    opendps_id: my_opendps
    output_enabled:
      name: "Power Output State"
```

## Actions

The component provides several actions that can be called from automations. Both lambda-based and YAML action syntax are supported.

### Enable/Disable Output

**Using Lambda:**
```yaml
button:
  - platform: template
    name: "Enable Output"
    on_press:
      - lambda: |-
          id(my_opendps).enable_output(true);

  - platform: template
    name: "Disable Output"
    on_press:
      - lambda: |-
          id(my_opendps).enable_output(false);
```

**Using YAML Actions:**
```yaml
button:
  - platform: template
    name: "Enable Output"
    on_press:
      - opendps.enable_output:
          id: my_opendps
          enable: true

  - platform: template
    name: "Disable Output"
    on_press:
      - opendps.enable_output:
          id: my_opendps
          enable: false
```

### Set Voltage/Current

**Using Lambda:**
```yaml
number:
  - platform: template
    name: "Set Voltage"
    min_value: 0
    max_value: 30
    step: 0.1
    unit_of_measurement: "V"
    mode: box
    set_action:
      - lambda: |-
          id(my_opendps).set_voltage(x);

  - platform: template
    name: "Set Current Limit"
    min_value: 0
    max_value: 5
    step: 0.01
    unit_of_measurement: "A"
    mode: box
    set_action:
      - lambda: |-
          id(my_opendps).set_current(x);
```

**Using YAML Actions:**
```yaml
number:
  - platform: template
    name: "Set Voltage"
    min_value: 0
    max_value: 30
    step: 0.1
    unit_of_measurement: "V"
    mode: box
    set_action:
      - opendps.set_voltage:
          id: my_opendps
          voltage: !lambda "return x;"

  - platform: template
    name: "Set Current Limit"
    min_value: 0
    max_value: 5
    step: 0.01
    unit_of_measurement: "A"
    mode: box
    set_action:
      - opendps.set_current:
          id: my_opendps
          current: !lambda "return x;"
```

### Utility Actions

**Using Lambda:**
```yaml
button:
  - platform: template
    name: "Ping Device"
    on_press:
      - lambda: |-
          id(my_opendps).send_ping();

  - platform: template
    name: "Request Version"
    on_press:
      - lambda: |-
          id(my_opendps).request_version();

  - platform: template
    name: "Lock Device"
    on_press:
      - lambda: |-
          id(my_opendps).lock(true);

  - platform: template
    name: "Unlock Device"
    on_press:
      - lambda: |-
          id(my_opendps).lock(false);
```

**Using YAML Actions:**
```yaml
button:
  - platform: template
    name: "Ping Device"
    on_press:
      - opendps.ping:
          id: my_opendps

  - platform: template
    name: "Request Version"
    on_press:
      - opendps.request_version:
          id: my_opendps

  - platform: template
    name: "Lock Device"
    on_press:
      - opendps.lock:
          id: my_opendps
          locked: true

  - platform: template
    name: "Unlock Device"
    on_press:
      - opendps.lock:
          id: my_opendps
          locked: false
```

### Set Brightness

**Using Lambda:**
```yaml
number:
  - platform: template
    name: "Display Brightness"
    min_value: 0
    max_value: 100
    step: 1
    unit_of_measurement: "%"
    mode: slider
    set_action:
      - lambda: |-
          id(my_opendps).set_brightness((uint8_t)x);
```

**Using YAML Actions:**
```yaml
number:
  - platform: template
    name: "Display Brightness"
    min_value: 0
    max_value: 100
    step: 1
    unit_of_measurement: "%"
    mode: slider
    set_action:
      - opendps.set_brightness:
          id: my_opendps
          brightness: !lambda "return (uint8_t)x;"
```

### Set Function

**Using Lambda:**
```yaml
select:
  - platform: template
    name: "Operating Function"
    options:
      - "cv"        # Constant Voltage
      - "cc"        # Constant Current
      - "cp"        # Constant Power
    set_action:
      - lambda: |-
          id(my_opendps).set_function(x.c_str());
```

**Using YAML Actions:**
```yaml
select:
  - platform: template
    name: "Operating Function"
    options:
      - "cv"        # Constant Voltage
      - "cc"        # Constant Current
      - "cp"        # Constant Power
    set_action:
      - opendps.set_function:
          id: my_opendps
          function: !lambda "return x.c_str();"
```

### Set Parameter (Advanced)

For setting arbitrary parameters directly:

**Using Lambda:**
```yaml
button:
  - platform: template
    name: "Set Custom Parameter"
    on_press:
      - lambda: |-
          id(my_opendps).set_parameter("vset", "12000");  // 12V in mV
```

**Using YAML Actions:**
```yaml
button:
  - platform: template
    name: "Set Custom Parameter"
    on_press:
      - opendps.set_parameter:
          id: my_opendps
          key: "vset"
          value: "12000"
```

### Firmware Upgrade

**Note:** Firmware upgrade feature is currently a placeholder and not fully implemented. The HTTP download and upgrade protocol implementation will be added in a future update.

**Using YAML Actions:**
```yaml
button:
  - platform: template
    name: "Upgrade Firmware"
    on_press:
      - opendps.upgrade_firmware:
          id: my_opendps
          firmware_url: "http://192.168.1.100/opendps-v5.bin"
```

## Update Intervals

The `update_interval` parameter controls how often the component queries the OpenDPS device:

- **1000ms (1s)**: Default, good for general monitoring
- **100ms**: Fast updates (10Hz), excellent for monitoring during repairs/debugging
- **50ms**: Very fast updates (20Hz), maximum practical speed
- **As fast as loop**: Set to very low values (10-20ms) for maximum speed

Note: OpenDPS firmware and serial baud rate may limit practical update speeds. 115200 baud is recommended.

## Protocol Details

The component implements the OpenDPS serial protocol:
- Frame format: SOF (0x7E) + payload + CRC-16 + EOF (0x7F)
- Byte stuffing with DLE (0x7D) escape sequences
- CRC-16 CCITT for error detection
- Command/response based protocol

## Advantages over ESP8266-proxy

1. **Full ESPHome integration**: Use all ESPHome components (WiFi, Ethernet, MQTT, Home Assistant API, etc.)
2. **Modern hardware**: Use ESP32 with better performance and more GPIO
3. **Flexible connectivity**: WiFi, Ethernet, or both
4. **Built-in monitoring**: All ESPHome sensors and diagnostic features
5. **OTA updates**: Easy firmware updates
6. **Automation**: Full Home Assistant integration with automations

## Troubleshooting

**No communication:**
- Check UART wiring (TX/RX might be swapped)
- Verify baud rate matches (115200)
- Check logic levels (3.3V vs 5V)
- Enable debug logging to see frames

**Slow updates:**
- Lower `update_interval`
- Check baud rate
- Reduce log level to reduce UART overhead

**CRC errors:**
- Check for electrical noise
- Ensure good ground connection
- Try lower baud rate
- Add pull-up resistors on TX/RX lines

## License

This component is licensed under MIT License.

## Credits

Based on the OpenDPS project by Johan Kanflo: https://github.com/kanflo/opendps
