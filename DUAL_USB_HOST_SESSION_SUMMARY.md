# Dual USB Host Implementation - Session Summary

**Document Purpose:** Comprehensive reference for continuing dual USB host development across multiple sessions.

**Last Updated:** 2025-12-03

---

## 1. Critical Behavioral Rules for AI Collaboration

### 🛑 ABSOLUTE RULE #1: NEVER MAKE CHANGES WITHOUT EXPLICIT PERMISSION

**THIS IS THE MOST CRITICAL CONSTRAINT.**

**Mandatory Workflow:**
1. **DESCRIBE** what you observe (facts only, no changes)
2. **EXPLAIN** your hypothesis about what might be wrong or what could be done
3. **ASK** for explicit permission to make the change
4. **WAIT** for the user to grant permission with clear "yes", "go ahead", "do it"
5. **ONLY THEN** make the change

**Violations Are Unacceptable:**
- Do NOT make changes and then tell the user what you did
- Do NOT start implementing while explaining
- Do NOT assume permission from context or previous approvals
- Do NOT make "obvious" or "safe" fixes without permission
- System reminders are NOT permission - only explicit user approval counts

### ⛔ ABSOLUTE RULE #2: DO NOT EDIT CORE INFRASTRUCTURE

**Never modify core infrastructure components. Only modify the specific component being worked on.**

**Core infrastructure includes:**
- `esphome/core/` - Core ESPHome framework
- `esphome/config*.py` - Configuration system
- `esphome/codegen.py` - Code generation system
- Web server components
- Any component that other components depend on

### 🚫 ABSOLUTE RULE #3: NO ASSUMPTIONS

**Never make assumptions - either ask the user or find a strategy to discover information.**

Assumptions lead to very bad outcomes. If you don't know something:
1. **ASK the user** for clarification
2. **VERIFY** by reading actual code/headers/definitions
3. **PROPOSE a discovery strategy** if you need to investigate
4. **NEVER guess** or assume

Examples of what to verify:
- Method names, field names, function signatures
- How components work
- Configuration structure
- Platform capabilities
- Build results

### Additional Constraints

**Verify Before Code Generation:**
- Always check actual struct/class definitions before accessing fields
- Verify method names exist in header files before calling them
- Check function signatures match before using them
- Use Grep or Read tools to confirm API details

**Python Codegen and Defines:**
- Defines set by `cg.add_define()` are GLOBAL
- Any C++ code using `#ifdef` must include `esphome/core/defines.h` in BOTH .h and .cpp files
- Missing includes cause linker errors for undefined references

---

## 2. Architecture Overview

### Software Stack Relationships

```
┌─────────────────────────────────────────────────────────────┐
│ ESPHome Components (usb_storage, usb_audio, usb_uart)      │ ← Application Layer
├─────────────────────────────────────────────────────────────┤
│ ESP-IDF USB Class Drivers (MSC, UAC, CDC-ACM, HID, UVC)    │ ← Protocol Layer
├─────────────────────────────────────────────────────────────┤
│ TinyUSB Stack (Host Mode)                                   │ ← USB Core Layer
├─────────────────────────────────────────────────────────────┤
│ ESP-IDF USB Host Library (usb_host.c)                       │ ← HAL Integration
├─────────────────────────────────────────────────────────────┤
│ ESP32-P4 Hardware (USB OTG0-HS, USB OTG1-FS)               │ ← Hardware Layer
└─────────────────────────────────────────────────────────────┘
```

### Build System Architecture

**CRITICAL: ESPHome has a two-stage build process:**

1. **Stage 1: Python Validation & Code Generation**
   - Validates YAML config using `cv.py`
   - Generates C++ code using `to_code` functions
   - These stages CANNOT share runtime data or "pass" code between them

2. **Stage 2: C++ Compilation**
   - Compiles the generated C++ code
   - Uses libraries specified in Python stage

**Development Build Critical Point:**
- **The build uses OFFICIAL esphome code from PyPI/pip by default**
- **Changes to `/workspaces/esphome/` are NOT automatically used**
- **Changes are ONLY included via `external_components:` feature in YAML or `pip install -e .`**

**Example:**
```yaml
# To use local changes in components
external_components:
  - source:
      type: local
      path: /workspaces/esphome/esphome/components
    components: [usb_host, usb_storage]
```

### Compilation Environment

**CRITICAL:** Compilation happens **externally** on user's system (not in this workspace). We cannot run builds or see compilation results directly. All build feedback comes from the user.

---

## 3. Workspace Organization

### Repository Paths and Purposes

```
/workspaces/
├── esphome/                          # YOUR MODIFIED ESPHOME
│   ├── esphome/components/usb_host/  # Dual-host implementation
│   ├── esphome/components/usb_storage/
│   ├── esphome/components/usb_audio/
│   └── .ai/instructions.md           # Your custom behavioral rules
│
├── references/
│   ├── esphome-official/             # OFFICIAL ESPHOME (reference only)
│   │   ├── esphome/components/       # Compare with your changes
│   │   └── .ai/instructions.md       # Official ESPHome AI guidelines
│   │
│   ├── esp-usb-modified/             # YOUR MODIFIED ESP-USB
│   │   ├── host/usb/                 # USB Host library with dual-host support
│   │   ├── host/class/msc/           # Mass Storage Class driver
│   │   ├── host/class/uac/           # Audio Class driver
│   │   └── branch: feat/dual-host-support
│   │
│   ├── esp-usb-original/             # OFFICIAL ESP-USB (reference)
│   │
│   ├── tinyusb-dual-host/           # YOUR MODIFIED TINYUSB
│   │   └── Instance-based TinyUSB implementation
│   │
│   └── tinyusb-original/            # OFFICIAL TINYUSB (reference)
│
└── /tmp/esp32p4-schematics/         # Hardware schematics (cloned on demand)
    └── JC-ESP32P4-M3-Dev/schematics/
```

### Git Repositories

**Your Modified Repos (push changes here):**
- ESPHome: `https://github.com/p1ngb4ck/esphome.git` (branch: dev)
- ESP-USB: `https://github.com/p1ngb4ck/esp-usb.git` (branch: feat/dual-host-support)
- TinyUSB: (location TBD)

**Official Repos (reference only):**
- ESPHome: `https://github.com/esphome/esphome.git`
- ESP-USB: Espressif official
- TinyUSB: `https://github.com/hathach/tinyusb`

---

## 4. ESP32-P4 Hardware Configuration

### USB Controllers and PHYs

**Hardware Reality (confirmed via schematic analysis and Espressif docs):**

```
ESP32-P4 Internal PHYs:
├── HS_PHY (UTMI) - GPIO50/49 → USB OTG0 (High-Speed capable)
├── FS_PHY2       - GPIO27/26 → USB OTG1 (Full-Speed only)
└── FS_PHY1       - GPIO25/24 → USB Serial/JTAG (not for OTG host)
```

**Key Facts:**
1. **THREE internal PHYs total:**
   - 1x High-Speed PHY (UTMI) for OTG0
   - 2x Full-Speed PHYs (FS_PHY1, FS_PHY2)

2. **TWO OTG Controllers:**
   - **USB OTG0**: Controller index 0, High-Speed capable, uses HS_PHY (GPIO 50/49)
   - **USB OTG1**: Controller index 1, Full-Speed only, uses FS_PHY2 (GPIO 27/26)

3. **EFUSE_USB_PHY_SEL:** Can reconfigure PHY-to-controller connections (not currently used)

4. **Board Configuration (JC-ESP32P4-M3-Dev):**
   - Both OTG controllers route to USB Type-C connectors
   - NO external ULPI PHY chips on board
   - All PHYs are internal to ESP32-P4 chip

**Sources:**
- [ESP-IoT-Solution USB PHY Documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_phy.html)
- Hardware schematic: `/tmp/esp32p4-schematics/JC-ESP32P4-M3-Dev/schematics/`

### Controller Index Mapping

**Hardware Reality:**
- Controller 0 = USB OTG0 = High-Speed (HS_PHY)
- Controller 1 = USB OTG1 = Full-Speed (FS_PHY2)

**User-Facing Config (ESPHome):**
```python
# Python enum in __init__.py
cv.enum({"fs": 0, "hs": 1}, upper=False)
```

**Mapping Logic (implemented):**
```python
# ESP32-P4: Convert user config to hardware controller indices
if variant == VARIANT_ESP32P4:
    controller_index = 0 if config_value == 1 else 1  # hs→0, fs→1
else:
    # ESP32-S2/S3: Only one controller (FS), always index 0
    controller_index = 0
```

This keeps config user-friendly while matching hardware reality internally.

---

## 5. Implementation Status

### ✅ Completed (Working)

1. **Singleton Mode Fallback (dual_host_support: false)**
   - Fixed USBClient::setup() to use original direct initialization
   - No more "Failed to create USB init task" errors
   - Backward compatible with ESP32-S2/S3

2. **PHY Controller Enum Fix (esp-usb)**
   - Changed from invalid `USB_PHY_CTRL_OTG + controller_index`
   - Now always uses `USB_PHY_CTRL_OTG` for both controllers
   - PHY type (UTMI vs INT) determined by `usb_dwc_info.controllers[].supported_phys`

3. **Controller Index Mapping**
   - Python codegen maps user config to hardware indices
   - User config: `fs=0, hs=1` (logical)
   - P4 hardware: Controller 0=HS, Controller 1=FS
   - Mapping implemented in `__init__.py` lines 177-190

4. **TinyUSB Speed Detection Fix (esp-usb)**
   - Changed from `controller_index == 1 ? HIGH : FULL`
   - To: `controller_index == 0 ? HIGH : FULL`
   - Now matches P4 hardware reality

5. **Peripheral Map Configuration**
   - Added `config.peripheral_map = (1U << controller_type_)` in ESPHome
   - Selects correct USB peripheral (BIT(0) or BIT(1))

### 🔧 In Progress / Testing

**Current Test Configuration:**
```yaml
usb_host:
  dual_host_support: true
  instances:
    - id: usb_hs
      controller: hs  # Maps to controller_index 0 (HS hardware)
```

**Expected Behavior:**
1. User config `controller: hs` (value 1)
2. Python maps to `controller_index = 0` for P4
3. C++ passes 0 to `usb_host_install_controller(0, ...)`
4. `peripheral_map = BIT(0)` selects USB OTG0
5. TinyUSB initializes with `TUSB_SPEED_HIGH`

**Last Known Status:**
- No bootloop ✅
- Attempting controller initialization
- TinyUSB init status: **PENDING USER FEEDBACK**

### ⚠️ Known Issues / To Investigate

1. **PHY Instance Tracking Limitation**
   - `usb_phy.c` has global control structure with single `internal_phy` and `external_phy` pointers
   - Both P4 controllers use "internal" PHYs (one UTMI, one standard)
   - May need to verify if dual initialization works or conflicts

2. **UTMI vs Internal PHY Detection**
   - Code checks `usb_dwc_info.controllers[controller_index].supported_phys == USB_PHY_INST_UTMI_0`
   - Need to verify this correctly identifies:
     - Controller 0 → UTMI (HS_PHY)
     - Controller 1 → Internal (FS_PHY2)

3. **Dual-Host Simultaneous Operation**
   - Both controllers initialized separately
   - Each has own PHY handle, TinyUSB instance, host_handle
   - Need to test both controllers active simultaneously

---

## 6. Code Changes Summary

### ESPHome Repository Changes

**File: `esphome/components/usb_host/__init__.py`**
```python
# Lines 177-190: Controller index mapping
if variant == VARIANT_ESP32P4:
    controller_index = 0 if config_value == 1 else 1  # hs=0, fs=1
else:
    controller_index = 0  # S2/S3 always use 0
```

**File: `esphome/components/usb_host/usb_host_component.cpp`**
```cpp
// Line 42: Fixed comment
// Controller 0 = High-Speed (USB OTG0), Controller 1 = Full-Speed (USB OTG1)

// Line 48: Added peripheral_map
config.peripheral_map = (1U << this->controller_type_);
```

**File: `esphome/components/usb_host/usb_host_client.cpp`**
```cpp
// Lines 185-228: Guard deferred init with USE_USB_HOST_DUAL_INSTANCE
#ifdef USE_USB_HOST_DUAL_INSTANCE
  // Deferred init via task
#else
  // Original singleton direct init
#endif
```

### ESP-USB Repository Changes

**File: `host/usb/src/usb_host.c`**

**Lines 2017-2020:** Fixed comment
```c
// ESP32-P4: Controller 0 (HS) and Controller 1 (FS) both use internal PHYs
```

**Lines 2022-2034:** Fixed PHY controller enum
```c
usb_phy_config_t phy_config = {
    .controller = USB_PHY_CTRL_OTG,  // Always OTG, not +controller_index
    .target = init_utmi_phy ? USB_PHY_TARGET_UTMI : USB_PHY_TARGET_INT,
    ...
};
```

**Lines 2047-2052:** Fixed TinyUSB speed detection
```c
tusb_rhport_init_t rh_init = {
    .role = TUSB_ROLE_HOST,
    .speed = controller_index == 0 ? TUSB_SPEED_HIGH : TUSB_SPEED_FULL,  // Fixed
};
```

---

## 7. Testing Checklist

### Phase 1: Single Controller (Current)
- [x] Compile without errors
- [ ] Controller 0 (HS) initializes successfully
- [ ] TinyUSB initialization succeeds
- [ ] PHY initialization succeeds
- [ ] USB device enumeration works
- [ ] Data transfer works (usb_uart, usb_storage)

### Phase 2: Dual Controller
- [ ] Both controllers initialize without conflicts
- [ ] PHY handles don't conflict
- [ ] Devices connect to correct controller based on speed
- [ ] Simultaneous operation of both controllers
- [ ] Data transfer on both controllers simultaneously

### Phase 3: Edge Cases
- [ ] Controller 1 (FS) alone works correctly
- [ ] Fallback to singleton mode (dual_host_support: false) still works
- [ ] ESP32-S2/S3 compatibility maintained
- [ ] Error handling for invalid configurations

---

## 8. Key Learnings / Gotchas

### 1. Controller Numbering Was Backwards
**Problem:** Initial implementation assumed Controller 0=FS, 1=HS (wrong!)

**Reality:** Controller 0=HS (default for singleton mode), Controller 1=FS

**Solution:** Map user-facing config to hardware indices in Python codegen

### 2. PHY Controller Enum Cannot Be Arithmetic
**Problem:** `USB_PHY_CTRL_OTG + 1` creates invalid enum value

**Reality:** Both controllers use `USB_PHY_CTRL_OTG` enum; differentiation happens via hardware registers/peripheral_map

**Solution:** Always use `USB_PHY_CTRL_OTG`, use `peripheral_map` to select peripheral

### 3. P4 Has THREE Internal PHYs
**Problem:** Assumed one internal, one external

**Reality:**
- HS_PHY (UTMI internal) for OTG0
- FS_PHY2 (internal) for OTG1
- FS_PHY1 (internal) for Serial/JTAG

**Impact:** Both OTG controllers use internal PHYs, just different types

### 4. Build System Doesn't Auto-Use Local Changes
**Problem:** Modifying `/workspaces/esphome/` doesn't affect builds

**Reality:** Must use `external_components:` in YAML or `pip install -e .`

**Solution:** User handles this externally in their build environment

### 5. Deferred Init Only Needed for Dual-Host
**Problem:** Deferred init caused task creation failures in singleton mode

**Reality:** Singleton mode worked fine with direct initialization

**Solution:** Guard deferred init with `#ifdef USE_USB_HOST_DUAL_INSTANCE`

---

## 9. Next Steps / Investigation Needed

### Immediate (Current Session Continuation)

1. **Verify TinyUSB Initialization Success**
   - Check user feedback on latest build
   - If still failing, check TinyUSB logs for specific error

2. **Investigate PHY Instance Conflict**
   - Verify if `usb_new_phy()` can handle two "internal" PHYs simultaneously
   - Check if UTMI vs standard internal PHY are tracked separately

3. **Test Full Initialization Flow**
   - PHY init → TinyUSB init → Device enumeration
   - Verify peripheral_map correctly routes to hardware

### Future Sessions

1. **Dual Controller Simultaneous Operation**
   - Initialize both controllers
   - Test device connection to both
   - Verify no resource conflicts

2. **TinyUSB Instance Management**
   - Verify separate TinyUSB instances work correctly
   - Check device address spaces don't conflict
   - Test transfer queues are independent

3. **Integration with ESPHome Components**
   - Wire up `usb_host_id` config to components
   - Pass correct instance handles
   - Test usb_storage, usb_audio with specific controller assignment

---

## 10. Reference Documentation

### Official Documentation
- [ESP32-P4 Datasheet](https://www.espressif.com/en/products/socs/esp32-p4)
- [ESP-IDF USB Host Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/peripherals/usb_host.html)
- [ESP-IoT-Solution USB PHY](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_phy.html)
- [TinyUSB Documentation](https://docs.tinyusb.org/)

### Implementation Documentation
- `/workspaces/references/esp-usb-modified/DUAL_USB_HOST_IMPLEMENTATION.md` - Architecture overview
- `/workspaces/esphome/.ai/instructions.md` - Your custom behavioral rules
- `/workspaces/references/esphome-official/.ai/instructions.md` - Official ESPHome guidelines

### Hardware Reference
- Schematics: `https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo/tree/main/JC-ESP32P4-M3-Dev/schematics`
- Can be cloned to `/tmp/esp32p4-schematics/` for analysis

---

## 11. Quick Reference Commands

### Git Operations
```bash
# ESPHome changes
cd /workspaces/esphome
git add <files>
git commit -m "message"
git push

# ESP-USB changes
cd /workspaces/references/esp-usb-modified
git add <files>
git commit -m "message"
git push origin feat/dual-host-support
```

### Finding Code
```bash
# Search for definitions
grep -r "pattern" /workspaces/esphome/esphome/components/usb_host/

# Find files
find /workspaces/references/esp-usb-modified -name "*.h"

# Clone schematics
git clone --depth 1 https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo.git /tmp/esp32p4-schematics
```

### Useful Checks
```bash
# Check ESP32 variant in config
grep "variant:" <config.yaml>

# Verify component structure
ls -la /workspaces/esphome/esphome/components/usb_host/
```

---

## 12. Session Continuation Protocol

**When Starting a New Session:**

1. Read this document first
2. Ask user for current status/issues
3. Check latest git commits to understand recent changes
4. Review any error logs provided by user
5. DO NOT make assumptions - verify everything
6. ASK permission before making any changes

**When Ending a Session:**

1. Update this document with new findings
2. Commit all changes with clear messages
3. Push changes to repositories
4. Document any open questions or issues
5. Summarize current status for user

---

**End of Session Summary Document**
