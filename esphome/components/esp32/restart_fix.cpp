#include "esphome/core/defines.h"

#ifdef USE_ESP32_P4_RESTART_FIX

#include <esp_attr.h>
#include <esp_cpu.h>
#include <riscv/rv_utils.h>
#include <soc/cache_reg.h>
#include <soc/soc.h>

// ESP32-P4: IDF's esp_restart_noos() resets the other core and only then stalls it (IDF 6.1
// esp_system/port/soc/esp32p4/system_internal.c, esp_restart_noos). A cache sync that core had in flight
// leaves the shared sync engine busy, and the restart's Cache_WriteBack_All(CACHE_MAP_L1_DCACHE) faults on its
// first write to the sync registers (store access fault, mtval CACHE_SYNC_MAP_REG) - or hangs there, with no
// watchdog to end it: the panel stays dead until a power cycle (2026-10-19). espressif/esp-idf#19171.
//
// Linked in through -Wl,--wrap=esp_restart_noos (esp32/__init__.py): every restart (esp_restart, the panic
// handler's, the startup's) comes here first. The other core is stalled (not reset), the sync engine is given
// the time to finish what it runs, then IDF's own esp_restart_noos() does the rest unchanged.

extern "C" void __real_esp_restart_noos(void);

// Enough for the longest writeback of the whole L1 data cache many times over; bounded so a broken engine
// cannot hold the restart (IDF's RTC watchdog, armed inside the real function, follows it).
static constexpr uint32_t SYNC_WAIT_SPINS = 10000000;

extern "C" void IRAM_ATTR __wrap_esp_restart_noos(void) {
  rv_utils_intr_global_disable();
#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE
  const int other = esp_cpu_get_core_id() == 0 ? 1 : 0;
  esp_cpu_stall(other);
#endif
  // A sync in progress clears SYNC_DONE until it is finished
  for (uint32_t i = 0; i < SYNC_WAIT_SPINS && !REG_GET_BIT(CACHE_SYNC_CTRL_REG, CACHE_SYNC_DONE); i++) {
  }
  __real_esp_restart_noos();
}

#endif  // USE_ESP32_P4_RESTART_FIX
