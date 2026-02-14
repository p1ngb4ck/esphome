# OpenDPS Virtual Lab PSU Card

A skeuomorphic Home Assistant dashboard card that looks and feels like a real bench power supply. Built for controlling OpenDPS devices via ESPHome.

## Features

- **Seven-segment LCD displays** with configurable color (green, amber, blue, white)
- **Rotary encoder knobs** for voltage and current adjustment (drag or scroll)
- **Illuminated power button** with glow effect when output is active
- **CV/CC/CP mode indicator LEDs** showing current operating mode
- **Temperature bar gauges** for heatsink and ambient monitoring
- **Input voltage display** in the top panel
- **Click-to-edit** - click any LCD display to type a value directly
- **Brushed metal chassis** with ventilation slots and realistic styling
- **Secondary LCD row** showing power, set voltage, and set current

## Installation

### Manual Installation

1. Download `opendps-card.js` from this repository
2. Copy the file to your Home Assistant `config/www/` directory
3. Add the resource to your Lovelace configuration:

**Via UI:**
1. Go to Settings -> Dashboards
2. Click the three dots menu -> Resources
3. Add resource:
   - URL: `/local/opendps-card.js`
   - Resource type: JavaScript Module

**Via YAML:**
```yaml
lovelace:
  resources:
    - url: /local/opendps-card.js
      type: module
```

## Configuration

### Basic Configuration

```yaml
type: custom:opendps-card
name: "DPS5005"
entities:
  voltage_out: sensor.opendps_output_voltage
  current_out: sensor.opendps_output_current
  output_switch: switch.opendps_output_enable
```

### Full Configuration

```yaml
type: custom:opendps-card
name: "Lab PSU"
lcd_color: green          # green, amber, blue, or white

entities:
  # Sensors
  voltage_out: sensor.opendps_output_voltage
  current_out: sensor.opendps_output_current
  power_out: sensor.opendps_output_power
  voltage_in: sensor.opendps_input_voltage

  # Controls
  output_switch: switch.opendps_output_enable
  output_enabled: binary_sensor.opendps_output_state
  set_voltage: number.opendps_set_voltage
  set_current: number.opendps_set_current_limit

  # Mode select
  operating_mode: select.opendps_operating_mode

  # Temperature sensors
  temp1: sensor.opendps_heatsink_temperature
  temp2: sensor.opendps_ambient_temperature

# Display options
show_temperatures: true
show_input_voltage: true
show_power: true

# Precision
decimal_places_voltage: 2
decimal_places_current: 3
decimal_places_power: 2

# Knob step sizes
voltage_step: 0.1
current_step: 0.01
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `name` | string | "OpenDPS" | Card title / model label |
| `lcd_color` | string | "green" | LCD color theme: `green`, `amber`, `blue`, `white` |
| `show_temperatures` | boolean | true | Show temperature bar gauges |
| `show_input_voltage` | boolean | true | Show input voltage in top panel |
| `show_power` | boolean | true | Show secondary LCD row (power + setpoints) |
| `voltage_step` | number | 0.1 | Voltage increment per knob/scroll step |
| `current_step` | number | 0.01 | Current increment per knob/scroll step |
| `decimal_places_voltage` | number | 2 | Decimal places for voltage displays |
| `decimal_places_current` | number | 3 | Decimal places for current displays |
| `decimal_places_power` | number | 2 | Decimal places for power display |

### Entity Configuration

| Entity | Type | Required | Description |
|--------|------|----------|-------------|
| `voltage_out` | sensor | Yes | Output voltage sensor |
| `current_out` | sensor | Yes | Output current sensor |
| `output_switch` | switch | Yes | Output enable/disable switch |
| `power_out` | sensor | No | Output power sensor |
| `voltage_in` | sensor | No | Input voltage sensor |
| `output_enabled` | binary_sensor | No | Output state indicator |
| `set_voltage` | number | No | Voltage setpoint control |
| `set_current` | number | No | Current limit control |
| `operating_mode` | select | No | CV/CC/CP mode selector |
| `temp1` | sensor | No | Heatsink temperature |
| `temp2` | sensor | No | Ambient temperature |

## Usage

### Rotary Knobs
- **Click and drag** in a circular motion to adjust voltage or current
- **Scroll wheel** over a knob for fine-step adjustment
- The red indicator line shows the current position

### LCD Displays
- **Click** the main voltage or current LCD to type a value directly
- **Click** the SET V or SET A displays in the secondary row to edit setpoints
- Press **Enter** to confirm, **Escape** to cancel

### Power Button
- **Click** the illuminated power button to toggle the output on/off
- The ring glows green when the output is active

### Mode LEDs
- **CV** (green) - Constant Voltage mode
- **CC** (amber) - Constant Current mode
- **CP** (red) - Constant Power mode
- **OUT** - Output active indicator

## Troubleshooting

### Card not appearing
1. Clear browser cache (Ctrl+Shift+R)
2. Check browser console (F12) for JavaScript errors
3. Verify the resource URL is `/local/opendps-card.js`
4. Ensure the file is in your HA `config/www/` directory

### Entities show dashes
1. Check entity IDs in Developer Tools -> States
2. Verify your ESPHome device is online and connected
3. Entity names depend on your ESPHome YAML configuration

### Knobs not responding
1. Make sure `set_voltage` and `set_current` entities are configured
2. These must be `number` entities (not sensors)

## License

MIT License
