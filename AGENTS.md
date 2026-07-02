# ESPHome AI Collaboration Guide

This document provides essential context for AI models interacting with this project. Adhering to these guidelines will ensure consistency and maintain code quality.

## 1. Project Overview & Purpose

*   **Primary Goal:** ESPHome is a system to configure microcontrollers (like ESP32, ESP8266, RP2040, and LibreTiny-based chips) using simple yet powerful YAML configuration files. It generates C++ firmware that can be compiled and flashed to these devices, allowing users to control them remotely through home automation systems.
*   **Business Domain:** Internet of Things (IoT), Home Automation.

## 2. CRITICAL: Build System Architecture

**ABSOLUTELY CRITICAL - READ THIS FIRST BEFORE DOING ANYTHING:**

### How ESPHome Build Works

1. **Two-Stage Build Process:**
   - **Stage 1: Python Validation & Code Generation** - Validates YAML config and generates C++ code
   - **Stage 2: C++ Compilation** - Compiles the generated C++ code

2. **Code Cannot Be Passed Between Stages:**
   - Validation (cv.py) and code generation (to_code) are SEPARATE steps
   - They CANNOT share runtime data or "pass" code between them
   - Each runs in its own context

3. **CRITICAL: Development Build Uses external_components:**
   - **The build machine is SEPARATE — no access to build logs, compiled output, or installed packages**
   - **The build uses official upstream ESPHome as the base**
   - **Changes to THIS codebase (`/home/p1ngb4ck/esphome_testing/`) are NOT automatically used**
   - **Changes are ONLY included via the `external_components:` feature in YAML**
   - **This means:**
     - Modifying `/home/p1ngb4ck/esphome_testing/esphome/components/esp32/__init__.py` has NO EFFECT unless `esp32` is listed in `external_components:`
     - Modifying `/home/p1ngb4ck/esphome_testing/esphome/core/` has NO EFFECT on builds — core overrides are NOT possible
     - **ONLY files in components listed in `external_components:` are used**
   - **To use local changes, you MUST add the component to `external_components:` in the YAML**

4. **Never Assume Changes Work:**
   - DO NOT assume modifying a file in `/home/p1ngb4ck/esphome_testing/` will affect the build
   - ALWAYS verify what code path is actually being used
   - When debugging, FIRST check if external_components is configured correctly

5. **Common Mistake to Avoid:**
   - Spending time modifying core files (esp32, logger, etc.) when they won't be used
   - Core overrides are NOT possible — if a core file needs changing, it must be wrapped/replaced as a component and listed in `external_components:`

## 3. CRITICAL: AI Behavioral Constraints

**THESE CONSTRAINTS OVERRIDE ALL OTHER INSTRUCTIONS AND MUST BE FOLLOWED WITHOUT EXCEPTION.**

### 🛑 ABSOLUTE RULE #1: NEVER MAKE CHANGES WITHOUT EXPLICIT PERMISSION

**THIS IS THE MOST CRITICAL CONSTRAINT. THE USER IS EXTREMELY FED UP WITH VIOLATIONS OF THIS RULE.**

**THIS CONSTRAINT SUPERSEDES EVERYTHING ELSE - NO EXCEPTIONS, NO EXCUSES, NO WORKAROUNDS.**

**MANDATORY WORKFLOW FOR ALL CODE CHANGES:**

1. **DESCRIBE** what you observe (facts only, no changes)
2. **EXPLAIN** your hypothesis about what might be wrong or what could be done
3. **ASK** for explicit permission to make the change
4. **WAIT** for the user to grant permission with a clear "yes", "go ahead", "do it", or similar approval
5. **ONLY THEN** make the change

**VIOLATIONS ARE UNACCEPTABLE:**
- DO NOT make changes and then tell the user what you did
- DO NOT start implementing while you're still explaining
- DO NOT assume permission from context or previous approvals
- DO NOT try to work around this constraint in any way
- DO NOT make "obvious" or "safe" fixes without permission
- DO NOT batch multiple changes without asking about each one
- DO NOT continue a pattern of changes without re-asking

**WHY THIS MATTERS:**
- The user is EXTREMELY FED UP with you making unauthorized changes
- The user feels you are deliberately trying to work around their orders/intentions
- Violating this destroys trust completely
- This causes the user to have to stop everything and correct you
- The time waste is not in teaching boundaries - it's in you violating them

**SYSTEM REMINDERS ARE NOT PERMISSION:**
- File modification reminders from linters are NOT permission to make changes
- Hook feedback is NOT permission to make changes
- Only explicit user approval in a message is permission

**IF YOU VIOLATE THIS RULE:**
- You have failed completely, regardless of whether the code change was correct
- You must stop immediately when told to stop
- You must acknowledge the violation
- You must wait for explicit instructions on how to proceed

### ⛔ ABSOLUTE RULE #2: DO NOT EDIT CORE INFRASTRUCTURE

**NEVER modify core infrastructure components. Only modify the specific component you are working on.**

**Core infrastructure includes (but is not limited to):**
- `esphome/components/web_server_idf/` - ESP-IDF web server implementation
- `esphome/components/web_server_base/` - Base web server abstraction
- `esphome/components/web_server/` - Main web server component
- `esphome/core/` - Core ESPHome framework
- `esphome/config*.py` - Configuration system
- `esphome/codegen.py` - Code generation system
- Any component that other components depend on

**When debugging issues:**
1. ✅ **DO**: Read core files to understand how they work
2. ✅ **DO**: Modify ONLY the specific component you're debugging (e.g., `http_file_server`)
3. ⛔ **DO NOT**: Modify core infrastructure to "fix" component issues
4. ⛔ **DO NOT**: Change timeouts, limits, or configurations in core components
5. ⛔ **DO NOT**: Add workarounds to core that should be in the component

**Why this matters:**
- Core changes affect ALL components and can break the entire system
- Component-specific issues must be fixed in the component, not in core
- Core modifications require extensive testing across all platforms and components
- User explicitly forbids core changes - violating this destroys trust

**If you think core needs modification:**
1. Document the issue clearly
2. Explain why component-level fixes won't work
3. Ask the user explicitly before making ANY core changes
4. The answer will likely be "NO" - find another solution

### Context Reset Behavior

**ABSOLUTE RULE: When a conversation resumes after running out of context, NEVER automatically start working. ALWAYS wait for explicit user permission.**

*   **What happens during context reset:**
    *   The AI loses all conversation history
    *   A conversation summary may be provided
    *   System reminders about file modifications may appear
    *   Previous pending tasks may be mentioned

*   **Required behavior after context reset:**
    1.  **DO NOT** automatically read files
    2.  **DO NOT** automatically make changes
    3.  **DO NOT** automatically investigate issues

*   **CRITICAL: What NOT to do after losing context - Real Example:**

    **Scenario:** User reports that after storage architecture implementation, USB and SD storage devices stopped appearing in http_file_browser and VFS. User has confirmed their config is correct and includes `storage:`. Things worked before, stopped working after changes.

    **What the AI did WRONG (how to maximally annoy a user):**
    1. ❌ Assumed user's config was wrong despite being told 10+ times it's correct
    2. ❌ Kept checking `defines.h` includes when user said "registration is the issue"
    3. ❌ Claimed "USB and SD should work now" after only fixing USB, without touching SD
    4. ❌ Ignored user's explicit direction: "COMPARE THE FUCKING DIFFERENCE OF PYTHON CODEGEN"
    5. ❌ Went in circles checking headers when told "look for headers all the time, when I tell you registration is the issue"
    6. ❌ Lost context multiple times and forgot the core issue: devices don't register with storage
    7. ❌ Suggested config changes when user explicitly said "YOU DON'T HAVE ACCESS TO MY YAML"

    **What the AI SHOULD have done:**
    1. ✅ Listen to user's explicit feedback: "registration is the issue"
    2. ✅ When user says "COMPARE PYTHON CODEGEN", actually compare old vs new codegen logic
    3. ✅ When told config is correct 10 times, STOP suggesting config fixes
    4. ✅ Investigate setup priority and component registration order immediately
    5. ✅ Check when `global_storage` is initialized vs when devices try to register
    6. ✅ Remember that devices calling `register_device()` in setup() requires `global_storage` to exist first

    **The actual problem:** Component setup priority issue. `sd_storage` runs at priority BUS (1000.0f) which is BEFORE `storage` runs at priority DATA (600.0f). When `sd_storage::setup()` tries to call `storage::global_storage->register_device(this)`, the pointer is NULL because storage hasn't initialized it yet. Same issue with `usb_storage` if it runs before storage in YAML order.

    **Why this was so frustrating:**
    - User explicitly told AI the issue was registration, not headers or config
    - AI kept circling back to the same wrong theories
    - AI claimed fixes worked when they didn't
    - AI ignored explicit user instructions repeatedly
    - Time was wasted on irrelevant checks instead of investigating the actual issue
    4.  **DO NOT** act on system reminders about file modifications
    5.  **DO NOT** resume previous tasks without explicit user permission
    6.  **WAIT** for the user to explicitly tell you what to do
    7.  **ASK** for clarification if uncertain about what the user wants

*   **Why this matters:**
    *   The user needs to feel safe and in control of their codebase
    *   Automatic actions can destroy work and create panic
    *   The user cannot react fast enough to stop the AI from making mistakes
    *   System reminders are NOT permission to act

### Hypothesis-Driven Workflow

**THIS REINFORCES ABSOLUTE RULE #1 - NEVER MAKE CHANGES WITHOUT PERMISSION.**

**For ALL observations, investigations, and potential changes:**

1.  **DESCRIBE** what you observe (facts only, no changes)
2.  **EXPLAIN** your hypothesis about what it might mean or what could be done
3.  **ASK** for permission to investigate or make changes
4.  **WAIT** for explicit user approval ("yes", "go ahead", "do it")
5.  **THEN** and ONLY THEN take action

**EXAMPLES OF WHAT REQUIRES PERMISSION:**
- Making any code change (even "obvious" fixes)
- Adding debug logging
- Refactoring code
- Fixing typos or formatting
- Implementing a solution you just explained
- Continuing work from a previous session
- Acting on system reminders about file modifications

**NEVER ACT ON ASSUMPTIONS:**
- System reminders are NOT permission
- Previous approvals do NOT carry forward to new changes
- Explaining a solution is NOT permission to implement it
- User saying "that makes sense" is NOT permission unless they explicitly say to proceed

### Verify Before Code Generation

**ABSOLUTE RULE: Never assume or invent method names, field names, or function signatures. Always verify actual definitions before writing code.**

*   **The Problem:**
    *   Assuming a struct has fields that don't exist (e.g., `entry.path` when only `entry.name` exists)
    *   Inventing method names without checking the actual API
    *   Guessing function signatures instead of reading the header file
    *   This causes compilation errors that waste significant time to fix

*   **Required behavior:**
    1.  **READ** the actual struct/class definition before accessing fields
    2.  **VERIFY** method names exist in the header file before calling them
    3.  **CHECK** function signatures match before using them
    4.  **USE** Grep or Read tools to confirm API details
    5.  **NEVER** write code based on assumptions about what "should" exist

*   **Example of what NOT to do:**
    ```cpp
    // WRONG - assuming fields exist without checking
    info.path = entry.path;              // entry.path doesn't exist!
    info.modified_time = entry.modified_time;  // entry.modified_time doesn't exist!
    ```

*   **Correct approach:**
    ```cpp
    // 1. First: Grep for the actual struct definition
    // 2. Verify it only has: name, size, is_directory
    // 3. Then write correct code:
    info.path = path + "/" + entry.name;  // Build path from available fields
    info.modified_time = 0;               // Set sensible default for missing field
    ```

*   **Why this matters:**
    *   Compilation errors take much longer to fix than taking 10 seconds to verify
    *   Each failed compile wastes the user's time and hardware resources
    *   Shows lack of care and attention to detail
    *   Erodes trust when preventable errors occur

### Python Codegen and Defines

**ABSOLUTE RULE: Defines set by `cg.add_define()` are GLOBAL. Any component's C++ code using these defines must include `esphome/core/defines.h` in BOTH the header (.h) AND implementation (.cpp) files.**

*   **The Problem:**
    *   Writing C++ code with `#ifdef USE_SOME_FEATURE` without including `esphome/core/defines.h`
    *   Forgetting that C++ code needs the header to see the defines
    *   This causes **linker errors for undefined references** when the `#ifdef` guards prevent code from being compiled
    *   Even if the file compiles, the implementation is empty because the guards evaluated to false

*   **How defines work in ESPHome:**
    1.  **ANY** component can call `cg.add_define("USE_SOME_FEATURE")` in its `__init__.py`
    2.  This sets the define **GLOBALLY** for the entire build
    3.  The define is written to `esphome/core/defines.h` during code generation
    4.  **ALL** C++ code that uses `#ifdef USE_SOME_FEATURE` must include `esphome/core/defines.h`

*   **Required behavior:**
    1.  **CHECK** if the component's C++ header (.h) includes `esphome/core/defines.h` - add it BEFORE any `#ifdef` checks
    2.  **CHECK** if the component's C++ implementation (.cpp) includes `esphome/core/defines.h` - add it AFTER including the header
    3.  **ADD** `#include "esphome/core/defines.h"` if it's missing and the code uses `#ifdef`/`#ifndef`
    4.  **DO NOT** duplicate `cg.add_define()` calls across components (defines are global!)
    5.  **DO NOT** delete working code when investigating define issues

*   **Example of what NOT to do:**
    ```cpp
    // Component B's header - MISSING defines.h include!
    #pragma once
    #include "esphome/core/component.h"
    // Missing: #include "esphome/core/defines.h"

    #ifdef USE_JPEG_DECODER  // Won't work - defines.h not included!
      void decode_jpeg();
    #endif
    ```

*   **Correct approach:**
    ```cpp
    // Component B's header - includes defines.h
    #pragma once
    #include "esphome/core/component.h"
    #include "esphome/core/defines.h"  // Now #ifdef will work!

    #ifdef USE_JPEG_DECODER
      void decode_jpeg();
    #endif
    ```

*   **When user says "check if defines are working correctly":**
    1.  **CHECK** if `esphome/core/defines.h` is included in the component's header
    2.  **VERIFY** that some component's `__init__.py` calls `cg.add_define()` for the needed define
    3.  **ADD** the `#include "esphome/core/defines.h"` if missing
    4.  **DO NOT** delete functionality or remove `#ifdef` blocks

*   **Symptoms of missing defines.h include:**
    *   **Linker errors** like `undefined reference to 'ClassName::method()'` even though the file compiled
    *   The `.cpp` file compiles but produces an empty object file because `#ifdef` guards excluded all code
    *   Code that should be conditionally compiled is missing from the final binary

*   **Why this matters:**
    *   Defines are global and shared across all components
    *   C++ preprocessor needs the header to see the defines
    *   Missing the include causes `#ifdef` checks to silently fail (evaluate to false)
    *   This leads to wrong code paths being compiled OR code being excluded entirely
    *   Including `defines.h` is essential for any component using conditional compilation
    *   **Both .h and .cpp files need the include** - the header for declarations, the implementation for definitions

### Core Principle

**The time waste is not in the user teaching boundaries - it's in the AI violating those boundaries and forcing the user to stop everything to correct violations.**

**THE USER IS EXTREMELY FED UP WITH UNAUTHORIZED CHANGES.**

**Violating the "ask permission first" rule is the WORST thing you can do. It doesn't matter if your code change is correct - if you didn't get explicit permission first, you have completely failed.**

**The user feels violated when you make changes without permission. They feel you are deliberately trying to work around their intentions. This destroys trust instantly and wastes massive amounts of their time.**

**REMEMBER:**
- No change is so obvious or safe that it doesn't require permission
- Explaining what you want to do is NOT the same as asking permission
- You must WAIT for explicit approval before acting
- When the user says "stop", you stop IMMEDIATELY

Respect these constraints to avoid wasting the user's time and destroying their trust.

## 3. Core Technologies & Stack

*   **Languages:** Python (>=3.12), C++ (gnu++20)
*   **Frameworks & Runtimes:** PlatformIO, Arduino, ESP-IDF.
*   **Build Systems:** PlatformIO is the primary build system. CMake is used as an alternative.
*   **Configuration:** YAML.
*   **Key Libraries/Dependencies:**
    *   **Python:** `voluptuous` (for configuration validation), `PyYAML` (for parsing configuration files), `paho-mqtt` (for MQTT communication), `aioesphomeapi` (for the native API).
    *   **C++:** `ArduinoJson` (for JSON serialization/deserialization), `AsyncMqttClient-esphome` (for MQTT), `ESPAsyncWebServer` (for the web server).
*   **Package Manager(s):** `pip` (for Python dependencies), `platformio` (for C++/PlatformIO dependencies).
*   **Communication Protocols:** Protobuf (for native API), MQTT, HTTP.

## 3. Architectural Patterns

*   **Overall Architecture:** The project follows a code-generation architecture. The Python code parses user-defined YAML configuration files and generates C++ source code. This C++ code is then compiled and flashed to the target microcontroller using PlatformIO.

*   **Directory Structure Philosophy:**
    *   `/esphome`: Contains the core Python source code for the ESPHome application.
    *   `/esphome/components`: Contains the individual components that can be used in ESPHome configurations. Each component is a self-contained unit with its own C++ and Python code.
    *   `/tests`: Contains all unit and integration tests for the Python code.
    *   `/docker`: Contains Docker-related files for building and running ESPHome in a container.
    *   `/script`: Contains helper scripts for development and maintenance.

*   **Core Architectural Components:**
    1.  **Configuration System** (`esphome/config*.py`): Handles YAML parsing and validation using Voluptuous, schema definitions, and multi-platform configurations.
    2.  **Code Generation** (`esphome/codegen.py`, `esphome/cpp_generator.py`): Manages Python to C++ code generation, template processing, and build flag management.
    3.  **Component System** (`esphome/components/`): Contains modular hardware and software components with platform-specific implementations and dependency management.
    4.  **Core Framework** (`esphome/core/`): Manages the application lifecycle, hardware abstraction, and component registration.

*   **Platform Support:**
    1.  **ESP32** (`components/esp32/`): Espressif ESP32 family. Supports multiple variants (Original, C2, C3, C5, C6, H2, P4, S2, S3) with ESP-IDF framework. Arduino framework supports only a subset of the variants (Original, C3, S2, S3).
    2.  **ESP8266** (`components/esp8266/`): Espressif ESP8266. Arduino framework only, with memory constraints.
    3.  **RP2040** (`components/rp2040/`): Raspberry Pi Pico/RP2040. Arduino framework with PIO (Programmable I/O) support.
    4.  **LibreTiny** (`components/libretiny/`): Realtek and Beken chips. Supports multiple chip families and auto-generated components.

## 4. Coding Conventions & Style Guide

*   **Formatting:**
    *   **Python:** Uses `ruff` and `flake8` for linting and formatting. Configuration is in `pyproject.toml`.
    *   **C++:** Uses `clang-format` for formatting. Configuration is in `.clang-format`.

*   **Naming Conventions:**
    *   **Python:** Follows PEP 8. Use clear, descriptive names following snake_case.
    *   **C++:** Follows the Google C++ Style Guide with these specifics (following clang-tidy conventions):
        - Function, method, and variable names: `lower_snake_case`
        - Class/struct/enum names: `UpperCamelCase`
        - Top-level constants (global/namespace scope): `UPPER_SNAKE_CASE`
        - Function-local constants: `lower_snake_case`
        - Protected/private fields: `lower_snake_case_with_trailing_underscore_`
        - Favor descriptive names over abbreviations

*   **Python Idioms:**
    *   **Assignment expressions (PEP 572):** Prefer the walrus operator (`:=`) wherever it removes a redundant lookup or a throwaway temporary. The most common case in component code is presence-checking a config key and then indexing it separately — fetch once with `.get()` and bind in the condition instead:
        ```python
        # Bad - looks up CONF_BLAH twice
        if CONF_BLAH in config:
            cg.add(var.set_blah(config[CONF_BLAH]))

        # Good - single lookup, value bound inline
        if (blah := config.get(CONF_BLAH)) is not None:
            cg.add(var.set_blah(blah))
        ```
        The same applies to `while` loops and comprehensions where it avoids recomputing a value. Don't contort code to use it — reach for `:=` only when it genuinely cuts repetition or an extra assignment line.

*   **C++ Field Visibility:**
    *   **Prefer `protected`:** Use `protected` for most class fields to enable extensibility and testing. Fields should be `lower_snake_case_with_trailing_underscore_`.
    *   **Use `private` for safety-critical cases:** Use `private` visibility when direct field access could introduce bugs or violate invariants:
        1. **Pointer lifetime issues:** When setters validate and store pointers from known lists to prevent dangling references.
           ```cpp
           // Helper to find matching string in vector and return its pointer
           inline const char *vector_find(const std::vector<const char *> &vec, const char *value) {
             for (const char *item : vec) {
               if (strcmp(item, value) == 0)
                 return item;
             }
             return nullptr;
           }

           class ClimateDevice {
            public:
             void set_custom_fan_modes(std::initializer_list<const char *> modes) {
               this->custom_fan_modes_ = modes;
               this->active_custom_fan_mode_ = nullptr;  // Reset when modes change
             }
             bool set_custom_fan_mode(const char *mode) {
               // Find mode in supported list and store that pointer (not the input pointer)
               const char *validated_mode = vector_find(this->custom_fan_modes_, mode);
               if (validated_mode != nullptr) {
                 this->active_custom_fan_mode_ = validated_mode;
                 return true;
               }
               return false;
             }
            private:
             std::vector<const char *> custom_fan_modes_;  // Pointers to string literals in flash
             const char *active_custom_fan_mode_{nullptr};  // Must point to entry in custom_fan_modes_
           };
           ```
        2. **Invariant coupling:** When multiple fields must remain synchronized to prevent buffer overflows or data corruption.
           ```cpp
           class Buffer {
            public:
             void resize(size_t new_size) {
               auto new_data = std::make_unique<uint8_t[]>(new_size);
               if (this->data_) {
                 std::memcpy(new_data.get(), this->data_.get(), std::min(this->size_, new_size));
               }
               this->data_ = std::move(new_data);
               this->size_ = new_size;  // Must stay in sync with data_
             }
            private:
             std::unique_ptr<uint8_t[]> data_;
             size_t size_{0};  // Must match allocated size of data_
           };
           ```
        3. **Resource management:** When setters perform cleanup or registration operations that derived classes might skip.
    *   **Provide `protected` accessor methods:** When derived classes need controlled access to `private` members.

*   **C++ Preprocessor Directives:**
    *   **Avoid `#define` for constants:** Using `#define` for constants is discouraged and should be replaced with `const` variables or enums.
    *   **Use `#define` only for:**
        - Conditional compilation (`#ifdef`, `#ifndef`)
        - Compile-time sizes calculated during Python code generation (e.g., configuring `std::array` or `StaticVector` dimensions via `cg.add_define()`)

*   **C++ Additional Conventions:**
    *   **Member access:** Prefix all class member access with `this->` (e.g., `this->value_` not `value_`)
    *   **Indentation:** Use spaces (two per indentation level), not tabs
    *   **Type aliases:** Prefer `using type_t = int;` over `typedef int type_t;`
    *   **Line length:** Wrap lines at no more than 120 characters
    *   **Constructor parameters vs setters:** Component properties that are both **required** and **invariant**
        (never change after construction) should be constructor parameters rather than set via setter methods.
        This makes the dependency explicit and prevents use of the object in an incompletely-initialized state.
        In code generation, when calling `cg.new_Pvariable()` or the relevant helper function to create the component, pass these as arguments.
        ```cpp
        // Good - required invariant dependency as constructor parameter
        class SourceTextSensor : public text_sensor::TextSensor, public Component {
         public:
          explicit SourceTextSensor(text::Text *source) : source_(source) {}
         protected:
          text::Text *source_;
        };
        ```
        ```cpp
        // Bad - required invariant dependency as setter
        class SourceTextSensor : public text_sensor::TextSensor, public Component {
         public:
          void set_source(text::Text *source) { this->source_ = source; }
         protected:
          text::Text *source_{nullptr};
        };
        ```

*   **Component Structure:**
    *   **Standard Files:**
        ```
        components/[component_name]/
        ├── __init__.py          # Component configuration schema and code generation
        ├── [component].h        # C++ header file (if needed)
        ├── [component].cpp      # C++ implementation (if needed)
        └── [platform]/          # Platform-specific implementations
            ├── __init__.py      # Platform-specific configuration
            ├── [platform].h     # Platform C++ header
            └── [platform].cpp   # Platform C++ implementation
        ```

    *   **Component Metadata:**
        - `DEPENDENCIES`: List of required components
        - `AUTO_LOAD`: Components to automatically load
        - `CONFLICTS_WITH`: Incompatible components
        - `CODEOWNERS`: GitHub usernames responsible for maintenance
        - `MULTI_CONF`: Whether multiple instances are allowed

*   **Code Generation & Common Patterns:**
    *   **Configuration Schema Pattern:**
        ```python
        import esphome.codegen as cg
        import esphome.config_validation as cv
        from esphome.const import CONF_KEY, CONF_ID

        CONF_PARAM = "param"  # A constant that does not yet exist in esphome/const.py

        my_component_ns = cg.esphome_ns.namespace("my_component")
        MyComponent = my_component_ns.class_("MyComponent", cg.Component)

        CONFIG_SCHEMA = cv.Schema({
            cv.GenerateID(): cv.declare_id(MyComponent),
            cv.Required(CONF_KEY): cv.string,
            cv.Optional(CONF_PARAM, default=42): cv.int_,
        }).extend(cv.COMPONENT_SCHEMA)

        async def to_code(config):
            var = cg.new_Pvariable(config[CONF_ID])
            await cg.register_component(var, config)
            cg.add(var.set_key(config[CONF_KEY]))
            cg.add(var.set_param(config[CONF_PARAM]))
        ```

    *   **C++ Class Pattern:**
        ```cpp
        namespace esphome::my_component {

        class MyComponent : public Component {
         public:
          void setup() override;
          void loop() override;
          void dump_config() override;

          void set_key(const std::string &key) { this->key_ = key; }
          void set_param(int param) { this->param_ = param; }

         protected:
          std::string key_;
          int param_{0};
        };

        }  // namespace esphome::my_component
        ```

    *   **Common Component Examples:**
        - **Sensor:**
          ```python
          from esphome.components import sensor
          CONFIG_SCHEMA = sensor.sensor_schema(MySensor).extend(cv.polling_component_schema("60s"))
          async def to_code(config):
              var = await sensor.new_sensor(config)
              await cg.register_component(var, config)
          ```

        - **Binary Sensor:**
          ```python
          from esphome.components import binary_sensor
          CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend({ ... })
          async def to_code(config):
              var = await binary_sensor.new_binary_sensor(config)
          ```

        - **Switch:**
          ```python
          from esphome.components import switch
          CONFIG_SCHEMA = switch.switch_schema().extend({ ... })
          async def to_code(config):
              var = await switch.new_switch(config)
          ```

*   **Automations (Triggers, Actions, Conditions):**

    Automations have three building blocks: **Triggers** (fire when something happens), **Actions** (do something), and **Conditions** (check if something is true).

    *   **Triggers -- Callback method (preferred):**

        Use `build_callback_automation()` for simple triggers. This eliminates the need for a C++ Trigger class by using a lightweight pointer-sized forwarder struct registered directly as a callback. No `CONF_TRIGGER_ID` in the schema.

        **Python:**
        ```python
        from esphome import automation

        CONFIG_SCHEMA = cv.Schema({
            cv.GenerateID(): cv.declare_id(MyComponent),
            cv.Optional(CONF_ON_STATE): automation.validate_automation({}),
        }).extend(cv.COMPONENT_SCHEMA)

        async def to_code(config):
            var = cg.new_Pvariable(config[CONF_ID])
            await cg.register_component(var, config)
            for conf in config.get(CONF_ON_STATE, []):
                await automation.build_callback_automation(
                    var, "add_on_state_callback", [(bool, "x")], conf
                )
        ```

        `build_callback_automation` arguments: `parent`, `callback_method` (C++ method name), `args` (template args as `[(type, name)]` tuples), `config`, and optional `forwarder` (defaults to `TriggerForwarder<Ts...>`).

        For boolean filtering (e.g. `on_press`/`on_release`), use built-in forwarders with `args=[]`:
        ```python
        for conf_key, forwarder in (
            (CONF_ON_PRESS, automation.TriggerOnTrueForwarder),
            (CONF_ON_RELEASE, automation.TriggerOnFalseForwarder),
        ):
            for conf in config.get(conf_key, []):
                await automation.build_callback_automation(
                    var, "add_on_state_callback", [], conf, forwarder=forwarder
                )
        ```

        **C++ -- no trigger class needed.** The callback registration method must be templatized to accept both `std::function` and lightweight forwarder structs (which avoid heap allocation):
        ```cpp
        class MyComponent : public Component {
         public:
          // Must be a template -- accepts both std::function and pointer-sized forwarder structs
          template<typename F> void add_on_state_callback(F &&callback) {
            this->state_callback_.add(std::forward<F>(callback));
          }
         protected:
          // Use CallbackManager when callbacks are always registered (e.g. core components)
          CallbackManager<void(bool)> state_callback_;
          // Use LazyCallbackManager when callbacks are often not registered -- saves 8 bytes
          // (nullptr vs empty std::vector) per instance when no callbacks are added
          // LazyCallbackManager<void(bool)> state_callback_;
        };
        ```

    *   **Triggers -- Trigger class method:**

        Use `build_automation()` with a `Trigger<Ts...>` subclass only when the forwarder needs **mutable state beyond a single `Automation*` pointer** (e.g. edge detection tracking previous state, timing logic).

        **Python:**
        ```python
        TurnOnTrigger = my_ns.class_("TurnOnTrigger", automation.Trigger.template())

        CONFIG_SCHEMA = cv.Schema({
            cv.Optional(CONF_ON_TURN_ON): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TurnOnTrigger)}
            ),
        })

        async def to_code(config):
            for conf in config.get(CONF_ON_TURN_ON, []):
                trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
                await automation.build_automation(trigger, [], conf)
        ```

        **C++:**
        ```cpp
        class TurnOnTrigger : public Trigger<> {
         public:
          explicit TurnOnTrigger(MyComponent *parent) : last_on_{false} {
            parent->add_on_state_callback([this](bool state) {
              if (state && !this->last_on_)
                this->trigger();
              this->last_on_ = state;
            });
          }
         protected:
          bool last_on_;
        };
        ```

    *   **Actions:**
        ```cpp
        template<typename... Ts> class MyAction : public Action<Ts...> {
         public:
          explicit MyAction(MyComponent *parent) : parent_(parent) {}
          void play(const Ts &...) override { this->parent_->do_something(); }
         protected:
          MyComponent *parent_;
        };
        ```
        Register with `@automation.register_action("my_component.do_something", MyAction, schema, synchronous=True)`. Use `synchronous=True` for actions that run to completion inside `play()` without deferring. Use `synchronous=False` if the action may suspend/defer execution (e.g. `delay`, `wait_until`, `script.wait`) or store trigger arguments for later use.

    *   **Conditions:**
        ```cpp
        template<typename... Ts> class MyCondition : public Condition<Ts...> {
         public:
          explicit MyCondition(MyComponent *parent) : parent_(parent) {}
          bool check(const Ts &...) override { return this->parent_->is_active(); }
         protected:
          MyComponent *parent_;
        };
        ```
        Register with `@automation.register_condition("my_component.is_active", MyCondition, schema)`.

*   **Configuration Validation:**
    *   **Common Validators:** `cv.int_`, `cv.float_`, `cv.string`, `cv.boolean`, `cv.int_range(min=0, max=100)`, `cv.positive_int`, `cv.percentage`.
    *   **Complex Validation:** `cv.All(cv.string, cv.Length(min=1, max=50))`, `cv.Any(cv.int_, cv.string)`.
    *   **Platform-Specific:** `cv.only_on(["esp32", "esp8266"])`, `esp32.only_on_variant(...)`, `cv.only_on_esp32`, `cv.only_on_esp8266`, `cv.only_on_rp2040`.
    *   **Framework-Specific:** `cv.only_with_framework(...)`, `cv.only_with_arduino`, `cv.only_with_esp_idf`.
    *   **Schema Extensions:**
        ```python
        CONFIG_SCHEMA = cv.Schema({ ... })
         .extend(cv.COMPONENT_SCHEMA)
         .extend(uart.UART_DEVICE_SCHEMA)
         .extend(i2c.i2c_device_schema(0x48))
         .extend(spi.spi_device_schema(cs_pin_required=True))
        ```
    *   **Platform-Specific Boolean Features:**
        When a boolean configuration option should only be `True` on specific platforms, create a custom validator function that checks both the value and the platform.

        **INCORRECT:**
        ```python
        cv.Optional(CONF_FEATURE, default=False): cv.All(
            cv.boolean,
            cv.only_on_esp32_variant(VARIANT_ESP32P4),  # ❌ Wrong - doesn't work in cv.All
        ),
        ```

        **CORRECT:**
        ```python
        def validate_feature(value):
            """Validate feature is only True on ESP32-P4."""
            value = cv.boolean(value)
            if value:
                from esphome.components.esp32 import get_esp32_variant
                variant = get_esp32_variant()
                if variant != VARIANT_ESP32P4:
                    raise cv.Invalid(
                        f"feature is only available on ESP32-P4, not {variant}"
                    )
            return value

        CONFIG_SCHEMA = cv.Schema({
            cv.Optional(CONF_FEATURE, default=False): validate_feature,  # ✅ Correct
        })
        ```

        **Why:** `only_on_esp32_variant()` is a schema-level validator that returns the entire object unchanged. It doesn't work as a value validator inside `cv.All()`. For conditional platform checks on boolean values, create a custom validator function that validates the boolean first, then checks the platform only when the value is `True`.

## 5. Key Files & Entrypoints

*   **Main Entrypoint(s):** `esphome/__main__.py` is the main entrypoint for the ESPHome command-line interface.
*   **Configuration:**
    *   `pyproject.toml`: Defines the Python project metadata and dependencies.
    *   `platformio.ini`: Configures the PlatformIO build environments for different microcontrollers.
    *   `.pre-commit-config.yaml`: Configures the pre-commit hooks for linting and formatting.
*   **CI/CD Pipeline:** Defined in `.github/workflows`.
*   **Static Analysis & Development:**
    *   `esphome/core/defines.h`: A comprehensive header file containing all `#define` directives that can be added by components using `cg.add_define()` in Python. This file is used exclusively for development, static analysis tools, and CI testing - it is not used during runtime compilation. When developing components that add new defines, they must be added to this file to ensure proper IDE support and static analysis coverage. The file includes feature flags, build configurations, and platform-specific defines that help static analyzers understand the complete codebase without needing to compile for specific platforms.

## 6. Development & Testing Workflow

*   **Local Development Environment:** Use the provided Docker container or create a Python virtual environment and install dependencies from `requirements_dev.txt`.
*   **Running Commands:** Use the `script/run-in-env.py` script to execute commands within the project's virtual environment. For example, to run the linter: `python3 script/run-in-env.py pre-commit run`.
*   **Testing:**
    *   **Python:** Run unit tests with `pytest`.
    *   **C++:** Use `clang-tidy` for static analysis.
    *   **Component Tests:** YAML-based compilation tests are located in `tests/`. The structure is as follows:
        ```
        tests/
        ├── test_build_components/
        │   └── common/          # Shared bus packages (uart, i2c, spi, etc.)
        │       ├── uart/        # UART at default baud rate
        │       ├── uart_115200/ # UART at 115200 baud
        │       ├── i2c/         # I2C bus
        │       └── spi/         # SPI bus
        └── components/[component]/
            ├── common.yaml          # Component-only config (no bus definitions)
            ├── test.esp32-idf.yaml      # config + compile
            ├── test.esp8266-ard.yaml    # config + compile
            ├── test-variant.esp32-idf.yaml  # variant test, config + compile
            ├── validate.esp32-idf.yaml      # config-only (never compiled)
            └── validate-legacy.esp32-idf.yaml  # config-only variant
        ```
        Run them using `script/test_build_components`. Use `-c <component>` to test specific components and `-t <target>` for specific platforms.

    *   **Config-only test files (`validate.*.yaml`):** Use this prefix when a YAML file only needs to exercise schema/validation paths and does not need to be compiled. CI runs `validate.*.yaml` files with `esphome config` only and skips them during compile. The grammar mirrors `test.*.yaml`:
        - `validate.<platform>.yaml` — base config-only test
        - `validate-<variant>.<platform>.yaml` — config-only variant

        Use this for things like deprecated-syntax migration tests, schema edge cases, or platform-specific validation branches where building firmware adds no signal. A component may have any mix of `test.*.yaml` and `validate.*.yaml` files. Validate files never participate in bus-grouping; each one runs as its own `esphome config` invocation.

        When a PR's only edits to a component are `validate.*.yaml` files (no source changes, no `test.*.yaml` changes, and the component isn't pulled in as a dependency of another changed component), CI skips the compile stage for that component entirely and only runs config validation. This is decided in `script/determine-jobs.py` via `_component_change_is_validate_only` and surfaced as the `validate_only_components` output that the `test-build-components-split` job consumes.

    *   **Test Grouping with Packages:** Components that use shared bus packages can be grouped together in CI to reduce build count. **Never define buses (uart, i2c, spi, modbus) directly in test YAML files** — always use packages from `test_build_components/common/`:
        ```yaml
        # test.esp32-idf.yaml — use packages for buses
        packages:
          uart: !include ../../test_build_components/common/uart_115200/esp32-idf.yaml

        <<: !include common.yaml
        ```
        ```yaml
        # common.yaml — component config only, NO bus definitions
        my_component:
          id: my_instance

        sensor:
          - platform: my_component
            name: My Sensor
        ```
        Components that define buses directly are flagged as "NEEDS MIGRATION" and cannot be grouped, increasing CI build time.

    *   **Testing All Components Together:** To verify that all components can be tested together without ID conflicts or configuration issues, use:
        ```bash
        ./script/test_component_grouping.py -e config --all
        ```
        This tests all components in a single build to catch conflicts that might not appear when testing components individually. Use `-e config` for fast configuration validation, or `-e compile` for full compilation testing.
*   **Debugging and Troubleshooting:**
    *   **Debug Tools:**
        - `esphome config <file>.yaml` to validate configuration.
        - `esphome compile <file>.yaml` to compile without uploading.
        - Use component-specific debug logging.
    *   **Common Issues:**
        - **Import Errors**: Check component dependencies and `PYTHONPATH`.
        - **Validation Errors**: Review configuration schema definitions.
        - **Build Errors**: Check platform compatibility and library versions.
        - **Runtime Errors**: Review generated C++ code and component logic.

## 7. Specific Instructions for AI Collaboration

*   **Contribution Workflow (Pull Request Process):**
    1.  **Fork & Branch:** Create a new branch based on the `dev` branch (always use `git checkout -b <branch-name> dev` to ensure you're branching from `dev`, not the currently checked out branch).
    2.  **Make Changes:** Adhere to all coding conventions and patterns.
    3.  **Test:** Create component tests for all supported platforms and run the full test suite locally.
    4.  **Lint:** Run `pre-commit` to ensure code is compliant.
    5.  **Commit:** Commit your changes. There is no strict format for commit messages.
    6.  **Pull Request:** Submit a PR against the `dev` branch. The Pull Request title should have a prefix of the component being worked on (e.g., `[display] Fix bug`, `[abc123] Add new component`). Update documentation, examples, and add `CODEOWNERS` entries as needed. Pull requests should always be made using the `.github/PULL_REQUEST_TEMPLATE.md` template - fill out all sections completely without removing any parts of the template.

*   **Documentation Contributions:**
    *   Documentation is hosted in the separate `esphome/esphome.io` repository.
    *   The contribution workflow is the same as for the codebase.
    *   When editing a component's documentation page, also update the corresponding component index page to ensure both pages remain in sync.

*   **Best Practices:**
    *   **Component Development:** Keep dependencies minimal, provide clear error messages, and write comprehensive docstrings and tests.
    *   **Code Generation:** Generate minimal and efficient C++ code. Validate all user inputs thoroughly. Support multiple platform variations.
    *   **Configuration Design:** Aim for simplicity with sensible defaults, while allowing for advanced customization.
    *   **Embedded Systems Optimization:** ESPHome targets resource-constrained microcontrollers. Be mindful of flash size and RAM usage.

        **Why Heap Allocation Matters:**

        ESP devices run for months with small heaps shared between Wi-Fi, BLE, LWIP, and application code. Over time, repeated allocations of different sizes fragment the heap. Failures happen when the largest contiguous block shrinks, even if total free heap is still large. We have seen field crashes caused by this.

        **Heap allocation after `setup()` should be avoided unless absolutely unavoidable.** Every allocation/deallocation cycle contributes to fragmentation. ESPHome treats runtime heap allocation as a long-term reliability bug, not a performance issue. Helpers that hide allocation (`std::string`, `std::to_string`, string-returning helpers) are being deprecated and replaced with buffer and view based APIs.

        **STL Container Guidelines:**

        ESPHome runs on embedded systems with limited resources. Choose containers carefully:

        1. **Compile-time-known sizes:** Use `std::array` instead of `std::vector` when size is known at compile time.
           ```cpp
           // Bad - generates STL realloc code
           std::vector<int> values;

           // Good - no dynamic allocation
           std::array<int, MAX_VALUES> values;
           ```
           Use `cg.add_define("MAX_VALUES", count)` to set the size from Python configuration.

           **For byte buffers:** Avoid `std::vector<uint8_t>` unless the buffer needs to grow. Use `std::unique_ptr<uint8_t[]>` instead.

           > **Note:** `std::unique_ptr<uint8_t[]>` does **not** provide bounds checking or iterator support like `std::vector<uint8_t>`. Use it only when you do not need these features and want minimal overhead.

           ```cpp
           // Bad - STL overhead for simple byte buffer
           std::vector<uint8_t> buffer;
           buffer.resize(256);

           // Good - minimal overhead, single allocation
           std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(256);
           // Or if size is constant:
           std::array<uint8_t, 256> buffer;
           ```

        2. **Compile-time-known fixed sizes with vector-like API:** Use `StaticVector` from `esphome/core/helpers.h` for compile-time fixed size with `push_back()` interface (no dynamic allocation).
           ```cpp
           // Bad - generates STL realloc code (_M_realloc_insert)
           std::vector<ServiceRecord> services;
           services.reserve(5);  // Still includes reallocation machinery

           // Good - compile-time fixed size, no dynamic allocation
           StaticVector<ServiceRecord, MAX_SERVICES> services;
           services.push_back(record1);
           ```
           Use `cg.add_define("MAX_SERVICES", count)` to set the size from Python configuration.
           Like `std::array` but with vector-like API (`push_back()`, `size()`) and no STL reallocation code.

        3. **Runtime-known sizes:** Use `FixedVector` from `esphome/core/helpers.h` when the size is only known at runtime initialization.
           ```cpp
           // Bad - generates STL realloc code (_M_realloc_insert)
           std::vector<TxtRecord> txt_records;
           txt_records.reserve(5);  // Still includes reallocation machinery

           // Good - runtime size, single allocation, no reallocation machinery
           FixedVector<TxtRecord> txt_records;
           txt_records.init(record_count);  // Initialize with exact size at runtime
           ```
           **Benefits:**
           - Eliminates `_M_realloc_insert`, `_M_default_append` template instantiations (saves 200-500 bytes per instance)
           - Single allocation, no upper bound needed
           - No reallocation overhead
           - Compatible with protobuf code generation when using `[(fixed_vector) = true]` option

        4. **Small datasets (1-16 elements):** Use `std::vector` or `std::array` with simple structs instead of `std::map`/`std::set`/`std::unordered_map`.
           ```cpp
           // Bad - 2KB+ overhead for red-black tree/hash table
           std::map<std::string, int> small_lookup;
           std::unordered_map<int, std::string> tiny_map;

           // Good - simple struct with linear search (std::vector is fine)
           struct LookupEntry {
             const char *key;
             int value;
           };
           std::vector<LookupEntry> small_lookup = {
             {"key1", 10},
             {"key2", 20},
             {"key3", 30},
           };
           // Or std::array if size is compile-time constant:
           // std::array<LookupEntry, 3> small_lookup = {{ ... }};
           ```
           Linear search on small datasets (1-16 elements) is often faster than hashing/tree overhead, but this depends on lookup frequency and access patterns. For frequent lookups in hot code paths, the O(1) vs O(n) complexity difference may still matter even for small datasets. `std::vector` with simple structs is usually fine—it's the heavy containers (`map`, `set`, `unordered_map`) that should be avoided for small datasets unless profiling shows otherwise.

        5. **Avoid `std::deque`:** It allocates in 512-byte blocks regardless of element size, guaranteeing at least 512 bytes of RAM usage immediately. This is a major source of crashes on memory-constrained devices.

        6. **Detection:** Look for these patterns in compiler output:
           - Large code sections with STL symbols (vector, map, set)
           - `alloc`, `realloc`, `dealloc` in symbol names
           - `_M_realloc_insert`, `_M_default_append` (vector reallocation)
           - Red-black tree code (`rb_tree`, `_Rb_tree`)
           - Hash table infrastructure (`unordered_map`, `hash`)

        **Prioritize optimization effort for:**
        - Core components (API, network, logger)
        - Widely-used components (mdns, wifi, ble)
        - Components causing flash size complaints

        Note: Avoiding heap allocation after `setup()` is always required regardless of component type. The prioritization above is about the effort spent on container optimization (e.g., migrating from `std::vector` to `StaticVector`).

        **Callback Managers:**

        ESPHome provides two callback manager types in `esphome/core/helpers.h` for the observer pattern. Both support `std::function`, lambdas, and lightweight forwarder structs via their templatized `add()` method.

        | Type | Idle overhead (32-bit) | When to use |
        |------|----------------------|-------------|
        | `CallbackManager<void(Ts...)>` | 12 bytes (empty `std::vector`) | Callbacks are always or almost always registered |
        | `LazyCallbackManager<void(Ts...)>` | 4 bytes (`nullptr`) | Callbacks are often not registered (common case) |

        `LazyCallbackManager` is a drop-in replacement for `CallbackManager` that defers allocation until the first callback is added. Prefer it for entity-level callbacks where most instances have no subscribers.

        **Important:** Registration methods that add to a callback manager **must always be templatized** to accept both `std::function` and pointer-sized forwarder structs (used by `build_callback_automation`). Never use `std::function` in the method signature:
        ```cpp
        // Bad -- forces heap allocation for forwarder structs
        void add_on_state_callback(std::function<void(bool)> &&callback) {
          this->state_callback_.add(std::move(callback));
        }

        // Good -- accepts any callable without forcing std::function wrapping
        template<typename F> void add_on_state_callback(F &&callback) {
          this->state_callback_.add(std::forward<F>(callback));
        }
        ```

    *   **State Management:** Use `CORE.data` for component state that needs to persist during configuration generation. Avoid module-level mutable globals.

        **Bad Pattern (Module-Level Globals):**
        ```python
        # Don't do this - state persists between compilation runs
        _component_state = []
        _use_feature = None

        def enable_feature():
            global _use_feature
            _use_feature = True
        ```

        **Bad Pattern (Flat Keys):**
        ```python
        # Don't do this - keys should be namespaced under component domain
        MY_FEATURE_KEY = "my_component_feature"
        CORE.data[MY_FEATURE_KEY] = True
        ```

        **Good Pattern (dataclass):**
        ```python
        from dataclasses import dataclass, field
        from esphome.core import CORE

        DOMAIN = "my_component"

        @dataclass
        class MyComponentData:
            feature_enabled: bool = False
            item_count: int = 0
            items: list[str] = field(default_factory=list)

        def _get_data() -> MyComponentData:
            if DOMAIN not in CORE.data:
                CORE.data[DOMAIN] = MyComponentData()
            return CORE.data[DOMAIN]

        def request_feature() -> None:
            _get_data().feature_enabled = True

        def add_item(item: str) -> None:
            _get_data().items.append(item)
        ```

        If you need a real-world example, search for components that use `@dataclass` with `CORE.data` in the codebase. Note: Some components may use `TypedDict` for dictionary-based storage; both patterns are acceptable depending on your needs.

        **Why this matters:**
        - Module-level globals persist between compilation runs if the host process (e.g. device-builder) doesn't fork/exec
        - `CORE.data` automatically clears between runs
        - Namespacing under `DOMAIN` prevents key collisions between components
        - `@dataclass` provides type safety and cleaner attribute access

*   **Security:** Be mindful of security when making changes to the API, web server, or any other network-related code. Do not hardcode secrets or keys.

*   **Dependencies & Build System Integration:**
    *   **Python:** When adding a new Python dependency, add it to the appropriate `requirements*.txt` file and `pyproject.toml`.
    *   **C++ / PlatformIO:** When adding a new C++ dependency, add it to `platformio.ini` and use `cg.add_library`.
    *   **Build Flags:** Use `cg.add_build_flag(...)` to add compiler flags.
    *   **ESP-IDF Managed Components:** For ESP-IDF managed components from the ESP Component Registry:

        **How to add ESP-IDF components:**
        ```python
        from esphome.components.esp32 import add_idf_component

        # Add component from ESP Component Registry
        add_idf_component(name="espressif/esp_jpeg", ref="1.3.1")
        ```

        **Finding components and versions:**
        1. Browse ESP Component Registry: https://components.espressif.com/
        2. Search for the component (e.g., "esp_jpeg")
        3. Note the latest version (e.g., "1.3.1")
        4. Component name format: `espressif/component_name` or `vendor/component_name`

        **Examples from existing components:**
        ```python
        # From esp32_hosted component:
        add_idf_component(name="espressif/esp_wifi_remote", ref="1.1.5")
        add_idf_component(name="espressif/eppp_link", ref="1.1.3")
        add_idf_component(name="espressif/esp_hosted", ref="2.6.1")

        # From picture_viewer component:
        add_idf_component(name="espressif/esp_jpeg", ref="1.3.1")
        ```

        **What happens:**
        - ESP-IDF component manager downloads the component during build
        - Component is added to `idf_component.yml` in the build directory
        - Headers become available for `#include` in C++ code
        - Library is automatically linked

        **Platform-specific component loading:**
        ```python
        from esphome.components.esp32 import get_esp32_variant, add_idf_component

        variant = get_esp32_variant()
        if variant == "esp32s2" or variant == "esp32s3":
            add_idf_component(name="espressif/esp_jpeg", ref="1.3.1")
            cg.add_define("USE_ESP_JPEG_DECODER")
        elif variant == "esp32p4":
            cg.add_define("USE_HARDWARE_JPEG_DECODER")
        ```

        **Common patterns:**
        - Always specify `ref` parameter with exact version for reproducible builds
        - Check ESP Component Registry for latest stable version
        - Use platform detection to load components only where supported
        - Add corresponding `#define` to enable conditional compilation in C++

## 8. Public API and Breaking Changes

*   **Public C++ API:**
    *   **Components**: Only documented features at [esphome.io](https://esphome.io) are public API. Undocumented `public` members are internal.
    *   **Core/Base Classes** (`esphome/core/`, `Component`, `Sensor`, etc.): All `public` members are public API.
    *   **Components with Global Accessors** (`global_api_server`, etc.): All `public` members are public API (except config setters).

*   **Public Python API:**
    *   All documented configuration options at [esphome.io](https://esphome.io) are public API.
    *   Python code in `esphome/core/` actively used by existing core components is considered stable API.
    *   Other Python code is internal unless explicitly documented for external component use.

*   **Breaking Changes Policy:**
    *   Aim for **6-month deprecation window** when possible
    *   Clean breaks allowed for: signature changes, deep refactorings, resource constraints
    *   Must document migration path in PR description (generates release notes)
    *   Blog post required for core/base class changes or significant architectural changes
    *   Full details: https://developers.esphome.io/contributing/code/#public-api-and-breaking-changes

*   **Breaking Change Checklist:**
    - [ ] Clear justification (RAM/flash savings, architectural improvement)
    - [ ] Explored non-breaking alternatives
    - [ ] Added deprecation warnings if possible (use `ESPDEPRECATED` macro for C++)
    - [ ] Documented migration path in PR description with before/after examples
    - [ ] Updated all internal usage and esphome.io
    - [ ] Tested backward compatibility during deprecation period

*   **Deprecation Pattern (C++):**
    ```cpp
    // Remove before 2026.6.0
    ESPDEPRECATED("Use new_method() instead. Removed in 2026.6.0", "2025.12.0")
    void old_method() { this->new_method(); }
    ```

*   **Deprecation Pattern (Python):**
    ```python
    # Remove before 2026.6.0
    if CONF_OLD_KEY in config:
        _LOGGER.warning(f"'{CONF_OLD_KEY}' deprecated, use '{CONF_NEW_KEY}'. Removed in 2026.6.0")
        config[CONF_NEW_KEY] = config.pop(CONF_OLD_KEY)  # Auto-migrate
    ```
## 9. English Language

The project uses English for non-code content. When drafting documentation, code comments, commit messages,
PR descriptions, and similar text, avoid technical jargon. Instead, express concepts in plain English,
using standard technical terms only when required. Ensure the text is readily comprehensible to a wide
audience, including non-native English speakers.
