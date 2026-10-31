#include "rtsp_server.h"

#ifdef USE_CAMERA

#include <cerrno>
#include <cinttypes>
#include <utility>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::rtsp_server {

ESPHOME_LOG_TAG(TAG, "rtsp_server");

void RTSPServer::socket_failed_(const char *what) {
  ESP_LOGE(TAG, "Socket %s failed: errno %d", what, errno);
  delete this->socket_;
  this->socket_ = nullptr;
  this->mark_failed();
}

void RTSPServer::setup() {
  this->socket_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0).release();
  if (this->socket_ == nullptr) {
    ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
    this->mark_failed();
    return;
  }

  int enable = 1;
  this->socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

  if (this->socket_->setblocking(false) != 0) {
    this->socket_failed_("nonblocking");
    return;
  }

  struct sockaddr_storage server{};
  socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &server, sizeof(server), this->port_);
  if (sl == 0 || this->socket_->bind((struct sockaddr *) &server, sl) != 0) {
    this->socket_failed_("bind");
    return;
  }

  if (this->socket_->listen(4) != 0) {
    this->socket_failed_("listen");
    return;
  }

  camera::Camera *camera = camera::Camera::instance();
  if (camera == nullptr) {
    ESP_LOGE(TAG, "No camera");
    this->mark_failed();
    return;
  }
  if (!camera->supports_format(this->format_)) {
    ESP_LOGW(TAG, "The camera has no %s stream; JPEG is used", camera::to_string(this->format_));
    this->format_ = camera::IMAGE_FORMAT_JPEG;
  }
  camera->add_listener(this);
}

void RTSPServer::set_format(camera::ImageFormat format) {
  camera::Camera *camera = camera::Camera::instance();
  if (format == this->format_ || (camera != nullptr && !camera->supports_format(format)))
    return;
  this->format_ = format;
  for (auto &session : this->sessions_)
    session.close();
  ESP_LOGI(TAG, "Stream format %s", camera::to_string(format));
}

void RTSPServer::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->socket_ != nullptr && this->socket_->ready()) {
    this->accept_new_connections_(now);
  }

  bool playing = false;
  for (auto &session : this->sessions_) {
    session.loop(now, this->port_);
    playing |= session.is_playing();
  }

  if (playing != this->streaming_ || (playing && this->stream_format_ != this->format_)) {
    camera::Camera *camera = camera::Camera::instance();
    if (this->streaming_)
      camera->stop_stream_as(camera::RTSP_REQUESTER, this->stream_format_);
    this->streaming_ = playing;
    this->stream_format_ = this->format_;
    if (playing)
      camera->start_stream_as(camera::RTSP_REQUESTER, this->stream_format_);
  }
}

void RTSPServer::accept_new_connections_(uint32_t now) {
  while (true) {
    struct sockaddr_storage source_addr;
    socklen_t addr_len = sizeof(source_addr);

    auto sock = this->socket_->accept_loop_monitored((struct sockaddr *) &source_addr, &addr_len);
    if (!sock)
      break;
#ifdef USE_SOCKET_IPV4_ALLOW
    if (!this->allow_.admit(reinterpret_cast<const struct sockaddr *>(&source_addr), addr_len, TAG, now))
      continue;
#endif

    RTSPSession *free_slot = nullptr;
    for (auto &session : this->sessions_) {
      if (!session.is_active()) {
        free_slot = &session;
        break;
      }
    }
    if (free_slot == nullptr) {
      ESP_LOGW(TAG, "Max connections (%d) reached, rejecting connection", MAX_RTSP_CONNECTIONS);
      continue;
    }

    ESP_LOGD(TAG, "Accepted RTSP connection");
    free_slot->open(std::move(sock), now, this->format_);
  }
}

void RTSPServer::on_camera_image(const std::shared_ptr<camera::CameraImage> &image) {
  if (!this->streaming_ || !image->was_requested_by(camera::RTSP_REQUESTER) ||
      image->get_format() != this->stream_format_)
    return;
  if (this->stream_format_ == camera::IMAGE_FORMAT_H264) {
    const uint32_t now = App.get_loop_component_start_time();
    bool key = false;
    for (auto &session : this->sessions_)
      key |= session.on_new_h264_frame(image, now);
    if (key)
      camera::Camera::instance()->request_key_frame();
    return;
  }

  JpegInfo jpeg;
  if (!parse_jpeg_info(image->get_data_buffer(), image->get_data_length(), jpeg) ||
      jpeg.width > RTP_JPEG_MAX_DIMENSION || jpeg.height > RTP_JPEG_MAX_DIMENSION) {
    if (!this->frame_warned_) {
      this->frame_warned_ = true;
      ESP_LOGW(TAG,
               "Dropping frame %" PRIu16 "x%" PRIu16 ": not a baseline 4:2:2/4:2:0 JPEG without restart markers"
               " of at most %" PRIu16 " px per side",
               jpeg.width, jpeg.height, RTP_JPEG_MAX_DIMENSION);
    }
    return;
  }

  const uint32_t now = App.get_loop_component_start_time();
  for (auto &session : this->sessions_) {
    session.on_new_frame(image, jpeg, now);
  }
}

void RTSPServer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "RTSP Server:\n"
                "  Port: %u\n"
                "  Max connections: %u\n"
                "  Format: %s",
                this->port_, MAX_RTSP_CONNECTIONS, camera::to_string(this->format_));
#ifdef USE_SOCKET_IPV4_ALLOW
  this->allow_.dump_config(TAG);
#endif
}

float RTSPServer::get_setup_priority() const { return setup_priority::LATE; }

}  // namespace esphome::rtsp_server

#endif
