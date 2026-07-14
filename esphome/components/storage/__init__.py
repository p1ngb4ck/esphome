from dataclasses import dataclass
import hashlib
import re

from esphome import automation, core
import esphome.codegen as cg
from esphome.components import globals as globals_
import esphome.config_validation as cv
from esphome.const import (
    CONF_ARGS,
    CONF_FORMAT,
    CONF_FROM,
    CONF_GROUP,
    CONF_ID,
    CONF_INDEX,
    CONF_KEY,
    CONF_ON_VALUE,
    CONF_PATH,
    CONF_TO,
    CONF_TYPE,
)
from esphome.core import CORE, ID, CoroPriority, coroutine_with_priority

CODEOWNERS = ["@p1ngb4ck"]

DOMAIN = "storage"

CONF_COPY_CHUNK_SIZE = "copy_chunk_size"
CONF_MAX_BLOCKING_TRANSFER_SIZE = "max_blocking_transfer_size"
CONF_TASK_STACK_SIZE = "task_stack_size"
CONF_TASK_PRIORITY = "task_priority"
CONF_MAX_PENDING = "max_pending"
CONF_MAX_STREAMS = "max_streams"

# Not yet in esphome/const.py
CONF_ON_REGISTERED = "on_registered"
CONF_ON_UNREGISTERED = "on_unregistered"

# json is header-only (ArduinoJson): auto-loading it costs nothing when unused
# and lets the json extract step and the preferences json format work without
# an explicit `json:` block in the config.
AUTO_LOAD = ["json"]

storage_ns = cg.esphome_ns.namespace("storage")
Storage = storage_ns.class_("Storage", cg.Component)
StoragePtr = Storage.operator("ptr")
PathStorage = storage_ns.class_("PathStorage", Storage)
MountableStorage = storage_ns.class_("MountableStorage")
StorageRegistry = storage_ns.class_("StorageRegistry", cg.Component)
StorageWorker = storage_ns.class_("StorageWorker", cg.Component)


def validate_sector_multiple(value):
    """Require a multiple of 512 (the common sector size).

    Anything else loses the FATFS direct-sector-read path that motivated picking a
    16kB chunk size in the first place — see STORAGE_COPY_CHUNK_SIZE's comment in storage.h.
    """
    if value % 512 != 0:
        raise cv.Invalid(f"copy_chunk_size must be a multiple of 512, got {value}")
    return value


# Default kept in sync with the STORAGE_COPY_CHUNK_SIZE fallback in storage.h.
# Lower bound matches copy()'s allocation fallback floor (4096, see storage.cpp); upper bound
# is a sanity cap so a typo can't request an unreasonable single allocation (e.g. 16777216).
#
# The task_*/max_pending keys only take effect when the async worker (storage_worker.h/.cpp,
# compiled in as USE_STORAGE_WORKER) is actually pulled in by a path-based driver, via that
# driver's own request_storage_worker() call in its to_code() (mirrors how sd_storage already
# calls request_storage_device()). If no such driver is configured, these keys are simply
# unused, same as any other config key with no effect in a given configuration.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StorageRegistry),
        cv.Optional(CONF_COPY_CHUNK_SIZE, default=16384): cv.All(
            cv.int_range(min=4096, max=131072), validate_sector_multiple
        ),
        # Guard-rail for the blocking copy/read/write helpers: 0 means unlimited (default,
        # preserves current behavior). See max_blocking_transfer_size's comment in storage.h.
        cv.Optional(CONF_MAX_BLOCKING_TRANSFER_SIZE, default=0): cv.int_range(min=0),
        # FATFS LFN + NFS/lwIP transfers both need headroom on the worker task's stack.
        cv.Optional(CONF_TASK_STACK_SIZE, default=8192): cv.int_range(
            min=4096, max=32768
        ),
        # FreeRTOS priority: above idle (0), below networking tasks (typically higher).
        cv.Optional(CONF_TASK_PRIORITY, default=1): cv.int_range(min=1, max=23),
        # Fixed request pool/queue depth — sized exactly at codegen like the storage
        # registry's device count, no heap allocation per request at runtime.
        cv.Optional(CONF_MAX_PENDING, default=4): cv.int_range(min=1, max=16),
        # Fixed stream pool depth (begin_write()/begin_read() and friends, storage_worker.h) —
        # streams are typically much longer-lived than a single copy/move (e.g. one HTTP
        # upload in progress), so a node doing one at a time needs very few slots.
        cv.Optional(CONF_MAX_STREAMS, default=2): cv.int_range(min=1, max=8),
        # Fired for every storage device, not just file-browser-style consumers — any
        # component that cares about hotplug/availability can listen here instead of
        # each reinventing its own notion of "storage changed". See
        # StorageRegistry::add_on_registered_callback()/add_on_unregistered_callback()
        # in storage.h.
        cv.Optional(CONF_ON_REGISTERED): automation.validate_automation({}),
        cv.Optional(CONF_ON_UNREGISTERED): automation.validate_automation({}),
    }
)


@dataclass
class StorageData:
    device_count: int = 0
    worker_count: int = 0
    worker_task_safe: bool = False


def _get_data() -> StorageData:
    if DOMAIN not in CORE.data:
        CORE.data[DOMAIN] = StorageData()
    return CORE.data[DOMAIN]


def request_storage_device() -> None:
    """Called by each storage driver's to_code() to count configured devices.

    The accumulated count is passed to StorageRegistry.set_device_count() so the
    internal FixedVector is sized exactly — no compile-time upper bound needed.
    """
    _get_data().device_count += 1


def request_storage_worker(task_safe: bool = False) -> None:
    """Called by path-based drivers (Filesystem/NetworkStorage) that need the async worker.

    RawStorage drivers never call this, so on a raw-only node storage_worker.h/.cpp is not
    even compiled in (see USE_STORAGE_WORKER below) — zero RAM/flash cost for the feature.

    task_safe should be True only if the driver's data-plane calls are safe to run from a
    background FreeRTOS task for every instance it registers (e.g. SdMmc, which owns its bus
    exclusively) — not if that safety depends on how the bus is shared (e.g. SdSpi, which
    shares its bus with other devices). This aggregates via OR across all callers: if any
    driver requests task-safe operation, the worker creates its background task, which then
    also depends per-request on Storage::get_capabilities() reporting STORAGE_CAP_IO_TASK_SAFE.
    """
    data = _get_data()
    data.worker_count += 1
    if task_safe:
        data.worker_task_safe = True


# storage is a dependency of every driver and would otherwise run BEFORE them (default
# priority), reading device_count/worker_count as 0 — every driver's own to_code() is where
# request_storage_device()/request_storage_worker() actually get called. LATE (-100) runs
# after all default-priority driver to_code()s, so those counts are final by the time this
# reads them. Consumers awaiting the registry/worker variables (e.g. via cg.get_variable())
# are unaffected either way, since that call already suspends until the variable exists.
@coroutine_with_priority(CoroPriority.LATE)
async def to_code(config):
    var = cg.new_Pvariable(config[cv.GenerateID()])
    await cg.register_component(var, config)

    device_count = _get_data().device_count
    cg.add(var.set_device_count(device_count))

    cg.add(cg.RawExpression(f"{storage_ns}::global_storage_registry = {var}"))

    cg.add_define("USE_STORAGE_COPY_CHUNK_SIZE", config[CONF_COPY_CHUNK_SIZE])
    cg.add(var.set_max_blocking_transfer_size(config[CONF_MAX_BLOCKING_TRANSFER_SIZE]))

    for conf in config.get(CONF_ON_REGISTERED, []):
        await automation.build_callback_automation(
            var, "add_on_registered_callback", [(StoragePtr, "x")], conf
        )
    for conf in config.get(CONF_ON_UNREGISTERED, []):
        await automation.build_callback_automation(
            var, "add_on_unregistered_callback", [(StoragePtr, "x")], conf
        )

    data = _get_data()
    if data.worker_count > 0:
        cg.add_define("USE_STORAGE_WORKER")
        if data.worker_task_safe:
            cg.add_define("USE_STORAGE_WORKER_TASK")

        worker_id = ID(f"{var}_worker", is_declaration=True, type=StorageWorker)
        CORE.component_ids.add(str(worker_id))
        worker_var = cg.new_Pvariable(worker_id)
        await cg.register_component(worker_var, {})

        cg.add(worker_var.set_task_stack_size(config[CONF_TASK_STACK_SIZE]))
        cg.add(worker_var.set_task_priority(config[CONF_TASK_PRIORITY]))
        cg.add(worker_var.set_max_pending(config[CONF_MAX_PENDING]))
        cg.add(worker_var.set_max_streams(config[CONF_MAX_STREAMS]))

        cg.add(cg.RawExpression(f"{storage_ns}::global_storage_worker = {worker_var}"))


# ---------------------------------------------------------------------------
# Globally available file-op actions: storage.file_write / file_append / file_read.
# Like web_server sorting groups, these work everywhere once storage is loaded
# (every storage driver AUTO_LOADs it) — no per-component preparation required.
# ---------------------------------------------------------------------------

CONF_CONTENT = "content"
CONF_NEWLINE = "newline"
CONF_EXTRACT = "extract"
CONF_JSON = "json"
CONF_TO_GLOBAL = "to_global"
CONF_LINE = "line"
CONF_SPLIT = "split"
CONF_SEPARATOR = "separator"
CONF_REGEX = "regex"
CONF_TRIM = "trim"

FileWriteAction = storage_ns.class_("FileWriteAction", automation.Action)
FileReadAction = storage_ns.class_("FileReadAction", automation.Action)
ExtractStepType = storage_ns.enum("ExtractStepType", is_class=True)


def _validate_write_content(config):
    has_content = CONF_CONTENT in config
    has_format = CONF_FORMAT in config
    if has_content == has_format:
        raise cv.Invalid("Exactly one of 'content' or 'format' is required")
    if config.get(CONF_ARGS) and not has_format:
        raise cv.Invalid("'args' requires 'format'")
    return config


def _file_write_schema(newline_default):
    return cv.All(
        cv.Schema(
            {
                cv.Required(CONF_PATH): cv.templatable(cv.string),
                cv.Optional(CONF_CONTENT): cv.templatable(cv.string),
                cv.Optional(CONF_FORMAT): cv.string,
                cv.Optional(CONF_ARGS, default=[]): cv.ensure_list(cv.lambda_),
                cv.Optional(CONF_NEWLINE, default=newline_default): cv.boolean,
            }
        ),
        _validate_write_content,
    )


def _validate_regex(value):
    value = cv.string(value)
    try:
        # Python's re syntax is a close superset of std::regex ECMAScript for the
        # constructs typically used here; this catches plain syntax errors at
        # config time so the (exception-free) runtime never sees a bad pattern.
        re.compile(value)
    except re.error as e:
        raise cv.Invalid(f"Invalid regex: {e}") from e
    return value


def _exactly_one_step_kind(config):
    kinds = [
        k
        for k in (CONF_LINE, CONF_SPLIT, CONF_KEY, CONF_REGEX, CONF_TRIM, CONF_JSON)
        if k in config
    ]
    if len(kinds) != 1:
        raise cv.Invalid(
            f"Each extract step needs exactly one of line/split/key/regex/trim/json, got {kinds}"
        )
    if CONF_INDEX in config and CONF_SPLIT not in config:
        raise cv.Invalid("'index' is only valid with 'split'")
    if CONF_SEPARATOR in config and CONF_KEY not in config:
        raise cv.Invalid("'separator' is only valid with 'key'")
    if CONF_GROUP in config and CONF_REGEX not in config:
        raise cv.Invalid("'group' is only valid with 'regex'")
    return config


_EXTRACT_STEP_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_LINE): cv.positive_not_null_int,
            # '/'-separated pointer into a JSON document ("a/b/0").
            cv.Optional(CONF_JSON): cv.string_strict,
            cv.Optional(CONF_SPLIT): cv.string_strict,
            cv.Optional(CONF_INDEX): cv.positive_int,
            cv.Optional(CONF_KEY): cv.string_strict,
            cv.Optional(CONF_SEPARATOR): cv.string_strict,
            cv.Optional(CONF_REGEX): _validate_regex,
            cv.Optional(CONF_GROUP): cv.positive_int,
            cv.Optional(CONF_TRIM): cv.boolean,
        }
    ),
    _exactly_one_step_kind,
)


def _validate_read(config):
    if CONF_TO_GLOBAL not in config and CONF_ON_VALUE not in config:
        raise cv.Invalid("At least one of 'to_global' or 'on_value' is required")
    return config


_FILE_READ_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Optional(CONF_EXTRACT, default=[]): cv.ensure_list(_EXTRACT_STEP_SCHEMA),
            cv.Optional(CONF_TO_GLOBAL): cv.use_id(globals_.GlobalsComponent),
            cv.Optional(CONF_ON_VALUE): automation.validate_automation(single=True),
        }
    ),
    _validate_read,
)


async def _build_write_action(config, action_id, template_arg, args, append):
    var = cg.new_Pvariable(action_id, template_arg, append)
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))
    if CONF_CONTENT in config:
        template_ = await cg.templatable(config[CONF_CONTENT], args, cg.std_string)
        cg.add(var.set_content(template_))
    else:
        # Render printf-style format + args into the content string, logger.log-style:
        # the validated arg lambdas are embedded verbatim as C++ expressions.
        format_literal = str(cg.safe_exp(config[CONF_FORMAT]))
        arg_exprs = "".join(f", {x}" for x in config[CONF_ARGS])
        lambda_ = await cg.process_lambda(
            core.Lambda(f"return str_sprintf({format_literal}{arg_exprs});"),
            args,
            return_type=cg.std_string,
        )
        cg.add(var.set_content(lambda_))
    cg.add(var.set_newline(config[CONF_NEWLINE]))
    return var


@automation.register_action(
    "storage.file_write",
    FileWriteAction,
    _file_write_schema(newline_default=False),
    synchronous=True,
)
async def file_write_action_to_code(config, action_id, template_arg, args):
    return await _build_write_action(config, action_id, template_arg, args, False)


@automation.register_action(
    "storage.file_append",
    FileWriteAction,
    _file_write_schema(newline_default=True),
    synchronous=True,
)
async def file_append_action_to_code(config, action_id, template_arg, args):
    return await _build_write_action(config, action_id, template_arg, args, True)


@automation.register_action(
    "storage.file_read",
    FileReadAction,
    _FILE_READ_SCHEMA,
    synchronous=True,
)
async def file_read_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))

    for step in config[CONF_EXTRACT]:
        if CONF_LINE in step:
            cg.add(var.add_step(ExtractStepType.LINE, "", "", step[CONF_LINE]))
        elif CONF_SPLIT in step:
            cg.add(
                var.add_step(
                    ExtractStepType.SPLIT, step[CONF_SPLIT], "", step.get(CONF_INDEX, 0)
                )
            )
        elif CONF_KEY in step:
            cg.add(
                var.add_step(
                    ExtractStepType.KEY,
                    step[CONF_KEY],
                    step.get(CONF_SEPARATOR, "="),
                    0,
                )
            )
        elif CONF_JSON in step:
            cg.add_define("USE_STORAGE_JSON_EXTRACT")
            cg.add(var.add_step(ExtractStepType.JSON, step[CONF_JSON], "", 0))
        elif CONF_REGEX in step:
            cg.add_define("USE_STORAGE_REGEX_EXTRACT")
            # group default: 1 (first capture) if the pattern has groups, else whole match
            default_group = 1 if re.compile(step[CONF_REGEX]).groups > 0 else 0
            cg.add(
                var.add_step(
                    ExtractStepType.REGEX,
                    step[CONF_REGEX],
                    "",
                    step.get(CONF_GROUP, default_group),
                )
            )
        elif CONF_TRIM in step and step[CONF_TRIM]:
            cg.add(var.add_step(ExtractStepType.TRIM, "", "", 0))

    if CONF_TO_GLOBAL in config:
        glob = await cg.get_variable(config[CONF_TO_GLOBAL])
        cg.add(
            var.set_global_setter(
                cg.RawExpression(
                    f"[](const std::string &x) {{ {storage_ns}::assign_from_string({glob}, x); }}"
                )
            )
        )
    if CONF_ON_VALUE in config:
        await automation.build_automation(
            var.get_value_trigger(), [(cg.std_string, "x")], config[CONF_ON_VALUE]
        )
    return var


CONF_RECURSIVE = "recursive"

FileCopyAction = storage_ns.class_("FileCopyAction", automation.Action)
FileDeleteAction = storage_ns.class_("FileDeleteAction", automation.Action)
FileExistsCondition = storage_ns.class_("FileExistsCondition", automation.Condition)

_FILE_COPY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FROM): cv.templatable(cv.string),
        cv.Required(CONF_TO): cv.templatable(cv.string),
    }
)


async def _build_copy_action(config, action_id, template_arg, args, is_move):
    var = cg.new_Pvariable(action_id, template_arg, is_move)
    cg.add(var.set_from(await cg.templatable(config[CONF_FROM], args, cg.std_string)))
    cg.add(var.set_to(await cg.templatable(config[CONF_TO], args, cg.std_string)))
    return var


@automation.register_action(
    "storage.file_copy", FileCopyAction, _FILE_COPY_SCHEMA, synchronous=True
)
async def file_copy_action_to_code(config, action_id, template_arg, args):
    return await _build_copy_action(config, action_id, template_arg, args, False)


# Doubles as a rename action: same-storage moves take the rename() fast path internally.
@automation.register_action(
    "storage.file_move", FileCopyAction, _FILE_COPY_SCHEMA, synchronous=True
)
async def file_move_action_to_code(config, action_id, template_arg, args):
    return await _build_copy_action(config, action_id, template_arg, args, True)


@automation.register_action(
    "storage.file_delete",
    FileDeleteAction,
    cv.Schema(
        {
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Optional(CONF_RECURSIVE, default=False): cv.boolean,
        }
    ),
    synchronous=True,
)
async def file_delete_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    cg.add(var.set_recursive(config[CONF_RECURSIVE]))
    return var


@automation.register_condition(
    "storage.file_exists",
    FileExistsCondition,
    cv.maybe_simple_value(
        {cv.Required(CONF_PATH): cv.templatable(cv.string)}, key=CONF_PATH
    ),
)
async def file_exists_condition_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    cg.add(var.set_path(await cg.templatable(config[CONF_PATH], args, cg.std_string)))
    return var


MountAction = storage_ns.class_("MountAction", automation.Action)

_MOUNT_SCHEMA = cv.maybe_simple_value(
    {cv.Required(CONF_ID): cv.use_id(MountableStorage)}, key=CONF_ID
)


async def _build_mount_action(config, action_id, template_arg, args, mount):
    # cv.use_id(MountableStorage) already rejected non-removable targets at YAML time.
    target = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, target, mount)


@automation.register_action(
    "storage.mount", MountAction, _MOUNT_SCHEMA, synchronous=True
)
async def mount_action_to_code(config, action_id, template_arg, args):
    return await _build_mount_action(config, action_id, template_arg, args, True)


@automation.register_action(
    "storage.unmount", MountAction, _MOUNT_SCHEMA, synchronous=True
)
async def unmount_action_to_code(config, action_id, template_arg, args):
    return await _build_mount_action(config, action_id, template_arg, args, False)


# ==================== PREFERENCES BACKUP/RESTORE ====================
# storage.export_preferences / storage.import_preferences: back up ESPHome
# preferences (the "esphome" NVS namespace) to a storage target and restore
# them. Provided by storage itself, guard-protected: the C++ only compiles
# in when one of these actions is used, and only on esp32 (preferences are
# always NVS-backed there — no extra YAML needed to "enable" them).
#
# Selection is an option on the action: `preferences:` lists global IDs —
# with it, only those entries round-trip and export under their YAML id;
# without it, the whole namespace round-trips under numeric NVS keys.

CONF_PREFERENCES = "preferences"
CONF_REBOOT = "reboot"

# Keep in sync with globals_component.h ("1944399030U ^ this->name_hash_")
# and the md5-based name hash in globals/__init__.py.
_GLOBALS_KEY_XOR = 1944399030

# YAML `type:` -> (PrefType tag, element size irrelevant here). Blob layouts:
# scalars/arrays are raw T bytes; std::string is length-prefixed char[SZ]
# with SZ = max_restore_data_length (default 63) + 1 — keep in sync with
# globals/__init__.py.
_PREF_SCALAR_TYPES = {
    "bool": "BOOL",
    "int8_t": "I8",
    "char": "I8",
    "uint8_t": "U8",
    "unsigned char": "U8",
    "int16_t": "I16",
    "short": "I16",
    "uint16_t": "U16",
    "unsigned short": "U16",
    "int": "I32",
    "int32_t": "I32",
    "long": "I32",
    "uint32_t": "U32",
    "unsigned int": "U32",
    "unsigned long": "U32",
    "size_t": "U32",
    "float": "F32",
    "double": "F64",
}
_ARRAY_TYPE_RE = re.compile(r"^\s*(.+?)\s*\[\s*(\d+)\s*\]\s*$")

_RESTORING_RE = re.compile(r"RestoringGlobalsComponent<\s*(.+?)\s*>\s*$")
_RESTORING_STRING_RE = re.compile(
    r"RestoringGlobalStringComponent<\s*.+?,\s*(\d+)\s*>\s*$"
)


def _pref_type_from_class(type_str: str) -> tuple[str, int] | None:
    """(PrefType tag, count) from a declared global's C++ class string —
    codegen-world data only (ID.type of the registered variable). None when
    the class is not a restoring global at all."""
    if m := _RESTORING_STRING_RE.search(type_str):
        return "STRING", int(m.group(1))  # SZ straight from the template arg
    if m := _RESTORING_RE.search(type_str):
        inner = m.group(1)
        if inner in _PREF_SCALAR_TYPES:
            return _PREF_SCALAR_TYPES[inner], 1
        if am := _ARRAY_TYPE_RE.match(inner):
            base, count = am.group(1), int(am.group(2))
            if base in _PREF_SCALAR_TYPES:
                return _PREF_SCALAR_TYPES[base], count
        return "HEX", 0  # restoring, but a type we cannot render — hex round-trip
    return None



ExportPreferencesAction = storage_ns.class_(
    "ExportPreferencesAction", automation.Action
)
ImportPreferencesAction = storage_ns.class_(
    "ImportPreferencesAction", automation.Action
)


def _global_nvs_key(global_id: str) -> int:
    name_hash = int(hashlib.md5(global_id.encode()).hexdigest()[:8], 16)
    return (_GLOBALS_KEY_XOR ^ name_hash) & 0xFFFFFFFF


_PREFERENCES_ACTION_BASE = {
    cv.Required(CONF_PATH): cv.templatable(cv.string_strict),
    cv.Optional(CONF_FORMAT, default="kv"): cv.one_of("kv", "json", lower=True),
    # Globals with restore_value. cv.use_id(cg.Component) because the globals
    # component declares several unrelated classes (GlobalsComponent,
    # RestoringGlobalsComponent, RestoringGlobalStringComponent) — only the
    # id string is consumed here (baked into the name<->key table), the
    # variable itself is never awaited.
    cv.Optional(CONF_PREFERENCES): cv.ensure_list(cv.use_id(cg.Component)),
}

_EXPORT_PREFERENCES_SCHEMA = cv.All(
    cv.only_on(["esp32"]),
    cv.Schema(_PREFERENCES_ACTION_BASE),
)

_IMPORT_PREFERENCES_SCHEMA = cv.All(
    cv.only_on(["esp32"]),
    cv.Schema(
        {
            **_PREFERENCES_ACTION_BASE,
            # Preferences are read at boot — imported values only take effect
            # after a restart. Opt-in convenience.
            cv.Optional(CONF_REBOOT, default=False): cv.boolean,
        }
    ),
)


# Per-type version constants of EntityBase::make_entity_preference_() callers.
# Keep in sync: fan/fan.cpp, climate/climate.cpp; every other core entity uses
# the default version 0. template text is special-cased (trait-salted key).
# (module, class, version, EntityKind) — kinds map to real-struct codecs in
# preferences_backup.cpp; anything not matched below registers as RAW (named,
# hex value). datetime template platforms carry their own versions.
_ENTITY_KINDS = (
    ("fan", "Fan", 0x71700ABB, "FAN"),
    ("climate", "Climate", 0x848EA6AD, "CLIMATE"),
    ("light", "LightState", 0, "LIGHT"),
    ("cover", "Cover", 0, "COVER"),
    ("valve", "Valve", 0, "VALVE"),
    ("switch", "Switch", 0, "BOOL"),
    ("number", "Number", 0, "FLOAT"),
)


def _entity_registration(reg_id) -> str | None:
    """Generated registration call for a restoring entity ID, or None if the
    ID is not an entity. The KEY is computed at runtime by the entity object
    itself — codegen only supplies name and per-type version."""
    type_ = reg_id.type
    if type_ is None or not hasattr(type_, "inherits_from"):
        return None
    if not type_.inherits_from(cg.EntityBase):
        return None
    from esphome.components import text as text_

    if type_.inherits_from(text_.Text):
        # trait-salted key (template_text.cpp) — traits read from the live
        # object inside the helper; nothing recipe-shaped is baked here
        return (
            f"esphome::storage::detail::register_text_pref_impl({reg_id.id}, \"{reg_id.id}\", "
            f"{reg_id.id}->traits.get_min_length(), {reg_id.id}->traits.get_max_length(), "
            f"{reg_id.id}->traits.get_pattern_c_str())"
        )
    version, kind = 0, "RAW"
    for mod_name, cls_name, ver, k in _ENTITY_KINDS:
        mod = __import__(f"esphome.components.{mod_name}", fromlist=[cls_name])
        if hasattr(mod, cls_name) and type_.inherits_from(getattr(mod, cls_name)):
            version, kind = ver, k
            break
    return (
        f'esphome::storage::register_entity_pref({reg_id.id}, "{reg_id.id}", '
        f"{version}UL, esphome::storage::EntityKind::{kind})"
    )


async def _build_preferences_action(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    cg.add_define("USE_STORAGE_PREFERENCES")
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))
    cg.add(var.set_format(config[CONF_FORMAT]))

    def _bake(entries, restrict):
        arr = f"{action_id}_psel"
        cg.add_global(
            cg.RawExpression(
                f"static const esphome::storage::PrefSelection {arr}[] = {{"
                + ", ".join(entries)
                + "}"
            )
        )
        cg.add(var.set_selection(cg.RawExpression(arr), len(entries), restrict))

    def _entry(name, tag, count):
        key = _global_nvs_key(name)
        return f'{{"{name}", {key}UL, esphome::storage::PrefType::{tag}, {count}}}'

    if selection := config.get(CONF_PREFERENCES):
        # get_variable_with_full_id is a coroutine: it suspends until the
        # global's own to_code has registered the variable — the declaration
        # ID it returns carries the real C++ class (codegen-world data, no
        # validation-step leftovers).
        entries = []
        entity_names = []
        for gid in selection:
            full_id, _ = await cg.get_variable_with_full_id(gid)
            parsed = _pref_type_from_class(str(full_id.type))
            if parsed is not None:
                entries.append(_entry(gid.id, *parsed))
                continue
            if reg := _entity_registration(full_id):
                # entity preference: named round-trip, value stays hex
                cg.add(cg.RawExpression(reg))
                entity_names.append(gid.id)
                continue
            raise cv.Invalid(
                f"'{gid.id}' stores no known preference (restoring global or "
                f"restorable entity required)"
            )
        _bake(entries, True)
        if entity_names:
            arr = f"{action_id}_pent"
            cg.add_global(
                cg.RawExpression(
                    f"static const char *const {arr}[] = {{"
                    + ", ".join(f'"{n}"' for n in entity_names)
                    + "}"
                )
            )
            cg.add(var.set_entity_selection(cg.RawExpression(arr), len(entity_names)))
    else:
        # All mode: enumerate the codegen variable registry once every
        # pending to_code has run. Scheduled as its own coroutine job — it is
        # enqueued behind all already-queued component jobs, so the globals
        # are registered by the time it executes.
        async def _bake_all():
            entries = []
            seen_regs = set()
            for reg_id in CORE.variables:
                parsed = _pref_type_from_class(str(reg_id.type))
                if parsed is not None:
                    entries.append(_entry(reg_id.id, *parsed))
                    continue
                if (reg := _entity_registration(reg_id)) and reg_id.id not in seen_regs:
                    seen_regs.add(reg_id.id)
                    cg.add(cg.RawExpression(reg))
            if entries:
                _bake(entries, False)

        CORE.add_job(_bake_all)
    return var


@automation.register_action(
    "storage.export_preferences",
    ExportPreferencesAction,
    _EXPORT_PREFERENCES_SCHEMA,
    synchronous=True,
)
async def export_preferences_to_code(config, action_id, template_arg, args):
    return await _build_preferences_action(config, action_id, template_arg, args)


@automation.register_action(
    "storage.import_preferences",
    ImportPreferencesAction,
    _IMPORT_PREFERENCES_SCHEMA,
    synchronous=True,
)
async def import_preferences_to_code(config, action_id, template_arg, args):
    var = await _build_preferences_action(config, action_id, template_arg, args)
    cg.add(var.set_reboot(config[CONF_REBOOT]))
    return var
