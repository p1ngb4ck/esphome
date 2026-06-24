#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "uvc_types.h"

#include <atomic>
#include <functional>
#include <vector>
#include <string>

namespace esphome {
namespace usb_uvc {

// Callback fired in the main loop when a complete frame arrives.
// data/len: frame buffer (valid only for the duration of the callback).
// format: encoding of this frame.
using frame_cb_t = std::function<void(const uint8_t *data, size_t len, UvcFormat format)>;

// ─────────────────────────────────────────────────────────────────────────────
// UVCClient
//
// Native UVC (USB Video Class) host driver built on USBClient.
// No uvc_host library involved — implements the class protocol
// directly using USBClient's control_transfer() and stream_open_() / stream_close_().
//
// Lifecycle (all called from main-loop context):
//   on_connected()    — parse descriptors → probe/commit → stream_open_()
//   on_disconnected() — stream_close_() → notify callbacks
//   on_isoc_packet()  — frame assembly (USB-task context, fast path)
//
// Frame delivery:
//   Complete frames are assembled in on_isoc_packet() into frame_buf_.
//   When end-of-frame is detected the assembled frame is queued via a
//   flag so loop() can fire the user callback from the main-loop context.
// ─────────────────────────────────────────────────────────────────────────────
class UVCClient : public usb_host::USBClient {
 public:
  UVCClient() : usb_host::USBClient(0, 0) {}

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }
  uint8_t get_interface_class() const override { return USB_CLASS_VIDEO; }

  // ── Configuration setters (called from to_code()) ─────────────────────────
  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  // Requested format and resolution — negotiated with device during connect.
  void set_format(UvcFormat fmt) { this->req_format_ = fmt; }
  void set_width(uint16_t w)    { this->req_width_  = w; }
  void set_height(uint16_t h)   { this->req_height_ = h; }
  void set_fps(uint8_t fps)     { this->req_fps_    = fps; }
  // Number of isochronous URBs in flight (triple-buffering = 3 recommended).
  void set_num_urbs(uint8_t n)  { this->num_urbs_   = n; }

  void add_frame_callback(frame_cb_t cb) { this->frame_callbacks_.push_back(std::move(cb)); }

 protected:
  void on_connected() override;
  void on_disconnected() override;

  // USB-task context — must be fast and non-blocking.
  void on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) override;

  // ── Descriptor parsing ────────────────────────────────────────────────────
  // Scan config descriptor for VideoStreaming interface with a matching format.
  // Fills vs_intf_, vs_alt_, vs_ep_, vs_mps_, selected_frame_.
  bool find_streaming_interface_();

  // ── Probe / Commit ────────────────────────────────────────────────────────
  bool probe_commit_();

  // ── Frame assembly state ──────────────────────────────────────────────────
  std::vector<uint8_t> frame_buf_;          // accumulates current frame (USB-task writes)
  std::atomic<bool> frame_ready_{false};    // USB task sets, main loop clears
  uint8_t current_frame_id_{2};             // 0/1 toggle; 2 = "not started" (USB-task only)
  bool skip_frame_{false};                  // USB-task only

  // ── Stream parameters (filled by find_streaming_interface_()) ─────────────
  uint8_t  vs_intf_{0};              // VideoStreaming interface number
  uint8_t  vs_alt_{0};              // alternate setting with bandwidth
  uint8_t  vs_ep_{0};              // isochronous IN endpoint address
  uint16_t vs_mps_{0};             // max packet size × mult
  UvcFrameDesc selected_frame_{};

  // ── User configuration ────────────────────────────────────────────────────
  UvcFormat req_format_{UvcFormat::MJPEG};
  uint16_t  req_width_{0};          // 0 = any
  uint16_t  req_height_{0};
  uint8_t   req_fps_{0};            // 0 = device default
  uint8_t   num_urbs_{3};

  // ── IsocStream member (passed to USBStreamClient) ─────────────────────────
  usb_host::IsocStream stream_{};

  std::vector<frame_cb_t> frame_callbacks_{};
};

}  // namespace usb_uvc
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
