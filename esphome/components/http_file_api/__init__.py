from esphome import automation
import esphome.codegen as cg
from esphome.components import storage
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import coroutine_with_priority
from esphome.cpp_types import const_char_ptr

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage"]

CONF_STORAGE_ID = "storage_id"
CONF_MAX_DIR_ENTRIES = "max_dir_entries"
CONF_ENABLE_ASYNC_TRANSFERS = "enable_async_transfers"
CONF_ASYNC_TASK_SAFE = "async_task_safe"

# Not yet in esphome/const.py
CONF_ON_UPLOAD_COMPLETE = "on_upload_complete"
CONF_ON_DELETE = "on_delete"
CONF_ON_MKDIR = "on_mkdir"
CONF_ON_RENAME = "on_rename"
CONF_ON_COPY_COMPLETE = "on_copy_complete"
CONF_ON_MOVE_COMPLETE = "on_move_complete"

http_file_api_ns = cg.esphome_ns.namespace("http_file_api")
HttpFileApi = http_file_api_ns.class_("HttpFileApi", cg.Component)

storage_ns = cg.esphome_ns.namespace("storage")
StorageError = storage_ns.enum("StorageError", is_class=True)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HttpFileApi),
        # Optional: omit for registry-wide mode (caller passes an explicit PathStorage
        # per call, or a path resolved via storage::global_storage_registry->resolve_path()).
        cv.Optional(CONF_STORAGE_ID): cv.use_id(storage.PathStorage),
        cv.Optional(CONF_MAX_DIR_ENTRIES, default=256): cv.int_range(min=1, max=4096),
        cv.Optional(CONF_ENABLE_ASYNC_TRANSFERS, default=False): cv.boolean,
        cv.Optional(CONF_ASYNC_TASK_SAFE, default=False): cv.boolean,
        cv.Optional(CONF_ON_UPLOAD_COMPLETE): automation.validate_automation({}),
        cv.Optional(CONF_ON_DELETE): automation.validate_automation({}),
        cv.Optional(CONF_ON_MKDIR): automation.validate_automation({}),
        cv.Optional(CONF_ON_RENAME): automation.validate_automation({}),
        cv.Optional(CONF_ON_COPY_COMPLETE): automation.validate_automation({}),
        cv.Optional(CONF_ON_MOVE_COMPLETE): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)


@coroutine_with_priority(45.0)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if storage_id := config.get(CONF_STORAGE_ID):
        storage_var = await cg.get_variable(storage_id)
        cg.add(var.set_storage(storage_var))

    cg.add(var.set_max_dir_entries(config[CONF_MAX_DIR_ENTRIES]))

    if config[CONF_ENABLE_ASYNC_TRANSFERS]:
        storage.request_storage_worker(task_safe=config[CONF_ASYNC_TASK_SAFE])

    for conf in config.get(CONF_ON_UPLOAD_COMPLETE, []):
        await automation.build_callback_automation(
            var, "add_on_upload_complete_callback", [(const_char_ptr, "path")], conf
        )
    for conf in config.get(CONF_ON_DELETE, []):
        await automation.build_callback_automation(
            var, "add_on_delete_callback", [(const_char_ptr, "path")], conf
        )
    for conf in config.get(CONF_ON_MKDIR, []):
        await automation.build_callback_automation(
            var, "add_on_mkdir_callback", [(const_char_ptr, "path")], conf
        )
    for conf in config.get(CONF_ON_RENAME, []):
        await automation.build_callback_automation(
            var,
            "add_on_rename_callback",
            [(const_char_ptr, "old_path"), (const_char_ptr, "new_path")],
            conf,
        )

    if config[CONF_ENABLE_ASYNC_TRANSFERS]:
        for conf in config.get(CONF_ON_COPY_COMPLETE, []):
            await automation.build_callback_automation(
                var, "add_on_copy_complete_callback", [(StorageError, "err")], conf
            )
        for conf in config.get(CONF_ON_MOVE_COMPLETE, []):
            await automation.build_callback_automation(
                var, "add_on_move_complete_callback", [(StorageError, "err")], conf
            )
