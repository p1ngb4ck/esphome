from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_TARGET
from esphome.core import CORE

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["storage", "lvgl"]

CONF_SCREENSHOT_DIRECTORY = "screenshot_directory"
CONF_SCREENCAST_DIRECTORY = "screencast_directory"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_FPS = "fps"
CONF_SOUND = "sound"
CONF_ON_SCREENSHOT = "on_screenshot"
CONF_ON_SCREENCAST_STARTED = "on_screencast_started"
CONF_ON_SCREENCAST_STOPPED = "on_screencast_stopped"
CONF_RELAY = "relay"
SCREENCAST_TARGETS = {"local": 1, "relay": 2, "both": 3}


def AUTO_LOAD(config=None) -> list[str]:
    load = ["avi", "json", "ring_buffer"]
    if config and config.get(CONF_RELAY):
        load.append("relay_link")
    return load


screen_capture_ns = cg.esphome_ns.namespace("screen_capture")
ScreenCaptureComponent = screen_capture_ns.class_("ScreenCaptureComponent", cg.Component)
ScreenshotAction = screen_capture_ns.class_("ScreenshotAction", automation.Action)
ScreencastAction = screen_capture_ns.class_("ScreencastAction", automation.Action)


def _storage_dir(value):
    value = cv.string_strict(value)
    if not value.startswith("/") or value.endswith("/"):
        raise cv.Invalid("a directory's storage path, as /sd/screenshots")
    return value


def capture_schema(directory=cv.Required):
    from esphome.components import microphone

    return cv.Schema(
        {
            directory(CONF_SCREENSHOT_DIRECTORY): _storage_dir,
            cv.Optional(CONF_SCREENCAST_DIRECTORY): _storage_dir,
            cv.Optional(CONF_JPEG_QUALITY, default=80): cv.int_range(min=1, max=100),
            cv.Optional(CONF_FPS, default=25): cv.int_range(min=1, max=30),
            cv.Optional(CONF_TARGET, default="local"): cv.one_of(*SCREENCAST_TARGETS, lower=True),
            cv.Optional(CONF_MICROPHONE): microphone.microphone_source_schema(min_channels=1, max_channels=2),
            cv.Optional(CONF_SOUND, default=True): cv.boolean,
            cv.Optional(CONF_ON_SCREENSHOT): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_SCREENCAST_STARTED): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_SCREENCAST_STOPPED): automation.validate_automation(single=True),
        }
    )


def _standalone(config):
    if CONF_SCREENSHOT_DIRECTORY not in config and any(
        key in config
        for key in (
            CONF_SCREENCAST_DIRECTORY,
            CONF_MICROPHONE,
            CONF_ON_SCREENSHOT,
            CONF_ON_SCREENCAST_STARTED,
            CONF_ON_SCREENCAST_STOPPED,
        )
    ):
        raise cv.Invalid(f"{CONF_SCREENSHOT_DIRECTORY} is required")
    return config


CONFIG_SCHEMA = cv.All(
    capture_schema(cv.Optional)
    .extend(
        {
            cv.GenerateID(): cv.declare_id(ScreenCaptureComponent),
            cv.Optional(CONF_RELAY, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _standalone,
)


def final_validate_capture(config, component):
    if (mic := config.get(CONF_MICROPHONE)) is not None:
        from esphome.components import microphone

        microphone.final_validate_microphone_source_schema(component)(mic)


def _final_validate(config):
    final_validate_capture(config, "screen_capture")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def capture_to_code(var, capture):
    cg.add_define("USE_SCREEN_CAPTURE")
    cg.add(var.set_screenshot_directory(capture[CONF_SCREENSHOT_DIRECTORY]))
    cg.add(var.set_screencast_directory(capture.get(CONF_SCREENCAST_DIRECTORY, capture[CONF_SCREENSHOT_DIRECTORY])))
    cg.add(var.set_capture_quality(capture[CONF_JPEG_QUALITY]))
    cg.add(var.set_screencast_fps(capture[CONF_FPS]))
    cg.add(var.set_screencast_target(SCREENCAST_TARGETS[capture[CONF_TARGET]]))
    cg.add(var.set_screencast_sound(capture[CONF_SOUND]))
    if (mic := capture.get(CONF_MICROPHONE)) is not None:
        from esphome.components import microphone

        cg.add_define("USE_SCREEN_CAPTURE_MIC")
        mic_source = await microphone.microphone_source_to_code(mic, passive=False)
        cg.add(var.set_capture_microphone(mic_source))
    if on_shot := capture.get(CONF_ON_SCREENSHOT):
        await automation.build_automation(var.get_screenshot_trigger(), [(cg.std_string, "path")], on_shot)
    if on_started := capture.get(CONF_ON_SCREENCAST_STARTED):
        await automation.build_automation(var.get_screencast_started_trigger(), [], on_started)
    if on_stopped := capture.get(CONF_ON_SCREENCAST_STOPPED):
        await automation.build_automation(var.get_screencast_stopped_trigger(), [(cg.std_string, "reason")], on_stopped)


async def to_code(config):
    if CONF_SCREENSHOT_DIRECTORY not in config:
        return
    cg.add_define("USE_SCREEN_CAPTURE_COMPONENT")
    var = cg.Pvariable(config[CONF_ID], cg.MockObj(str(ScreenCaptureComponent), "::").create())
    await cg.register_component(var, config)
    cg.add(var.set_lvgl(await cg.get_variable(CORE.config["lvgl"][0][CONF_ID])))
    if config[CONF_RELAY]:
        from esphome.components import relay_link

        cg.add_define("USE_SCREEN_CAPTURE_RELAY")
        cg.add(var.set_relay(await relay_link.get_relay_link()))
    await capture_to_code(var, config)


ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(ScreenCaptureComponent)})


@automation.register_action("screen_capture.screenshot", ScreenshotAction, ACTION_SCHEMA, synchronous=True)
async def screenshot_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


SCREENCAST_COMMANDS = {"start": 0, "stop": 1, "toggle": 2}


def _register_screencast(name, command):
    @automation.register_action(f"screen_capture.screencast_{name}", ScreencastAction, ACTION_SCHEMA, synchronous=True)
    async def _to_code(config, action_id, template_arg, args):
        var = cg.new_Pvariable(action_id, template_arg)
        await cg.register_parented(var, config[CONF_ID])
        cg.add(var.set_command(command))
        return var

    return _to_code


for _name, _command in SCREENCAST_COMMANDS.items():
    _register_screencast(_name, _command)
