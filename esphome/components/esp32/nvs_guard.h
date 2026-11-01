#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESP32_NVS_GUARD)

#include <cstddef>
#include <cstdint>

namespace esphome::esp32::nvs_guard {

enum class Result : uint8_t { FRESH, SAME, ADOPTED, UPGRADED, DOWNGRADED, FOREIGN, CORRUPT };

enum class Policy : uint8_t { KEEP, INVALIDATE_CHANGED, RESET, READ_ONLY, RESET_ALL };

struct State {
  Result result{Result::FRESH};
  Policy applied{Policy::KEEP};
  bool requested{false};
  bool read_only{false};
  bool sweep_pending{false};
  int32_t init_err{0};
  uint16_t invalidated{0};
  uint16_t restored{0};
  uint8_t *backup{nullptr};
  size_t backup_size{0};
};

const State &state();
const char *result_name(Result result);
const char *policy_name(Policy policy);
bool parse_policy(const char *text, Policy *out);

bool open_partition(bool *nvs_ready);
void claim(uint32_t key, size_t len);
uint16_t sweep(uint32_t handle);
uint16_t erase_stale(uint32_t handle);
void release_backup();

bool request(Policy policy);
bool is_protected(const char *ns);

size_t snapshot(bool protected_only, uint8_t **out);
bool restore(const uint8_t *data, size_t size, bool protected_only, uint16_t *count);
bool reset(bool keep_protected, uint16_t *restored);
size_t verify(uint16_t *unreadable);

}

#endif
