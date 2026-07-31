import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.const import CONF_ID

CODEOWNERS = ["@p1ngb4ck"]
# The module lives on a mounted storage filesystem; the stub waits for it before dlopen.
DEPENDENCIES = ["storage"]

module_host_ns = cg.esphome_ns.namespace("module_host")
ModuleHost = module_host_ns.class_("ModuleHost", cg.Component)

CONF_MODULE_PATH = "module_path"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ModuleHost),
            # Full VFS path of the .so on a mounted storage (e.g. "/flash/demo_module.so").
            cv.Required(CONF_MODULE_PATH): cv.string,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # elf_loader dlopen path + PSRAM execution: ESP-IDF + S3/P4 only.
    cv.only_with_framework("esp-idf")
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_module_path(config[CONF_MODULE_PATH]))

    # Pull the loader in from the component registry and turn on the dynamic-shared-object API +
    # PSRAM execution. Exact Kconfig names come from the elf_loader Kconfig:
    #   ELF_LOADER                     -- master enable (default y)
    #   ELF_DYNAMIC_LOAD_SHARED_OBJECT -- dlopen/dlsym API (default n -> must enable)
    #   ELF_LOADER_LOAD_PSRAM          -- run ELF from PSRAM (default y on S2/S3/P4/C61 with SPIRAM)
    # SPIRAM itself must be enabled by the board's psram: config -- this component does not force it,
    # but ELF_LOADER_LOAD_PSRAM has no effect without it.
    add_idf_component(name="espressif/elf_loader", ref="1.*")
    add_idf_sdkconfig_option("CONFIG_ELF_LOADER", True)
    add_idf_sdkconfig_option("CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT", True)
    add_idf_sdkconfig_option("CONFIG_ELF_LOADER_LOAD_PSRAM", True)
    # Executing dynamically-loaded code requires the loaded region to be instruction-fetchable.
    # Two memory-protection settings otherwise mark data/heap (where the module lands) non-executable,
    # which faults on the first call into the module (RISC-V instruction access fault, MCAUSE=1):
    #   ESP_SYSTEM_MEMPROT_FEATURE   -- the generic memory-protection feature
    #   ESP_SYSTEM_PMP_IDRAM_SPLIT   -- P4/RISC-V PMP split that makes DRAM non-executable
    # The elf_loader examples disable both; a code loader inherently needs this.
    add_idf_sdkconfig_option("CONFIG_ESP_SYSTEM_MEMPROT_FEATURE", False)
    add_idf_sdkconfig_option("CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT", False)
