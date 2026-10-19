#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "preferences.h"
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
// logged once the 90 s are over, with when the component running then started its call.
static void boot_ram_probe(void *) {
  struct Sample {
    uint32_t ms, component_ms, internal, dma, dma_min;
  };
  static constexpr size_t SAMPLES = 100;
  static constexpr uint32_t STEP = 2048;
  Sample samples[SAMPLES];
  size_t count = 0;
  bool full = false;
  uint32_t last_internal = 0, last_dma = 0, last_min = UINT32_MAX;
  Sample low{0, 0, 0, UINT32_MAX, 0};  // the lowest internal DMA free seen
  const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
  while (xTaskGetTickCount() * portTICK_PERIOD_MS < 90000) {
    const uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t dma = heap_caps_get_free_size(dma_caps);
    const uint32_t dma_min = heap_caps_get_minimum_free_size(dma_caps);
    const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (dma < low.dma)
      low = {now, App.get_loop_component_start_time(), internal, dma, dma_min};
    // Drops of 2 KB, rises of 8 KB (a task's stack given back), each new low of the heap's minimum
    const bool moved = internal + STEP <= last_internal || last_internal + 4 * STEP <= internal ||
                       dma + STEP <= last_dma || last_dma + 4 * STEP <= dma || dma_min + 512 <= last_min;
    if (moved) {
      if (count < SAMPLES) {
        samples[count++] = {now, App.get_loop_component_start_time(), internal, dma, dma_min};
      } else {
        full = true;
      }
      last_internal = internal;
      last_dma = dma;
      last_min = dma_min;
    }
    vTaskDelay(1);
  }
  ESP_LOGI("boot_ram", "Boot RAM (free bytes; the component's call started at):");
  for (size_t i = 0; i < count; i++) {
    const Sample &s = samples[i];
    ESP_LOGI("boot_ram", "%6" PRIu32 " ms (component since %6" PRIu32 "): internal %6" PRIu32 ", DMA %6" PRIu32
                         ", DMA low %6" PRIu32, s.ms, s.component_ms, s.internal, s.dma, s.dma_min);
  }
  if (full)
    ESP_LOGI("boot_ram", "(more changes than %u kept)", static_cast<unsigned>(SAMPLES));
  ESP_LOGI("boot_ram", "Lowest internal DMA free %" PRIu32 " at %" PRIu32 " ms (component since %" PRIu32
                       "), internal then %" PRIu32 ", heap minimum %" PRIu32,
           low.dma, low.ms, low.component_ms, low.internal, low.dma_min);
  vTaskDeleteWithCaps(nullptr);
}
#endif

void loop_task(void *pv_params) {
  setup();
  while (true) {
    App.loop();
  }
}

extern "C" void app_main() {
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
                                &loop_task_tcb, 1);
#endif
}

}  // namespace esphome

#endif  // USE_ESP32
