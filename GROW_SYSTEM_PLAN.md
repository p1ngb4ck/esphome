# 🌱 Multi-Tent Grow Control System - Master Plan

**Project:** Distributed grow tent control & monitoring system using ESPHome
**Author:** p1ngb4ck
**Created:** 2024-12-28
**Status:** Planning Phase

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [System Architecture](#system-architecture)
3. [Hardware Specifications](#hardware-specifications)
4. [Network Architecture](#network-architecture)
5. [Software Architecture](#software-architecture)
6. [User Interface Design](#user-interface-design)
7. [Control & Automation Strategy](#control--automation-strategy)
8. [Data Logging & Analysis](#data-logging--analysis)
9. [Safety & Fail-Safe Mechanisms](#safety--fail-safe-mechanisms)
10. [Enclosure Design](#enclosure-design)
11. [Implementation Phases](#implementation-phases)
12. [Bill of Materials](#bill-of-materials)
13. [Session Notes](#session-notes)

---

## Project Overview

### Goals

**Primary Objectives:**
- Control multiple grow tents independently with local and centralized management
- Real-time environmental monitoring (temp, humidity, light, VPD)
- Hydroponic system control (pH, EC, pumps, reservoir management)
- Reliable operation in high-humidity environments
- Local control when network/HA is unavailable
- Data logging and trend analysis
- Integration with Home Assistant for automation and remote access

**Design Principles:**
- **Reliability First:** System continues operating if master or HA fails
- **Local Control:** Each tent has autonomous operation and local UI
- **Scalability:** Easy to add more tents without redesigning
- **Maintainability:** Modular design with shared configuration packages
- **Safety:** Multiple fail-safe mechanisms for critical failures

### Available Hardware

**Microcontrollers:**
- ESP32-P4 (for master controller)
- ESP32-S3 N16R8 boards (16MB Flash, 8MB PSRAM)
- ESP32-WROOM-32 boards

**Displays:**
- 1x Large capacitive touch (7" 1024x600 or similar) for ESP32-P4
- 1x 480x320 RGB TFT (resistive/capacitive touch)
- 1x 320x280 color LCD
- 4x 0.96" I2C OLED (white on black)

**Networking:**
- SPI Ethernet modules (W5500)
- WiFi (built-in, backup only)

**Relays & Control:**
- SSR 8A (230V AC) for high-power loads (lighting)
- Songle relays (typical blue ones, 2A) for fans/pumps
- TLC5947 (24-channel PWM) for dimming control

**Sensors:**
- Temperature: DHT11, DHT22, DS18B20, BME280
- Humidity/Pressure: DHT22, BME280, BMP180
- Light: BH1750 (lux)
- pH sensors (analog, via ADS1115)
- EC/TDS sensors (analog, via ADS1115)
- ADS1115 (I2C ADC for analog sensors)

**Storage:**
- SD card modules (SPI)
- USB storage (ESP32-S3 capable)

**Input Devices:**
- Potentiometers
- Rotary encoders with push-button
- Touch screen input

**Power:**
- Step-down converters
- Mini 5V 2A AC-DC charger PCBs

---

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    HOME ASSISTANT (Cloud)                    │
│                  - Automation & Scheduling                   │
│                  - Data Visualization                        │
│                  - Remote Access                             │
└──────────────────────────┬──────────────────────────────────┘
                           │ WiFi/Ethernet
                           ▼
┌─────────────────────────────────────────────────────────────┐
│           MASTER CONTROLLER (ESP32-P4)                       │
│  - Large 7" capacitive touchscreen (1024x600)               │
│  - Ethernet (SPI W5500) - reliable communication            │
│  - Aggregated data logging (SD card via SPI)                │
│  - Cross-tent automation coordination                       │
│  - System-wide settings & overview                          │
│  - Central location (outside tents)                          │
└──────────────┬───────────┬───────────┬──────────────────────┘
               │           │           │
        Wired Ethernet (CAT5e/CAT6)
               │           │           │
       ┌───────▼───┐   ┌──▼──────┐  ┌▼──────────┐
       │ TENT 1    │   │ TENT 2  │  │ TENT 3    │
       │ ESP32     │   │ ESP32   │  │ ESP32-S3  │
       │ 320x280   │   │ 320x280 │  │ 480x320   │
       │ Soil/Coco │   │ Flower  │  │ HYDRO DWC │
       └───────────┘   └─────────┘  └───────────┘
```

### Why This Architecture?

**Distributed Master-Controller Design:**
- **Master (ESP32-P4):** Central monitoring and coordination, outside tents
- **Tent Controllers (ESP32/ESP32-S3):** Autonomous operation, inside each tent
- **Home Assistant:** Cloud automation, historical data, remote access

**Benefits:**
1. ✅ **Resilience:** Each tent operates autonomously if master/HA fails
2. ✅ **Responsiveness:** Local sensors → local control (no network delay)
3. ✅ **Scalability:** Add tents without impacting existing ones
4. ✅ **Reliability:** Wired Ethernet eliminates WiFi issues in humid environment
5. ✅ **Local UI:** Check/adjust tent without smartphone/HA access

---

## Hardware Specifications

### Master Controller (ESP32-P4)

**MCU:** ESP32-P4
**Purpose:** Central command & monitoring station
**Location:** Outside tents (dry, accessible area)

**Key Components:**

| Component | Model/Type | Interface | Purpose |
|-----------|------------|-----------|---------|
| Display | 7" 1024x600 capacitive touch | MIPI-DSI | System overview UI |
| Network | W5500 Ethernet module | SPI | Wired connection to tents |
| Storage | SD card (32GB+) | SPI/SDMMC | Data logging |
| Ambient Sensors (optional) | BME280, BH1750 | I2C | Room conditions |
| Alert | Buzzer | GPIO | Critical alerts |

**Pin Allocation (ESP32-P4):**
```
MIPI-DSI: Hardware interface (display)
SPI0: Ethernet (W5500)
SPI1: SD card
I2C: Sensors (BME280, BH1750)
GPIO: Touch interrupt, buzzer
USB-C: Programming & power
```

**Why ESP32-P4 for Master?**
- ✅ Hardware MIPI-DSI (no bit-banging RGB, better performance)
- ✅ Dual-core RISC-V up to 800MHz (smooth LVGL UI)
- ✅ Hardware JPEG decoder (future: camera feeds)
- ✅ Better multitasking (UI + network + logging)
- ✅ More RAM for larger LVGL canvases

---

### Hydro Tent Controller (ESP32-S3 N16R8)

**MCU:** ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM)
**Purpose:** Complex hydroponic (DWC) system control
**Location:** Inside hydro tent

**Key Components:**

| Component | Model/Type | Interface | Purpose |
|-----------|------------|-----------|---------|
| Display | 480x320 RGB TFT touch | Parallel RGB | Detailed hydro UI |
| Network | W5500 Ethernet module | SPI | Connection to master |
| Input | Rotary encoder + button | GPIO | Menu navigation |
| Relays | 2x SSR 8A | GPIO | Lights (main + supplemental) |
| Relays | 4x Songle 2A | GPIO | Fans, pumps, heater, humidifier |
| PWM Controller | TLC5947 | SPI | Lamp dimming, fan speed |
| ADC | ADS1115 | I2C | pH, EC, analog sensors |
| Temp/Humidity | BME280 or DHT22 | I2C / 1-Wire | Air conditions (canopy) |
| Water Temp | DS18B20 | 1-Wire | Reservoir temperature |
| Light Sensor | BH1750 | I2C | PAR approximation |
| pH Probe | Analog pH sensor | ADS1115 Ch0 | Nutrient pH |
| EC Probe | Analog EC/TDS sensor | ADS1115 Ch1 | Nutrient strength |
| Water Level | Float switch / ultrasonic | GPIO / Trigger+Echo | Reservoir level |

**Hydro-Specific Relay Configuration:**
```
SSR1: Main grow light (high current)
SSR2: Supplemental light (high current)
Relay1: Circulation fan
Relay2: Exhaust fan with carbon filter
Relay3: Feed pump (nutrient solution)
Relay4: Air pump (oxygenation)
Optional Relay5: Drain pump
Optional Relay6: Heater
Optional Relay7: Humidifier/Dehumidifier
```

**Why ESP32-S3 for Hydro?**
- ✅ PSRAM for buffering sensor data (local graphs)
- ✅ More processing power for PID control (future pH/EC dosing)
- ✅ Larger display needed for pH/EC trends
- ✅ More sensors = need more I/O and ADC channels
- ✅ Complex UI with real-time graphs

---

### Standard Tent Controller (ESP32-WROOM-32)

**MCU:** ESP32-WROOM-32 (4MB Flash)
**Purpose:** Soil/coco grow tents with simpler needs
**Location:** Inside each standard tent

**Key Components:**

| Component | Model/Type | Interface | Purpose |
|-----------|------------|-----------|---------|
| Display | 320x280 color LCD | SPI | Basic tent status UI |
| Network | W5500 Ethernet module | SPI | Connection to master |
| Input | Rotary encoder + button | GPIO | Menu navigation |
| Relays | 2x SSR 8A | GPIO | Lights |
| Relays | 3x Songle 2A | GPIO | Circulation fan, exhaust, pump |
| PWM Controller | TLC5947 (shared SPI) | SPI | Lamp dimming |
| Temp/Humidity | BME280 or DHT22 | I2C / 1-Wire | Air conditions |
| Canopy Temp | DS18B20 | 1-Wire | Plant temperature |
| Light Sensor | BH1750 | I2C | Light intensity |
| Soil Moisture (optional) | Capacitive sensor | ADC | Watering automation |

**Standard Tent Relay Configuration:**
```
SSR1: Main grow light
SSR2: Supplemental light (optional)
Relay1: Circulation fan
Relay2: Exhaust fan
Relay3: Watering pump (if automated)
```

**Why ESP32-WROOM for Standard Tents?**
- ✅ Cost-effective (simpler tents don't need expensive hardware)
- ✅ Sufficient I/O for basic sensors + relays
- ✅ Proven reliability
- ✅ Lower power consumption
- ✅ Easier to troubleshoot

---

## Network Architecture

### Topology

```
        [Router + Home Assistant]
                 │
                WiFi (backup/setup only)
                 │
        ┌────────▼────────┐
        │  MASTER P4      │ ← Central control (outside tents)
        │  IP: 10.0.1.10  │
        └────────┬────────┘
                 │
         Gigabit Ethernet Switch
         (5-port minimum)
            /    |    \    \
    ┌──────▼┐  ┌▼──────┐ ┌▼──────┐ ┌▼──────┐
    │TENT 1 │  │TENT 2 │ │TENT 3 │ │TENT 4 │
    │.11    │  │.12    │ │.13    │ │.14    │
    │ESP32  │  │ESP32  │ │ESP32-S3│ │ESP32  │
    └───────┘  └───────┘ └───────┘ └───────┘
```

### IP Addressing Scheme

**Network:** 10.0.1.0/24
**Gateway:** 10.0.1.1 (Router)
**DNS:** 10.0.1.1

| Device | IP Address | Hostname | MAC (example) |
|--------|------------|----------|---------------|
| Master Controller | 10.0.1.10 | grow-master | (set via config) |
| Tent 1 (Veg/Soil) | 10.0.1.11 | grow-tent1 | (set via config) |
| Tent 2 (Flower) | 10.0.1.12 | grow-tent2 | (set via config) |
| Tent 3 (Hydro) | 10.0.1.13 | grow-tent3-hydro | (set via config) |
| Tent 4 (Seedlings) | 10.0.1.14 | grow-tent4 | (set via config) |

**Static IP Configuration:** All devices use static IPs (configured in ESPHome YAML)

### Why Ethernet Over WiFi?

**Advantages:**
1. ✅ **Reliability:** No dropouts in high-humidity environments
2. ✅ **No interference:** 2.4GHz congestion common in homes
3. ✅ **Deterministic latency:** Critical for real-time control
4. ✅ **Longer range:** Up to 100m per segment vs WiFi ~30m
5. ✅ **Lower power:** No WiFi power management issues
6. ✅ **Sealed enclosures:** Better for IP65 waterproofing

**Cable Requirements:**
- **Type:** Outdoor-rated CAT6 (or CAT5e minimum)
- **Length:** Measure tent-to-switch distance + 20% slack
- **Connector:** RJ45 crimped or pre-made
- **Routing:** Cable glands through tent fabric, drip loops

### Communication Protocols

**Primary:** ESPHome Native API over Ethernet
**Fallback:** MQTT (if needed for non-ESPHome integrations)
**Discovery:** mDNS disabled (static IPs, no broadcast storms)

**Message Flow:**
```
Tent Sensor Update → Tent Controller → Master Controller → Home Assistant
                                     ↓
                                 SD Card Log
```

---

## Software Architecture

### Configuration Structure

**Directory Layout:**
```
esphome/
├── common/
│   ├── base.yaml              # Base config (logger, api, ota)
│   ├── ethernet.yaml          # W5500 Ethernet config
│   ├── sensors_environment.yaml  # BME280, BH1750 templates
│   ├── sensors_hydro.yaml     # pH, EC sensor templates
│   ├── relays.yaml            # Relay control templates
│   └── vpd_calculation.yaml   # VPD calculation lambda
├── master/
│   └── grow-master.yaml       # ESP32-P4 master controller
├── tents/
│   ├── grow-tent1.yaml        # Tent 1 (standard)
│   ├── grow-tent2.yaml        # Tent 2 (standard)
│   ├── grow-tent3-hydro.yaml # Tent 3 (hydro, S3)
│   └── grow-tent4.yaml        # Tent 4 (standard)
└── secrets.yaml               # WiFi passwords, API keys
```

### Shared Packages (common/)

**Purpose:** Reusable configuration blocks to avoid duplication

**base.yaml:**
- ESPHome platform configuration
- Logger settings
- API configuration
- OTA updates
- Watchdog timer

**ethernet.yaml:**
- W5500 SPI Ethernet configuration
- Static IP settings
- mDNS configuration

**sensors_environment.yaml:**
- BME280 (temp, humidity, pressure)
- BH1750 (lux)
- DS18B20 (temperature)
- VPD calculation template

**sensors_hydro.yaml:**
- ADS1115 configuration
- pH sensor calibration
- EC/TDS sensor calibration
- Water level sensor

**relays.yaml:**
- SSR control templates
- Songle relay templates
- Interlock logic (prevent conflicting states)

### Configuration Inheritance

**Example: Standard Tent Controller**

```yaml
# grow-tent1.yaml
substitutions:
  device_name: grow-tent1
  friendly_name: "Tent 1 - Vegetative"
  tent_id: "1"
  static_ip: 10.0.1.11

packages:
  base: !include common/base.yaml
  ethernet: !include common/ethernet.yaml
  sensors_env: !include common/sensors_environment.yaml
  relays: !include common/relays.yaml

# Tent-specific overrides and additions
esp32:
  board: esp32dev

# Custom sensors, UI, automations...
```

### State Management

**Each Controller Tracks:**
- Current environmental conditions (temp, humidity, VPD)
- Light state and intensity
- Fan speeds
- Relay states
- Sensor calibration data
- Last successful communication timestamp

**Master Controller Aggregates:**
- All tent states
- System-wide power consumption
- Alert counts
- Historical trend data

### Autonomous Operation Logic

**Critical: Each tent MUST operate independently**

**Failure Scenarios:**
1. **Master offline:** Tent continues with last schedule
2. **Network down:** Tent operates autonomously (safety limits enforced)
3. **HA offline:** Tent continues (no cloud automation, but local control works)
4. **Sensor failure:** Use last known good value, log warning, notify

**Watchdog Behavior:**
```yaml
# Each tent controller
esp32:
  framework:
    type: esp-idf

# Watchdog timer - reboot if hung
ota:
  safe_mode: true
  reboot_timeout: 5min

# Heartbeat to master
interval:
  - interval: 60s
    then:
      - mqtt.publish:
          topic: "grow/${device_name}/heartbeat"
          payload: !lambda 'return to_string(millis());'
```

---

## User Interface Design

### Master Controller UI (ESP32-P4, 7" 1024x600)

**Framework:** LVGL 8.x (built into ESPHome)

**Main Dashboard:**
```
┌──────────────────────────────────────────────────────────────────────────────┐
│  🌱 GROW SYSTEM           🕐 14:32:15      ☁ Online  📶 Strong  🔔 1 Alert  │
├────────────────┬────────────────┬────────────────┬─────────────────────────┤
│  TENT 1 - VEG  │ TENT 2 - FLOWER│ TENT 3 - HYDRO │ TENT 4 - SEEDLINGS      │
│ ────────────── │ ───────────────│ ───────────────│ ────────────────────    │
│ 💡 ON  [████░] │ 💡 ON [██████] │ 💡 ON  [███░░] │ 💡 OFF                  │
│ 🌡 24.5°C 💧65%│ 🌡 25.8°C 💧58%│ 🌡 23.2°C 💧72%│ 🌡 22.1°C 💧60%         │
│ VPD: 1.2 ✓     │ VPD: 1.4 ✓     │ VPD: 1.1 ⚠    │ VPD: 1.0 ✓             │
│ 💨 60%         │ 💨 75%         │ 💨 40%         │ 💨 30%                  │
│ ☀ 420 lux     │ ☀ 850 lux     │ ☀ 380 lux     │ ☀ 150 lux              │
│                │                │ 💧 pH:5.8 EC:1.4│                        │
│ ✓ All OK       │ ✓ All OK       │ ⚠ High Humidity│ ✓ All OK                │
│ [Tap: Detail]  │ [Tap: Detail]  │ [Tap: Detail]  │ [Tap: Detail]           │
└────────────────┴────────────────┴────────────────┴─────────────────────────┘
│ POWER: 1.85 kW / 3.0 kW  │ SD: 24.3 GB free │ HA: Connected              │
│ ALERTS: ⚠ Tent 3: High humidity (72% > 70%) - Increasing exhaust         │
│ [Schedule] [Manual] [Data View] [Settings] [Logs] [Export]               │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Pages:**
1. **Dashboard** - All tents overview
2. **Tent Detail** - Individual tent (tap on tent card)
3. **Schedule** - Light/pump timers for all tents
4. **Manual Control** - Override automation
5. **Data View** - Historical graphs
6. **Settings** - System configuration
7. **Logs** - Event log viewer
8. **Export** - SD card data export

**Navigation:**
- Touch: Tap to select, swipe to change pages
- Visual feedback: Button press animations, state changes

---

### Hydro Tent Controller UI (ESP32-S3, 480x320)

**Main Screen:**
```
┌─────────────────────────────────────────────────────────┐
│  TENT 3 - HYDRO DWC       🕐 14:32      [AUTO MODE]    │
├─────────────────────────────────────────────────────────┤
│ ENVIRONMENT              │ RESERVOIR                    │
│ ───────────────────────  │ ──────────────────────────   │
│ 🌡 23.2°C  💧 72% ⚠     │ 💧 pH:   5.8  ✓             │
│ VPD: 1.1 kPa             │ ⚡ EC:   1.4 mS/cm ✓        │
│ ☀ 380 lux               │ 🌡 Temp: 19.8°C ✓          │
│                          │ 📊 Level: 85% ████████░     │
│                          │                              │
│ pH TREND (24h)           │ EC TREND (24h)               │
│ ┌─────────────────┐     │ ┌─────────────────┐         │
│ │    ──⌃──         │     │ │     ──────        │         │
│ │  ⌄─    ─⌄       │     │ │   ⌄─      ─⌄     │         │
│ │ 5.5   6.0   6.5 │     │ │ 1.2   1.4   1.6  │         │
│ └─────────────────┘     │ └─────────────────┘         │
│                          │                              │
│ EQUIPMENT               │ PUMP SCHEDULE                │
│ ───────────────────────  │ ──────────────────────────   │
│ 💡 [████████░] 80% ON   │ 🚰 Feed:  Next in 2h15m     │
│ 💨 [████░░░░░] 40%      │ 🚰 Drain: Next in 5h30m     │
│ 💨 [██████░░░] 60% AUTO │ 🚰 Air:   Running           │
│                          │                              │
│ [⚙Settings] [📊Graphs] [🔧Manual] [📋Schedule]        │
└─────────────────────────────────────────────────────────┘
```

**Rotary Encoder Navigation:**
- **Rotate:** Highlight next menu item
- **Short press:** Select/activate
- **Long press:** Quick actions menu
  - Lights Off Now
  - Fan Boost (max exhaust for 10 min)
  - Feed Cycle Now
  - Emergency Stop All

**Pages:**
1. **Main** - Overview with trends
2. **Graphs** - Detailed 24h pH/EC/temp/humidity graphs
3. **Manual Control** - Override all equipment
4. **Schedule** - Pump timers, light schedule
5. **Calibration** - Sensor calibration wizards
6. **Settings** - VPD targets, alert thresholds

---

### Standard Tent Controller UI (ESP32, 320x280)

**Main Screen:**
```
┌─────────────────────────────────────┐
│     TENT 1 - VEGETATIVE             │
│  ──────────────────────────────     │
│  🌡 24.5°C  💧 65%  💡 450 lux      │
│  VPD: 1.2 kPa (OPTIMAL)             │
│                                     │
│  LIGHTS:  [████████░░] 80%  ON     │
│  FAN:     [██████░░░░] 60%          │
│  EXHAUST: [████░░░░░░] 40%          │
│                                     │
│  Last watered: 2d 14h ago           │
│  Soil moisture: 45% (good)          │
│                                     │
│  [⚙Settings] [📊History] [Manual]  │
└─────────────────────────────────────┘
```

**Pages:**
1. **Main** - Current status
2. **History** - 24h trend (simple line graphs)
3. **Manual** - Override lights/fans
4. **Settings** - Targets, schedules

---

## Control & Automation Strategy

### Three-Tier Control Architecture

**Tier 1: Local Autonomous (Tent Controller)**
- **Priority:** HIGHEST (safety & immediate response)
- **Latency:** <1 second
- **Scope:** Individual tent

**Responsibilities:**
- Emergency shutdown on critical temp (>35°C)
- VPD-based fan control
- Light schedule execution
- Pump timers (hydro)
- Safety interlocks (prevent conflicting relay states)

**Example: Emergency Temp Shutdown**
```yaml
binary_sensor:
  - platform: template
    id: emergency_temp
    lambda: |-
      return id(tent_temp).state > 35.0;
    on_press:
      - switch.turn_off: main_lights
      - switch.turn_off: supplemental_lights
      - switch.turn_on: exhaust_fan
      - output.set_level:
          id: exhaust_fan_pwm
          level: 100%
      - logger.log:
          level: ERROR
          format: "EMERGENCY SHUTDOWN: Temp %.1f°C"
          args: [id(tent_temp).state]
```

---

**Tier 2: Master Coordination**
- **Priority:** MEDIUM (optimization & coordination)
- **Latency:** <5 seconds
- **Scope:** Cross-tent

**Responsibilities:**
- Staggered light turn-on (reduce power spikes)
- Cross-tent ventilation (exhaust from one tent to another)
- Load balancing (don't exceed total power capacity)
- Data aggregation & trend analysis
- Alert aggregation & prioritization

**Example: Staggered Light Turn-On**
```yaml
# Master controller automation
automation:
  - alias: "Staggered Morning Lights"
    trigger:
      - platform: time
        at: "06:00:00"
    action:
      - homeassistant.service:
          service: switch.turn_on
          data:
            entity_id: switch.tent1_lights
      - delay: 2min  # Wait 2 min
      - homeassistant.service:
          service: switch.turn_on
          data:
            entity_id: switch.tent2_lights
      - delay: 2min
      - homeassistant.service:
          service: switch.turn_on
          data:
            entity_id: switch.tent3_lights
```

---

**Tier 3: Home Assistant (Cloud)**
- **Priority:** LOW (convenience & analytics)
- **Latency:** <30 seconds
- **Scope:** System-wide

**Responsibilities:**
- Long-term scheduling (veg→flower transitions)
- Growth stage management
- Historical data visualization (InfluxDB/Grafana)
- Remote monitoring & control
- Push notifications (critical alerts)
- Integration with external systems (weather, electricity pricing)

---

### VPD-Based Control

**What is VPD (Vapor Pressure Deficit)?**
- Measure of "drying power" of air
- Critical for plant transpiration
- Calculated from temp + humidity

**Formula:**
```
VPD (kPa) = (1 - RH/100) × SVP(T)

where SVP = Saturation Vapor Pressure
SVP(T) = 0.61078 × e^(17.27×T / (T+237.3))
```

**Target VPD Ranges:**
- **Seedlings/Clones:** 0.4 - 0.8 kPa (gentle)
- **Vegetative:** 0.8 - 1.2 kPa (moderate)
- **Flowering:** 1.0 - 1.5 kPa (stronger)

**Control Strategy:**
```yaml
# VPD calculation
sensor:
  - platform: template
    id: vpd
    name: "VPD"
    unit_of_measurement: "kPa"
    accuracy_decimals: 2
    lambda: |-
      float temp = id(tent_temp).state;
      float rh = id(tent_humidity).state;
      float svp = 0.61078 * exp((17.27 * temp) / (temp + 237.3));
      return (1.0 - rh/100.0) * svp;
    update_interval: 30s

# VPD-based fan control
climate:
  - platform: pid
    id: vpd_climate
    sensor: vpd
    default_target_temperature: 1.2  # Target VPD for veg
    heat_output: exhaust_fan_pwm
    control_parameters:
      kp: 0.5
      ki: 0.01
      kd: 0.1
```

---

### Hydroponic Control (DWC Specific)

**pH Control:**
- **Target:** 5.5 - 6.5 (optimal nutrient uptake)
- **Method:** Monitor only (manual dosing initially)
- **Future:** Automatic dosing pumps (pH up/down)

**EC/TDS Control:**
- **Target:** Depends on growth stage
  - Seedlings: 0.4 - 0.8 mS/cm
  - Vegetative: 0.8 - 1.4 mS/cm
  - Flowering: 1.2 - 2.0 mS/cm
- **Method:** Monitor + alert on drift

**Water Temperature:**
- **Target:** 18 - 22°C (prevent root rot)
- **Control:** Chiller or heater (relay-controlled)

**Dissolved Oxygen:**
- **Method:** Air pump (continuous or timed)
- **Target:** Keep roots well-oxygenated

**Pump Schedules:**
```yaml
# Feed pump (top-feed drip in DWC)
switch:
  - platform: template
    id: feed_pump
    turn_on_action:
      - switch.turn_on: relay_feed_pump
      - delay: 5min  # Run for 5 minutes
      - switch.turn_off: relay_feed_pump

# Feed schedule
interval:
  - interval: 4h  # Feed every 4 hours
    then:
      - switch.turn_on: feed_pump
```

---

## Data Logging & Analysis

### What to Log

**Environmental Data (Every 5 min):**
- Temperature (air, canopy, water if hydro)
- Humidity
- Light intensity (lux)
- VPD (calculated)

**Hydro Data (Every 15 min):**
- pH
- EC/TDS
- Water temperature
- Water level

**Equipment State (On Change):**
- Light on/off, brightness
- Fan speeds
- Relay states (pumps, heaters, etc.)

**Power Consumption (Every 5 min):**
- Per-tent power draw (if monitored)
- Total system power

**System Health (Every hour):**
- Uptime
- Free memory
- WiFi/Ethernet signal strength
- Last successful HA communication

### Storage Strategy

**SD Card (Master Controller):**
- **Format:** CSV files
- **Rotation:** Daily files, keep 30 days
- **Filename:** `grow_YYYYMMDD.csv`
- **Backup:** Auto-upload to HA before deletion

**CSV Format:**
```csv
timestamp,tent,sensor,value,unit
2024-12-28 14:32:15,tent1,temperature,24.5,C
2024-12-28 14:32:15,tent1,humidity,65.0,%
2024-12-28 14:32:15,tent1,vpd,1.2,kPa
...
```

**Home Assistant Database:**
- **Storage:** InfluxDB (time-series optimized)
- **Retention:** Unlimited (compressed)
- **Visualization:** Grafana dashboards

### Data Export

**Manual Export (via Master UI):**
- Select date range
- Select tents/sensors
- Export to SD card as ZIP
- Retrieve via USB or network share

**Automated Backup:**
```yaml
# Master controller - daily backup to HA
time:
  - platform: sntp
    on_time:
      - hours: 3
        minutes: 0
        then:
          - lambda: |-
              // Upload yesterday's CSV to HA
              // (Implementation in C++ component)
```

---

## Safety & Fail-Safe Mechanisms

### Hardware Safety

**Electrical:**
- ✅ GFCI protection on all AC circuits
- ✅ Thermal fuses on high-power relays
- ✅ Proper wire gauge (14 AWG for 15A, 12 AWG for 20A)
- ✅ Strain relief on all cable entries
- ✅ Waterproof enclosures (IP65 minimum)

**Thermal:**
- ✅ Heatsinks on SSRs if needed
- ✅ Ventilation in controller enclosures
- ✅ Thermal cutoff on heaters

### Software Safety

**Watchdog Timers:**
```yaml
esp32:
  framework:
    type: esp-idf

ota:
  safe_mode: true
  reboot_timeout: 5min
  num_attempts: 3
```

**Emergency Shutdowns:**
```yaml
# Temperature too high
binary_sensor:
  - platform: template
    id: temp_critical
    lambda: 'return id(tent_temp).state > 35.0;'
    on_press:
      - switch.turn_off: all_lights
      - switch.turn_on: exhaust_max
      - logger.log:
          level: ERROR
          format: "CRITICAL TEMP: %.1f°C"

# Temperature too low (frost protection)
  - platform: template
    id: temp_freeze
    lambda: 'return id(tent_temp).state < 10.0;'
    on_press:
      - switch.turn_off: exhaust_fan
      - switch.turn_on: heater
```

**Relay Interlocks:**
```yaml
# Never run humidifier and dehumidifier simultaneously
switch:
  - platform: template
    id: humidifier
    turn_on_action:
      - if:
          condition:
            switch.is_on: dehumidifier
          then:
            - logger.log: "Interlock: Turning off dehumidifier"
            - switch.turn_off: dehumidifier
      - switch.turn_on: relay_humidifier
```

**Network Timeout Handling:**
```yaml
# If master offline for 10 min, continue with last schedule
binary_sensor:
  - platform: status
    id: master_connected
    on_release:
      - delay: 10min
      - logger.log: "Master offline, autonomous mode"
      - script.execute: autonomous_schedule
```

**Sensor Validation:**
```yaml
sensor:
  - platform: bme280
    id: tent_temp
    filters:
      # Reject impossible values
      - lambda: |-
          if (x < -10.0 || x > 60.0) {
            ESP_LOGW("sensor", "Invalid temp: %.1f", x);
            return {};  // Reject reading
          }
          return x;
      # Use last good value if sensor fails
      - or:
        - throttle: 5min  # Max 5 min without update
        - delta: 10.0     # Or >10°C change (sensor fault)
```

### Alert System

**Alert Levels:**
1. **INFO:** Normal events (light schedule change)
2. **WARNING:** Non-critical issues (high humidity, low reservoir)
3. **ERROR:** Serious issues (sensor failure, network down)
4. **CRITICAL:** Emergency (temperature extreme, pump failure)

**Alert Destinations:**
- Master controller display (popup)
- Tent controller display (icon)
- Home Assistant (push notification)
- SD card log
- Buzzer (critical only)

---

## Enclosure Design

### Requirements

**Environmental:**
- **Temperature:** -10°C to 50°C (storage), 0°C to 40°C (operation)
- **Humidity:** Up to 95% RH (non-condensing)
- **IP Rating:** IP65 minimum (dust-tight, water-resistant)

**Material:**
- **Body:** PETG (better temp resistance than PLA, less warping than ABS)
- **Gasket:** TPU (flexible seal)
- **Lid:** Transparent PETG (view display without opening)

### Design Features

**Master Controller Enclosure:**
```
Dimensions: 250mm × 180mm × 80mm (W×H×D)

Front View:
┌─────────────────────────────────────┐
│     ╔═══════════════════╗           │  ← Transparent hinged lid (PETG)
│     ║   7" Display      ║           │
│     ║   (1024×600)      ║           │
│     ╚═══════════════════╝           │
│  ┌────────────────────────────────┐ │
│  │  [Reset] [Status LED]          │ │  ← Button panel
│  └────────────────────────────────┘ │
└─────────────────────────────────────┘

Side View:
                 Lid hinge
                    ↓
┌─────────────────────────────────────┐
│  ╔═══════════════╗                  │
│  ║ Display       ║                  │  ← Air gap for convection
│  ╚═══════════════╝                  │
│ ┌──────────────────────────────────┤
│ │ ESP32-P4 + Ethernet + SD         │  ← Main body
│ │ ┌─────┐  ┌────┐  ┌────┐         │
│ │ │ P4  │  │W5500│ │SD  │         │
│ │ └─────┘  └────┘  └────┘         │
│ └──────────────────────────────────┤
│ [Cable glands: Ethernet, Power]    │  ← Bottom cable entries
└─────────────────────────────────────┘
```

**Features:**
- **Drip edge:** Above display to divert moisture
- **Vent holes:** Passive ventilation (top/bottom, baffled)
- **Cable glands:** PG7/PG9 with rubber seals
- **Mounting:** DIN rail clips or keyhole hangers
- **TPU gasket:** Between lid and body (compression seal)

**Tent Controller Enclosure:**
```
Dimensions: 200mm × 150mm × 70mm (W×H×D)

Similar design but smaller:
- 480×320 or 320×280 display window
- Rotary encoder cutout (with O-ring seal)
- 2-3 button cutouts
- More cable glands (sensors, relays)
```

### Printability

**Print Settings:**
- **Material:** PETG
- **Layer height:** 0.2mm
- **Infill:** 20% gyroid
- **Walls:** 3-4 perimeters
- **Top/bottom:** 5 layers
- **Supports:** Yes (for overhangs)

**Post-Processing:**
- Sand mating surfaces smooth
- Drill out screw holes if needed
- Apply silicone sealant around cable glands
- Test fit before final assembly

---

## Implementation Phases

### Phase 1: Single Tent POC (Weeks 1-2)

**Goal:** Validate all components working together in one tent

**Tasks:**
- [ ] Select tent (recommend: Tent 1 standard or Tent 3 hydro)
- [ ] Build tent controller hardware
  - [ ] Assemble ESP32/ESP32-S3 on terminal board
  - [ ] Connect display (SPI)
  - [ ] Connect Ethernet (W5500 via SPI)
  - [ ] Connect sensors (I2C: BME280, BH1750, ADS1115 if hydro)
  - [ ] Connect relays (SSRs + Songle)
  - [ ] Connect TLC5947 for PWM dimming
  - [ ] Wire power supply (5V step-down)
- [ ] Create ESPHome configuration
  - [ ] Base config (logger, API, OTA)
  - [ ] Ethernet config
  - [ ] Sensor config
  - [ ] Relay control
  - [ ] Basic LVGL UI (single page)
- [ ] Test functionality
  - [ ] All sensors reading correctly
  - [ ] All relays switching
  - [ ] PWM dimming working (test with lamp)
  - [ ] Display showing data
  - [ ] Ethernet communication to HA
- [ ] Implement basic automation
  - [ ] Light schedule (on/off times)
  - [ ] VPD-based fan control
  - [ ] Emergency temp shutdown
- [ ] Run for 48h continuous (stability test)

**Deliverables:**
- ✅ One fully functional tent controller
- ✅ Validated hardware design
- ✅ Base ESPHome configs (reusable for other tents)
- ✅ Documented any issues/fixes

---

### Phase 2: Master Controller (Weeks 3-4)

**Goal:** Build central command station

**Tasks:**
- [ ] Build master controller hardware
  - [ ] ESP32-P4 on development board
  - [ ] Connect 7" MIPI-DSI display
  - [ ] Connect Ethernet (W5500)
  - [ ] Connect SD card module
  - [ ] Optional: BME280 for room conditions
  - [ ] Wire power supply
- [ ] Create ESPHome configuration
  - [ ] Base config
  - [ ] Ethernet config
  - [ ] SD card logging setup
  - [ ] Multi-page LVGL UI
    - [ ] Dashboard (all tents overview)
    - [ ] Tent detail pages
    - [ ] Settings page
    - [ ] Data export page
- [ ] Implement data aggregation
  - [ ] Subscribe to tent controller MQTT topics
  - [ ] Aggregate sensor data
  - [ ] Calculate system totals (power, alerts)
- [ ] Implement SD card logging
  - [ ] CSV writing every 5 min
  - [ ] Daily file rotation
  - [ ] Data export functionality
- [ ] Test with Phase 1 tent
  - [ ] Master shows tent data correctly
  - [ ] UI responsive to tent changes
  - [ ] Logs writing to SD card

**Deliverables:**
- ✅ Functional master controller
- ✅ Beautiful multi-tent dashboard
- ✅ Data logging working
- ✅ Communication with Phase 1 tent validated

---

### Phase 3: Multi-Tent Expansion (Weeks 5-6)

**Goal:** Build remaining tent controllers, validate network

**Tasks:**
- [ ] Build Tent 2 controller (clone of Tent 1 if standard)
- [ ] Build Tent 3 hydro controller (if not done in Phase 1)
  - [ ] Additional: pH/EC sensors via ADS1115
  - [ ] More complex UI (graphs)
  - [ ] Pump schedules
- [ ] Build Tent 4 controller (if needed)
- [ ] Set up Ethernet network
  - [ ] Install switch
  - [ ] Run cables to each tent (with drip loops)
  - [ ] Cable glands through tent fabric
  - [ ] Static IP configuration
- [ ] Test inter-controller communication
  - [ ] All tents visible on master
  - [ ] Latency testing (ping times)
  - [ ] Network reliability (24h stress test)
- [ ] Implement cross-tent coordination
  - [ ] Staggered light turn-on
  - [ ] System-wide power monitoring
  - [ ] Alert aggregation

**Deliverables:**
- ✅ All tent controllers operational
- ✅ Reliable Ethernet network
- ✅ Master coordinating all tents
- ✅ Cross-tent automations working

---

### Phase 4: Enclosures & Installation (Weeks 7-8)

**Goal:** Permanent installation in grow space

**Tasks:**
- [ ] Design 3D enclosures (or use existing designs)
  - [ ] Master controller enclosure
  - [ ] Tent controller enclosure (2-3 sizes)
- [ ] Print enclosures
  - [ ] Master: 1×
  - [ ] Standard tent: 3× (or as needed)
  - [ ] Hydro tent: 1×
- [ ] Print TPU gaskets
- [ ] Assemble enclosures
  - [ ] Mount electronics
  - [ ] Install displays
  - [ ] Wire cable glands
  - [ ] Apply silicone sealant
  - [ ] Test seal (spray test)
- [ ] Install in grow space
  - [ ] Master: Central location (outside tents, accessible)
  - [ ] Tent controllers: Inside each tent (avoid direct water)
  - [ ] Secure cables (zip ties, clips)
  - [ ] Final Ethernet runs
- [ ] Environmental testing
  - [ ] Run tents at high humidity (80%+)
  - [ ] Monitor for condensation in enclosures
  - [ ] Verify no water ingress
- [ ] Fine-tune PID controllers
  - [ ] VPD control tuning
  - [ ] Fan speed curves
  - [ ] Temperature stability

**Deliverables:**
- ✅ All hardware in waterproof enclosures
- ✅ Professional installation
- ✅ Environmental testing passed
- ✅ Optimized control parameters

---

### Phase 5: Home Assistant Integration (Weeks 9-10)

**Goal:** Full cloud integration and advanced automation

**Tasks:**
- [ ] Configure ESPHome → HA integration
  - [ ] All devices auto-discovered
  - [ ] Entities properly named
  - [ ] Device classes assigned
- [ ] Create HA dashboards
  - [ ] System overview (all tents)
  - [ ] Per-tent detail views
  - [ ] Historical graphs (InfluxDB + Grafana)
- [ ] Set up automations
  - [ ] Growth stage transitions (veg → flower)
  - [ ] Nutrient change reminders (hydro)
  - [ ] Harvest countdowns
  - [ ] Integration with calendar
- [ ] Configure alerting
  - [ ] Push notifications (critical alerts)
  - [ ] Email for warnings
  - [ ] Alert history log
- [ ] Set up data retention
  - [ ] InfluxDB for time-series
  - [ ] PostgreSQL for events
  - [ ] Backup strategy
- [ ] Create advanced automations
  - [ ] Weather-based exhaust (hot days = more cooling)
  - [ ] Electricity pricing (run pumps during cheap hours)
  - [ ] Vacation mode (reduce intensity, extend cycles)

**Deliverables:**
- ✅ Full HA integration
- ✅ Beautiful dashboards
- ✅ Intelligent automations
- ✅ Reliable alerting
- ✅ Long-term data storage

---

### Phase 6: Refinement & Documentation (Weeks 11-12)

**Goal:** Polish and document everything

**Tasks:**
- [ ] System optimization
  - [ ] Review all PID parameters
  - [ ] Optimize power consumption
  - [ ] Reduce network traffic if needed
- [ ] UI polish
  - [ ] Final LVGL theme/styling
  - [ ] Icon improvements
  - [ ] Animation tuning
- [ ] Create user documentation
  - [ ] Quick start guide
  - [ ] Sensor calibration procedures
  - [ ] Troubleshooting guide
  - [ ] Maintenance schedule
- [ ] Create technical documentation
  - [ ] Wiring diagrams
  - [ ] Component list with sources
  - [ ] ESPHome config explanations
  - [ ] Network topology diagram
- [ ] Backup & disaster recovery
  - [ ] SD card image backups
  - [ ] Config file backups (git repository)
  - [ ] Spare parts inventory

**Deliverables:**
- ✅ Fully optimized system
- ✅ Complete documentation
- ✅ Backup strategy in place
- ✅ Ready for long-term operation

---

## Bill of Materials

### Master Controller (1× Required)

| Component | Quantity | Notes |
|-----------|----------|-------|
| ESP32-P4 dev board | 1 | With MIPI-DSI interface |
| 7" capacitive touchscreen | 1 | 1024×600 resolution, MIPI-DSI |
| W5500 Ethernet module | 1 | SPI interface |
| SD card module | 1 | SPI or SDMMC interface |
| BME280 sensor (optional) | 1 | Room ambient monitoring |
| BH1750 sensor (optional) | 1 | Room light level |
| Buzzer | 1 | Piezo or active buzzer |
| 5V power supply | 1 | 2A minimum |
| Enclosure (3D printed) | 1 | PETG material |
| TPU gasket | 1 | Seal between lid and body |
| Cable glands | 3-4 | PG7 or PG9 size |

**Estimated Cost:** ~$120-150 USD

---

### Hydro Tent Controller (1× Required)

| Component | Quantity | Notes |
|-----------|----------|-------|
| ESP32-S3 N16R8 dev board | 1 | 16MB Flash, 8MB PSRAM |
| 480×320 TFT LCD touch | 1 | Resistive or capacitive |
| W5500 Ethernet module | 1 | SPI interface |
| Rotary encoder + button | 1 | EC11 type with switch |
| SSR 8A relay | 2 | For high-power lights |
| Songle relay 2A | 4-6 | For fans, pumps |
| TLC5947 PWM driver | 1 | 24 channels, SPI |
| ADS1115 ADC | 1 | 16-bit, 4-channel, I2C |
| BME280 sensor | 1 | Temp, humidity, pressure |
| BH1750 sensor | 1 | Light intensity |
| DS18B20 sensor | 2 | Air temp, water temp |
| pH sensor (analog) | 1 | Calibrated probe + board |
| EC/TDS sensor (analog) | 1 | Calibrated probe + board |
| Water level sensor | 1 | Float switch or ultrasonic |
| 5V power supply | 1 | 3A recommended |
| Enclosure (3D printed) | 1 | PETG material |
| TPU gasket | 1 | Seal between lid and body |
| Cable glands | 8-10 | PG7 or PG9 size |

**Estimated Cost:** ~$180-220 USD

---

### Standard Tent Controller (3× Required)

| Component | Quantity | Notes |
|-----------|----------|-------|
| ESP32-WROOM-32 dev board | 1 | 4MB Flash |
| 320×280 color LCD | 1 | SPI interface |
| W5500 Ethernet module | 1 | SPI interface |
| Rotary encoder + button | 1 | EC11 type with switch |
| SSR 8A relay | 2 | For lights |
| Songle relay 2A | 3-4 | For fans, pump |
| TLC5947 PWM driver | 1 | Shared SPI bus OK |
| BME280 sensor | 1 | Temp, humidity |
| BH1750 sensor | 1 | Light intensity |
| DS18B20 sensor | 1 | Canopy temp |
| Soil moisture sensor (opt) | 1 | Capacitive type |
| 5V power supply | 1 | 2A minimum |
| Enclosure (3D printed) | 1 | PETG material |
| TPU gasket | 1 | Seal |
| Cable glands | 6-8 | PG7 or PG9 |

**Estimated Cost:** ~$90-120 USD each, ×3 = $270-360 USD

---

### Network Infrastructure

| Component | Quantity | Notes |
|-----------|----------|-------|
| Gigabit Ethernet switch | 1 | 5-port minimum (8-port recommended) |
| CAT6 cable (outdoor rated) | ~50-100m | Depends on tent spacing |
| RJ45 connectors | 10-20 | Crimped ends or pre-made cables |
| Cable ties / clips | 1 pack | For cable management |

**Estimated Cost:** ~$40-60 USD

---

### Miscellaneous

| Component | Quantity | Notes |
|-----------|----------|-------|
| PETG filament | 2-3 kg | For enclosures |
| TPU filament | 0.5 kg | For gaskets |
| Silicone sealant | 1 tube | Waterproofing |
| Screws/nuts/bolts | Assorted | M3, M4 sizes |
| Wire (various gauges) | Assorted | 22-14 AWG |
| Heat shrink tubing | Assorted | Various sizes |
| Ferrules (if using) | 50-100 | For screw terminals |

**Estimated Cost:** ~$50-80 USD

---

### Total System Cost Estimate

| Category | Cost Range |
|----------|------------|
| Master Controller | $120-150 |
| Hydro Tent Controller | $180-220 |
| Standard Tent Controllers (×3) | $270-360 |
| Network Infrastructure | $40-60 |
| Miscellaneous | $50-80 |
| **TOTAL** | **$660-870 USD** |

**Note:** Prices are estimates and vary by region/supplier. Does not include grow equipment (lights, fans, tents, pumps).

---

## Session Notes

### Session 1 (2024-12-28)

**Attendees:** p1ngb4ck + Claude
**Duration:** ~2 hours

**Topics Covered:**
- Initial project scope discussion
- System architecture design
- Hardware selection (ESP32-P4 for master, ESP32-S3 for hydro)
- Network topology (Ethernet over WiFi rationale)
- UI mockups for master and tent controllers
- Three-tier control strategy
- Safety mechanisms

**Decisions Made:**
1. ✅ Master controller: ESP32-P4 with 7" capacitive touch (outside tents)
2. ✅ Hydro tent: ESP32-S3 with 480×320 display (complex UI needed)
3. ✅ Standard tents: ESP32-WROOM-32 with 320×280 display (cost-effective)
4. ✅ Network: Wired Ethernet (W5500 modules) for reliability
5. ✅ Data logging: SD card on master, long-term in HA (InfluxDB)
6. ✅ Control: Three-tier (local autonomous → master coordination → HA automation)

**Next Steps:**
- Create detailed plan document ✅ (this file)
- Choose implementation starting point
- Begin Phase 1 (single tent POC)

**Questions for Next Session:**
- Which tent to start with? (Tent 1 standard vs Tent 3 hydro)
- Do we have all hardware components available?
- Any specific sensor calibration procedures needed upfront?

---

### Session 2 (TBD)

*Session notes will be added here as work progresses*

---

## Appendices

### A. Wiring Diagrams

*TODO: Add wiring diagrams for each controller type*

**Master Controller Wiring:**
```
[Diagram pending - Session 2]
```

**Hydro Tent Controller Wiring:**
```
[Diagram pending - Session 2]
```

**Standard Tent Controller Wiring:**
```
[Diagram pending - Session 2]
```

---

### B. ESPHome Config Templates

*TODO: Add base configuration templates*

**File:** `common/base.yaml`
```yaml
# [Config pending - Session 2 or 3]
```

**File:** `common/ethernet.yaml`
```yaml
# [Config pending - Session 2 or 3]
```

---

### C. Calibration Procedures

**pH Sensor Calibration:**
```
[Procedure pending - Session 3 or 4]
1. Prepare buffer solutions (pH 4.0, 7.0, 10.0)
2. Rinse probe with distilled water
3. ...
```

**EC/TDS Sensor Calibration:**
```
[Procedure pending - Session 3 or 4]
1. Prepare calibration solutions (0.0, 1.413, 12.88 mS/cm)
2. ...
```

---

### D. Troubleshooting Guide

*TODO: Add common issues and solutions as discovered*

**Network Issues:**
```
[Troubleshooting pending - as issues arise]
```

**Sensor Issues:**
```
[Troubleshooting pending - as issues arise]
```

---

### E. Maintenance Schedule

**Daily:**
- [ ] Check master dashboard for alerts
- [ ] Visual inspection of all tents

**Weekly:**
- [ ] Check sensor calibration (pH, EC)
- [ ] Clean sensors
- [ ] Check reservoir levels (hydro)

**Monthly:**
- [ ] Backup SD card data
- [ ] Update firmware if needed
- [ ] Test emergency shutdown

**Quarterly:**
- [ ] Full sensor recalibration
- [ ] Check relay contacts
- [ ] Inspect enclosures for moisture

---

### F. References & Resources

**ESPHome Documentation:**
- https://esphome.io/
- LVGL component: https://esphome.io/components/lvgl.html

**Sensor Datasheets:**
- BME280: [Link pending]
- BH1750: [Link pending]
- ADS1115: [Link pending]

**VPD Calculators:**
- https://www.dimluxlighting.com/knowledge/blog/vapor-pressure-deficit-the-ultimate-guide-to-vpd/

**Hydroponic Resources:**
- pH/EC ranges: [Link pending]
- DWC best practices: [Link pending]

---

## Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2024-12-28 | p1ngb4ck + Claude | Initial plan document created |
|  |  |  |  |

---

**END OF PLAN DOCUMENT**

*This is a living document. Update as the project progresses.*
