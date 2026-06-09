# ESPHome Build Workflow - CRITICAL UNDERSTANDING

## Repository Structure

1. **Official ESPHome GitHub Repo** - The base ESPHome repository
   - Located at: https://github.com/esphome/esphome
   - Used as the BASE for compilation
   - Contains the core ESPHome framework

2. **This Workspace** (`/workspaces/esphome/`)
   - A MODIFIED clone of the official ESPHome repo
   - Contains our custom additions and modifications to components
   - This is where we develop our custom components

3. **External Components (PR sources, etc.)**
   - Can be copied into our workspace
   - Can be modified as needed
   - Example: USB audio PR can be integrated here

## Build Process - HOW IT ACTUALLY WORKS

### Critical Rule: External Components REPLACE, Not Double

When building:
1. **Base compilation** uses the OFFICIAL ESPHome repo
2. **YAML configuration** specifies `external_components` pointing to our workspace
3. **external_components REPLACES the official component** - it does NOT create duplicates
4. Only the components specified in external_components are pulled from our workspace
5. All other components come from the official base

### Example Flow:
```
Official Repo: audio/, speaker/, usb_host/, etc.
Our Workspace: audio/ (modified), speaker/ (modified), usb_audio/ (new)
YAML external_components: pulls audio/, speaker/, usb_audio/ from our workspace
Result: Official base + OUR versions of audio/, speaker/, usb_audio/ (REPLACED, not doubled)
```

## What Can Be Modified

### DO NOT MODIFY:
- `esphome/core/` - Core ESPHome framework
- Core infrastructure that other components depend on
- Build system internals

### CAN MODIFY:
- Individual components in `esphome/components/`
- Add new components
- Extend existing components
- External PR components (like USB audio)

## Common Mistakes to Avoid

1. **WRONG:** Assuming external_components creates duplicates
   - **RIGHT:** External_components REPLACES official components

2. **WRONG:** Modifying core when the issue is in a component
   - **RIGHT:** Fix component-specific issues in the component itself

3. **WRONG:** Removing workspace modifications to "fix" duplicates
   - **RIGHT:** Investigate the actual cause of redefinition errors in the code

## Compilation Environment

- Compilation happens EXTERNALLY (not in this workspace)
- The external build system pulls from:
  - Official ESPHome base
  - Our workspace components via external_components configuration
- Hash-based cache paths like `61a01c5f` are generated during build

## Reference Locations

- Official repo comparison: `/tmp/esphome-official/` (when cloned for comparison)
- Our workspace: `/workspaces/esphome/`
- Build cache: `/home/esphome/.espconfig/.esphome/external_components/` (external build system)
