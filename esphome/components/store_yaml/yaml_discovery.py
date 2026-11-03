"""Compatibility layer over esphome.yaml_util for store_yaml.

Makes the component fully self-contained (usable as an external component,
zero core diff) across ESPHome versions. The discovery machinery upstream
predates the store_yaml PR and differs in shape (DiscoveredYamlFiles without
unresolved/load_errors, force_load_include_files returning None instead of a
result tuple) — so this module always returns its own PR-shaped result type
and normalizes whatever the running ESPHome provides:

- File tracking never relies on upstream internals' shape: the loader core
  yaml_util._load_yaml_internal is temporarily wrapped during the discovery
  re-parse (the one long-stable choke point every loaded file passes
  through).
- force_load_include_files is used from upstream when present; its return is
  normalized (older versions only log unresolved includes, so the
  "uncaptured files" note in the embedded YAML is unavailable there — the
  files themselves are still force-loaded and captured). Very old versions
  without it resolve includes eagerly, so there is nothing to force-load.
- The secret helpers wrap the long-stable yaml_util._SECRET_VALUES mapping,
  preferring upstream implementations once the PR lands.
"""

from __future__ import annotations

from collections.abc import Generator
from contextlib import contextmanager
from dataclasses import dataclass, field
import logging
from pathlib import Path
import re

from esphome import yaml_util
from esphome.core import EsphomeError
from esphome.yaml_util import load_yaml

_LOGGER = logging.getLogger(__name__)


@dataclass(slots=True)
class DiscoveredYamlFiles:
    """PR-shaped discovery result (superset of the upstream dataclass).

    ``files``: every resolved path the YAML loader touched during the
    re-parse. ``secrets``: the subset whose un-resolved filename matched
    SECRETS_FILES. ``unresolved``: ``!include`` path strings that contain
    substitution variables and could not be loaded (only fillable when the
    running ESPHome reports them, see module docstring). ``load_errors``:
    files that failed to parse or load. Consumers should treat ``files`` as
    incomplete when either of the last two is non-empty.
    """

    files: list[Path] = field(default_factory=list)
    secrets: set[Path] = field(default_factory=set)
    unresolved: list[str] = field(default_factory=list)
    load_errors: list[str] = field(default_factory=list)


@contextmanager
def _load_hook(callbacks) -> Generator[None]:
    """Wrap yaml_util._load_yaml_internal so ``callbacks`` see every file the
    loader opens while the context is active."""
    original = yaml_util._load_yaml_internal  # noqa: SLF001 — deliberate, see module docstring

    def wrapper(fname, *args, **kwargs):
        for cb in callbacks:
            cb(fname)
        return original(fname, *args, **kwargs)

    yaml_util._load_yaml_internal = wrapper  # noqa: SLF001
    try:
        yield
    finally:
        yaml_util._load_yaml_internal = original  # noqa: SLF001


@contextmanager
def track_yaml_loads() -> Generator[list[Path]]:
    """Context manager that records every file loaded by the YAML loader."""
    loaded: list[Path] = []
    with _load_hook([lambda fname: loaded.append(Path(fname).resolve())]):
        yield loaded


def _force_load_includes(data) -> tuple[list[str], list[str]]:
    """Resolve deferred !include nodes via upstream machinery, tolerating
    every historical signature/return shape. Returns (unresolved, errors)."""
    fn = getattr(yaml_util, "force_load_include_files", None)
    if fn is None:
        # Pre-lazy-include ESPHome: includes were resolved eagerly during
        # load_yaml, so everything is already captured by the load hook.
        return [], []
    try:
        res = fn(data, warn_on_unresolved=False)
    except TypeError:  # older signature without the kwarg
        res = fn(data)
    if res is None:
        # Older upstream shape: unresolved includes are only logged, not
        # reported — the embedded YAML then simply lacks the uncaptured note.
        return [], []
    # PR shape: NamedTuple/tuple of (unresolved, errors).
    unresolved, errors = res
    return list(unresolved), list(errors)


def discover_user_yaml_files(config_path: Path) -> DiscoveredYamlFiles:
    """Fresh-re-parse ``config_path`` and report every file the YAML loader
    pulled in, plus which of them came in under a secrets filename.

    Does NOT run schema validation, substitutions, or package resolution — so
    component-internal YAML loaded by validators is *not* captured. Must run
    on a fresh parse because deferred-include loading caches its result.
    """
    from esphome.const import SECRETS_FILES

    loaded: list[Path] = []
    secrets: set[Path] = set()

    def _capture(fname) -> None:
        p = Path(fname)
        loaded.append(p.resolve())
        if p.name in SECRETS_FILES:
            secrets.add(p.resolve())

    with _load_hook([_capture]):
        try:
            data = load_yaml(config_path)
        except EsphomeError as err:
            _LOGGER.warning("YAML discovery failed to parse %s: %s", config_path, err)
            return DiscoveredYamlFiles(
                list(loaded), secrets, load_errors=[f"{config_path}: {err}"]
            )
        unresolved, load_errors = _force_load_includes(data)

    # Deduplicate while preserving first-seen order.
    seen: set[Path] = set()
    unique: list[Path] = []
    for path in loaded:
        if path not in seen:
            seen.add(path)
            unique.append(path)
    return DiscoveredYamlFiles(unique, secrets, unresolved, load_errors)


# Matches !secret references in YAML text. An optional surrounding quote pair
# around the key is allowed and ignored: YAML treats ``!secret 'foo'`` and
# ``!secret foo`` as the same key. Intentionally a simple regex scan rather
# than a YAML parse — it may match inside comments or multi-line strings,
# which is the conservative direction (include more secrets rather than
# fewer).
_SECRET_REFERENCE_RE = re.compile(r"""!secret\s+['"]?([^\s'"]+)""")


def _find_secret_references(text: str) -> set[str]:
    """Return the ``!secret <key>`` names referenced in a YAML document text."""
    return {match.group(1) for match in _SECRET_REFERENCE_RE.finditer(text)}


def _registered_secret_names() -> set[str]:
    """Names of all ``!secret`` keys the loader has seen since the last clear."""
    return set(yaml_util._SECRET_VALUES.values())  # noqa: SLF001


@contextmanager
def _secret_values_registered(values: dict[str, str]) -> Generator[None]:
    """Temporarily register value→name mappings so :func:`yaml_util.dump`
    renders those scalars as ``!secret <name>``.

    Mappings already present (values loaded through a real ``!secret``) win
    over the supplied ones and are left untouched.
    """
    secret_values = yaml_util._SECRET_VALUES  # noqa: SLF001
    added = {v: n for v, n in values.items() if v not in secret_values}
    secret_values.update(added)
    try:
        yield
    finally:
        for value in added:
            secret_values.pop(value, None)


# Prefer the upstream implementations once they exist (they will, when the
# PR lands); fall back to the local ports above until then.
find_secret_references = getattr(yaml_util, "find_secret_references", _find_secret_references)
registered_secret_names = getattr(yaml_util, "registered_secret_names", _registered_secret_names)
secret_values_registered = getattr(yaml_util, "secret_values_registered", _secret_values_registered)
