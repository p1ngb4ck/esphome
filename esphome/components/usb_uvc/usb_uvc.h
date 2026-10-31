#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "uvc_types.h"
#ifdef USE_CAMERA
#include "esphome/components/camera/camera.h"
#include "esphome/components/camera/encoder.h"
#endif

#include <atomic>
#include <functional>
#include <vector>

namespace esphome::usb_uvc {

/// The frame buffers' memory: taken when a stream starts, given back when it stops (the player's pool on the panel).
using BufferAlloc = uint8_t *(*) (size_t size);
using BufferFree = void (*)(uint8_t *ptr);

/// One frame size of one format a camera reports (its descriptors), the unit a stream is selected in.
struct UvcFrameInfo {
  UvcFormat format;
  uint8_t format_index;
  uint8_t frame_index;
  // Uncompressed / frame-based: the first 4 bytes of the format GUID ("YUY2", "NV12", "H264"); 0 for MJPEG
  uint32_t fourcc;
  uint16_t width;
  uint16_t height;
  uint32_t max_frame_size;    // 0: not reported (frame-based), the probe says
  uint32_t default_interval;  // 100 ns units
  // Discrete intervals (interval_count of them), or a continuous range (interval_count 0: min, max, step)
  static constexpr uint8_t MAX_INTERVALS = 8;
  uint8_t interval_count;
  uint32_t intervals[MAX_INTERVALS];
  uint32_t min_interval;
  uint32_t max_interval;
  uint32_t interval_step;
};

// A complete frame of the camera at bus address `camera`, on the main loop. data is valid only during the call.
using frame_cb_t = std::function<void(uint8_t camera, const uint8_t *data, size_t len, const UvcFrameInfo &info)>;
// A camera plugged in (true) or gone (false), on the main loop.
using camera_cb_t = std::function<void(uint8_t camera, bool connected)>;

// UVC (USB Video Class) cameras as one usb_host client serving any number of them at once. Nothing of a camera
// exists before it is plugged in: on plug its formats are read from the descriptors; its stream runs only while
// asked for (start / stop), its frame buffers from the allocator for that time. A selected format / frame size /
// interval is switched by closing the stream and opening it again in the new one.
//
// Setup of a stream runs on the main loop as a chain of control transfers, each continued from its
// completion (no waiting): PROBE SET_CUR -> PROBE GET_CUR -> COMMIT SET_CUR -> isochronous
// stream on the smallest alt-setting that carries the negotiated payload size.
// Frames are assembled on the USB task into one of two buffers; a finished frame is
// handed to the main loop, while the other buffer takes the next one.
class UVCClient : public usb_host::USBClient {
 public:
  UVCClient() : usb_host::USBClient(0, 0) { this->add_class_match(USB_CLASS_VIDEO); }

  void loop() override;

  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }
  // The selection a camera starts with, where it reports it (0: any)
  void set_format(UvcFormat fmt) { this->req_format_ = fmt; }
  void set_width(uint16_t w) { this->req_width_ = w; }
  void set_height(uint16_t h) { this->req_height_ = h; }
  void set_fps(uint8_t fps) { this->req_fps_ = fps; }
  void set_num_urbs(uint8_t n) { this->num_urbs_ = n; }
  void set_buffer_allocator(BufferAlloc alloc, BufferFree free) {
    this->alloc_ = alloc;
    this->free_ = free;
  }

  void add_frame_callback(frame_cb_t cb) { this->frame_callbacks_.push_back(std::move(cb)); }
  void add_camera_callback(camera_cb_t cb) { this->camera_callbacks_.push_back(std::move(cb)); }

  // Main loop. Cameras by their bus address.
  uint8_t camera_count() const { return static_cast<uint8_t>(this->cameras_.size()); }
  /// The bus address of the index-th plugged camera (0: none).
  uint8_t camera_at(uint8_t index) const;
  /// The frame sizes (of every format) the camera reports; nullptr past the end.
  size_t frame_count(uint8_t camera) const;
  const UvcFrameInfo *frame_info(uint8_t camera, size_t index) const;
  /// The selected entry of frame_info() and its interval (100 ns units); -1: none.
  int selected(uint8_t camera) const;
  uint32_t selected_interval(uint8_t camera) const;
  /// Selects a frame size (and interval, 0: its default); a running stream switches to it. False: no such entry.
  bool select(uint8_t camera, size_t index, uint32_t interval = 0);
  /// The stream runs while anyone started it (counted: each start() one stop()); the last stop gives its buffers back.
  void start(uint8_t camera);
  void stop(uint8_t camera);
  bool is_streaming(uint8_t camera) const;
  /// In a frame callback: the frame stays (its buffer is not refilled, the next one waits) until release_frame() with
  /// the token returned (0: no frame being delivered); counted, so several consumers can hold it.
  uint32_t hold_frame(uint8_t camera);
  /// Main loop: a held frame is done with; the camera may be gone meanwhile.
  void release_frame(uint32_t token);

 protected:
  struct AltSetting {
    uint8_t alt;
    uint8_t ep;
    uint16_t mps;
    uint16_t interval;  // (micro)frames between its transactions: 2^(bInterval - 1)
  };

  enum class StreamState : uint8_t { IDLE, NEGOTIATING, STREAMING, CLOSING };

  // One plugged camera (its UsbDevice's context): made in on_device_connected(), freed in
  // free_device_context() once its stream is closed (device_busy_()).
  // A stream that died is opened again this often in a row (without a frame between) before it is given up
  static constexpr uint8_t MAX_RESTARTS = 3;
  struct Camera {
    usb_host::UsbDevice *dev{nullptr};
    uint32_t id{0};  // continuations of an earlier camera find it gone

    // Device, from the descriptors.
    uint16_t probe_len{26};  // probe/commit structure size of the device's UVC version
    uint8_t vs_intf{0};
    std::vector<AltSetting> alts{};
    bool high_speed{false};
    UvcFrameInfo *frames{nullptr};  // PSRAM, frame_count of them
    uint16_t frame_count{0};

    int16_t selected{-1};
    uint32_t interval{0};
    StreamState state{StreamState::IDLE};
    bool wanted{false};   // start() asked for the stream (starts > 0)
    uint8_t starts{0};
    bool reopen{false};   // closing for a switch: open again in the new selection once closed
    uint8_t restarts{0};  // the stream died and was opened again; back to 0 with each delivered frame

    UvcVsCtrl probe{};
    usb_host::IsocStream stream{};

    // Two frame buffers (the allocator's): the USB task fills one while the main loop reads the other.
    uint8_t *buffers[2]{nullptr, nullptr};
    size_t buffer_size{0};
    uint8_t fill{0};         // USB task
    size_t fill_len{0};      // USB task
    uint8_t frame_id{2};     // USB task; 2: no frame started
    bool skip_frame{false};  // USB task
    uint8_t skip_why{0};     // USB task: why this frame is not delivered (bit 0 a hole, 1 its header's error bit / too big)
    // Frames of the stream since stats_since (the 10 s line): USB task counts, main loop reads and clears
    std::atomic<uint16_t> n_ok{0};      // handed over
    std::atomic<uint16_t> n_holes{0};   // a lost or skipped packet
    std::atomic<uint16_t> n_bad{0};     // the header's error bit, larger than the buffer, or MJPEG without SOI / EOI
    std::atomic<uint16_t> n_busy{0};    // whole, but the last one was still read
    uint32_t stats_since{0};            // main loop
    uint8_t ready_index{0};
    size_t ready_len{0};
    std::atomic<bool> ready{false};  // set by the USB task, cleared by the main loop
    uint8_t held{0};                 // consumers reading the ready frame (hold_frame / release_frame)
  };

  bool accepts_more_devices_() const override { return true; }
  void on_device_connected(usb_host::UsbDevice &dev) override;
  void on_device_disconnected(usb_host::UsbDevice &dev) override;
  void free_device_context(usb_host::UsbDevice &dev) override;
  bool device_busy_(const usb_host::UsbDevice &dev) const override;
  void on_stream_open(usb_host::IsocStream &stream, bool ok) override;
  // USB task.
  void on_stream_packet(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error,
                        bool skipped) override;

  bool parse_descriptors_(Camera &cam);
  void select_default_(Camera &cam);
  Camera *find_(uint8_t camera) const;
  Camera *find_camera_(uint32_t id) const;
  // The stream in the selection: probe / commit, buffers, isochronous stream
  void open_(Camera &cam);
  // A probe/commit request; next runs on the main loop with the outcome, unless the camera
  // went away meanwhile.
  void probe_request_(Camera &cam, uint8_t request, uint8_t selector, std::function<void(bool)> &&next);
  void commit_(Camera &cam);
  void start_stream_(Camera &cam);
  bool alloc_buffers_(Camera &cam, size_t size);
  void free_buffers_(Camera &cam);
  /// The stream is asked to close; its buffers go once it is closed (loop()).
  void close_(Camera &cam);
  // A failed setup: back to idle, the buffers given back
  void fail_(Camera &cam);
  void destroy_(Camera *cam);

  std::vector<Camera *> cameras_{};
  std::vector<Camera *> gone_{};  // unplugged while a frame of theirs is held: freed once it is released
  uint32_t next_id_{0};

  UvcFormat req_format_{UvcFormat::MJPEG};
  uint16_t req_width_{0};  // 0: any
  uint16_t req_height_{0};
  uint8_t req_fps_{0};  // 0: the frame's default interval
  uint8_t num_urbs_{3};
  BufferAlloc alloc_{nullptr};
  BufferFree free_{nullptr};

  std::vector<frame_cb_t> frame_callbacks_{};
  std::vector<camera_cb_t> camera_callbacks_{};
};

#ifdef USE_CAMERA
/// A held MJPEG frame of a UVC camera as a camera image; the frame is released when the last reference goes.
class UvcImage : public camera::CameraImage {
 public:
  UvcImage(UVCClient *client, uint32_t token, uint8_t *data, size_t length, uint8_t requesters,
           camera::ImageFormat format = camera::IMAGE_FORMAT_JPEG, bool key = true)
      : client_(client),
        token_(token),
        data_(data),
        length_(length),
        requesters_(requesters),
        format_(format),
        key_(key) {}
  ~UvcImage() override { this->client_->release_frame(this->token_); }
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1U << requester)) != 0;
  }
  camera::ImageFormat get_format() const override { return this->format_; }
  bool is_key_frame() const override { return this->key_; }

 protected:
  UVCClient *client_;
  uint32_t token_;
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
  camera::ImageFormat format_;
  bool key_;
};

class UvcCamera;

class UvcEncodedImage : public camera::CameraImage {
 public:
  UvcEncodedImage(UvcCamera *camera, uint8_t *data, size_t length, uint8_t requesters, bool key)
      : camera_(camera), data_(data), length_(length), requesters_(requesters), key_(key) {}
  ~UvcEncodedImage() override;
  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1U << requester)) != 0;
  }
  camera::ImageFormat get_format() const override { return camera::IMAGE_FORMAT_H264; }
  bool is_key_frame() const override { return this->key_; }

 protected:
  UvcCamera *camera_;
  uint8_t *data_;
  size_t length_;
  uint8_t requesters_;
  bool key_;
};

class UvcImageReader : public camera::CameraImageReader {
 public:
  void set_image(std::shared_ptr<camera::CameraImage> image) override {
    this->image_ = std::move(image);
    this->offset_ = 0;
  }
  size_t available() const override { return this->image_ ? this->image_->get_data_length() - this->offset_ : 0; }
  uint8_t *peek_data_buffer() override {
    return this->image_ ? this->image_->get_data_buffer() + this->offset_ : nullptr;
  }
  void consume_data(size_t consumed) override { this->offset_ += consumed; }
  void return_image() override { this->image_.reset(); }

 protected:
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

/// The UVC camera as a Home Assistant camera entity: the first plugged camera, its MJPEG frames as they are. The
/// stream runs (counted start) while an image or a stream is asked for.
class UvcCamera : public camera::Camera {
 public:
  void set_client(UVCClient *client) { this->client_ = client; }
  void set_h264_encoder(camera::Encoder *encoder) { this->h264_encoder_ = encoder; }
  bool supports_format(camera::ImageFormat) const override { return true; }
  void start_stream_as(camera::CameraRequester requester, camera::ImageFormat format) override;
  void stop_stream_as(camera::CameraRequester requester, camera::ImageFormat format) override;
  void request_key_frame() override;
  void on_encoded_released();
  void setup() override;
  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new UvcImageReader(); }  // NOLINT
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  void on_frame_(uint8_t camera, const uint8_t *data, size_t length, const UvcFrameInfo &info);
  void update_stream_();
  void pick_format_();
  void on_h264_frame_(uint8_t camera, const uint8_t *data, size_t length, const UvcFrameInfo &info);
  void stop_encoder_();

  UVCClient *client_{nullptr};
  camera::Encoder *h264_encoder_{nullptr};
  uint8_t h264_{0};
  int16_t jpeg_pick_{-1};
  bool h264_picked_{false};
  bool encoder_started_{false};
  std::atomic<bool> encoded_held_{false};
  bool h264_warned_{false};
  std::vector<camera::CameraListener *> listeners_{};
  uint8_t single_{0};     // requesters waiting for one image
  uint8_t streaming_{0};  // requesters streaming
  uint8_t camera_{0};     // the camera this entity started (0: none)
  bool warned_{false};
};
#endif

}  // namespace esphome::usb_uvc

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31
