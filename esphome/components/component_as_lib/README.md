# component_as_lib (first iteration -- diagnostic-first)

Builds selected EXISTING components (by name -- a component is the shared code unit; its entities
hold the ids, so targeting is by component/platform name, capturing all its entities) as loadable .so
libraries, with no per-component changes and no
esphome core edits. Runs at FINAL codegen priority, captures each target's construct+configure+
register statements out of CORE.main_statements, redirects them into a generated .so entry
(__lib_construct_<target>), and registers a post-build step to build the .so + targeted host-symbol
table + relink. The firmware then has no reference to the component, so --gc-sections drops it
(automatic exclusion). Load the resulting .so via module_host.

## Why "diagnostic-first"

The exact content of CORE.main_statements (marker text, slice boundaries) and the post-build build
tree (compile_commands.json location, ELF name, per-component source set) can only be confirmed on a
real ESPHome build. So this first version LOGS the ground truth on the first `esphome compile`:
- __init__.py dumps the FINAL main_statements structure and which target slices it captured.
- the helper IDF component's tools/build_libs.py logs the discovered compile_commands.json + builds
  each .so + reports its undefined symbols (run in-build after the firmware binary).
  source dir, builds the .so's, and reports each .so's undefined symbols -- but deliberately does NOT
  yet generate the symbol table or relink the firmware (that is the next iteration, once the
  discovery output confirms the paths).

## Config

    component_as_lib:
      components: [dht]                  # component/platform NAMES (not entity ids); all entities
                                        # of that component are captured together into one dht.so
      # dump_statements: true           # (default) log the main_statements structure

    module_host:                        # load the produced .so (validation pairing comes later)
      - path: /flash/dht.so

## What to send back after the first run

- the `component_as_lib: main_statements dump` lines + the `targets` and any capture warnings,
- the post-build discovery lines (build dir, compile_commands.json, ELF, comp_dir guesses),
- each `.so has N undefined symbols` line.

From those facts the slice matching, the component-source mapping, and the symbol-table + relink step
get tightened -- no more guessing.
