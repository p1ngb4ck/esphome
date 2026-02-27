#include "ps_tools.h"

#include <cstring>
#include <cstdio>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_cpu.h>
#include <esp_random.h>
#include <esp_rom_sys.h>
#include "esphome/components/uart/uart_component_esp_idf.h"
#endif

namespace esphome::ps_tools {

static const char *const TAG = "ps_tools";

// FreeRTOS task parameters
static const uint32_t TASK_STACK_SIZE = 8192;
static const UBaseType_t TASK_PRIORITY = 5;  // Above main loop (1), below WiFi (23)

#if defined(CONFIG_FREERTOS_UNICORE) || (portNUM_PROCESSORS == 1)
static const BaseType_t TASK_CORE = 0;
#else
static const BaseType_t TASK_CORE = 1;  // Core 1: ESPHome main loop runs on core 0
#endif

// ════════════════════════════════════════════════════════════════════════════
// Component lifecycle
// ════════════════════════════════════════════════════════════════════════════

void PsTools::setup() {
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);  // RESET HIGH = inactive

  if (this->glitch_pin_ != nullptr) {
    this->glitch_pin_->setup();
    this->glitch_pin_->digital_write(true);  // VDD HIGH = chip powered
    this->isr_glitch_pin_ = this->glitch_pin_->to_isr();
  }
  if (this->rx_pulldown_pin_ != nullptr) {
    this->rx_pulldown_pin_->setup();
    this->rx_pulldown_pin_->digital_write(false);
  }

  ESP_LOGI(TAG, "ps_tools initialized");
  ESP_LOGI(TAG, "  Mode: %s", this->mode_str_());
  if (!this->nor_chip_id_.empty()) {
    ESP_LOGI(TAG, "  NOR chip: %s", this->nor_chip_id_.c_str());
  }
}

void PsTools::dump_config() {
  ESP_LOGCONFIG(TAG, "ps_tools:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->mode_str_());
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  if (this->glitch_pin_ != nullptr) {
    LOG_PIN("  Glitch pin: ", this->glitch_pin_);
    ESP_LOGCONFIG(TAG, "  Glitch delay: %u-%u us", this->glitch_delay_min_us_, this->glitch_delay_max_us_);
    ESP_LOGCONFIG(TAG, "  Glitch width max: %u ns", this->glitch_width_max_ns_);
    ESP_LOGCONFIG(TAG, "  Max attempts: %u (0=infinite)", this->max_attempts_);
  }
  if (this->rx_pulldown_pin_ != nullptr) {
    LOG_PIN("  RX pulldown pin: ", this->rx_pulldown_pin_);
  }
  if (this->mode_ == MODE_GLITCH_DUMP || this->mode_ == MODE_PROTO_A_READ) {
    ESP_LOGCONFIG(TAG, "  Dump path: %s", this->dump_path_.c_str());
  }
  if (this->mode_ == MODE_PROTO_A_WRITE) {
    ESP_LOGCONFIG(TAG, "  Write path: %s (%s)", this->write_path_.c_str(), this->write_is_mot_ ? "mot" : "bin");
  }
  ESP_LOGCONFIG(TAG, "  Voltage: %s", this->voltage_byte_ == 0x32 ? "5.0V" : "3.3V");
  ESP_LOGCONFIG(TAG, "  PC UART: %s", this->pc_uart_ != nullptr ? "yes" : "no");
  if (!this->nor_chip_id_.empty()) {
    ESP_LOGCONFIG(TAG, "  NOR chip: %s", this->nor_chip_id_.c_str());
  }
}

void PsTools::loop() {
  auto state = this->state_.load(std::memory_order_acquire);

  // Flasher mode: always poll pc_uart for incoming commands
  if (this->mode_ == MODE_GLITCH_FLASHER && this->pc_uart_ != nullptr) {
    this->handle_flasher_protocol_();
  }

  switch (state) {
    case STATE_IDLE:
      break;

    case STATE_GLITCHING: {
      // Glitch loop runs on core 1 — log attempt progress here
      uint32_t count = this->attempt_count_.load(std::memory_order_relaxed);
      static uint32_t last_logged = 0;
      if (count - last_logged >= 500) {
        ESP_LOGI(TAG, "Glitch attempt %u...", count);
        last_logged = count;
      }
      break;
    }

    case STATE_UPLOADING_SHELLCODE:
      // Task handles this — nothing to do on main loop
      break;

    case STATE_DUMPING:
      // Dump is received on the core-1 FreeRTOS task via run_dump_on_task_().
      // Nothing to do here — main loop just waits for STATE_DONE/FAILED.
      break;

    case STATE_FLASHER:
      // Handled at top of loop()
      break;

    case STATE_WRITING:
    case STATE_READING:
    case STATE_BLANK_CHECKING: {
      // Task running — log progress
      uint32_t prog = this->progress_bytes_.load(std::memory_order_relaxed);
      static uint32_t last_prog = 0;
      if (prog - last_prog >= 16 * 1024) {
        ESP_LOGI(TAG, "Progress: %u / %u KB", prog / 1024, SYSCON_FLASH_SIZE / 1024);
        last_prog = prog;
      }
      break;
    }

    case STATE_ANALYZING_NOR:
      // NOR analysis task runs on core 1 — nothing to poll here
      break;

    case STATE_PROBING:
      // Probe task runs on core 1 — nothing to poll here
      break;

    case STATE_DONE:
      ESP_LOGI(TAG, "Operation complete.");
      this->state_.store(STATE_IDLE, std::memory_order_release);
      break;

    case STATE_FAILED:
      ESP_LOGW(TAG, "Operation failed (attempts=%u)", this->attempt_count_.load(std::memory_order_relaxed));
      this->state_.store(STATE_IDLE, std::memory_order_release);
      break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Public action entry points
// ════════════════════════════════════════════════════════════════════════════

void PsTools::start_glitch() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start glitch — not idle");
    return;
  }
  if (this->glitch_pin_ == nullptr) {
    ESP_LOGE(TAG, "Cannot start glitch — glitch_pin not configured");
    return;
  }
  ESP_LOGI(TAG, "Starting OCD glitch attack...");
  this->attempt_count_.store(0, std::memory_order_relaxed);
  this->stop_requested_.store(false, std::memory_order_release);
  this->ocd_active_ = false;
  this->reset_pin_->digital_write(true);
  this->glitch_pin_->digital_write(true);
  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(false);
  this->state_.store(STATE_GLITCHING, std::memory_order_release);
  this->spawn_task_("ps_glitch");
}

void PsTools::start_ocd_read() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start ocd read — not idle");
    return;
  }
  ESP_LOGI(TAG, "Starting OCD read...");
  this->attempt_count_.store(0, std::memory_order_relaxed);
  this->stop_requested_.store(false, std::memory_order_release);
  this->ocd_active_ = true;
  this->state_.store(STATE_OCD_READING, std::memory_order_release);
  this->spawn_task_("ps_ocd_read");
}

void PsTools::start_glitch_write() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start glitch_write — not idle");
    return;
  }
  if (this->glitch_pin_ == nullptr) {
    ESP_LOGE(TAG, "Cannot start glitch_write — glitch_pin not configured");
    return;
  }
  if (this->write_path_.empty()) {
    ESP_LOGE(TAG, "Cannot start glitch_write — write_path not configured");
    return;
  }
  ESP_LOGI(TAG, "Starting OCD glitch+write from: %s", this->write_path_.c_str());
  this->attempt_count_.store(0, std::memory_order_relaxed);
  this->stop_requested_.store(false, std::memory_order_release);
  this->ocd_active_ = false;
  this->reset_pin_->digital_write(true);
  this->glitch_pin_->digital_write(true);
  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(false);
  this->state_.store(STATE_GLITCH_WRITING, std::memory_order_release);
  this->spawn_task_("ps_gw");
}

void PsTools::stop() {
  ESP_LOGI(TAG, "Stop requested (state=%u, attempts=%u)", this->state_.load(std::memory_order_relaxed),
           this->attempt_count_.load(std::memory_order_relaxed));
  this->stop_requested_.store(true, std::memory_order_release);
  this->ocd_active_ = false;
  this->proto_a_active_ = false;
}

void PsTools::start_write() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start write — not idle");
    return;
  }
  if (this->write_path_.empty()) {
    ESP_LOGE(TAG, "Cannot start write — write_path not configured");
    return;
  }
  ESP_LOGI(TAG, "Starting ProtoA write from: %s (%s)", this->write_path_.c_str(), this->write_is_mot_ ? "mot" : "bin");
  this->stop_requested_.store(false, std::memory_order_release);
  this->progress_bytes_.store(0, std::memory_order_relaxed);
  this->state_.store(STATE_WRITING, std::memory_order_release);
  this->spawn_task_("ps_write");
}

void PsTools::start_read() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start read — not idle");
    return;
  }
  ESP_LOGI(TAG, "Starting ProtoA read to: %s", this->dump_path_.c_str());
  this->stop_requested_.store(false, std::memory_order_release);
  this->progress_bytes_.store(0, std::memory_order_relaxed);
  this->state_.store(STATE_READING, std::memory_order_release);
  this->spawn_task_("ps_read");
}

void PsTools::start_blank_check() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start blank check — not idle");
    return;
  }
  ESP_LOGI(TAG, "Starting ProtoA blank check...");
  this->stop_requested_.store(false, std::memory_order_release);
  this->blank_check_result_ = false;
  this->state_.store(STATE_BLANK_CHECKING, std::memory_order_release);
  this->spawn_task_("ps_blank");
}

void PsTools::start_analyze_nor() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start NOR analysis — not idle");
    return;
  }
  if (this->nor_flash_ == nullptr && this->nor_dump_path_.empty()) {
    ESP_LOGE(TAG, "Cannot start NOR analysis — neither nor_flash nor nor_dump_path configured");
    return;
  }
  ESP_LOGI(TAG, "Starting PS4 NOR analysis (%s)...",
           this->nor_flash_ != nullptr ? "live hardware" : this->nor_dump_path_.c_str());
  this->stop_requested_.store(false, std::memory_order_release);
  this->state_.store(STATE_ANALYZING_NOR, std::memory_order_release);
  this->spawn_task_("ps_nor");
}

void PsTools::start_probe_syscon() {
  if (this->state_.load(std::memory_order_acquire) != STATE_IDLE) {
    ESP_LOGW(TAG, "Cannot start probe — not idle");
    return;
  }
  ESP_LOGI(TAG, "Starting syscon access probe...");
  this->stop_requested_.store(false, std::memory_order_release);
  this->state_.store(STATE_PROBING, std::memory_order_release);
  this->spawn_task_("ps_probe");
}

// ════════════════════════════════════════════════════════════════════════════
// FreeRTOS task dispatch
// ════════════════════════════════════════════════════════════════════════════

#ifdef USE_ESP32

void PsTools::spawn_task_(const char *name) {
  BaseType_t result =
      xTaskCreatePinnedToCore(task_func_, name, TASK_STACK_SIZE, this, TASK_PRIORITY, &this->task_handle_, TASK_CORE);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create task '%s'!", name);
    this->state_.store(STATE_FAILED, std::memory_order_release);
  }
}

void PsTools::task_func_(void *param) {
  auto *self = static_cast<PsTools *>(param);
  self->run_task_();
  self->task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void PsTools::run_task_() {
  auto state = this->state_.load(std::memory_order_acquire);
  switch (state) {
    case STATE_GLITCH_WRITING:
      this->run_glitch_write_loop_();
      break;
    case STATE_GLITCHING:
      this->run_glitch_loop_();
      break;
    case STATE_WRITING:
      this->run_write_loop_();
      break;
    case STATE_READING:
      this->run_read_loop_();
      break;
    case STATE_BLANK_CHECKING:
      this->run_blank_check_loop_();
      break;
    case STATE_ANALYZING_NOR:
      this->run_analyze_nor_();
      break;
    case STATE_PROBING:
      this->run_probe_syscon_();
      break;
    case STATE_OCD_READING:
      this->run_ocd_read_();
      break;
    default:
      ESP_LOGW(TAG, "Task started in unexpected state %u", state);
      this->state_.store(STATE_FAILED, std::memory_order_release);
      break;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// OCD glitch loop
// ════════════════════════════════════════════════════════════════════════════

void PsTools::run_glitch_loop_() {
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

  ESP_LOGI(TAG, "Glitch task on core %d (UART %d)", xPortGetCoreID(), uart_num);

  while (!this->stop_requested_.load(std::memory_order_acquire)) {
    uint32_t attempt = this->attempt_count_.fetch_add(1, std::memory_order_relaxed);

    if (this->max_attempts_ > 0 && attempt >= this->max_attempts_) {
      ESP_LOGW(TAG, "Max attempts (%u) reached", this->max_attempts_);
      this->state_.store(STATE_FAILED, std::memory_order_release);
      this->reset_pin_->digital_write(true);
      this->glitch_pin_->digital_write(true);
      if (this->rx_pulldown_pin_ != nullptr)
        this->rx_pulldown_pin_->digital_write(false);
      return;
    }

    // ── Step 1: RESET LOW, release UART so TX floats HIGH (UART idle = HIGH)
    // RL78/G13 OCD entry: TOOL0 must be HIGH when RESET is released.
    // Arduino reference: Serial.end() → TX floats high → RESET released → OCD.
    // We match this by: delete driver (TX goes high via internal pull/idle) → RESET low → RESET high.
    this->reset_pin_->digital_write(false);
    esp_rom_delay_us(40);  // ≥40µs reset hold (matches Arduino's digitalWrite+end)

    // ── Step 2: Release UART driver so TX pin idles HIGH
    uart_driver_delete(uart_num);
    gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);
    gpio_reset_pin(tx_gpio);
    // Drive TX HIGH explicitly (TOOL0 HIGH = OCD mode at reset release)
    gpio_set_direction(tx_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(tx_gpio, 1);
    esp_rom_delay_us(5000);  // 5ms (matches Arduino delay(5) after Serial.end())

    // ── Step 3: Release RESET — chip samples TOOL0=HIGH → enters OCD mode
    this->reset_pin_->digital_write(true);
    esp_rom_delay_us(5000);  // 5ms (matches Arduino delay(5) after RESET high)

    // ── Step 4: Restore UART driver (TX re-idles HIGH — no gap on TOOL0)
    idf_uart->load_settings(false);
    esp_rom_delay_us(1000);

    // ── Step 5: Send OCD mode entry byte 0xC5
    this->tool0_uart_->write_byte(MODE_OCD);
    esp_rom_delay_us(200);

    // ── Step 6: OCD baud-set frame
    // Format: SOH(1) | LEN(1) | CMD(1) | data0(1) | data1(1) | checksum(1) | ETX(1)
    // In OCD mode data1 is a baud-rate code, NOT the ProtoA voltage×10 byte.
    // 0x14 = 115200 baud (matches Arduino syscon_reader at Serial.begin(115200)).
    // Checksum = two's-complement of sum(LEN + CMD + data0 + data1)
    //          = ~(0x03 + 0x9A + 0x00 + 0x14) + 1 = ~0xB1 + 1 = 0x4F
    static const uint8_t OCD_BAUD_115200 = 0x14;
    static const uint8_t OCD_BAUD_CSUM = 0x4F;  // pre-computed: ~(0x03+0x9A+0x00+0x14)+1
    const uint8_t baud_tx[] = {PA_SOH, 0x03, PA_CMD_BAUD_SET, 0x00, OCD_BAUD_115200, OCD_BAUD_CSUM, PA_ETX};
    this->tool0_uart_->write_array(baud_tx, sizeof(baud_tx));
    this->tool0_uart_->flush();
    esp_rom_delay_us(5000);  // Give chip time to process baud-set

    // ── Step 7: Read and log baud-set response, then discard before glitch
    {
      bool logged = false;
      for (int i = 0; i < 20 && this->tool0_uart_->available(); i++) {
        uint8_t b;
        this->tool0_uart_->read_byte(&b);
        if (!logged) {
          ESP_LOGD(TAG, "Baud-set response[0]: 0x%02X", b);
          logged = true;
        }
      }
    }

    // ── Step 8: Random glitch delay then VDD pulse
    uint32_t delay_range = this->glitch_delay_max_us_ - this->glitch_delay_min_us_;
    uint32_t random_delay = this->glitch_delay_min_us_;
    if (delay_range > 0)
      random_delay += esp_random() % delay_range;
    esp_rom_delay_us(random_delay);

    uint32_t glitch_width_ns = 0;
    if (this->glitch_width_max_ns_ > 0)
      glitch_width_ns = esp_random() % this->glitch_width_max_ns_;

    this->isr_glitch_pin_.digital_write(false);  // VDD LOW = glitch pulse
    if (glitch_width_ns > 0)
      delay_ns_(glitch_width_ns);
    this->isr_glitch_pin_.digital_write(true);  // VDD HIGH = restore power

    // ── Step 9: Wait for chip to recover, flush any post-glitch bytes, send OCD_CONNECT
    // Arduino reference: after glitch pulse → delay(5) → w(OCD_CONNECT_CMD).
    // The chip may send a partial baud-set response frame after glitch — discard it.
    esp_rom_delay_us(5000);
    while (this->tool0_uart_->available()) {
      uint8_t discard;
      this->tool0_uart_->read_byte(&discard);
    }

    this->tool0_uart_->write_byte(OCD_CONNECT_CMD);
    esp_rom_delay_us(1000);
    this->tool0_uart_->write_array(this->passcode_.data(), 10);
    this->tool0_uart_->write_byte(this->compute_passcode_checksum_());
    this->tool0_uart_->flush();

    // ── Step 10: Check for OCD unlock response (0xF0 = already unlocked, 0xF2 = unlock OK)
    bool got_unlock = false;
    esp_rom_delay_us(10000);
    while (this->tool0_uart_->available()) {
      uint8_t rx;
      if (this->tool0_uart_->read_byte(&rx)) {
        ESP_LOGD(TAG, "OCD connect response: 0x%02X", rx);
        if (rx == OCD_UNLOCK_ALREADY || rx == OCD_UNLOCK_OK) {
          ESP_LOGI(TAG, "OCD response: 0x%02X (%s)", rx, rx == OCD_UNLOCK_ALREADY ? "already unlocked" : "unlock OK");
          got_unlock = true;
          break;
        }
      }
    }

    if (got_unlock) {
      ESP_LOGI(TAG, "*** GLITCH SUCCESS after %u attempts! ***", attempt + 1);
      this->ocd_active_ = true;

      if (this->mode_ == MODE_GLITCH_FLASHER) {
        if (this->rx_pulldown_pin_ != nullptr)
          this->rx_pulldown_pin_->digital_write(true);
        ESP_LOGI(TAG, "Entering scflasher protocol mode.");
        this->state_.store(STATE_FLASHER, std::memory_order_release);
      } else {
        // Dump mode: upload shellcode then release TX so RL78 can drive TOOL0
        this->state_.store(STATE_UPLOADING_SHELLCODE, std::memory_order_release);
        this->upload_and_execute_shellcode_();
        if (this->rx_pulldown_pin_ != nullptr)
          this->rx_pulldown_pin_->digital_write(true);
        this->progress_bytes_.store(0, std::memory_order_relaxed);
        ESP_LOGI(TAG, "Shellcode running. Entering dump mode.");
        this->state_.store(STATE_DUMPING, std::memory_order_release);
        this->run_dump_on_task_(uart_num);
      }
      return;
    }

    if ((attempt % 50) == 49)
      vTaskDelay(1);  // Feed watchdog
  }

  ESP_LOGI(TAG, "Glitch task stopped (user requested)");
  this->reset_pin_->digital_write(true);
  this->glitch_pin_->digital_write(true);
  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(false);
  this->state_.store(STATE_IDLE, std::memory_order_release);
}

void PsTools::run_ocd_read_() {
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

  ESP_LOGI(TAG, "OCD read task on core %d (UART %d)", xPortGetCoreID(), uart_num);

  // ── Step 1: RESET LOW, TX HIGH → chip enters OCD mode at reset release
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(40);

  uart_driver_delete(uart_num);
  gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);
  gpio_reset_pin(tx_gpio);
  gpio_set_direction(tx_gpio, GPIO_MODE_OUTPUT);
  gpio_set_level(tx_gpio, 1);  // TOOL0 HIGH = OCD entry
  esp_rom_delay_us(5000);

  // ── Step 2: Release RESET
  this->reset_pin_->digital_write(true);
  esp_rom_delay_us(5000);

  // ── Step 3: Restore UART driver
  idf_uart->load_settings(false);
  esp_rom_delay_us(1000);

  // ── Step 4: MODE_OCD byte + baud-set frame
  this->tool0_uart_->write_byte(MODE_OCD);
  esp_rom_delay_us(200);

  static const uint8_t OCD_BAUD_115200 = 0x14;
  static const uint8_t OCD_BAUD_CSUM = 0x4F;
  const uint8_t baud_tx[] = {PA_SOH, 0x03, PA_CMD_BAUD_SET, 0x00, OCD_BAUD_115200, OCD_BAUD_CSUM, PA_ETX};
  this->tool0_uart_->write_array(baud_tx, sizeof(baud_tx));
  this->tool0_uart_->flush();
  esp_rom_delay_us(5000);

  // ── Step 5: Log baud-set response, flush it
  {
    uint8_t resp_buf[8];
    int n = 0;
    for (int i = 0; i < 20 && this->tool0_uart_->available(); i++) {
      uint8_t b;
      this->tool0_uart_->read_byte(&b);
      if (n < 8)
        resp_buf[n++] = b;
    }
    if (n > 0) {
      ESP_LOGI(TAG, "Baud-set response (%d bytes): 0x%02X ...", n, resp_buf[0]);
    } else {
      ESP_LOGW(TAG, "No baud-set response — check wiring and baud rate");
    }
  }

  // ── Step 6: OCD_CONNECT + passcode
  this->tool0_uart_->write_byte(OCD_CONNECT_CMD);
  esp_rom_delay_us(500);
  this->tool0_uart_->write_array(this->passcode_.data(), 10);
  this->tool0_uart_->write_byte(this->compute_passcode_checksum_());
  this->tool0_uart_->flush();

  // ── Step 7: Wait for unlock response
  esp_rom_delay_us(10000);
  bool got_unlock = false;
  while (this->tool0_uart_->available()) {
    uint8_t rx;
    if (this->tool0_uart_->read_byte(&rx)) {
      ESP_LOGI(TAG, "OCD connect response: 0x%02X", rx);
      if (rx == OCD_UNLOCK_ALREADY || rx == OCD_UNLOCK_OK) {
        got_unlock = true;
        break;
      }
    }
  }

  if (!got_unlock) {
    ESP_LOGE(TAG, "OCD connect failed — no unlock response. Check passcode and baud rate.");
    this->reset_pin_->digital_write(true);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "OCD connected. Uploading dump shellcode...");
  this->ocd_active_ = true;

  // ── Step 8: Upload shellcode + exec, release TX, receive dump
  this->state_.store(STATE_UPLOADING_SHELLCODE, std::memory_order_release);
  this->upload_and_execute_shellcode_();
  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(true);
  this->progress_bytes_.store(0, std::memory_order_relaxed);
  ESP_LOGI(TAG, "Shellcode running. Entering dump mode.");
  this->state_.store(STATE_DUMPING, std::memory_order_release);
  this->run_dump_on_task_(uart_num);
}

// ════════════════════════════════════════════════════════════════════════════
// OCD glitch + write-agent loop
// ════════════════════════════════════════════════════════════════════════════

void PsTools::run_glitch_write_loop_() {
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

  ESP_LOGI(TAG, "Glitch+write task on core %d (UART %d)", xPortGetCoreID(), uart_num);

  // ── Load write image into PSRAM first ──────────────────────────────────────
  uint32_t image_size = 0;
  uint8_t *image = this->write_is_mot_ ? this->load_mot_to_psram_(&image_size) : this->load_bin_to_psram_(&image_size);

  if (image == nullptr) {
    ESP_LOGE(TAG, "Failed to load write image from: %s", this->write_path_.c_str());
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  if (image_size != SYSCON_FLASH_SIZE) {
    ESP_LOGE(TAG, "Image size mismatch: expected %u, got %u", SYSCON_FLASH_SIZE, image_size);
    heap_caps_free(image);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "Image loaded (%u bytes). Starting glitch loop...", image_size);

  // ── Glitch loop: same as run_glitch_loop_() but on success uploads write-agent ──
  while (!this->stop_requested_.load(std::memory_order_acquire)) {
    uint32_t attempt = this->attempt_count_.fetch_add(1, std::memory_order_relaxed);

    if (this->max_attempts_ > 0 && attempt >= this->max_attempts_) {
      ESP_LOGW(TAG, "Max attempts (%u) reached without glitch success", this->max_attempts_);
      heap_caps_free(image);
      this->state_.store(STATE_FAILED, std::memory_order_release);
      this->reset_pin_->digital_write(true);
      this->glitch_pin_->digital_write(true);
      return;
    }

    // ── Step 1: RESET LOW, TX HIGH (OCD entry: TOOL0=HIGH at reset release)
    this->reset_pin_->digital_write(false);
    esp_rom_delay_us(40);

    uart_driver_delete(uart_num);
    gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);
    gpio_reset_pin(tx_gpio);
    gpio_set_direction(tx_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(tx_gpio, 1);  // TOOL0 HIGH → OCD mode
    esp_rom_delay_us(5000);

    // ── Step 2: Release RESET
    this->reset_pin_->digital_write(true);
    esp_rom_delay_us(5000);

    // ── Step 3: Restore UART driver
    idf_uart->load_settings(false);
    esp_rom_delay_us(1000);

    // ── Step 4: MODE_OCD byte
    this->tool0_uart_->write_byte(MODE_OCD);
    esp_rom_delay_us(200);

    // ── Step 5: OCD baud-set frame (0x14 = 115200 baud, checksum 0x4F)
    {
      static const uint8_t OCD_BAUD_115200 = 0x14;
      static const uint8_t OCD_BAUD_CSUM = 0x4F;
      const uint8_t baud_tx[] = {PA_SOH, 0x03, PA_CMD_BAUD_SET, 0x00, OCD_BAUD_115200, OCD_BAUD_CSUM, PA_ETX};
      this->tool0_uart_->write_array(baud_tx, sizeof(baud_tx));
      this->tool0_uart_->flush();
    }
    esp_rom_delay_us(5000);
    while (this->tool0_uart_->available()) {
      uint8_t discard;
      this->tool0_uart_->read_byte(&discard);
    }

    // ── Step 6: Random glitch delay then VDD pulse
    uint32_t delay_range = this->glitch_delay_max_us_ - this->glitch_delay_min_us_;
    uint32_t random_delay = this->glitch_delay_min_us_;
    if (delay_range > 0)
      random_delay += esp_random() % delay_range;
    esp_rom_delay_us(random_delay);

    uint32_t glitch_width_ns = 0;
    if (this->glitch_width_max_ns_ > 0)
      glitch_width_ns = esp_random() % this->glitch_width_max_ns_;

    this->isr_glitch_pin_.digital_write(false);
    if (glitch_width_ns > 0)
      delay_ns_(glitch_width_ns);
    this->isr_glitch_pin_.digital_write(true);

    // ── Step 7: Wait for chip to recover, flush post-glitch bytes, send OCD_CONNECT
    esp_rom_delay_us(5000);
    while (this->tool0_uart_->available()) {
      uint8_t discard;
      this->tool0_uart_->read_byte(&discard);
    }

    // ── Step 8: OCD_CONNECT + passcode
    this->tool0_uart_->write_byte(OCD_CONNECT_CMD);
    esp_rom_delay_us(1000);
    this->tool0_uart_->write_array(this->passcode_.data(), 10);
    this->tool0_uart_->write_byte(this->compute_passcode_checksum_());
    this->tool0_uart_->flush();

    // ── Step 9: Check for OCD unlock response
    esp_rom_delay_us(10000);
    bool got_unlock = false;
    while (this->tool0_uart_->available()) {
      uint8_t rx;
      if (this->tool0_uart_->read_byte(&rx)) {
        ESP_LOGD(TAG, "OCD connect response: 0x%02X", rx);
        if (rx == OCD_UNLOCK_ALREADY || rx == OCD_UNLOCK_OK) {
          ESP_LOGI(TAG, "OCD response: 0x%02X (%s)", rx, rx == OCD_UNLOCK_ALREADY ? "already unlocked" : "unlock OK");
          got_unlock = true;
          break;
        }
      }
    }

    if (!got_unlock) {
      if ((attempt % 50) == 49)
        vTaskDelay(1);
      continue;
    }

    // ── Glitch succeeded! Upload write-agent shellcode ────────────────────────
    ESP_LOGI(TAG, "*** GLITCH SUCCESS after %u attempts! Uploading write agent...", attempt + 1);
    this->ocd_active_ = true;

    this->upload_write_agent_();

    // ── Write each 1KB block ──────────────────────────────────────────────────
    ESP_LOGI(TAG, "Write agent running. Writing %u blocks...", SYSCON_BLOCK_COUNT);
    uint32_t blocks_written = 0;
    uint32_t blocks_failed = 0;

    for (uint16_t block = 0; block < SYSCON_BLOCK_COUNT; block++) {
      if (this->stop_requested_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Write stopped by user at block %u/%u", block, SYSCON_BLOCK_COUNT);
        break;
      }

      uint32_t addr = (uint32_t) block * SYSCON_BLOCK_SIZE;
      const uint8_t *block_data = image + addr;

      if (this->ocd_write_block_(addr, block_data)) {
        blocks_written++;
        this->progress_bytes_.store(blocks_written * SYSCON_BLOCK_SIZE, std::memory_order_relaxed);
        if ((block % 32) == 0 || block == SYSCON_BLOCK_COUNT - 1)
          ESP_LOGI(TAG, "Block %u/%u written (addr 0x%05X)", block + 1, SYSCON_BLOCK_COUNT, addr);
      } else {
        ESP_LOGE(TAG, "Block %u FAILED (addr 0x%05X)", block, addr);
        blocks_failed++;
      }
    }

    heap_caps_free(image);

    if (blocks_failed == 0) {
      ESP_LOGI(TAG, "Write complete: %u blocks OK", blocks_written);
      this->state_.store(STATE_DONE, std::memory_order_release);
    } else {
      ESP_LOGE(TAG, "Write finished with %u failures / %u total", blocks_failed, SYSCON_BLOCK_COUNT);
      this->state_.store(STATE_FAILED, std::memory_order_release);
    }

    this->ocd_active_ = false;
    this->reset_pin_->digital_write(true);
    return;
  }

  // Stopped before glitch succeeded
  heap_caps_free(image);
  ESP_LOGI(TAG, "Glitch+write task stopped (user requested)");
  this->reset_pin_->digital_write(true);
  this->glitch_pin_->digital_write(true);
  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(false);
  this->state_.store(STATE_IDLE, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════════════
// ProtoA write loop
// ════════════════════════════════════════════════════════════════════════════

void PsTools::run_write_loop_() {
  ESP_LOGI(TAG, "Write task on core %d", xPortGetCoreID());

  // Load the source image into a PSRAM buffer
  uint32_t image_size = 0;
  uint8_t *image = this->write_is_mot_ ? this->load_mot_to_psram_(&image_size) : this->load_bin_to_psram_(&image_size);

  if (image == nullptr) {
    ESP_LOGE(TAG, "Failed to load write image");
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  if (image_size != SYSCON_FLASH_SIZE) {
    ESP_LOGE(TAG, "Image size mismatch: expected %u, got %u", SYSCON_FLASH_SIZE, image_size);
    heap_caps_free(image);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "Image loaded (%u bytes). Entering ProtoA mode...", image_size);

  if (!this->enter_proto_a_()) {
    ESP_LOGE(TAG, "Failed to enter ProtoA mode");
    heap_caps_free(image);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "ProtoA active. Writing %u blocks...", SYSCON_BLOCK_COUNT);

  uint32_t blocks_written = 0;
  uint32_t blocks_failed = 0;

  for (uint16_t block = 0; block < SYSCON_BLOCK_COUNT; block++) {
    if (this->stop_requested_.load(std::memory_order_acquire)) {
      ESP_LOGW(TAG, "Write stopped by user at block %u/%u", block, SYSCON_BLOCK_COUNT);
      break;
    }

    uint32_t addr = block * SYSCON_BLOCK_SIZE;
    const uint8_t *block_data = image + addr;

    if (!this->pa_erase_block_(addr)) {
      ESP_LOGE(TAG, "Erase failed at block %u (addr 0x%05X)", block, addr);
      blocks_failed++;
      continue;
    }

    bool program_ok = true;
    for (uint16_t off = 0; off < SYSCON_BLOCK_SIZE; off += SYSCON_PAGE_SIZE) {
      if (!this->pa_program_block_(addr + off, block_data + off, SYSCON_PAGE_SIZE)) {
        ESP_LOGE(TAG, "Program failed at block %u offset 0x%X", block, off);
        program_ok = false;
        break;
      }
    }

    if (!program_ok) {
      blocks_failed++;
      continue;
    }

    if (!this->pa_verify_block_(addr, block_data, SYSCON_BLOCK_SIZE)) {
      ESP_LOGW(TAG, "Verify mismatch at block %u", block);
    }

    blocks_written++;
    this->progress_bytes_.store((block + 1) * SYSCON_BLOCK_SIZE, std::memory_order_relaxed);

    if ((block % 16) == 0 || block == SYSCON_BLOCK_COUNT - 1) {
      ESP_LOGI(TAG, "Write: block %u/%u (%u KB / %u KB)", block + 1, SYSCON_BLOCK_COUNT,
               (block + 1) * SYSCON_BLOCK_SIZE / 1024, SYSCON_FLASH_SIZE / 1024);
    }

    if ((block % 8) == 7)
      vTaskDelay(1);
  }

  heap_caps_free(image);

  // Reset chip to normal boot
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(50000);
  this->reset_pin_->digital_write(true);

  ESP_LOGI(TAG, "Write done: %u blocks written, %u failed", blocks_written, blocks_failed);
  this->state_.store(blocks_failed > 0 ? STATE_FAILED : STATE_DONE, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════════════
// ProtoA read loop (unprotected chip only)
// ════════════════════════════════════════════════════════════════════════════

void PsTools::run_read_loop_() {
  ESP_LOGI(TAG, "Read task on core %d", xPortGetCoreID());
  ESP_LOGW(TAG, "ProtoA does not expose a read command — use glitch_dump mode for locked chips.");
  // ProtoA provides only BLANK_CHECK and CHECKSUM, not raw read.
  // A direct read requires OCD mode (shellcode dump).
  // This mode is reserved for future use if Renesas adds read commands,
  // or if the chip is confirmed blank and we want to verify.
  this->state_.store(STATE_FAILED, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════════════
// ProtoA blank check loop
// ════════════════════════════════════════════════════════════════════════════

void PsTools::run_blank_check_loop_() {
  ESP_LOGI(TAG, "Blank check task on core %d", xPortGetCoreID());

  if (!this->enter_proto_a_()) {
    ESP_LOGE(TAG, "Failed to enter ProtoA mode for blank check");
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  bool all_blank = true;
  for (uint16_t block = 0; block < SYSCON_BLOCK_COUNT; block++) {
    if (this->stop_requested_.load(std::memory_order_acquire))
      break;

    uint32_t addr = block * SYSCON_BLOCK_SIZE;
    if (!this->pa_blank_check_block_(addr)) {
      ESP_LOGI(TAG, "Block %u (addr 0x%05X) is NOT blank", block, addr);
      all_blank = false;
      break;  // No need to check further once we know it's not blank
    }

    if ((block % 64) == 0) {
      ESP_LOGD(TAG, "Blank check: block %u/%u", block, SYSCON_BLOCK_COUNT);
    }

    if ((block % 32) == 31)
      vTaskDelay(1);
  }

  this->blank_check_result_ = all_blank;
  ESP_LOGI(TAG, "Blank check result: %s", all_blank ? "BLANK (chip is erased)" : "NOT BLANK (chip has data)");

  // Reset chip after ProtoA session
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(50000);
  this->reset_pin_->digital_write(true);

  this->state_.store(STATE_DONE, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════════════
// PS4 NOR flash analysis
// ════════════════════════════════════════════════════════════════════════════

// ── NOR flash layout (from ps4-wee-tools sflash.py) ─────────────────────────
// All offsets into the 32 MB MX25L25635F (or compatible) NOR flash.

// Named byte offsets (SFLASH_AREAS)
static const uint32_t NOR_ACT_SLOT = 0x001000;   // Active boot slot flag
static const uint32_t NOR_BOARD_ID = 0x1C4000;   // Board/mobo ID bytes
static const uint32_t NOR_SN = 0x1C8030;         // Serial number (8 bytes ASCII)
static const uint32_t NOR_SKU = 0x1C8041;        // SKU / product code
static const uint32_t NOR_REGION = 0x1C8047;     // Region code (1 byte)
static const uint32_t NOR_FW_VER = 0x1C906A;     // FW version (2 bytes)
static const uint32_t NOR_UART = 0x1C931F;       // UART enable flag (1 byte)
static const uint32_t NOR_MEMCLK = 0x1C9320;     // GDDR5 clock register (1 byte)
static const uint32_t NOR_EAP_MGC = 0x1C91FC;    // EAP magic bytes (4 bytes)
static const uint32_t NOR_EAP_KEY = 0x1C9200;    // EAP key slot (16 bytes)
static const uint32_t NOR_CORE_SWCH = 0x201000;  // CoreOS slot switch offset

// Partition layout (SFLASH_PARTITIONS) — start, size
static const uint32_t NOR_EAP_KBL_OFF = 0x0C4000;
static const uint32_t NOR_EAP_KBL_SIZE = 0x080000;  // 512 KB
static const uint32_t NOR_WIFI_OFF = 0x144000;
static const uint32_t NOR_WIFI_SIZE = 0x080000;  // 512 KB
static const uint32_t NOR_EMC_IPL_A_OFF = 0x004000;
static const uint32_t NOR_EMC_IPL_A_SIZE = 0x060000;
static const uint32_t NOR_EMC_IPL_B_OFF = 0x064000;
static const uint32_t NOR_EMC_IPL_B_SIZE = 0x060000;
static const uint32_t NOR_NVS1_OFF = 0x1C4000;
static const uint32_t NOR_NVS1_SIZE = 0x006000;
static const uint32_t NOR_NVS2_OFF = 0x1CA000;
static const uint32_t NOR_NVS2_SIZE = 0x006000;

// Boot mode values (BOOT_MODES)
static const uint8_t NOR_BOOT_MODE_DEV = 0xFE;
static const uint8_t NOR_BOOT_MODE_ASSIST = 0xFB;
static const uint8_t NOR_BOOT_MODE_RELEASE = 0xFF;

// ── MD5 helper (software, no mbedtls dependency) ────────────────────────────
// RFC 1321 — minimal implementation for partition fingerprinting

struct NorMD5Ctx {
  uint32_t state[4];
  uint32_t count[2];
  uint8_t buf[64];
};

static void nor_md5_init_(NorMD5Ctx *ctx) {
  ctx->state[0] = 0x67452301u;
  ctx->state[1] = 0xEFCDAB89u;
  ctx->state[2] = 0x98BADCFEu;
  ctx->state[3] = 0x10325476u;
  ctx->count[0] = ctx->count[1] = 0;
}

static const uint32_t MD5_T_[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

static const uint8_t MD5_S_[64] = {
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
    14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

static inline uint32_t md5_rotl_(uint32_t x, uint8_t n) { return (x << n) | (x >> (32u - n)); }

static void nor_md5_transform_(uint32_t state[4], const uint8_t block[64]) {
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t m[16];
  for (int i = 0; i < 16; i++)
    m[i] = (uint32_t) block[i * 4] | ((uint32_t) block[i * 4 + 1] << 8) | ((uint32_t) block[i * 4 + 2] << 16) |
           ((uint32_t) block[i * 4 + 3] << 24);
  for (int i = 0; i < 64; i++) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    f = f + a + MD5_T_[i] + m[g];
    a = d;
    d = c;
    c = b;
    b = b + md5_rotl_(f, MD5_S_[i]);
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

static void nor_md5_update_(NorMD5Ctx *ctx, const uint8_t *data, size_t len) {
  uint32_t idx = (ctx->count[0] >> 3) & 0x3F;
  ctx->count[0] += (uint32_t) (len << 3);
  if (ctx->count[0] < (uint32_t) (len << 3))
    ctx->count[1]++;
  ctx->count[1] += (uint32_t) (len >> 29);
  size_t part = 64 - idx;
  size_t i = 0;
  if (len >= part) {
    memcpy(&ctx->buf[idx], data, part);
    nor_md5_transform_(ctx->state, ctx->buf);
    for (i = part; i + 63 < len; i += 64)
      nor_md5_transform_(ctx->state, data + i);
    idx = 0;
  }
  memcpy(&ctx->buf[idx], data + i, len - i);
}

static void nor_md5_final_(NorMD5Ctx *ctx, uint8_t digest[16]) {
  static const uint8_t padding[64] = {0x80};
  uint8_t bits[8];
  for (int i = 0; i < 4; i++) {
    bits[i] = (ctx->count[0] >> (8 * i)) & 0xFF;
    bits[i + 4] = (ctx->count[1] >> (8 * i)) & 0xFF;
  }
  uint32_t idx = (ctx->count[0] >> 3) & 0x3F;
  uint32_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
  nor_md5_update_(ctx, padding, pad_len);
  nor_md5_update_(ctx, bits, 8);
  for (int i = 0; i < 4; i++) {
    digest[i * 4] = (ctx->state[i]) & 0xFF;
    digest[i * 4 + 1] = (ctx->state[i] >> 8) & 0xFF;
    digest[i * 4 + 2] = (ctx->state[i] >> 16) & 0xFF;
    digest[i * 4 + 3] = (ctx->state[i] >> 24) & 0xFF;
  }
}

// ── Partition MD5 database entries ──────────────────────────────────────────
// Source: ps4-wee-tools data/data.py (EAP_KBL_MD5, TORUS_FW_MD5)
// Format: {md5[16], type_byte, label}

struct NorPartEntry {
  uint8_t md5[16];
  uint8_t type_byte;
  const char *label;
};

// Southbridge types (type_byte from EAP_KBL_MD5)
// Values: 0x01/0x02 = Aeolia A0, 0x0D/0x0E = Aeolia A1/A2,
//         0x20/0x21 = Belize A0/B0, 0x24/0x25 = Baikal B1, 0x2A/0x2B = Belize2 A0
static const char *nor_southbridge_name_(uint8_t type_byte) {
  switch (type_byte) {
    case 0x01:
    case 0x02:
      return "Aeolia A0";
    case 0x0D:
    case 0x0E:
      return "Aeolia A1/A2";
    case 0x20:
    case 0x21:
      return "Belize A0/B0";
    case 0x24:
    case 0x25:
      return "Baikal B1";
    case 0x2A:
    case 0x2B:
      return "Belize2 A0";
    default:
      return "Unknown";
  }
}

// Torus (WiFi) chip types (type_byte from TORUS_FW_MD5)
static const char *nor_torus_name_(uint8_t type_byte) {
  switch (type_byte) {
    case 0x03:
      return "Marvell 88W8797";
    case 0x22:
      return "Marvell 88W8897";
    case 0x30:
      return "MediaTek MT7667BSN";
    default:
      return "Unknown";
  }
}

// PS4 region codes (SFLASH_AREAS / PS_REGIONS)
static const char *nor_region_name_(uint8_t region) {
  switch (region) {
    case 0x01:
      return "JP";
    case 0x02:
      return "US";
    case 0x03:
      return "EU";
    case 0x04:
      return "KR";
    case 0x05:
      return "AU";
    case 0x06:
      return "CN";
    case 0x07:
      return "MX";
    default:
      return "Unknown";
  }
}

// Board/mobo name derivation (from sflash.py getMobo())
// BOARD_ID[0] prefix: 2=CV, 3=SA, 4=HA, 5=NV
// BOARD_ID[2] determines suffix letter (A-Z by index)
static void nor_format_mobo_(const uint8_t *board_id, char *out, size_t out_len) {
  static const char *prefixes[] = {"??", "??", "CV", "SA", "HA", "NV"};
  uint8_t p = board_id[0];
  const char *prefix = (p < 6) ? prefixes[p] : "??";
  char suffix = (board_id[2] < 26) ? ('A' + board_id[2]) : '?';
  snprintf(out, out_len, "%s-%c", prefix, suffix);
}

// ── MD5 of a NOR partition (hardware or file) ────────────────────────────────
// Reads in 4KB chunks, computing MD5 on the fly.
// Returns true on success, digest[16] is filled.
static bool nor_partition_md5_(esphome::binary_storage::BinaryStorage *flash, FILE *fp, uint32_t offset, uint32_t size,
                               uint8_t digest[16]) {
  static const uint32_t CHUNK = 4096;
  auto *chunk_buf = static_cast<uint8_t *>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!chunk_buf) {
    chunk_buf = static_cast<uint8_t *>(malloc(CHUNK));
    if (!chunk_buf)
      return false;
  }

  NorMD5Ctx ctx;
  nor_md5_init_(&ctx);

  bool ok = true;
  for (uint32_t pos = 0; pos < size; pos += CHUNK) {
    uint32_t to_read = (pos + CHUNK <= size) ? CHUNK : (size - pos);
    if (flash) {
      if (!flash->read(offset + pos, chunk_buf, to_read)) {
        ok = false;
        break;
      }
    } else {
      if (fseek(fp, (long) (offset + pos), SEEK_SET) != 0 || fread(chunk_buf, 1, to_read, fp) != to_read) {
        ok = false;
        break;
      }
    }
    nor_md5_update_(&ctx, chunk_buf, to_read);
    vTaskDelay(0);  // yield between chunks
  }

  free(chunk_buf);
  if (ok)
    nor_md5_final_(&ctx, digest);
  return ok;
}

// Format a 16-byte MD5 into a 32-char hex string (+ null terminator).
static void nor_fmt_md5_(const uint8_t *d, char *out) {
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    out[i * 2] = hex[d[i] >> 4];
    out[i * 2 + 1] = hex[d[i] & 0xF];
  }
  out[32] = '\0';
}

// Check if all bytes in a NOR region are a single fill value (0xFF or 0x00).
// Used for NVS validation: NVS is "OK" if it is NOT all-0xFF or all-0x00.
static bool nor_region_all_same_(esphome::binary_storage::BinaryStorage *flash, FILE *fp, uint32_t offset,
                                 uint32_t size, uint8_t fill) {
  static const uint32_t CHUNK = 4096;
  auto *buf = static_cast<uint8_t *>(malloc(CHUNK));
  if (!buf)
    return false;

  bool all_same = true;
  for (uint32_t pos = 0; pos < size && all_same; pos += CHUNK) {
    uint32_t to_read = (pos + CHUNK <= size) ? CHUNK : (size - pos);
    if (flash) {
      if (!flash->read(offset + pos, buf, to_read)) {
        free(buf);
        return false;
      }
    } else {
      if (fseek(fp, (long) (offset + pos), SEEK_SET) != 0 || fread(buf, 1, to_read, fp) != to_read) {
        free(buf);
        return false;
      }
    }
    for (uint32_t i = 0; i < to_read; i++) {
      if (buf[i] != fill) {
        all_same = false;
        break;
      }
    }
    vTaskDelay(0);
  }
  free(buf);
  return all_same;
}

// Read a few bytes from NOR (hardware or file).
static bool nor_read_bytes_(esphome::binary_storage::BinaryStorage *flash, FILE *fp, uint32_t offset, uint8_t *out,
                            size_t len) {
  if (flash)
    return flash->read(offset, out, len);
  return (fseek(fp, (long) offset, SEEK_SET) == 0 && fread(out, 1, len, fp) == len);
}

void PsTools::run_analyze_nor_() {
  ESP_LOGI(TAG, "NOR analysis task on core %d", xPortGetCoreID());

  // ── Open source (hardware or dump file) ──────────────────────────────────
  FILE *fp = nullptr;
  if (this->nor_flash_ == nullptr) {
    fp = fopen(this->nor_dump_path_.c_str(), "rb");
    if (!fp) {
      ESP_LOGE(TAG, "Cannot open NOR dump: %s", this->nor_dump_path_.c_str());
      this->state_.store(STATE_FAILED, std::memory_order_release);
      return;
    }
    ESP_LOGI(TAG, "NOR source: file %s", this->nor_dump_path_.c_str());
  } else {
    ESP_LOGI(TAG, "NOR source: SPI hardware (%s, %u MB)", this->nor_flash_->get_device_name(),
             this->nor_flash_->get_capacity() / (1024 * 1024));
  }

  uint8_t tmp[32];
  char hex_buf[33];

  ESP_LOGI(TAG, "══════════════════════════════════════════════");
  ESP_LOGI(TAG, "  PS4 NOR Flash Analysis");
  ESP_LOGI(TAG, "══════════════════════════════════════════════");

  // ── Serial number ─────────────────────────────────────────────────────────
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_SN, tmp, 8)) {
    char sn[9];
    memcpy(sn, tmp, 8);
    sn[8] = '\0';
    ESP_LOGI(TAG, "  Serial number : %.8s", sn);
  }

  // ── SKU ───────────────────────────────────────────────────────────────────
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_SKU, tmp, 8)) {
    char sku[9];
    memcpy(sku, tmp, 8);
    sku[8] = '\0';
    ESP_LOGI(TAG, "  SKU           : %.8s", sku);
  }

  // ── Region ────────────────────────────────────────────────────────────────
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_REGION, tmp, 1)) {
    ESP_LOGI(TAG, "  Region        : %s (0x%02X)", nor_region_name_(tmp[0]), tmp[0]);
  }

  // ── Firmware version ──────────────────────────────────────────────────────
  // 2 bytes at NOR_FW_VER: byte[1]=major, byte[0]=minor
  // Format: "{:X}.{:02X}".format(byte[1], byte[0]) e.g. 0x07 0x02 → "7.02"
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_FW_VER, tmp, 2)) {
    ESP_LOGI(TAG, "  FW version    : %X.%02X", tmp[1], tmp[0]);
  }

  // ── Mobo (board ID) ───────────────────────────────────────────────────────
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_BOARD_ID, tmp, 4)) {
    char mobo[16];
    nor_format_mobo_(tmp, mobo, sizeof(mobo));
    ESP_LOGI(TAG, "  Mobo          : %s (raw: %02X %02X %02X %02X)", mobo, tmp[0], tmp[1], tmp[2], tmp[3]);
  }

  // ── Active boot slot ──────────────────────────────────────────────────────
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_ACT_SLOT, tmp, 1)) {
    ESP_LOGI(TAG, "  Active slot   : %u", tmp[0]);
  }

  // ── Boot mode ─────────────────────────────────────────────────────────────
  // Boot mode is stored in a dedicated NVS area; the uart byte doubles as
  // an indicator of the boot/service mode toggle on some firmware.
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_UART, tmp, 1)) {
    const char *bm;
    switch (tmp[0]) {
      case NOR_BOOT_MODE_DEV:
        bm = "Development";
        break;
      case NOR_BOOT_MODE_ASSIST:
        bm = "Assist";
        break;
      case NOR_BOOT_MODE_RELEASE:
        bm = "Release";
        break;
      default:
        bm = "Unknown";
        break;
    }
    ESP_LOGI(TAG, "  Boot mode     : %s (0x%02X)", bm, tmp[0]);
    ESP_LOGI(TAG, "  UART enabled  : %s", (tmp[0] == NOR_BOOT_MODE_DEV) ? "YES" : "no");
  }

  // ── GDDR5 memory clock ───────────────────────────────────────────────────
  // MHz = (raw - 0x10) * 25 + 400, valid for raw 0x10–0x50
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_MEMCLK, tmp, 1)) {
    uint8_t raw_clk = tmp[0];
    if (raw_clk >= 0x10 && raw_clk <= 0x50) {
      uint32_t mhz = ((uint32_t) (raw_clk - 0x10) * 25) + 400;
      ESP_LOGI(TAG, "  GDDR5 clock   : %u MHz (reg=0x%02X)", mhz, raw_clk);
    } else {
      ESP_LOGI(TAG, "  GDDR5 clock   : ? (reg=0x%02X)", raw_clk);
    }
  }

  // ── EAP magic / key presence ─────────────────────────────────────────────
  if (nor_read_bytes_(this->nor_flash_, fp, NOR_EAP_MGC, tmp, 4)) {
    bool has_magic = (tmp[0] != 0xFF && tmp[0] != 0x00);
    ESP_LOGI(TAG, "  EAP magic     : %02X %02X %02X %02X (%s)", tmp[0], tmp[1], tmp[2], tmp[3],
             has_magic ? "present" : "blank");
  }

  // ── NVS validation ────────────────────────────────────────────────────────
  // NVS is "OK" if it is NOT all-0xFF and NOT all-0x00
  {
    bool nvs1_ff = nor_region_all_same_(this->nor_flash_, fp, NOR_NVS1_OFF, NOR_NVS1_SIZE, 0xFF);
    bool nvs1_00 = (!nvs1_ff) && nor_region_all_same_(this->nor_flash_, fp, NOR_NVS1_OFF, NOR_NVS1_SIZE, 0x00);
    bool nvs2_ff = nor_region_all_same_(this->nor_flash_, fp, NOR_NVS2_OFF, NOR_NVS2_SIZE, 0xFF);
    bool nvs2_00 = (!nvs2_ff) && nor_region_all_same_(this->nor_flash_, fp, NOR_NVS2_OFF, NOR_NVS2_SIZE, 0x00);
    ESP_LOGI(TAG, "  NVS1 (0x%06X): %s", NOR_NVS1_OFF, (nvs1_ff || nvs1_00) ? "BLANK/ERASED" : "OK");
    ESP_LOGI(TAG, "  NVS2 (0x%06X): %s", NOR_NVS2_OFF, (nvs2_ff || nvs2_00) ? "BLANK/ERASED" : "OK");
  }

  // ── Southbridge identification via EAP KBL MD5 ───────────────────────────
  {
    uint8_t digest[16];
    ESP_LOGI(TAG, "  Computing EAP KBL MD5 (512 KB @ 0x%06X)...", NOR_EAP_KBL_OFF);
    if (nor_partition_md5_(this->nor_flash_, fp, NOR_EAP_KBL_OFF, NOR_EAP_KBL_SIZE, digest)) {
      nor_fmt_md5_(digest, hex_buf);
      ESP_LOGI(TAG, "  EAP KBL MD5   : %s", hex_buf);
      // type_byte is encoded in the MD5 table; the last nibble of the MD5's
      // byte index 6 carries the southbridge type in ps4-wee-tools.
      // Since we don't embed the full table here, log the raw MD5 for user lookup.
      // The southbridge type can be read directly from the first byte of the
      // EAP header at offset 0 within the partition if a known magic is present.
      if (nor_read_bytes_(this->nor_flash_, fp, NOR_EAP_KBL_OFF, tmp, 4)) {
        // Southbridge type byte is at offset 1 within the EAP KBL header in
        // ps4-wee-tools SOUTHBRIDGES table — but without the full MD5 table
        // embedded we report the raw header bytes for user-side lookup.
        ESP_LOGI(TAG, "  SB header[0:4]: %02X %02X %02X %02X  (use MD5 for SB type lookup)", tmp[0], tmp[1], tmp[2],
                 tmp[3]);
        // Attempt type detection from known header patterns
        uint8_t type_candidate = tmp[1];
        const char *sb_name = nor_southbridge_name_(type_candidate);
        if (strcmp(sb_name, "Unknown") != 0) {
          ESP_LOGI(TAG, "  Southbridge   : %s (type=0x%02X)", sb_name, type_candidate);
        } else {
          ESP_LOGI(TAG, "  Southbridge   : %s (raw header byte=0x%02X)", sb_name, type_candidate);
        }
      }
    } else {
      ESP_LOGW(TAG, "  EAP KBL MD5   : READ ERROR");
    }
  }

  // ── Torus (WiFi) identification via WiFi partition MD5 ───────────────────
  {
    uint8_t digest[16];
    ESP_LOGI(TAG, "  Computing WiFi partition MD5 (512 KB @ 0x%06X)...", NOR_WIFI_OFF);
    if (nor_partition_md5_(this->nor_flash_, fp, NOR_WIFI_OFF, NOR_WIFI_SIZE, digest)) {
      nor_fmt_md5_(digest, hex_buf);
      ESP_LOGI(TAG, "  WiFi MD5      : %s", hex_buf);
      // Similarly, detect torus from first header bytes
      if (nor_read_bytes_(this->nor_flash_, fp, NOR_WIFI_OFF, tmp, 4)) {
        uint8_t type_candidate = tmp[1];
        const char *torus_name = nor_torus_name_(type_candidate);
        if (strcmp(torus_name, "Unknown") != 0) {
          ESP_LOGI(TAG, "  WiFi chip     : %s (type=0x%02X)", torus_name, type_candidate);
        } else {
          ESP_LOGI(TAG, "  WiFi chip     : %s (header: %02X %02X %02X %02X)", torus_name, tmp[0], tmp[1], tmp[2],
                   tmp[3]);
        }
      }
    } else {
      ESP_LOGW(TAG, "  WiFi MD5      : READ ERROR");
    }
  }

  ESP_LOGI(TAG, "══════════════════════════════════════════════");
  ESP_LOGI(TAG, "  NOR analysis complete");
  ESP_LOGI(TAG, "══════════════════════════════════════════════");

  if (fp)
    fclose(fp);

  this->state_.store(STATE_DONE, std::memory_order_release);
}

// ════════════════════════════════════════════════════════════════════════════
// Syscon access probe
// ════════════════════════════════════════════════════════════════════════════
//
// Context: chip whose flash state is unknown — e.g. accidentally erased.
//
// When RL78/G13 flash is fully erased (all 0xFF):
//   - The OCD security code area is also 0xFF×10
//   - ProtoA entry DOES NOT use the security code — it's purely timing
//   - OCD connect without glitch MAY succeed if the chip accepts the erased
//     security code (0xFF×10) directly
//   - Glitch may still work if direct OCD fails
//
// Probe sequence (read-only, no writes):
//   PHASE 1 — ProtoA entry: verify chip is alive and ProtoA works
//     1a. enter_proto_a_() → PA_CMD_SILICON_SIG (read chip ID)
//     1b. PA_CMD_BLANK_CHECK on block 0 (is block 0 erased?)
//   PHASE 2 — OCD connect without glitch
//     2a. Passcode: 0xFF×10 (erased flash state)
//     2b. Passcode: :Not:Used: (original Sony passcode)
//   PHASE 3 — OCD connect WITH glitch (single attempt each)
//     3a. Passcode: 0xFF×10
//     3b. Passcode: :Not:Used:
//
// Results logged. State set to STATE_DONE regardless (probe is informational).

// Helper: drain RX FIFO, log every byte received, return count.
// label is a short string shown in the log so we know which step produced the bytes.
static int probe_drain_log_(uart::UARTComponent *uart, const char *label) {
  int n = 0;
  uint8_t rx;
  while (uart->available()) {
    if (uart->read_byte(&rx)) {
      ESP_LOGI(TAG, "    [%s] RX[%d] = 0x%02X", label, n, rx);
      n++;
    }
  }
  if (n == 0)
    ESP_LOGI(TAG, "    [%s] RX = (empty)", label);
  return n;
}

// Helper: try OCD connect (no glitch) with a given passcode.
// Returns the response byte received (0xF0/0xF2/0xF1 = chip response, 0xFF = no response).
//
// Protocol notes:
//   - TOOL0 is a 1-wire UART: TX and RX share the same physical line (diode on TX).
//   - Every byte we send echoes back into our own RX FIFO.
//   - We must drain/discard the echo bytes BEFORE reading genuine chip responses.
//   - Echo counts: MODE_OCD=1, baud-set frame=7, OCD_CONNECT=1, passcode+csum=11 → total 20 bytes TX.
//   - After draining echoes the next byte in RX (if any) is the chip's unlock response.
static uint8_t probe_ocd_no_glitch_(uart::UARTComponent *uart, uart::IDFUARTComponent *idf_uart, uart_port_t uart_num,
                                    gpio_num_t tx_gpio, InternalGPIOPin *reset_pin, const uint8_t *passcode,
                                    uint8_t checksum) {
  // ── Step 1: Assert RESET LOW ≥40µs ──────────────────────────────────────
  reset_pin->digital_write(false);
  esp_rom_delay_us(500);

  // ── Step 2: Delete UART driver, drive TX HIGH explicitly ─────────────────
  // TOOL0 must be HIGH when RESET is released so the chip enters OCD mode.
  uart_driver_delete(uart_num);
  gpio_reset_pin(tx_gpio);
  gpio_set_direction(tx_gpio, GPIO_MODE_OUTPUT);
  gpio_set_level(tx_gpio, 1);
  esp_rom_delay_us(5000);  // 5ms — matches Arduino delay(5) after Serial.end()

  // ── Step 3: Release RESET — chip samples TOOL0 HIGH → enters OCD mode ───
  reset_pin->digital_write(true);
  esp_rom_delay_us(5000);  // 5ms for chip to boot

  // ── Step 4: Restore UART driver ──────────────────────────────────────────
  idf_uart->load_settings(false);
  esp_rom_delay_us(1000);

  // Drain anything that appeared during init (noise / spurious)
  probe_drain_log_(uart, "post-reset");

  // ── Step 5: Send MODE_OCD (0xC5) ─────────────────────────────────────────
  // This byte echoes back to us immediately (1-wire). Drain it.
  uart->write_byte(MODE_OCD);
  uart->flush();
  esp_rom_delay_us(2000);
  probe_drain_log_(uart, "after-MODE_OCD-echo");

  // ── Step 6: Send baud-set frame {01 03 9A 00 14 4F 03} ───────────────────
  // 7 bytes TX → 7 echoed bytes in RX. Drain echo, then read chip's ACK frame.
  static const uint8_t OCD_BAUD_115200 = 0x14;
  static const uint8_t OCD_BAUD_CSUM = 0x4F;
  const uint8_t baud_tx[] = {PA_SOH, 0x03, PA_CMD_BAUD_SET, 0x00, OCD_BAUD_115200, OCD_BAUD_CSUM, PA_ETX};
  uart->write_array(baud_tx, sizeof(baud_tx));
  uart->flush();
  esp_rom_delay_us(5000);  // wait for echo + chip response

  // Drain: first 7 bytes are our own echo, remainder is chip's baud-set response
  {
    int drained = 0;
    uint8_t rx;
    while (uart->available()) {
      if (uart->read_byte(&rx)) {
        if (drained < 7) {
          ESP_LOGI(TAG, "    [baud-set-echo] RX[%d] = 0x%02X (our echo)", drained, rx);
        } else {
          ESP_LOGI(TAG, "    [baud-set-resp] RX[%d] = 0x%02X  *** CHIP RESPONSE ***", drained, rx);
        }
        drained++;
      }
    }
    if (drained == 0)
      ESP_LOGW(TAG, "    [baud-set] RX empty — no echo and no chip response");
    else if (drained <= 7)
      ESP_LOGW(TAG, "    [baud-set] only %d bytes (echo only, no chip response)", drained);
  }

  // ── Step 7: Send OCD_CONNECT (0x91) ──────────────────────────────────────
  // 1 byte TX → 1 echoed byte. Drain echo before passcode.
  uart->write_byte(OCD_CONNECT_CMD);
  uart->flush();
  esp_rom_delay_us(2000);
  probe_drain_log_(uart, "after-OCD_CONNECT-echo");

  // ── Step 8: Send passcode (10 bytes) + checksum (1 byte) = 11 bytes ──────
  // 11 bytes TX → 11 echoed bytes. Chip responds with 1 byte after the last.
  uart->write_array(passcode, 10);
  uart->write_byte(checksum);
  uart->flush();
  esp_rom_delay_us(10000);  // 10ms — give chip time to respond after last byte

  // Drain: first 11 bytes are our own echo, byte 12+ is the chip's unlock response
  uint8_t response = 0xFF;
  {
    int drained = 0;
    uint8_t rx;
    while (uart->available()) {
      if (uart->read_byte(&rx)) {
        if (drained < 11) {
          ESP_LOGI(TAG, "    [passcode-echo] RX[%d] = 0x%02X (our echo)", drained, rx);
        } else {
          ESP_LOGI(TAG, "    [unlock-resp]   RX[%d] = 0x%02X  *** CHIP RESPONSE ***", drained, rx);
          if (rx == OCD_UNLOCK_ALREADY || rx == OCD_UNLOCK_OK || rx == OCD_UNLOCK_LOCKED)
            response = rx;
        }
        drained++;
      }
    }
    if (drained == 0)
      ESP_LOGW(TAG, "    [passcode] RX empty — no echo at all (UART problem?)");
    else if (drained <= 11)
      ESP_LOGW(TAG, "    [passcode] only %d echo bytes, no chip response byte (drained=%d)", drained, drained);
    else
      ESP_LOGI(TAG, "    [passcode] %d total bytes (11 echo + %d chip)", drained, drained - 11);
  }

  // ── Step 9: Reset chip to idle for next attempt ───────────────────────────
  reset_pin->digital_write(false);
  esp_rom_delay_us(500);
  reset_pin->digital_write(true);
  esp_rom_delay_us(5000);

  return response;
}

// Helper: format a passcode as hex string for logging.
static void probe_fmt_passcode_(const uint8_t *pc, char *out) {
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 0; i < 10; i++) {
    out[i * 3] = hex[pc[i] >> 4];
    out[i * 3 + 1] = hex[pc[i] & 0xF];
    out[i * 3 + 2] = (i < 9) ? ' ' : '\0';
  }
  out[29] = '\0';
}

// Compute OCD checksum for an arbitrary 10-byte passcode.
static uint8_t probe_passcode_checksum_(const uint8_t *pc) {
  uint8_t s = 0;
  for (int i = 0; i < 10; i++)
    s += pc[i];
  return (s - 1) & 0xFF;
}

void PsTools::run_probe_syscon_() {
  ESP_LOGI(TAG, "══════════════════════════════════════════════");
  ESP_LOGI(TAG, "  Syscon Access Probe");
  ESP_LOGI(TAG, "  Chip marking: A04-COL2 (RL78/G13 LQFP64)");
  ESP_LOGI(TAG, "══════════════════════════════════════════════");

  // Resolve UART handle and TX GPIO — needed for OCD entry sequence
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());
  gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);

  // Known passcodes to try
  static const uint8_t PC_ERASED[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static const uint8_t PC_NOT_USED[10] = {0x3A, 0x4E, 0x6F, 0x74, 0x3A, 0x55, 0x73, 0x65, 0x64, 0x3A};
  static const uint8_t PC_ZEROES[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  char pc_str[31];
  probe_fmt_passcode_(PC_ERASED, pc_str);
  ESP_LOGI(TAG, "  Passcode A: %s  (erased/0xFF)", pc_str);
  probe_fmt_passcode_(PC_NOT_USED, pc_str);
  ESP_LOGI(TAG, "  Passcode B: %s  (:Not:Used:)", pc_str);
  probe_fmt_passcode_(PC_ZEROES, pc_str);
  ESP_LOGI(TAG, "  Passcode C: %s  (all zeros)", pc_str);

  // ── PHASE 1: ProtoA entry ────────────────────────────────────────────────
  // ProtoA does not check the security code — it's purely RESET+TOOL0 timing.
  // If this fails the chip is physically unresponsive (dead or bad wiring).
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "── PHASE 1: ProtoA entry (no security code involved) ──");

  bool proto_a_ok = this->enter_proto_a_();
  ESP_LOGI(TAG, "  ProtoA entry     : %s", proto_a_ok ? "SUCCESS" : "FAILED");

  if (proto_a_ok) {
    // 1a: Silicon signature — proves the chip is alive and responding
    uint8_t sig_cmd = PA_CMD_SILICON_SIG;
    uint8_t sig_buf[16] = {};
    uint8_t sig_len = 0;
    bool sig_ok = false;

    if (this->pa_send_frame_(&sig_cmd, 1)) {
      sig_ok = this->pa_recv_frame_(sig_buf, &sig_len, sizeof(sig_buf));
    }

    if (sig_ok && sig_len >= 4) {
      ESP_LOGI(TAG, "  Silicon sig      : SUCCESS — %u bytes", sig_len);
      // RL78/G13 silicon sig: device code, code flash size, data flash size, RAM size
      ESP_LOGI(TAG, "  Sig[0..3]        : %02X %02X %02X %02X", sig_buf[0], sig_buf[1], sig_buf[2], sig_buf[3]);
      if (sig_len >= 8)
        ESP_LOGI(TAG, "  Sig[4..7]        : %02X %02X %02X %02X", sig_buf[4], sig_buf[5], sig_buf[6], sig_buf[7]);
    } else {
      ESP_LOGW(TAG, "  Silicon sig      : no response (%u bytes)", sig_len);
    }

    // 1b: Blank check block 0 — tells us if the flash is erased
    bool block0_blank = this->pa_blank_check_block_(0x00000);
    ESP_LOGI(TAG, "  Block 0 blank?   : %s  (addr 0x00000)", block0_blank ? "YES — erased" : "NO — has data");

    // Check a few more blocks to characterise the erase state
    bool block255_blank = this->pa_blank_check_block_(0x3FC00);  // last block
    bool block1_blank = this->pa_blank_check_block_(0x00400);
    ESP_LOGI(TAG, "  Block 1 blank?   : %s  (addr 0x00400)", block1_blank ? "YES" : "NO");
    ESP_LOGI(TAG, "  Block 511 blank? : %s  (addr 0x7FC00)", block255_blank ? "YES" : "NO");

    if (block0_blank && block1_blank && block255_blank) {
      ESP_LOGI(TAG, "  *** Flash appears FULLY ERASED ***");
      ESP_LOGI(TAG, "  *** ProtoA write should work — security code is 0xFF x10 ***");
    } else if (!block0_blank) {
      ESP_LOGI(TAG, "  *** Block 0 has data — may be partial erase or intact flash ***");
    }

    // Reset chip for next phase
    this->reset_pin_->digital_write(false);
    esp_rom_delay_us(50000);
    this->reset_pin_->digital_write(true);
    esp_rom_delay_us(5000);
    this->proto_a_active_ = false;

  } else {
    ESP_LOGW(TAG, "  ProtoA failed — check TOOL0 wiring, RESET line, and tool0_tx_pin");
    ESP_LOGW(TAG, "  Continuing with OCD probe anyway...");
  }

  // ── PHASE 2: OCD connect WITHOUT glitch ─────────────────────────────────
  // After an erase, the security code in flash is 0xFF×10 — the chip may
  // accept this directly without needing a glitch pulse.
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "── PHASE 2: OCD connect (no glitch) ──");

  struct {
    const uint8_t *pc;
    const char *label;
  } passcodes[] = {
      {PC_ERASED, "0xFF*10 (erased)"},
      {PC_NOT_USED, ":Not:Used:"},
      {PC_ZEROES, "0x00*10 (zeros)"},
  };

  bool ocd_no_glitch_ok = false;
  for (auto &entry : passcodes) {
    if (this->stop_requested_.load(std::memory_order_acquire))
      break;

    uint8_t csum = probe_passcode_checksum_(entry.pc);
    uint8_t resp =
        probe_ocd_no_glitch_(this->tool0_uart_, idf_uart, uart_num, tx_gpio, this->reset_pin_, entry.pc, csum);

    const char *resp_str;
    bool ok = false;
    switch (resp) {
      case OCD_UNLOCK_ALREADY:
        resp_str = "0xF0 ALREADY UNLOCKED";
        ok = true;
        break;
      case OCD_UNLOCK_OK:
        resp_str = "0xF2 UNLOCKED";
        ok = true;
        break;
      case OCD_UNLOCK_LOCKED:
        resp_str = "0xF1 LOCKED";
        ok = false;
        break;
      default:
        resp_str = "no response";
        ok = false;
        break;
    }

    ESP_LOGI(TAG, "  No-glitch %-22s -> %s", entry.label, resp_str);
    if (ok) {
      ocd_no_glitch_ok = true;
      ESP_LOGI(TAG, "  *** OCD ACCESS WITHOUT GLITCH! Passcode: %s ***", entry.label);
      ESP_LOGI(TAG, "  *** You can now dump/write via OCD without the glitch circuit ***");
      // Leave chip in OCD mode so user can see it worked — then reset
      this->ocd_active_ = true;
      this->reset_pin_->digital_write(false);
      esp_rom_delay_us(1000);
      this->reset_pin_->digital_write(true);
      this->ocd_active_ = false;
      break;  // No need to try more passcodes
    }
  }

  if (!ocd_no_glitch_ok) {
    ESP_LOGI(TAG, "  No passcode worked without glitch (expected if chip is locked)");
  }

  // ── PHASE 3: OCD connect WITH glitch (single attempt each passcode) ─────
  // Only run if glitch_pin is configured and no-glitch already succeeded.
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "── PHASE 3: OCD connect (with glitch, 1 attempt each) ──");

  if (this->glitch_pin_ == nullptr) {
    ESP_LOGI(TAG, "  Skipped — glitch_pin not configured");
  } else if (ocd_no_glitch_ok) {
    ESP_LOGI(TAG, "  Skipped — OCD already accessible without glitch");
  } else {
    auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
    uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

    for (auto &entry : passcodes) {
      if (this->stop_requested_.load(std::memory_order_acquire))
        break;

      uint8_t csum = probe_passcode_checksum_(entry.pc);

      // ── Single glitch attempt ──
      // Sequence mirrors run_glitch_loop_() for a single try.

      // Power-cycle reset
      this->glitch_pin_->digital_write(false);  // VDD off
      this->reset_pin_->digital_write(false);
      esp_rom_delay_us(200000);  // 200ms off
      this->reset_pin_->digital_write(true);
      esp_rom_delay_us(500);
      this->glitch_pin_->digital_write(true);  // VDD on

      if (this->rx_pulldown_pin_ != nullptr)
        this->rx_pulldown_pin_->digital_write(false);

      // Send OCD mode byte
      while (this->tool0_uart_->available()) {
        uint8_t x;
        this->tool0_uart_->read_byte(&x);
      }
      esp_rom_delay_us(1000);
      this->tool0_uart_->write_byte(MODE_OCD);
      this->tool0_uart_->flush();
      esp_rom_delay_us(2000);
      while (this->tool0_uart_->available()) {
        uint8_t x;
        this->tool0_uart_->read_byte(&x);
      }

      // Random glitch: use middle of the default delay window
      uint32_t glitch_delay_us = (this->glitch_delay_min_us_ + this->glitch_delay_max_us_) / 2;
      esp_rom_delay_us(glitch_delay_us);

      // Single glitch pulse (use configured max width ÷ 2 as a sensible default)
      uint32_t pulse_ns = this->glitch_width_max_ns_ / 2;
      {
        portDISABLE_INTERRUPTS();
        this->isr_glitch_pin_.digital_write(false);  // VDD momentarily off
        PsTools::delay_ns_(pulse_ns);
        this->isr_glitch_pin_.digital_write(true);  // VDD back on
        portENABLE_INTERRUPTS();
      }

      // OCD sync
      esp_rom_delay_us(1000);
      this->tool0_uart_->write_byte(OCD_SYNC);
      this->tool0_uart_->flush();
      esp_rom_delay_us(3000);
      while (this->tool0_uart_->available()) {
        uint8_t x;
        this->tool0_uart_->read_byte(&x);
      }

      // Ping
      this->tool0_uart_->write_byte(OCD_PING_CMD);
      this->tool0_uart_->flush();
      esp_rom_delay_us(5000);
      bool got_ping = false;
      while (this->tool0_uart_->available()) {
        uint8_t rx;
        this->tool0_uart_->read_byte(&rx);
        if (rx == 0x00 || rx == 0x06) {
          got_ping = true;
          break;
        }
      }

      if (!got_ping) {
        ESP_LOGD(TAG, "  Glitch %-22s -> no ping response", entry.label);
        continue;
      }

      // Connect + passcode
      esp_rom_delay_us(15000);
      this->tool0_uart_->write_byte(OCD_CONNECT_CMD);
      esp_rom_delay_us(1000);
      this->tool0_uart_->write_array(entry.pc, 10);
      this->tool0_uart_->write_byte(csum);
      this->tool0_uart_->flush();
      esp_rom_delay_us(5000);

      uint8_t resp = 0xFF;
      while (this->tool0_uart_->available()) {
        uint8_t rx;
        this->tool0_uart_->read_byte(&rx);
        if (rx == OCD_UNLOCK_ALREADY || rx == OCD_UNLOCK_OK || rx == OCD_UNLOCK_LOCKED) {
          resp = rx;
          break;
        }
      }

      const char *resp_str;
      bool ok = false;
      switch (resp) {
        case OCD_UNLOCK_ALREADY:
          resp_str = "0xF0 ALREADY UNLOCKED";
          ok = true;
          break;
        case OCD_UNLOCK_OK:
          resp_str = "0xF2 UNLOCKED";
          ok = true;
          break;
        case OCD_UNLOCK_LOCKED:
          resp_str = "0xF1 LOCKED";
          ok = false;
          break;
        default:
          resp_str = "no response";
          ok = false;
          break;
      }

      ESP_LOGI(TAG, "  Glitch+OCD %-18s -> %s", entry.label, resp_str);
      if (ok) {
        ESP_LOGI(TAG, "  *** GLITCH + OCD SUCCESS! Passcode: %s ***", entry.label);
        this->reset_pin_->digital_write(false);
        esp_rom_delay_us(1000);
        this->reset_pin_->digital_write(true);
        break;
      }
    }
  }

  // ── Summary ──────────────────────────────────────────────────────────────
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "── Probe Summary ──");
  ESP_LOGI(TAG, "  ProtoA entry     : %s", proto_a_ok ? "OK" : "FAILED");
  if (proto_a_ok) {
    ESP_LOGI(TAG, "  → If flash is fully erased: set passcode to [0xFF x10] and");
    ESP_LOGI(TAG, "    use proto_a_write mode to reflash. No glitch needed.");
  }
  if (ocd_no_glitch_ok) {
    ESP_LOGI(TAG, "  OCD (no glitch)  : ACCESSIBLE — set mode to glitch_dump");
    ESP_LOGI(TAG, "    with matching passcode, trigger start_glitch (it will");
    ESP_LOGI(TAG, "    succeed on first attempt without the glitch pulse).");
  }
  ESP_LOGI(TAG, "══════════════════════════════════════════════");

  this->ocd_active_ = false;
  this->proto_a_active_ = false;
  this->reset_pin_->digital_write(true);
  if (this->glitch_pin_ != nullptr)
    this->glitch_pin_->digital_write(true);

  this->state_.store(STATE_DONE, std::memory_order_release);
}

#endif  // USE_ESP32

// ════════════════════════════════════════════════════════════════════════════
// OCD helpers
// ════════════════════════════════════════════════════════════════════════════

void PsTools::upload_and_execute_shellcode_() {
  this->tool0_uart_->write_byte(OCD_WRITE_CMD);
  esp_rom_delay_us(1000);
  this->tool0_uart_->write_array(SHELLCODE_DUMP, sizeof(SHELLCODE_DUMP));
  this->tool0_uart_->flush();
  esp_rom_delay_us(5000);
  this->tool0_uart_->write_byte(OCD_EXEC_CMD);
  this->tool0_uart_->flush();
  esp_rom_delay_us(2000);

  // ── Critical: release TX pin so RL78 can drive TOOL0 to send the dump.
  // Arduino reference: after OCD_EXEC → pinMode(TX, INPUT) → Serial.end().
  // If we keep driving TX HIGH the RL78 can't pull it low to send bytes.
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());
  uart_driver_delete(uart_num);
  gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);
  gpio_reset_pin(tx_gpio);
  gpio_set_direction(tx_gpio, GPIO_MODE_INPUT);  // Release TX — RL78 drives TOOL0
  esp_rom_delay_us(5000);

  // Re-init UART for RX-only (to receive the dump stream)
  idf_uart->load_settings(false);
  esp_rom_delay_us(2000);
}

void PsTools::upload_write_agent_() {
  // OCD_WRITE #1: upload 192-byte write-agent to RL78 RAM 0xFB00
  this->tool0_uart_->write_byte(OCD_WRITE_CMD);
  esp_rom_delay_us(500);
  // SHELLCODE_WRITE_AGENT contains [addr_lo, addr_hi, count, data...]
  this->tool0_uart_->write_array(SHELLCODE_WRITE_AGENT, sizeof(SHELLCODE_WRITE_AGENT));
  this->tool0_uart_->flush();
  esp_rom_delay_us(5000);

  // OCD_WRITE #2: upload 4-byte entry stub to RL78 RAM 0xF07E0
  this->tool0_uart_->write_byte(OCD_WRITE_CMD);
  esp_rom_delay_us(500);
  // SHELLCODE_WRITE_STUB contains [addr_lo, addr_hi, count, data...]
  this->tool0_uart_->write_array(SHELLCODE_WRITE_STUB, sizeof(SHELLCODE_WRITE_STUB));
  this->tool0_uart_->flush();
  esp_rom_delay_us(2000);

  // OCD_EXEC: run code at 0xF07E0 → stub loads HL=0xFB00 → calls write-agent
  this->tool0_uart_->write_byte(OCD_EXEC_CMD);
  this->tool0_uart_->flush();
  esp_rom_delay_us(20000);  // Give agent time to initialise
}

bool PsTools::ocd_write_block_(uint32_t addr, const uint8_t *data) {
  // Write command: 'w' (0x77) + 4-byte LE flash address
  uint8_t cmd[5];
  cmd[0] = 0x77;  // 'w' = write command
  cmd[1] = (uint8_t) (addr & 0xFF);
  cmd[2] = (uint8_t) ((addr >> 8) & 0xFF);
  cmd[3] = (uint8_t) ((addr >> 16) & 0xFF);
  cmd[4] = (uint8_t) ((addr >> 24) & 0xFF);

  // Flush any pending RX bytes before issuing command
  while (this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
  }

  this->tool0_uart_->write_array(cmd, sizeof(cmd));
  this->tool0_uart_->flush();

  // Agent erases the block, then we send 1024 data bytes
  esp_rom_delay_us(30000);  // Wait for erase (RL78 flash erase ~20ms)

  this->tool0_uart_->write_array(data, SYSCON_BLOCK_SIZE);
  this->tool0_uart_->flush();

  // Read 1-byte ACK from agent (timeout 500ms)
  uint8_t ack = 0;
  if (!this->read_bytes_timeout_(this->tool0_uart_, &ack, 1, 500)) {
    ESP_LOGW(TAG, "ocd_write_block_ timeout waiting for ACK at addr 0x%05X", addr);
    return false;
  }

  if (ack != 0x00 && ack != 0xAC && ack != 0x06) {
    // Log unexpected ACK but treat non-zero as possible error indicator
    ESP_LOGW(TAG, "ocd_write_block_ unexpected ACK 0x%02X at addr 0x%05X", ack, addr);
  }

  return true;
}

uint8_t PsTools::compute_ocd_checksum_(const uint8_t *data, uint8_t len) {
  uint8_t csum = 0;
  for (uint8_t i = 0; i < len; i++)
    csum += data[i];
  return (csum - 1) & 0xFF;
}

uint8_t PsTools::compute_passcode_checksum_() { return this->compute_ocd_checksum_(this->passcode_.data(), 10); }

// ════════════════════════════════════════════════════════════════════════════
// ProtoA entry sequence
// ════════════════════════════════════════════════════════════════════════════

bool PsTools::enter_proto_a_() {
#ifdef USE_ESP32
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(this->tool0_uart_);
  uart_port_t uart_num = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());

  // RL78/G13 ProtoA entry (1-wire mode, from rl78flash reference):
  // 1. Assert RESET LOW
  // 2. Drive TOOL0 LOW (delete UART driver, GPIO control)
  // 3. Hold RESET+TOOL0 LOW for reset hold time
  // 4. Drive TOOL0 HIGH (reinit UART driver — idle = HIGH)
  // 5. Release RESET — chip boots, samples TOOL0=HIGH → ProtoA serial bootloader
  // 6. Send mode byte 0x3A (1-wire mode select)
  // 7. Set baud rate
  // 8. Send ProtoA RESET command

  // Step 1+2: RESET LOW, then TOOL0 LOW
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(1000);

  uart_driver_delete(uart_num);
  gpio_num_t tx_gpio = static_cast<gpio_num_t>(this->tool0_tx_gpio_);
  gpio_reset_pin(tx_gpio);
  gpio_set_direction(tx_gpio, GPIO_MODE_OUTPUT);
  gpio_set_level(tx_gpio, 0);

  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(false);

  // Step 3: Hold reset + TOOL0 low (40ms)
  esp_rom_delay_us(40000);

  // Step 4: TOOL0 HIGH (reinit UART → TX idle = HIGH)
  idf_uart->load_settings(false);
  esp_rom_delay_us(1000);

  if (this->rx_pulldown_pin_ != nullptr)
    this->rx_pulldown_pin_->digital_write(true);

  // Step 5: Release RESET — chip boots into ProtoA
  this->reset_pin_->digital_write(true);
  esp_rom_delay_us(3000);

  // Flush any garbage
  while (this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
  }

  // Step 6: Send 1-wire mode byte
  this->tool0_uart_->write_byte(MODE_A_1WIRE);
  this->tool0_uart_->flush();

  // Discard loopback echo of mode byte
  esp_rom_delay_us(5000);
  while (this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
  }

  // Step 7: Set baud rate (first framed command)
  if (!this->pa_set_baudrate_()) {
    ESP_LOGE(TAG, "ProtoA baud rate set failed");
    return false;
  }

  // Step 8: ProtoA RESET command (required to start a session)
  uint8_t reset_cmd = PA_CMD_RESET;
  if (!this->pa_send_frame_(&reset_cmd, 1)) {
    ESP_LOGE(TAG, "ProtoA reset command failed");
    return false;
  }

  ESP_LOGI(TAG, "ProtoA mode active");
  this->proto_a_active_ = true;
  this->ocd_active_ = false;
  return true;
#else
  return false;
#endif
}

// ════════════════════════════════════════════════════════════════════════════
// ProtoA framing
// ════════════════════════════════════════════════════════════════════════════

uint8_t PsTools::pa_checksum_(const uint8_t *data, uint8_t len) {
  uint8_t csum = 0;
  for (uint8_t i = 0; i < len; i++)
    csum -= data[i];
  return csum & 0xFF;
}

bool PsTools::pa_send_frame_(const uint8_t *data, uint8_t len, bool is_cmd) {
  uint8_t header = is_cmd ? PA_SOH : PA_STX;
  uint8_t size_byte = (len == 0x100) ? 0 : static_cast<uint8_t>(len);

  // Checksum covers: size_byte + data bytes
  uint8_t csum_buf[257];
  csum_buf[0] = size_byte;
  memcpy(csum_buf + 1, data, len);
  uint8_t csum = this->pa_checksum_(csum_buf, len + 1);

  this->tool0_uart_->write_byte(header);
  this->tool0_uart_->write_byte(size_byte);
  this->tool0_uart_->write_array(data, len);
  this->tool0_uart_->write_byte(csum);
  this->tool0_uart_->write_byte(PA_ETX);
  this->tool0_uart_->flush();

  // Discard loopback (1-wire: we see our own TX bytes back)
  esp_rom_delay_us(5000);
  uint8_t expected_loopback = 4 + len;
  uint8_t loopback_actual = 0;
  while (loopback_actual < expected_loopback && this->tool0_uart_->available()) {
    uint8_t discard;
    this->tool0_uart_->read_byte(&discard);
    loopback_actual++;
  }
  esp_rom_delay_us(2000);
  while (this->tool0_uart_->available()) {
    uint8_t extra;
    this->tool0_uart_->read_byte(&extra);
    loopback_actual++;
  }

  // Read chip response frame
  uint8_t resp_buf[8];
  uint8_t resp_len;
  if (!this->pa_recv_frame_(resp_buf, &resp_len, sizeof(resp_buf))) {
    ESP_LOGW(TAG, "PA send: no response");
    return false;
  }

  return resp_len > 0 && resp_buf[0] == PA_ACK;
}

bool PsTools::pa_recv_frame_(uint8_t *buf, uint8_t *out_len, uint8_t max_len) {
  uint32_t start = millis();
  uint8_t byte = 0;

  // Wait for STX
  while (millis() - start < 1000) {
    if (this->tool0_uart_->available()) {
      this->tool0_uart_->read_byte(&byte);
      if (byte == PA_STX)
        break;
    }
    delay(1);
  }

  if (byte != PA_STX) {
    ESP_LOGW(TAG, "PA recv: timeout waiting for STX");
    return false;
  }

  // Read LEN
  if (!this->read_bytes_timeout_(this->tool0_uart_, &byte, 1, 500))
    return false;
  uint8_t len = (byte == 0) ? 0 : byte;
  if (len > max_len)
    len = max_len;

  // Read DATA + checksum + ETX
  uint8_t frame_buf[260];
  if (!this->read_bytes_timeout_(this->tool0_uart_, frame_buf, len + 2, 1000))
    return false;

  memcpy(buf, frame_buf, len);
  *out_len = len;
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// ProtoA block operations
// ════════════════════════════════════════════════════════════════════════════

bool PsTools::pa_set_baudrate_() {
  // PA_CMD_BAUD_SET | baud_index=0x00 (115200) | voltage_byte
  uint8_t cmd[] = {PA_CMD_BAUD_SET, 0x00, this->voltage_byte_};
  return this->pa_send_frame_(cmd, sizeof(cmd));
}

bool PsTools::pa_erase_block_(uint32_t addr) {
  uint8_t cmd[] = {
      PA_CMD_ERASE,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
  };
  return this->pa_send_frame_(cmd, sizeof(cmd));
}

bool PsTools::pa_program_block_(uint32_t addr, const uint8_t *data, uint16_t len) {
  uint32_t end_addr = addr + len - 1;
  uint8_t cmd[] = {
      PA_CMD_PROGRAM,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
      static_cast<uint8_t>(end_addr & 0xFF),
      static_cast<uint8_t>((end_addr >> 8) & 0xFF),
      static_cast<uint8_t>((end_addr >> 16) & 0xFF),
  };

  if (!this->pa_send_frame_(cmd, sizeof(cmd)))
    return false;

  // Send data in 256-byte data frames
  for (uint16_t off = 0; off < len; off += 0x100) {
    uint16_t chunk = len - off;
    if (chunk > 0x100)
      chunk = 0x100;
    if (!this->pa_send_frame_(data + off, static_cast<uint8_t>(chunk), false))
      return false;
  }

  // Read final verify status
  uint8_t resp_buf[8];
  uint8_t resp_len;
  this->pa_recv_frame_(resp_buf, &resp_len, sizeof(resp_buf));

  return true;
}

bool PsTools::pa_verify_block_(uint32_t addr, const uint8_t *data, uint16_t len) {
  uint32_t end_addr = addr + len - 1;
  uint8_t cmd[] = {
      PA_CMD_VERIFY,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
      static_cast<uint8_t>(end_addr & 0xFF),
      static_cast<uint8_t>((end_addr >> 8) & 0xFF),
      static_cast<uint8_t>((end_addr >> 16) & 0xFF),
  };

  if (!this->pa_send_frame_(cmd, sizeof(cmd)))
    return false;

  for (uint16_t off = 0; off < len; off += 0x100) {
    uint16_t chunk = len - off;
    if (chunk > 0x100)
      chunk = 0x100;
    if (!this->pa_send_frame_(data + off, static_cast<uint8_t>(chunk), false))
      return false;
  }

  return true;
}

bool PsTools::pa_blank_check_block_(uint32_t addr) {
  uint32_t end_addr = addr + SYSCON_BLOCK_SIZE - 1;
  uint8_t cmd[] = {
      PA_CMD_BLANK_CHECK,
      static_cast<uint8_t>(addr & 0xFF),
      static_cast<uint8_t>((addr >> 8) & 0xFF),
      static_cast<uint8_t>((addr >> 16) & 0xFF),
      static_cast<uint8_t>(end_addr & 0xFF),
      static_cast<uint8_t>((end_addr >> 8) & 0xFF),
      static_cast<uint8_t>((end_addr >> 16) & 0xFF),
  };
  return this->pa_send_frame_(cmd, sizeof(cmd));
}

// ════════════════════════════════════════════════════════════════════════════
// PSRAM file loaders
// ════════════════════════════════════════════════════════════════════════════

uint8_t *PsTools::load_bin_to_psram_(uint32_t *out_size) {
  FILE *f = fopen(this->write_path_.c_str(), "rb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Cannot open: %s", this->write_path_.c_str());
    return nullptr;
  }

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size <= 0) {
    ESP_LOGE(TAG, "Empty or unreadable file: %s", this->write_path_.c_str());
    fclose(f);
    return nullptr;
  }

  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    ESP_LOGE(TAG, "PSRAM alloc failed for %ld bytes", file_size);
    fclose(f);
    return nullptr;
  }

  size_t read = fread(buf, 1, file_size, f);
  fclose(f);

  if (read != static_cast<size_t>(file_size)) {
    ESP_LOGE(TAG, "File read error: expected %ld, got %u", file_size, (uint32_t) read);
    heap_caps_free(buf);
    return nullptr;
  }

  *out_size = static_cast<uint32_t>(file_size);
  ESP_LOGI(TAG, "Loaded .bin: %u bytes from PSRAM", *out_size);
  return buf;
}

uint8_t *PsTools::load_mot_to_psram_(uint32_t *out_size) {
  // Motorola S-record format (.mot):
  //   S0 — header record (ignore)
  //   S2 — 3-byte address data record: S2 LL AAAAAA DD...DD CC
  //   S8 — 3-byte address end record (EOF marker, ignore)
  //   S9, S5 — other record types (ignore)
  //
  // LL = byte count (address + data + checksum)
  // AAAAAA = 24-bit big-endian address
  // DD...DD = data bytes
  // CC = checksum (one's complement of sum of all bytes from LL to last DD)

  FILE *f = fopen(this->write_path_.c_str(), "r");  // text mode for line reading
  if (f == nullptr) {
    ESP_LOGE(TAG, "Cannot open: %s", this->write_path_.c_str());
    return nullptr;
  }

  // Allocate PSRAM buffer pre-filled with 0xFF (erased flash state)
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(SYSCON_FLASH_SIZE, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    ESP_LOGE(TAG, "PSRAM alloc failed for .mot decode (%u bytes)", SYSCON_FLASH_SIZE);
    fclose(f);
    return nullptr;
  }
  memset(buf, 0xFF, SYSCON_FLASH_SIZE);

  uint32_t records_parsed = 0;
  uint32_t bytes_placed = 0;
  char line[600];  // S2 record: max 255 data bytes = ~516 hex chars + prefix

  while (fgets(line, sizeof(line), f) != nullptr) {
    // Must start with 'S'
    if (line[0] != 'S')
      continue;

    char type = line[1];

    // Only process S2 (3-byte address data records) for RL78/G13 512KB
    if (type != '2')
      continue;

    // Parse byte count (2 hex chars at position 2)
    char hex2[3] = {line[2], line[3], 0};
    uint8_t byte_count = static_cast<uint8_t>(strtoul(hex2, nullptr, 16));

    // byte_count = address_bytes(3) + data_bytes + checksum_byte(1)
    // So data_byte_count = byte_count - 3 - 1 = byte_count - 4
    if (byte_count < 4)
      continue;
    uint8_t data_len = byte_count - 4;

    // Parse 24-bit address (6 hex chars at position 4)
    char hex6[7] = {line[4], line[5], line[6], line[7], line[8], line[9], 0};
    uint32_t addr = static_cast<uint32_t>(strtoul(hex6, nullptr, 16));

    if (addr + data_len > SYSCON_FLASH_SIZE) {
      ESP_LOGW(TAG, ".mot: record at 0x%06X + %u exceeds flash size, skipping", addr, data_len);
      continue;
    }

    // Parse data bytes (2 hex chars each, starting at position 10)
    const char *p = line + 10;
    for (uint8_t i = 0; i < data_len; i++) {
      char hb[3] = {p[0], p[1], 0};
      buf[addr + i] = static_cast<uint8_t>(strtoul(hb, nullptr, 16));
      p += 2;
    }

    records_parsed++;
    bytes_placed += data_len;
  }

  fclose(f);

  if (records_parsed == 0) {
    ESP_LOGE(TAG, ".mot: no S2 records found in %s", this->write_path_.c_str());
    heap_caps_free(buf);
    return nullptr;
  }

  *out_size = SYSCON_FLASH_SIZE;
  ESP_LOGI(TAG, "Loaded .mot: %u records, %u bytes placed (gaps filled with 0xFF)", records_parsed, bytes_placed);
  return buf;
}

// ════════════════════════════════════════════════════════════════════════════
// Dump helpers
// ════════════════════════════════════════════════════════════════════════════

#ifdef USE_ESP32
void PsTools::run_dump_on_task_(int uart_num) {
  // Called from the core-1 FreeRTOS task immediately after shellcode is
  // executing.  Reads all 512KB from TOOL0 using uart_read_bytes() (blocking
  // with timeout) so we never stall on the main loop and never overflow the
  // FIFO.  Saves to dump_path_ when complete.

  // Allocate PSRAM receive buffer
  uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(SYSCON_FLASH_SIZE, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    ESP_LOGE(TAG, "PSRAM alloc failed for dump buffer");
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }
  ESP_LOGI(TAG, "Dump buffer allocated (%u bytes). Receiving on core %d...", SYSCON_FLASH_SIZE, xPortGetCoreID());

  // xRingbufferReceiveUpTo() inside uart_read_bytes() re-applies ticks_to_wait
  // on every ring buffer segment boundary — so a large timeout causes per-segment
  // stalls even when data is flowing.  Work around it: block with portMAX_DELAY
  // for 1 byte to wait for data to arrive, then drain the rest with timeout=0
  // (non-blocking) to consume all buffered segments without any per-segment wait.
  uint32_t received = 0;
  uint32_t last_log = 0;
  int idle_count = 0;
  auto port = static_cast<uart_port_t>(uart_num);

  while (received < SYSCON_FLASH_SIZE) {
    // Wait for at least one byte
    int got = uart_read_bytes(port, buf + received, 1, pdMS_TO_TICKS(20));
    if (got <= 0) {
      if (++idle_count >= 250) {
        ESP_LOGE(TAG, "Dump RX stalled at byte %u / %u", received, SYSCON_FLASH_SIZE);
        heap_caps_free(buf);
        this->state_.store(STATE_FAILED, std::memory_order_release);
        return;
      }
      continue;
    }
    received++;
    idle_count = 0;

    // Drain everything else that arrived while we were waiting — no per-segment stall
    uint32_t want = SYSCON_FLASH_SIZE - received;
    if (want > 0) {
      got = uart_read_bytes(port, buf + received, want, 0);
      if (got > 0)
        received += (uint32_t) got;
    }

    this->progress_bytes_.store(received, std::memory_order_relaxed);
    if (received - last_log >= 65536) {
      ESP_LOGI(TAG, "Dump progress: %u / %u bytes (%.1f%%)", received, SYSCON_FLASH_SIZE,
               100.0f * received / SYSCON_FLASH_SIZE);
      last_log = received;
    }
  }

  ESP_LOGI(TAG, "Dump receive complete (%u bytes). Writing to %s...", received, this->dump_path_.c_str());

  FILE *f = fopen(this->dump_path_.c_str(), "wb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "Cannot open dump output: %s", this->dump_path_.c_str());
    heap_caps_free(buf);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  size_t written = fwrite(buf, 1, SYSCON_FLASH_SIZE, f);
  fclose(f);
  heap_caps_free(buf);

  if (written != SYSCON_FLASH_SIZE) {
    ESP_LOGE(TAG, "Dump write error: wrote %u / %u bytes", (uint32_t) written, SYSCON_FLASH_SIZE);
    this->state_.store(STATE_FAILED, std::memory_order_release);
    return;
  }

  ESP_LOGI(TAG, "Dump saved to %s", this->dump_path_.c_str());
  this->state_.store(STATE_DONE, std::memory_order_release);
}
#endif  // USE_ESP32

void PsTools::bridge_uarts_() {
  // Transparent bridge: TOOL0 ↔ pc_uart (for MODE_GLITCH_FLASHER after glitch)
  while (this->tool0_uart_->available()) {
    uint8_t b;
    if (this->tool0_uart_->read_byte(&b) && this->pc_uart_ != nullptr)
      this->pc_uart_->write_byte(b);
  }
  if (this->pc_uart_ != nullptr) {
    while (this->pc_uart_->available()) {
      uint8_t b;
      if (this->pc_uart_->read_byte(&b))
        this->tool0_uart_->write_byte(b);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Scflasher protocol handler (MODE_GLITCH_FLASHER)
// ════════════════════════════════════════════════════════════════════════════

void PsTools::handle_flasher_protocol_() {
  if (this->pc_uart_ == nullptr || !this->pc_uart_->available())
    return;

  uint8_t cmd;
  if (!this->pc_uart_->read_byte(&cmd))
    return;

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
      uint8_t params[4];
      if (this->read_bytes_timeout_(this->pc_uart_, params, 4, 1000))
        this->scf_handle_read_block_(params, 4);
      break;
    }
    case SCF_ERASE_BLOCK: {
      uint8_t params[4];
      if (this->read_bytes_timeout_(this->pc_uart_, params, 4, 1000))
        this->scf_handle_erase_block_(params, 4);
      break;
    }
    case SCF_ERASE_CHIP:
      this->scf_handle_erase_chip_();
      break;
    case SCF_WRITE_BLOCK:
    case SCF_WRITE_BLOCK_EX: {
      uint8_t params[2];
      if (this->read_bytes_timeout_(this->pc_uart_, params, 2, 1000)) {
        uint8_t block_data[SYSCON_BLOCK_SIZE];
        if (this->read_bytes_timeout_(this->pc_uart_, block_data, SYSCON_BLOCK_SIZE, 5000)) {
          uint8_t full_params[2 + SYSCON_BLOCK_SIZE];
          memcpy(full_params, params, 2);
          memcpy(full_params + 2, block_data, SYSCON_BLOCK_SIZE);
          this->scf_handle_write_block_(full_params, sizeof(full_params), cmd == SCF_WRITE_BLOCK_EX);
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
      ESP_LOGW(TAG, "Unknown scflasher cmd: 0x%02X", cmd);
      this->pc_uart_->write_byte(SCF_ERR_UNKNOWN);
      break;
  }
}

void PsTools::scf_handle_ping1_() {
  this->pc_uart_->write_byte(SCF_VERSION_MAJOR);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_ping2_() {
  this->pc_uart_->write_byte(SCF_VERSION_MINOR);
  this->pc_uart_->write_byte(0x80);
  this->pc_uart_->write_byte(0x00);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_init_() {
  if (this->proto_a_active_) {
    this->pc_uart_->write_byte(SCF_STATUS_OK);
  } else {
    if (this->enter_proto_a_()) {
      this->pc_uart_->write_byte(SCF_STATUS_OK);
    } else {
      this->pc_uart_->write_byte(SCF_ERR_INIT);
    }
  }
  this->pc_uart_->flush();
}

void PsTools::scf_handle_uninit_() {
  this->proto_a_active_ = false;
  this->ocd_active_ = false;
  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_read_block_(const uint8_t *params, uint8_t len) {
  uint16_t start_block = (params[0] << 8) | params[1];
  uint16_t end_block = (params[2] << 8) | params[3];
  ESP_LOGW(TAG, "SCF READ blocks %u-%u: not supported in ProtoA mode", start_block, end_block);
  this->pc_uart_->write_byte(SCF_ERR_READ);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_erase_block_(const uint8_t *params, uint8_t len) {
  uint16_t start_block = (params[0] << 8) | params[1];
  uint16_t end_block = (params[2] << 8) | params[3];

  if (!this->proto_a_active_ && !this->enter_proto_a_()) {
    this->pc_uart_->write_byte(SCF_ERR_ERASE);
    this->pc_uart_->flush();
    return;
  }

  for (uint16_t b = start_block; b <= end_block && b < SYSCON_BLOCK_COUNT; b++) {
    uint32_t addr = b * SYSCON_BLOCK_SIZE;
    if (!this->pa_erase_block_(addr)) {
      ESP_LOGE(TAG, "SCF ERASE failed at block %u", b);
      this->pc_uart_->write_byte(SCF_ERR_ERASE);
      this->pc_uart_->flush();
      return;
    }
  }
  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_erase_chip_() {
  ESP_LOGI(TAG, "SCF ERASE CHIP");
  if (!this->proto_a_active_ && !this->enter_proto_a_()) {
    this->pc_uart_->write_byte(SCF_ERR_ERASE);
    this->pc_uart_->flush();
    return;
  }
  for (uint16_t b = 0; b < SYSCON_BLOCK_COUNT; b++) {
    if (!this->pa_erase_block_(b * SYSCON_BLOCK_SIZE)) {
      this->pc_uart_->write_byte(SCF_ERR_ERASE);
      this->pc_uart_->flush();
      return;
    }
  }
  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_write_block_(const uint8_t *params, uint16_t len, bool extended) {
  uint16_t block_num = (params[0] << 8) | params[1];
  const uint8_t *data = params + 2;

  if (block_num >= SYSCON_BLOCK_COUNT) {
    this->pc_uart_->write_byte(SCF_ERR_WRITE);
    this->pc_uart_->flush();
    return;
  }

  uint32_t addr = block_num * SYSCON_BLOCK_SIZE;

  if (!this->proto_a_active_ && !this->enter_proto_a_()) {
    this->pc_uart_->write_byte(SCF_ERR_WRITE);
    this->pc_uart_->flush();
    return;
  }

  if (!this->pa_erase_block_(addr)) {
    this->pc_uart_->write_byte(SCF_ERR_ERASE);
    this->pc_uart_->flush();
    return;
  }

  for (uint16_t off = 0; off < SYSCON_BLOCK_SIZE; off += SYSCON_PAGE_SIZE) {
    if (!this->pa_program_block_(addr + off, data + off, SYSCON_PAGE_SIZE)) {
      this->pc_uart_->write_byte(SCF_ERR_WRITE);
      this->pc_uart_->flush();
      return;
    }
  }

  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

void PsTools::scf_handle_reset_() {
  ESP_LOGI(TAG, "SCF RESET");
  this->proto_a_active_ = false;
  this->ocd_active_ = false;
  this->reset_pin_->digital_write(false);
  esp_rom_delay_us(50000);
  this->reset_pin_->digital_write(true);
  this->pc_uart_->write_byte(SCF_STATUS_OK);
  this->pc_uart_->flush();
}

// ════════════════════════════════════════════════════════════════════════════
// Utilities
// ════════════════════════════════════════════════════════════════════════════

bool PsTools::read_bytes_timeout_(uart::UARTComponent *uart, uint8_t *buf, uint16_t len, uint32_t timeout_ms) {
  uint32_t start = millis();
  uint16_t received = 0;
  while (received < len) {
    if (millis() - start > timeout_ms) {
      ESP_LOGW(TAG, "read_bytes_timeout: timeout after %u/%u bytes", received, len);
      return false;
    }
    if (uart->available()) {
      uint8_t b;
      if (uart->read_byte(&b))
        buf[received++] = b;
    }
  }
  return true;
}

void IRAM_ATTR PsTools::delay_ns_(uint32_t ns) {
#ifdef USE_ESP32
  uint32_t cycles = (uint64_t) ns * esp_rom_get_cpu_ticks_per_us() / 1000;
  uint32_t start = esp_cpu_get_cycle_count();
  while ((esp_cpu_get_cycle_count() - start) < cycles) {
    // busy wait
  }
#endif
}

const char *PsTools::mode_str_() const {
  switch (this->mode_) {
    case MODE_GLITCH_DUMP:
      return "glitch_dump";
    case MODE_GLITCH_FLASHER:
      return "glitch_flasher";
    case MODE_GLITCH_WRITE:
      return "glitch_write";
    case MODE_PROTO_A_WRITE:
      return "proto_a_write";
    case MODE_PROTO_A_READ:
      return "proto_a_read";
    case MODE_PROTO_A_BLANK_CHECK:
      return "proto_a_blank_check";
    default:
      return "unknown";
  }
}

}  // namespace esphome::ps_tools
