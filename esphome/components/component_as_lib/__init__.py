"""component_as_lib -- build selected existing components as loadable .so libraries.

This component adds NO per-component code and does not edit esphome core. It runs at FINAL codegen
priority, after every other component's to_code has appended its construct+configure+register
statements to CORE.main_statements. For each configured target it:

  1. locates that target's slice of main_statements (using the `// <name>:` markers that
     esphome/__main__.py:_wrap_to_code already emits before every component, plus the target's own
     id name appearing inside the slice to disambiguate multiple instances of one platform),
  2. removes that slice from main_statements (so it is NOT emitted into the firmware; the firmware
     then has no reference to the component and --gc-sections drops it -- automatic exclusion),
  3. renders the slice into a generated .so entry `extern "C" void __lib_construct_<n>()` written to
     a glue file the post-build script compiles into <target>.so,
  4. registers the post-build script that builds the .so's, generates the targeted host-symbol table,
     and relinks the firmware (see build_libs.py.script).

NOTE (first iteration): this file also DUMPS the real main_statements structure at FINAL, because the
exact marker text and slice boundaries can only be confirmed on a real ESPHome run. The dump lets the
first `esphome compile` show the ground truth so the slice logic can be tightened from facts.
"""

import logging
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.cpp_generator import LineComment

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@p1ngb4ck"]

CONF_COMPONENTS = "components"
CONF_DUMP_STATEMENTS = "dump_statements"

# The generated glue + the .so build output live under the build src tree so the post-build script
# and the copied-source tree see them.
GLUE_SUBDIR = "component_libs"

CONFIG_SCHEMA = cv.Schema(
    {
        # List of component INSTANCE ids to provide as .so libraries. These must resolve to real
        # component ids elsewhere in the YAML.
        cv.Required(CONF_COMPONENTS): cv.ensure_list(cv.use_id(cg.Component)),
        # First-iteration aid: dump the FINAL main_statements structure to the log.
        cv.Optional(CONF_DUMP_STATEMENTS, default=True): cv.boolean,
    }
)


def _marker_name(stmt) -> str | None:
    """If stmt is a component marker LineComment (`<name>:`), return <name>, else None."""
    if not isinstance(stmt, LineComment):
        return None
    text = str(stmt).strip()
    # LineComment renders as `// <value>`; _wrap_to_code's value is f"{name}:"
    if not text.startswith("// "):
        return None
    body = text[3:].strip()
    if body.endswith(":") and "\n" not in body and " " not in body:
        return body[:-1]
    return None


def _slice_bounds(statements):
    """Yield (start_index, end_index, marker_name) for each component slice delimited by markers.

    A slice runs from just after its marker to just before the next marker (or end of list).
    """
    marker_positions = []
    for i, s in enumerate(statements):
        name = _marker_name(s)
        if name is not None:
            marker_positions.append((i, name))
    for idx, (pos, name) in enumerate(marker_positions):
        start = pos + 1
        end = marker_positions[idx + 1][0] if idx + 1 < len(marker_positions) else len(statements)
        yield start, end, name


@coroutine_with_priority(CoroPriority.FINAL)
async def to_code(config):
    statements = CORE.main_statements

    if config[CONF_DUMP_STATEMENTS]:
        _LOGGER.info("component_as_lib: main_statements dump (%d entries)", len(statements))
        for i, s in enumerate(statements):
            rendered = str(s).replace("\n", " \\n ")
            _LOGGER.info("  [%3d] %-22s %s", i, type(s).__name__, rendered[:100])

    # Resolve the target ids to their rendered variable-name strings so we can find their slice.
    target_id_names = set()
    for var in config[CONF_COMPONENTS]:
        # cv.use_id yields the id object; its variable name is what new_Pvariable emitted.
        target_id_names.add(str(var))
    _LOGGER.info("component_as_lib: targets = %s", sorted(target_id_names))

    glue_dir = Path(CORE.relative_src_path("esphome", GLUE_SUBDIR))
    captured = []  # (target_name, [rendered_lines]) preserved for the post-build glue emission

    # Walk slices; a slice belongs to a target if the target's id name appears in its rendered text.
    # Collect the exact index ranges first, then delete from the tail so earlier indices stay valid.
    to_delete = []
    for start, end, marker_name in _slice_bounds(statements):
        slice_stmts = statements[start:end]
        slice_text = "\n".join(str(s) for s in slice_stmts)
        matched = [t for t in target_id_names if t in slice_text]
        if not matched:
            continue
        if len(matched) > 1:
            _LOGGER.warning(
                "component_as_lib: slice '%s' matched multiple targets %s; needs tighter matching",
                marker_name,
                matched,
            )
        target = matched[0]
        captured.append((target, marker_name, [str(s) for s in slice_stmts]))
        to_delete.append((start, end))

    # Remove captured slices from the firmware main() (tail-first to keep indices valid).
    for start, end in sorted(to_delete, reverse=True):
        del statements[start:end]

    if not captured:
        _LOGGER.warning(
            "component_as_lib: no target slices captured -- check ids and the marker format in the "
            "dump above before the .so build is meaningful"
        )
        return

    # Emit one glue file per captured target. The post-build script compiles these together with the
    # component's own sources into <target>.so. Rendering detail (includes, namespace) is confirmed
    # from the first on-device run; esphome.h pulls in all component types + the extern App.
    glue_dir.mkdir(parents=True, exist_ok=True)
    manifest = []
    for target, marker_name, lines in captured:
        safe = target.replace(":", "_")
        body = "\n".join(f"  {ln};" if not ln.rstrip().endswith((";", "{", "}")) else f"  {ln}"
                         for ln in lines)
        glue = (
            "// Generated by component_as_lib. Runs the captured construct+configure+register\n"
            f"// sequence for '{marker_name}' inside the .so; wires into the firmware App via the\n"
            "// exported symbol table.\n"
            '#include "esphome.h"\n\n'
            "using namespace esphome;  // NOLINT\n\n"
            f'extern "C" void __lib_construct_{safe}() {{\n'
            f"{body}\n"
            "}\n"
        )
        out = glue_dir / f"{safe}.lib.cpp"
        out.write_text(glue, encoding="utf-8")
        manifest.append({"target": target, "marker": marker_name, "glue": str(out)})
        _LOGGER.info("component_as_lib: wrote glue for '%s' -> %s", marker_name, out)

    # Hand the manifest to the post-build script (targets + glue files). The script also needs the
    # firmware ELF + compile_commands.json, both discoverable in the build dir at post-build time.
    (glue_dir / "manifest.json").write_text(
        __import__("json").dumps(manifest, indent=2), encoding="utf-8"
    )

    # Register the post-build step (build .so's, targeted symbol export, relink). esp32-only hook.
    from esphome.components.esp32 import add_extra_script

    add_extra_script(
        "post",
        "component_as_lib_build.py",
        Path(__file__).parent / "build_libs.py.script",
    )
