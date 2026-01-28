# OpenDPS Custom Lovelace Card

A feature-rich Home Assistant dashboard card for controlling and monitoring OpenDPS power supplies via ESPHome integration.

![OpenDPS Card Preview](preview.png)

## Features

- Real-time voltage, current, and power display with configurable decimal places
- Output enable/disable control with visual status indicator
- Voltage and current setpoint adjustment
- Temperature monitoring (heatsink and ambient)
- Input voltage display
- Calibration controls (optional)
- **Light/Dark theme support** (auto-detect or manual override)
- Compact mode for smaller displays
- Fully customizable colors

## Installation

### HACS (Recommended)

1. Open HACS in Home Assistant
2. Go to "Frontend" section
3. Click the three dots menu and select "Custom repositories"
4. Add this repository URL and select "Lovelace" as category
5. Search for "OpenDPS Card" and install

### Manual Installation

1. Download `opendps-card.js` from this repository
2. Copy the file to your Home Assistant `config/www/` directory
3. Add the resource to your Lovelace configuration:

**Via UI:**
1. Go to Settings → Dashboards
2. Click the three dots menu → Resources
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
  power_out: sensor.opendps_output_power
  output_switch: switch.opendps_output_enable
  set_voltage: number.opendps_set_voltage
  set_current: number.opendps_set_current_limit
```

### Full Configuration

```yaml
type: custom:opendps-card
name: "My Power Supply"
theme: auto  # auto, light, or dark

entities:
  # Sensors (required for display)
  voltage_in: sensor.opendps_input_voltage
  voltage_out: sensor.opendps_output_voltage
  current_out: sensor.opendps_output_current
  power_out: sensor.opendps_output_power

  # Binary sensor for output state
  output_enabled: binary_sensor.opendps_output_state

  # Controls
  set_voltage: number.opendps_set_voltage
  set_current: number.opendps_set_current_limit
  output_switch: switch.opendps_output_enable

  # Temperature sensors (optional)
  temp1: sensor.opendps_heatsink_temperature
  temp2: sensor.opendps_ambient_temperature

  # Calibration buttons (optional)
  request_calibration: button.opendps_request_calibration_report
  save_calibration: button.opendps_save_calibration_to_storage
  restore_calibration: button.opendps_restore_calibration_from_storage
  clear_calibration: button.opendps_clear_all_calibration

  # Firmware upgrade (optional)
  firmware_status: text_sensor.opendps_upgrade_status
  firmware_progress: sensor.opendps_upgrade_progress
  start_upgrade: button.opendps_start_firmware_upgrade
  cancel_upgrade: button.opendps_cancel_firmware_upgrade
  switch_to_bootloader: button.opendps_switch_to_bootloader

# Display options
show_calibration: false
show_firmware: false
show_temperatures: true
show_input_voltage: true
compact: false

# Decimal places
decimal_places_voltage: 2
decimal_places_current: 3
decimal_places_power: 2

# Custom colors (optional)
colors:
  voltage: "#4CAF50"    # Green
  current: "#2196F3"    # Blue
  power: "#FF9800"      # Orange
  temperature: "#F44336" # Red
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `name` | string | "OpenDPS" | Card title |
| `theme` | string | "auto" | Theme mode: `auto`, `light`, or `dark` |
| `show_calibration` | boolean | false | Show calibration controls (collapsible) |
| `show_firmware` | boolean | false | Show firmware upgrade controls (collapsible) |
| `show_temperatures` | boolean | true | Show temperature readings |
| `show_input_voltage` | boolean | true | Show input voltage |
| `compact` | boolean | false | Use compact layout |
| `decimal_places_voltage` | number | 2 | Decimal places for voltage |
| `decimal_places_current` | number | 3 | Decimal places for current |
| `decimal_places_power` | number | 2 | Decimal places for power |
| `colors` | object | (defaults) | Custom accent colors |

### Entity Configuration

| Entity | Type | Required | Description |
|--------|------|----------|-------------|
| `voltage_out` | sensor | Yes | Output voltage sensor |
| `current_out` | sensor | Yes | Output current sensor |
| `power_out` | sensor | No | Output power sensor |
| `voltage_in` | sensor | No | Input voltage sensor |
| `output_switch` | switch | No | Output enable switch |
| `output_enabled` | binary_sensor | No | Output state sensor |
| `set_voltage` | number | No | Voltage setpoint control |
| `set_current` | number | No | Current limit control |
| `temp1` | sensor | No | Heatsink temperature |
| `temp2` | sensor | No | Ambient temperature |
| `request_calibration` | button | No | Request calibration report |
| `save_calibration` | button | No | Save calibration to storage |
| `restore_calibration` | button | No | Restore calibration from storage |
| `clear_calibration` | button | No | Clear all calibration |
| `firmware_status` | text_sensor | No | Firmware upgrade status |
| `firmware_progress` | sensor | No | Firmware upgrade progress (0-100%) |
| `start_upgrade` | button | No | Start firmware upgrade |
| `cancel_upgrade` | button | No | Cancel firmware upgrade |
| `switch_to_bootloader` | button | No | Switch DPS to bootloader mode |

## Theme Support

The card automatically detects your Home Assistant theme (light/dark) and adjusts its appearance accordingly. You can also force a specific theme:

```yaml
type: custom:opendps-card
theme: dark  # Force dark mode
# ...
```

### Custom Color Scheme

You can customize the accent colors to match your dashboard:

```yaml
colors:
  voltage: "#00BCD4"   # Cyan for voltage
  current: "#9C27B0"   # Purple for current
  power: "#FFEB3B"     # Yellow for power
  temperature: "#E91E63" # Pink for temperature
```

## Examples

### Minimal Card

```yaml
type: custom:opendps-card
entities:
  voltage_out: sensor.opendps_output_voltage
  current_out: sensor.opendps_output_current
  output_switch: switch.opendps_output_enable
```

### Compact Card for Sidebar

```yaml
type: custom:opendps-card
name: "Lab PSU"
compact: true
show_temperatures: false
show_input_voltage: false
entities:
  voltage_out: sensor.opendps_output_voltage
  current_out: sensor.opendps_output_current
  power_out: sensor.opendps_output_power
  output_switch: switch.opendps_output_enable
  set_voltage: number.opendps_set_voltage
  set_current: number.opendps_set_current_limit
```

### Full-Featured Card with Calibration

```yaml
type: custom:opendps-card
name: "DPS5005 Bench Supply"
theme: auto
show_calibration: true
show_temperatures: true
decimal_places_voltage: 3
decimal_places_current: 4
entities:
  voltage_in: sensor.opendps_input_voltage
  voltage_out: sensor.opendps_output_voltage
  current_out: sensor.opendps_output_current
  power_out: sensor.opendps_output_power
  output_enabled: binary_sensor.opendps_output_state
  set_voltage: number.opendps_set_voltage
  set_current: number.opendps_set_current_limit
  output_switch: switch.opendps_output_enable
  temp1: sensor.opendps_heatsink_temperature
  temp2: sensor.opendps_ambient_temperature
  request_calibration: button.request_calibration_report
  save_calibration: button.save_calibration_to_storage
  restore_calibration: button.restore_calibration_from_storage
colors:
  voltage: "#66BB6A"
  current: "#42A5F5"
```

## Troubleshooting

### Card not showing up

1. Clear your browser cache
2. Check browser console for JavaScript errors
3. Verify the resource URL is correct
4. Make sure the file is in the `www` directory

### Entities not found

1. Verify entity IDs in Developer Tools → States
2. Check that your ESPHome device is online
3. Entity names may vary based on your ESPHome configuration

### Theme not auto-detecting

The auto theme detection relies on Home Assistant's theme settings. If it's not working:
1. Try setting a theme manually in HA
2. Use the explicit `theme: light` or `theme: dark` option

## License

MIT License - feel free to use and modify as needed.

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.
