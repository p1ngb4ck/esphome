#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include "preferences.h"
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#ifdef USE_ESP32_BOOT_RAM_PROBE
#include <cinttypes>
#include <esp_heap_caps.h>
#include "esphome/core/log.h"
#endif

void setup();  // NOLINT(readability-redundant-declaration)

// Weak stub for initArduino - overridden when the Arduino component is present.
// Name must match the Arduino framework's entry point, so the naming check is suppressed.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" __attribute__((weak)) void initArduino() {}

namespace esphome {

// HAL functions live in hal.cpp. This file keeps only the loop task setup.
static StaticTask_t loop_task_tcb;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static StackType_t
    loop_task_stack[ESPHOME_LOOP_TASK_STACK_SIZE];  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef USE_ESP32_BOOT_RAM_PROBE
// Internal and internal DMA free RAM, sampled every millisecond for the first 90 s; each change of
// 2 KB or more and each new low of the heap's own minimum are kept (on this task's PSRAM stack) and
// logged once the 90 s are over, with the component running then: its name once setup() is complete
// (App tracks the running component only for loop / scheduler dispatch), "setup" before, and when its
// call started (the setup order resolves that one).
static const char *boot_component_name(Component *component) {
  return component != nullptr ? LOG_STR_ARG(component->get_component_log_str()) : "setup";
}

static void boot_ram_probe(void *) {
  struct Sample {
    uint32_t ms, component_ms, internal, dma, dma_min;
    Component *component;
  };
  static constexpr size_t SAMPLES = 100;
  static constexpr uint32_t STEP = 2048;
  Sample samples[SAMPLES];
  size_t count = 0;
  bool full = false;
  uint32_t last_internal = 0, last_dma = 0, last_min = UINT32_MAX;
  Sample low{0, 0, 0, UINT32_MAX, 0, nullptr};  // the lowest internal DMA free seen
  uint32_t setup_done_ms = 0;
  const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
  while (xTaskGetTickCount() * portTICK_PERIOD_MS < 90000) {
    const uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t dma = heap_caps_get_free_size(dma_caps);
    const uint32_t dma_min = heap_caps_get_minimum_free_size(dma_caps);
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    Component *component = App.is_setup_complete() ? App.get_current_component() : nullptr;
    if (setup_done_ms == 0 && component != nullptr)
      setup_done_ms = now;
    if (dma < low.dma)
      low = {now, App.get_loop_component_start_time(), internal, dma, dma_min, component};
    // Drops of 2 KB, rises of 8 KB (a task's stack given back), each new low of the heap's minimum
    const bool moved = internal + STEP <= last_internal || last_internal + 4 * STEP <= internal ||
                       dma + STEP <= last_dma || last_dma + 4 * STEP <= dma || dma_min + 512 <= last_min;
    if (moved) {
      if (count < SAMPLES) {
        samples[count++] = {now, App.get_loop_component_start_time(), internal, dma, dma_min, component};
      } else {
        full = true;
      }
      last_internal = internal;
      last_dma = dma;
      last_min = dma_min;
    }
    vTaskDelay(1);
  }
  ESP_LOGI("boot_ram", "Boot RAM (free bytes; the component running, since):");
  for (size_t i = 0; i < count; i++) {
    const Sample &s = samples[i];
    ESP_LOGI("boot_ram", "%6" PRIu32 " ms: internal %6" PRIu32 ", DMA %6" PRIu32 ", DMA low %6" PRIu32 "  %s (%" PRIu32
                         " ms)",
             s.ms, s.internal, s.dma, s.dma_min, boot_component_name(s.component), s.component_ms);
  }
  if (full)
    ESP_LOGI("boot_ram", "(more changes than %u kept)", static_cast<unsigned>(SAMPLES));
  ESP_LOGI("boot_ram", "setup() complete by %" PRIu32 " ms", setup_done_ms);
  ESP_LOGI("boot_ram", "Lowest internal DMA free %" PRIu32 " at %" PRIu32 " ms (%s), internal then %" PRIu32
                       ", heap minimum %" PRIu32,
           low.dma, low.ms, boot_component_name(low.component), low.internal, low.dma_min);
  vTaskDeleteWithCaps(nullptr);
}
#endif

void loop_task(void *pv_params) {
  setup();
  while (true) {
    App.loop();
  }
}

#ifdef USE_ESP32_COMPONENTS_IN_PSRAM
// The component objects' storage, placed in PSRAM's noinit section (esp32 components_in_psram): zeroed
// here as .bss would be, before setup() constructs the objects in it.
extern "C" uint8_t _esphome_main_bss_start, _esphome_main_bss_end;        // NOLINT
extern "C" uint8_t _esphome_main_common_start, _esphome_main_common_end;  // NOLINT
#endif

extern "C" void app_main() {
#ifdef USE_ESP32_COMPONENTS_IN_PSRAM
  memset(&_esphome_main_bss_start, 0, &_esphome_main_bss_end - &_esphome_main_bss_start);
  memset(&_esphome_main_common_start, 0, &_esphome_main_common_end - &_esphome_main_common_start);
#endif
  // Apply the custom eFuse MAC (if burned and valid) as the base MAC before any
  // interface (Wi-Fi, Ethernet, Bluetooth, 802.15.4) derives its address from it.
  // The logger does not exist yet, so only log-free helpers may be used here.
  uint8_t mac[MAC_ADDRESS_SIZE];
  if (get_custom_mac_address(mac)) {
    set_mac_address(mac);
  }
  initArduino();
  esp32::setup_preferences();
#ifdef USE_ESP32_BOOT_RAM_PROBE
  xTaskCreatePinnedToCoreWithCaps(boot_ram_probe, "boot_ram", 6144, nullptr, 22, nullptr, 0, MALLOC_CAP_SPIRAM);
#endif
#if CONFIG_FREERTOS_UNICORE
  xTaskCreateStatic(loop_task, "loopTask", ESPHOME_LOOP_TASK_STACK_SIZE, nullptr, 1, loop_task_stack, &loop_task_tcb);
#else
  xTaskCreateStaticPinnedToCore(loop_task, "loopTask", ESPHOME_LOOP_TASK_STACK_SIZE, nullptr, 1, loop_task_stack,
                                &loop_task_tcb, LOOP_TASK_CORE);
#endif
}

}  // namespace esphome

#endif  // USE_ESP32
