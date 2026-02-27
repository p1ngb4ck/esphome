#pragma once

#include <atomic>
#include <array>
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/binary_storage/binary_storage.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#endif

namespace esphome::ps_tools {

// ── RL78/G13 OCD protocol commands ──────────────────────────────────────────
static const uint8_t OCD_SYNC = 0x00;
static const uint8_t OCD_PING_CMD = 0x90;
static const uint8_t OCD_CONNECT_CMD = 0x91;
static const uint8_t OCD_READ_CMD = 0x92;
static const uint8_t OCD_WRITE_CMD = 0x93;
static const uint8_t OCD_EXEC_CMD = 0x94;
static const uint8_t OCD_EXIT_RETI = 0x95;
static const uint8_t OCD_EXIT_RAM = 0x97;

// ── OCD unlock status codes ──────────────────────────────────────────────────
static const uint8_t OCD_UNLOCK_ALREADY = 0xF0;
static const uint8_t OCD_UNLOCK_LOCKED = 0xF1;
static const uint8_t OCD_UNLOCK_OK = 0xF2;

// ── OCD / ProtoA mode entry bytes ────────────────────────────────────────────
static const uint8_t MODE_OCD = 0xC5;
static const uint8_t MODE_A_1WIRE = 0x3A;

// ── ProtoA framing bytes ─────────────────────────────────────────────────────
static const uint8_t PA_SOH = 0x01;
static const uint8_t PA_STX = 0x02;
static const uint8_t PA_ETX = 0x03;
static const uint8_t PA_ETB = 0x17;
static const uint8_t PA_ACK = 0x06;
static const uint8_t PA_NACK = 0x15;

// ── ProtoA command bytes ─────────────────────────────────────────────────────
static const uint8_t PA_CMD_RESET = 0x00;
static const uint8_t PA_CMD_VERIFY = 0x13;
static const uint8_t PA_CMD_ERASE = 0x22;
static const uint8_t PA_CMD_BLANK_CHECK = 0x32;
static const uint8_t PA_CMD_PROGRAM = 0x40;
static const uint8_t PA_CMD_BAUD_SET = 0x9A;
static const uint8_t PA_CMD_SILICON_SIG = 0xC0;
static const uint8_t PA_CMD_SEC_SET = 0xA0;
static const uint8_t PA_CMD_SEC_GET = 0xA1;
static const uint8_t PA_CMD_SEC_RLS = 0xA2;
static const uint8_t PA_CMD_CHECKSUM = 0xB0;

// ── Scflasher PC protocol (emulates Teensy flasher v2.05) ───────────────────
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

// ── Scflasher status codes ───────────────────────────────────────────────────
static const uint8_t SCF_STATUS_OK = 0x00;
static const uint8_t SCF_ERR_INIT = 0xF0;
static const uint8_t SCF_ERR_READ = 0xF1;
static const uint8_t SCF_ERR_ERASE = 0xF4;
static const uint8_t SCF_ERR_WRITE = 0xF6;
static const uint8_t SCF_ERR_CMD_LEN = 0xFA;
static const uint8_t SCF_ERR_CMD_EXEC = 0xFE;
static const uint8_t SCF_ERR_UNKNOWN = 0xFF;

// ── Scflasher emulated version ───────────────────────────────────────────────
static const uint8_t SCF_VERSION_MAJOR = 2;
static const uint8_t SCF_VERSION_MINOR = 0x05;

// ── Syscon flash geometry ────────────────────────────────────────────────────
static const uint32_t SYSCON_BLOCK_SIZE = 0x400;                                   // 1 KB per erase block
static const uint32_t SYSCON_BLOCK_COUNT = 512;                                    // 512 blocks
static const uint32_t SYSCON_FLASH_SIZE = SYSCON_BLOCK_SIZE * SYSCON_BLOCK_COUNT;  // 512 KB
static const uint32_t SYSCON_PAGE_SIZE = 0x100;                                    // 256 B program page

// ── Shellcode: dumps full flash 0x00000–0x7FFFF over TOOL0 ──────────────────
// Writes to RL78 RAM at 0xF07E0, length 0x26 bytes
static const uint8_t SHELLCODE_DUMP[] = {
    0xe0, 0x07, 0x26,  // address(0x07E0) + length(0x26) header
    0x41, 0x00, 0x34, 0x00, 0x00, 0x00, 0x11, 0x89, 0xFC, 0xA1, 0xFF, 0x0E, 0xA5, 0x15, 0x44, 0x00, 0x00, 0xDF, 0xF3,
    0xEF, 0x04, 0x55, 0x00, 0x00, 0x00, 0x8E, 0xFD, 0x81, 0x5C, 0x0F, 0x9E, 0xFD, 0x71, 0x00, 0x90, 0x00, 0xEF, 0xE0,
};

// ── Shellcode: resident flash write agent (reverse-engineered from Abkarino Teensy flasher v2.05)
// Two OCD_WRITE payloads:
//   1. 192-byte write-agent uploaded to RL78 RAM 0xFB00
//   2. 4-byte entry stub uploaded to RL78 RAM 0xF07E0 (OCD_EXEC jumps here → 0xFB00)
//
// After OCD_EXEC the agent loops receiving commands over TOOL0 UART:
//   Send 'w' (0x77) + 4-byte LE flash addr → RL78 erases block → send 1024 data bytes → recv 1 ACK
//
// OCD_WRITE #1 payload: addr_lo=0x00 addr_hi=0xFB count=0xC0 data[192]
static const uint8_t SHELLCODE_WRITE_AGENT[] = {
    0x00,
    0xFB,
    0xC0,  // OCD_WRITE header: addr=0xFB00, count=192
    // 192-byte RL78 flash write agent (extracted from Teensy flasher AVR data section)
    0x13,
    0xBF,
    0xE6,
    0x07,
    0xC0,
    0xBF,
    0xEC,
    0x07,
    0xC0,
    0xBF,
    0xEE,
    0x07,
    0xCF,
    0xEB,
    0x07,
    0xEC,
    0xF5,
    0xEA,
    0x07,
    0xFC,
    0xB2,
    0xFF,
    0x0E,
    0x72,
    0xFC,
    0xB2,
    0xFF,
    0x0E,
    0x76,
    0xFC,
    0xB2,
    0xFF,
    0x0E,
    0x77,
    0xFC,
    0xB2,
    0xFF,
    0x0E,
    0x9E,
    0xFD,
    0xFC,
    0xB2,
    0xFF,
    0x0E,
    0x73,
    0x62,
    0x4C,
    0x69,
    0xDD,
    0x6D,
    0x4C,
    0x77,
    0xDD,
    0x1D,
    0x4C,
    0x72,
    0xDD,
    0x29,
    0x4C,
    0x65,
    0xDD,
    0x32,
    0x4C,
    0x75,
    0xDD,
    0x62,
    0xFC,
    0xA1,
    0xFF,
    0x0E,
    0xD5,
    0xEA,
    0x07,
    0xDF,
    0xC8,
    0xAF,
    0xE6,
    0x07,
    0x12,
    0xEC,
    0xEB,
    0x07,
    0x0F,
    0xFC,
    0xB2,
    0xFF,
    0x0E,
    0x11,
    0x9B,
    0xA7,
    0x93,
    0xDF,
    0xF6,
    0xFE,
    0x4C,
    0x00,
    0xEE,
    0xE3,
    0xFF,
    0x11,
    0x8B,
    0xFC,
    0xA1,
    0xFF,
    0x0E,
    0xA7,
    0x93,
    0xDF,
    0xF6,
    0xEE,
    0xD6,
    0xFF,
    0x61,
    0xFF,
    0xFC,
    0xF8,
    0xFF,
    0x0E,
    0x8F,
    0x02,
    0x08,
    0x4C,
    0x0F,
    0xDD,
    0x0B,
    0x62,
    0x4C,
    0xFF,
    0xDF,
    0x11,
    0xFC,
    0xC4,
    0x08,
    0x0F,
    0xEF,
    0xF5,
    0x62,
    0x4C,
    0xFF,
    0xDF,
    0x06,
    0xFC,
    0x04,
    0xF0,
    0x0E,
    0xEF,
    0xF5,
    0xC3,
    0x61,
    0xCF,
    0xC2,
    0x62,
    0xFC,
    0xA1,
    0xFF,
    0x0E,
    0xEE,
    0xA7,
    0xFF,
    0xE5,
    0xEA,
    0x07,
    0xEF,
    0x03,
    0xE5,
    0xEA,
    0x07,
    0xFE,
    0x02,
    0x00,
    0xEF,
    0x9A,
    0x51,
    0x00,
    0xFC,
    0xA1,
    0xFF,
    0x0E,
    0xD7,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
};

// OCD_WRITE #2 payload: entry stub at 0xF07E0 (addr_lo=0xE0 addr_hi=0x07 count=4)
static const uint8_t SHELLCODE_WRITE_STUB[] = {
    0xE0, 0x07, 0x04,        // OCD_WRITE header: addr=0x07E0, count=4
    0xEC, 0x00, 0xFB, 0x0F,  // RL78: MOVW HL,#0xFB00 + branch to shellcode
};

// ── PSRAM buffer size for full syscon dump ───────────────────────────────────
static const uint32_t PSRAM_SYSCON_BUF_SIZE = SYSCON_FLASH_SIZE;  // 512 KB

// ── Operational modes ────────────────────────────────────────────────────────
enum PsToolsMode : uint8_t {
  // OCD glitch modes (locked/original chip)
  MODE_GLITCH_DUMP = 0,  // Glitch → OCD → shellcode → dump 512 KB to storage path
  MODE_GLITCH_FLASHER,   // Glitch → OCD → scflasher v2.05 protocol on pc_uart
  MODE_GLITCH_WRITE,     // Glitch → OCD → write-agent shellcode → erase+write from file
  // ProtoA modes (stock/blank/erased chip)
  MODE_PROTO_A_WRITE,        // ProtoA → erase all → program from file (bin or mot)
  MODE_PROTO_A_READ,         // ProtoA → read all blocks → dump to file (unprotected only)
  MODE_PROTO_A_BLANK_CHECK,  // ProtoA → blank check only → log result
};

// ── Component state machine ──────────────────────────────────────────────────
enum PsToolsState : uint8_t {
  STATE_IDLE = 0,
  STATE_GLITCHING,  // Glitch loop running on core 1
  STATE_UPLOADING_SHELLCODE,
  STATE_DUMPING,         // Streaming dump to storage or pc_uart
  STATE_FLASHER,         // Scflasher protocol active
  STATE_GLITCH_WRITING,  // Glitch → OCD write-agent → block write in progress
  STATE_WRITING,         // ProtoA write in progress
  STATE_READING,         // ProtoA read in progress
  STATE_OCD_READING,     // OCD read in progress
  STATE_BLANK_CHECKING,  // ProtoA blank check in progress
  STATE_ANALYZING_NOR,   // PS4 NOR flash analysis running on core 1
  STATE_PROBING,         // Systematic access probe running on core 1
  STATE_DONE,
  STATE_FAILED,
};

// ── Main component class ─────────────────────────────────────────────────────
class PsTools : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── UART channels ──
  void set_tool0_uart(uart::UARTComponent *uart) { this->tool0_uart_ = uart; }
  void set_pc_uart(uart::UARTComponent *uart) { this->pc_uart_ = uart; }

  // ── GPIO pins ──
  void set_reset_pin(InternalGPIOPin *pin) { this->reset_pin_ = pin; }
  // Controls VDD for glitch pulse (switches chip power momentarily)
  void set_glitch_pin(InternalGPIOPin *pin) { this->glitch_pin_ = pin; }
  // Optional: pull RX line low during glitch entry to prevent false OCD sync
  void set_rx_pulldown_pin(InternalGPIOPin *pin) { this->rx_pulldown_pin_ = pin; }
  // GPIO number of TOOL0 UART TX pin (needed to drive LOW during ProtoA entry)
  void set_tool0_tx_gpio(uint8_t pin) { this->tool0_tx_gpio_ = pin; }

  // ── Mode and paths ──
  void set_mode(PsToolsMode mode) { this->mode_ = mode; }
  void set_dump_path(const std::string &path) { this->dump_path_ = path; }
  void set_write_path(const std::string &path) { this->write_path_ = path; }
  void set_write_is_mot(bool is_mot) { this->write_is_mot_ = is_mot; }

  // ── Glitch timing parameters ──
  void set_glitch_delay_min(uint32_t us) { this->glitch_delay_min_us_ = us; }
  void set_glitch_delay_max(uint32_t us) { this->glitch_delay_max_us_ = us; }
  void set_glitch_width_max(uint32_t ns) { this->glitch_width_max_ns_ = ns; }
  void set_max_attempts(uint32_t max) { this->max_attempts_ = max; }

  // ── OCD passcode (10 bytes) ──
  void set_passcode(const std::array<uint8_t, 10> &pc) { this->passcode_ = pc; }

  // ── ProtoA operating voltage byte (voltage * 10, e.g. 0x32 = 5.0V, 0x21 = 3.3V) ──
  void set_voltage_byte(uint8_t vb) { this->voltage_byte_ = vb; }

  // ── NOR flash chip identifier (informational, for logging) ──
  void set_nor_chip_id(const std::string &id) { this->nor_chip_id_ = id; }

  // ── NOR flash device (optional — 32MB PS4 SPI NOR, via binary_storage) ──
  void set_nor_flash(esphome::binary_storage::BinaryStorage *flash) { this->nor_flash_ = flash; }

  // ── Path to existing 32MB NOR dump on SD (used when nor_flash not wired) ──
  void set_nor_dump_path(const std::string &path) { this->nor_dump_path_ = path; }

  // ── Action entry points (called from automations or loop) ──
  void start_glitch();
  void start_glitch_write();
  void stop();
  void start_write();
  void start_read();
  void start_ocd_read();
  void start_blank_check();

  // ── NOR analysis: reads NOR hardware or dump file, logs PS4-specific info ──
  void start_analyze_nor();

  // ── Syscon probe: systematically tries every known access path and passcode
  //    variant. Designed for chips whose flash state is unknown (e.g. erased).
  //    Reports what works and what doesn't — does NOT write anything. ──
  void start_probe_syscon();

  // ── State accessors ──
  PsToolsState get_state() const { return this->state_.load(std::memory_order_relaxed); }
  uint32_t get_attempt_count() const { return this->attempt_count_.load(std::memory_order_relaxed); }
  uint32_t get_progress_bytes() const { return this->progress_bytes_.load(std::memory_order_relaxed); }
  bool is_blank() const { return this->blank_check_result_; }

 protected:
  // ── UART channels ──
  uart::UARTComponent *tool0_uart_{nullptr};
  uart::UARTComponent *pc_uart_{nullptr};

  // ── Pins ──
  InternalGPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *glitch_pin_{nullptr};
  InternalGPIOPin *rx_pulldown_pin_{nullptr};
  uint8_t tool0_tx_gpio_{0};

  // ── ISR-safe glitch pin for nanosecond timing ──
  ISRInternalGPIOPin isr_glitch_pin_{};

  // ── Mode and paths ──
  PsToolsMode mode_{MODE_GLITCH_DUMP};
  std::string dump_path_{"/sd/syscon_dump.bin"};
  std::string write_path_{"/sd/syscon.bin"};
  // True if write_path is a Motorola S-record (.mot) file; false = raw binary
  bool write_is_mot_{false};

  // ── Glitch timing ──
  uint32_t glitch_delay_min_us_{1500};
  uint32_t glitch_delay_max_us_{7500};
  uint32_t glitch_width_max_ns_{6300};
  uint32_t max_attempts_{0};  // 0 = infinite

  // ── OCD ──
  std::array<uint8_t, 10> passcode_{};
  bool ocd_active_{false};

  // ── ProtoA ──
  bool proto_a_active_{false};
  // Voltage byte for PA_CMD_BAUD_SET: voltage * 10 (0x32 = 5.0V, 0x21 = 3.3V)
  uint8_t voltage_byte_{0x32};

  // ── NOR flash info (informational only) ──
  std::string nor_chip_id_{};

  // ── NOR flash device (32MB PS4 SPI NOR via binary_storage, optional) ──
  esphome::binary_storage::BinaryStorage *nor_flash_{nullptr};

  // ── Path to existing 32MB NOR dump on SD (offline analysis, optional) ──
  std::string nor_dump_path_{};

  // ── Result of last blank check ──
  bool blank_check_result_{false};

  // ── Shared atomic state (main loop + FreeRTOS task) ──
  std::atomic<PsToolsState> state_{STATE_IDLE};
  std::atomic<uint32_t> attempt_count_{0};
  std::atomic<uint32_t> progress_bytes_{0};  // bytes written/read so far
  std::atomic<bool> stop_requested_{false};

#ifdef USE_ESP32
  // ── FreeRTOS task handles ──
  TaskHandle_t task_handle_{nullptr};

  void spawn_task_(const char *name);
  static void task_func_(void *param);
  void run_task_();

  // ── Per-mode task bodies (called from run_task_) ──
  void run_glitch_loop_();
  void run_glitch_write_loop_();
  void run_write_loop_();
  void run_read_loop_();
  void run_ocd_read_();
  void run_blank_check_loop_();
  void run_analyze_nor_();   // PS4 NOR analysis: southbridge, torus, FW version, etc.
  void run_probe_syscon_();  // Systematic access probe: ProtoA + OCD variants
#endif

  // ── OCD operations ──
  void upload_and_execute_shellcode_();
  // Uploads the 192-byte write-agent + 4-byte stub via OCD_WRITE, then OCD_EXEC
  void upload_write_agent_();
  // Sends one 1KB block to the running write-agent: 'w' + 4-byte LE addr + 1024 bytes of data
  // Returns true on ACK, false on timeout/error
  bool ocd_write_block_(uint32_t addr, const uint8_t *data);
  uint8_t compute_ocd_checksum_(const uint8_t *data, uint8_t len);
  uint8_t compute_passcode_checksum_();

  // ── ProtoA entry ──
  // Drives RESET and TOOL0 to enter RL78 serial bootloader mode.
  // Requires tool0_tx_gpio_ to directly drive TX pin LOW during reset.
  bool enter_proto_a_();

  // ── ProtoA framing ──
  // is_cmd=true → SOH header (command frame); false → STX header (data frame)
  bool pa_send_frame_(const uint8_t *data, uint8_t len, bool is_cmd = true);
  bool pa_recv_frame_(uint8_t *buf, uint8_t *out_len, uint8_t max_len);
  uint8_t pa_checksum_(const uint8_t *data, uint8_t len);

  // ── ProtoA block operations ──
  bool pa_set_baudrate_();
  bool pa_erase_block_(uint32_t addr);
  bool pa_program_block_(uint32_t addr, const uint8_t *data, uint16_t len);
  bool pa_verify_block_(uint32_t addr, const uint8_t *data, uint16_t len);
  bool pa_blank_check_block_(uint32_t addr);

  // ── Scflasher protocol (runs on main loop, MODE_GLITCH_FLASHER) ──
  void handle_flasher_protocol_();
  void scf_handle_ping1_();
  void scf_handle_ping2_();
  void scf_handle_init_();
  void scf_handle_uninit_();
  void scf_handle_read_block_(const uint8_t *params, uint8_t len);
  void scf_handle_write_block_(const uint8_t *params, uint16_t len, bool extended);
  void scf_handle_erase_block_(const uint8_t *params, uint8_t len);
  void scf_handle_erase_chip_();
  void scf_handle_reset_();

  // ── Dump helpers ──
  void bridge_uarts_();  // Stream OCD dump to pc_uart (MODE_GLITCH_FLASHER)
#ifdef USE_ESP32
  void run_dump_on_task_(int uart_num);  // Receive dump via uart_read_bytes() on core-1 task, then save
#endif

  // ── .mot (Motorola S-record) parser ──
  // Reads .mot file from write_path_, decodes into a PSRAM buffer.
  // Returns allocated buffer (caller must heap_caps_free) and sets out_size.
  // Returns nullptr on failure.
  uint8_t *load_mot_to_psram_(uint32_t *out_size);

  // ── Raw binary loader ──
  // Reads raw .bin file from write_path_ into a PSRAM buffer.
  // Returns allocated buffer (caller must heap_caps_free) and sets out_size.
  // Returns nullptr on failure.
  uint8_t *load_bin_to_psram_(uint32_t *out_size);

  // ── UART read helper ──
  bool read_bytes_timeout_(uart::UARTComponent *uart, uint8_t *buf, uint16_t len, uint32_t timeout_ms);

  // ── Nanosecond-precision busy wait (CPU cycle counter) ──
  static void IRAM_ATTR delay_ns_(uint32_t ns);

  // ── Logging helper ──
  const char *mode_str_() const;
};

}  // namespace esphome::ps_tools
