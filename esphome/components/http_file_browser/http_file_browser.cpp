#include "http_file_browser.h"
#include "esphome/components/storage/storage.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"

// Storage device headers for mount/unmount
#ifdef USE_USB_STORAGE
#include "esphome/components/usb_storage/usb_storage.h"
#endif
#ifdef USE_SD_STORAGE
#include "esphome/components/sd_storage/sd_storage.h"
#endif

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <cerrno>
#ifdef USE_ESP_IDF
#include <esp_vfs_fat.h>
#include <diskio_impl.h>
#include <esp_heap_caps.h>
#endif

namespace esphome {
namespace http_file_browser {

// Note: File buffers are now preallocated in setup() as part of buffer_pool_
// See upload_buffer_, download_buffer_, copy_buffer_ members

void HttpFileBrowser::setup() {
  ESP_LOGI(TAG, "Setting up HTTP File Browser with prefix: %s", this->url_prefix_.c_str());
  ESP_LOGI(TAG, "Root path: %s", this->root_path_.c_str());
  ESP_LOGI(TAG, "Upload: %s, Download: %s, Delete: %s", this->upload_enabled_ ? "YES" : "NO",
           this->download_enabled_ ? "YES" : "NO", this->deletion_enabled_ ? "YES" : "NO");

  // Allocate buffer pool for file operations
  // Pool contains 3 buffers: upload, download, copy (each FILE_BUFFER_SIZE)
  // FILE_BUFFER_SIZE varies by platform and PSRAM availability:
  //   - PSRAM devices: 32-64KB per buffer = 96-192KB total
  //   - Non-PSRAM devices: 4-16KB per buffer = 12-48KB total
  this->buffer_pool_size_ = FILE_BUFFER_SIZE * 3;

#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
  this->buffer_pool_ = (uint8_t *) heap_caps_malloc(this->buffer_pool_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->buffer_pool_ != nullptr) {
    ESP_LOGI(TAG, "Allocated %" PRIu32 " byte buffer pool from PSRAM (%" PRIu32 " KB per buffer)", (uint32_t) this->buffer_pool_size_,
             FILE_BUFFER_SIZE / 1024);
  } else {
    ESP_LOGW(TAG, "PSRAM allocation failed, using heap for buffer pool");
    this->buffer_pool_ = (uint8_t *) malloc(this->buffer_pool_size_);
  }
#else
  this->buffer_pool_ = (uint8_t *) malloc(this->buffer_pool_size_);
  ESP_LOGI(TAG, "Allocated %" PRIu32 " byte buffer pool from heap (%" PRIu32 " KB per buffer)", (uint32_t) this->buffer_pool_size_,
           FILE_BUFFER_SIZE / 1024);
#endif

  if (this->buffer_pool_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate buffer pool!");
    this->mark_failed();
    return;
  }

  // Partition buffer pool into separate buffers
  this->upload_buffer_ = this->buffer_pool_;
  this->download_buffer_ = this->buffer_pool_ + FILE_BUFFER_SIZE;
  this->copy_buffer_ = this->buffer_pool_ + (FILE_BUFFER_SIZE * 2);

  // Register directly with AsyncWebServer to bypass web_server_base's auth middleware
  // This allows http_file_browser to have its own independent authentication
  auto server = this->base_->get_server();
  if (server) {
    server->addHandler(this);
    ESP_LOGI(TAG, "HTTP File Browser registered successfully (bypassing base auth)");
  } else {
    ESP_LOGE(TAG, "Failed to get web server instance");
  }
}

HttpFileBrowser::~HttpFileBrowser() {
  // Free buffer pool
  if (this->buffer_pool_ != nullptr) {
    free(this->buffer_pool_);
    this->buffer_pool_ = nullptr;
    this->upload_buffer_ = nullptr;
    this->download_buffer_ = nullptr;
    this->copy_buffer_ = nullptr;
  }
  // Free chunk buffer
  if (this->chunk_buffer_ != nullptr) {
    free(this->chunk_buffer_);
    this->chunk_buffer_ = nullptr;
  }
}

void HttpFileBrowser::dump_config() {
  ESP_LOGCONFIG(TAG, "HTTP File Browser:");
  ESP_LOGCONFIG(TAG, "  Root path: %s", this->root_path_.c_str());
  ESP_LOGCONFIG(TAG, "  URL prefix: %s", this->url_prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Auth enabled: %s", this->auth_enabled_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Upload enabled: %s", this->upload_enabled_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Download enabled: %s", this->download_enabled_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Deletion enabled: %s", this->deletion_enabled_ ? "YES" : "NO");
}

void HttpFileBrowser::loop() {
  // Check for deferred mount operations
  if (this->deferred_mount_op_.type != DeferredMountOp::NONE) {
    // Check if it's time to execute the deferred operation
    if (millis() >= this->deferred_mount_op_.schedule_time) {
      std::string mount_point = this->deferred_mount_op_.mount_point;
      DeferredMountOp::Type op_type = this->deferred_mount_op_.type;

      // Clear the deferred operation first (prevents re-execution)
      this->deferred_mount_op_.type = DeferredMountOp::NONE;
      this->deferred_mount_op_.mount_point.clear();

      ESP_LOGI(TAG, "Executing deferred %s operation for mount point: %s",
               op_type == DeferredMountOp::MOUNT     ? "MOUNT"
               : op_type == DeferredMountOp::UNMOUNT ? "UNMOUNT"
                                                     : "REMOUNT",
               mount_point.c_str());

      // Execute the deferred operation
      bool success = false;
      std::string error_msg;

      // Find device via storage registry
      storage::StorageDevice *storage_device = nullptr;
      if (this->storage_ != nullptr) {
        storage_device = this->storage_->get_device_by_mount_path(mount_point);
      }

      if (storage_device == nullptr) {
        ESP_LOGW(TAG, "Device not found for mount point %s", mount_point.c_str());
      } else {
        auto info = storage_device->get_info();

        if (op_type == DeferredMountOp::UNMOUNT) {
#ifdef USE_USB_STORAGE
          if (info.type == storage::StorageType::USB_MSC) {
            auto *device = static_cast<usb_storage::USBStorageDevice *>(storage_device);
            device->unmount_device();
            ESP_LOGI(TAG, "Successfully unmounted USB storage device at %s", mount_point.c_str());
            success = true;
          }
#endif
#ifdef USE_SD_STORAGE
          if (!success && info.type == storage::StorageType::SD_CARD) {
            auto *device = static_cast<sd_storage::SdMmc *>(storage_device);
            device->unmount_card();
            ESP_LOGI(TAG, "Successfully unmounted SD storage device at %s", mount_point.c_str());
            success = true;
          }
#endif
          if (!success) {
            ESP_LOGW(TAG, "Unmount not supported for device type at %s", mount_point.c_str());
          }

        } else if (op_type == DeferredMountOp::MOUNT) {
#ifdef USE_USB_STORAGE
          if (info.type == storage::StorageType::USB_MSC) {
            auto *device = static_cast<usb_storage::USBStorageDevice *>(storage_device);
            if (device->remount_device()) {
              ESP_LOGI(TAG, "Successfully mounted USB storage device at %s", mount_point.c_str());
              success = true;
            } else {
              ESP_LOGE(TAG, "Failed to mount USB storage device at %s", mount_point.c_str());
              error_msg = "Mount failed";
            }
          }
#endif
#ifdef USE_SD_STORAGE
          if (!success && error_msg.empty() && info.type == storage::StorageType::SD_CARD) {
            auto *device = static_cast<sd_storage::SdMmc *>(storage_device);
            if (device->mount_card()) {
              ESP_LOGI(TAG, "Successfully mounted SD storage device at %s", mount_point.c_str());
              success = true;
            } else {
              ESP_LOGE(TAG, "Failed to mount SD storage device at %s", mount_point.c_str());
              error_msg = "Mount failed";
            }
          }
#endif
          if (!success && error_msg.empty()) {
            ESP_LOGW(TAG, "Mount not supported for device type at %s", mount_point.c_str());
          }

        } else if (op_type == DeferredMountOp::REMOUNT) {
#ifdef USE_USB_STORAGE
          if (info.type == storage::StorageType::USB_MSC) {
            auto *device = static_cast<usb_storage::USBStorageDevice *>(storage_device);
            if (device->remount_device()) {
              ESP_LOGI(TAG, "Successfully remounted USB storage device at %s", mount_point.c_str());
              success = true;
            } else {
              ESP_LOGE(TAG, "Failed to remount USB storage device at %s", mount_point.c_str());
              error_msg = "Remount failed";
            }
          }
#endif
#ifdef USE_SD_STORAGE
          if (!success && error_msg.empty() && info.type == storage::StorageType::SD_CARD) {
            auto *device = static_cast<sd_storage::SdMmc *>(storage_device);
            device->unmount_card();
            if (device->mount_card()) {
              ESP_LOGI(TAG, "Successfully remounted SD storage device at %s", mount_point.c_str());
              success = true;
            } else {
              ESP_LOGE(TAG, "Failed to remount SD storage device at %s", mount_point.c_str());
              error_msg = "Remount failed";
            }
          }
#endif
          if (!success && error_msg.empty()) {
            ESP_LOGW(TAG, "Remount not supported for device type at %s", mount_point.c_str());
          }
        }
      }
    }
  }
}

// AsyncWebHandler interface implementation
bool HttpFileBrowser::canHandle(AsyncWebServerRequest *request) const {
  // Handle requests that start with our URL prefix
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef uri = request->url_to(url_buf);
  const char *method_name = (request->method() == HTTP_GET)      ? "GET"
                            : (request->method() == HTTP_POST)   ? "POST"
                            : (request->method() == HTTP_DELETE) ? "DELETE"
                                                                 : "OTHER";

  ESP_LOGD(TAG, "canHandle called: %s %s (prefix: %s)", method_name, uri.c_str(), this->url_prefix_.c_str());

  // Check if URI starts with our prefix
  if (uri.find(this->url_prefix_.c_str()) != 0) {
    ESP_LOGD(TAG, "  -> NO: URI doesn't start with prefix");
    return false;
  }

  // We handle GET, POST, and DELETE methods
  if (request->method() == HTTP_GET || request->method() == HTTP_POST || request->method() == HTTP_DELETE) {
    ESP_LOGI(TAG, "  -> YES: Will handle %s %s", method_name, uri.c_str());
    return true;
  }

  ESP_LOGW(TAG, "  -> NO: Unsupported method %s", method_name);
  return false;
}

void HttpFileBrowser::handleRequest(AsyncWebServerRequest *request) {
  // Check authentication if enabled
  if (this->auth_enabled_) {
    if (!request->authenticate(this->username_.c_str(), this->password_.c_str())) {
      return request->requestAuthentication();
    }
  }

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef uri = request->url_to(url_buf);
  ESP_LOGI(TAG, "handleRequest: uri='%s', url_prefix_='%s', method=%d", uri.c_str(), this->url_prefix_.c_str(),
           request->method());

  // Check for API endpoints
  if (uri.find((this->url_prefix_ + "/api/copy").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API COPY endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_copy(request);
  } else if (uri.find((this->url_prefix_ + "/api/move").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API MOVE endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_move(request);
  } else if (uri.find((this->url_prefix_ + "/api/rename").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API RENAME endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_rename(request);
  } else if (uri.find((this->url_prefix_ + "/api/mkdir").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API MKDIR endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_mkdir(request);
  } else if (uri.find((this->url_prefix_ + "/api/delete").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API DELETE endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_delete(request);
  } else if (uri.find((this->url_prefix_ + "/api/mount").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API MOUNT endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_mount(request);
  } else if (uri.find((this->url_prefix_ + "/api/unmount").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API UNMOUNT endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_unmount(request);
  } else if (uri.find((this->url_prefix_ + "/api/remount").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API REMOUNT endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_remount(request);
  } else if (uri.find((this->url_prefix_ + "/api/fileinfo").c_str()) == 0 && request->method() == HTTP_GET) {
    this->handle_api_fileinfo(request);
    return;
  } else if (uri.find((this->url_prefix_ + "/api/exists").c_str()) == 0 && request->method() == HTTP_GET) {
    this->handle_api_exists(request);
  } else if (uri.find((this->url_prefix_ + "/api/dirisempty").c_str()) == 0 && request->method() == HTTP_GET) {
    this->handle_api_dirisempty(request);
  } else if (uri.find((this->url_prefix_ + "/api/dirinfo").c_str()) == 0 && request->method() == HTTP_GET) {
    this->handle_api_dirinfo(request);
  } else if (uri.find((this->url_prefix_ + "/api/progress").c_str()) == 0 && request->method() == HTTP_GET) {
    this->handle_api_progress(request);
  } else if (uri.find((this->url_prefix_ + "/api/cancel").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API CANCEL endpoint hit");
    this->handle_api_cancel(request);
  } else if (uri.find((this->url_prefix_ + "/api/upload_chunk").c_str()) == 0 && request->method() == HTTP_POST) {
    ESP_LOGD(TAG, "API UPLOAD_CHUNK endpoint hit, body_buffer size: %" PRIu32, (uint32_t) this->body_buffer_.size());
    this->handle_api_upload_chunk(request);
  } else if (request->method() == HTTP_GET) {
    // Handle GET request (directory listing or file download)
    std::string filepath = this->uri_to_filepath(uri);
    ESP_LOGD(TAG, "GET request for: %s (mapped to: %s)", uri.c_str(), filepath.c_str());

    if (filepath.empty()) {
      request->send(400, "text/plain", "Bad Request: Invalid file path");
      return;
    }
    bool is_virtual_root = (this->root_path_ == "/" && filepath == "/");
    // Check if this is network storage
    storage::NetworkStorage *net_storage = nullptr;
    if (filepath.empty()) {
      request->send(400, "text/plain", "Bad Request: Invalid file path");
      return;
    } else {
      if (this->storage_ != nullptr) {
        net_storage = this->storage_->find_network_storage_for_path(filepath);
      }
    }

    struct stat file_stat;
    if (net_storage != nullptr) {
      // Network storage - check if connected first
      if (!net_storage->is_connected()) {
        ESP_LOGW(TAG, "Network storage not connected: %s", filepath.c_str());
        request->send(503, "text/plain", "Service Unavailable: Network storage not connected");
        return;
      }

      // Check if accessing the mount point itself
      if (net_storage->get_mount_path() == filepath) {
        // Accessing mount point root - show directory listing
        this->handle_network_directory_listing(request, net_storage, filepath);
        return;
      }

      // For paths within the mount, check if file/directory exists
      if (this->get_network_file_stat(net_storage, filepath, file_stat) == false) {
        ESP_LOGW(TAG, "Network storage file stat failed for: %s", filepath.c_str());
        request->send(404, "text/plain", "Not Found");
        return;
      }

      // Route to directory listing or file download based on type
      std::string relative_filepath = strip_network_mount_prefix(net_storage, filepath);
      if (net_storage->is_directory(relative_filepath)) {
        this->handle_network_directory_listing(request, net_storage, filepath);
        return;
      }
      // For network storage files, fall through to handle_file_download which supports chunked reads
    }

    if (!is_virtual_root && net_storage == nullptr && stat(filepath.c_str(), &file_stat) != 0) {
      request->send(400, "text/plain", "Bad Request: File not found");
      return;
    }
    if (is_virtual_root || (net_storage == nullptr && S_ISDIR(file_stat.st_mode))) {
      this->handle_directory_listing(request, filepath);
    } else {
      this->handle_file_download(request, filepath);
    }
  } else if (request->method() == HTTP_POST) {
    // Handle POST for file uploads (non-API endpoints)
    ESP_LOGD(TAG, "POST handler - upload for URI: %s", uri.c_str());

    // Check Content-Type to determine upload method
    auto content_type = request->get_header("Content-Type");
    if (content_type.has_value()) {
      ESP_LOGD(TAG, "Content-Type: %s", content_type.value().c_str());
    }

    // If multipart/form-data, it was already handled by handleUpload() callback
    // The web server calls handleRequest() after processing multipart, so just send success
    if (content_type.has_value() && content_type.value().find("multipart/form-data") != std::string::npos) {
      ESP_LOGD(TAG, "Multipart upload was handled by handleUpload() callback");
      // Don't send another response - handleUpload() already sent it
      return;
    } else {
      // Old synchronous upload handler for raw binary uploads
      auto *filename_param = request->getParam("filename");
      if (filename_param) {
        this->handle_file_upload(request, filename_param->value().c_str());
      } else {
        request->send(400, "text/plain", "Missing filename parameter");
      }
    }
  } else {
    request->send(405, "application/json", "{\"error\":\"Method not allowed\"}");
  }
}

bool HttpFileBrowser::get_network_file_stat(storage::NetworkStorage *net_storage, const std::string &path,
                                            struct stat &file_stat) {
  // Strip mount point prefix and use network storage API to get file stat
  std::string relative_path = strip_network_mount_prefix(net_storage, path);
  return net_storage->stat(relative_path, file_stat);
}

bool HttpFileBrowser::handle_network_directory_listing(AsyncWebServerRequest *request,
                                                       storage::NetworkStorage *net_storage, const std::string &path) {
  // Strip mount point prefix to get path relative to network storage root
  std::string relative_path = strip_network_mount_prefix(net_storage, path);

  // Get directory entries from network storage
  std::vector<storage::NetworkStorage::DirEntry> entries;
  if (!net_storage->list_directory(relative_path, entries)) {
    ESP_LOGW(TAG, "Failed to list network directory: %s (relative: %s)", path.c_str(), relative_path.c_str());
    request->send(500, "text/plain", "Internal Server Error: Failed to list directory");
    return false;
  }

  // Generate HTML directory listing (matching main directory listing style)
  std::string html = this->generate_html_header("File Browser");

  html += "<div class=\"header-actions\">";
  html += "<h1>File Browser</h1>";
  html += "<button onclick=\"create_directory()\">New Folder</button>";
  html += "</div>";

  html += this->generate_breadcrumb(path);

  // Upload form
  if (this->upload_enabled_) {
    html += R"HTML(<div class="upload-form">
      <form id="uploadForm" onsubmit="return handleUpload(event);">
        <input type="file" name="file" id="uploadFile" required>
        <button type="submit">Upload</button>
      </form>
    </div>)HTML";
  }

  // File table with Size column (READDIRPLUS provides file attributes)
  html += R"(<table>
    <thead>
      <tr>
        <th>Name</th>
        <th>Size</th>
        <th>Type</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>)";

  // Generate rows for network storage entries
  for (const auto &entry : entries) {
    std::string entry_path = Path::join(path, entry.name);
    std::string file_uri = this->url_prefix_ + entry_path;

    html += "<tr>";

    // Name column
    html += "<td>";
    if (entry.is_directory) {
      html += "<a href=\"" + file_uri + "\" class=\"folder\">" + entry.name + "</a>";
    } else {
      html += "<span class=\"file\">" + entry.name + "</span>";
    }
    html += "</td>";

    // Size column
    html += "<td>";
    if (!entry.is_directory) {
      if (entry.size < 1024) {
        html += std::to_string(entry.size) + " B";
      } else if (entry.size < 1024 * 1024) {
        html += std::to_string(entry.size / 1024) + " KB";
      } else {
        html += std::to_string(entry.size / (1024 * 1024)) + " MB";
      }
    }
    html += "</td>";

    // Type column
    html += "<td>";
    if (entry.is_directory) {
      html += "<span class=\"file-type\">Folder</span>";
    } else {
      html += "<span class=\"file-type\">" + Path::file_type(entry.name) + "</span>";
    }
    html += "</td>";

    // Actions column
    html += "<td class=\"actions\">";
    if (!entry.is_directory) {
      if (this->download_enabled_) {
        html += "<button onclick=\"download_file('" + file_uri + "', '" + entry.name + "')\">Download</button>";
      }
    }
    html += "<button onclick=\"copy_file('" + entry_path + "')\">Copy</button>";
    html += "<button onclick=\"move_file('" + entry_path + "')\">Move</button>";
    html += "<button onclick=\"rename_file('" + entry_path + "')\">Rename</button>";
    html += "<button onclick=\"show_file_info('" + entry_path + "')\">Info</button>";
    if (this->deletion_enabled_) {
      if (entry.is_directory) {
        html += "<button onclick=\"delete_directory('" + entry_path + "')\">Delete</button>";
      } else {
        html += "<button onclick=\"delete_file('" + entry_path + "')\">Delete</button>";
      }
    }
    html += "</td>";

    html += "</tr>";
  }

  html += "</tbody></table>";
  html += this->generate_html_footer();

  // Send response
  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", html);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  request->send(response);
  return true;
}

bool HttpFileBrowser::handle_network_file_download(AsyncWebServerRequest *request, storage::NetworkStorage *net_storage,
                                                   const std::string &path) {
  // Strip mount point prefix
  std::string relative_path = strip_network_mount_prefix(net_storage, path);

  // Get file size
  struct stat file_stat;
  if (!net_storage->stat(relative_path, file_stat)) {
    ESP_LOGW(TAG, "Network storage file stat failed for: %s (relative: %s)", path.c_str(), relative_path.c_str());
    request->send(404, "text/plain", "Not Found");
    return false;
  }
  size_t file_size = file_stat.st_size;
  ESP_LOGD(TAG, "Network storage file size for %s: %" PRIu32 " bytes", path.c_str(), (uint32_t) file_size);

  // For now, read entire file into memory
  // TODO: Implement streaming with AsyncResponseStream when web_server_idf supports it
  std::vector<uint8_t> file_data;
  if (!net_storage->read_file(relative_path, file_data)) {
    ESP_LOGW(TAG, "Network storage read failed for: %s (relative: %s)", path.c_str(), relative_path.c_str());
    request->send(500, "text/plain", "Internal Server Error: Failed to read file");
    return false;
  }

  // Send file from memory
  AsyncWebServerResponse *response =
      request->beginResponse(200, "application/octet-stream", file_data.data(), file_data.size());
  std::string content_disposition = "attachment; filename=\"" + get_filename_from_path(path) + "\"";
  response->addHeader("Content-Disposition", content_disposition.c_str());
  request->send(response);
  return true;
}

void HttpFileBrowser::handleUpload(AsyncWebServerRequest *request, const PlatformString &filename, size_t index,
                                   uint8_t *data, size_t len, bool final) {
  // Only log at milestones to avoid flooding logs with hundreds of tiny chunk calls
  if (index == 0 || final || (index % 1048576 == 0)) {  // Log at start, end, and every 1MB
    ESP_LOGD(TAG, "handleUpload: filename='%s', index=%" PRIu32 ", len=%" PRIu32 ", final=%d", filename.c_str(), (uint32_t) index, (uint32_t) len, final);
  }

  // Check authentication if enabled
  if (this->auth_enabled_) {
    if (!request->authenticate(this->username_.c_str(), this->password_.c_str())) {
      ESP_LOGW(TAG, "Upload authentication failed");
      return;
    }
  }

  if (!this->upload_enabled_) {
    ESP_LOGW(TAG, "Upload disabled in configuration");
    if (final) {
      request->send(403, "text/plain", "File upload is disabled");
    }
    return;
  }

  // Get upload directory from request URL
  if (index == 0) {
    // Clean up any stale upload state from previous failed upload
    if (this->upload_file_) {
      ESP_LOGW(TAG, "Closing stale upload file from previous upload");
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
    }

    // Clear chunk buffer if it was used for network storage
    if (this->chunk_buffer_) {
      free(this->chunk_buffer_);
      this->chunk_buffer_ = nullptr;
      this->chunk_buffer_size_ = 0;
    }

    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    StringRef uri = request->url_to(url_buf);
    ESP_LOGI(TAG, "Upload started: index=0, uri='%s', filename='%s'", uri.c_str(), filename.c_str());

    this->upload_directory_ = this->uri_to_filepath(uri);
    this->upload_filename_ = filename.c_str();

    ESP_LOGD(TAG, "Upload directory: %s", this->upload_directory_.c_str());

    // Build full upload path
    std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);

    // Check if this is network storage
    storage::NetworkStorage *net_storage = nullptr;
    if (this->storage_ != nullptr) {
      net_storage = this->storage_->find_network_storage_for_path(upload_path);
    }

    if (net_storage != nullptr) {
      // Network storage - accumulate data in memory buffer
      ESP_LOGI(TAG, "Starting network storage upload: %s", upload_path.c_str());

      // Check if directory exists (network storage doesn't support stat, so we'll trust it)
      // The actual directory check will happen during list_directory call if needed

      // Get expected file size from query parameter (if provided by JavaScript)
      size_t expected_size = 0;
      auto *filesize_param = request->getParam("filesize");
      if (filesize_param) {
        expected_size = std::stoul(filesize_param->value().c_str());
        ESP_LOGI(TAG, "Expected upload size from query param: %" PRIu32 " bytes (network storage)", (uint32_t) expected_size);
      } else {
        ESP_LOGW(TAG, "No filesize query parameter found - network storage requires known file size");
      }

      // Initialize progress tracking (thread-safe)
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.operation = "upload";
      this->progress_.source = filename.c_str();
      this->progress_.destination = upload_path;
      this->progress_.total_bytes = expected_size;
      this->progress_.transferred_bytes = 0;
      this->progress_.in_progress = true;
      this->progress_.cancelled = false;
      this->progress_.start_time = millis();
      portEXIT_CRITICAL(&this->progress_mutex_);

      // Reset yield tracking
      this->upload_bytes_since_yield_ = 0;

      // Allocate buffer for network storage upload (will accumulate all chunks)
      // Use PSRAM if available for better performance
      this->chunk_buffer_size_ = expected_size > 0 ? expected_size : FILE_BUFFER_SIZE;

      // Free old buffer if it exists
      if (this->chunk_buffer_) {
        free(this->chunk_buffer_);
      }

#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
      this->chunk_buffer_ = (uint8_t *) heap_caps_malloc(this->chunk_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (this->chunk_buffer_ != nullptr) {
        ESP_LOGD(TAG, "Allocated %" PRIu32 " byte upload buffer from PSRAM", (uint32_t) this->chunk_buffer_size_);
      } else {
        ESP_LOGW(TAG, "PSRAM allocation failed for %" PRIu32 " bytes, using heap", (uint32_t) this->chunk_buffer_size_);
        this->chunk_buffer_ = (uint8_t *) malloc(this->chunk_buffer_size_);
      }
#else
      this->chunk_buffer_ = (uint8_t *) malloc(this->chunk_buffer_size_);
      ESP_LOGD(TAG, "Allocated %" PRIu32 " byte upload buffer from heap", (uint32_t) this->chunk_buffer_size_);
#endif

      if (this->chunk_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %" PRIu32 " bytes for network upload!", (uint32_t) this->chunk_buffer_size_);
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
        request->send(500, "text/plain", "Failed to allocate upload buffer");
        return;
      }

      // upload_file_ remains null - signals network storage mode
      this->upload_file_ = nullptr;

      return;
    }

    // Local storage - check if target is a directory
    struct stat file_stat;
    if (stat(this->upload_directory_.c_str(), &file_stat) != 0 || !S_ISDIR(file_stat.st_mode)) {
      ESP_LOGE(TAG, "Upload target is not a directory: %s (errno: %d)", this->upload_directory_.c_str(), errno);
      if (final) {
        request->send(400, "text/plain", "Upload target must be a directory");
      }
      return;
    }

    ESP_LOGI(TAG, "Starting local storage upload: %s", upload_path.c_str());

    // Get expected file size from query parameter (if provided by JavaScript)
    size_t expected_size = 0;
    auto *filesize_param = request->getParam("filesize");
    if (filesize_param) {
      expected_size = std::stoul(filesize_param->value().c_str());
      ESP_LOGI(TAG, "Expected upload size from query param: %" PRIu32 " bytes", (uint32_t) expected_size);
    } else {
      ESP_LOGW(TAG, "No filesize query parameter found in upload request");
    }

    // Initialize progress tracking (thread-safe)
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.operation = "upload";
    this->progress_.source = filename.c_str();
    this->progress_.destination = upload_path;
    this->progress_.total_bytes = expected_size;  // Use expected size from JavaScript
    this->progress_.transferred_bytes = 0;
    this->progress_.in_progress = true;
    this->progress_.cancelled = false;
    this->progress_.start_time = millis();
    portEXIT_CRITICAL(&this->progress_mutex_);

    // Reset yield tracking for upload
    this->upload_bytes_since_yield_ = 0;

    this->upload_file_ = fopen(upload_path.c_str(), "wb");
    if (!this->upload_file_) {
      ESP_LOGE(TAG, "Failed to open file for async upload: %s", upload_path.c_str());
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;  // Reset cancelled flag after handling
      portEXIT_CRITICAL(&this->progress_mutex_);
      if (final) {
        request->send(500, "text/plain", "Failed to create file");
      }
      return;
    }
  }

  // Check for cancellation (thread-safe)
  portENTER_CRITICAL(&this->progress_mutex_);
  bool cancelled = this->progress_.cancelled;
  size_t current_transferred = this->progress_.transferred_bytes;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (cancelled) {
    ESP_LOGI(TAG, "Upload cancelled by user");

    if (this->upload_file_) {
      // Local storage cleanup
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;

      // Delete partial file
      std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);
      if (remove(upload_path.c_str()) == 0) {
        ESP_LOGI(TAG, "Deleted partial upload file: %s", upload_path.c_str());
      } else {
        ESP_LOGE(TAG, "Failed to delete partial upload file: %s (errno: %d)", upload_path.c_str(), errno);
      }
    } else if (this->chunk_buffer_) {
      // Network storage cleanup
      ESP_LOGI(TAG, "Clearing network storage upload buffer");
      free(this->chunk_buffer_);
      this->chunk_buffer_ = nullptr;
      this->chunk_buffer_size_ = 0;
    }

    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;  // Reset cancelled flag after handling
    portEXIT_CRITICAL(&this->progress_mutex_);

    if (final) {
      request->send(499, "text/plain", "Upload cancelled");
    }
    return;
  }

  // Handle data chunk
  if (len > 0) {
    if (this->upload_file_) {
      // Local storage - write directly to file
      size_t written = fwrite(data, 1, len, this->upload_file_);
      if (written != len) {
        ESP_LOGE(TAG, "Failed to write async upload data");
        fclose(this->upload_file_);
        this->upload_file_ = nullptr;

        // Delete partial file on write failure
        std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);
        if (remove(upload_path.c_str()) == 0) {
          ESP_LOGI(TAG, "Deleted partial upload file after write failure: %s", upload_path.c_str());
        } else {
          ESP_LOGE(TAG, "Failed to delete partial upload file: %s (errno: %d)", upload_path.c_str(), errno);
        }

        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;  // Reset cancelled flag after handling
        portEXIT_CRITICAL(&this->progress_mutex_);
        if (final) {
          request->send(500, "text/plain", "Failed to write file");
        }
        return;
      }

      // Update progress (thread-safe)
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.in_progress) {
        this->progress_.transferred_bytes += written;
      }
      portEXIT_CRITICAL(&this->progress_mutex_);

      // Yield CPU periodically to allow web server to handle other requests (like progress API)
      // Longer yield time (100ms) but less frequent (every 128KB instead of 16KB)
      this->upload_bytes_since_yield_ += written;
      static constexpr size_t YIELD_INTERVAL_BYTES = 128 * 1024;  // 128KB between yields
      if (this->upload_bytes_since_yield_ >= YIELD_INTERVAL_BYTES) {
        vTaskDelay(10);  // Yield for ~100ms to give httpd time to send progress responses
        this->upload_bytes_since_yield_ = 0;
      }
    } else if (this->chunk_buffer_) {
      // Network storage - accumulate data in buffer
      // Check if we have enough space in buffer
      if (current_transferred + len > this->chunk_buffer_size_) {
        ESP_LOGE(TAG, "Network storage upload buffer overflow: %" PRIu32 " + %" PRIu32 " > %" PRIu32, (uint32_t) current_transferred, (uint32_t) len,
                 this->chunk_buffer_size_);

        // Clean up
        free(this->chunk_buffer_);
        this->chunk_buffer_ = nullptr;
        this->chunk_buffer_size_ = 0;

        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
        portEXIT_CRITICAL(&this->progress_mutex_);

        if (final) {
          request->send(500, "text/plain", "Upload buffer overflow");
        }
        return;
      }

      // Copy data to buffer
      std::memcpy(this->chunk_buffer_ + current_transferred, data, len);

      // Update progress (thread-safe)
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.in_progress) {
        this->progress_.transferred_bytes += len;
      }
      portEXIT_CRITICAL(&this->progress_mutex_);

      // Yield CPU periodically
      this->upload_bytes_since_yield_ += len;
      static constexpr size_t YIELD_INTERVAL_BYTES = 128 * 1024;  // 128KB between yields
      if (this->upload_bytes_since_yield_ >= YIELD_INTERVAL_BYTES) {
        vTaskDelay(10);  // Yield for ~100ms to give httpd time to send progress responses
        this->upload_bytes_since_yield_ = 0;
      }
    } else {
      // No file handle and no chunk buffer - this shouldn't happen
      ESP_LOGE(TAG, "Upload state corrupted: no file handle or chunk buffer");
      if (final) {
        request->send(500, "text/plain", "Upload state corrupted");
      }
      return;
    }
  }

  // Finalize upload
  if (final) {
    if (this->upload_file_) {
      // Local storage - close file
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
      ESP_LOGI(TAG, "Local storage upload completed: %s", this->upload_filename_.c_str());

      // Mark progress as complete (thread-safe)
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;  // Reset cancelled flag after handling
      portEXIT_CRITICAL(&this->progress_mutex_);

      request->send(201, "text/plain", "File uploaded successfully");
    } else if (this->chunk_buffer_) {
      // Network storage - write accumulated data
      std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);

      // Get current transferred size for final write
      portENTER_CRITICAL(&this->progress_mutex_);
      size_t final_size = this->progress_.transferred_bytes;
      portEXIT_CRITICAL(&this->progress_mutex_);

      ESP_LOGI(TAG, "Writing %" PRIu32 " bytes to network storage: %s", (uint32_t) final_size, upload_path.c_str());

      // Get network storage for path
      storage::NetworkStorage *net_storage = nullptr;
      if (this->storage_ != nullptr) {
        net_storage = this->storage_->find_network_storage_for_path(upload_path);
      }

      bool success = false;
      if (net_storage != nullptr) {
        std::string relative_path = strip_network_mount_prefix(net_storage, upload_path);
        success = net_storage->write_file(relative_path, this->chunk_buffer_, final_size);
      } else {
        ESP_LOGE(TAG, "Network storage not found for path: %s", upload_path.c_str());
      }

      // Clean up buffer
      free(this->chunk_buffer_);
      this->chunk_buffer_ = nullptr;
      this->chunk_buffer_size_ = 0;

      // Mark progress as complete
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;
      portEXIT_CRITICAL(&this->progress_mutex_);

      if (success) {
        ESP_LOGI(TAG, "Network storage upload completed: %s (%" PRIu32 " bytes)", upload_path.c_str(), (uint32_t) final_size);
        request->send(201, "text/plain", "File uploaded successfully");
      } else {
        ESP_LOGE(TAG, "Network storage upload failed: %s", upload_path.c_str());
        request->send(500, "text/plain", "Failed to write file to network storage");
      }
    } else {
      // No file handle and no chunk buffer
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;  // Reset cancelled flag after handling
      portEXIT_CRITICAL(&this->progress_mutex_);
      request->send(500, "text/plain", "Upload failed");
    }
  }
}

void HttpFileBrowser::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                 size_t total) {
  // Check authentication if enabled
  if (this->auth_enabled_) {
    if (!request->authenticate(this->username_.c_str(), this->password_.c_str())) {
      return;
    }
  }

  // Accumulate body data in buffer
  if (index == 0) {
    ESP_LOGD(TAG, "handleBody called: total=%" PRIu32 " bytes", (uint32_t) total);
    this->body_buffer_.clear();
    this->body_buffer_.reserve(total);
  }

  this->body_buffer_.append(reinterpret_cast<char *>(data), len);
  ESP_LOGV(TAG, "handleBody: appended %" PRIu32 " bytes (total now: %" PRIu32 "/%" PRIu32 ")", (uint32_t) len, (uint32_t) this->body_buffer_.size(), (uint32_t) total);
}

// Helper methods
std::string HttpFileBrowser::uri_to_filepath(const std::string &uri) {
  // Decode URL first
  std::string decoded_uri = url_decode(uri);
  ESP_LOGD(TAG, "URI: %s -> Decoded: %s", uri.c_str(), decoded_uri.c_str());

  // Remove URL prefix from URI to get relative path
  std::string relative_path = decoded_uri;

  if (decoded_uri.find(this->url_prefix_) == 0) {
    relative_path = decoded_uri.substr(this->url_prefix_.length());
  }

  // Remove leading slash if present
  if (!relative_path.empty() && relative_path[0] == '/') {
    relative_path = relative_path.substr(1);
  }

  // Handle empty path (root access) - return root_path as-is
  if (relative_path.empty()) {
    ESP_LOGD(TAG, "Root access: %s", this->root_path_.c_str());
    return this->root_path_;
  }

  // Construct full path
  std::string full_path = this->root_path_;
  if (!this->root_path_.empty() && this->root_path_.back() != '/') {
    full_path += "/";
  }
  full_path += relative_path;

  ESP_LOGD(TAG, "Mapped URI %s to path %s", uri.c_str(), full_path.c_str());

  return full_path;
}

std::string HttpFileBrowser::url_decode(const std::string &src) {
  std::string result;
  result.reserve(src.length());

  const char *str = src.c_str();
  int i = 0;
  char ch;
  int j;

  while (str[i]) {
    if (str[i] == '%' && str[i + 1] && str[i + 2]) {
      if (sscanf(str + i + 1, "%2x", &j) == 1) {
        ch = static_cast<char>(j);
        result += ch;
        i += 3;
      } else {
        result += str[i++];
      }
    } else if (str[i] == '+') {
      result += ' ';
      i++;
    } else {
      result += str[i++];
    }
  }

  return result;
}

esphome::FixedVector<FileInfo> HttpFileBrowser::list_directory(const std::string &path) {
  esphome::FixedVector<FileInfo> files;

  ESP_LOGD(TAG, "Attempting to open directory: %s", path.c_str());

  // Check if this path is on network storage
  if (this->storage_ != nullptr) {
    auto *net_storage = this->storage_->find_network_storage_for_path(path);
    if (net_storage != nullptr) {
      ESP_LOGD(TAG, "Path %s is on network storage, using NetworkStorage API", path.c_str());

      // Use NetworkStorage API instead of VFS
      std::vector<storage::NetworkStorage::DirEntry> entries;
      if (net_storage->list_directory(path, entries)) {
        files.init(std::min(entries.size(), MAX_DIR_ENTRIES));

        for (const auto &entry : entries) {
          if (files.size() >= MAX_DIR_ENTRIES) {
            ESP_LOGW(TAG, "Directory %s has more than %" PRIu32 " entries, truncating", path.c_str(), (uint32_t) MAX_DIR_ENTRIES);
            break;
          }

          FileInfo info;
          info.name = entry.name;
          info.path = Path::join(path, entry.name);
          info.is_directory = entry.is_directory;
          info.size = entry.size;
          info.modified = 0;  // Network storage doesn't provide mtime yet

          files.push_back(info);
        }
      } else {
        ESP_LOGE(TAG, "Failed to list network storage directory: %s", path.c_str());
      }
      return files;
    }
  }

  // Use standard VFS opendir for local storage
  DIR *dir = opendir(path.c_str());

  if (dir != nullptr) {
    ESP_LOGV(TAG, "Directory opened successfully: %s", path.c_str());
    // Count entries first to allocate once
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
        count++;
        if (count >= MAX_DIR_ENTRIES) {
          ESP_LOGW(TAG, "Directory %s has more than %" PRIu32 " entries, truncating", path.c_str(), (uint32_t) MAX_DIR_ENTRIES);
          break;
        }
      }
    }

    // Allocate exact size needed (single allocation)
    files.init(count);

    // Rewind and fill
    rewinddir(dir);
    while ((entry = readdir(dir)) != nullptr && files.size() < count) {
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
        FileInfo info;
        info.name = entry->d_name;
        info.path = Path::join(path, entry->d_name);

        struct stat file_stat;
        if (stat(info.path.c_str(), &file_stat) == 0) {
          info.is_directory = S_ISDIR(file_stat.st_mode);
          info.size = file_stat.st_size;
          info.modified = file_stat.st_mtime;
        } else {
          info.is_directory = false;
          info.size = 0;
          info.modified = 0;
        }

        files.push_back(info);
      }
    }
    closedir(dir);
  } else {
    ESP_LOGE(TAG, "Cannot open directory: %s (errno: %d)", path.c_str(), errno);
  }
  return files;
}

// Network storage aware file operation helpers
bool HttpFileBrowser::path_exists(const std::string &path, bool &is_directory) {
  // Check if this path is on network storage
  if (this->storage_ != nullptr) {
    auto *net_storage = this->storage_->find_network_storage_for_path(path);
    if (net_storage != nullptr) {
      // Network storage - strip mount prefix for NFS operations
      std::string relative_path = strip_network_mount_prefix(net_storage, path);

      // Verify directory exists using NetworkStorage API
      if (net_storage->is_directory(relative_path)) {
        is_directory = true;
        return true;
      } else if (net_storage->file_exists(relative_path)) {
        is_directory = false;
        return true;
      } else {
        return false;
      }
    }
  }

  // Use VFS stat for local storage
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    is_directory = S_ISDIR(st.st_mode);
    return true;
  }
  return false;
}

bool HttpFileBrowser::delete_path(const std::string &path, bool is_directory) {
  // Check if this path is on network storage
  if (this->storage_ != nullptr) {
    auto *net_storage = this->storage_->find_network_storage_for_path(path);
    if (net_storage != nullptr) {
      // Use NetworkStorage API
      std::string relative_path = strip_network_mount_prefix(net_storage, path);
      if (is_directory) {
        return net_storage->delete_directory(relative_path);
      } else {
        return net_storage->delete_file(relative_path);
      }
    }
  }

  // Use VFS for local storage
  if (is_directory) {
    return rmdir(path.c_str()) == 0;
  } else {
    return remove(path.c_str()) == 0;
  }
}

bool HttpFileBrowser::create_dir(const std::string &path) {
  // Check if this path is on network storage
  if (this->storage_ != nullptr) {
    auto *net_storage = this->storage_->find_network_storage_for_path(path);
    if (net_storage != nullptr) {
      // Use NetworkStorage API
      std::string relative_path = strip_network_mount_prefix(net_storage, path);
      return net_storage->create_directory(relative_path);
    }
  }

  // Use VFS for local storage
  return mkdir(path.c_str(), 0777) == 0;
}

bool HttpFileBrowser::read_file_content(const std::string &path, std::vector<uint8_t> &data) {
  // Check if this path is on network storage
  if (this->storage_ != nullptr) {
    auto *net_storage = this->storage_->find_network_storage_for_path(path);
    if (net_storage != nullptr) {
      // Use NetworkStorage API
      return net_storage->read_file(path, data);
    }
  }

  // Use VFS for local storage
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) {
    return false;
  }

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (size < 0) {
    fclose(file);
    return false;
  }

  data.resize(size);
  size_t read = fread(data.data(), 1, size, file);
  fclose(file);

  return read == (size_t) size;
}

bool HttpFileBrowser::write_file_content(const std::string &path, const uint8_t *data, size_t length) {
  // Check if this path is on network storage
  if (this->storage_ != nullptr) {
    auto *net_storage = this->storage_->find_network_storage_for_path(path);
    if (net_storage != nullptr) {
      // Use NetworkStorage API
      return net_storage->write_file(path, data, length);
    }
  }

  // Use VFS for local storage
  FILE *file = fopen(path.c_str(), "wb");
  if (!file) {
    return false;
  }

  size_t written = fwrite(data, 1, length, file);
  fclose(file);

  return written == length;
}

// HTML generation helpers
std::string HttpFileBrowser::generate_html_header(const std::string &title) {
  std::string html = R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
  <title>)";
  html += title;
  html += R"(</title>
  <style>
    body {
      font-family: 'Segoe UI', system-ui, sans-serif;
      margin: 0;
      padding: 2rem;
      background: #f5f5f7;
      color: #1d1d1f;
    }
    h1 {
      color: #0066cc;
      margin-bottom: 1.5rem;
      display: flex;
      align-items: center;
      gap: 1rem;
    }
    .container {
      max-width: 1200px;
      margin: 0 auto;
      background: white;
      border-radius: 12px;
      box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
      padding: 2rem;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 1.5rem;
    }
    th, td {
      padding: 12px;
      text-align: left;
      border-bottom: 1px solid #e0e0e0;
    }
    th {
      background: #f8f9fa;
      font-weight: 500;
    }
    .file-actions {
      display: flex;
      gap: 8px;
    }
    button {
      padding: 6px 12px;
      border: none;
      border-radius: 6px;
      background: #0066cc;
      color: white;
      cursor: pointer;
      transition: background 0.2s;
    }
    button:hover {
      background: #0052a3;
    }
    button.delete {
      background: #dc3545;
    }
    button.delete:hover {
      background: #c82333;
    }
    .upload-form {
      margin-bottom: 2rem;
      padding: 1rem;
      background: #f8f9fa;
      border-radius: 8px;
    }
    .upload-form input[type="file"] {
      margin-right: 1rem;
    }
    .breadcrumb {
      margin-bottom: 1.5rem;
      font-size: 0.9rem;
      color: #666;
    }
    .breadcrumb a {
      color: #0066cc;
      text-decoration: none;
    }
    .breadcrumb a:hover {
      text-decoration: underline;
    }
    .breadcrumb span:not(:last-child)::after {
      display: inline-block;
      margin: 0 .25rem;
      content: ">";
    }
    .folder {
      color: #0066cc;
      font-weight: 500;
    }
    .file-type {
      color: #666;
      font-size: 0.9rem;
    }
    .header-actions {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 1rem;
    }
    .header-actions button {
      background: #4CAF50;
    }
    .header-actions button:hover {
      background: #45a049;
    }
    .progress-modal {
      display: none;
      position: fixed;
      top: 0;
      left: 0;
      width: 100%;
      height: 100%;
      background: rgba(0, 0, 0, 0.5);
      z-index: 1000;
      align-items: center;
      justify-content: center;
    }
    .progress-modal.active {
      display: flex;
    }
    .progress-content {
      background: white;
      padding: 2rem;
      border-radius: 12px;
      min-width: 400px;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.2);
    }
    .progress-title {
      font-size: 1.2rem;
      font-weight: 500;
      margin-bottom: 1rem;
      color: #1d1d1f;
    }
    .progress-bar-container {
      width: 100%;
      height: 20px;
      background: #e0e0e0;
      border-radius: 10px;
      overflow: hidden;
      margin: 1rem 0;
    }
    .progress-bar {
      height: 100%;
      background: linear-gradient(90deg, #0066cc, #0052a3);
      width: 0%;
      transition: width 0.3s ease;
      display: flex;
      align-items: center;
      justify-content: center;
      color: white;
      font-size: 0.8rem;
      font-weight: 500;
    }
    .progress-details {
      color: #666;
      font-size: 0.9rem;
      margin-top: 0.5rem;
    }
    .progress-file-info {
      margin-top: 1rem;
      padding: 1rem;
      background: #f8f9fa;
      border-radius: 8px;
      font-size: 0.85rem;
      word-break: break-all;
    }
    .progress-speed {
      margin-top: 0.5rem;
      font-size: 0.9rem;
      color: #666;
    }
    #cancelBtn {
      margin-top: 1rem;
      width: 100%;
    }
  </style>
</head>
<body>
<div class="container">
)";
  return html;
}

std::string HttpFileBrowser::generate_html_footer() {
  return R"HTML(
</div>
<div id="progressModal" class="progress-modal">
  <div class="progress-content">
    <div class="progress-title" id="progressTitle">Processing...</div>
    <div class="progress-bar-container">
      <div class="progress-bar" id="progressBar">0%</div>
    </div>
    <div class="progress-details" id="progressDetails">Initializing...</div>
    <div class="progress-speed" id="progressSpeed"></div>
    <div class="progress-file-info">
      <div><strong>From:</strong> <span id="progressSource">-</span></div>
      <div><strong>To:</strong> <span id="progressDest">-</span></div>
    </div>
    <button id="cancelBtn" class="delete" onclick="cancelOperation()" > Cancel</ button></ div></ div><script>
             // API base path for this file server instance
             const API_BASE = ')HTML" +
         this->url_prefix_ + R"HTML(';
  console.log('[FileServer] Script loaded, API_BASE:', API_BASE);
  let progressPollInterval = null;

  function showProgressModal(operation, source, destination) {
    const modal = document.getElementById('progressModal');
    const title = document.getElementById('progressTitle');
    const bar = document.getElementById('progressBar');
    const details = document.getElementById('progressDetails');
    const sourceEl = document.getElementById('progressSource');
    const destEl = document.getElementById('progressDest');

    // Set title based on operation type
    if (operation === 'copy') {
      title.textContent = 'Copying File...';
    } else if (operation === 'move') {
      title.textContent = 'Moving File...';
    } else if (operation === 'upload') {
      title.textContent = 'Uploading File...';
    } else {
      title.textContent = 'Processing...';
    }

    bar.style.width = '0%';
    bar.textContent = '0%';
    details.textContent = 'Starting operation...';
    sourceEl.textContent = source;

    // Strip URL prefix from destination if present
    let cleanDest = destination;
    if (cleanDest.startsWith(API_BASE)) {
      cleanDest = cleanDest.substring(API_BASE.length);
    }
    destEl.textContent = cleanDest;

    modal.classList.add('active');
  }

  function hideProgressModal() {
    const modal = document.getElementById('progressModal');
    modal.classList.remove('active');
    if (progressPollInterval) {
      clearInterval(progressPollInterval);
      progressPollInterval = null;
    }
  }

  function formatBytes(bytes) {
    if (bytes === 0)
      return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return (bytes / Math.pow(k, i)).toFixed(1) + ' ' + sizes[i];
  }

  function formatTime(ms) {
    if (ms < 1000)
      return ms + 'ms';
    const seconds = Math.floor(ms / 1000);
    if (seconds < 60)
      return seconds + 's';
    const minutes = Math.floor(seconds / 60);
    const remainingSeconds = seconds % 60;
    return minutes + 'm ' + remainingSeconds + 's';
  }

  let progressPollCount = 0;
  let hasSeenProgress = false;
  let consecutiveTimeouts = 0;
  let lastTransferredBytes = 0;
  let lastProcessedItems = 0;
  let stallCount = 0;  // Count polls with no progress change

  function pollProgress() {
    console.log('[FileServer] pollProgress() START, count=' + progressPollCount);
    progressPollCount++;

    const url = API_BASE + '/api/progress';
    console.log('[FileServer] Polling:', url);

    // Use XMLHttpRequest instead of fetch for better compatibility with ESP32 web server
    const xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.withCredentials = true;  // Include authentication credentials
    xhr.timeout = 5000;          // 5 second timeout

    xhr.onload = function() {
      console.log('[FileServer] XHR onload, status:', xhr.status, 'response:', xhr.responseText);

      // Reset timeout counter on successful response
      consecutiveTimeouts = 0;

      if (xhr.status !== 200) {
        console.error('[FileServer] Bad status:', xhr.status, 'text:', xhr.responseText);
        consecutiveTimeouts++;
        checkTimeoutLimit();
        return;
      }

      let data;
      try {
        data = JSON.parse(xhr.responseText);
      } catch (e) {
        console.error('[FileServer] JSON parse error:', e, 'text:', xhr.responseText);
        return;
      }

      console.log('[FileServer] Progress update #' + progressPollCount + ':', data);

      if (!data.in_progress) {
        // Only close if we've seen progress before, or after 10 polls
        if (hasSeenProgress || progressPollCount > 10) {
          console.log('[FileServer] Operation complete, closing modal');
          hideProgressModal();
          location.reload();
        } else {
          console.log('[FileServer] Waiting for operation to start...');
        }
        return;
      }

      // Mark that we've seen progress
      hasSeenProgress = true;

      // Check if progress is actually advancing (not stalled)
      let progressMade = false;
      if (data.operation === 'delete') {
        progressMade = (data.processed_items > lastProcessedItems);
        lastProcessedItems = data.processed_items;
      } else {
        progressMade = (data.transferred_bytes > lastTransferredBytes);
        lastTransferredBytes = data.transferred_bytes;
      }

      if (progressMade) {
        stallCount = 0;  // Reset stall counter when progress is made
      } else {
        stallCount++;
        console.log('[FileServer] No progress detected, stall count:', stallCount);
        // Only abort if stalled for 5 consecutive polls (10 seconds with no progress at 2s interval)
        if (stallCount >= 5) {
          console.warn('[FileServer] Operation appears stalled, calling cancel and closing modal');
          // Call cancel API to clean up partial files and reset backend state
          fetch(API_BASE + '/api/cancel', {method: 'POST'})
              .then(() => {
                console.log('[FileServer] Cancel request sent');
              })
              .catch(error => {
                console.error('[FileServer] Cancel request failed:', error);
              });
          hideProgressModal();
          alert('Operation appears to have stalled and has been cancelled. Please refresh the page.');
          return;
        }
      }

      const bar = document.getElementById('progressBar');
      const details = document.getElementById('progressDetails');
      const speed = document.getElementById('progressSpeed');

      const percentage = data.percentage || 0;
      bar.style.width = percentage + '%';
      bar.textContent = percentage.toFixed(1) + '%';

      let detailText = '';
      if (data.operation === 'delete') {
        // For delete operations, show item count and current file
        detailText = data.processed_items + ' / ' + data.total_items + ' items';
        if (data.elapsed_ms) {
          detailText += ' • Elapsed: ' + formatTime(data.elapsed_ms);
        }
        // Show current file being deleted
        if (data.current_file) {
          speed.textContent = 'Deleting: ' + data.current_file;
        } else {
          speed.textContent = '';
        }
      } else {
        // For byte-based operations (copy, move, upload)
        detailText = formatBytes(data.transferred_bytes) + ' / ' + formatBytes(data.total_bytes);
        if (data.elapsed_ms) {
          detailText += ' • Elapsed: ' + formatTime(data.elapsed_ms);
        }
        if (data.remaining_ms) {
          detailText += ' • Remaining: ' + formatTime(data.remaining_ms);
        }
        // Display average speed
        if (data.avg_speed && data.avg_speed > 0) {
          speed.textContent = 'Average speed: ' + formatBytes(data.avg_speed) + '/s';
        } else {
          speed.textContent = '';
        }
      }

      details.textContent = detailText;

      console.log('[FileServer] Updated progress bar:', percentage.toFixed(1) + '%');
    };

    xhr.onerror = function() {
      console.error('[FileServer] XHR network error, status:', xhr.status, 'readyState:', xhr.readyState);
      consecutiveTimeouts++;
      checkTimeoutLimit();
    };

    xhr.ontimeout = function() {
      console.error('[FileServer] XHR timeout after 5s (consecutive: ' + (consecutiveTimeouts + 1) + ')');
      consecutiveTimeouts++;
      checkTimeoutLimit();
    };

    xhr.onreadystatechange = function() {
      console.log('[FileServer] XHR readyState changed to:', xhr.readyState, 'status:', xhr.status);
    };

    try {
      xhr.send();
      console.log('[FileServer] XHR sent successfully');
    } catch (e) {
      console.error('[FileServer] XHR send failed:', e);
    }
  }

  function checkTimeoutLimit() {
    // Close modal after 5 consecutive timeouts (25 seconds of no responses)
    // This handles the case where upload is blocking the server from responding
    if (consecutiveTimeouts >= 5) {
      console.warn('[FileServer] Too many consecutive timeouts (' + consecutiveTimeouts + '), closing modal');
      alert(
          'Progress tracking unavailable - the operation may still be running in the background. Please refresh the page in a few moments.');
      hideProgressModal();
    }
  }

  function startProgressPolling() {
    console.log('[FileServer] startProgressPolling called');
    // Reset progress tracking
    progressPollCount = 0;
    hasSeenProgress = false;
    consecutiveTimeouts = 0;
    lastTransferredBytes = 0;
    lastProcessedItems = 0;
    stallCount = 0;

    if (progressPollInterval) {
      clearInterval(progressPollInterval);
    }
    // Poll every 2 seconds (reduced from 500ms to minimize httpd socket/connection load)
    progressPollInterval = setInterval(pollProgress, 2000);
    console.log('[FileServer] Set interval ID:', progressPollInterval);
    // First poll after 500ms delay to give backend time to start tracking
    setTimeout(pollProgress, 500);
    console.log('[FileServer] Scheduled first poll in 500ms');
  }

  function cancelOperation() {
    if (!confirm('Are you sure you want to cancel the current operation?')) {
      return;
    }

    fetch(API_BASE + '/api/cancel', {method: 'POST', headers: {'Content-Type': 'application/json'}})
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         alert('Operation cancelled');
                         hideProgressModal();
                         location.reload();
                       } else {
                         alert(data.message || 'No operation in progress');
                       }
                     })
        .catch(error => { alert('Error cancelling operation: ' + error); });
  }

  function delete_file(path) {
    if (confirm('Are you sure you want to delete this file?')) {
      fetch(API_BASE + '/api/delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'path=' + encodeURIComponent(path)
      })
          .then(response =>
                           {
                             if (!response.ok) {
                               return response.text().then(
                                   text => { throw new Error('HTTP ' + response.status + ': ' + text); });
                             }
                             return response.json();
                           })
          .then(data =>
                       {
                         if (data.success) {
                           alert('File deleted successfully!');
                           location.reload();
                         } else {
                           alert('Delete failed: ' + (data.error || 'Unknown error'));
                         }
                       })
          .catch(error => { alert('Error: ' + error); });
    }
  }

  async function delete_directory(path) {
    try {
      // First do lightweight check if directory is empty
      const emptyCheck = await fetch(API_BASE + '/api/dirisempty?path=' + encodeURIComponent(path));
      const emptyInfo = await emptyCheck.json();

      if (emptyInfo.error) {
        alert('Error checking directory: ' + emptyInfo.error);
        return;
      }

      let message;
      if (emptyInfo.is_empty) {
        // Fast path for empty directories
        message = 'Delete empty directory?';
      } else {
        // Only count contents if directory is not empty
        const response = await fetch(API_BASE + '/api/dirinfo?path=' + encodeURIComponent(path));
        const info = await response.json();

        if (info.error) {
          alert('Error checking directory contents: ' + info.error);
          return;
        }

        const dirName = path.split('/').pop();
        message = 'Delete directory "' + dirName + '" and all its contents?\n\n';
        message += 'This will delete:\n';
        if (info.file_count > 0) {
          message += '- ' + info.file_count + ' file' + (info.file_count > 1 ? 's' : '') + '\n';
        }
        if (info.dir_count > 0) {
          message += '- ' + info.dir_count + ' subdirector' + (info.dir_count > 1 ? 'ies' : 'y') + '\n';
        }
        message += '\nThis action cannot be undone!';
      }

      if (!confirm(message)) {
        return;
      }

      // Perform deletion using API endpoint
      fetch(API_BASE + '/api/delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'path=' + encodeURIComponent(path)
      })
          .then(response =>
                           {
                             if (!response.ok) {
                               return response.text().then(
                                   text => { throw new Error('HTTP ' + response.status + ': ' + text); });
                             }
                             return response.json();
                           })
          .then(data =>
                       {
                         if (data.success) {
                           alert('Directory deleted successfully!');
                           location.reload();
                         } else {
                           alert('Delete failed: ' + (data.error || 'Unknown error'));
                         }
                       })
          .catch(error => { alert('Error: ' + error); });
    } catch (error) {
      alert('Error: ' + error);
    }
  }

  function download_file(path, filename) {
    // Direct download - just navigate to the URL
    // The Content-Disposition: attachment header will trigger the browser's save dialog
    window.location.href = path;
  }

  // Helper function to suggest a new filename with (1) appended before extension
  function suggestNewFilename(filepath) {
    const lastSlashIndex = filepath.lastIndexOf('/');
    const path = filepath.substring(0, lastSlashIndex + 1);
    const filename = filepath.substring(lastSlashIndex + 1);

    const lastDotIndex = filename.lastIndexOf('.');
    if (lastDotIndex === -1) {
      // No extension
      return filepath + ' (1)';
    } else {
      const basename = filename.substring(0, lastDotIndex);
      const extension = filename.substring(lastDotIndex);
      return path + basename + ' (1)' + extension;
    }
  }

  // Helper function to check if a file exists
  async function fileExists(filepath) {
    try {
      const response = await fetch(API_BASE + '/api/exists?path=' + encodeURIComponent(filepath));
      const data = await response.json();
      return data.exists;
    } catch (error) {
      console.error('Error checking file existence:', error);
      return false;
    }
  }

  // Helper function to handle file copy with existence check
  async function performCopy(source, destination) {
    console.log('[FileServer] performCopy called:', source, '->', destination);
    showProgressModal('copy', source, destination);
    console.log('[FileServer] Starting progress polling');
    startProgressPolling();

    fetch(API_BASE + '/api/copy', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'source=' + encodeURIComponent(source) + '&destination=' + encodeURIComponent(destination)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         // Task started successfully - polling will handle closing modal when done
                         console.log('[FileServer] Copy task started, polling will track progress');
                       } else {
                         hideProgressModal();
                         alert('Copy failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => {
          hideProgressModal();
          alert('Error: ' + error);
        });
  }

  async function copy_file(source) {
    let destination = prompt('Enter destination path:', source);
    if (!destination)
      return;

    // Check if destination exists
    const exists = await fileExists(destination);
    if (exists) {
      const choice = confirm('File already exists. Click OK to overwrite, or Cancel to change the filename.');
      if (!choice) {
        // User wants to change filename - suggest new name with (1) appended
        const suggested = suggestNewFilename(destination);
        destination = prompt('Enter new destination path:', suggested);
        if (!destination)
          return;
      }
    }

    performCopy(source, destination);
  }

  // Helper function to handle file move with existence check
  async function performMove(source, destination) {
    showProgressModal('move', source, destination);
    startProgressPolling();

    fetch(API_BASE + '/api/move', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'source=' + encodeURIComponent(source) + '&destination=' + encodeURIComponent(destination)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         // Task started successfully - polling will handle closing modal when done
                         console.log('[FileServer] Move task started, polling will track progress');
                       } else {
                         hideProgressModal();
                         alert('Move failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => {
          hideProgressModal();
          alert('Error: ' + error);
        });
  }

  async function move_file(source) {
    let destination = prompt('Enter destination path:', source);
    if (!destination)
      return;

    // Check if destination exists
    const exists = await fileExists(destination);
    if (exists) {
      const choice = confirm('File already exists. Click OK to overwrite, or Cancel to change the filename.');
      if (!choice) {
        // User wants to change filename - suggest new name with (1) appended
        const suggested = suggestNewFilename(destination);
        destination = prompt('Enter new destination path:', suggested);
        if (!destination)
          return;
      }
    }

    performMove(source, destination);
  }
  function rename_file(source) {
    const currentName = source.split('/').pop();
    const newName = prompt('Enter new name:', currentName);
    if (!newName || newName === currentName)
      return;

    fetch(API_BASE + '/api/rename', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'source=' + encodeURIComponent(source) + '&name=' + encodeURIComponent(newName)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         alert('File renamed successfully!');
                         location.reload();
                       } else {
                         alert('Rename failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => alert('Error: ' + error));
  }
  function show_file_info(path) {
    fetch(API_BASE + '/api/fileinfo?path=' + encodeURIComponent(path))
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.error) {
                         alert('Error retrieving file info: ' + data.error);
                         return;
                       }

                       // Format size with appropriate units
                       let sizeStr;
                       if (data.size < 1024) {
                         sizeStr = data.size + ' B';
                       } else if (data.size < 1024 * 1024) {
                         sizeStr = (data.size / 1024).toFixed(1) + ' KB';
                       } else if (data.size < 1024 * 1024 * 1024) {
                         sizeStr = (data.size / (1024 * 1024)).toFixed(1) + ' MB';
                       } else {
                         sizeStr = (data.size / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
                       }

                       let info = 'File Information:\n\n';
                       info += 'Name: ' + data.name + '\n';
                       info += 'Path: ' + data.path + '\n';
                       info += 'Type: ' + (data.is_directory ? 'Directory' : 'File') + '\n';
                       info += 'Size: ' + sizeStr + ' (' + data.size + ' bytes)\n';
                       info += 'Modified: ' + new Date(data.last_modified * 1000).toLocaleString() + '\n';
                       info += 'Permissions: ' + data.mode + '\n';
                       info += 'Owner: ' + data.uid + ':' + data.gid + '\n';
                       info += 'Storage: ' + data.storage_type + '\n';

                       alert(info);
                     })
        .catch(error => alert('Error: ' + error));
  }
  function create_directory() {
    const name = prompt('Enter directory name:');
    if (!name || !name.trim())
      return;

    // Create directory in current location
    let currentPath = window.location.pathname;
    if (currentPath.endsWith('/')) {
      currentPath = currentPath.slice(0, -1);
    }
    const fullPath = currentPath + '/' + name.trim();

    fetch(API_BASE + '/api/mkdir', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'name=' + encodeURIComponent(fullPath)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         alert('Directory created successfully!');
                         location.reload();
                       } else {
                         alert('Create directory failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => alert('Error: ' + error));
  }

  function mount_device(mount_point) {
    if (!confirm('Mount device at ' + mount_point + '?'))
      return;

    fetch(API_BASE + '/api/mount', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'mount_point=' + encodeURIComponent(mount_point)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         alert('Device mounted successfully!');
                         location.reload();
                       } else {
                         alert('Mount failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => alert('Error: ' + error));
  }

  function unmount_device(mount_point) {
    if (!confirm('Unmount device at ' + mount_point + '?'))
      return;

    fetch(API_BASE + '/api/unmount', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'mount_point=' + encodeURIComponent(mount_point)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         alert('Device unmounted successfully!');
                         location.reload();
                       } else {
                         alert('Unmount failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => alert('Error: ' + error));
  }

  function remount_device(mount_point) {
    if (!confirm('Remount device at ' + mount_point + '?'))
      return;

    fetch(API_BASE + '/api/mount', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'mount_point=' + encodeURIComponent(mount_point)
    })
        .then(response => response.json())
        .then(data =>
                     {
                       if (data.success) {
                         alert('Device remounted successfully!');
                         location.reload();
                       } else {
                         alert('Remount failed: ' + (data.error || 'Unknown error'));
                       }
                     })
        .catch(error => alert('Error: ' + error));
  }

  async function handleUpload(event) {
    console.log('[FileServer] handleUpload called, event:', event);
    event.preventDefault();

    const fileInput = document.getElementById('uploadFile');
    const file = fileInput.files[0];

    if (!file) {
      alert('Please select a file');
      return false;
    }

    console.log('[FileServer] File selected:', file.name, 'size:', file.size);
    console.log('[FileServer] File object details:', {
      name: file.name,
      size: file.size,
      type: file.type,
      lastModified: new Date(file.lastModified).toISOString(),
      lastModifiedDate: file.lastModifiedDate ? file.lastModifiedDate.toISOString() : 'N/A'
    });

    // Show progress modal
    showProgressModal('upload', file.name, window.location.pathname);
    startProgressPolling();

    // Prevent default form submission
    event.stopPropagation();

    // Chunked upload configuration
    const CHUNK_SIZE = 256 * 1024; // 256KB chunks for good balance of speed vs responsiveness
    const totalChunks = Math.ceil(file.size / CHUNK_SIZE);

    console.log('[FileServer] Starting chunked upload: ' + totalChunks + ' chunks of ' + CHUNK_SIZE + ' bytes');

    // Stall detection: Track last progress and check if upload has stalled
    let lastChunkCompleteTime = Date.now();
    let lastChunkIndex = -1;
    const STALL_TIMEOUT_MS = 90000; // 90 seconds without completing a chunk = stalled

    // Start stall detection interval
    const stallDetectionInterval = setInterval(() => {
      const timeSinceLastChunk = Date.now() - lastChunkCompleteTime;
      if (timeSinceLastChunk > STALL_TIMEOUT_MS) {
        console.error('[FileServer] Upload stalled: No chunk completed in ' +
                      Math.round(timeSinceLastChunk / 1000) + ' seconds (last: chunk ' +
                      (lastChunkIndex + 1) + ')');
        clearInterval(stallDetectionInterval);
        hideProgressModal();
        alert('Upload stalled: No progress for ' + Math.round(timeSinceLastChunk / 1000) +
              ' seconds. Upload cancelled.');
        location.reload(); // Reload to clean up state
      }
    }, 10000); // Check every 10 seconds

    try {
      // Upload each chunk sequentially
      for (let chunkIndex = 0; chunkIndex < totalChunks; chunkIndex++) {
        const start = chunkIndex * CHUNK_SIZE;
        const end = Math.min(start + CHUNK_SIZE, file.size);
        const chunk = file.slice(start, end);

        // Only log every 50th chunk, first chunk, and last chunk to reduce overhead
        if (chunkIndex % 50 === 0 || chunkIndex === 0 || chunkIndex === totalChunks - 1) {
          console.log('[FileServer] Uploading chunk ' + (chunkIndex + 1) + '/' + totalChunks +
                      ' (bytes ' + start + '-' + end + ')');
        }

        // Build API URL with query parameters
        const apiUrl = API_BASE + '/api/upload_chunk' +
                       '?filename=' + encodeURIComponent(file.name) +
                       '&chunkIndex=' + chunkIndex +
                       '&totalChunks=' + totalChunks +
                       '&path=' + encodeURIComponent(window.location.pathname) +
                       '&fileSize=' + file.size;

        // Log chunk details for debugging (every 50th, first, last, and 170-180 range)
        if (chunkIndex % 50 === 0 || chunkIndex === 0 || chunkIndex === totalChunks - 1 ||
            (chunkIndex >= 170 && chunkIndex <= 180)) {
          console.log('[FileServer] Chunk ' + chunkIndex + ' details: size=' + chunk.size +
                      ', expected=' + (end - start) + ', start=' + start + ', end=' + end);
        }

        // No timeout - support arbitrarily large files (up to filesystem limits)
        try {
          const response = await fetch(apiUrl, {
            method: 'POST',
            body: chunk,
            credentials: 'include',
            headers: {
              'Content-Type': 'application/octet-stream',
              'Connection': 'close'  // Force fresh connection to avoid socket exhaustion
            }
          });

          if (!response.ok) {
            const errorText = await response.text();
            console.error('[FileServer] Chunk ' + (chunkIndex + 1) + ' HTTP error:', {
              status: response.status,
              statusText: response.statusText,
              headers: Object.fromEntries(response.headers.entries()),
              body: errorText
            });
            throw new Error('Chunk ' + (chunkIndex + 1) + ' failed: ' + errorText);
          }

          const result = await response.json();

          // Check if upload was cancelled
          if (result.cancelled) {
            console.log('[FileServer] Upload cancelled by user');
            hideProgressModal();
            alert('Upload cancelled');
            return false;
          }

          if (!result.success) {
            console.error('[FileServer] Chunk ' + (chunkIndex + 1) + ' server rejected:', result);
            throw new Error('Chunk ' + (chunkIndex + 1) + ' upload failed');
          }

          // Check if server thinks upload is complete
          if (result.complete) {
            if (chunkIndex < totalChunks - 1) {
              // Server says complete but we have more chunks - something went wrong
              console.error('[FileServer] Server completed upload early:', {
                chunkIndex: chunkIndex,
                totalChunks: totalChunks,
                expected: totalChunks - 1
              });
              throw new Error('Server completed upload early at chunk ' + (chunkIndex + 1) + '/' + totalChunks);
            }
            console.log('[FileServer] Upload completed by server at final chunk');
          }

          // Log progress sparingly to reduce overhead (every 50th chunk, first, and last)
          if (chunkIndex % 50 === 0 || chunkIndex === 0 || chunkIndex === totalChunks - 1) {
            console.log('[FileServer] Chunk ' + (chunkIndex + 1) + '/' + totalChunks + ' uploaded successfully');
          }

          // Update stall detection - chunk completed successfully
          lastChunkCompleteTime = Date.now();
          lastChunkIndex = chunkIndex;
        } catch (fetchError) {
          // Enhanced error logging
          console.error('[FileServer] Chunk ' + (chunkIndex + 1) + ' fetch error:', {
            chunkIndex: chunkIndex,
            totalChunks: totalChunks,
            chunkSize: chunk.size,
            byteRange: start + '-' + end,
            error: fetchError,
            errorType: fetchError.constructor.name,
            errorMessage: fetchError.message,
            errorStack: fetchError.stack,
            uploadedSoFar: (chunkIndex * CHUNK_SIZE) + ' bytes (' +
                          Math.round((chunkIndex * CHUNK_SIZE) / (1024 * 1024)) + ' MB)'
          });
          throw fetchError;
        }
      }

      console.log('[FileServer] All chunks uploaded successfully');

      // Clear stall detection - upload complete
      clearInterval(stallDetectionInterval);

      // Upload complete - give progress modal a moment to show 100%, then reload
      setTimeout(() => {
        hideProgressModal();
        alert('File uploaded successfully!');
        location.reload();
      }, 500);

    } catch (error) {
      console.error('[FileServer] Upload error:', error);
      // Clear stall detection - upload failed
      clearInterval(stallDetectionInterval);
      hideProgressModal();
      alert('Error: ' + error.message);
    }

    return false;
  }
</script>
</body>
</html>
)HTML";
}

std::string HttpFileBrowser::generate_breadcrumb(const std::string &current_path) {
  std::string breadcrumb = R"(<div class="breadcrumb">)";
  breadcrumb += "<span><a href=\"" + this->url_prefix_ + "\">Home</a></span>";

  std::string relative_path = Path::remove_root_path(current_path, this->root_path_);
  std::vector<std::string> parts = Path::split_path(relative_path);

  std::string accumulated_path = this->url_prefix_;
  for (const std::string &part : parts) {
    if (!part.empty()) {
      accumulated_path = Path::join(accumulated_path, part);
      breadcrumb += "<span><a href=\"" + accumulated_path + "\">" + part + "</a></span>";
    }
  }

  breadcrumb += "</div>";
  return breadcrumb;
}

std::string HttpFileBrowser::generate_file_row(const FileInfo &info, const std::string &uri_prefix) {
  std::string row = "<tr><td>";

  std::string relative_path = Path::remove_root_path(info.path, this->root_path_);
  std::string file_uri = Path::join(uri_prefix, relative_path);

  if (info.is_directory) {
    if (info.is_mount_point && !info.mounted) {
      // Unmounted mount point - show as disabled (not clickable)
      row += "<span class=\"folder unmounted\" style=\"color: #999; cursor: not-allowed;\">" + info.name +
             " (unmounted)</span>";
    } else {
      // Normal directory or mounted mount point
      row += "<a href=\"" + file_uri + "\" class=\"folder\">" + info.name + "</a>";
    }
  } else {
    row += info.name;
  }

  row += "</td><td>";

  if (info.is_directory) {
    if (info.is_mount_point && !info.mounted) {
      row += "<span style=\"color: #999;\">Mount Point</span>";
    } else {
      row += "Folder";
    }
  } else {
    row += "<span class=\"file-type\">" + Path::file_type(info.name) + "</span>";
  }

  row += "</td><td>";

  if (!info.is_directory) {
    // Format file size
    if (info.size < 1024) {
      row += std::to_string(info.size) + " B";
    } else if (info.size < 1024 * 1024) {
      row += std::to_string(info.size / 1024) + " KB";
    } else {
      row += std::to_string(info.size / (1024 * 1024)) + " MB";
    }
  } else if (info.is_mount_point && info.has_space_info) {
    // Show space info for mount points
    uint64_t used = info.total_space - info.free_space;

    // Calculate percentage used
    int percent_used = (info.total_space > 0) ? (used * 100 / info.total_space) : 0;

    // Helper to format bytes with one decimal place
    auto format_size = [](uint64_t bytes) -> std::string {
      if (bytes < 1024ULL * 1024) {
        return std::to_string(bytes / 1024) + " KB";
      } else if (bytes < 1024ULL * 1024 * 1024) {
        uint64_t mb_x10 = (bytes * 10) / (1024 * 1024);
        return std::to_string(mb_x10 / 10) + "." + std::to_string(mb_x10 % 10) + " MB";
      } else {
        uint64_t gb_x10 = (bytes * 10) / (1024ULL * 1024 * 1024);
        return std::to_string(gb_x10 / 10) + "." + std::to_string(gb_x10 % 10) + " GB";
      }
    };

    // Format: "Used / Total (XX%)"
    row += format_size(used) + " / " + format_size(info.total_space);
    row += " (" + std::to_string(percent_used) + "%)";
  }

  row += "</td><td><div class=\"file-actions\">";

  if (!info.is_directory) {
    // Download button
    if (this->download_enabled_) {
      row += "<button onclick=\"download_file('" + file_uri + "', '" + info.name + "')\">Download</button>";
    }
    // Copy button
    row += "<button onclick=\"copy_file('" + info.path + "')\">Copy</button>";
    // Move button
    row += "<button onclick=\"move_file('" + info.path + "')\">Move</button>";
    // Rename button
    row += "<button onclick=\"rename_file('" + info.path + "')\">Rename</button>";
    // Info button
    row += "<button onclick=\"show_file_info('" + info.path + "')\">Info</button>";
    // Delete button
    if (this->deletion_enabled_) {
      row += "<button class=\"delete\" onclick=\"delete_file('" + file_uri + "')\">Delete</button>";
    }
  } else {
    // Directory actions
    if (!info.is_mount_point) {
      // Copy button (not for mount points)
      row += "<button onclick=\"copy_file('" + info.path + "')\">Copy</button>";
      // Move button (not for mount points)
      row += "<button onclick=\"move_file('" + info.path + "')\">Move</button>";
      // Rename button (not for mount points)
      row += "<button onclick=\"rename_file('" + info.path + "')\">Rename</button>";
      // Delete button (not for mount points)
      if (this->deletion_enabled_) {
        row += "<button class=\"delete\" onclick=\"delete_directory('" + file_uri + "')\">Delete</button>";
      }
    } else {
      // Mount point actions
      if (info.mounted) {
        // Show Remount and Unmount for mounted devices
        row += "<button onclick=\"remount_device('" + info.path + "')\">Remount</button>";
        row += "<button onclick=\"unmount_device('" + info.path + "')\">Unmount</button>";
      } else {
        // Show only Mount for unmounted devices
        row += "<button onclick=\"mount_device('" + info.path + "')\">Mount</button>";
      }
    }
  }

  row += "</div></td></tr>";

  return row;
}

// Request handlers
void HttpFileBrowser::handle_directory_listing(AsyncWebServerRequest *request, const std::string &filepath) {
  bool is_virtual_root = (this->root_path_ == "/" && filepath == "/");

  // Generate HTML
  std::string html = this->generate_html_header("File Browser");

  html += "<div class=\"header-actions\">";
  html += "<h1>File Browser</h1>";
  html += "<button onclick=\"create_directory()\">New Folder</button>";
  html += "</div>";

  html += this->generate_breadcrumb(filepath);

  // Upload form (hidden for root directory)
  if (this->upload_enabled_ && !is_virtual_root) {
    html += R"HTML(<div class="upload-form">
      <form id="uploadForm" onsubmit="return handleUpload(event);">
        <input type="file" name="file" id="uploadFile" required>
        <button type="submit">Upload</button>
      </form>
    </div>)HTML";
  }

  // File table
  html += R"(<table>
    <thead>
      <tr>
        <th>Name</th>
        <th>Type</th>
        <th>Size</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>)";

  if (is_virtual_root) {
    // List local mount points from storage
    const auto &mounts = this->storage_->get_mounts();
    ESP_LOGI(TAG, "Virtual root - listing %" PRIu32 " local mount points", (uint32_t) mounts.size());

    for (const auto &mount : mounts) {
      FileInfo info;
      info.name = mount.path.substr(1);  // Remove leading slash
      if (info.name.empty())
        info.name = "root";
      info.path = mount.path;
      info.is_directory = true;
      info.is_mount_point = true;
      info.mounted = this->is_mount_point_mounted(mount.path);  // Check mount status
      info.size = 0;
      info.modified = 0;

      // Try to get space information for mounted devices
      if (info.mounted) {
        uint64_t total = 0, free = 0;
        bool got_space = false;

        if (mount.platform == "sd_mmc" || mount.platform == "usb_msc") {
          // FAT-based filesystems
          got_space = this->get_mount_space_info(mount.path, total, free);
        } else if (mount.space_provider != nullptr) {
          // LittleFS-based mounts (FRAM, EEPROM, SPI flash, etc.)
          uint64_t used = 0;
          if (mount.space_provider->get_space_info(total, used)) {
            free = total - used;
            got_space = true;
          }
        }

        if (got_space) {
          info.total_space = total;
          info.free_space = free;
          info.has_space_info = true;
        }
      }

      html += this->generate_file_row(info, this->url_prefix_);
    }

    // List network storage mount points
    const auto &network_storage = this->storage_->get_network_storage();
    ESP_LOGI(TAG, "Virtual root - listing %" PRIu32 " network storage mount points", (uint32_t) network_storage.size());

    for (const auto *net_storage : network_storage) {
      if (net_storage == nullptr)
        continue;

      FileInfo info;
      const std::string &mount_path = net_storage->get_mount_path();
      info.name = mount_path.substr(1);  // Remove leading slash
      if (info.name.empty())
        info.name = "network";
      info.path = mount_path;
      info.is_directory = true;
      info.is_mount_point = true;
      info.mounted = net_storage->is_connected();  // Use is_connected() for network storage
      info.size = 0;
      info.modified = 0;

      // Try to get space info from network storage
      if (info.mounted) {
        uint64_t total = 0, free = 0;
        // const_cast needed as get_space_info may need to do RPC calls
        if (const_cast<storage::NetworkStorage *>(net_storage)->get_space_info(total, free)) {
          info.total_space = total;
          info.free_space = free;
          info.has_space_info = true;
        }
      }

      html += this->generate_file_row(info, this->url_prefix_);
    }
  } else {
    // Regular directory listing
    // check if filepath is on network storage
    storage::NetworkStorage *net_storage = nullptr;
    if (this->storage_ != nullptr) {
      net_storage = this->storage_->find_network_storage_for_path(filepath);
    }
    if (net_storage != nullptr) {
      ESP_LOGI(TAG, "Listing directory on network storage: %s", filepath.c_str());
      std::vector<esphome::storage::NetworkStorage::DirEntry> entries;
      bool ret = net_storage->list_directory(filepath, entries);
      if (!ret) {
        request->send(500, "text/plain", "Failed to list directory on network storage");
        return;
      }
      ESP_LOGI(TAG, "Found %" PRIu32 " files/folders in %s", (uint32_t) entries.size(), filepath.c_str());
      for (const auto &entry : entries) {
        FileInfo info;
        info.name = entry.name;
        info.path = Path::join(filepath, entry.name);
        info.is_directory = entry.is_directory;
        info.size = entry.size;
        html += this->generate_file_row(info, this->url_prefix_);
      }
    } else {
      // Local storage path
      ESP_LOGI(TAG, "Listing local directory: %s", filepath.c_str());
      esphome::FixedVector<FileInfo> files = this->list_directory(filepath);
      ESP_LOGI(TAG, "Found %" PRIu32 " files/folders in %s", (uint32_t) files.size(), filepath.c_str());
      for (const auto &info : files) {
        html += this->generate_file_row(info, this->url_prefix_);
      }
    }
  }

  html += "</tbody></table>";
  html += this->generate_html_footer();

  // Send response with cache-busting headers to ensure browser gets latest JS
  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", html);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  request->send(response);
}

void HttpFileBrowser::handle_file_download(AsyncWebServerRequest *request, const std::string &filepath) {
  if (!this->download_enabled_) {
    request->send(403, "text/plain", "File download is disabled");
    return;
  }

  // Set content type based on file extension
  std::string mime_type = Path::mime_type(filepath);
  std::string filename = Path::file_name(filepath);
  std::string content_disposition = "attachment; filename=\"" + filename + "\"";

  // Use raw httpd API for streaming (AsyncWebServer wraps httpd but doesn't expose streaming)
  // Get the underlying httpd_req_t
  httpd_req_t *req = static_cast<httpd_req_t *>(*request);

  // Check if this is network storage
  storage::NetworkStorage *net_storage = nullptr;
  if (this->storage_ != nullptr) {
    net_storage = this->storage_->find_network_storage_for_path(filepath);
  }

  if (net_storage != nullptr) {
    // Network storage path - stream using chunked reads (memory efficient)
    ESP_LOGI(TAG, "Starting network storage file download: %s", filename.c_str());

    // Get file size via stat
    std::string relative_path = strip_network_mount_prefix(net_storage, filepath);
    struct stat file_stat;
    if (!net_storage->stat(relative_path, file_stat)) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Failed to get file info from network storage");
      return;
    }
    size_t file_size = file_stat.st_size;

    ESP_LOGI(TAG, "Network storage file size: %" PRIu32 " bytes, starting chunked download", (uint32_t) file_size);

    // Set response headers
    httpd_resp_set_type(req, mime_type.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
    httpd_resp_set_hdr(req, "Content-Length", std::to_string(file_size).c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    // Stream data in chunks using read_file_chunk (memory efficient)
    uint8_t *buffer = this->download_buffer_;  // Network download
    size_t total_sent = 0;
    bool success = true;

    while (total_sent < file_size) {
      App.feed_wdt();  // Feed watchdog for large files

      size_t bytes_read = 0;
      if (!net_storage->read_file_chunk(relative_path, buffer, total_sent, FILE_BUFFER_SIZE, bytes_read)) {
        ESP_LOGE(TAG, "Failed to read chunk at offset %" PRIu32 " from network storage", (uint32_t) total_sent);
        success = false;
        break;
      }

      if (bytes_read == 0) {
        // EOF reached
        break;
      }

      esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Download cancelled or connection closed at %" PRIu32 " / %" PRIu32 " bytes", (uint32_t) total_sent, (uint32_t) file_size);
        success = false;
        break;
      }

      total_sent += bytes_read;

      // Log progress every ~3MB
      if (file_size > 3 * 1024 * 1024 && (total_sent % (3 * 1024 * 1024) < FILE_BUFFER_SIZE)) {
        ESP_LOGI(TAG, "Download progress: %" PRIu32 " / %" PRIu32 " MB (%.1f%%)", (uint32_t)(total_sent / (1024 * 1024)), (uint32_t)(file_size / (1024 * 1024)),
                 (float) total_sent / file_size * 100.0f);
      }
    }

    // Send final empty chunk to signal completion
    if (success) {
      httpd_resp_send_chunk(req, nullptr, 0);
      ESP_LOGI(TAG, "Download completed: %" PRIu32 " bytes", (uint32_t) total_sent);
    } else {
      ESP_LOGW(TAG, "Download incomplete: %" PRIu32 " / %" PRIu32 " bytes", (uint32_t) total_sent, (uint32_t) file_size);
    }

    return;
  }

  // Local storage path - use VFS streaming
  // Get file size
  struct stat file_stat;
  if (stat(filepath.c_str(), &file_stat) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return;
  }
  size_t file_size = file_stat.st_size;

  ESP_LOGI(TAG, "Starting local file download: %s (size: %" PRIu32 " bytes)", filename.c_str(), (uint32_t) file_size);

  // Open file
  FILE *file = fopen(filepath.c_str(), "rb");
  if (!file) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return;
  }

  // Set response headers using raw httpd API
  httpd_resp_set_type(req, mime_type.c_str());
  httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
  httpd_resp_set_hdr(req, "Content-Length", std::to_string(file_size).c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  // Stream file in chunks
  uint8_t *buffer = this->download_buffer_;  // Network download
  size_t total_sent = 0;
  bool success = true;

  while (total_sent < file_size) {
    App.feed_wdt();  // Feed watchdog for large files

    size_t to_read = std::min(FILE_BUFFER_SIZE, file_size - total_sent);
    size_t bytes_read = fread(buffer, 1, to_read, file);

    if (bytes_read == 0) {
      ESP_LOGE(TAG, "Failed to read chunk at offset %" PRIu32, (uint32_t) total_sent);
      success = false;
      break;
    }

    esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Download cancelled or connection closed at %" PRIu32 " / %" PRIu32 " bytes", (uint32_t) total_sent, (uint32_t) file_size);
      success = false;
      break;
    }

    total_sent += bytes_read;

    // Log progress every ~3MB
    if (file_size > 3 * 1024 * 1024 && (total_sent % (3 * 1024 * 1024) < FILE_BUFFER_SIZE)) {
      ESP_LOGI(TAG, "Download progress: %" PRIu32 " / %" PRIu32 " MB (%.1f%%)", (uint32_t)(total_sent / (1024 * 1024)), (uint32_t)(file_size / (1024 * 1024)),
               (float) total_sent / file_size * 100.0f);
    }
  }

  fclose(file);

  // Send final empty chunk to signal completion
  if (success) {
    httpd_resp_send_chunk(req, nullptr, 0);
    ESP_LOGI(TAG, "Download completed: %" PRIu32 " bytes", (uint32_t) total_sent);
  } else {
    ESP_LOGW(TAG, "Download incomplete: %" PRIu32 " / %" PRIu32 " bytes", (uint32_t) total_sent, (uint32_t) file_size);
  }
}

void HttpFileBrowser::handle_file_upload(AsyncWebServerRequest *request, const std::string &filename) {
  if (!this->upload_enabled_) {
    request->send(403, "text/plain", "File upload is disabled");
    return;
  }

  // Get the directory path from URI
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef uri = request->url_to(url_buf);
  std::string dir_path = this->uri_to_filepath(uri);

  // Check if target is a directory
  struct stat file_stat;
  if (stat(dir_path.c_str(), &file_stat) != 0 || !S_ISDIR(file_stat.st_mode)) {
    request->send(400, "text/plain", "Upload target must be a directory");
    return;
  }

  // Build full upload path
  std::string upload_path = Path::join(dir_path, filename);

  // Atomically check and set in_progress flag to prevent concurrent uploads
  portENTER_CRITICAL(&this->progress_mutex_);
  if (this->progress_.in_progress) {
    portEXIT_CRITICAL(&this->progress_mutex_);
    ESP_LOGW(TAG, "Upload rejected: another operation is already in progress (%s)", upload_path.c_str());
    request->send(409, "text/plain", "Another upload/copy/move is already in progress. Please wait.");
    return;
  }
  // Set progress tracking immediately while still in critical section to prevent race
  this->progress_.operation = "upload";
  this->progress_.source = filename.c_str();
  this->progress_.destination = upload_path;
  this->progress_.total_bytes = request->contentLength();
  this->progress_.transferred_bytes = 0;
  this->progress_.in_progress = true;
  this->progress_.cancelled = false;
  this->progress_.start_time = millis();
  portEXIT_CRITICAL(&this->progress_mutex_);

  ESP_LOGI(TAG, "Starting upload: %s (%" PRIu32 " bytes, handler address: %p)", upload_path.c_str(), (uint32_t) request->contentLength(),
           (void *) this);

  // Open file for writing
  FILE *file = fopen(upload_path.c_str(), "wb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s", upload_path.c_str());
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(500, "text/plain", "Failed to open file for writing");
    return;
  }

  // Get raw httpd_req_t to read POST body
  httpd_req_t *req = static_cast<httpd_req_t *>(*request);
  size_t remaining = request->contentLength();
  // Use preallocated upload buffer from buffer pool
  uint8_t *buffer = this->upload_buffer_;
  bool success = true;

  ESP_LOGI(TAG, "Reading upload data: %" PRIu32 " bytes total", (uint32_t) remaining);

  // Read and write in chunks
  size_t chunks_since_yield = 0;
  while (remaining > 0) {
    // Feed the watchdog to prevent timeout on large uploads
    App.feed_wdt();

    size_t to_read = (remaining > FILE_BUFFER_SIZE) ? FILE_BUFFER_SIZE : remaining;
    int received = httpd_req_recv(req, reinterpret_cast<char *>(buffer), to_read);

    if (received <= 0) {
      ESP_LOGE(TAG, "Failed to receive data: %d (errno: %d)", received, errno);
      success = false;
      break;
    }

    size_t written = fwrite(buffer, 1, received, file);
    fflush(file);  // Ensure data is written immediately

    if (written != static_cast<size_t>(received)) {
      ESP_LOGE(TAG, "Failed to write to file: wrote %" PRIu32 " of %d bytes", (uint32_t) written, received);
      success = false;
      break;
    }

    // Update progress (thread-safe)
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.transferred_bytes += written;
    size_t current_transferred = this->progress_.transferred_bytes;
    size_t current_total = this->progress_.total_bytes;
    portEXIT_CRITICAL(&this->progress_mutex_);

    remaining -= received;
    chunks_since_yield++;

    // Yield to other tasks every 64 chunks (256KB - 1MB) to allow progress polling
    if (chunks_since_yield >= 64) {
      vTaskDelay(1);  // Brief yield to let other tasks run (including /api/progress requests)
      chunks_since_yield = 0;
    }

    // Log progress every ~50KB
    if (current_transferred % (50 * 1024) < FILE_BUFFER_SIZE) {
      ESP_LOGD(TAG, "Upload progress: %" PRIu32 " / %" PRIu32 " bytes (%.1f%%)", (uint32_t) current_transferred, (uint32_t) current_total,
               (float) current_transferred / current_total * 100.0f);
    }
  }

  fclose(file);

  // Mark progress as complete (thread-safe)
  portENTER_CRITICAL(&this->progress_mutex_);
  size_t final_transferred = this->progress_.transferred_bytes;
  this->progress_.in_progress = false;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (success) {
    ESP_LOGI(TAG, "Upload complete: %" PRIu32 " bytes", (uint32_t) final_transferred);
    request->send(200, "text/plain", "File uploaded successfully");
  } else {
    ESP_LOGE(TAG, "Upload failed");
    remove(upload_path.c_str());  // Clean up partial file
    request->send(500, "text/plain", "Upload failed");
  }
}

// Directory helpers
bool HttpFileBrowser::is_directory_empty(const std::string &path) {
  // Check if this is network storage
  storage::NetworkStorage *net_storage = nullptr;
  if (this->storage_ != nullptr) {
    net_storage = this->storage_->find_network_storage_for_path(path);
  }

  if (net_storage != nullptr) {
    // Network storage - use NetworkStorage API
    std::string relative_path = strip_network_mount_prefix(net_storage, path);
    std::vector<storage::NetworkStorage::DirEntry> entries;
    if (!net_storage->list_directory(relative_path, entries)) {
      return true;  // Can't list, treat as empty
    }
    return entries.empty();
  }

  // Local VFS storage
  DIR *dir = opendir(path.c_str());
  if (!dir) {
    return true;  // Can't open, treat as empty
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    // Found at least one entry - not empty
    closedir(dir);
    return false;
  }

  closedir(dir);
  return true;  // No entries found - empty
}

bool HttpFileBrowser::is_directory_writable(const std::string &dir_path) {
  // Check if this is network storage
  storage::NetworkStorage *net_storage = nullptr;
  if (this->storage_ != nullptr) {
    net_storage = this->storage_->find_network_storage_for_path(dir_path);
  }

  if (net_storage != nullptr) {
    // Network storage - use NetworkStorage API
    std::string relative_path = strip_network_mount_prefix(net_storage, dir_path);

    // Check if directory exists using network storage
    struct stat dir_stat;
    if (!net_storage->stat(relative_path, dir_stat)) {
      ESP_LOGW(TAG, "Directory does not exist (network): %s", dir_path.c_str());
      return false;
    }

    if (!S_ISDIR(dir_stat.st_mode)) {
      ESP_LOGW(TAG, "Path is not a directory (network): %s", dir_path.c_str());
      return false;
    }

    // For network storage, assume writable if directory exists
    // (NFS permissions are handled by the server)
    return true;
  }

  // Local VFS storage
  struct stat dir_stat;
  if (stat(dir_path.c_str(), &dir_stat) != 0) {
    ESP_LOGW(TAG, "Directory does not exist: %s", dir_path.c_str());
    return false;
  }

  if (!S_ISDIR(dir_stat.st_mode)) {
    ESP_LOGW(TAG, "Path is not a directory: %s", dir_path.c_str());
    return false;
  }

  // Try to create a temporary test file to verify write permissions
  std::string test_file = Path::join(dir_path, ".write_test");
  FILE *f = fopen(test_file.c_str(), "w");
  if (!f) {
    ESP_LOGW(TAG, "Directory is not writable: %s (errno: %d, %s)", dir_path.c_str(), errno, strerror(errno));
    return false;
  }

  fclose(f);
  remove(test_file.c_str());  // Clean up test file
  return true;
}

void HttpFileBrowser::count_directory_contents(const std::string &path, int &file_count, int &dir_count) {
  DIR *dir = opendir(path.c_str());
  if (!dir) {
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    std::string entry_path = Path::join(path, entry->d_name);
    struct stat entry_stat;
    if (stat(entry_path.c_str(), &entry_stat) == 0) {
      if (S_ISDIR(entry_stat.st_mode)) {
        dir_count++;
        // Recursively count subdirectory contents
        this->count_directory_contents(entry_path, file_count, dir_count);
      } else {
        file_count++;
      }
    }
  }

  closedir(dir);
}

// Network storage version of count_directory_contents
void HttpFileBrowser::count_directory_contents_network(storage::NetworkStorage *net_storage, const std::string &path,
                                                       int &file_count, int &dir_count) {
  if (net_storage == nullptr) {
    return;
  }

  // List directory contents
  std::vector<storage::NetworkStorage::DirEntry> entries;
  if (!net_storage->list_directory(path, entries)) {
    return;
  }

  for (const auto &entry : entries) {
    if (entry.is_directory) {
      dir_count++;
      // Recursively count subdirectory contents
      std::string entry_path = Path::join(path, entry.name);
      this->count_directory_contents_network(net_storage, entry_path, file_count, dir_count);
    } else {
      file_count++;
    }
  }
}

bool HttpFileBrowser::recursive_delete_directory(const std::string &path, bool track_progress) {
  DIR *dir = opendir(path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Cannot open directory for deletion: %s (errno: %d)", path.c_str(), errno);
    return false;
  }

  // First pass: count the number of files in the directory
  size_t file_count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      file_count++;
    }
  }
  closedir(dir);

  // Determine if we should log each file deletion
  bool log_each_file = (file_count > 5) || track_progress;
  if (log_each_file && !track_progress) {
    ESP_LOGI(TAG, "Deleting directory with %zu files: %s (will log each file)", file_count, path.c_str());
  }

  // Second pass: actually delete the files
  dir = opendir(path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Cannot reopen directory for deletion: %s (errno: %d)", path.c_str(), errno);
    return false;
  }

  bool success = true;
  while ((entry = readdir(dir)) != nullptr) {
    // Check for cancellation
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      bool cancelled = this->progress_.cancelled;
      portEXIT_CRITICAL(&this->progress_mutex_);

      if (cancelled) {
        ESP_LOGI(TAG, "Delete operation cancelled by user");
        closedir(dir);
        return false;
      }
    }

    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    std::string entry_path = Path::join(path, entry->d_name);
    struct stat entry_stat;
    if (stat(entry_path.c_str(), &entry_stat) == 0) {
      if (S_ISDIR(entry_stat.st_mode)) {
        // Recursively delete subdirectory
        if (!this->recursive_delete_directory(entry_path, track_progress)) {
          success = false;
          break;
        }
      } else {
        // Update progress with current file
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.current_file = entry_path;
          portEXIT_CRITICAL(&this->progress_mutex_);
        }

        // Delete file
        if (log_each_file) {
          ESP_LOGI(TAG, "Deleting file: %s", entry_path.c_str());
        }
        if (remove(entry_path.c_str()) != 0) {
          ESP_LOGE(TAG, "Failed to delete file: %s (errno: %d, %s)", entry_path.c_str(), errno, strerror(errno));
          success = false;
          break;
        }

        // Update progress counter
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.processed_items++;
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
      }
    }
  }

  closedir(dir);

  // Delete the directory itself
  if (success) {
    if (rmdir(path.c_str()) != 0) {
      ESP_LOGE(TAG, "Failed to delete directory: %s (errno: %d, %s)", path.c_str(), errno, strerror(errno));
      return false;
    }

    // Update progress for the directory
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.processed_items++;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
  }

  return success;
}

// Network storage recursive directory deletion
bool HttpFileBrowser::recursive_delete_directory_network(storage::NetworkStorage *net_storage, const std::string &path,
                                                         bool track_progress) {
  if (net_storage == nullptr) {
    ESP_LOGE(TAG, "Network storage is null");
    return false;
  }

  ESP_LOGI(TAG, "Recursively deleting network storage directory: %s", path.c_str());

  // List directory contents
  std::vector<storage::NetworkStorage::DirEntry> entries;
  if (!net_storage->list_directory(path, entries)) {
    ESP_LOGE(TAG, "Failed to list network storage directory: %s", path.c_str());
    return false;
  }

  bool log_each_file = (entries.size() > 5) || track_progress;
  if (log_each_file && !track_progress) {
    ESP_LOGI(TAG, "Deleting network directory with %zu entries: %s (will log each entry)", entries.size(),
             path.c_str());
  }

  bool success = true;

  // Delete all entries
  for (const auto &entry : entries) {
    // Feed watchdog
    App.feed_wdt();

    // Check for cancellation if tracking progress
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      bool cancelled = this->progress_.cancelled;
      portEXIT_CRITICAL(&this->progress_mutex_);

      if (cancelled) {
        ESP_LOGI(TAG, "Delete operation cancelled by user");
        return false;
      }
    }

    std::string entry_path = Path::join(path, entry.name);

    if (entry.is_directory) {
      // Recursively delete subdirectory
      if (!this->recursive_delete_directory_network(net_storage, entry_path, track_progress)) {
        success = false;
        break;
      }
    } else {
      // Update progress with current file
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.current_file = entry_path;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }

      // Delete file
      if (log_each_file) {
        ESP_LOGI(TAG, "Deleting network file: %s", entry_path.c_str());
      }

      if (!net_storage->delete_file(entry_path)) {
        ESP_LOGE(TAG, "Failed to delete network file: %s", entry_path.c_str());
        success = false;
        break;
      }

      // Update progress counter
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.processed_items++;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    }
  }

  // Delete the directory itself (now empty)
  if (success) {
    if (!net_storage->delete_directory(path)) {
      ESP_LOGE(TAG, "Failed to delete network directory: %s", path.c_str());
      return false;
    }

    // Update progress for the directory
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.processed_items++;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
  }

  return success;
}

// Mount status helper
bool HttpFileBrowser::is_mount_point_mounted(const std::string &mount_path) {
  // Use storage component if available (new architecture)
  if (this->storage_ != nullptr) {
    // Check if this is a network storage mount
    auto *net_storage = this->storage_->find_network_storage_for_path(mount_path);
    if (net_storage != nullptr) {
      // Network storage - use is_connected()
      return net_storage->is_connected();
    }

    // Check local storage mounts
    const auto &mounts = this->storage_->get_mounts();
    for (const auto &mount : mounts) {
      if (mount.path == mount_path) {
        // Local storage - check if mount path exists and is accessible
        struct stat st;
        if (stat(mount_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
          return false;
        }

        // Try to actually open the directory to verify filesystem is ready
        DIR *dir = opendir(mount_path.c_str());
        if (dir == nullptr) {
          return false;
        }
        closedir(dir);
        return true;
      }
    }
  }

  // Fallback to old device-specific checks for backwards compatibility
#ifdef USE_USB_STORAGE
  for (void *dev_ptr : this->usb_storage_devices_) {
    auto *device = static_cast<usb_storage::USBStorageDevice *>(dev_ptr);
    if (device->get_mount_path() == mount_path) {
      return device->is_mounted();
    }
  }
#endif

#ifdef USE_SD_STORAGE
  for (void *dev_ptr : this->sd_storage_devices_) {
    auto *device = static_cast<sd_storage::SdMmc *>(dev_ptr);
    if (device->get_mount_path() == mount_path) {
      return device->is_mounted();
    }
  }
#endif

  // If no matching device found, try to check if path exists and is accessible
  struct stat st;
  if (stat(mount_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return false;
  }

  DIR *dir = opendir(mount_path.c_str());
  if (dir == nullptr) {
    return false;
  }
  closedir(dir);
  return true;
}

bool HttpFileBrowser::get_mount_space_info(const std::string &mount_path, uint64_t &total, uint64_t &free) {
  // Check if storage component has registered devices
  if (this->storage_ == nullptr) {
    return false;
  }

#ifdef USE_ESP_IDF
  // Use ESP-IDF VFS function to get FAT filesystem info
  uint64_t total_bytes = 0, free_bytes = 0;
  esp_err_t ret = esp_vfs_fat_info(mount_path.c_str(), &total_bytes, &free_bytes);
  if (ret == ESP_OK) {
    total = total_bytes;
    free = free_bytes;
    return true;
  }
  ESP_LOGV(TAG, "esp_vfs_fat_info failed for %s: %s", mount_path.c_str(), esp_err_to_name(ret));
#endif

  return false;
}

std::string HttpFileBrowser::get_filename_from_path(const std::string &path) {
  // Find the last '/' in the path
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    // No '/' found, return the whole path
    return path;
  }
  // Return everything after the last '/'
  return path.substr(pos + 1);
}

std::string HttpFileBrowser::strip_network_mount_prefix(storage::NetworkStorage *net_storage, const std::string &path) {
  // Strip mount point prefix to get path relative to network storage root
  const std::string &mount_path = net_storage->get_mount_path();
  if (path.compare(0, mount_path.length(), mount_path) == 0) {
    std::string relative = path.substr(mount_path.length());
    // Ensure it starts with / or is /
    if (relative.empty() || relative[0] != '/') {
      relative = "/" + relative;
    }
    return relative;
  }
  return path;
}

// API handlers
void HttpFileBrowser::handle_api_copy(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *source_param = request->getParam("source");
  auto *dest_param = request->getParam("destination");

  if (!source_param || !dest_param) {
    ESP_LOGW(TAG, "Missing source or destination parameter");
    request->send(400, "application/json", "{\"error\":\"Missing source or destination\"}");
    return;
  }

  // Convert URI paths to filesystem paths (strips URL prefix)
  std::string source = this->uri_to_filepath(source_param->value().c_str());
  std::string destination = this->uri_to_filepath(dest_param->value().c_str());

  ESP_LOGI(TAG, "API COPY: %s -> %s", source.c_str(), destination.c_str());

  // Check if source exists (supports network storage)
  bool is_directory = false;
  if (!this->path_exists(source, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Source not found\"}");
    return;
  }

  // Get file size for progress tracking (0 for directories, will be updated as we discover files)
  off_t file_size = 0;
  if (!is_directory) {
    struct stat src_stat;
    // Try local stat first
    if (stat(source.c_str(), &src_stat) == 0) {
      file_size = src_stat.st_size;
    } else if (this->storage_ != nullptr) {
      // Network storage - try to get size via NetworkStorage::stat
      auto *net_storage = this->storage_->find_network_storage_for_path(source);
      if (net_storage != nullptr && net_storage->stat(source, src_stat)) {
        file_size = src_stat.st_size;
      }
    }
  }

  // Check if destination directory is writable (only once at start of operation)
  std::string dest_dir = Path::dirname(destination);
  if (!this->is_directory_writable(dest_dir)) {
    request->send(403, "application/json", "{\"error\":\"Destination directory is not writable\"}");
    return;
  }

  // Check if another operation is already in progress
  portENTER_CRITICAL(&this->progress_mutex_);
  bool already_in_progress = this->progress_.in_progress;
  std::string current_operation = this->progress_.operation;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (already_in_progress) {
    ESP_LOGW(TAG, "Cannot start copy: %s operation already in progress", current_operation.c_str());
    request->send(409, "application/json", "{\"error\":\"Another operation is already in progress\"}");
    return;
  }

  // Create task parameters
  // Always track progress for consistent modal behavior (overhead is minimal)
  bool track_progress = true;
  ESP_LOGI(TAG, "Copy: %s, size %lld bytes, free_heap=%" PRIu32, is_directory ? "directory" : "file",
           (long long) file_size, esp_get_free_heap_size());

  auto *task_params = new CopyTaskParams{this, source, destination, file_size, track_progress, is_directory};

  // Create FreeRTOS task for background copy (4KB stack, priority 1)
  BaseType_t result = xTaskCreate(copy_task, "http_copy", 4096, task_params, 1, nullptr);

  if (result == pdPASS) {
    ESP_LOGI(TAG, "Copy task created successfully");
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    ESP_LOGE(TAG, "Failed to create copy task (heap: %lu)", esp_get_free_heap_size());
    delete task_params;
    request->send(500, "application/json", "{\"error\":\"Failed to start copy operation\"}");
  }
}

void HttpFileBrowser::handle_api_move(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *source_param = request->getParam("source");
  auto *dest_param = request->getParam("destination");

  if (!source_param || !dest_param) {
    ESP_LOGW(TAG, "Missing source or destination parameter");
    request->send(400, "application/json", "{\"error\":\"Missing source or destination\"}");
    return;
  }

  // Convert URI paths to filesystem paths (strips URL prefix)
  std::string source = this->uri_to_filepath(source_param->value().c_str());
  std::string destination = this->uri_to_filepath(dest_param->value().c_str());

  ESP_LOGI(TAG, "API MOVE: %s -> %s", source.c_str(), destination.c_str());

  // Check if source exists (supports network storage)
  bool is_directory = false;
  if (!this->path_exists(source, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Source not found\"}");
    return;
  }

  // Get file size for progress tracking (0 for directories)
  off_t file_size = 0;
  if (!is_directory) {
    struct stat src_stat;
    // Try local stat first
    if (stat(source.c_str(), &src_stat) == 0) {
      file_size = src_stat.st_size;
    } else if (this->storage_ != nullptr) {
      // Network storage - try to get size via NetworkStorage::stat
      auto *net_storage = this->storage_->find_network_storage_for_path(source);
      if (net_storage != nullptr && net_storage->stat(source, src_stat)) {
        file_size = src_stat.st_size;
      }
    }
  }

  // Check if destination directory is writable (only once at start of operation)
  std::string dest_dir = Path::dirname(destination);
  if (!this->is_directory_writable(dest_dir)) {
    request->send(403, "application/json", "{\"error\":\"Destination directory is not writable\"}");
    return;
  }

  // Check if another operation is already in progress
  portENTER_CRITICAL(&this->progress_mutex_);
  bool already_in_progress = this->progress_.in_progress;
  std::string current_operation = this->progress_.operation;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (already_in_progress) {
    ESP_LOGW(TAG, "Cannot start move: %s operation already in progress", current_operation.c_str());
    request->send(409, "application/json", "{\"error\":\"Another operation is already in progress\"}");
    return;
  }

  // Create task parameters
  // Always track progress for consistent modal behavior (overhead is minimal)
  bool track_progress = true;
  ESP_LOGI(TAG, "Move: %s, size %lld bytes, free_heap=%" PRIu32, is_directory ? "directory" : "file",
           (long long) file_size, esp_get_free_heap_size());

  auto *task_params = new MoveTaskParams{this, source, destination, file_size, track_progress, is_directory};

  // Create FreeRTOS task for background move (4KB stack, priority 1)
  BaseType_t result = xTaskCreate(move_task, "http_move", 4096, task_params, 1, nullptr);

  if (result == pdPASS) {
    ESP_LOGI(TAG, "Move task created successfully");
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    ESP_LOGE(TAG, "Failed to create move task (heap: %" PRIu32 ")", esp_get_free_heap_size());
    delete task_params;
    request->send(500, "application/json", "{\"error\":\"Failed to start move operation\"}");
  }
}

void HttpFileBrowser::handle_api_rename(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *source_param = request->getParam("source");
  auto *name_param = request->getParam("name");

  if (!source_param || !name_param) {
    ESP_LOGW(TAG, "Missing source or name parameter");
    request->send(400, "application/json", "{\"error\":\"Missing source or name\"}");
    return;
  }

  // Convert URI path to filesystem path (strips URL prefix)
  std::string source = this->uri_to_filepath(source_param->value().c_str());
  std::string new_name = name_param->value().c_str();

  // Build new path (same directory, new name)
  std::string dir_path = source.substr(0, source.find_last_of('/'));
  std::string new_path = Path::join(dir_path, new_name);

  ESP_LOGI(TAG, "API RENAME: %s -> %s", source.c_str(), new_path.c_str());

  // Check if source exists (using helper that supports network storage)
  bool is_directory = false;
  if (!this->path_exists(source, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Source file not found\"}");
    return;
  }

  // Check if source is on network storage
  storage::NetworkStorage *net_storage = nullptr;
  if (this->storage_ != nullptr) {
    net_storage = this->storage_->find_network_storage_for_path(source);
  }

  // Perform rename
  bool success = false;

  if (net_storage != nullptr) {
    // Check if destination is also on the same network storage
    storage::NetworkStorage *dest_net_storage = nullptr;
    if (this->storage_ != nullptr) {
      dest_net_storage = this->storage_->find_network_storage_for_path(new_path);
    }

    // If both source and destination are on the same network storage, try native rename
    if (dest_net_storage != nullptr && dest_net_storage == net_storage) {
      ESP_LOGI(TAG, "Same network storage detected, using native rename");
      std::string source_relative = strip_network_mount_prefix(net_storage, source);
      std::string dest_relative = strip_network_mount_prefix(net_storage, new_path);
      success = net_storage->rename_path(source_relative, dest_relative);

      if (!success) {
        // If native rename failed or not supported, fall back to copy+delete
        ESP_LOGW(TAG, "Native rename failed or not supported, falling back to copy+delete");
        if (is_directory) {
          success = this->perform_directory_move(source, new_path, false);
        } else {
          struct stat file_stat;
          off_t file_size = 0;
          if (net_storage->stat(source_relative, file_stat)) {
            file_size = file_stat.st_size;
          }
          success = this->perform_file_move(source, new_path, file_size, false);
        }
      }
    } else {
      // Cross-storage move - use copy+delete
      ESP_LOGI(TAG, "Cross-storage move detected, using copy+delete");
      if (is_directory) {
        success = this->perform_directory_move(source, new_path, false);
      } else {
        struct stat file_stat;
        off_t file_size = 0;
        std::string source_relative = strip_network_mount_prefix(net_storage, source);
        if (net_storage->stat(source_relative, file_stat)) {
          file_size = file_stat.st_size;
        }
        success = this->perform_file_move(source, new_path, file_size, false);
      }
    }
  } else {
    // Local storage - use atomic rename
    success = (rename(source.c_str(), new_path.c_str()) == 0);
  }

  if (success) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    ESP_LOGE(TAG, "Rename failed: %s (errno: %d, %s)", source.c_str(), errno, strerror(errno));
    request->send(500, "application/json", "{\"error\":\"Rename operation failed\"}");
  }
}

void HttpFileBrowser::handle_api_mkdir(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *name_param = request->getParam("name");

  if (!name_param) {
    ESP_LOGW(TAG, "Missing name parameter");
    request->send(400, "application/json", "{\"error\":\"Missing directory name\"}");
    return;
  }

  std::string dir_uri = name_param->value().c_str();

  // Convert URI to filesystem path
  std::string dir_path = this->uri_to_filepath(dir_uri);

  ESP_LOGI(TAG, "API MKDIR: URI=%s -> Path=%s", dir_uri.c_str(), dir_path.c_str());

  // Create directory (using helper that supports network storage)
  if (this->create_dir(dir_path)) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    ESP_LOGE(TAG, "Mkdir failed: %s", dir_path.c_str());
    request->send(500, "application/json", "{\"error\":\"Mkdir operation failed\"}");
  }
}

void HttpFileBrowser::handle_api_delete(AsyncWebServerRequest *request) {
  if (!this->deletion_enabled_) {
    ESP_LOGW(TAG, "Deletion is disabled");
    request->send(403, "application/json", "{\"error\":\"File deletion is disabled\"}");
    return;
  }

  // Get parameters from POST body
  auto *path_param = request->getParam("path");

  if (!path_param) {
    ESP_LOGW(TAG, "Missing path parameter");
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }

  std::string path_uri = path_param->value().c_str();

  // Convert URI to filesystem path
  std::string filepath = this->uri_to_filepath(path_uri);

  ESP_LOGI(TAG, "API DELETE: URI=%s -> Path=%s", path_uri.c_str(), filepath.c_str());

  // Check if path exists (using helper that supports network storage)
  bool is_directory = false;
  if (!this->path_exists(filepath, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Path not found\"}");
    return;
  }

  bool success = false;
  if (is_directory) {
    // Check if this is network storage
    storage::NetworkStorage *net_storage = nullptr;
    if (this->storage_ != nullptr) {
      net_storage = this->storage_->find_network_storage_for_path(filepath);
    }

    if (net_storage != nullptr) {
      // Network storage - use network storage recursive deletion
      // Strip mount prefix for NFS operations
      std::string relative_path = strip_network_mount_prefix(net_storage, filepath);
      ESP_LOGI(TAG, "Attempting to delete directory on network storage: %s (relative: %s)", filepath.c_str(),
               relative_path.c_str());

      // Count files/directories first (recursively)
      int file_count = 0;
      int dir_count = 0;
      this->count_directory_contents_network(net_storage, relative_path, file_count, dir_count);
      int total_items = file_count + dir_count;

      // Initialize progress tracking for large deletions (>5 items)
      bool track_progress = (total_items > 5);
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.operation = "delete";
        this->progress_.source = filepath;
        this->progress_.destination = "";
        this->progress_.current_file = "";
        this->progress_.total_items = total_items;
        this->progress_.processed_items = 0;
        this->progress_.in_progress = true;
        this->progress_.cancelled = false;
        this->progress_.start_time = millis();
        portEXIT_CRITICAL(&this->progress_mutex_);
        ESP_LOGI(TAG, "Deleting network directory recursively: %s (%d files, %d dirs)", relative_path.c_str(),
                 file_count, dir_count);
      } else {
        ESP_LOGI(TAG, "Deleting network directory recursively: %s", relative_path.c_str());
      }

      // Recursive directory deletion for network storage (uses relative paths)
      success = this->recursive_delete_directory_network(net_storage, relative_path, track_progress);

      // Clear progress tracking
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;  // Reset cancelled flag after handling
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    } else {
      // Local storage - use VFS recursive deletion
      // Count files/directories first
      int file_count = 0;
      int dir_count = 0;
      this->count_directory_contents(filepath, file_count, dir_count);
      int total_items = file_count + dir_count;

      // Initialize progress tracking for large deletions (>5 items)
      bool track_progress = (total_items > 5);
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.operation = "delete";
        this->progress_.source = filepath;
        this->progress_.destination = "";
        this->progress_.current_file = "";
        this->progress_.total_items = total_items;
        this->progress_.processed_items = 0;
        this->progress_.in_progress = true;
        this->progress_.cancelled = false;
        this->progress_.start_time = millis();
        portEXIT_CRITICAL(&this->progress_mutex_);
        ESP_LOGI(TAG, "Deleting directory recursively: %s (%d files, %d dirs)", filepath.c_str(), file_count,
                 dir_count);
      } else {
        ESP_LOGI(TAG, "Deleting directory recursively: %s", filepath.c_str());
      }

      // Recursive directory deletion
      success = this->recursive_delete_directory(filepath, track_progress);

      // Clear progress tracking
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;  // Reset cancelled flag after handling
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    }
  } else {
    // Single file deletion (using helper that supports network storage)
    ESP_LOGI(TAG, "Deleting file: %s", filepath.c_str());
    success = this->delete_path(filepath, false);
  }

  if (success) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    ESP_LOGE(TAG, "Delete failed: %s (errno: %d, %s)", filepath.c_str(), errno, strerror(errno));
    request->send(500, "application/json", "{\"error\":\"Delete operation failed\"}");
  }
}

void HttpFileBrowser::handle_api_mount(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *mount_point_param = request->getParam("mount_point");

  if (!mount_point_param) {
    ESP_LOGW(TAG, "Missing mount_point parameter");
    request->send(400, "application/json", "{\"error\":\"Missing mount_point parameter\"}");
    return;
  }

  std::string mount_point = mount_point_param->value().c_str();
  ESP_LOGI(TAG, "API MOUNT: mount_point=%s (scheduling deferred mount)", mount_point.c_str());

  // Schedule deferred mount to happen in loop() after HTTP response completes
  // This prevents potential memory issues and maintains consistency with unmount behavior
  this->deferred_mount_op_.type = DeferredMountOp::MOUNT;
  this->deferred_mount_op_.mount_point = mount_point;
  this->deferred_mount_op_.schedule_time = millis() + 100;  // 100ms delay

  request->send(200, "application/json", "{\"success\":true}");
  return;
}

void HttpFileBrowser::handle_api_unmount(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *mount_point_param = request->getParam("mount_point");

  if (!mount_point_param) {
    ESP_LOGW(TAG, "Missing mount_point parameter");
    request->send(400, "application/json", "{\"error\":\"Missing mount_point parameter\"}");
    return;
  }

  std::string mount_point = mount_point_param->value().c_str();
  ESP_LOGI(TAG, "API UNMOUNT: mount_point=%s (scheduling deferred unmount)", mount_point.c_str());

  // Schedule deferred unmount to happen in loop() after HTTP response completes
  // This prevents double-free crashes from unmounting while HTTP request is active
  this->deferred_mount_op_.type = DeferredMountOp::UNMOUNT;
  this->deferred_mount_op_.mount_point = mount_point;
  this->deferred_mount_op_.schedule_time = millis() + 100;  // 100ms delay

  request->send(200, "application/json", "{\"success\":true}");
  // NOTE: The actual unmount happens in loop() - see the loop() method
}

void HttpFileBrowser::handle_api_remount(AsyncWebServerRequest *request) {
  // Get parameters from POST body
  auto *mount_point_param = request->getParam("mount_point");

  if (!mount_point_param) {
    ESP_LOGW(TAG, "Missing mount_point parameter");
    request->send(400, "application/json", "{\"error\":\"Missing mount_point parameter\"}");
    return;
  }

  std::string mount_point = mount_point_param->value().c_str();
  ESP_LOGI(TAG, "API REMOUNT: mount_point=%s (scheduling deferred remount)", mount_point.c_str());

  // Schedule deferred remount to happen in loop() after HTTP response completes
  // This prevents double-free crashes from unmounting while HTTP request is active
  this->deferred_mount_op_.type = DeferredMountOp::REMOUNT;
  this->deferred_mount_op_.mount_point = mount_point;
  this->deferred_mount_op_.schedule_time = millis() + 100;  // 100ms delay

  request->send(200, "application/json", "{\"success\":true}");
  return;
}

void HttpFileBrowser::handle_api_progress(AsyncWebServerRequest *request) {
  // Thread-safe snapshot of progress data
  portENTER_CRITICAL(&this->progress_mutex_);
  bool in_progress = this->progress_.in_progress;
  std::string operation = this->progress_.operation;
  std::string source = this->progress_.source;
  std::string destination = this->progress_.destination;
  std::string current_file = this->progress_.current_file;
  size_t total_bytes = this->progress_.total_bytes;
  size_t transferred_bytes = this->progress_.transferred_bytes;
  int total_items = this->progress_.total_items;
  int processed_items = this->progress_.processed_items;
  uint32_t start_time = this->progress_.start_time;

  // Auto-clear stuck operations (timeout after 5 minutes with no progress updates)
  if (in_progress && start_time > 0) {
    uint32_t elapsed_ms = millis() - start_time;
    if (elapsed_ms > 300000) {  // 5 minutes
      ESP_LOGW(TAG, "Auto-clearing stuck operation: %s (elapsed: %" PRIu32 " ms)", operation.c_str(), elapsed_ms);
      this->progress_.in_progress = false;
      in_progress = false;
    }
  }
  portEXIT_CRITICAL(&this->progress_mutex_);

  // Build JSON response with progress information
  std::string json = "{";
  json += "\"in_progress\":" + std::string(in_progress ? "true" : "false");

  if (in_progress) {
    json += ",\"operation\":\"" + operation + "\"";
    json += ",\"source\":\"" + source + "\"";
    json += ",\"destination\":\"" + destination + "\"";

    // For delete operations, include item-based progress
    if (operation == "delete") {
      json += ",\"total_items\":" + std::to_string(total_items);
      json += ",\"processed_items\":" + std::to_string(processed_items);
      if (!current_file.empty()) {
        json += ",\"current_file\":\"" + current_file + "\"";
      }

      // Calculate percentage based on items
      float percentage = 0.0;
      if (total_items > 0) {
        percentage = (processed_items * 100.0) / total_items;
      }

      // Format percentage with 1 decimal place
      char percent_buf[16];
      snprintf(percent_buf, sizeof(percent_buf), "%.1f", percentage);
      json += ",\"percentage\":" + std::string(percent_buf);
    } else {
      // For byte-based operations (copy, move, upload)
      json += ",\"total_bytes\":" + std::to_string(total_bytes);
      json += ",\"transferred_bytes\":" + std::to_string(transferred_bytes);

      // Calculate progress percentage
      float percentage = 0.0;
      if (total_bytes > 0) {
        percentage = (transferred_bytes * 100.0) / total_bytes;
      }

      // Format percentage with 1 decimal place
      char percent_buf[16];
      snprintf(percent_buf, sizeof(percent_buf), "%.1f", percentage);
      json += ",\"percentage\":" + std::string(percent_buf);
    }

    // Calculate elapsed time
    uint32_t elapsed_ms = millis() - start_time;
    json += ",\"elapsed_ms\":" + std::to_string(elapsed_ms);

    // Calculate average speed (bytes per second)
    if (elapsed_ms > 0 && transferred_bytes > 0) {
      // Convert to bytes/sec: (transferred_bytes * 1000) / elapsed_ms
      // Use 64-bit arithmetic to avoid overflow
      uint64_t avg_speed = ((uint64_t) transferred_bytes * 1000) / elapsed_ms;
      json += ",\"avg_speed\":" + std::to_string(avg_speed);
    }

    // Estimate remaining time if we have progress (only for byte-based operations)
    if (operation != "delete" && transferred_bytes > 0 && total_bytes > 0) {
      // Use 64-bit arithmetic to avoid overflow when multiplying elapsed_ms * total_bytes
      uint64_t total_estimated_ms = ((uint64_t) elapsed_ms * total_bytes) / transferred_bytes;
      uint64_t remaining_ms = total_estimated_ms > elapsed_ms ? total_estimated_ms - elapsed_ms : 0;
      json += ",\"remaining_ms\":" + std::to_string(remaining_ms);
    }

    // Log progress
    if (operation == "delete") {
      ESP_LOGD(TAG, "Progress poll: delete operation, %d/%d items", processed_items, total_items);
    } else {
      ESP_LOGD(TAG, "Progress poll: %s operation, %" PRIu32 "/%" PRIu32 " bytes", operation.c_str(),
               (uint32_t) transferred_bytes, (uint32_t) total_bytes);
    }
  } else {
    ESP_LOGD(TAG, "Progress poll: no operation in progress");
  }

  json += "}";

  ESP_LOGD(TAG, "Sending progress JSON response (%zu bytes): %s", json.length(), json.c_str());
  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);

  // CORS headers for XHR with credentials - required even for same-origin requests
  response->addHeader("Access-Control-Allow-Credentials", "true");
  // Get origin from Host header (same-origin case) or reflect Origin header
  auto origin_header = request->get_header("Origin");
  if (origin_header.has_value()) {
    // Reflect the Origin header back for CORS
    response->addHeader("Access-Control-Allow-Origin", origin_header.value().c_str());
  } else {
    // Construct origin from Host header for same-origin requests
    auto host_header = request->get_header("Host");
    if (host_header.has_value()) {
      std::string origin = "http://" + host_header.value();
      response->addHeader("Access-Control-Allow-Origin", origin.c_str());
    }
  }

  request->send(response);
  ESP_LOGD(TAG, "Progress response sent");
}

void HttpFileBrowser::handle_api_cancel(AsyncWebServerRequest *request) {
  ESP_LOGI(TAG, "Cancel request received");

  // Set cancelled flag (thread-safe)
  portENTER_CRITICAL(&this->progress_mutex_);
  if (this->progress_.in_progress) {
    this->progress_.cancelled = true;
    ESP_LOGI(TAG, "Cancelled %s operation", this->progress_.operation.c_str());
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Operation cancelled\"}");
  } else {
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(200, "application/json", "{\"success\":false,\"message\":\"No operation in progress\"}");
  }
}

void HttpFileBrowser::handle_api_upload_chunk(AsyncWebServerRequest *request) {
  if (!this->upload_enabled_) {
    request->send(403, "application/json", "{\"error\":\"Upload is disabled\"}");
    return;
  }

  // Get parameters from query string
  auto *filename_param = request->getParam("filename");
  auto *chunk_index_param = request->getParam("chunkIndex");
  auto *total_chunks_param = request->getParam("totalChunks");
  auto *path_param = request->getParam("path");
  auto *file_size_param = request->getParam("fileSize");

  if (!filename_param || !chunk_index_param || !total_chunks_param || !path_param || !file_size_param) {
    request->send(400, "application/json", "{\"error\":\"Missing required parameters\"}");
    return;
  }

  std::string filename = filename_param->value().c_str();
  int chunk_index = std::stoi(chunk_index_param->value().c_str());
  int total_chunks = std::stoi(total_chunks_param->value().c_str());
  std::string path = path_param->value().c_str();
  size_t file_size = std::stoull(file_size_param->value().c_str());

  // Log only every 50th chunk, first, and last to reduce overhead
  // Also log around potential failure threshold (chunks 170-180)
  if (chunk_index % 50 == 0 || chunk_index == 0 || chunk_index == total_chunks - 1 ||
      (chunk_index >= 170 && chunk_index <= 180)) {
    ESP_LOGD(TAG, "Upload chunk: file=%s, chunk=%d/%d, path=%s, size=%" PRIu32 ", free_heap=%" PRIu32, filename.c_str(),
             chunk_index, total_chunks, path.c_str(), (uint32_t) file_size, esp_get_free_heap_size());
  }

  // Convert path to filesystem path
  std::string dir_path = this->uri_to_filepath(path);
  std::string upload_path = Path::join(dir_path, filename);

  // Check if cancelled
  portENTER_CRITICAL(&this->progress_mutex_);
  bool cancelled = this->progress_.cancelled;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (cancelled) {
    // Clean up
    if (this->upload_file_) {
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
      remove(upload_path.c_str());
      ESP_LOGI(TAG, "Cleaned up cancelled chunked upload: %s", upload_path.c_str());
    }

    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;
    portEXIT_CRITICAL(&this->progress_mutex_);

    request->send(200, "application/json", "{\"success\":false,\"cancelled\":true}");
    return;
  }

  // First chunk - initialize
  if (chunk_index == 0) {
    // Clean up any stale upload state
    if (this->upload_file_) {
      ESP_LOGW(TAG, "Closing stale upload file from previous chunked upload");
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
    }
    this->upload_is_network_ = false;
    this->upload_network_storage_ = nullptr;
    this->upload_network_path_.clear();
    this->upload_network_offset_ = 0;

    // Check if this is network storage
#if defined(USE_STORAGE)
    storage::NetworkStorage *net_storage = nullptr;
    if (this->storage_ != nullptr) {
      net_storage = this->storage_->find_network_storage_for_path(dir_path);
    }

    if (net_storage != nullptr) {
      // Network storage - strip mount prefix for NFS operations
      std::string relative_dir = strip_network_mount_prefix(net_storage, dir_path);
      std::string relative_upload = strip_network_mount_prefix(net_storage, upload_path);

      // Verify directory exists using NetworkStorage API
      if (!net_storage->is_directory(relative_dir)) {
        ESP_LOGE(TAG, "Network storage upload target is not a directory: %s (relative: %s)", dir_path.c_str(),
                 relative_dir.c_str());
        request->send(400, "application/json", "{\"error\":\"Target is not a directory\"}");
        return;
      }

      // Set up network storage upload state
      this->upload_is_network_ = true;
      this->upload_network_storage_ = net_storage;
      this->upload_network_path_ = relative_upload;
      this->upload_network_offset_ = 0;

      ESP_LOGI(TAG, "Starting network storage chunked upload: %s (relative: %s)", upload_path.c_str(),
               relative_upload.c_str());
    } else
#endif
    {
      // Local storage - check if target is a directory using VFS
      struct stat file_stat;
      if (stat(dir_path.c_str(), &file_stat) != 0 || !S_ISDIR(file_stat.st_mode)) {
        ESP_LOGE(TAG, "Upload target is not a directory: %s", dir_path.c_str());
        request->send(400, "application/json", "{\"error\":\"Target is not a directory\"}");
        return;
      }

      // Check if directory is writable (only once at start of upload)
      if (!this->is_directory_writable(dir_path)) {
        request->send(403, "application/json", "{\"error\":\"Upload directory is not writable\"}");
        return;
      }

      // Open file for writing
      this->upload_file_ = fopen(upload_path.c_str(), "wb");
      if (!this->upload_file_) {
        ESP_LOGE(TAG, "Failed to open file for chunked upload: %s", upload_path.c_str());
        request->send(500, "application/json", "{\"error\":\"Failed to open file\"}");
        return;
      }
    }

    // Initialize progress tracking
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.operation = "upload";
    this->progress_.source = filename;
    this->progress_.destination = upload_path;
    this->progress_.total_bytes = file_size;
    this->progress_.transferred_bytes = 0;
    this->progress_.in_progress = true;
    this->progress_.cancelled = false;
    this->progress_.start_time = millis();
    portEXIT_CRITICAL(&this->progress_mutex_);

    this->upload_directory_ = dir_path;
    this->upload_filename_ = filename;

    ESP_LOGI(TAG, "Started chunked upload: %s (%zu bytes, %d chunks)", upload_path.c_str(), file_size, total_chunks);
  }

  // Write chunk data
  // For network storage, upload_file_ is null but upload_is_network_ is true
  if (!this->upload_file_ && !this->upload_is_network_) {
    ESP_LOGE(TAG, "Upload file not open for chunk %d", chunk_index);

    // Clean up progress state (file handle was lost/corrupted)
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;  // Reset cancelled flag after handling
    portEXIT_CRITICAL(&this->progress_mutex_);

    request->send(500, "application/json", "{\"error\":\"Upload not initialized\"}");
    return;
  }

  size_t bytes_to_write = 0;
  const uint8_t *data_ptr = nullptr;

  // Check if data is in body_buffer_ (form-urlencoded) or needs to be read from request (octet-stream)
  if (!this->body_buffer_.empty()) {
    // Data already in body_buffer_
    data_ptr = reinterpret_cast<const uint8_t *>(this->body_buffer_.data());
    bytes_to_write = this->body_buffer_.size();
    ESP_LOGD(TAG, "Reading chunk %d from body_buffer_: %zu bytes", chunk_index, bytes_to_write);
  } else {
    // body_buffer_ is empty - read directly from httpd_req_t (octet-stream case)
    httpd_req_t *req = *request;  // Convert AsyncWebServerRequest to httpd_req_t
    size_t content_len = req->content_len;

    if (content_len == 0) {
      ESP_LOGW(TAG, "Chunk %d has zero content length", chunk_index);
      // Empty chunk is valid (could be last chunk of a file that's exactly divisible by chunk size)
      bytes_to_write = 0;
    } else {
      ESP_LOGD(TAG, "Reading chunk %d directly from httpd request: %zu bytes", chunk_index, content_len);

      // Reuse chunk buffer to avoid repeated allocations (allocate/resize only if needed)
      if (!this->chunk_buffer_ || this->chunk_buffer_size_ < content_len) {
        // Free old buffer if it exists
        if (this->chunk_buffer_) {
          free(this->chunk_buffer_);
        }
        // Allocate new buffer with PSRAM preference
#if defined(USE_PSRAM) && defined(USE_ESP_IDF)
        this->chunk_buffer_ = (uint8_t *) heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!this->chunk_buffer_) {
          this->chunk_buffer_ = (uint8_t *) malloc(content_len);
        }
#else
        this->chunk_buffer_ = (uint8_t *) malloc(content_len);
#endif
        this->chunk_buffer_size_ = content_len;
      }

      // Read chunk data from HTTP request in a loop (httpd_req_recv may return partial reads)
      size_t total_received = 0;
      while (total_received < content_len) {
        int ret = httpd_req_recv(req, reinterpret_cast<char *>(this->chunk_buffer_) + total_received,
                                 content_len - total_received);
        if (ret <= 0) {
          ESP_LOGE(TAG, "Failed to receive chunk %d data: httpd_req_recv returned %d after %zu/%zu bytes", chunk_index,
                   ret, total_received, content_len);
          fclose(this->upload_file_);
          this->upload_file_ = nullptr;

          if (remove(upload_path.c_str()) == 0) {
            ESP_LOGI(TAG, "Deleted partial upload file after receive failure: %s", upload_path.c_str());
          } else {
            ESP_LOGE(TAG, "Failed to delete partial upload file: %s (errno: %d)", upload_path.c_str(), errno);
          }

          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.in_progress = false;
          this->progress_.cancelled = false;
          portEXIT_CRITICAL(&this->progress_mutex_);

          request->send(500, "application/json", "{\"error\":\"Failed to receive chunk data\"}");
          return;
        }
        total_received += ret;
      }

      data_ptr = this->chunk_buffer_;
      bytes_to_write = total_received;
    }
  }

  // Write data to file
  if (bytes_to_write > 0) {
    bool write_success = false;
    size_t written = 0;

#if defined(USE_STORAGE)
    if (this->upload_is_network_ && this->upload_network_storage_ != nullptr) {
      // Network storage - write chunk directly via NetworkStorage API
      // First chunk creates the file, subsequent chunks append at offset
      bool create = (chunk_index == 0);
      if (this->upload_network_storage_->write_file_chunk(this->upload_network_path_, data_ptr,
                                                          this->upload_network_offset_, bytes_to_write, create)) {
        write_success = true;
        written = bytes_to_write;
        this->upload_network_offset_ += bytes_to_write;
      }

      if (!write_success) {
        ESP_LOGE(TAG, "Network storage write failed for chunk %d", chunk_index);

        // Try to delete partial file
        this->upload_network_storage_->delete_file(this->upload_network_path_);

        // Clear state
        this->upload_is_network_ = false;
        this->upload_network_storage_ = nullptr;
        this->upload_network_path_.clear();
        this->upload_network_offset_ = 0;

        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
        portEXIT_CRITICAL(&this->progress_mutex_);

        request->send(500, "application/json", "{\"error\":\"Network storage write failed\"}");
        return;
      }
    } else
#endif
    {
      // Local storage - write via FILE*
      written = fwrite(data_ptr, 1, bytes_to_write, this->upload_file_);
      write_success = (written == bytes_to_write);

      if (!write_success) {
        ESP_LOGE(TAG, "Failed to write chunk %d data: wrote %zu of %zu bytes", chunk_index, written, bytes_to_write);
        fclose(this->upload_file_);
        this->upload_file_ = nullptr;

        // Delete partial file on write failure
        if (remove(upload_path.c_str()) == 0) {
          ESP_LOGI(TAG, "Deleted partial upload file after write failure: %s", upload_path.c_str());
        } else {
          ESP_LOGE(TAG, "Failed to delete partial upload file: %s (errno: %d)", upload_path.c_str(), errno);
        }

        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
        portEXIT_CRITICAL(&this->progress_mutex_);

        request->send(500, "application/json", "{\"error\":\"Write failed\"}");
        return;
      }

      // Flush periodically for local storage
      if (chunk_index % 10 == 0) {
        fflush(this->upload_file_);
      }
    }

    // Update progress
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.transferred_bytes += written;
    portEXIT_CRITICAL(&this->progress_mutex_);

    // Log only every 50th chunk, first, and last
    if (chunk_index % 50 == 0 || chunk_index == 0 || chunk_index == total_chunks - 1 ||
        (chunk_index >= 170 && chunk_index <= 180)) {
      ESP_LOGD(TAG, "Wrote chunk %d: %" PRIu32 " bytes (total: %" PRIu32 "/%" PRIu32 "), free_heap=%" PRIu32,
               chunk_index, (uint32_t) written, (uint32_t) this->progress_.transferred_bytes, (uint32_t) file_size,
               esp_get_free_heap_size());
    }
  }

  // Clear body buffer for next chunk
  this->body_buffer_.clear();

  // Last chunk - finalize
  if (chunk_index == total_chunks - 1) {
    if (this->upload_file_) {
      // Local storage - flush and close
      fflush(this->upload_file_);
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
      ESP_LOGI(TAG, "Completed chunked upload: %s (%zu bytes)", upload_path.c_str(), this->progress_.transferred_bytes);
    }
#if defined(USE_STORAGE)
    if (this->upload_is_network_) {
      // Network storage - clean up state (file already written chunk by chunk)
      ESP_LOGI(TAG, "Completed network storage chunked upload: %s (%zu bytes)", this->upload_network_path_.c_str(),
               this->progress_.transferred_bytes);
      this->upload_is_network_ = false;
      this->upload_network_storage_ = nullptr;
      this->upload_network_path_.clear();
      this->upload_network_offset_ = 0;
    }
#endif

    // Mark progress as complete
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;
    portEXIT_CRITICAL(&this->progress_mutex_);

    request->send(200, "application/json", "{\"success\":true,\"complete\":true}");
  } else {
    // Not the last chunk
    request->send(200, "application/json", "{\"success\":true,\"complete\":false}");
  }
}

void HttpFileBrowser::handle_api_exists(AsyncWebServerRequest *request) {
  // Get path parameter from query string
  auto *path_param = request->getParam("path");

  if (!path_param) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }

  // Convert URI path to filesystem path (strips URL prefix)
  std::string filepath = this->uri_to_filepath(path_param->value().c_str());

  // Check if file exists
  struct stat file_stat;
  bool exists = (stat(filepath.c_str(), &file_stat) == 0);

  std::string json = "{\"exists\":" + std::string(exists ? "true" : "false") + "}";
  request->send(200, "application/json", json.c_str());
}

void HttpFileBrowser::handle_api_dirisempty(AsyncWebServerRequest *request) {
  // Lightweight check - only checks if directory has any entries, doesn't count them
  auto *path_param = request->getParam("path");

  if (!path_param) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }

  std::string dir_uri = path_param->value().c_str();
  std::string dirpath = this->uri_to_filepath(dir_uri);

  // Check if this is network storage
  storage::NetworkStorage *net_storage = nullptr;
  if (this->storage_ != nullptr) {
    net_storage = this->storage_->find_network_storage_for_path(dirpath);
  }

  bool is_empty = false;

  if (net_storage != nullptr) {
    // Network storage - use NetworkStorage API
    std::string relative_path = strip_network_mount_prefix(net_storage, dirpath);

    // Check if it's a directory
    if (!net_storage->is_directory(relative_path)) {
      request->send(400, "application/json", "{\"error\":\"Path is not a directory\"}");
      return;
    }

    // List directory and check if empty
    std::vector<storage::NetworkStorage::DirEntry> entries;
    if (!net_storage->list_directory(relative_path, entries)) {
      request->send(404, "application/json", "{\"error\":\"Failed to list directory\"}");
      return;
    }
    is_empty = entries.empty();
  } else {
    // Local storage - use VFS
    struct stat dir_stat;
    if (stat(dirpath.c_str(), &dir_stat) != 0) {
      request->send(404, "application/json", "{\"error\":\"Path not found\"}");
      return;
    }

    if (!S_ISDIR(dir_stat.st_mode)) {
      request->send(400, "application/json", "{\"error\":\"Path is not a directory\"}");
      return;
    }

    is_empty = this->is_directory_empty(dirpath);
  }

  std::string json = "{\"is_empty\":" + std::string(is_empty ? "true" : "false") + "}";
  request->send(200, "application/json", json.c_str());
}

void HttpFileBrowser::handle_api_dirinfo(AsyncWebServerRequest *request) {
  // Get path parameter from query string
  auto *path_param = request->getParam("path");

  if (!path_param) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }

  std::string dir_uri = path_param->value().c_str();
  std::string dirpath = this->uri_to_filepath(dir_uri);

  // Check if path exists and is a directory
  struct stat dir_stat;
  if (stat(dirpath.c_str(), &dir_stat) != 0) {
    request->send(404, "application/json", "{\"error\":\"Path not found\"}");
    return;
  }

  if (!S_ISDIR(dir_stat.st_mode)) {
    request->send(400, "application/json", "{\"error\":\"Path is not a directory\"}");
    return;
  }

  // Count directory contents recursively
  int file_count = 0;
  int dir_count = 0;
  this->count_directory_contents(dirpath, file_count, dir_count);

  bool is_empty = (file_count == 0 && dir_count == 0);
  int total_items = file_count + dir_count;

  std::string json = "{";
  json += "\"is_empty\":" + std::string(is_empty ? "true" : "false");
  json += ",\"file_count\":" + std::to_string(file_count);
  json += ",\"dir_count\":" + std::to_string(dir_count);
  json += ",\"total_items\":" + std::to_string(total_items);
  json += "}";

  request->send(200, "application/json", json.c_str());
}

// Form data parsing helper
bool HttpFileBrowser::parse_json_request(const uint8_t *body, size_t body_len, ApiRequest &req) {
  // Parse URL-encoded form data
  // Format: source=/path/to/file&destination=/path/to/dest&name=newname&path=/file&mount_point=/mnt&device=/dev/sda1
  std::string form_data((const char *) body, body_len);

  // Split by '&' to get key=value pairs
  size_t pos = 0;
  while (pos < form_data.length()) {
    size_t amp_pos = form_data.find('&', pos);
    if (amp_pos == std::string::npos) {
      amp_pos = form_data.length();
    }

    std::string pair = form_data.substr(pos, amp_pos - pos);
    size_t eq_pos = pair.find('=');
    if (eq_pos != std::string::npos) {
      std::string key = pair.substr(0, eq_pos);
      std::string value = pair.substr(eq_pos + 1);

      // URL decode the value
      value = this->url_decode(value);

      if (key == "source") {
        req.source = value;
      } else if (key == "destination") {
        req.destination = value;
      } else if (key == "name") {
        req.name = value;
      } else if (key == "path") {
        req.path = value;
      } else if (key == "mount_point") {
        req.mount_point = value;
      } else if (key == "device") {
        req.device = value;
      }
    }

    pos = amp_pos + 1;
  }

  return !req.source.empty() || !req.destination.empty() || !req.name.empty() || !req.path.empty() ||
         !req.mount_point.empty() || !req.device.empty();
}

// RAII wrapper for FILE* to ensure files are always closed
struct FileCloser {
  FILE *fp;
  FileCloser(FILE *f) : fp(f) {}
  ~FileCloser() {
    if (fp) {
      fclose(fp);
    }
  }
  // Delete copy/move to prevent double-close
  FileCloser(const FileCloser &) = delete;
  FileCloser &operator=(const FileCloser &) = delete;
};

// FreeRTOS task functions for background operations
void HttpFileBrowser::copy_task(void *params) {
  auto *task_params = static_cast<CopyTaskParams *>(params);
  ESP_LOGI(TAG, "Copy task started for %s -> %s (%s)", task_params->source.c_str(), task_params->destination.c_str(),
           task_params->is_directory ? "directory" : "file");

  // Perform the copy operation
  if (task_params->is_directory) {
    task_params->server->perform_directory_copy(task_params->source, task_params->destination,
                                                task_params->track_progress);
  } else {
    task_params->server->perform_file_copy(task_params->source, task_params->destination, task_params->file_size,
                                           task_params->track_progress);
  }

  ESP_LOGI(TAG, "Copy task completed");

  // Clean up parameters
  delete task_params;

  // Delete this task
  vTaskDelete(nullptr);
}

void HttpFileBrowser::move_task(void *params) {
  auto *task_params = static_cast<MoveTaskParams *>(params);
  ESP_LOGI(TAG, "Move task started for %s -> %s (%s)", task_params->source.c_str(), task_params->destination.c_str(),
           task_params->is_directory ? "directory" : "file");

  // Perform the move operation
  if (task_params->is_directory) {
    task_params->server->perform_directory_move(task_params->source, task_params->destination,
                                                task_params->track_progress);
  } else {
    task_params->server->perform_file_move(task_params->source, task_params->destination, task_params->file_size,
                                           task_params->track_progress);
  }

  ESP_LOGI(TAG, "Move task completed");

  // Clean up parameters
  delete task_params;

  // Delete this task
  vTaskDelete(nullptr);
}

void HttpFileBrowser::handle_api_fileinfo(AsyncWebServerRequest *request) {
  // Get path parameter from query string
  auto *path_param = request->getParam("path");

  if (!path_param) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }

  std::string filepath = this->uri_to_filepath(path_param->value().c_str());

  // Check if this is network storage
  storage::NetworkStorage *net_storage = nullptr;
  if (this->storage_ != nullptr) {
    net_storage = this->storage_->find_network_storage_for_path(filepath);
  }

  struct stat file_stat;
  bool stat_ok = false;

  if (net_storage != nullptr) {
    // Network storage
    if (!net_storage->is_connected()) {
      request->send(503, "application/json", "{\"error\":\"Network storage not connected\"}");
      return;
    }
    // Strip mount prefix to get path relative to network storage root
    std::string relative_path = strip_network_mount_prefix(net_storage, filepath);
    stat_ok = net_storage->stat(relative_path, file_stat);
  } else {
    // Local storage
    stat_ok = (stat(filepath.c_str(), &file_stat) == 0);
  }

  if (!stat_ok) {
    request->send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
  }

  std::string json = "{";
  json += "\"name\":\"" + get_filename_from_path(filepath) + "\"";
  json += ",\"path\":\"" + filepath + "\"";
  json += ",\"size\":" + std::to_string(file_stat.st_size);
  json += ",\"is_directory\":" + std::string(S_ISDIR(file_stat.st_mode) ? "true" : "false");
  json += ",\"last_modified\":" + std::to_string(file_stat.st_mtime);

  // Add permissions as octal string
  json += ",\"mode\":\"" + std::to_string((file_stat.st_mode & 0777)) + "\"";
  json += ",\"uid\":" + std::to_string(file_stat.st_uid);
  json += ",\"gid\":" + std::to_string(file_stat.st_gid);

  // Add storage type
  if (net_storage != nullptr) {
    json += ",\"storage_type\":\"" + std::string(net_storage->get_protocol()) + "\"";
  } else {
    json += ",\"storage_type\":\"local\"";
  }

  json += "}";

  request->send(200, "application/json", json.c_str());
}

void HttpFileBrowser::download_task(void *params) {
  auto *task_params = static_cast<DownloadTaskParams *>(params);
  httpd_req_t *req = task_params->req;

  ESP_LOGI(TAG, "Download task started for %s (size: %zu bytes)", task_params->filename.c_str(),
           task_params->file_size);

  // Track start time for performance logging
  uint32_t start_time = millis();

  // Open the file
  FILE *file = fopen(task_params->filepath.c_str(), "rb");
  if (!file) {
    ESP_LOGE(TAG, "Failed to open file for download: %s", task_params->filepath.c_str());
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    // Notify caller task if it's waiting
    if (task_params->caller_task) {
      xTaskNotifyGive(task_params->caller_task);
    }
    delete task_params;
    vTaskDelete(nullptr);
    return;
  }

  // Set response headers
  std::string content_disposition = "attachment; filename=\"" + task_params->filename + "\"";
  std::string content_length = std::to_string(task_params->file_size);

  httpd_resp_set_type(req, task_params->mime_type.c_str());
  httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
  httpd_resp_set_hdr(req, "Content-Length", content_length.c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  // Allocate download buffer for this task
  // Use architecture-specific buffer size (4KB/8KB/16KB/32KB/64KB based on ESP32 variant and PSRAM)
#if defined(USE_PSRAM) && defined(USE_ESP_IDF)
  uint8_t *buffer = (uint8_t *) heap_caps_malloc(FILE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    buffer = (uint8_t *) malloc(FILE_BUFFER_SIZE);
  }
#else
  uint8_t *buffer = (uint8_t *) malloc(FILE_BUFFER_SIZE);
#endif

  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate download buffer");
    fclose(file);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    if (task_params->caller_task) {
      xTaskNotifyGive(task_params->caller_task);
    }
    delete task_params;
    vTaskDelete(nullptr);
    return;
  }

  size_t total_sent = 0;
  bool success = true;

  while (total_sent < task_params->file_size) {
    App.feed_wdt();  // Feed watchdog for large files

    size_t to_read = std::min(FILE_BUFFER_SIZE, task_params->file_size - total_sent);
    size_t bytes_read = fread(buffer, 1, to_read, file);

    if (bytes_read == 0) {
      ESP_LOGE(TAG, "Failed to read chunk at offset %zu", total_sent);
      success = false;
      break;
    }

    esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read);
    if (err != ESP_OK) {
      ESP_LOGI(TAG, "Download cancelled or connection closed at %zu / %zu bytes (%s)", total_sent,
               task_params->file_size, esp_err_to_name(err));
      success = false;
      break;
    }

    total_sent += bytes_read;

    // Log progress every ~5MB for less verbose logging
    if (total_sent % (5 * 1024 * 1024) < FILE_BUFFER_SIZE) {
      ESP_LOGI(TAG, "Download progress: %zu / %zu MB (%.1f%%)", total_sent / (1024 * 1024),
               task_params->file_size / (1024 * 1024), (float) total_sent / task_params->file_size * 100.0f);
    }

    // Yield every 4 chunks to balance responsiveness and throughput
    if ((total_sent / FILE_BUFFER_SIZE) % 4 == 0) {
      vTaskDelay(1);  // 1 tick = ~10ms
    }
  }

  fclose(file);

  // Send final empty chunk to signal completion
  if (success) {
    httpd_resp_send_chunk(req, nullptr, 0);
    uint32_t elapsed_ms = millis() - start_time;
    float elapsed_sec = elapsed_ms / 1000.0f;
    float speed_mbps = (total_sent / (1024.0f * 1024.0f)) / (elapsed_sec > 0 ? elapsed_sec : 1.0f);
    ESP_LOGI(TAG, "Download completed: %zu MB in %.1f seconds (%.2f MB/s)", total_sent / (1024 * 1024), elapsed_sec,
             speed_mbps);
  } else {
    ESP_LOGW(TAG, "Download incomplete: %zu / %zu bytes (%.1f%%)", total_sent, task_params->file_size,
             (float) total_sent / task_params->file_size * 100.0f);
  }

  // Free download buffer
  free(buffer);

  // Notify caller task if it's waiting
  if (task_params->caller_task) {
    xTaskNotifyGive(task_params->caller_task);
  }

  // Clean up parameters
  delete task_params;

  // Delete this task
  vTaskDelete(nullptr);
}

// File operation helpers (reused from WebDAV logic)
bool HttpFileBrowser::perform_file_copy(const std::string &src_path, const std::string &dst_path, off_t file_size,
                                        bool track_progress) {
  // Initialize progress tracking if requested (only if not already in progress)
  // This allows perform_file_move to set up progress as "move" before calling this function
  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    if (!this->progress_.in_progress) {
      this->progress_.operation = "copy";
      this->progress_.source = src_path;
      this->progress_.destination = dst_path;
      this->progress_.total_bytes = file_size;
      this->progress_.transferred_bytes = 0;
      this->progress_.in_progress = true;
      this->progress_.cancelled = false;
      this->progress_.start_time = millis();
    }
    portEXIT_CRITICAL(&this->progress_mutex_);
  }

  // Check if source or destination is on network storage
  storage::NetworkStorage *src_net_storage = nullptr;
  storage::NetworkStorage *dst_net_storage = nullptr;
  if (this->storage_ != nullptr) {
    src_net_storage = this->storage_->find_network_storage_for_path(src_path);
    dst_net_storage = this->storage_->find_network_storage_for_path(dst_path);
  }

  // Handle network storage copy operations (chunked for memory efficiency)
  if (src_net_storage != nullptr || dst_net_storage != nullptr) {
    ESP_LOGI(TAG, "Starting network storage file copy: %s -> %s (size: %lld bytes)", src_path.c_str(), dst_path.c_str(),
             (long long) file_size);

    // Strip mount prefixes for network storage operations
    std::string src_relative = src_net_storage ? strip_network_mount_prefix(src_net_storage, src_path) : src_path;
    std::string dst_relative = dst_net_storage ? strip_network_mount_prefix(dst_net_storage, dst_path) : dst_path;

    // Use chunked copy to avoid loading entire file into memory
    // Use preallocated copy buffer from buffer pool
    uint8_t *buffer = this->copy_buffer_;
    size_t total_copied = 0;
    bool copy_success = true;
    bool first_chunk = true;

    // For local source, open file
    FILE *src_file = nullptr;
    if (src_net_storage == nullptr) {
      src_file = fopen(src_path.c_str(), "rb");
      if (!src_file) {
        ESP_LOGE(TAG, "Failed to open source file: %s", src_path.c_str());
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.in_progress = false;
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
        return false;
      }
    }

    // For local destination, open file
    FILE *dst_file = nullptr;
    if (dst_net_storage == nullptr) {
      dst_file = fopen(dst_path.c_str(), "wb");
      if (!dst_file) {
        ESP_LOGE(TAG, "Failed to open destination file: %s", dst_path.c_str());
        if (src_file)
          fclose(src_file);
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.in_progress = false;
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
        return false;
      }
    }

    while (copy_success) {
      App.feed_wdt();

      // Check for cancellation
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        bool cancelled = this->progress_.cancelled;
        portEXIT_CRITICAL(&this->progress_mutex_);
        if (cancelled) {
          ESP_LOGI(TAG, "Copy cancelled at %zu bytes", total_copied);
          copy_success = false;
          break;
        }
      }

      // Read chunk
      size_t bytes_read = 0;
      if (src_net_storage != nullptr) {
        if (!src_net_storage->read_file_chunk(src_relative, buffer, total_copied, FILE_BUFFER_SIZE, bytes_read)) {
          ESP_LOGE(TAG, "Failed to read chunk at offset %zu", total_copied);
          copy_success = false;
          break;
        }
      } else {
        bytes_read = fread(buffer, 1, FILE_BUFFER_SIZE, src_file);
        if (bytes_read == 0 && ferror(src_file)) {
          ESP_LOGE(TAG, "Failed to read from source file");
          copy_success = false;
          break;
        }
      }

      if (bytes_read == 0) {
        // EOF
        break;
      }

      // Write chunk
      if (dst_net_storage != nullptr) {
        if (!dst_net_storage->write_file_chunk(dst_relative, buffer, total_copied, bytes_read, first_chunk)) {
          ESP_LOGE(TAG, "Failed to write chunk at offset %zu", total_copied);
          copy_success = false;
          break;
        }
      } else {
        size_t written = fwrite(buffer, 1, bytes_read, dst_file);
        if (written != bytes_read) {
          ESP_LOGE(TAG, "Failed to write to destination file");
          copy_success = false;
          break;
        }
      }

      total_copied += bytes_read;
      first_chunk = false;

      // Update progress
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.transferred_bytes = total_copied;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    }

    // Cleanup
    if (src_file)
      fclose(src_file);
    if (dst_file)
      fclose(dst_file);

    if (copy_success) {
      ESP_LOGI(TAG, "Network storage file copy completed: %zu bytes", total_copied);
    }

    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return copy_success;
  }

  // Local storage copy (both source and destination are local)
  FILE *src = fopen(src_path.c_str(), "rb");
  if (!src) {
    ESP_LOGE(TAG, "Failed to open source file: %s (errno: %d, %s)", src_path.c_str(), errno, strerror(errno));
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return false;
  }
  FileCloser src_closer(src);  // RAII: will close src on scope exit

  FILE *dst = fopen(dst_path.c_str(), "wb");
  if (!dst) {
    ESP_LOGE(TAG, "Failed to open destination file: %s (errno: %d, %s)", dst_path.c_str(), errno, strerror(errno));
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return false;
  }
  FileCloser dst_closer(dst);  // RAII: will close dst on scope exit

  // Use copy buffer from buffer pool (allocated in setup())
  uint8_t *buffer = this->copy_buffer_;
  size_t bytes_read;
  size_t total_copied = 0;
  bool copy_success = true;
  size_t buffer_count = 0;  // Counter for periodic operations (cheaper than modulo on large numbers)

  ESP_LOGI(TAG, "Starting file copy: %s -> %s (size: %lld bytes)", src_path.c_str(), dst_path.c_str(),
           (long long) file_size);

  while ((bytes_read = fread(buffer, 1, FILE_BUFFER_SIZE, src)) > 0) {
    // Check for cancellation (thread-safe)
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      bool cancelled = this->progress_.cancelled;
      portEXIT_CRITICAL(&this->progress_mutex_);

      if (cancelled) {
        ESP_LOGI(TAG, "Copy operation cancelled by user at %zu bytes", total_copied);
        copy_success = false;
        break;
      }
    }

    size_t bytes_written = fwrite(buffer, 1, bytes_read, dst);
    if (bytes_written != bytes_read) {
      ESP_LOGE(TAG, "Write failed at offset %zu (errno: %d, %s)", total_copied, errno, strerror(errno));
      copy_success = false;
      break;
    }
    total_copied += bytes_written;
    buffer_count++;

    // Update progress (thread-safe)
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.transferred_bytes = total_copied;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }

    // Yield to other tasks periodically to allow web server to respond to progress polls
    // Yield every 16 buffers (~1MB with 64KB chunks)
    if (buffer_count % 16 == 0) {
      vTaskDelay(0);  // Yield to higher priority tasks only (no time delay)

      // Log progress for large files
      if (file_size > 0) {
        ESP_LOGD(TAG, "Copy progress: %zu / %lld bytes (%.1f%%)", total_copied, (long long) file_size,
                 (total_copied * 100.0) / file_size);
      }
    }
  }

  // Check for read errors
  if (ferror(src)) {
    ESP_LOGE(TAG, "Read error at offset %zu (errno: %d, %s)", total_copied, errno, strerror(errno));
    copy_success = false;
  }

  // Flush destination file before closing
  if (fflush(dst) != 0) {
    ESP_LOGE(TAG, "Flush failed (errno: %d, %s)", errno, strerror(errno));
    copy_success = false;
  }

  // Files will be automatically closed by RAII wrappers (FileCloser destructors)
  // This happens even if HTTP connection times out or handler is interrupted

  if (copy_success && total_copied == static_cast<size_t>(file_size)) {
    ESP_LOGI(TAG, "File copy completed successfully: %zu bytes", total_copied);
    // Clear progress tracking on success (unless perform_file_move will handle it)
    // Note: perform_file_move sets operation to "move" and will clear progress itself
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.operation == "copy") {
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;  // Reset cancelled flag after handling
      }
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return true;
  } else {
    ESP_LOGE(TAG, "File copy failed: %s to %s (copied %zu of %lld bytes)", src_path.c_str(), dst_path.c_str(),
             total_copied, (long long) file_size);

    // Clean up partial file
    if (remove(dst_path.c_str()) == 0) {
      ESP_LOGI(TAG, "Deleted partial destination file: %s", dst_path.c_str());
    } else {
      ESP_LOGE(TAG, "Failed to remove partial destination file: %s (errno: %d, %s)", dst_path.c_str(), errno,
               strerror(errno));
    }

    // Clear progress tracking on failure (unless perform_file_move will handle it)
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.operation == "copy") {
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;  // Reset cancelled flag after handling
      }
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return false;
  }
}

bool HttpFileBrowser::perform_file_move(const std::string &src_path, const std::string &dst_path, off_t file_size,
                                        bool track_progress) {
  // Initialize progress tracking if requested
  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.operation = "move";
    this->progress_.source = src_path;
    this->progress_.destination = dst_path;
    this->progress_.total_bytes = file_size;
    this->progress_.transferred_bytes = 0;
    this->progress_.in_progress = true;
    this->progress_.cancelled = false;
    this->progress_.start_time = millis();
    portEXIT_CRITICAL(&this->progress_mutex_);
  }

  // Check if source or destination is on network storage
  storage::NetworkStorage *src_net_storage = nullptr;
  storage::NetworkStorage *dst_net_storage = nullptr;
  if (this->storage_ != nullptr) {
    src_net_storage = this->storage_->find_network_storage_for_path(src_path);
    dst_net_storage = this->storage_->find_network_storage_for_path(dst_path);
  }

  bool is_network_move = (src_net_storage != nullptr || dst_net_storage != nullptr);

  // Try atomic rename first (local-to-local)
  if (!is_network_move && rename(src_path.c_str(), dst_path.c_str()) == 0) {
    ESP_LOGI(TAG, "File moved successfully (atomic rename)");
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.transferred_bytes = file_size;
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;  // Reset cancelled flag after handling
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return true;
  }

  // If both source and destination are on the same network storage, try native rename
  if (src_net_storage != nullptr && dst_net_storage != nullptr && src_net_storage == dst_net_storage) {
    ESP_LOGI(TAG, "Same network storage detected, using native rename");
    std::string src_relative = strip_network_mount_prefix(src_net_storage, src_path);
    std::string dst_relative = strip_network_mount_prefix(dst_net_storage, dst_path);
    if (src_net_storage->rename_path(src_relative, dst_relative)) {
      ESP_LOGI(TAG, "File moved successfully (network native rename)");
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.transferred_bytes = file_size;
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
      return true;
    }
    ESP_LOGW(TAG, "Native rename failed or not supported, falling back to copy+delete");
  }

  // If network storage or rename failed with EXDEV (cross-device), use copy+delete fallback
  if (is_network_move || errno == EXDEV) {
    if (is_network_move) {
      ESP_LOGI(TAG, "Cross-storage move detected, using copy+delete");
    } else {
      ESP_LOGI(TAG, "Cross-mount move detected, using copy+delete fallback");
    }

    // Note: perform_file_copy will handle progress tracking if track_progress is true
    if (perform_file_copy(src_path, dst_path, file_size, track_progress)) {
      // Delete source file using helper that supports network storage
      if (this->delete_path(src_path, false)) {
        ESP_LOGI(TAG, "File move completed successfully");
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.in_progress = false;
          this->progress_.cancelled = false;  // Reset cancelled flag after handling
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
        return true;
      } else {
        ESP_LOGE(TAG, "Failed to delete source after copy: %s", src_path.c_str());
        // File was copied but source remains - still consider success
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.in_progress = false;
          this->progress_.cancelled = false;  // Reset cancelled flag after handling
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
        return true;
      }
    } else {
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;  // Reset cancelled flag after handling
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    }
  } else {
    ESP_LOGE(TAG, "Failed to move %s to %s (errno: %d, %s)", src_path.c_str(), dst_path.c_str(), errno,
             strerror(errno));
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;  // Reset cancelled flag after handling
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
  }

  return false;
}

bool HttpFileBrowser::perform_directory_copy(const std::string &src_path, const std::string &dst_path,
                                             bool track_progress) {
  ESP_LOGI(TAG, "Starting recursive directory copy: %s -> %s", src_path.c_str(), dst_path.c_str());

  // Initialize progress tracking
  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.operation = "copy";
    this->progress_.source = src_path;
    this->progress_.destination = dst_path;
    this->progress_.total_bytes = 0;  // Will be updated as we discover files
    this->progress_.transferred_bytes = 0;
    this->progress_.in_progress = true;
    this->progress_.cancelled = false;
    this->progress_.start_time = millis();
    portEXIT_CRITICAL(&this->progress_mutex_);
  }

  // Check for network storage
  storage::NetworkStorage *src_net_storage = nullptr;
  storage::NetworkStorage *dst_net_storage = nullptr;
  if (this->storage_ != nullptr) {
    src_net_storage = this->storage_->find_network_storage_for_path(src_path);
    dst_net_storage = this->storage_->find_network_storage_for_path(dst_path);
  }

  // Create destination directory
  if (!this->create_dir(dst_path)) {
    ESP_LOGE(TAG, "Failed to create destination directory: %s", dst_path.c_str());
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return false;
  }

  bool success = true;

  // List source directory entries
  if (src_net_storage != nullptr) {
    // Network storage source
    std::string src_relative = strip_network_mount_prefix(src_net_storage, src_path);
    std::vector<storage::NetworkStorage::DirEntry> entries;
    if (!src_net_storage->list_directory(src_relative, entries)) {
      ESP_LOGE(TAG, "Failed to list source directory: %s", src_path.c_str());
      success = false;
    } else {
      for (const auto &entry : entries) {
        App.feed_wdt();

        // Check for cancellation
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          bool cancelled = this->progress_.cancelled;
          portEXIT_CRITICAL(&this->progress_mutex_);
          if (cancelled) {
            ESP_LOGI(TAG, "Directory copy cancelled");
            success = false;
            break;
          }
        }

        std::string src_entry_path = src_path + "/" + entry.name;
        std::string dst_entry_path = dst_path + "/" + entry.name;

        if (entry.is_directory) {
          // Recursively copy subdirectory (don't track progress for nested calls)
          if (!this->perform_directory_copy(src_entry_path, dst_entry_path, false)) {
            ESP_LOGE(TAG, "Failed to copy subdirectory: %s", src_entry_path.c_str());
            success = false;
            break;
          }
        } else {
          // Copy file (don't track progress for individual files in directory copy)
          if (!this->perform_file_copy(src_entry_path, dst_entry_path, entry.size, false)) {
            ESP_LOGE(TAG, "Failed to copy file: %s", src_entry_path.c_str());
            success = false;
            break;
          }
          // Update progress
          if (track_progress) {
            portENTER_CRITICAL(&this->progress_mutex_);
            this->progress_.transferred_bytes += entry.size;
            portEXIT_CRITICAL(&this->progress_mutex_);
          }
        }
      }
    }
  } else {
    // Local VFS source
    DIR *dir = opendir(src_path.c_str());
    if (!dir) {
      ESP_LOGE(TAG, "Failed to open source directory: %s", src_path.c_str());
      success = false;
    } else {
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr && success) {
        App.feed_wdt();

        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
          continue;
        }

        // Check for cancellation
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          bool cancelled = this->progress_.cancelled;
          portEXIT_CRITICAL(&this->progress_mutex_);
          if (cancelled) {
            ESP_LOGI(TAG, "Directory copy cancelled");
            success = false;
            break;
          }
        }

        std::string src_entry_path = src_path + "/" + entry->d_name;
        std::string dst_entry_path = dst_path + "/" + entry->d_name;

        struct stat entry_stat;
        if (stat(src_entry_path.c_str(), &entry_stat) != 0) {
          ESP_LOGE(TAG, "Failed to stat: %s", src_entry_path.c_str());
          success = false;
          break;
        }

        if (S_ISDIR(entry_stat.st_mode)) {
          // Recursively copy subdirectory
          if (!this->perform_directory_copy(src_entry_path, dst_entry_path, false)) {
            ESP_LOGE(TAG, "Failed to copy subdirectory: %s", src_entry_path.c_str());
            success = false;
            break;
          }
        } else {
          // Copy file
          if (!this->perform_file_copy(src_entry_path, dst_entry_path, entry_stat.st_size, false)) {
            ESP_LOGE(TAG, "Failed to copy file: %s", src_entry_path.c_str());
            success = false;
            break;
          }
          // Update progress
          if (track_progress) {
            portENTER_CRITICAL(&this->progress_mutex_);
            this->progress_.transferred_bytes += entry_stat.st_size;
            portEXIT_CRITICAL(&this->progress_mutex_);
          }
        }
      }
      closedir(dir);
    }
  }

  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
  }

  if (success) {
    ESP_LOGI(TAG, "Directory copy completed: %s -> %s", src_path.c_str(), dst_path.c_str());
  }
  return success;
}

bool HttpFileBrowser::perform_directory_move(const std::string &src_path, const std::string &dst_path,
                                             bool track_progress) {
  ESP_LOGI(TAG, "Starting directory move: %s -> %s", src_path.c_str(), dst_path.c_str());

  // Check for network storage
  storage::NetworkStorage *src_net_storage = nullptr;
  storage::NetworkStorage *dst_net_storage = nullptr;
  if (this->storage_ != nullptr) {
    src_net_storage = this->storage_->find_network_storage_for_path(src_path);
    dst_net_storage = this->storage_->find_network_storage_for_path(dst_path);
  }

  bool is_network_move = (src_net_storage != nullptr || dst_net_storage != nullptr);

  // Try atomic rename first
  if (!is_network_move && rename(src_path.c_str(), dst_path.c_str()) == 0) {
    ESP_LOGI(TAG, "Directory moved successfully (atomic rename)");
    return true;
  }

  // If both source and destination are on the same network storage, try native rename
  if (src_net_storage != nullptr && dst_net_storage != nullptr && src_net_storage == dst_net_storage) {
    ESP_LOGI(TAG, "Same network storage detected, using native rename");
    std::string src_relative = strip_network_mount_prefix(src_net_storage, src_path);
    std::string dst_relative = strip_network_mount_prefix(dst_net_storage, dst_path);
    if (src_net_storage->rename_path(src_relative, dst_relative)) {
      ESP_LOGI(TAG, "Directory moved successfully (network native rename)");
      return true;
    }
    ESP_LOGW(TAG, "Native rename failed or not supported, falling back to copy+delete");
  }

  // Use copy+delete for network storage or cross-mount moves
  if (is_network_move || errno == EXDEV) {
    if (is_network_move) {
      ESP_LOGI(TAG, "Network storage directory move, using copy+delete");
    } else {
      ESP_LOGI(TAG, "Cross-mount directory move, using copy+delete");
    }

    // Copy the entire directory tree
    if (!this->perform_directory_copy(src_path, dst_path, track_progress)) {
      ESP_LOGE(TAG, "Failed to copy directory for move operation");
      return false;
    }

    // Delete the source directory tree
    if (src_net_storage != nullptr) {
      std::string src_relative = strip_network_mount_prefix(src_net_storage, src_path);
      if (!this->recursive_delete_directory_network(src_net_storage, src_relative, false)) {
        ESP_LOGE(TAG, "Failed to delete source directory after copy (network): %s", src_path.c_str());
        // Directory was copied but source remains - consider partial success
      }
    } else {
      if (!this->recursive_delete_directory(src_path, false)) {
        ESP_LOGE(TAG, "Failed to delete source directory after copy: %s", src_path.c_str());
        // Directory was copied but source remains - consider partial success
      }
    }

    ESP_LOGI(TAG, "Directory move completed: %s -> %s", src_path.c_str(), dst_path.c_str());
    return true;
  }

  ESP_LOGE(TAG, "Failed to move directory %s to %s (errno: %d)", src_path.c_str(), dst_path.c_str(), errno);
  return false;
}

// Path utility implementations
std::string Path::file_name(const std::string &path) {
  size_t pos = path.rfind(separator);
  if (pos != std::string::npos) {
    return path.substr(pos + 1);
  }
  return path;
}

std::string Path::dirname(const std::string &path) {
  // Find the last separator
  size_t pos = path.rfind(separator);
  if (pos != std::string::npos) {
    // If separator found, return everything before it
    // Special case: if pos is 0, return "/" (root directory)
    if (pos == 0) {
      return "/";
    }
    return path.substr(0, pos);
  }
  // No separator found - return current directory
  return ".";
}

bool Path::is_absolute(const std::string &path) { return !path.empty() && path[0] == separator; }

bool Path::has_trailing_slash(const std::string &path) { return !path.empty() && path.back() == separator; }

std::string Path::join(const std::string &first, const std::string &second) {
  if (first.empty())
    return second;
  if (second.empty())
    return first;

  std::string result = first;
  if (!has_trailing_slash(first) && !is_absolute(second)) {
    result.push_back(separator);
  }
  if (has_trailing_slash(first) && is_absolute(second)) {
    result.pop_back();
  }
  result.append(second);
  return result;
}

std::string Path::remove_root_path(const std::string &path, const std::string &root) {
  if (path.find(root) != 0)
    return path;
  if (path.size() == root.size())
    return "/";
  if (path.size() < root.size())
    return "/";

  std::string result = path.substr(root.size());
  if (result.empty() || result[0] != '/')
    result = "/" + result;
  return result;
}

std::vector<std::string> Path::split_path(const std::string &path) {
  std::vector<std::string> parts;
  std::string current_path = path;
  size_t pos = 0;

  while ((pos = current_path.find(separator)) != std::string::npos) {
    std::string part = current_path.substr(0, pos);
    if (!part.empty()) {
      parts.push_back(part);
    }
    current_path.erase(0, pos + 1);
  }

  if (!current_path.empty()) {
    parts.push_back(current_path);
  }

  return parts;
}

std::string Path::extension(const std::string &file) {
  size_t pos = file.find_last_of('.');
  if (pos == std::string::npos || pos == file.size() - 1)
    return "";
  return file.substr(pos + 1);
}

std::string Path::file_type(const std::string &file) {
  static const std::array<std::pair<const char *, const char *>, 20> file_types = {{
      {"mp3", "Audio (MP3)"},   {"wav", "Audio (WAV)"},   {"flac", "Audio (FLAC)"}, {"png", "Image (PNG)"},
      {"jpg", "Image (JPG)"},   {"jpeg", "Image (JPEG)"}, {"gif", "Image (GIF)"},   {"bmp", "Image (BMP)"},
      {"txt", "Text (TXT)"},    {"log", "Text (LOG)"},    {"csv", "Text (CSV)"},    {"html", "Web (HTML)"},
      {"css", "Web (CSS)"},     {"js", "Web (JS)"},       {"json", "Data (JSON)"},  {"xml", "Data (XML)"},
      {"zip", "Archive (ZIP)"}, {"gz", "Archive (GZ)"},   {"tar", "Archive (TAR)"}, {"mp4", "Video (MP4)"},
  }};

  std::string ext = extension(file);
  if (ext.empty())
    return "File";

  // Convert to lowercase
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

  for (const auto &[file_ext, file_type_str] : file_types) {
    if (ext == file_ext)
      return file_type_str;
  }

  return "File (" + ext + ")";
}

std::string Path::mime_type(const std::string &file) {
  static const std::array<std::pair<const char *, const char *>, 25> mime_types = {{
      {"mp3", "audio/mpeg"},        {"wav", "audio/wav"},       {"flac", "audio/flac"},
      {"png", "image/png"},         {"jpg", "image/jpeg"},      {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},         {"bmp", "image/bmp"},       {"txt", "text/plain"},
      {"log", "text/plain"},        {"csv", "text/csv"},        {"html", "text/html"},
      {"htm", "text/html"},         {"css", "text/css"},        {"js", "application/javascript"},
      {"json", "application/json"}, {"xml", "application/xml"}, {"pdf", "application/pdf"},
      {"zip", "application/zip"},   {"gz", "application/gzip"}, {"tar", "application/x-tar"},
      {"mp4", "video/mp4"},         {"avi", "video/x-msvideo"}, {"webm", "video/webm"},
      {"mkv", "video/x-matroska"},
  }};

  std::string ext = extension(file);
  if (ext.empty())
    return "application/octet-stream";

  // Convert to lowercase
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

  for (const auto &[file_ext, mime] : mime_types) {
    if (ext == file_ext)
      return mime;
  }

  return "application/octet-stream";
}

}  // namespace http_file_browser
}  // namespace esphome
