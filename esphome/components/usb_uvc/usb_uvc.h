#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "uvc_types.h"

#include <atomic>
#include <functional>
#include <vector>

namespace esphome::usb_uvc {

// A complete frame, on the main loop. data is valid only during the call.
using frame_cb_t = std::function<void(const uint8_t *data, size_t len, UvcFormat format)>;

// UVC (USB Video Class) camera as a usb_host client: it shares the host with every other client.
//
// Setup runs on the main loop as a chain of control transfers, each continued from its
// completion (no waiting): PROBE SET_CUR -> PROBE GET_CUR -> COMMIT SET_CUR -> isochronous
// stream on the smallest alt-setting that carries the negotiated payload size.
// Frames are assembled on the USB task into one of two PSRAM buffers; a finished frame is
// handed to the main loop, while the other buffer takes the next one.
class UVCClient : public usb_host::USBClient {
 public:
  UVCClient() : usb_host::USBClient(0, 0) { this->set_required_interface_class(USB_CLASS_VIDEO); }

  void loop() override;

  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  void set_format(UvcFormat fmt) { this->req_format_ = fmt; }
  void set_width(uint16_t w) { this->req_width_ = w; }
  void set_height(uint16_t h) { this->req_height_ = h; }
  void set_fps(uint8_t fps) { this->req_fps_ = fps; }
  void set_num_urbs(uint8_t n) { this->num_urbs_ = n; }

  void add_frame_callback(frame_cb_t cb) { this->frame_callbacks_.push_back(std::move(cb)); }

 protected:
  struct AltSetting {
    uint8_t alt;
    uint8_t ep;
    uint16_t mps;
  };

  bool reports_connection_itself() const override { return true; }
  void on_connected() override;
  void on_disconnected() override;
  void on_stream_open(usb_host::IsocStream &stream, bool ok) override;
  // USB task.
  void on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) override;

  bool parse_descriptors_();
  // A probe/commit request; next runs on the main loop with the outcome, unless the device
  // went away meanwhile.
  void probe_request_(uint8_t request, uint8_t selector, std::function<void(bool)> &&next);
  void commit_();
  void start_stream_();
  bool ensure_buffers_(size_t size);
  void finish_setup_(bool ok);

  // Device, from the descriptors.
  uint16_t probe_len_{26};  // probe/commit structure size of the device's UVC version
  uint8_t vs_intf_{0};
  std::vector<AltSetting> alts_{};
  UvcFrameDesc selected_frame_{};
  bool high_speed_{false};

  UvcVsCtrl probe_{};
  // Bumped on every connect and disconnect: a completion from an earlier device is dropped.
  std::atomic<uint32_t> generation_{0};

  usb_host::IsocStream stream_{};

  // Two frame buffers in PSRAM: the USB task fills one while the main loop reads the other.
  uint8_t *buffers_[2]{nullptr, nullptr};
  size_t buffer_size_{0};
  uint8_t fill_{0};         // USB task
  size_t fill_len_{0};      // USB task
  uint8_t frame_id_{2};     // USB task; 2: no frame started
  bool skip_frame_{false};  // USB task
  uint8_t ready_index_{0};
  size_t ready_len_{0};
  std::atomic<bool> ready_{false};  // set by the USB task, cleared by the main loop

  UvcFormat req_format_{UvcFormat::MJPEG};
  uint16_t req_width_{0};  // 0: any
  uint16_t req_height_{0};
  uint8_t req_fps_{0};  // 0: the frame's default interval
  uint8_t num_urbs_{3};

  std::vector<frame_cb_t> frame_callbacks_{};
};

}  // namespace esphome::usb_uvc

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
