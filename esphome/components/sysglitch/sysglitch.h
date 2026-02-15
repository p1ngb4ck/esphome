#pragma once

#include <array>
#include <atomic>
#include <string>
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart_component.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_device.h"
#endif

namespace esphome::sysglitch {

// ── OCD protocol commands ──
static const uint8_t OCD_SYNC = 0x00;
static const uint8_t OCD_PING_CMD = 0x90;
static const uint8_t OCD_CONNECT_CMD = 0x91;
static const uint8_t OCD_READ_CMD = 0x92;
static const uint8_t OCD_WRITE_CMD = 0x93;
static const uint8_t OCD_EXEC_CMD = 0x94;
static const uint8_t OCD_EXIT_RETI = 0x95;
static const uint8_t OCD_EXIT_RAM = 0x97;

// ── ProtoA framing ──
static const uint8_t SOH = 0x01;
static const uint8_t STX = 0x02;
static const uint8_t ETX = 0x03;
static const uint8_t ETB = 0x17;
static const uint8_t ACK = 0x06;
static const uint8_t NACK = 0x15;

// ── ProtoA commands (serial bootloader) ──
static const uint8_t PA_RESET = 0x00;
static const uint8_t PA_VERIFY = 0x13;
static const uint8_t PA_ERASE = 0x22;
static const uint8_t PA_BLANK_CHECK = 0x32;
static const uint8_t PA_PROGRAM = 0x40;
static const uint8_t PA_BAUD_SET = 0x9A;
static const uint8_t PA_SILICON_SIG = 0xC0;
static const uint8_t PA_SEC_SET = 0xA0;
static const uint8_t PA_SEC_GET = 0xA1;
static const uint8_t PA_SEC_RLS = 0xA2;
static const uint8_t PA_CHECKSUM = 0xB0;

// ── OCD mode entry ──
static const uint8_t MODE_OCD = 0xC5;
static const uint8_t MODE_A_1WIRE = 0x3A;

// ── OCD unlock status codes ──
static const uint8_t OCD_UNLOCK_ALREADY = 0xF0;
static const uint8_t OCD_UNLOCK_LOCKED = 0xF1;
static const uint8_t OCD_UNLOCK_OK = 0xF2;

// ── Scflasher PC protocol commands (matches scflasher.py) ──
static const uint8_t SCF_PING1 = 0x00;
static const uint8_t SCF_PING2 = 0x01;
static const uint8_t SCF_READ_BLOCK = 0x02;
static const uint8_t SCF_READ_CHIP = 0x03;
static const uint8_t SCF_ERASE_BLOCK = 0x04;
static const uint8_t SCF_ERASE_CHIP = 0x05;
static const uint8_t SCF_WRITE_BLOCK = 0x06;
static const uint8_t SCF_WRITE_BLOCK_EX = 0x07;
static const uint8_t SCF_SET_DATA = 0x0A;
static const uint8_t SCF_INIT = 0x10;
static const uint8_t SCF_UNINIT = 0x20;
static const uint8_t SCF_RESET = 0x80;

// ── Scflasher status codes ──
static const uint8_t SCF_STATUS_OK = 0x00;
static const uint8_t SCF_ERR_INIT = 0xF0;
static const uint8_t SCF_ERR_READ = 0xF1;
static const uint8_t SCF_ERR_ERASE = 0xF4;
static const uint8_t SCF_ERR_WRITE = 0xF6;
static const uint8_t SCF_ERR_CMD_LEN = 0xFA;
static const uint8_t SCF_ERR_CMD_EXEC = 0xFE;
static const uint8_t SCF_ERR_UNKNOWN = 0xFF;

// ── Syscon flash geometry ──
static const uint32_t BLOCK_SIZE = 0x400;                     // 1KB per block
static const uint32_t BLOCK_COUNT = 512;                      // 512 blocks
static const uint32_t FLASH_SIZE = BLOCK_SIZE * BLOCK_COUNT;  // 512KB total

// ── Scflasher version (we emulate v2.05) ──
static const uint8_t SCF_VERSION_MAJOR = 2;
static const uint8_t SCF_VERSION_MINOR = 0x05;

// Shellcode: dumps full flash 0x0-0xFFFFF over TOOL0
// Writes to 0xF07E0, length 0x26
static const uint8_t SHELLCODE_DUMP[] = {
    0xe0, 0x07, 0x26,  // address(0x07E0) + length(0x26) header
    0x41, 0x00, 0x34, 0x00, 0x00, 0x00, 0x11, 0x89, 0xFC, 0xA1, 0xFF, 0x0E, 0xA5, 0x15, 0x44, 0x00, 0x00, 0xDF, 0xF3,
    0xEF, 0x04, 0x55, 0x00, 0x00, 0x00, 0x8E, 0xFD, 0x81, 0x5C, 0x0F, 0x9E, 0xFD, 0x71, 0x00, 0x90, 0x00, 0xEF, 0xE0,
};

// ── Operational modes ──
enum SysGlitchMode : uint8_t {
  MODE_DUMP_SD = 0,  // Dump flash to SD card (standalone, no PC needed)
  MODE_DUMP_UART,    // Dump flash over pc_uart (bridge mode)
  MODE_FLASHER,      // Scflasher-compatible protocol over pc_uart (read/write/erase)
};

enum SysGlitchState : uint8_t {
  STATE_IDLE = 0,
  STATE_GLITCHING,
  STATE_UPLOADING_SHELLCODE,
  STATE_DUMPING,
  STATE_FLASHER,  // scflasher-compatible protocol handler
  STATE_DONE,
  STATE_FAILED,
};

class SysGlitch : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Setters for UART channels
  void set_tool0_uart(uart::UARTComponent *uart) { this->tool0_uart_ = uart; }
  void set_pc_uart(uart::UARTComponent *uart) { this->pc_uart_ = uart; }

  // Setters for pins
  void set_reset_pin(InternalGPIOPin *pin) { this->reset_pin_ = pin; }
  void set_glitch_pin(InternalGPIOPin *pin) { this->glitch_pin_ = pin; }
  void set_rx_pulldown_pin(InternalGPIOPin *pin) { this->rx_pulldown_pin_ = pin; }

  // Setters for timing config
  void set_glitch_delay_min(uint32_t us) { this->glitch_delay_min_us_ = us; }
  void set_glitch_delay_max(uint32_t us) { this->glitch_delay_max_us_ = us; }
  void set_glitch_width_max(uint32_t ns) { this->glitch_width_max_ns_ = ns; }
  void set_passcode(const std::array<uint8_t, 10> &pc) { this->passcode_ = pc; }
  void set_max_attempts(uint32_t max) { this->max_attempts_ = max; }
  void set_mode(SysGlitchMode mode) { this->mode_ = mode; }
  void set_dump_path(const std::string &path) { this->dump_path_ = path; }

  // Action entry points (called from main loop / automations)
  void start_glitch();
  void stop_glitch();

  SysGlitchState get_state() const { return this->state_.load(std::memory_order_relaxed); }
  uint32_t get_attempt_count() const { return this->attempt_count_.load(std::memory_order_relaxed); }

 protected:
  // UART channels
  uart::UARTComponent *tool0_uart_{nullptr};
  uart::UARTComponent *pc_uart_{nullptr};

  // Pins
  InternalGPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *glitch_pin_{nullptr};
  InternalGPIOPin *rx_pulldown_pin_{nullptr};

  // Fast GPIO for timing-critical glitch pulse
  ISRInternalGPIOPin isr_glitch_pin_{};

  // Timing config
  uint32_t glitch_delay_min_us_{1500};
  uint32_t glitch_delay_max_us_{7500};
  uint32_t glitch_width_max_ns_{6300};

  // OCD passcode (10 bytes)
  std::array<uint8_t, 10> passcode_{};

  // Max attempts (0 = infinite)
  uint32_t max_attempts_{0};

  // Operational mode
  SysGlitchMode mode_{MODE_DUMP_SD};

  // Dump file path (for SD mode, e.g. "/sd/syscon_dump.bin")
  std::string dump_path_{"/sd/syscon_dump.bin"};

  // Whether OCD session is active (glitch succeeded, chip is unlocked)
  bool ocd_active_{false};

  // ── Shared state between cores (atomic for lock-free access) ──
  std::atomic<SysGlitchState> state_{STATE_IDLE};
  std::atomic<uint32_t> attempt_count_{0};
  std::atomic<bool> stop_requested_{false};

#ifdef USE_ESP32
  // FreeRTOS task handle for glitch task on core 1
  TaskHandle_t glitch_task_handle_{nullptr};

  // The glitch task entry point (static so it can be passed to xTaskCreatePinnedToCore)
  static void glitch_task_func_(void *param);

  // The actual glitch loop running on the dedicated core
  void run_glitch_loop_();
#endif

  // ── OCD operations ──
  void upload_and_execute_();
  uint8_t compute_ocd_checksum_(const uint8_t *data, uint8_t len);
  uint8_t compute_passcode_checksum_();

  // ── ProtoA operations (serial bootloader mode) ──
  // Enter ProtoA mode: reset chip with TOOL0=HIGH, send mode byte 0x3A
  bool enter_proto_a_();
  // ProtoA frame send/receive
  bool pa_send_frame_(const uint8_t *data, uint8_t len, bool is_cmd = true);
  bool pa_recv_frame_(uint8_t *buf, uint8_t *out_len, uint8_t max_len);
  uint8_t pa_checksum_(const uint8_t *data, uint8_t len);
  // ProtoA block operations
  bool pa_set_baudrate_();
  bool pa_erase_block_(uint32_t addr);
  bool pa_program_block_(uint32_t addr, const uint8_t *data, uint16_t len);
  bool pa_verify_block_(uint32_t addr, const uint8_t *data, uint16_t len);
  bool pa_read_block_(uint32_t block_num, uint8_t *out_data);

  // ── Scflasher protocol handler (runs on main loop) ──
  void handle_flasher_protocol_();
  void scf_handle_ping1_();
  void scf_handle_ping2_();
  void scf_handle_init_();
  void scf_handle_uninit_();
  void scf_handle_read_block_(const uint8_t *params, uint8_t len);
  void scf_handle_write_block_(const uint8_t *params, uint8_t len, bool extended);
  void scf_handle_erase_block_(const uint8_t *params, uint8_t len);
  void scf_handle_erase_chip_();
  void scf_handle_reset_();

  // UART bridge (runs on main loop after glitch succeeds, dump-uart mode)
  void bridge_uarts_();

  // Save flash dump to SD card (runs on main loop, dump-sd mode)
  void dump_to_sd_();
  uint32_t dump_bytes_received_{0};

  // Helper to read bytes with timeout
  bool read_bytes_timeout_(uart::UARTComponent *uart, uint8_t *buf, uint16_t len, uint32_t timeout_ms);

  // Nanosecond-precision busy wait using CPU cycle counter
  static void IRAM_ATTR delay_ns_(uint32_t ns);
};

}  // namespace esphome::sysglitch
