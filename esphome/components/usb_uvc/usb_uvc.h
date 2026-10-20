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

// A complete frame of the camera at bus address `camera`, on the main loop. data is valid only during the call.
using frame_cb_t = std::function<void(uint8_t camera, const uint8_t *data, size_t len, UvcFormat format)>;

// UVC (USB Video Class) cameras as one usb_host client serving any number of them at once: it shares the
// host with every other client. Nothing of a camera exists before it is plugged in: its state, stream and
// frame buffers are made when it connects and freed once it is gone and its stream has drained.
//
// Setup runs on the main loop as a chain of control transfers, each continued from its
// completion (no waiting): PROBE SET_CUR -> PROBE GET_CUR -> COMMIT SET_CUR -> isochronous
// stream on the smallest alt-setting that carries the negotiated payload size.
// Frames are assembled on the USB task into one of two PSRAM buffers; a finished frame is
// handed to the main loop, while the other buffer takes the next one.
class UVCClient : public usb_host::USBClient {
 public:
  UVCClient() : usb_host::USBClient(0, 0) { this->add_class_match(USB_CLASS_VIDEO); }

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

  // One plugged camera (its UsbDevice's context): made in on_device_connected(), freed in
  // free_device_context() once its stream is closed (device_busy_()).
  struct Camera {
    usb_host::UsbDevice *dev{nullptr};
    uint32_t id{0};  // continuations of an earlier camera find it gone

    // Device, from the descriptors.
    uint16_t probe_len{26};  // probe/commit structure size of the device's UVC version
    uint8_t vs_intf{0};
    std::vector<AltSetting> alts{};
    UvcFrameDesc selected_frame{};
    bool high_speed{false};

    UvcVsCtrl probe{};
    usb_host::IsocStream stream{};

    // Two frame buffers in PSRAM: the USB task fills one while the main loop reads the other.
    uint8_t *buffers[2]{nullptr, nullptr};
    size_t buffer_size{0};
    uint8_t fill{0};         // USB task
    size_t fill_len{0};      // USB task
    uint8_t frame_id{2};     // USB task; 2: no frame started
    bool skip_frame{false};  // USB task
    uint8_t ready_index{0};
    size_t ready_len{0};
    std::atomic<bool> ready{false};  // set by the USB task, cleared by the main loop

    ~Camera();
  };

  bool accepts_more_devices_() const override { return true; }
  void on_device_connected(usb_host::UsbDevice &dev) override;
  void on_device_disconnected(usb_host::UsbDevice &dev) override;
  void free_device_context(usb_host::UsbDevice &dev) override;
  bool device_busy_(const usb_host::UsbDevice &dev) const override;
  void on_stream_open(usb_host::IsocStream &stream, bool ok) override;
  // USB task.
  void on_stream_packet(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error) override;

  bool parse_descriptors_(Camera &cam);
  // A probe/commit request; next runs on the main loop with the outcome, unless the camera
  // went away meanwhile.
  void probe_request_(Camera &cam, uint8_t request, uint8_t selector, std::function<void(bool)> &&next);
  void commit_(Camera &cam);
  void start_stream_(Camera &cam);
  bool alloc_buffers_(Camera &cam, size_t size);
  // A failed setup gives the camera up (closed, its memory freed once nothing of it is out).
  void finish_setup_(Camera &cam, bool ok);
  Camera *find_camera_(uint32_t id) const;

  std::vector<Camera *> cameras_{};
  uint32_t next_id_{0};

  UvcFormat req_format_{UvcFormat::MJPEG};
  uint16_t req_width_{0};  // 0: any
  uint16_t req_height_{0};
  uint8_t req_fps_{0};  // 0: the frame's default interval
  uint8_t num_urbs_{3};

  std::vector<frame_cb_t> frame_callbacks_{};
};

}  // namespace esphome::usb_uvc

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
