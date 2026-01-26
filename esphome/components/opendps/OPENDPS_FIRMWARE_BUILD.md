# OpenDPS Firmware Build & Flash Guide

This document covers building OpenDPS firmware with custom settings (like 115200 baud) and safely flashing via the ESPHome OpenDPS component.

## Hardware Overview

- **MCU**: STM32F100 (Value Line, 24 MHz max clock)
- **USART1**: Connected to APB2 @ 24 MHz
- **Default baudrate**: 9600 (configurable at build time)
- **Max reliable baudrate**: 460800 (theoretical 1.5 Mbaud)

## Safety Mechanisms - How to Avoid Bricking

### 1. Bootloader Recovery (SEL Button)
**CRITICAL SAFETY FEATURE**: If you flash bad firmware, hold the **SEL button** while powering on the device. This forces the bootloader into upgrade mode.

From `dpsboot/hw.c`:
```c
bool hw_check_forced_upgrade(void)
{
    return gpio_get(BUTTON_SEL_PORT, BUTTON_SEL_PIN) != BUTTON_SEL_PIN;
}
```

### 2. Bootloader Architecture
The OpenDPS has a **two-stage boot**:
1. **dpsboot** (bootloader) - Lives in protected flash, handles upgrades
2. **opendps** (application) - The main firmware you're upgrading

The bootloader is **never overwritten** during OTA upgrades - only the app partition is updated.

### 3. Upgrade Flow (from protocol.h)
```
1. Host sends: [cmd_upgrade_start] [chunk_size:16] [crc:16]
2. App writes magic + chunk_size + crc to bootcom RAM
3. App triggers system reset
4. Bootloader detects magic, enters upgrade mode
5. Bootloader acks with: [cmd_response | cmd_upgrade_start] [status] [chunk_size:16] [reason:8]
6. Host sends firmware in chunks: [cmd_upgrade_data] [payload]+
7. Bootloader writes each chunk to flash, acks with status
8. Final chunk (smaller than chunk_size) triggers CRC verification
9. On success: bootloader clears upgrade flag, boots new app
```

### 4. Upgrade Status Codes
```c
typedef enum {
    upgrade_continue = 0,      // Device ready for more data
    upgrade_bootcom_error,     // Bootcom data corrupted
    upgrade_crc_error,         // CRC verification failed
    upgrade_erase_error,       // Flash erase failed
    upgrade_flash_error,       // Flash write failed
    upgrade_overflow_error,    // Firmware too large
    upgrade_protocol_error,    // Data received without start
    upgrade_success = 16       // Upgrade complete, CRC verified
} upgrade_status_t;
```

### 5. Bootloader Entry Reasons
```c
typedef enum {
    reason_unknown = 0,        // Unknown
    reason_forced,             // User held SEL button
    reason_past_failure,       // Parameter storage init failed
    reason_bootcom,            // App requested upgrade
    reason_unfinished_upgrade, // Previous upgrade incomplete
    reason_app_start_failed    // App failed to start
} upgrade_reason_t;
```

## Building OpenDPS Firmware

### Prerequisites
```bash
# Install ARM toolchain
sudo apt-get install gcc-arm-none-eabi

# Clone repository (if not already)
git clone --recursive https://github.com/kanflo/opendps.git
cd opendps

# Build libopencm3 (only needed once)
make -C libopencm3
```

### Build Commands

**Build with default 9600 baud:**
```bash
make -C opendps clean
make -C opendps bin
# Output: opendps/opendps_DPS5005.bin
```

**Build with 115200 baud (RECOMMENDED):**
```bash
make -C opendps clean
make -C opendps bin BAUDRATE=115200
# Output: opendps/opendps_DPS5005.bin
```

**Build for different DPS models:**
```bash
# DPS5005 (default, 5A max)
make -C opendps bin BAUDRATE=115200 MODEL=DPS5005

# DPS5015 (15A max)
make -C opendps bin BAUDRATE=115200 MODEL=DPS5015

# DPS5020 (20A max)
make -C opendps bin BAUDRATE=115200 MODEL=DPS5020

# DPS3005 (5A max, 30V)
make -C opendps bin BAUDRATE=115200 MODEL=DPS3005
```

**Full build options:**
```bash
make -C opendps bin \
    BAUDRATE=115200 \
    MODEL=DPS5005 \
    WIFI=1 \
    DEBUG=0 \
    CC_ENABLE=1 \
    CV_ENABLE=1 \
    CL_ENABLE=1 \
    FUNCGEN_ENABLE=1
```

### IMPORTANT: Bootloader Baudrate
**The bootloader MUST be built with the SAME baudrate as the app!**

If you change baudrate, you need to reflash the bootloader via SWD/JTAG (direct flash method):
```bash
# Build bootloader with matching baudrate
make -C dpsboot clean
make -C dpsboot bin BAUDRATE=115200

# Flash via SWD (requires physical access and STLink)
make -C dpsboot flash BAUDRATE=115200
```

**However**, if your bootloader is already at 9600 baud:
- You can upgrade to new app firmware at 9600 baud
- The new app will use the new baudrate (115200)
- BUT recovery via SEL button will still use 9600 (bootloader baudrate)

**Safest approach**: Keep bootloader at 9600, upgrade app to 115200. If something goes wrong, SEL button recovery still works at 9600.

## Firmware Upgrade via ESPHome

### Pre-flight Checklist
1. ✅ Firmware file is valid (magic byte at offset 0x06 should be 0x20)
2. ✅ Communication is working (test with ping/query first)
3. ✅ Firmware file is accessible via storage component
4. ✅ Know the SEL button recovery procedure

### Upgrade Process
```yaml
# In ESPHome config
button:
  - platform: template
    name: "OpenDPS Upgrade Firmware"
    on_press:
      - lambda: |-
          id(my_opendps).start_firmware_upgrade("/usb/opendps_DPS5005.bin");
```

### Monitoring Progress
The upgrade function logs progress. Watch the ESPHome logs for:
- `Starting firmware upgrade from: ...`
- `Firmware size: X bytes`
- `Sending upgrade start command`
- Status updates for each chunk
- `upgrade_success` when complete

### If Upgrade Fails

1. **CRC Error**: Firmware file may be corrupted, re-download
2. **Flash Error**: Hardware issue or power glitch, retry
3. **Overflow Error**: Firmware too large for flash
4. **Black Screen**:
   - Wait 30 seconds
   - If still black: hold SEL button, power cycle
   - Bootloader will enter upgrade mode
   - Retry with dpsctl.py at bootloader baudrate (9600)

### Emergency Recovery via dpsctl.py
```bash
# If device is stuck, use original tool with SEL button held
cd opendps
python3 dpsctl/dpsctl.py -d /dev/ttyUSB0 -U opendps/opendps_DPS5005.bin
```

## Frame Protocol Reference

### Frame Format (uframe)
```
[SOF 0x7E] [escaped payload] [CRC16-CCITT] [EOF 0x7F]
```

### Byte Escaping
- SOF (0x7E), DLE (0x7D), EOF (0x7F) in payload are escaped
- Escape: [DLE 0x7D] [byte XOR 0x20]

### CRC-16 CCITT
- Polynomial: 0x1021
- Initial value: 0x0000
- Applied to unescaped payload

### Chunk Size
- Default: 1024 bytes
- Max: 2048 bytes (MAX_CHUNK_SIZE in bootloader)
- Device may negotiate smaller size

## File Locations

After building, firmware files are at:
```
opendps/opendps_DPS5005.bin   # Main firmware binary
opendps/opendps_DPS5005.elf   # Debug symbols
dpsboot/dpsboot.bin           # Bootloader binary
```

## Version Verification

After upgrade, query the version to confirm:
```yaml
button:
  - platform: template
    name: "Query Version"
    on_press:
      - lambda: id(my_opendps).request_version();
```

The version will appear in logs as the git tag/hash used during build.

## ESPHome Component Configuration Examples

### Basic Configuration

```yaml
uart:
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 115200

opendps:
  id: my_opendps
  update_interval: 500ms
  default_brightness: 50

  # TCP bridge for dpsctl.py access (optional)
  tcp_bridge:
    enabled: true
    port: 5005

  # Sensors
  voltage_in:
    name: "Input Voltage"
  voltage_out:
    name: "Output Voltage"
  current_out:
    name: "Output Current"
  power_out:
    name: "Output Power"
  temperature1:
    name: "Temperature 1"
  temperature2:
    name: "Temperature 2"
  output_enabled:
    name: "Output Enabled"

  # Actions on connect
  on_connect:
    - logger.log: "OpenDPS connected!"
    - opendps.set_brightness:
        id: my_opendps
        brightness: 50
```

### Control Actions

```yaml
button:
  # Enable/disable output
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

  # Set voltage and current
  - platform: template
    name: "Set 5V 1A"
    on_press:
      - opendps.set_voltage:
          id: my_opendps
          voltage: 5.0
      - opendps.set_current:
          id: my_opendps
          current: 1.0

  # Lock/unlock front panel
  - platform: template
    name: "Lock Panel"
    on_press:
      - opendps.lock:
          id: my_opendps
          locked: true

  # Set brightness
  - platform: template
    name: "Set Brightness 75%"
    on_press:
      - opendps.set_brightness:
          id: my_opendps
          brightness: 75

  # Ping device
  - platform: template
    name: "Ping"
    on_press:
      - opendps.ping:
          id: my_opendps

  # Request version info
  - platform: template
    name: "Get Version"
    on_press:
      - opendps.request_version:
          id: my_opendps
```

### Number Controls for Voltage/Current

```yaml
number:
  - platform: template
    name: "Set Voltage"
    min_value: 0
    max_value: 50
    step: 0.1
    unit_of_measurement: "V"
    set_action:
      - opendps.set_voltage:
          id: my_opendps
          voltage: !lambda return x;

  - platform: template
    name: "Set Current"
    min_value: 0
    max_value: 5
    step: 0.01
    unit_of_measurement: "A"
    set_action:
      - opendps.set_current:
          id: my_opendps
          current: !lambda return x;

  - platform: template
    name: "Set Brightness"
    min_value: 0
    max_value: 100
    step: 1
    unit_of_measurement: "%"
    set_action:
      - opendps.set_brightness:
          id: my_opendps
          brightness: !lambda return (uint8_t)x;
```

### Function and Parameter Control

```yaml
# Select different operating functions
select:
  - platform: template
    name: "Function"
    options:
      - "cv"   # Constant Voltage
      - "cc"   # Constant Current
      - "cl"   # Current Limit
    set_action:
      - opendps.set_function:
          id: my_opendps
          function: !lambda return x;

button:
  # Set a specific parameter
  - platform: template
    name: "Set OVP to 55V"
    on_press:
      - opendps.set_parameter:
          id: my_opendps
          key: "ovp"
          value: "55000"  # Value in mV
```

## Calibration

### Request Calibration Data

```yaml
button:
  - platform: template
    name: "Request Calibration"
    on_press:
      - opendps.request_calibration_report:
          id: my_opendps
```

### Manual Calibration (Set Individual Values)

```yaml
button:
  # Set a specific calibration coefficient
  - platform: template
    name: "Set VIN_ADC_K"
    on_press:
      - opendps.set_calibration:
          id: my_opendps
          name: "vin_adc_k"
          value: 16.37

  # Clear all calibration (restore defaults)
  - platform: template
    name: "Clear Calibration"
    on_press:
      - opendps.clear_calibration:
          id: my_opendps
```

Available calibration coefficients:
- `vin_adc_k`, `vin_adc_c` - Input voltage ADC
- `v_adc_k`, `v_adc_c` - Output voltage ADC
- `v_dac_k`, `v_dac_c` - Output voltage DAC
- `a_adc_k`, `a_adc_c` - Output current ADC
- `a_dac_k`, `a_dac_c` - Output current DAC

### Calibration Assistant

The calibration assistant provides a step-by-step guided calibration process similar to `dpsctl.py -C`. It walks through calibrating input voltage, output voltage, output current, and current limit.

**Requirements:**
- A calibrated multimeter
- A known load resistor (for current calibration, optional)
- Your fixed DC power supply (typically 48V for DPS modules)

**How it works:**
The OpenDPS is typically powered by a single fixed DC power supply (e.g., 48V). Before starting calibration, measure your actual input voltage with a multimeter and provide this value. The calibration assistant uses this single-point measurement along with the ADC reading to calibrate the input voltage sensor.

```yaml
# Global to store measured value for calibration steps
globals:
  - id: measured_value
    type: float
    initial_value: "0.0"

number:
  # Input for measured values during calibration
  - platform: template
    id: calibration_measurement
    name: "Calibration Measurement"
    min_value: 0
    max_value: 100000
    step: 0.1
    unit_of_measurement: "mV or mA"
    set_action:
      - globals.set:
          id: measured_value
          value: !lambda return x;

button:
  # Start calibration assistant
  # Measure your input voltage with a multimeter first!
  - platform: template
    name: "Start Calibration"
    on_press:
      - opendps.start_calibration_assistant:
          id: my_opendps
          vin_measured_mv: 48000  # Your measured input voltage in mV (e.g., 48V)
          load_resistance: 10.0   # Load resistor in ohms (0 to skip current cal)
          load_max_wattage: 50    # Load resistor max power in watts
          max_dps_current: 5.0    # DPS model max current (5A for DPS5005)

  # Advance to next calibration step (provide measured value)
  - platform: template
    name: "Submit Measurement"
    on_press:
      - opendps.calibration_assistant_step:
          id: my_opendps
          measured_value: !lambda return id(measured_value);

  # Cancel calibration if needed
  - platform: template
    name: "Cancel Calibration"
    on_press:
      - opendps.cancel_calibration_assistant:
          id: my_opendps
```

**Calibration Workflow:**

1. Measure your DC input voltage with a calibrated multimeter
2. Press "Start Calibration" with your measured input voltage - watch logs for instructions
3. The assistant will guide you through each step:
   - **Input Voltage Calibration**: Uses your measured input voltage + ADC reading
   - **Output Voltage Calibration**: Measure output at two voltage points (10% and 90%)
   - **Output Current Calibration** (if load provided): Measure current through load
   - **Current Limit Calibration**: Short output, measure current at limit
4. At each step, enter the measured value and press "Submit Measurement"
5. Watch the logs for prompts and results
6. Calibration coefficients are automatically saved to the DPS

**Example Log Output:**
```
[I][opendps:xxx]: ========================================
[I][opendps:xxx]: CALIBRATION ASSISTANT STARTED
[I][opendps:xxx]: ========================================
[I][opendps:xxx]: Parameters:
[I][opendps:xxx]:   Vin Measured: 48000 mV
[I][opendps:xxx]:   Load Resistance: 10.00 ohm
[I][opendps:xxx]: ----------------------------------------
[I][opendps:xxx]: STEP 1: Input Voltage Calibration
[I][opendps:xxx]: Using measured input voltage: 48000 mV
[I][opendps:xxx]: Reading ADC value from DPS...
```

### Home Assistant Integration Example

```yaml
# Complete example with Home Assistant controls
esphome:
  name: opendps-controller

esp32:
  board: esp32dev

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_key

logger:

uart:
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 115200

opendps:
  id: dps
  voltage_in:
    name: "DPS Input Voltage"
  voltage_out:
    name: "DPS Output Voltage"
  current_out:
    name: "DPS Output Current"
  power_out:
    name: "DPS Output Power"
  output_enabled:
    name: "DPS Output Active"

switch:
  - platform: template
    name: "DPS Output"
    lambda: return id(dps).get_data().output_enabled;
    turn_on_action:
      - opendps.enable_output:
          id: dps
          enable: true
    turn_off_action:
      - opendps.enable_output:
          id: dps
          enable: false

number:
  - platform: template
    name: "DPS Voltage Setpoint"
    min_value: 0
    max_value: 50
    step: 0.1
    unit_of_measurement: "V"
    lambda: return id(dps).get_voltage_setting();
    set_action:
      - opendps.set_voltage:
          id: dps
          voltage: !lambda return x;

  - platform: template
    name: "DPS Current Limit"
    min_value: 0
    max_value: 5
    step: 0.01
    unit_of_measurement: "A"
    lambda: return id(dps).get_current_setting();
    set_action:
      - opendps.set_current:
          id: dps
          current: !lambda return x;
```
