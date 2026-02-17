#include "sysglitch.h"

#include <cstring>
#include "esphome/core/hal.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_cpu.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include "esphome/components/uart/uart_component_esp_idf.h"
#endif

namespace esphome::sysglitch {

static const char *const TAG = "sysglitch";

// Stack size for the dedicated glitch task (needs enough for UART driver ops)
static const uint32_t GLITCH_TASK_STACK_SIZE = 4096;

// Task priority — higher than ESPHome main loop (1) but below WiFi (23)
static const UBaseType_t GLITCH_TASK_PRIORITY = 5;

// Which core to pin the glitch task to.
// ESPHome main loop runs on core 0, so we use core 1 for uninterrupted timing.
// On single-core ESP32 variants this falls back to core 0.
#if defined(CONFIG_FREERTOS_UNICORE) || (portNUM_PROCESSORS == 1)
static const BaseType_t GLITCH_TASK_CORE = 0;
#else
static const BaseType_t GLITCH_TASK_CORE = 1;
#endif

void SysGlitch::setup() {
  // Configure GPIO pins
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);  // Reset HIGH (inactive)

  // Glitch and RX pulldown pins are optional (not needed for write mode)
  if (this->glitch_pin_ != nullptr) {
    this->glitch_pin_->setup();
    this->glitch_pin_->digital_write(true);  // Glitch pin HIGH (inactive)
    this->isr_glitch_pin_ = this->glitch_pin_->to_isr();
  }
  if (this->rx_pulldown_pin_ != nullptr) {
    this->rx_pulldown_pin_->setup();
    this->rx_pulldown_pin_->digital_write(false);  // RX pulldown LOW
  }

  ESP_LOGI(TAG, "SysGlitch initialized");

  const char *mode_str = "unknown";
  switch (this->mode_) {
    case MODE_DUMP_SD:
      mode_str = "dump-to-sd";
      break;
    case MODE_DUMP_UART:
      mode_str = "dump-to-uart";
      break;
    case MODE_FLASHER:
      mode_str = "flasher";
      break;
    case MODE_WRITE:
      mode_str = "write";
      break;
  }
  ESP_LOGI(TAG, "  Mode: %s", mode_str);
  if (this->mode_ == MODE_DUMP_SD) {
    ESP_LOGI(TAG, "  Dump path: %s", this->dump_path_.c_str());
  }
  if (this->mode_ == MODE_WRITE) {
    ESP_LOGI(TAG, "  Write path: %s", this->write_path_.c_str());
  }
  if (this->mode_ != MODE_WRITE) {
    ESP_LOGI(TAG, "  Glitch delay: %u-%u us", this->glitch_delay_min_us_, this->glitch_delay_max_us_);
    ESP_LOGI(TAG, "  Glitch width max: %u ns", this->glitch_width_max_ns_);
    ESP_LOGI(TAG, "  Max attempts: %u (0=infinite)", this->max_attempts_);
    ESP_LOGI(TAG, "  Glitch task pinned to core %d", GLITCH_TASK_CORE);
  }
}

void SysGlitch::dump_config() {
  ESP_LOGCONFIG(TAG, "SysGlitch:");
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  if (this->glitch_pin_ != nullptr) {
    LOG_PIN("  Glitch Pin: ", this->glitch_pin_);
  }
  if (this->rx_pulldown_pin_ != nullptr) {
    LOG_PIN("  RX Pulldown Pin: ", this->rx_pulldown_pin_);
  }
  const char *mode_str = "unknown";
  switch (this->mode_) {
    case MODE_DUMP_SD:
      mode_str = "dump-to-sd";
      break;
    case MODE_DUMP_UART:
      mode_str = "dump-to-uart";
      break;
    case MODE_FLASHER:
      mode_str = "flasher";
      break;
    case MODE_WRITE:
      mode_str = "write";
      break;
  }
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_str);
  if (this->mode_ == MODE_WRITE) {
    ESP_LOGCONFIG(TAG, "  Write path: %s", this->write_path_.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Glitch delay: %u-%u us", this->glitch_delay_min_us_, this->glitch_delay_max_us_);
    ESP_LOGCONFIG(TAG, "  Glitch width max: %u ns", this->glitch_width_max_ns_);
    ESP_LOGCONFIG(TAG, "  Max attempts: %u", this->max_attempts_);
    ESP_LOGCONFIG(TAG, "  Glitch core: %d", GLITCH_TASK_CORE);
  }
  if (this->mode_ == MODE_DUMP_SD) {
    ESP_LOGCONFIG(TAG, "  Dump path: %s", this->dump_path_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  PC UART: %s", this->pc_uart_ != nullptr ? "yes" : "no");
}

void SysGlitch::loop() {
  auto state = this->state_.load(std::memory_order_acquire);

  // Flasher mode: always handle PC protocol, regardless of state
  if (this->mode_ == MODE_FLASHER && this->pc_uart_ != nullptr) {
    this->handle_flasher_protocol_();
  }

  switch (state) {
    case STATE_IDLE:
      break;

    case STATE_GLITCHING:
      // Glitching runs on dedicated core — just log progress here
      {
        uint32_t count = this->attempt_count_.load(std::memory_order_relaxed);
        static uint32_t last_logged = 0;
        if (count - last_logged >= 500) {
          ESP_LOGI(TAG, "Glitch attempt %u...", count);
          last_logged = count;
        }
      }
      break;

    case STATE_UPLOADING_SHELLCODE:
      // Glitch task handles upload + exec, then transitions
      break;

    case STATE_DUMPING:
      // Dump mode: either save to SD or bridge to PC UART
      if (this->mode_ == MODE_DUMP_SD) {
        this->dump_to_sd_();
      } else {
        this->bridge_uarts_();
      }
      break;

    case STATE_FLASHER:
      // Handled above the switch — always active in flasher mode
      break;

    case STATE_WRITING:
      // Write runs on dedicated FreeRTOS task — just log progress here
      break;

    case STATE_DONE:
      ESP_LOGI(TAG, "Operation complete.");
      this->state_.store(STATE_IDLE, std::memory_order_release);
      break;

    case STATE_FAILED: {
      uint32_t count = this->attempt_count_.load(std::memory_order_relaxed);
      ESP_LOGW(TAG, "Glitch failed after %u attempts", count);
      this->state_.store(STATE_IDLE, std::memory_order_release);
      break;
    }
  }
}

void SysGlitch::start_glitch() {
  auto current = this->state_.load(std::memory_order_acquire);
  if (current != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start glitch - not idle (state=%u)", current);
    return;
  }

  ESP_LOGI(TAG, "Starting glitch attack...");
  this->attempt_count_.store(0, std::memory_order_relaxed);
  this->stop_requested_.store(false, std::memory_order_release);
  this->ocd_active_ = false;
  this->state_.store(STATE_GLITCHING, std::memory_order_release);

  // Reset pin states
  this->reset_pin_->digital_write(true);
  if (this->rx_pulldown_pin_ != nullptr) {
    this->rx_pulldown_pin_->digital_write(false);
  }
  if (this->glitch_pin_ != nullptr) {
    this->glitch_pin_->digital_write(true);
  }

#ifdef USE_ESP32
  // Spawn glitch task on dedicated core
  BaseType_t result = xTaskCreatePinnedToCore(glitch_task_func_, "sysglitch", GLITCH_TASK_STACK_SIZE, this,
                                              GLITCH_TASK_PRIORITY, &this->glitch_task_handle_, GLITCH_TASK_CORE);

  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create glitch task!");
    this->state_.store(STATE_FAILED, std::memory_order_release);
  }
#else
  ESP_LOGE(TAG, "SysGlitch requires ESP32 platform");
  this->state_.store(STATE_FAILED, std::memory_order_release);
#endif
}

void SysGlitch::stop_glitch() {
  auto current = this->state_.load(std::memory_order_acquire);
  uint32_t count = this->attempt_count_.load(std::memory_order_relaxed);
  ESP_LOGI(TAG, "Stop requested (state=%u, attempts=%u)", current, count);

  this->stop_requested_.store(true, std::memory_order_release);
  this->ocd_active_ = false;
}

#ifdef USE_ESP32

void SysGlitch::glitch_task_func_(void *param) {
  auto *self = static_cast<SysGlitch *>(param);
  self->run_glitch_loop_();
  self->glitch_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SysGlitch::run_glitch_loop_() {
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

  ESP_LOGI(TAG, "Glitch task started on core %d (UART %d)", xPortGetCoreID(), uart_num);

  while (!this->stop_requested_.load(std::memory_order_acquire)) {
    uint32_t attempt = this->attempt_count_.fetch_add(1, std::memory_order_relaxed);

    // Check max attempts
    if (this->max_attempts_ > 0 && attempt >= this->max_attempts_) {
      ESP_LOGW(TAG, "Max attempts (%u) reached", this->max_attempts_);
      this->state_.store(STATE_FAILED, std::memory_order_release);
      this->reset_pin_->digital_write(true);
      if (this->glitch_pin_ != nullptr)
        this->glitch_pin_->digital_write(true);
      if (this->rx_pulldown_pin_ != nullptr)
        this->rx_pulldown_pin_->digital_write(false);
      return;
    }

    // ── Single glitch attempt (timing-critical section) ──

    // Step 1: Pull reset LOW
    this->reset_pin_->digital_write(false);
    esp_rom_delay_us(80);

    // Step 2: Disable UART driver → TX pin goes low → TOOL0 held low
    uart_driver_delete(uart_num);
    esp_rom_delay_us(1800);

    // Step 3: Release reset with TOOL0 low → chip enters OCD mode
    this->reset_pin_->digital_write(true);
    esp_rom_delay_us(2000);

    // Step 4: Re-enable UART driver
    idf_uart->load_settings(false);
    esp_rom_delay_us(1000);

    // Step 5: Debugger init command
    this->tool0_uart_->write_byte(MODE_OCD);
    esp_rom_delay_us(1000);

    // Step 6: Baud rate set frame
    // SOH | length(3) | 0x9A | 0x00(115200) | 0x22(2.4V) | checksum(0x41) | ETX
    const uint8_t baud_frame[] = {SOH, 0x03, PA_BAUD_SET, 0x00, 0x22, 0x41, ETX};
    this->tool0_uart_->write_array(baud_frame, sizeof(baud_frame));
    this->tool0_uart_->flush();

    // Steps 7-8: Glitch pulse (only if glitch_pin is configured)
    if (this->glitch_pin_ != nullptr) {
      // Step 7: Random delay in glitch window
      uint32_t delay_range = this->glitch_delay_max_us_ - this->glitch_delay_min_us_;
      uint32_t random_delay_us = this->glitch_delay_min_us_;
      if (delay_range > 0) {
        random_delay_us += esp_random() % delay_range;
      }
      esp_rom_delay_us(random_delay_us);

      // Step 8: Glitch pulse — nanosecond precision via cycle counter
      uint32_t glitch_width_ns = 0;
      if (this->glitch_width_max_ns_ > 0) {
        glitch_width_ns = esp_random() % this->glitch_width_max_ns_;
      }

      this->isr_glitch_pin_.digital_write(false);
      if (glitch_width_ns > 0) {
        delay_ns_(glitch_width_ns);
      }
      this->isr_glitch_pin_.digital_write(true);
    }

    // Step 9: Wait for chip to process the glitch, then attempt OCD connect
    esp_rom_delay_us(15000);

    this->tool0_uart_->write_byte(OCD_CONNECT_CMD);
    esp_rom_delay_us(1000);

    // Step 10: Send 10-byte passcode + checksum
    this->tool0_uart_->write_array(this->passcode_.data(), 10);
    this->tool0_uart_->write_byte(this->compute_passcode_checksum_());
    this->tool0_uart_->flush();

    // Step 11: Check for OCD unlock response
    // 0xF0 = already unlocked, 0xF2 = unlock success, 0xF1 = locked/failed
    esp_rom_delay_us(5000);

    bool got_unlock = false;
    while (this->tool0_uart_->available()) {
      uint8_t rx_byte;
      if (this->tool0_uart_->read_byte(&rx_byte)) {
        if (rx_byte == OCD_UNLOCK_ALREADY || rx_byte == OCD_UNLOCK_OK) {
          ESP_LOGI(TAG, "OCD response: 0x%02X (%s)", rx_byte,
                   rx_byte == OCD_UNLOCK_ALREADY ? "already unlocked" : "unlock OK");
          got_unlock = true;
          break;
        } else if (rx_byte == OCD_UNLOCK_LOCKED) {
          ESP_LOGD(TAG, "OCD response: 0xF1 (locked - glitch needed)");
        } else if ((attempt % 100) == 0) {
          ESP_LOGD(TAG, "OCD response byte: 0x%02X", rx_byte);
        }
      }
    }

    if (got_unlock) {
      ESP_LOGI(TAG, "*** GLITCH SUCCESS after %u attempts! ***", attempt + 1);
      this->ocd_active_ = true;

      if (this->mode_ == MODE_FLASHER) {
        // Flasher mode: enter scflasher-compatible protocol handler
        // The PC tool (ps4-wee-tools) will send commands over pc_uart
        if (this->rx_pulldown_pin_ != nullptr) {
          this->rx_pulldown_pin_->digital_write(true);
        }
        ESP_LOGI(TAG, "OCD unlocked. Entering flasher mode.");
        this->state_.store(STATE_FLASHER, std::memory_order_release);
      } else {
        // Dump mode (SD or UART): upload shellcode and stream flash dump
        this->state_.store(STATE_UPLOADING_SHELLCODE, std::memory_order_release);
        this->upload_and_execute_();
        if (this->rx_pulldown_pin_ != nullptr) {
          this->rx_pulldown_pin_->digital_write(true);
        }
        this->dump_bytes_received_ = 0;
        ESP_LOGI(TAG, "Shellcode uploaded. Entering dump mode (%s).", this->mode_ == MODE_DUMP_SD ? "SD" : "UART");
        this->state_.store(STATE_DUMPING, std::memory_order_release);
      }

      // Task is done — main loop handles the rest
      return;
    }

    // Small delay before next attempt
    esp_rom_delay_us(5000);

    // Brief yield every 50 attempts to feed the watchdog
    if ((attempt % 50) == 49) {
      vTaskDelay(1);
    }
  }

  // Stop was requested
  ESP_LOGI(TAG, "Glitch task stopping (user requested)");
  this->state_.store(STATE_IDLE, std::memory_order_release);

  this->reset_pin_->digital_write(true);
  if (this->glitch_pin_ != nullptr)
    this->glitch_pin_->digital_write(true);
  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(false);
}

// ════════════════════════════════════════════════════════════════
// ProtoA Write mode
// ════════════════════════════════════════════════════════════════

void SysGlitch::write_task_func_(void *param) {
  auto *self = static_cast<SysGlitch *>(param);
  self->run_write_loop_();
  self->glitch_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SysGlitch::run_write_loop_() {
  ESP_LOGI(TAG, "Write task started on core %d", xPortGetCoreID());
  ESP_LOGI(TAG, "Opening file: %s", this->write_path_.c_str());

  // Open the source file using VFS (works with /sdcard/, /usb/, etc.)
  FILE *f = fopen(this->write_path_.c_str(), "rb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Failed to open file: %s", this->write_path_.c_str());
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  // Check file size
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size != SYSCON_FLASH_SIZE) {
    ESP_LOGE(TAG, "File size mismatch: expected %u bytes, got %ld", SYSCON_FLASH_SIZE, file_size);
    fclose(f);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "File size OK: %ld bytes (%u blocks)", file_size, SYSCON_BLOCK_COUNT);

  // Enter ProtoA mode (serial bootloader)
  ESP_LOGI(TAG, "Entering ProtoA mode...");
  if (!this->enter_proto_a_()) {
    ESP_LOGE(TAG, "Failed to enter ProtoA mode!");
    fclose(f);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "ProtoA mode active. Starting flash write...");

  // Write all blocks
  uint8_t block_data[SYSCON_BLOCK_SIZE];
  uint32_t blocks_written = 0;
  uint32_t blocks_failed = 0;

  for (uint16_t block = 0; block < SYSCON_BLOCK_COUNT; block++) {
    // Check for stop request
    if (this->stop_requested_.load(std::memory_order_acquire)) {
      ESP_LOGW(TAG, "Write stopped by user at block %u/%u", block, SYSCON_BLOCK_COUNT);
      break;
    }

    // Read block from file
    size_t bytes_read = fread(block_data, 1, SYSCON_BLOCK_SIZE, f);
    if (bytes_read != SYSCON_BLOCK_SIZE) {
      ESP_LOGE(TAG, "File read error at block %u: got %u bytes", block, (uint32_t) bytes_read);
      fclose(f);
      this->state_.store(STATE_FAILED, std::memory_order_release);
      return;
    }

    uint32_t addr = block * SYSCON_BLOCK_SIZE;

    // Erase block
    if (!this->pa_erase_block_(addr)) {
      ESP_LOGE(TAG, "Erase failed at block %u (addr 0x%05X)", block, addr);
      blocks_failed++;
      continue;
    }

    // Program block (256 bytes at a time, 4 chunks per 1KB block)
    bool program_ok = true;
    for (uint16_t offset = 0; offset < SYSCON_BLOCK_SIZE; offset += 0x100) {
      if (!this->pa_program_block_(addr + offset, block_data + offset, 0x100)) {
        ESP_LOGE(TAG, "Program failed at block %u offset 0x%X", block, offset);
        program_ok = false;
        break;
      }
    }

    if (!program_ok) {
      blocks_failed++;
      continue;
    }

    // Verify block
    if (!this->pa_verify_block_(addr, block_data, SYSCON_BLOCK_SIZE)) {
      ESP_LOGW(TAG, "Verify failed at block %u — data may still be correct", block);
    }

    blocks_written++;

    // Progress log every 16 blocks (~16KB)
    if ((block % 16) == 0 || block == SYSCON_BLOCK_COUNT - 1) {
      ESP_LOGI(TAG, "Write progress: block %u/%u (%u KB / %u KB)", block + 1, SYSCON_BLOCK_COUNT,
               (block + 1) * SYSCON_BLOCK_SIZE / 1024, SYSCON_FLASH_SIZE / 1024);
    }

    // Yield every 8 blocks to feed the watchdog
    if ((block % 8) == 7) {
      vTaskDelay(1);
    }
  }

  fclose(f);

  // Reset the chip
  ESP_LOGI(TAG, "Write complete. Resetting chip...");
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(50000);
  this->reset_pin_->digital_write(true);

  ESP_LOGI(TAG, "Flash write finished: %u blocks written, %u failed", blocks_written, blocks_failed);

  if (blocks_failed > 0) {
    this->state_.store(STATE_FAILED, std::memory_order_release);
  } else {
    this->state_.store(STATE_DONE, std::memory_order_release);
  }
}

#endif  // USE_ESP32

void SysGlitch::start_write() {
  auto current = this->state_.load(std::memory_order_acquire);
  if (current != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start write - not idle (state=%u)", current);
    return;
  }

  ESP_LOGI(TAG, "Starting ProtoA flash write from: %s", this->write_path_.c_str());
  this->stop_requested_.store(false, std::memory_order_release);
  this->state_.store(STATE_WRITING, std::memory_order_release);

#ifdef USE_ESP32
  // Spawn write task on dedicated core (needs enough stack for file I/O + ProtoA frames)
  BaseType_t result = xTaskCreatePinnedToCore(write_task_func_, "syswrite", 8192, this, GLITCH_TASK_PRIORITY,
                                              &this->glitch_task_handle_, GLITCH_TASK_CORE);

  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create write task!");
    this->state_.store(STATE_FAILED, std::memory_order_release);
  }
#else
  ESP_LOGE(TAG, "SysGlitch write requires ESP32 platform");
  this->state_.store(STATE_FAILED, std::memory_order_release);
#endif
}

// ════════════════════════════════════════════════════════════════
// OCD operations
// ════════════════════════════════════════════════════════════════

void SysGlitch::upload_and_execute_() {
  this->tool0_uart_->write_byte(OCD_WRITE_CMD);
  esp_rom_delay_us(1000);

  this->tool0_uart_->write_array(SHELLCODE_DUMP, sizeof(SHELLCODE_DUMP));
  this->tool0_uart_->flush();
  esp_rom_delay_us(5000);

  this->tool0_uart_->write_byte(OCD_EXEC_CMD);
  this->tool0_uart_->flush();
  esp_rom_delay_us(10000);
}

uint8_t SysGlitch::compute_ocd_checksum_(const uint8_t *data, uint8_t len) {
  // OCD checksum: sum all bytes, subtract 1, keep low byte
  uint8_t csum = 0;
  for (uint8_t i = 0; i < len; i++) {
    csum += data[i];
  }
  csum &= 0xFF;
  csum -= 1;
  csum &= 0xFF;
  return csum;
}

uint8_t SysGlitch::compute_passcode_checksum_() { return this->compute_ocd_checksum_(this->passcode_.data(), 10); }

// ════════════════════════════════════════════════════════════════
// ProtoA operations (serial bootloader mode for write/erase)
// ════════════════════════════════════════════════════════════════

bool SysGlitch::enter_proto_a_() {
#ifdef USE_ESP32
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

  ESP_LOGI(TAG, "Entering ProtoA mode...");

  // RL78 ProtoA entry sequence (from rl78flash reference):
  // 1. RESET=LOW, TOOL0=LOW
  // 2. Wait (reset hold time)
  // 3. TOOL0=HIGH (UART reinit — idle HIGH)
  // 4. Wait 1ms
  // 5. RESET=HIGH (chip boots, samples TOOL0=HIGH → serial bootloader mode)
  // 6. Wait 3ms (chip startup)
  // 7. Send mode byte 0x3A

  // Step 1: RESET=LOW
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(1000);

  // Step 2: TOOL0=LOW (delete UART driver, then explicitly drive TX pin LOW via GPIO)
  uart_driver_delete(uart_num);
  gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);
  gpio_reset_pin(tx_gpio);
  gpio_set_direction(tx_gpio, GPIO_MODE_OUTPUT);
  gpio_set_level(tx_gpio, 0);
  ESP_LOGD(TAG, "ProtoA: TX GPIO%d driven LOW (TOOL0=LOW)", this->tool0_tx_gpio_);
  if (this->rx_pulldown_pin_ != nullptr) {
    this->rx_pulldown_pin_->digital_write(false);
  }
  esp_rom_delay_us(40000);  // 40ms reset hold with TOOL0 actively LOW

  // Step 3: TOOL0=HIGH (reinit UART driver — TX idle = HIGH)
  // gpio_reset_pin releases our manual control, then load_settings reconfigures for UART
  idf_uart->load_settings(false);
  esp_rom_delay_us(1000);  // 1ms settle — TOOL0 now HIGH via UART idle
  ESP_LOGD(TAG, "ProtoA: UART reinited (TOOL0=HIGH)");
  if (this->rx_pulldown_pin_ != nullptr) {
    this->rx_pulldown_pin_->digital_write(true);
  }

  // Step 4: Release RESET — chip boots, samples TOOL0=HIGH → ProtoA serial bootloader mode
  this->reset_pin_->digital_write(true);
  esp_rom_delay_us(3000);  // 3ms chip startup
  ESP_LOGD(TAG, "ProtoA: RESET released (chip booting into ProtoA)");

  // Flush any garbage in RX buffer
  while (this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
  }

  // Step 4: Send mode byte for 1-wire ProtoA (raw, unframed)
  this->tool0_uart_->write_byte(MODE_A_1WIRE);
  this->tool0_uart_->flush();

  // Discard loopback echo of mode byte (1-wire sees its own TX)
  esp_rom_delay_us(5000);
  uint8_t echo_count = 0;
  while (this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
    ESP_LOGD(TAG, "ProtoA mode echo: 0x%02X", discard);
    echo_count++;
  }
  ESP_LOGD(TAG, "ProtoA mode echo bytes discarded: %u", echo_count);

  // Step 6: Set baud rate (first framed ProtoA command)
  if (!this->pa_set_baudrate_()) {
    ESP_LOGE(TAG, "ProtoA baud rate set failed");
    return false;
  }

  // Step 7: Send ProtoA RESET command (required before operations)
  uint8_t reset_cmd = PA_RESET;
  if (!this->pa_send_frame_(&reset_cmd, 1)) {
    ESP_LOGE(TAG, "ProtoA reset command failed");
    return false;
  }

  ESP_LOGI(TAG, "ProtoA mode active");
  this->ocd_active_ = false;
  this->proto_a_active_ = true;
  return true;
#else
  return false;
#endif
}

uint8_t SysGlitch::pa_checksum_(const uint8_t *data, uint8_t len) {
  // ProtoA checksum: negate sum of all bytes, keep low byte
  uint8_t csum = 0;
  for (uint8_t i = 0; i < len; i++) {
    csum -= data[i];
  }
  return csum & 0xFF;
}

bool SysGlitch::pa_send_frame_(const uint8_t *data, uint8_t len, bool is_cmd) {
  // Frame format: SOH/STX | LEN | DATA... | CHECKSUM | ETX
  uint8_t header = is_cmd ? SOH : STX;
  uint8_t size_byte = (len == 0x100) ? 0 : len;

  // Build checksum over LEN + DATA
  uint8_t csum_data[257];
  csum_data[0] = size_byte;
  memcpy(csum_data + 1, data, len);
  uint8_t csum = this->pa_checksum_(csum_data, len + 1);

  // Send frame
  this->tool0_uart_->write_byte(header);
  this->tool0_uart_->write_byte(size_byte);
  this->tool0_uart_->write_array(data, len);
  this->tool0_uart_->write_byte(csum);
  this->tool0_uart_->write_byte(ETX);
  this->tool0_uart_->flush();

  // Discard loopback bytes (1-wire mode)
  // Expected: header + len + data[len] + csum + etx = len + 4 bytes
  esp_rom_delay_us(5000);
  uint8_t expected_loopback = 4 + len;
  uint8_t loopback_actual = 0;
  while (loopback_actual < expected_loopback && this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
    loopback_actual++;
  }
  // Drain any extra bytes (in case of timing variance)
  esp_rom_delay_us(2000);
  while (this->tool0_uart_->available()) {
    uint8_t extra;
    this->tool0_uart_->read_byte(&extra);
    ESP_LOGD(TAG, "PA frame extra RX after loopback: 0x%02X", extra);
    loopback_actual++;
  }
  ESP_LOGD(TAG, "PA frame: sent %u bytes, discarded %u loopback (expected %u)", len + 4, loopback_actual,
           expected_loopback);

  // Read response frame
  uint8_t resp_buf[8];
  uint8_t resp_len;
  if (!this->pa_recv_frame_(resp_buf, &resp_len, sizeof(resp_buf))) {
    ESP_LOGW(TAG, "PA frame: no response received");
    return false;
  }

  ESP_LOGD(TAG, "PA frame: response len=%u, status=0x%02X", resp_len, resp_len > 0 ? resp_buf[0] : 0xFF);

  // First byte of response is status
  return resp_len > 0 && resp_buf[0] == ACK;
}

bool SysGlitch::pa_recv_frame_(uint8_t *buf, uint8_t *out_len, uint8_t max_len) {
  // Wait for STX
  uint32_t timeout = 1000;  // ms
  uint32_t start = millis();
  uint8_t byte = 0;

  while (millis() - start < timeout) {
    if (this->tool0_uart_->available()) {
      this->tool0_uart_->read_byte(&byte);
      if (byte == STX) {
        break;
      }
      ESP_LOGD(TAG, "PA recv: skipping non-STX byte 0x%02X", byte);
    }
    delay(1);
  }

  if (byte != STX) {
    ESP_LOGW(TAG, "PA recv: timeout waiting for STX (last byte=0x%02X)", byte);
    return false;
  }

  // Read LEN
  if (!this->read_bytes_timeout_(this->tool0_uart_, &byte, 1, 500)) {
    return false;
  }
  uint8_t len = (byte == 0) ? 0 : byte;
  if (len > max_len) {
    len = max_len;
  }

  // Read DATA + CHECKSUM + ETX
  uint8_t frame_remaining = len + 2;  // data + checksum + etx
  uint8_t frame_buf[260];
  if (!this->read_bytes_timeout_(this->tool0_uart_, frame_buf, frame_remaining, 1000)) {
    return false;
  }

  // Copy data out
  memcpy(buf, frame_buf, len);
  *out_len = len;
  return true;
}

bool SysGlitch::pa_set_baudrate_() {
  // Send baud rate command: 0x9A, baudrate=0x00(115200), voltage=0x32(5.0V)
  // Voltage byte = operating voltage * 10 (e.g. 5.0V = 0x32, 3.3V = 0x21)
  uint8_t cmd[] = {PA_BAUD_SET, 0x00, 0x32};
  return this->pa_send_frame_(cmd, sizeof(cmd));
}

bool SysGlitch::pa_erase_block_(uint32_t addr) {
  // Erase command: PA_ERASE + 3-byte address (little-endian)
  uint8_t cmd[] = {
      PA_ERASE,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
  };
  return this->pa_send_frame_(cmd, sizeof(cmd));
}

bool SysGlitch::pa_program_block_(uint32_t addr, const uint8_t *data, uint16_t len) {
  // Program command: PA_PROGRAM + start_addr(3) + end_addr(3)
  uint32_t end_addr = addr + len - 1;
  uint8_t cmd[] = {
      PA_PROGRAM,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
      static_cast<uint8_t>(end_addr & 0xFF),
      static_cast<uint8_t>((end_addr >> 8) & 0xFF),
      static_cast<uint8_t>((end_addr >> 16) & 0xFF),
  };

  if (!this->pa_send_frame_(cmd, sizeof(cmd))) {
    return false;
  }

  // Send data in 256-byte chunks as data frames
  for (uint16_t offset = 0; offset < len; offset += 0x100) {
    uint16_t chunk = len - offset;
    if (chunk > 0x100)
      chunk = 0x100;

    if (!this->pa_send_frame_(data + offset, chunk, false)) {
      return false;
    }
  }

  // Read final iverify status
  uint8_t resp_buf[8];
  uint8_t resp_len;
  this->pa_recv_frame_(resp_buf, &resp_len, sizeof(resp_buf));

  return true;
}

bool SysGlitch::pa_verify_block_(uint32_t addr, const uint8_t *data, uint16_t len) {
  uint32_t end_addr = addr + len - 1;
  uint8_t cmd[] = {
      PA_VERIFY,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
      static_cast<uint8_t>(end_addr & 0xFF),
      static_cast<uint8_t>((end_addr >> 8) & 0xFF),
      static_cast<uint8_t>((end_addr >> 16) & 0xFF),
  };

  if (!this->pa_send_frame_(cmd, sizeof(cmd))) {
    return false;
  }

  // Send data in 256-byte chunks
  for (uint16_t offset = 0; offset < len; offset += 0x100) {
    uint16_t chunk = len - offset;
    if (chunk > 0x100)
      chunk = 0x100;

    if (!this->pa_send_frame_(data + offset, chunk, false)) {
      return false;
    }
  }

  return true;
}

bool SysGlitch::pa_read_block_(uint32_t block_num, uint8_t *out_data) {
  // ProtoA doesn't have a direct "read" command.
  // We use CHECKSUM to verify, but for reading we need OCD mode.
  // For the scflasher protocol, reads use the OCD dump shellcode approach.
  // This method is a placeholder — actual reads go through OCD.
  ESP_LOGW(TAG, "ProtoA read not directly supported — use OCD read");
  return false;
}

// ════════════════════════════════════════════════════════════════
// Scflasher protocol handler
// ════════════════════════════════════════════════════════════════

void SysGlitch::handle_flasher_protocol_() {
  if (this->pc_uart_ == nullptr) {
    return;
  }

  // Check for incoming command from PC
  if (!this->pc_uart_->available()) {
    return;
  }

  // Read command byte
  uint8_t cmd;
  if (!this->pc_uart_->read_byte(&cmd)) {
    return;
  }

  // Read remaining parameters based on command
  switch (cmd) {
    case SCF_PING1:
      this->scf_handle_ping1_();
      break;

    case SCF_PING2:
      this->scf_handle_ping2_();
      break;

    case SCF_INIT:
      this->scf_handle_init_();
      break;

    case SCF_UNINIT:
      this->scf_handle_uninit_();
      break;

    case SCF_READ_BLOCK: {
      // Params: START_BLOCK[2] + END_BLOCK[2] = 4 bytes
      uint8_t params[4];
      if (this->read_bytes_timeout_(this->pc_uart_, params, 4, 1000)) {
        this->scf_handle_read_block_(params, 4);
      }
      break;
    }

    case SCF_ERASE_BLOCK: {
      // Params: START_BLOCK[2] + END_BLOCK[2] = 4 bytes
      uint8_t params[4];
      if (this->read_bytes_timeout_(this->pc_uart_, params, 4, 1000)) {
        this->scf_handle_erase_block_(params, 4);
      }
      break;
    }

    case SCF_ERASE_CHIP:
      this->scf_handle_erase_chip_();
      break;

    case SCF_WRITE_BLOCK:
    case SCF_WRITE_BLOCK_EX: {
      // Params: BLOCK_NUM[2] + DATA[SYSCON_BLOCK_SIZE] = 2 + 1024 bytes
      uint8_t params[2];
      if (this->read_bytes_timeout_(this->pc_uart_, params, 2, 1000)) {
        // Allocate block buffer on stack (1KB)
        uint8_t block_data[SYSCON_BLOCK_SIZE];
        if (this->read_bytes_timeout_(this->pc_uart_, block_data, SYSCON_BLOCK_SIZE, 5000)) {
          // Pack params + data reference for handler
          uint8_t full_params[2 + SYSCON_BLOCK_SIZE];
          memcpy(full_params, params, 2);
          memcpy(full_params + 2, block_data, SYSCON_BLOCK_SIZE);
          this->scf_handle_write_block_(full_params, 2 + SYSCON_BLOCK_SIZE, cmd == SCF_WRITE_BLOCK_EX);
        } else {
          this->pc_uart_->write_byte(SCF_ERR_CMD_LEN);
        }
      }
      break;
    }

    case SCF_RESET:
      this->scf_handle_reset_();
      break;

    default:
      ESP_LOGW(TAG, "Unknown scflasher command: 0x%02X", cmd);
      this->pc_uart_->write_byte(SCF_ERR_UNKNOWN);
      break;
  }
}

void SysGlitch::scf_handle_ping1_() {
  // Return VERSION_MAJOR
  this->pc_uart_->write_byte(SCF_VERSION_MAJOR);
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_ping2_() {
  // Return VERSION_MINOR[1] + FREEMEM[2]
  this->pc_uart_->write_byte(SCF_VERSION_MINOR);
  // Report ~32KB free (arbitrary, matches typical Teensy)
  this->pc_uart_->write_byte(0x80);  // high byte
  this->pc_uart_->write_byte(0x00);  // low byte
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_init_() {
  // INIT: enter ProtoA mode if not already active
  if (this->proto_a_active_) {
    ESP_LOGI(TAG, "SCF INIT: ProtoA already active");
    this->pc_uart_->write_byte(SCF_STATUS_OK);
  } else {
    ESP_LOGI(TAG, "SCF INIT: entering ProtoA mode...");
    if (this->enter_proto_a_()) {
      this->proto_a_active_ = true;
      ESP_LOGI(TAG, "SCF INIT: ProtoA active");
      this->pc_uart_->write_byte(SCF_STATUS_OK);
    } else {
      ESP_LOGE(TAG, "SCF INIT: failed to enter ProtoA");
      this->pc_uart_->write_byte(SCF_ERR_INIT);
    }
  }
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_uninit_() {
  ESP_LOGI(TAG, "SCF UNINIT");
  this->ocd_active_ = false;
  this->proto_a_active_ = false;
  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_read_block_(const uint8_t *params, uint8_t len) {
  // ProtoA does not support reading flash data — only OCD mode can do that.
  // In ProtoA-based flasher mode, reads are not available.
  // Use dump_sd or dump_uart mode to read flash contents.
  uint16_t start_block = (params[0] << 8) | params[1];
  uint16_t end_block = (params[2] << 8) | params[3];
  ESP_LOGW(TAG, "SCF READ blocks %u-%u: not supported in ProtoA mode (use dump mode to read)", start_block, end_block);
  this->pc_uart_->write_byte(SCF_ERR_READ);
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_write_block_(const uint8_t *params, uint16_t len, bool extended) {
  // Params: BLOCK_NUM[2] + DATA[SYSCON_BLOCK_SIZE]
  uint16_t block_num = (params[0] << 8) | params[1];
  const uint8_t *data = params + 2;

  if (block_num >= SYSCON_BLOCK_COUNT) {
    ESP_LOGW(TAG, "SCF WRITE: invalid block %u", block_num);
    this->pc_uart_->write_byte(SCF_ERR_WRITE);
    this->pc_uart_->flush();
    return;
  }

  uint32_t addr = block_num * SYSCON_BLOCK_SIZE;
  ESP_LOGI(TAG, "SCF WRITE block %u (addr 0x%05X)%s", block_num, addr, extended ? " [EX]" : "");

  // Re-enter ProtoA if session was lost
  if (!this->proto_a_active_) {
    if (!this->enter_proto_a_()) {
      ESP_LOGE(TAG, "SCF WRITE: failed to enter ProtoA");
      this->pc_uart_->write_byte(SCF_ERR_WRITE);
      this->pc_uart_->flush();
      return;
    }
    this->proto_a_active_ = true;
  }

  // Erase the block first
  if (!this->pa_erase_block_(addr)) {
    ESP_LOGE(TAG, "SCF WRITE: erase failed for block %u", block_num);
    this->pc_uart_->write_byte(SCF_ERR_ERASE);
    this->pc_uart_->flush();
    return;
  }

  // Program the block (256 bytes at a time)
  for (uint16_t offset = 0; offset < SYSCON_BLOCK_SIZE; offset += 0x100) {
    if (!this->pa_program_block_(addr + offset, data + offset, 0x100)) {
      ESP_LOGE(TAG, "SCF WRITE: program failed at offset 0x%X", offset);
      this->pc_uart_->write_byte(SCF_ERR_WRITE);
      this->pc_uart_->flush();
      return;
    }
  }

  // Verify
  if (!this->pa_verify_block_(addr, data, SYSCON_BLOCK_SIZE)) {
    ESP_LOGW(TAG, "SCF WRITE: verify failed for block %u", block_num);
  }

  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_erase_block_(const uint8_t *params, uint8_t len) {
  uint16_t start_block = (params[0] << 8) | params[1];
  uint16_t end_block = (params[2] << 8) | params[3];

  if (start_block >= SYSCON_BLOCK_COUNT || end_block >= SYSCON_BLOCK_COUNT || start_block > end_block) {
    ESP_LOGW(TAG, "SCF ERASE: invalid block range %u-%u", start_block, end_block);
    this->pc_uart_->write_byte(SCF_ERR_ERASE);
    this->pc_uart_->flush();
    return;
  }

  ESP_LOGI(TAG, "SCF ERASE blocks %u-%u", start_block, end_block);

  // Re-enter ProtoA if session was lost
  if (!this->proto_a_active_) {
    if (!this->enter_proto_a_()) {
      ESP_LOGE(TAG, "SCF ERASE: failed to enter ProtoA");
      this->pc_uart_->write_byte(SCF_ERR_ERASE);
      this->pc_uart_->flush();
      return;
    }
    this->proto_a_active_ = true;
  }

  for (uint16_t block = start_block; block <= end_block; block++) {
    uint32_t addr = block * SYSCON_BLOCK_SIZE;
    if (!this->pa_erase_block_(addr)) {
      ESP_LOGE(TAG, "SCF ERASE: failed at block %u", block);
      this->pc_uart_->write_byte(SCF_ERR_ERASE);
      this->pc_uart_->flush();
      return;
    }
  }

  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_erase_chip_() {
  ESP_LOGI(TAG, "SCF ERASE CHIP");

  // Re-enter ProtoA if session was lost
  if (!this->proto_a_active_) {
    if (!this->enter_proto_a_()) {
      ESP_LOGE(TAG, "SCF ERASE CHIP: failed to enter ProtoA");
      this->pc_uart_->write_byte(SCF_ERR_ERASE);
      this->pc_uart_->flush();
      return;
    }
    this->proto_a_active_ = true;
  }

  for (uint16_t block = 0; block < SYSCON_BLOCK_COUNT; block++) {
    uint32_t addr = block * SYSCON_BLOCK_SIZE;
    if (!this->pa_erase_block_(addr)) {
      ESP_LOGE(TAG, "SCF ERASE CHIP: failed at block %u", block);
      this->pc_uart_->write_byte(SCF_ERR_ERASE);
      this->pc_uart_->flush();
      return;
    }
  }

  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void SysGlitch::scf_handle_reset_() {
  ESP_LOGI(TAG, "SCF RESET");
  this->ocd_active_ = false;
  this->proto_a_active_ = false;
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(50000);
  this->reset_pin_->digital_write(true);
  // No response expected for RESET
}

// ════════════════════════════════════════════════════════════════
// UART bridge (dump-uart mode)
// ════════════════════════════════════════════════════════════════

void SysGlitch::bridge_uarts_() {
  if (this->pc_uart_ == nullptr) {
    ESP_LOGE(TAG, "UART bridge mode requires pc_uart!");
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  // Bridge TOOL0 → PC
  uint8_t byte;
  int count = 256;
  while (this->tool0_uart_->available() && count-- > 0) {
    if (this->tool0_uart_->read_byte(&byte)) {
      this->pc_uart_->write_byte(byte);
      this->dump_bytes_received_++;
    }
  }

  // Bridge PC → TOOL0
  count = 256;
  while (this->pc_uart_->available() && count-- > 0) {
    if (this->pc_uart_->read_byte(&byte)) {
      this->tool0_uart_->write_byte(byte);
    }
  }

  // Log progress periodically
  if (this->dump_bytes_received_ > 0 && (this->dump_bytes_received_ % (64 * 1024)) < 256) {
    ESP_LOGI(TAG, "UART dump: %u / %u KB", this->dump_bytes_received_ / 1024, SYSCON_FLASH_SIZE / 1024);
  }

  // Check if dump is complete (full flash = 512KB)
  if (this->dump_bytes_received_ >= SYSCON_FLASH_SIZE) {
    ESP_LOGI(TAG, "UART dump complete: %u bytes received", this->dump_bytes_received_);
    this->state_.store(STATE_DONE, std::memory_order_release);
  }
}

// ════════════════════════════════════════════════════════════════
// SD card dump (dump-sd mode)
// ════════════════════════════════════════════════════════════════

void SysGlitch::dump_to_sd_() {
#ifdef USE_STORAGE
  if (storage::global_storage == nullptr) {
    ESP_LOGE(TAG, "Storage component not available!");
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  // On first call, create/truncate the dump file
  if (this->dump_bytes_received_ == 0) {
    ESP_LOGI(TAG, "Starting SD dump to: %s", this->dump_path_.c_str());

    // Find storage device for the dump path
    storage::StorageDevice *device = nullptr;
    std::string mount_point;
    auto all_devices = storage::global_storage->get_all_devices();
    for (auto *dev : all_devices) {
      auto info = dev->get_info();
      std::string dev_mount = info.mount_path;
      if (!dev_mount.empty() && this->dump_path_.find(dev_mount) == 0) {
        if (dev_mount.length() > mount_point.length()) {
          mount_point = dev_mount;
          device = dev;
        }
      }
    }

    if (device == nullptr || !device->is_available()) {
      ESP_LOGE(TAG, "No storage device found for path: %s", this->dump_path_.c_str());
      this->state_.store(STATE_FAILED, std::memory_order_release);
      return;
    }

    // Calculate relative path from mount point
    std::string rel_path = this->dump_path_.substr(mount_point.length());
    if (!rel_path.empty() && rel_path[0] == '/') {
      rel_path = rel_path.substr(1);
    }

    // Ensure parent directory exists
    size_t last_slash = rel_path.rfind('/');
    if (last_slash != std::string::npos) {
      std::string rel_dir = rel_path.substr(0, last_slash);
      if (!device->dir_exists(rel_dir.c_str())) {
        device->create_dir(rel_dir.c_str());
      }
    }

    // Create empty file (truncate if exists)
    device->write_file(rel_path.c_str(), nullptr, 0);
  }

  // Read available data from TOOL0 and append to file
  if (!this->tool0_uart_->available()) {
    return;
  }

  // Read in chunks for efficiency
  uint8_t chunk[256];
  uint16_t chunk_len = 0;
  while (this->tool0_uart_->available() && chunk_len < sizeof(chunk)) {
    if (this->tool0_uart_->read_byte(&chunk[chunk_len])) {
      chunk_len++;
    }
  }

  if (chunk_len > 0) {
    // Find the storage device and relative path (cached after first use would be better,
    // but for 512KB this is fine)
    storage::StorageDevice *device = nullptr;
    std::string mount_point;
    auto all_devices = storage::global_storage->get_all_devices();
    for (auto *dev : all_devices) {
      auto info = dev->get_info();
      std::string dev_mount = info.mount_path;
      if (!dev_mount.empty() && this->dump_path_.find(dev_mount) == 0) {
        if (dev_mount.length() > mount_point.length()) {
          mount_point = dev_mount;
          device = dev;
        }
      }
    }

    if (device != nullptr && device->is_available()) {
      std::string rel_path = this->dump_path_.substr(mount_point.length());
      if (!rel_path.empty() && rel_path[0] == '/') {
        rel_path = rel_path.substr(1);
      }

      if (!device->append_file(rel_path.c_str(), chunk, chunk_len)) {
        ESP_LOGW(TAG, "SD write failed at offset %u", this->dump_bytes_received_);
      }
    }

    this->dump_bytes_received_ += chunk_len;

    // Log progress every 64KB
    if ((this->dump_bytes_received_ % (64 * 1024)) < 256) {
      ESP_LOGI(TAG, "SD dump: %u / %u KB", this->dump_bytes_received_ / 1024, SYSCON_FLASH_SIZE / 1024);
    }
  }

  // Check if dump is complete
  if (this->dump_bytes_received_ >= SYSCON_FLASH_SIZE) {
    ESP_LOGI(TAG, "SD dump complete: %u bytes saved to %s", this->dump_bytes_received_, this->dump_path_.c_str());
    this->state_.store(STATE_DONE, std::memory_order_release);
  }
#else
  ESP_LOGE(TAG, "SD dump requires storage component! Add 'storage:' to your config.");
  this->state_.store(STATE_FAILED, std::memory_order_release);
#endif
}

// ════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════

bool SysGlitch::read_bytes_timeout_(uart::UARTComponent *uart, uint8_t *buf, uint16_t len, uint32_t timeout_ms) {
  uint32_t start = millis();
  uint16_t received = 0;

  while (received < len) {
    if (millis() - start > timeout_ms) {
      return false;
    }
    if (uart->available()) {
      if (uart->read_byte(&buf[received])) {
        received++;
      }
    } else {
      delay(1);
    }
  }
  return true;
}

void IRAM_ATTR SysGlitch::delay_ns_(uint32_t ns) {
#ifdef USE_ESP32
  if (ns == 0) {
    return;
  }
  uint32_t cpu_freq_mhz = esp_rom_get_cpu_ticks_per_us();
  uint32_t cycles_to_wait = (uint64_t) ns * cpu_freq_mhz / 1000;
  if (cycles_to_wait == 0) {
    return;
  }

  uint32_t start = esp_cpu_get_cycle_count();
  while ((esp_cpu_get_cycle_count() - start) < cycles_to_wait) {
    // Tight spin
  }
#endif
}

}  // namespace esphome::sysglitch
