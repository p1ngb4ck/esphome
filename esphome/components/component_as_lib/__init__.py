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
     and relinks the firmware (done in the helper IDF component, next iteration).

NOTE (first iteration): this file also DUMPS the real main_statements structure at FINAL, because the
exact marker text and slice boundaries can only be confirmed on a real ESPHome run. The dump lets the
first `esphome compile` show the ground truth so the slice logic can be tightened from facts.
"""

import logging
import re
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

# Helper IDF component (separate repo) that ships the project_include.cmake driving the in-build .so
# build. Adjust these if the repo is renamed/moved.
LIB_BUILDER_NAME = "p1ngb4ck/esphome_component_as_lib"
LIB_BUILDER_REPO = "https://github.com/p1ngb4ck/esphome_component_as_lib.git"
LIB_BUILDER_REF = "main"

CONFIG_SCHEMA = cv.Schema(
    {
        # List of component / platform NAMES to provide as .so libraries -- e.g. "dht" or the fully
        # qualified marker "sensor.dht". A component is the shared code unit (one source dir under
        # components/<name>/); ALL entities using it are captured together into one <name>.so.
        # Components do not have ids -- their entities do -- so targeting is by name, not id.
        cv.Required(CONF_COMPONENTS): cv.ensure_list(cv.string_strict),
        # First-iteration aid: dump the FINAL main_statements structure to the log.
        cv.Optional(CONF_DUMP_STATEMENTS, default=True): cv.boolean,
    }
)


def _marker_matches_target(marker_name: str, target: str) -> bool:
    """Match a component NAME against a codegen marker.

    A component's code can appear under several markers: its own domain ('mcp4461'), a sub-platform
    of it ('mcp4461.xxx'), or it as a platform under another domain ('output.mcp4461'). So a target
    matches when it equals the marker, the marker's domain part, or the marker's platform part.

    Note: template-based platform entities (e.g. 'cover.template' -> template_::TemplateCover) are the
    exception -- their class lives in the shared 'template' component, not in 'cover'. Those are best
    targeted as 'template' (which pulls the whole shared component), not as 'cover'.
    """
    if marker_name == target:
        return True
    parts = marker_name.split(".")
    return parts[0] == target or parts[-1] == target


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

    targets = list(config[CONF_COMPONENTS])
    _LOGGER.info("component_as_lib: targets = %s", targets)

    # IMPORTANT: NOT under src/ -- anything under src/ is globbed into the FIRMWARE build. The glue
    # is for the .so only, so it goes to a sibling build dir the post-build script reads.
    glue_dir = Path(CORE.relative_build_path(GLUE_SUBDIR))

    # Capture, per target, EVERY slice whose component marker matches it (all entities of that
    # component). Collect index ranges first, delete tail-first so earlier indices stay valid.
    per_target_lines = {t: [] for t in targets}
    per_target_marker = {}
    to_delete = []
    for start, end, marker_name in _slice_bounds(statements):
        matched = [t for t in targets if _marker_matches_target(marker_name, t)]
        if not matched:
            continue
        if len(matched) > 1:
            _LOGGER.warning("component_as_lib: marker '%s' matched multiple targets %s; using first",
                            marker_name, matched)
        target = matched[0]
        slice_stmts = statements[start:end]
        # separate each entity's slice with a blank line inside the shared __lib_construct
        if per_target_lines[target]:
            per_target_lines[target].append("")
        per_target_lines[target].extend(str(s) for s in slice_stmts)
        per_target_marker.setdefault(target, marker_name)
        to_delete.append((start, end))

    for start, end in sorted(to_delete, reverse=True):
        del statements[start:end]

    captured = [(t, per_target_marker[t], per_target_lines[t])
                for t in targets if per_target_lines[t]]
    missing = [t for t in targets if not per_target_lines[t]]
    if missing:
        _LOGGER.warning("component_as_lib: no slices captured for %s -- check the marker names in the "
                        "dump above", missing)
    if not captured:
        return

    # Emit one glue file per captured target. The post-build script compiles these together with the
    # component's own sources into <target>.so. Rendering detail (includes, namespace) is confirmed
    # from the first on-device run; esphome.h pulls in all component types + the extern App.
    glue_dir.mkdir(parents=True, exist_ok=True)

    # Build a name -> constructed-type map from EVERY placement-new, so both the target's own globals
    # and any external dependency it references can be declared/cast with the right type. e.g.
    # "i2c_bus_a" -> "i2c::IDFI2CBus", "mcp4461_output" -> "mcp4461::Mcp4461Component". The captured
    # slices were already removed from `statements`, so union the remaining statements (external deps)
    # with the captured lines (own globals) -- otherwise own-global types resolve to void.
    all_text = "\n".join(str(st) for st in statements)
    for _t, _m, _lines in captured:
        all_text += "\n" + "\n".join(_lines)
    type_of = {}
    for m in re.finditer(r"new\((\w+)\)\s+([A-Za-z_][\w:]*(?:<[^;]*?>)?)", all_text):
        type_of.setdefault(m.group(1), m.group(2))

    manifest = []
    for target, marker_name, lines in captured:
        # Only real code lines (skip the config-dump LineComments) for analysis + emission.
        code = [ln for ln in lines if not ln.lstrip().startswith("//")]
        text = "\n".join(code)

        own = []
        for ln in code:
            for nm in re.findall(r"new\((\w+)\)", ln):
                if nm not in own:
                    own.append(nm)
        own_set = set(own)

        # external refs: identifiers that are known constructed vars elsewhere but not own here
        refs = set(re.findall(r"\b(\w+)\b", text))
        external = [r for r in sorted(refs) if r in type_of and r not in own_set]

        _LOGGER.info("component_as_lib: '%s' own=%s external=%s", target, own, external)

        # rewrite each own global's placement-new into heap-new: `new(x) T(a)` -> `x = new T(a)`
        body_lines = []
        for ln in code:
            ln2 = re.sub(r"\bnew\((\w+)\)\s+", r"\1 = new ", ln)
            body_lines.append(f"  {ln2}")
        body = "\n".join(body_lines)

        # own-global pointer declarations (type from the placement-new)
        missing_types = [n for n in own if n not in type_of]
        if missing_types:
            _LOGGER.error("component_as_lib: '%s' could not resolve types for %s -- glue will be "
                          "wrong; check the placement-new rendering", target, missing_types)
        decls = "\n".join(
            f"static {type_of.get(n, 'void')} *{n};" for n in own
        )
        # external dependency casts from the passed deps[] array (type from its construction elsewhere)
        dep_casts = "\n".join(
            f"  auto *{d} = static_cast<{type_of[d]} *>(deps[{i}]);"
            for i, d in enumerate(external)
        )

        safe = target.replace(":", "_").replace(".", "_")
        glue = (
            "// Generated by component_as_lib. The .so OWNS its own entity globals (heap-allocated\n"
            "// here) and receives external firmware dependencies via deps[] (they are 'static' in\n"
            "// main.cpp and cannot be linked by name). Wires into the firmware App via exported symbols.\n"
            '#include "esphome.h"\n\n'
            "using namespace esphome;  // NOLINT\n\n"
            f"{decls}\n\n"
            f'extern "C" void __lib_construct_{safe}(void **deps) {{\n'
            "  (void) deps;\n"
            f"{dep_casts}\n"
            f"{body}\n"
            "}\n"
        )
        out = glue_dir / f"{safe}.lib.cpp"
        out.write_text(glue, encoding="utf-8")
        manifest.append({
            "target": target, "marker": marker_name, "glue": str(out),
            "symbol": f"__lib_construct_{safe}", "own": own, "external": external,
            "external_types": [type_of[d] for d in external],
        })
        _LOGGER.info("component_as_lib: wrote glue for '%s' -> %s (deps=%s)", marker_name, out, external)

    (glue_dir / "manifest.json").write_text(
        __import__("json").dumps(manifest, indent=2), encoding="utf-8"
    )

    # Native ESP-IDF hook (no PlatformIO, no core edits): pull in the helper IDF component, whose
    # project_include.cmake compiles the emitted glue + component sources into <target>.so as a
    # project-scope custom target after the firmware binary is built. Gated on the manifest existing.
    from esphome.components.esp32 import add_idf_component

    add_idf_component(name=LIB_BUILDER_NAME, repo=LIB_BUILDER_REPO, ref=LIB_BUILDER_REF)
    _LOGGER.info("component_as_lib: .so build runs in-build via %s (%s@%s)",
                 LIB_BUILDER_NAME, LIB_BUILDER_REPO, LIB_BUILDER_REF)
