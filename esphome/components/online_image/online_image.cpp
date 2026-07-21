#include "online_image.h"
#include "esphome/components/runtime_image/image_decoder.h"
#include "esphome/core/log.h"
#include <algorithm>

static const char *const TAG = "online_image";
static const char *const ETAG_HEADER_NAME = "etag";
static const char *const IF_NONE_MATCH_HEADER_NAME = "if-none-match";
static const char *const LAST_MODIFIED_HEADER_NAME = "last-modified";
static const char *const IF_MODIFIED_SINCE_HEADER_NAME = "if-modified-since";

namespace esphome::online_image {

OnlineImage::OnlineImage(const std::string &url, int width, int height, runtime_image::ImageFormat format,
                         image::ImageType type, image::Transparency transparency, image::Image *placeholder,
                         uint32_t buffer_size, bool is_big_endian)
    : RuntimeImage(format, type, transparency, placeholder, is_big_endian, width, height),
      download_buffer_(buffer_size),
      download_buffer_initial_size_(buffer_size) {
  this->set_url(url);
}

bool OnlineImage::validate_url_(const std::string &url) {
  if (url.empty()) {
    ESP_LOGE(TAG, "URL is empty");
    return false;
  }
  if (url.length() > 2048) {
    ESP_LOGE(TAG, "URL is too long");
    return false;
  }
#ifdef USE_STORAGE
  // A local storage source is anything without an http(s):// scheme: a bare POSIX path
  // (/sdcard/img.png) or the optional file:// alias. It is resolved at read time.
  if (!url.starts_with("http://") && !url.starts_with("https://")) {
    return true;
  }
#else
  if (!url.starts_with("http://") && !url.starts_with("https://")) {
    ESP_LOGE(TAG, "URL must start with http:// or https:// (local storage paths require the storage component)");
    return false;
  }
#endif
  return true;
}

void OnlineImage::update() {
  if (this->is_decoding()) {
    ESP_LOGW(TAG, "Image already being updated.");
    return;
  }

  if (!this->validate_url_(this->url_)) {
    ESP_LOGE(TAG, "Invalid URL: %s", this->url_.c_str());
    this->download_error_callback_.call();
    return;
  }

  ESP_LOGD(TAG, "Updating image from %s", this->url_.c_str());

#ifdef USE_STORAGE
  if (!this->url_.starts_with("http://") && !this->url_.starts_with("https://")) {
    if (!this->start_storage_read_()) {
      this->end_connection_();
      this->download_error_callback_.call();
      return;
    }
    if (!this->begin_decode(this->storage_size_)) {
      ESP_LOGE(TAG, "Failed to initialize decoder for format %d", this->get_format());
      this->end_connection_();
      this->download_error_callback_.call();
      return;
    }
    // JPEG requires the complete image in the buffer before decoding — same rule as http
    if (this->get_format() == runtime_image::JPEG && this->storage_size_ > this->download_buffer_.size()) {
      this->download_buffer_.resize(this->storage_size_);
    }
    ESP_LOGI(TAG, "Reading image from storage (Size: %llu)", (unsigned long long) this->storage_size_);
    this->start_time_ = millis();
    this->enable_loop();
    return;
  }
#endif

#ifdef USE_ONLINE_IMAGE_HTTP
  std::vector<http_request::Header> headers;

  // Add caching headers if we have them
  if (!this->etag_.empty()) {
    headers.push_back({IF_NONE_MATCH_HEADER_NAME, this->etag_});
  }
  if (!this->last_modified_.empty()) {
    headers.push_back({IF_MODIFIED_SINCE_HEADER_NAME, this->last_modified_});
  }

  // Add Accept header based on image format
  const char *accept_mime_type;
  switch (this->get_format()) {
#ifdef USE_RUNTIME_IMAGE_BMP
    case runtime_image::BMP:
      accept_mime_type = "image/bmp,*/*;q=0.8";
      break;
#endif
#ifdef USE_RUNTIME_IMAGE_JPEG
    case runtime_image::JPEG:
      accept_mime_type = "image/jpeg,*/*;q=0.8";
      break;
#endif
#ifdef USE_RUNTIME_IMAGE_PNG
    case runtime_image::PNG:
      accept_mime_type = "image/png,*/*;q=0.8";
      break;
#endif
    default:
      accept_mime_type = "image/*,*/*;q=0.8";
      break;
  }
  headers.push_back({"Accept", accept_mime_type});

  // User headers last so they can override any of the above
  for (auto &header : this->request_headers_) {
    headers.push_back(http_request::Header{header.first, header.second.value()});
  }

  this->downloader_ = this->parent_->get(this->url_, headers, {ETAG_HEADER_NAME, LAST_MODIFIED_HEADER_NAME});

  if (this->downloader_ == nullptr) {
    ESP_LOGE(TAG, "Download failed.");
    this->end_connection_();
    this->download_error_callback_.call();
    return;
  }

  int http_code = this->downloader_->status_code;
  if (http_code == HTTP_CODE_NOT_MODIFIED) {
    // Image hasn't changed on server. Skip download.
    ESP_LOGI(TAG, "Server returned HTTP 304 (Not Modified). Download skipped.");
    this->end_connection_();
    this->download_finished_callback_.call(true);
    return;
  }
  if (http_code != HTTP_CODE_OK) {
    ESP_LOGE(TAG, "HTTP result: %d", http_code);
    this->end_connection_();
    this->download_error_callback_.call();
    return;
  }

  ESP_LOGD(TAG, "Starting download");
  size_t total_size = this->downloader_->content_length;

  // Initialize decoder with the known format
  if (!this->begin_decode(total_size)) {
    ESP_LOGE(TAG, "Failed to initialize decoder for format %d", this->get_format());
    this->end_connection_();
    this->download_error_callback_.call();
    return;
  }

  // JPEG requires the complete image in the download buffer before decoding
  if (this->get_format() == runtime_image::JPEG && total_size > this->download_buffer_.size()) {
    this->download_buffer_.resize(total_size);
  }

  ESP_LOGI(TAG, "Downloading image (Size: %zu)", total_size);
  this->start_time_ = millis();
  this->enable_loop();
#endif  // USE_ONLINE_IMAGE_HTTP
}

void OnlineImage::loop() {
  if (!this->is_decoding()) {
    // Not decoding at the moment => nothing to do.
    this->disable_loop();
    return;
  }

#ifdef USE_STORAGE
  if (this->storage_ != nullptr) {
    this->storage_loop_();
    return;
  }
#endif

#ifdef USE_ONLINE_IMAGE_HTTP
  if (!this->downloader_) {
    ESP_LOGE(TAG, "Downloader not instantiated; cannot download");
    this->end_connection_();
    this->download_error_callback_.call();
    return;
  }

  // Check if download is complete — use decoder's format-specific completion check
  // to handle both known content-length and chunked transfer encoding
  if (this->is_decode_finished() || (this->downloader_->content_length > 0 &&
                                     this->downloader_->get_bytes_read() >= this->downloader_->content_length &&
                                     this->download_buffer_.unread() == 0)) {
    // Finalize decoding
    this->end_decode();

    ESP_LOGD(TAG, "Image fully downloaded, %zu bytes in %" PRIu32 " ms", this->downloader_->get_bytes_read(),
             millis() - this->start_time_);

    // Save caching headers
    this->etag_ = this->downloader_->get_response_header(ETAG_HEADER_NAME);
    this->last_modified_ = this->downloader_->get_response_header(LAST_MODIFIED_HEADER_NAME);

    this->download_finished_callback_.call(false);
    this->end_connection_();
    return;
  }

  // Download and decode more data
  size_t available = this->download_buffer_.free_capacity();
  if (available > 0) {
    // Download in chunks to avoid blocking
    available = std::min(available, this->download_buffer_initial_size_);
    auto len = this->downloader_->read(this->download_buffer_.append(), available);

    if (len > 0) {
      this->download_buffer_.write(len);

      // Feed data to decoder
      auto consumed = this->feed_data(this->download_buffer_.data(), this->download_buffer_.unread());

      if (consumed < 0) {
        ESP_LOGE(TAG, "Error decoding image: %s", esphome::runtime_image::decode_error_to_string(consumed));
        this->end_connection_();
        this->download_error_callback_.call();
        return;
      }

      if (consumed > 0) {
        this->download_buffer_.read(consumed);
      }
    } else if (len < 0) {
      ESP_LOGE(TAG, "Error downloading image: %d", len);
      this->end_connection_();
      this->download_error_callback_.call();
      return;
    }
  } else {
    // Buffer is full, need to decode some data first
    auto consumed = this->feed_data(this->download_buffer_.data(), this->download_buffer_.unread());
    if (consumed > 0) {
      this->download_buffer_.read(consumed);
    } else if (consumed < 0) {
      ESP_LOGE(TAG, "Decode error with full buffer: %d", consumed);
      this->end_connection_();
      this->download_error_callback_.call();
      return;
    } else {
      // Decoder can't process more data, might need complete image
      // This is normal for JPEG which needs complete data
      ESP_LOGV(TAG, "Decoder waiting for more data");
    }
  }
#endif  // USE_ONLINE_IMAGE_HTTP
}

void OnlineImage::end_connection_() {
  // Abort any in-progress decode to free decoder resources.
  // Use RuntimeImage::release() directly to avoid recursion with OnlineImage::release().
  if (this->is_decoding()) {
    RuntimeImage::release();
  }
#ifdef USE_STORAGE
  if (this->storage_ != nullptr) {
    if (this->storage_handle_ != nullptr) {
      static_cast<storage::FilesystemStorage *>(this->storage_)->close(this->storage_handle_);
      this->storage_handle_ = nullptr;
    }
    this->storage_ = nullptr;
    this->storage_path_.clear();
    this->storage_offset_ = 0;
    this->storage_size_ = 0;
  }
#endif
#ifdef USE_ONLINE_IMAGE_HTTP
  if (this->downloader_) {
    this->downloader_->end();
    this->downloader_ = nullptr;
  }
#endif
  this->download_buffer_.reset();
  this->disable_loop();
}

void OnlineImage::release() {
  // Clear cache headers
  this->etag_ = "";
  this->last_modified_ = "";

  // End any active connection
  this->end_connection_();

  // Call parent's release to free the image buffer
  RuntimeImage::release();
}

#ifdef USE_STORAGE
bool OnlineImage::start_storage_read_() {
  const char *path = this->url_.c_str();
  if (this->url_.starts_with("file://"))  // optional alias prefix
    path += strlen("file://");
  const char *rel = nullptr;
  if (storage::global_storage_registry == nullptr ||
      (this->storage_ = storage::global_storage_registry->resolve_path(path, &rel)) == nullptr || rel == nullptr) {
    ESP_LOGE(TAG, "'%s' does not resolve to a mounted storage", path);
    this->storage_ = nullptr;
    return false;
  }
  storage::FileStat st;
  storage::StorageError serr = this->storage_->stat(rel, &st);
  if (serr != storage::StorageError::OK || st.is_directory) {
    ESP_LOGE(TAG, "'%s' not found on storage (%s)", path, storage::error_to_string(serr));
    this->storage_ = nullptr;
    return false;
  }
  this->storage_size_ = st.size;
  this->storage_offset_ = 0;
  if (this->storage_->get_storage_type() == storage::StorageType::FILESYSTEM) {
    auto *fs = static_cast<storage::FilesystemStorage *>(this->storage_);
    serr = fs->open(rel, this->storage_handle_, storage::OpenMode::READ);
    if (serr != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Opening '%s' failed (%s)", path, storage::error_to_string(serr));
      this->storage_ = nullptr;
      this->storage_handle_ = nullptr;
      return false;
    }
  } else if (this->storage_->get_storage_type() == storage::StorageType::NETWORK) {
    this->storage_path_ = rel;  // owned copy — rel points into resolver scratch
  } else {
    ESP_LOGE(TAG, "Storage type of '%s' does not support streaming reads", path);
    this->storage_ = nullptr;
    return false;
  }
  return true;
}

void OnlineImage::storage_loop_() {
  // Completion: everything read AND the decoder consumed the buffer.
  if (this->is_decode_finished() ||
      (this->storage_offset_ >= this->storage_size_ && this->download_buffer_.unread() == 0)) {
    this->end_decode();
    ESP_LOGD(TAG, "Image fully read from storage, %llu bytes in %" PRIu32 " ms",
             (unsigned long long) this->storage_offset_, millis() - this->start_time_);
    this->download_finished_callback_.call(false);
    this->end_connection_();
    return;
  }

  size_t available = this->download_buffer_.free_capacity();
  if (available > 0 && this->storage_offset_ < this->storage_size_) {
    available = std::min(available, this->download_buffer_initial_size_);
    size_t received = 0;
    storage::StorageError serr;
    if (this->storage_handle_ != nullptr) {
      serr = static_cast<storage::FilesystemStorage *>(this->storage_)
                 ->read(this->storage_handle_, this->download_buffer_.append(), available, &received);
    } else {
      serr = static_cast<storage::NetworkStorage *>(this->storage_)
                 ->read_chunk(this->storage_path_.c_str(), this->download_buffer_.append(), this->storage_offset_,
                              available, &received);
    }
    if (serr != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Storage read failed (%s)", storage::error_to_string(serr));
      this->end_connection_();
      this->download_error_callback_.call();
      return;
    }
    this->storage_offset_ += received;
    if (received > 0) {
      this->download_buffer_.write(received);
    }
  }

  // Feed whatever is buffered to the decoder (mirrors the http path)
  if (this->download_buffer_.unread() > 0) {
    auto consumed = this->feed_data(this->download_buffer_.data(), this->download_buffer_.unread());
    if (consumed < 0) {
      ESP_LOGE(TAG, "Error decoding image: %s", esphome::runtime_image::decode_error_to_string(consumed));
      this->end_connection_();
      this->download_error_callback_.call();
      return;
    }
    if (consumed > 0) {
      this->download_buffer_.read(consumed);
    }
  }
}
#endif  // USE_STORAGE

}  // namespace esphome::online_image
