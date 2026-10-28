#include "c6_offload.h"

#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD

#include "usb_bluetooth.h"
#include "esphome/core/log.h"
#include "esphome/core/static_task.h"
#include "esp_hosted_misc.h"
#include "esp_heap_caps.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cinttypes>
#include <cstring>

namespace esphome::usb_bluetooth::c6_offload {

static const char *const TAG = "usb_bt_c6";

static constexpr uint32_t MSG_START = 0x42545001u;
static constexpr uint32_t MSG_H4_UP = 0x42545002u;
static constexpr uint32_t MSG_H4_DOWN = 0x42545003u;
static constexpr uint32_t MSG_FLOW = 0x42545004u;

static constexpr size_t QUEUE_DEPTH = 32;
static constexpr uint32_t SENDER_STACK = 3072;
static constexpr UBaseType_t SENDER_PRIORITY = 5;

struct Packet {
  uint32_t msg_id;
  uint16_t len;
  uint8_t data[];
};

static USBBluetooth *s_dongle = nullptr;
static QueueHandle_t s_queue = nullptr;
static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[QUEUE_DEPTH * sizeof(Packet *)];
static StaticTask s_sender;
static uint32_t s_dropped = 0;

static void sender_task(void *) {
  Packet *packet = nullptr;
  for (;;) {
    if (xQueueReceive(s_queue, &packet, portMAX_DELAY) != pdTRUE || packet == nullptr)
      continue;
    const esp_err_t err = esp_hosted_send_custom_data(packet->msg_id, packet->data, packet->len);
    if (err != ESP_OK)
      ESP_LOGW(TAG, "Message 0x%08" PRIX32 " of %u bytes not sent: %s", packet->msg_id, packet->len,
               esp_err_to_name(err));
    heap_caps_free(packet);
  }
}

static bool post(uint32_t msg_id, const uint8_t *data, size_t len) {
  if (s_queue == nullptr || len == 0 || len > UINT16_MAX)
    return false;
  auto *packet = static_cast<Packet *>(heap_caps_malloc(sizeof(Packet) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (packet == nullptr)
    return false;
  packet->msg_id = msg_id;
  packet->len = static_cast<uint16_t>(len);
  std::memcpy(packet->data, data, len);
  if (xQueueSend(s_queue, &packet, 0) != pdTRUE) {
    heap_caps_free(packet);
    return false;
  }
  return true;
}

static void on_down(uint32_t, const uint8_t *data, size_t len, void *) {
  USBBluetooth *dongle = s_dongle;
  if (dongle == nullptr || data == nullptr || len < 2 || len > UINT16_MAX)
    return;
  if (!dongle->hci_send_available())
    send_available(false);
  dongle->hci_send(const_cast<uint8_t *>(data), static_cast<uint16_t>(len));
}

bool start(USBBluetooth *dongle) {
  if (!s_sender.is_created()) {
    if (s_queue == nullptr)
      s_queue = xQueueCreateStatic(QUEUE_DEPTH, sizeof(Packet *), s_queue_storage, &s_queue_control);
    if (!s_sender.create(sender_task, "usb_bt_c6", SENDER_STACK, nullptr, SENDER_PRIORITY, false)) {
      ESP_LOGE(TAG, "No task for the C6 tunnel");
      return false;
    }
    const esp_err_t err = esp_hosted_register_custom_callback(MSG_H4_DOWN, on_down, nullptr);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "C6 tunnel handler not registered: %s", esp_err_to_name(err));
      return false;
    }
  }
  s_dongle = dongle;
  const uint8_t start = 1;
  if (!post(MSG_START, &start, sizeof(start))) {
    ESP_LOGE(TAG, "C6 not told to start its Bluetooth host");
    return false;
  }
  ESP_LOGI(TAG, "USB dongle handed to the C6's Bluetooth host");
  return true;
}

void up(const uint8_t *h4, size_t len) {
  if (!post(MSG_H4_UP, h4, len))
    s_dropped++;
}

void send_available(bool available) {
  const uint8_t value = available ? 1 : 0;
  post(MSG_FLOW, &value, sizeof(value));
}

}

#endif
