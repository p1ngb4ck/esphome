from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, microphone
from esphome.components.esp32 import add_idf_sdkconfig_option, only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME, CONF_TRIGGER_ID, CONF_VOLUME

CODEOWNERS = ["@p1ngb4ck"]
DEPENDENCIES = ["esp32", "microphone"]

CONF_A2DP_SOURCE_ID = "a2dp_source_id"
CONF_BUFFER_DURATION = "buffer_duration"
CONF_FALLBACK_TIME = "fallback_time"
CONF_LOCAL_NAME = "local_name"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_PAIRED = "on_paired"
CONF_PAIR_ON_BOOT_IF_EMPTY = "pair_on_boot_if_empty"
CONF_SETTLE_TIME = "settle_time"
CONF_TARGET_NAME = "target_name"

a2dp_source_ns = cg.esphome_ns.namespace("a2dp_source")
A2DPSource = a2dp_source_ns.class_("A2DPSource", cg.Component)
StartPairingAction = a2dp_source_ns.class_("StartPairingAction", automation.Action)
ForgetDeviceAction = a2dp_source_ns.class_("ForgetDeviceAction", automation.Action)
PairWithNameAction = a2dp_source_ns.class_("PairWithNameAction", automation.Action)
IsConnectedCondition = a2dp_source_ns.class_(
    "IsConnectedCondition", automation.Condition
)
IsPairedCondition = a2dp_source_ns.class_("IsPairedCondition", automation.Condition)

# Classic Bluetooth exists on the original ESP32 and nowhere else in the family;
# every later variant is BLE only, and A2DP is a Classic profile. Without this
# the mistake surfaces as an #error deep inside the library's platform check.
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(A2DPSource),
            # Stereo, because A2DP is. The library's frame is two int16 samples
            # and nothing in the path resamples or upmixes.
            cv.Required(
                microphone.CONF_MICROPHONE
            ): microphone.microphone_source_schema(min_channels=2, max_channels=2),
            cv.Optional(CONF_LOCAL_NAME, default="ESPHome Audio"): cv.string,
            # Empty means the strongest signal in the pairing window wins. Set a
            # name to remove the guesswork when several devices are in range.
            cv.Optional(CONF_TARGET_NAME, default=""): cv.string,
            # How long the pairing window only collects and logs before it picks.
            cv.Optional(
                CONF_SETTLE_TIME, default="15s"
            ): cv.positive_time_period_seconds,
            # If the winner never answers again, take whatever still does after
            # this long, so one device that replied once cannot block pairing.
            cv.Optional(
                CONF_FALLBACK_TIME, default="20s"
            ): cv.positive_time_period_seconds,
            cv.Optional(CONF_VOLUME, default=60): cv.int_range(0, 127),
            # Slack between the microphone and the Bluetooth stack. The two run
            # on independent clocks, so one always drifts against the other.
            cv.Optional(
                CONF_BUFFER_DURATION, default="100ms"
            ): cv.positive_time_period_milliseconds,
            # Open the pairing window at boot when nothing is stored yet. Turn
            # off if pairing should only ever happen on request.
            cv.Optional(CONF_PAIR_ON_BOOT_IF_EMPTY, default=True): cv.boolean,
            cv.Optional(CONF_ON_PAIRED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        automation.Trigger.template(cg.std_string)
                    )
                }
            ),
            cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger)}
            ),
            cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(automation.Trigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32], msg_prefix="a2dp_source"),
    cv.require_framework_version(esp_idf=cv.Version(5, 0, 0)),
)

# The helper validates a microphone source block, not a whole component config,
# so it has to be addressed at the key it belongs to.
FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(microphone.CONF_MICROPHONE): (
            microphone.final_validate_microphone_source_schema(
                "a2dp_source", sample_rate=44100
            )
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # passive=False: this component is the reason the microphone runs, so it
    # starts and stops it rather than listening in on someone else's stream.
    mic_source = await microphone.microphone_source_to_code(
        config[microphone.CONF_MICROPHONE], passive=False
    )
    cg.add(var.set_microphone_source(mic_source))

    cg.add(var.set_local_name(config[CONF_LOCAL_NAME]))
    cg.add(var.set_target_name(config[CONF_TARGET_NAME]))
    cg.add(var.set_settle_time(config[CONF_SETTLE_TIME].total_seconds))
    cg.add(var.set_fallback_time(config[CONF_FALLBACK_TIME].total_seconds))
    cg.add(var.set_volume(config[CONF_VOLUME]))
    cg.add(var.set_buffer_duration_ms(config[CONF_BUFFER_DURATION].total_milliseconds))
    cg.add(var.set_pair_on_boot_if_empty(config[CONF_PAIR_ON_BOOT_IF_EMPTY]))

    for conf in config.get(CONF_ON_PAIRED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "name")], conf)
        cg.add(var.add_on_paired_trigger(trigger))
    for conf in config.get(CONF_ON_CONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        cg.add(var.add_on_connected_trigger(trigger))
    for conf in config.get(CONF_ON_DISCONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
        cg.add(var.add_on_disconnected_trigger(trigger))

    # A2DP is Classic Bluetooth: Bluedroid host, controller in BR/EDR only mode.
    # BLE is switched off because the profile does not use it and dual mode costs
    # both RAM and radio time.
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_CONTROLLER_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)
    add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_A2DP_ENABLE", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_SPP_ENABLED", False)

    # bt sits on the default exclusion list, and request_bluetooth() is what
    # pulls it back while also telling the network reconciler that the radio is
    # in use.
    esp32.request_bluetooth()

    # The A2DP library is vendored beside this file rather than pulled in as a
    # managed component. Its own CMakeLists compiles every source under src/ and
    # REQUIRES arduino-audio-tools, which the sink's I2S output stage needs and
    # which does not exist here; it also sets A2DP_I2S_AUDIOTOOLS=1 as a PUBLIC
    # compile definition, so a -D on our side could not turn it off again. Only
    # the two translation units the source role needs are carried, and these
    # flags now reach them because we compile them ourselves.
    cg.add_build_flag("-DA2DP_I2S_AUDIOTOOLS=0")
    cg.add_build_flag("-DA2DP_LEGACY_I2S_SUPPORT=0")
    cg.add_build_flag("-DA2DP_SPP_SUPPORT=0")


A2DP_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(A2DPSource)}
)


@automation.register_action(
    "a2dp_source.start_pairing",
    StartPairingAction,
    A2DP_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "a2dp_source.forget_device",
    ForgetDeviceAction,
    A2DP_ACTION_SCHEMA,
    synchronous=True,
)
async def a2dp_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "a2dp_source.pair_with_name",
    PairWithNameAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(A2DPSource),
            cv.Required(CONF_NAME): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def pair_with_name_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_NAME], args, cg.std_string)
    cg.add(var.set_name(templ))
    return var


@automation.register_condition(
    "a2dp_source.is_connected", IsConnectedCondition, A2DP_ACTION_SCHEMA
)
@automation.register_condition(
    "a2dp_source.is_paired", IsPairedCondition, A2DP_ACTION_SCHEMA
)
async def a2dp_condition_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
