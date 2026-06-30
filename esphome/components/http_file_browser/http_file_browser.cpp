#include "http_file_browser.h"
#include "esphome/components/storage/storage.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"

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

namespace esphome::http_file_browser {

void HttpFileBrowser::setup() {
  ESP_LOGI(TAG, "Setting up HTTP File Browser with prefix: %s", this->url_prefix_.c_str());
  ESP_LOGI(TAG, "Root path: %s", this->root_path_.c_str());
  ESP_LOGI(TAG, "Upload: %s, Download: %s, Delete: %s", this->upload_enabled_ ? "YES" : "NO",
           this->download_enabled_ ? "YES" : "NO", this->deletion_enabled_ ? "YES" : "NO");

  this->registry_ = storage::global_storage_registry;

  this->buffer_pool_size_ = FILE_BUFFER_SIZE * 3;

#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
  this->buffer_pool_ = (uint8_t *) heap_caps_malloc(this->buffer_pool_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->buffer_pool_ != nullptr) {
    ESP_LOGI(TAG, "Allocated %zu byte buffer pool from PSRAM (%zu KB per buffer)", this->buffer_pool_size_,
             FILE_BUFFER_SIZE / 1024);
  } else {
    ESP_LOGW(TAG, "PSRAM allocation failed, using heap for buffer pool");
    this->buffer_pool_ = (uint8_t *) malloc(this->buffer_pool_size_);
  }
#else
  this->buffer_pool_ = (uint8_t *) malloc(this->buffer_pool_size_);
  ESP_LOGI(TAG, "Allocated %zu byte buffer pool from heap (%zu KB per buffer)", this->buffer_pool_size_,
           FILE_BUFFER_SIZE / 1024);
#endif

  if (this->buffer_pool_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate buffer pool!");
    this->mark_failed();
    return;
  }

  this->upload_buffer_ = this->buffer_pool_;
  this->download_buffer_ = this->buffer_pool_ + FILE_BUFFER_SIZE;
  this->copy_buffer_ = this->buffer_pool_ + (FILE_BUFFER_SIZE * 2);

  auto server = this->base_->get_server();
  if (server) {
    server->addHandler(this);
    ESP_LOGI(TAG, "HTTP File Browser registered successfully");
  } else {
    ESP_LOGE(TAG, "Failed to get web server instance");
  }
}

HttpFileBrowser::~HttpFileBrowser() {
  if (this->buffer_pool_ != nullptr) {
    free(this->buffer_pool_);
    this->buffer_pool_ = nullptr;
    this->upload_buffer_ = nullptr;
    this->download_buffer_ = nullptr;
    this->copy_buffer_ = nullptr;
  }
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
  if (this->deferred_mount_op_.type == DeferredMountOp::NONE)
    return;
  if (millis() < this->deferred_mount_op_.schedule_time)
    return;

  std::string mount_point = this->deferred_mount_op_.mount_point;
  DeferredMountOp::Type op_type = this->deferred_mount_op_.type;

  this->deferred_mount_op_.type = DeferredMountOp::NONE;
  this->deferred_mount_op_.mount_point.clear();

  ESP_LOGI(TAG, "Executing deferred %s for mount point: %s",
           op_type == DeferredMountOp::MOUNT     ? "MOUNT"
           : op_type == DeferredMountOp::UNMOUNT ? "UNMOUNT"
                                                 : "REMOUNT",
           mount_point.c_str());

  storage::FilesystemStorage *fs = this->find_filesystem_for_path_(mount_point);
  if (fs == nullptr) {
    ESP_LOGW(TAG, "No filesystem storage found for mount point %s", mount_point.c_str());
    return;
  }

  storage::StorageError err = storage::StorageError::OK;
  if (op_type == DeferredMountOp::UNMOUNT) {
    err = fs->unmount();
    if (err == storage::StorageError::OK) {
      ESP_LOGI(TAG, "Unmounted %s", mount_point.c_str());
    } else {
      ESP_LOGE(TAG, "Unmount failed for %s", mount_point.c_str());
    }
  } else if (op_type == DeferredMountOp::MOUNT) {
    err = fs->mount();
    if (err == storage::StorageError::OK) {
      ESP_LOGI(TAG, "Mounted %s", mount_point.c_str());
    } else {
      ESP_LOGE(TAG, "Mount failed for %s", mount_point.c_str());
    }
  } else {
    // REMOUNT: unmount then mount
    fs->unmount();
    err = fs->mount();
    if (err == storage::StorageError::OK) {
      ESP_LOGI(TAG, "Remounted %s", mount_point.c_str());
    } else {
      ESP_LOGE(TAG, "Remount failed for %s", mount_point.c_str());
    }
  }
}

// Storage routing helpers
storage::NetworkStorage *HttpFileBrowser::find_network_for_path_(const std::string &path) {
  struct Ctx {
    const std::string &path;
    storage::NetworkStorage *result;
  } ctx{path, nullptr};

  if (this->registry_ == nullptr)
    return nullptr;

  this->registry_->for_each_network(
      [](storage::NetworkStorage *s, void *c) {
        auto *ctx = static_cast<Ctx *>(c);
        if (ctx->result != nullptr)
          return;
        storage::StorageInfo info{};
        if (s->get_info(&info) != storage::StorageError::OK || info.id == nullptr)
          return;
        std::string mount(info.id);
        if (ctx->path.compare(0, mount.size(), mount) == 0) {
          ctx->result = s;
        }
      },
      &ctx);

  return ctx.result;
}

storage::FilesystemStorage *HttpFileBrowser::find_filesystem_for_path_(const std::string &path) {
  struct Ctx {
    const std::string &path;
    storage::FilesystemStorage *result;
  } ctx{path, nullptr};

  if (this->registry_ == nullptr)
    return nullptr;

  this->registry_->for_each_filesystem(
      [](storage::FilesystemStorage *s, void *c) {
        auto *ctx = static_cast<Ctx *>(c);
        if (ctx->result != nullptr)
          return;
        storage::StorageInfo info{};
        if (s->get_info(&info) != storage::StorageError::OK || info.id == nullptr)
          return;
        std::string mount(info.id);
        if (ctx->path.compare(0, mount.size(), mount) == 0) {
          ctx->result = s;
        }
      },
      &ctx);

  return ctx.result;
}

std::string HttpFileBrowser::get_network_mount_path(storage::NetworkStorage *net_storage) {
  storage::StorageInfo info{};
  if (net_storage->get_info(&info) == storage::StorageError::OK && info.id != nullptr)
    return std::string(info.id);
  return {};
}

std::string HttpFileBrowser::strip_network_mount_prefix(storage::NetworkStorage *net_storage,
                                                         const std::string &path) {
  std::string mount_path = get_network_mount_path(net_storage);
  if (!mount_path.empty() && path.compare(0, mount_path.size(), mount_path) == 0) {
    std::string relative = path.substr(mount_path.size());
    if (relative.empty() || relative[0] != '/')
      relative = "/" + relative;
    return relative;
  }
  return path;
}


// AsyncWebHandler interface
bool HttpFileBrowser::canHandle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef uri = request->url_to(url_buf);
  if (uri.find(this->url_prefix_.c_str()) != 0)
    return false;
  if (request->method() == HTTP_GET || request->method() == HTTP_POST || request->method() == HTTP_DELETE)
    return true;
  return false;
}

void HttpFileBrowser::handleRequest(AsyncWebServerRequest *request) {
  if (this->auth_enabled_) {
    if (!request->authenticate(this->username_.c_str(), this->password_.c_str()))
      return request->requestAuthentication();
  }

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef uri = request->url_to(url_buf);

  if (uri.find((this->url_prefix_ + "/api/copy").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_copy(request);
  } else if (uri.find((this->url_prefix_ + "/api/move").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_move(request);
  } else if (uri.find((this->url_prefix_ + "/api/rename").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_rename(request);
  } else if (uri.find((this->url_prefix_ + "/api/mkdir").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_mkdir(request);
  } else if (uri.find((this->url_prefix_ + "/api/delete").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_delete(request);
  } else if (uri.find((this->url_prefix_ + "/api/mount").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_mount(request);
  } else if (uri.find((this->url_prefix_ + "/api/unmount").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_unmount(request);
  } else if (uri.find((this->url_prefix_ + "/api/remount").c_str()) == 0 && request->method() == HTTP_POST) {
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
    this->handle_api_cancel(request);
  } else if (uri.find((this->url_prefix_ + "/api/upload_chunk").c_str()) == 0 && request->method() == HTTP_POST) {
    this->handle_api_upload_chunk(request);
  } else if (request->method() == HTTP_GET) {
    std::string filepath = this->uri_to_filepath(uri);
    if (filepath.empty()) {
      request->send(400, "text/plain", "Bad Request: Invalid file path");
      return;
    }
    bool is_virtual_root = (this->root_path_ == "/" && filepath == "/");
    storage::NetworkStorage *net_storage = this->find_network_for_path_(filepath);

    if (net_storage != nullptr) {
      storage::StorageInfo info{};
      net_storage->get_info(&info);
      if (!info.is_mounted) {
        request->send(503, "text/plain", "Service Unavailable: Network storage not connected");
        return;
      }
      std::string mount_path = get_network_mount_path(net_storage);
      if (mount_path == filepath) {
        this->handle_network_directory_listing(request, net_storage, filepath);
        return;
      }
      storage::FileStat fs{};
      std::string relative = strip_network_mount_prefix(net_storage, filepath);
      if (net_storage->stat(relative.c_str(), &fs) != storage::StorageError::OK) {
        request->send(404, "text/plain", "Not Found");
        return;
      }
      if (fs.is_dir) {
        this->handle_network_directory_listing(request, net_storage, filepath);
        return;
      }
    }

    struct stat file_stat;
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
    auto content_type = request->get_header("Content-Type");
    if (content_type.has_value() && content_type.value().find("multipart/form-data") != std::string::npos)
      return;
    auto *filename_param = request->getParam("filename");
    if (filename_param) {
      this->handle_file_upload(request, filename_param->value().c_str());
    } else {
      request->send(400, "text/plain", "Missing filename parameter");
    }
  } else {
    request->send(405, "application/json", "{\"error\":\"Method not allowed\"}");
  }
}

bool HttpFileBrowser::get_network_file_stat(storage::NetworkStorage *net_storage, const std::string &path,
                                             struct stat &file_stat) {
  std::string relative = strip_network_mount_prefix(net_storage, path);
  storage::FileStat fs{};
  if (net_storage->stat(relative.c_str(), &fs) != storage::StorageError::OK)
    return false;
  memset(&file_stat, 0, sizeof(file_stat));
  file_stat.st_size = (off_t) fs.size;
  if (fs.is_dir)
    file_stat.st_mode |= S_IFDIR;
  else
    file_stat.st_mode |= S_IFREG;
  return true;
}

bool HttpFileBrowser::handle_network_directory_listing(AsyncWebServerRequest *request,
                                                        storage::NetworkStorage *net_storage,
                                                        const std::string &path) {
  std::string relative = strip_network_mount_prefix(net_storage, path);

  struct ListCtx {
    const std::string &base_path;
    std::vector<storage::FileStat> entries;
  } lctx{path};

  auto err = net_storage->list_dir(
      relative.c_str(),
      [](const storage::FileStat *e, void *ctx) { static_cast<ListCtx *>(ctx)->entries.push_back(*e); }, &lctx);

  if (err != storage::StorageError::OK) {
    request->send(500, "text/plain", "Internal Server Error: Failed to list directory");
    return false;
  }

  std::string html = this->generate_html_header("File Browser");
  html += "<div class=\"header-actions\"><h1>File Browser</h1>";
  html += "<button onclick=\"create_directory()\">New Folder</button></div>";
  html += this->generate_breadcrumb(path);

  if (this->upload_enabled_) {
    html += R"HTML(<div class="upload-form">
      <form id="uploadForm" onsubmit="return handleUpload(event);">
        <input type="file" name="file" id="uploadFile" required>
        <button type="submit">Upload</button>
      </form>
    </div>)HTML";
  }

  html += R"(<table><thead><tr><th>Name</th><th>Size</th><th>Type</th><th>Actions</th></tr></thead><tbody>)";

  for (const auto &entry : lctx.entries) {
    std::string entry_name(entry.name);
    std::string entry_path = Path::join(path, entry_name);
    std::string file_uri = this->url_prefix_ + entry_path;

    html += "<tr><td>";
    if (entry.is_dir)
      html += "<a href=\"" + file_uri + "\" class=\"folder\">" + entry_name + "</a>";
    else
      html += "<span class=\"file\">" + entry_name + "</span>";
    html += "</td><td>";
    if (!entry.is_dir) {
      if (entry.size < 1024)
        html += std::to_string(entry.size) + " B";
      else if (entry.size < 1024 * 1024)
        html += std::to_string(entry.size / 1024) + " KB";
      else
        html += std::to_string(entry.size / (1024 * 1024)) + " MB";
    }
    html += "</td><td>";
    html += entry.is_dir ? "<span class=\"file-type\">Folder</span>"
                         : "<span class=\"file-type\">" + Path::file_type(entry_name) + "</span>";
    html += "</td><td class=\"actions\">";
    if (!entry.is_dir && this->download_enabled_)
      html += "<button onclick=\"download_file('" + file_uri + "', '" + entry_name + "')\">Download</button>";
    html += "<button onclick=\"copy_file('" + entry_path + "')\">Copy</button>";
    html += "<button onclick=\"move_file('" + entry_path + "')\">Move</button>";
    html += "<button onclick=\"rename_file('" + entry_path + "')\">Rename</button>";
    html += "<button onclick=\"show_file_info('" + entry_path + "')\">Info</button>";
    if (this->deletion_enabled_) {
      if (entry.is_dir)
        html += "<button onclick=\"delete_directory('" + entry_path + "')\">Delete</button>";
      else
        html += "<button onclick=\"delete_file('" + entry_path + "')\">Delete</button>";
    }
    html += "</td></tr>";
  }

  html += "</tbody></table>";
  html += this->generate_html_footer();

  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", html);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Expires", "0");
  request->send(response);
  return true;
}

bool HttpFileBrowser::handle_network_file_download(AsyncWebServerRequest *request,
                                                    storage::NetworkStorage *net_storage,
                                                    const std::string &path) {
  std::string relative = strip_network_mount_prefix(net_storage, path);
  storage::FileStat fs{};
  if (net_storage->stat(relative.c_str(), &fs) != storage::StorageError::OK) {
    request->send(404, "text/plain", "Not Found");
    return false;
  }
  size_t file_size = fs.size;

  httpd_req_t *req = static_cast<httpd_req_t *>(*request);
  std::string mime = Path::mime_type(path);
  std::string filename = Path::file_name(path);
  std::string content_disposition = "attachment; filename=\"" + filename + "\"";

  httpd_resp_set_type(req, mime.c_str());
  httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
  httpd_resp_set_hdr(req, "Content-Length", std::to_string(file_size).c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  uint8_t *buffer = this->download_buffer_;
  size_t total_sent = 0;
  bool success = true;

  while (total_sent < file_size) {
    App.feed_wdt();
    size_t bytes_read = 0;
    if (net_storage->read_chunk(relative.c_str(), buffer, total_sent, FILE_BUFFER_SIZE, &bytes_read) !=
        storage::StorageError::OK) {
      success = false;
      break;
    }
    if (bytes_read == 0)
      break;
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read) != ESP_OK) {
      success = false;
      break;
    }
    total_sent += bytes_read;
  }
  if (success)
    httpd_resp_send_chunk(req, nullptr, 0);
  return success;
}

// Network/filesystem path helpers
bool HttpFileBrowser::path_exists(const std::string &path, bool &is_directory) {
  storage::NetworkStorage *net = this->find_network_for_path_(path);
  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, path);
    storage::FileStat fs{};
    if (net->stat(relative.c_str(), &fs) != storage::StorageError::OK)
      return false;
    is_directory = fs.is_dir;
    return true;
  }
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    is_directory = S_ISDIR(st.st_mode);
    return true;
  }
  return false;
}

bool HttpFileBrowser::delete_path(const std::string &path, bool is_directory) {
  storage::NetworkStorage *net = this->find_network_for_path_(path);
  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, path);
    if (is_directory)
      return net->rmdir(relative.c_str(), false) == storage::StorageError::OK;
    return net->remove(relative.c_str()) == storage::StorageError::OK;
  }
  if (is_directory)
    return rmdir(path.c_str()) == 0;
  return remove(path.c_str()) == 0;
}

bool HttpFileBrowser::create_dir(const std::string &path) {
  storage::NetworkStorage *net = this->find_network_for_path_(path);
  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, path);
    return net->mkdir(relative.c_str()) == storage::StorageError::OK;
  }
  return mkdir(path.c_str(), 0777) == 0;
}

esphome::FixedVector<FileInfo> HttpFileBrowser::list_directory(const std::string &path) {
  esphome::FixedVector<FileInfo> files;

  storage::NetworkStorage *net = this->find_network_for_path_(path);
  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, path);

    size_t count = 0;
    net->list_dir(relative.c_str(), [](const storage::FileStat *, void *c) { (*static_cast<size_t *>(c))++; }, &count);

    size_t alloc = std::min(count, MAX_DIR_ENTRIES);
    files.init(alloc);

    struct FillCtx {
      const std::string &base_path;
      esphome::FixedVector<FileInfo> &files;
    } fctx{path, files};

    net->list_dir(
        relative.c_str(),
        [](const storage::FileStat *e, void *c) {
          auto *ctx = static_cast<FillCtx *>(c);
          if (ctx->files.size() >= MAX_DIR_ENTRIES)
            return;
          FileInfo info;
          info.name = e->name;
          info.path = Path::join(ctx->base_path, e->name);
          info.is_directory = e->is_dir;
          info.size = e->size;
          info.modified = 0;
          ctx->files.push_back(info);
        },
        &fctx);

    return files;
  }

  DIR *dir = opendir(path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Cannot open directory: %s (errno: %d)", path.c_str(), errno);
    return files;
  }
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      count++;
      if (count >= MAX_DIR_ENTRIES)
        break;
    }
  }
  files.init(count);
  rewinddir(dir);
  while ((entry = readdir(dir)) != nullptr && files.size() < count) {
    if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
      FileInfo info;
      info.name = entry->d_name;
      info.path = Path::join(path, entry->d_name);
      struct stat st;
      if (stat(info.path.c_str(), &st) == 0) {
        info.is_directory = S_ISDIR(st.st_mode);
        info.size = st.st_size;
        info.modified = st.st_mtime;
      } else {
        info.is_directory = false;
        info.size = 0;
        info.modified = 0;
      }
      files.push_back(info);
    }
  }
  closedir(dir);
  return files;
}

std::string HttpFileBrowser::uri_to_filepath(const std::string &uri) {
  std::string decoded = url_decode(uri);
  std::string rel = decoded;
  if (decoded.find(this->url_prefix_) == 0)
    rel = decoded.substr(this->url_prefix_.length());
  if (!rel.empty() && rel[0] == '/')
    rel = rel.substr(1);
  if (rel.empty())
    return this->root_path_;
  std::string full = this->root_path_;
  if (!full.empty() && full.back() != '/')
    full += "/";
  full += rel;
  return full;
}

std::string HttpFileBrowser::url_decode(const std::string &src) {
  std::string result;
  result.reserve(src.length());
  const char *str = src.c_str();
  int i = 0;
  while (str[i]) {
    if (str[i] == '%' && str[i + 1] && str[i + 2]) {
      int j;
      if (sscanf(str + i + 1, "%2x", &j) == 1) {
        result += static_cast<char>(j);
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

bool HttpFileBrowser::is_directory_writable(const std::string &dir_path) {
  storage::NetworkStorage *net = this->find_network_for_path_(dir_path);
  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, dir_path);
    storage::FileStat fs{};
    if (net->stat(relative.c_str(), &fs) != storage::StorageError::OK)
      return false;
    return fs.is_dir;
  }
  struct stat st;
  if (stat(dir_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return false;
  std::string test_file = Path::join(dir_path, ".write_test");
  FILE *f = fopen(test_file.c_str(), "w");
  if (!f)
    return false;
  fclose(f);
  remove(test_file.c_str());
  return true;
}

bool HttpFileBrowser::is_mount_point_mounted(const std::string &mount_path) {
  storage::NetworkStorage *net = this->find_network_for_path_(mount_path);
  if (net != nullptr) {
    storage::StorageInfo info{};
    net->get_info(&info);
    return info.is_mounted;
  }
  storage::FilesystemStorage *fs = this->find_filesystem_for_path_(mount_path);
  if (fs != nullptr) {
    storage::StorageInfo info{};
    fs->get_info(&info);
    return info.is_mounted;
  }
  struct stat st;
  if (stat(mount_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return false;
  DIR *dir = opendir(mount_path.c_str());
  if (!dir)
    return false;
  closedir(dir);
  return true;
}

bool HttpFileBrowser::get_mount_space_info(const std::string &mount_path, uint64_t &total, uint64_t &free) {
#ifdef USE_ESP_IDF
  uint64_t total_bytes = 0, free_bytes = 0;
  esp_err_t ret = esp_vfs_fat_info(mount_path.c_str(), &total_bytes, &free_bytes);
  if (ret == ESP_OK) {
    total = total_bytes;
    free = free_bytes;
    return true;
  }
#endif
  return false;
}

std::string HttpFileBrowser::get_filename_from_path(const std::string &path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos)
    return path;
  return path.substr(pos + 1);
}


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
    body { font-family: 'Segoe UI', system-ui, sans-serif; margin: 0; padding: 2rem; background: #f5f5f7; color: #1d1d1f; }
    h1 { color: #0066cc; margin-bottom: 1.5rem; display: flex; align-items: center; gap: 1rem; }
    .container { max-width: 1200px; margin: 0 auto; background: white; border-radius: 12px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); padding: 2rem; }
    table { width: 100%; border-collapse: collapse; margin-top: 1.5rem; }
    th, td { padding: 12px; text-align: left; border-bottom: 1px solid #e0e0e0; }
    th { background: #f8f9fa; font-weight: 500; }
    .file-actions { display: flex; gap: 8px; }
    button { padding: 6px 12px; border: none; border-radius: 6px; background: #0066cc; color: white; cursor: pointer; transition: background 0.2s; }
    button:hover { background: #0052a3; }
    button.delete { background: #dc3545; }
    button.delete:hover { background: #c82333; }
    .upload-form { margin-bottom: 2rem; padding: 1rem; background: #f8f9fa; border-radius: 8px; }
    .upload-form input[type="file"] { margin-right: 1rem; }
    .breadcrumb { margin-bottom: 1.5rem; font-size: 0.9rem; color: #666; }
    .breadcrumb a { color: #0066cc; text-decoration: none; }
    .breadcrumb a:hover { text-decoration: underline; }
    .breadcrumb span:not(:last-child)::after { display: inline-block; margin: 0 .25rem; content: ">"; }
    .folder { color: #0066cc; font-weight: 500; }
    .file-type { color: #666; font-size: 0.9rem; }
    .header-actions { display: flex; align-items: center; justify-content: space-between; margin-bottom: 1rem; }
    .header-actions button { background: #4CAF50; }
    .header-actions button:hover { background: #45a049; }
    .progress-modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 1000; align-items: center; justify-content: center; }
    .progress-modal.active { display: flex; }
    .progress-content { background: white; padding: 2rem; border-radius: 12px; min-width: 400px; box-shadow: 0 8px 24px rgba(0,0,0,0.2); }
    .progress-title { font-size: 1.2rem; font-weight: 500; margin-bottom: 1rem; color: #1d1d1f; }
    .progress-bar-container { width: 100%; height: 20px; background: #e0e0e0; border-radius: 10px; overflow: hidden; margin: 1rem 0; }
    .progress-bar { height: 100%; background: linear-gradient(90deg, #0066cc, #0052a3); width: 0%; transition: width 0.3s ease; display: flex; align-items: center; justify-content: center; color: white; font-size: 0.8rem; font-weight: 500; }
    .progress-details { color: #666; font-size: 0.9rem; margin-top: 0.5rem; }
    .progress-file-info { margin-top: 1rem; padding: 1rem; background: #f8f9fa; border-radius: 8px; font-size: 0.85rem; word-break: break-all; }
    .progress-speed { margin-top: 0.5rem; font-size: 0.9rem; color: #666; }
    #cancelBtn { margin-top: 1rem; width: 100%; }
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
    <button id="cancelBtn" class="delete" onclick="cancelOperation()">Cancel</button>
  </div>
</div>
<script>
const API_BASE = ')HTML" +
         this->url_prefix_ + R"HTML(';
function fmt_size(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';if(b<1073741824)return (b/1048576).toFixed(1)+' MB';return (b/1073741824).toFixed(1)+' GB';}
function show_progress(title){document.getElementById('progressTitle').textContent=title;document.getElementById('progressBar').style.width='0%';document.getElementById('progressBar').textContent='0%';document.getElementById('progressDetails').textContent='Initializing...';document.getElementById('progressSpeed').textContent='';document.getElementById('progressSource').textContent='-';document.getElementById('progressDest').textContent='-';document.getElementById('progressModal').classList.add('active');poll_progress();}
function hide_progress(){document.getElementById('progressModal').classList.remove('active');}
let progressInterval=null;
function poll_progress(){if(progressInterval)clearInterval(progressInterval);progressInterval=setInterval(async()=>{try{const r=await fetch(API_BASE+'/api/progress');const d=await r.json();if(!d.in_progress){clearInterval(progressInterval);progressInterval=null;hide_progress();location.reload();return;}const pct=d.percentage||0;document.getElementById('progressBar').style.width=pct+'%';document.getElementById('progressBar').textContent=pct.toFixed(1)+'%';if(d.source)document.getElementById('progressSource').textContent=d.source;if(d.destination)document.getElementById('progressDest').textContent=d.destination;if(d.avg_speed)document.getElementById('progressSpeed').textContent='Speed: '+fmt_size(d.avg_speed)+'/s';}catch(e){}},500);}
async function cancelOperation(){try{await fetch(API_BASE+'/api/cancel',{method:'POST'});hide_progress();}catch(e){}}
function download_file(uri,name){const a=document.createElement('a');a.href=uri;a.download=name;document.body.appendChild(a);a.click();document.body.removeChild(a);}
async function delete_file(path){if(!confirm('Delete file '+path+'?'))return;try{const r=await fetch(API_BASE+'/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(path)});const d=await r.json();if(d.success)location.reload();else alert('Delete failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function delete_directory(path){if(!confirm('Delete directory '+path+' and all contents?'))return;try{const r=await fetch(API_BASE+'/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(path)});const d=await r.json();if(d.success){show_progress('Deleting...');location.reload();}else alert('Delete failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function create_directory(){const name=prompt('Enter directory name:');if(!name)return;const current=window.location.pathname;try{const r=await fetch(API_BASE+'/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(current+'/'+name)});const d=await r.json();if(d.success)location.reload();else alert('Failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function rename_file(path){const name=prompt('New name:',path.split('/').pop());if(!name)return;try{const r=await fetch(API_BASE+'/api/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'source='+encodeURIComponent(path)+'&name='+encodeURIComponent(name)});const d=await r.json();if(d.success)location.reload();else alert('Rename failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function copy_file(path){const dest=prompt('Copy to path:',path);if(!dest||dest===path)return;show_progress('Copying...');try{const r=await fetch(API_BASE+'/api/copy',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'source='+encodeURIComponent(path)+'&destination='+encodeURIComponent(dest)});const d=await r.json();if(!d.success){hide_progress();alert('Copy failed: '+(d.error||'Unknown error'));}}catch(e){hide_progress();alert('Error: '+e);}}
async function move_file(path){const dest=prompt('Move to path:',path);if(!dest||dest===path)return;show_progress('Moving...');try{const r=await fetch(API_BASE+'/api/move',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'source='+encodeURIComponent(path)+'&destination='+encodeURIComponent(dest)});const d=await r.json();if(!d.success){hide_progress();alert('Move failed: '+(d.error||'Unknown error'));}}catch(e){hide_progress();alert('Error: '+e);}}
async function show_file_info(path){try{const r=await fetch(API_BASE+'/api/fileinfo?path='+encodeURIComponent(path));const d=await r.json();if(d.error){alert('Error: '+d.error);return;}let msg='Name: '+d.name+'\nPath: '+d.path+'\nSize: '+fmt_size(d.size)+'\nType: '+(d.is_directory?'Directory':'File')+'\nStorage: '+d.storage_type;alert(msg);}catch(e){alert('Error: '+e);}}
async function mount_device(path){try{const r=await fetch(API_BASE+'/api/mount',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mount_point='+encodeURIComponent(path)});const d=await r.json();if(d.success)setTimeout(()=>location.reload(),500);else alert('Mount failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function unmount_device(path){try{const r=await fetch(API_BASE+'/api/unmount',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mount_point='+encodeURIComponent(path)});const d=await r.json();if(d.success)setTimeout(()=>location.reload(),500);else alert('Unmount failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function remount_device(path){try{const r=await fetch(API_BASE+'/api/remount',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mount_point='+encodeURIComponent(path)});const d=await r.json();if(d.success)setTimeout(()=>location.reload(),500);else alert('Remount failed: '+(d.error||'Unknown error'));}catch(e){alert('Error: '+e);}}
async function handleUpload(event){event.preventDefault();const file=document.getElementById('uploadFile').files[0];if(!file)return false;const CHUNK=65536;const total=Math.ceil(file.size/CHUNK);show_progress('Uploading '+file.name+'...');for(let i=0;i<total;i++){const blob=file.slice(i*CHUNK,(i+1)*CHUNK);const fd=new FormData();fd.append('filename',file.name);fd.append('chunkIndex',i);fd.append('totalChunks',total);fd.append('path',window.location.pathname.replace(API_BASE,''));fd.append('fileSize',file.size);fd.append('chunk',blob);try{const r=await fetch(API_BASE+'/api/upload_chunk',{method:'POST',body:fd});const d=await r.json();if(!d.success&&!d.cancelled){hide_progress();alert('Upload failed');return false;}}catch(e){hide_progress();alert('Upload error: '+e);return false;}}hide_progress();location.reload();return false;}
</script>
</body>
</html>
)HTML";
}

std::string HttpFileBrowser::generate_breadcrumb(const std::string &current_path) {
  std::string breadcrumb = "<div class=\"breadcrumb\">";
  breadcrumb += "<span><a href=\"" + this->url_prefix_ + "/\">Root</a></span>";

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
      row += "<span class=\"folder unmounted\" style=\"color:#999;cursor:not-allowed;\">" + info.name + " (unmounted)</span>";
    } else {
      row += "<a href=\"" + file_uri + "\" class=\"folder\">" + info.name + "</a>";
    }
  } else {
    row += info.name;
  }
  row += "</td><td>";

  if (info.is_directory) {
    if (info.is_mount_point && !info.mounted) {
      row += "<span style=\"color:#999;\">Mount Point</span>";
    } else {
      row += "Folder";
    }
  } else {
    row += "<span class=\"file-type\">" + Path::file_type(info.name) + "</span>";
  }
  row += "</td><td>";

  if (!info.is_directory) {
    if (info.size < 1024)
      row += std::to_string(info.size) + " B";
    else if (info.size < 1024 * 1024)
      row += std::to_string(info.size / 1024) + " KB";
    else
      row += std::to_string(info.size / (1024 * 1024)) + " MB";
  } else if (info.is_mount_point && info.has_space_info) {
    uint64_t used = info.total_space - info.free_space;
    int pct = (info.total_space > 0) ? (int)(used * 100 / info.total_space) : 0;
    auto fmt = [](uint64_t b) -> std::string {
      if (b < 1024ULL * 1024) return std::to_string(b / 1024) + " KB";
      if (b < 1024ULL * 1024 * 1024) {
        uint64_t x = (b * 10) / (1024 * 1024);
        return std::to_string(x / 10) + "." + std::to_string(x % 10) + " MB";
      }
      uint64_t x = (b * 10) / (1024ULL * 1024 * 1024);
      return std::to_string(x / 10) + "." + std::to_string(x % 10) + " GB";
    };
    row += fmt(used) + " / " + fmt(info.total_space) + " (" + std::to_string(pct) + "%)";
  }
  row += "</td><td><div class=\"file-actions\">";

  if (!info.is_directory) {
    if (this->download_enabled_)
      row += "<button onclick=\"download_file('" + file_uri + "', '" + info.name + "')\">Download</button>";
    row += "<button onclick=\"copy_file('" + info.path + "')\">Copy</button>";
    row += "<button onclick=\"move_file('" + info.path + "')\">Move</button>";
    row += "<button onclick=\"rename_file('" + info.path + "')\">Rename</button>";
    row += "<button onclick=\"show_file_info('" + info.path + "')\">Info</button>";
    if (this->deletion_enabled_)
      row += "<button class=\"delete\" onclick=\"delete_file('" + file_uri + "')\">Delete</button>";
  } else {
    if (!info.is_mount_point) {
      row += "<button onclick=\"copy_file('" + info.path + "')\">Copy</button>";
      row += "<button onclick=\"move_file('" + info.path + "')\">Move</button>";
      row += "<button onclick=\"rename_file('" + info.path + "')\">Rename</button>";
      if (this->deletion_enabled_)
        row += "<button class=\"delete\" onclick=\"delete_directory('" + file_uri + "')\">Delete</button>";
    } else {
      if (info.mounted) {
        row += "<button onclick=\"remount_device('" + info.path + "')\">Remount</button>";
        row += "<button onclick=\"unmount_device('" + info.path + "')\">Unmount</button>";
      } else {
        row += "<button onclick=\"mount_device('" + info.path + "')\">Mount</button>";
      }
    }
  }
  row += "</div></td></tr>";
  return row;
}

void HttpFileBrowser::handle_directory_listing(AsyncWebServerRequest *request, const std::string &filepath) {
  bool is_virtual_root = (this->root_path_ == "/" && filepath == "/");

  std::string html = this->generate_html_header("File Browser");
  html += "<div class=\"header-actions\"><h1>File Browser</h1>";
  html += "<button onclick=\"create_directory()\">New Folder</button></div>";
  html += this->generate_breadcrumb(filepath);

  if (this->upload_enabled_ && !is_virtual_root) {
    html += R"HTML(<div class="upload-form">
      <form id="uploadForm" onsubmit="return handleUpload(event);">
        <input type="file" name="file" id="uploadFile" required>
        <button type="submit">Upload</button>
      </form>
    </div>)HTML";
  }

  html += R"(<table><thead><tr><th>Name</th><th>Type</th><th>Size</th><th>Actions</th></tr></thead><tbody>)";

  if (is_virtual_root) {
    // List all filesystem storage devices
    if (this->registry_ != nullptr) {
      struct FsCtx {
        HttpFileBrowser *self;
        std::string &html;
      } fsctx{this, html};

      this->registry_->for_each_filesystem(
          [](storage::FilesystemStorage *s, void *c) {
            auto *ctx = static_cast<FsCtx *>(c);
            storage::StorageInfo info{};
            if (s->get_info(&info) != storage::StorageError::OK || info.id == nullptr)
              return;
            FileInfo fi;
            fi.path = std::string(info.id);
            fi.name = fi.path.substr(1);  // strip leading /
            if (fi.name.empty())
              fi.name = "root";
            fi.is_directory = true;
            fi.is_mount_point = true;
            fi.mounted = info.is_mounted;
            fi.size = 0;
            fi.modified = 0;
            if (fi.mounted) {
              uint64_t total = 0, free = 0;
              if (ctx->self->get_mount_space_info(fi.path, total, free)) {
                fi.total_space = total;
                fi.free_space = free;
                fi.has_space_info = true;
              } else if (info.total_bytes > 0) {
                fi.total_space = info.total_bytes;
                fi.free_space = info.free_bytes;
                fi.has_space_info = true;
              }
            }
            ctx->html += ctx->self->generate_file_row(fi, ctx->self->url_prefix_);
          },
          &fsctx);

      // List all network storage devices
      struct NetCtx {
        HttpFileBrowser *self;
        std::string &html;
      } netctx{this, html};

      this->registry_->for_each_network(
          [](storage::NetworkStorage *s, void *c) {
            auto *ctx = static_cast<NetCtx *>(c);
            storage::StorageInfo info{};
            if (s->get_info(&info) != storage::StorageError::OK || info.id == nullptr)
              return;
            FileInfo fi;
            fi.path = std::string(info.id);
            fi.name = fi.path.substr(1);
            if (fi.name.empty())
              fi.name = "network";
            fi.is_directory = true;
            fi.is_mount_point = true;
            fi.mounted = info.is_mounted;
            fi.size = 0;
            fi.modified = 0;
            if (fi.mounted && info.total_bytes > 0) {
              fi.total_space = info.total_bytes;
              fi.free_space = info.free_bytes;
              fi.has_space_info = true;
            }
            ctx->html += ctx->self->generate_file_row(fi, ctx->self->url_prefix_);
          },
          &netctx);
    }
  } else {
    storage::NetworkStorage *net_storage = this->find_network_for_path_(filepath);
    if (net_storage != nullptr) {
      std::string relative = strip_network_mount_prefix(net_storage, filepath);
      struct ListCtx {
        HttpFileBrowser *self;
        std::string &html;
        const std::string &base_path;
      } lctx{this, html, filepath};

      net_storage->list_dir(
          relative.c_str(),
          [](const storage::FileStat *e, void *c) {
            auto *ctx = static_cast<ListCtx *>(c);
            FileInfo info;
            info.name = e->name;
            info.path = Path::join(ctx->base_path, e->name);
            info.is_directory = e->is_dir;
            info.size = e->size;
            info.modified = 0;
            ctx->html += ctx->self->generate_file_row(info, ctx->self->url_prefix_);
          },
          &lctx);
    } else {
      esphome::FixedVector<FileInfo> files = this->list_directory(filepath);
      for (const auto &info : files)
        html += this->generate_file_row(info, this->url_prefix_);
    }
  }

  html += "</tbody></table>";
  html += this->generate_html_footer();

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

  std::string mime = Path::mime_type(filepath);
  std::string filename = Path::file_name(filepath);
  std::string content_disposition = "attachment; filename=\"" + filename + "\"";

  httpd_req_t *req = static_cast<httpd_req_t *>(*request);

  storage::NetworkStorage *net_storage = this->find_network_for_path_(filepath);
  if (net_storage != nullptr) {
    std::string relative = strip_network_mount_prefix(net_storage, filepath);
    storage::FileStat fs{};
    if (net_storage->stat(relative.c_str(), &fs) != storage::StorageError::OK) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Failed to get file info");
      return;
    }
    size_t file_size = fs.size;

    httpd_resp_set_type(req, mime.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
    httpd_resp_set_hdr(req, "Content-Length", std::to_string(file_size).c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    uint8_t *buffer = this->download_buffer_;
    size_t total_sent = 0;
    bool success = true;

    while (total_sent < file_size) {
      App.feed_wdt();
      size_t bytes_read = 0;
      if (net_storage->read_chunk(relative.c_str(), buffer, total_sent, FILE_BUFFER_SIZE, &bytes_read) !=
          storage::StorageError::OK) {
        success = false;
        break;
      }
      if (bytes_read == 0)
        break;
      if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read) != ESP_OK) {
        success = false;
        break;
      }
      total_sent += bytes_read;
    }
    if (success)
      httpd_resp_send_chunk(req, nullptr, 0);
    return;
  }

  // Local storage
  struct stat file_stat;
  if (stat(filepath.c_str(), &file_stat) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return;
  }
  size_t file_size = file_stat.st_size;

  FILE *file = fopen(filepath.c_str(), "rb");
  if (!file) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return;
  }

  httpd_resp_set_type(req, mime.c_str());
  httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
  httpd_resp_set_hdr(req, "Content-Length", std::to_string(file_size).c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  uint8_t *buffer = this->download_buffer_;
  size_t total_sent = 0;
  bool success = true;

  while (total_sent < file_size) {
    App.feed_wdt();
    size_t to_read = std::min(FILE_BUFFER_SIZE, file_size - total_sent);
    size_t bytes_read = fread(buffer, 1, to_read, file);
    if (bytes_read == 0) {
      success = false;
      break;
    }
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read) != ESP_OK) {
      success = false;
      break;
    }
    total_sent += bytes_read;
  }
  fclose(file);
  if (success)
    httpd_resp_send_chunk(req, nullptr, 0);
}

void HttpFileBrowser::handleUpload(AsyncWebServerRequest *request, const PlatformString &filename, size_t index,
                                    uint8_t *data, size_t len, bool final) {
  if (this->auth_enabled_) {
    if (!request->authenticate(this->username_.c_str(), this->password_.c_str()))
      return;
  }
  if (!this->upload_enabled_) {
    if (final)
      request->send(403, "text/plain", "File upload is disabled");
    return;
  }

  if (index == 0) {
    if (this->upload_file_) {
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
    }
    if (this->chunk_buffer_) {
      free(this->chunk_buffer_);
      this->chunk_buffer_ = nullptr;
      this->chunk_buffer_size_ = 0;
    }

    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    StringRef uri = request->url_to(url_buf);
    this->upload_directory_ = this->uri_to_filepath(uri);
    this->upload_filename_ = filename.c_str();

    std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);
    storage::NetworkStorage *net_storage = this->find_network_for_path_(upload_path);

    if (net_storage != nullptr) {
      size_t expected_size = 0;
      auto *fs_param = request->getParam("filesize");
      if (fs_param)
        expected_size = std::stoul(fs_param->value().c_str());

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

      this->chunk_buffer_size_ = expected_size > 0 ? expected_size : FILE_BUFFER_SIZE;
      if (this->chunk_buffer_)
        free(this->chunk_buffer_);
#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
      this->chunk_buffer_ = (uint8_t *) heap_caps_malloc(this->chunk_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!this->chunk_buffer_)
        this->chunk_buffer_ = (uint8_t *) malloc(this->chunk_buffer_size_);
#else
      this->chunk_buffer_ = (uint8_t *) malloc(this->chunk_buffer_size_);
#endif
      if (!this->chunk_buffer_) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
        if (final)
          request->send(500, "text/plain", "Failed to allocate upload buffer");
        return;
      }
      this->upload_file_ = nullptr;
      return;
    }

    // Local storage
    struct stat file_stat;
    if (stat(this->upload_directory_.c_str(), &file_stat) != 0 || !S_ISDIR(file_stat.st_mode)) {
      if (final)
        request->send(400, "text/plain", "Upload target must be a directory");
      return;
    }

    size_t expected_size = 0;
    auto *fs_param = request->getParam("filesize");
    if (fs_param)
      expected_size = std::stoul(fs_param->value().c_str());

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

    this->upload_bytes_since_yield_ = 0;
    this->upload_file_ = fopen(upload_path.c_str(), "wb");
    if (!this->upload_file_) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
      if (final)
        request->send(500, "text/plain", "Failed to create file");
      return;
    }
  }

  portENTER_CRITICAL(&this->progress_mutex_);
  bool cancelled = this->progress_.cancelled;
  size_t current_transferred = this->progress_.transferred_bytes;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (cancelled) {
    if (this->upload_file_) {
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
      std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);
      remove(upload_path.c_str());
    } else if (this->chunk_buffer_) {
      free(this->chunk_buffer_);
      this->chunk_buffer_ = nullptr;
      this->chunk_buffer_size_ = 0;
    }
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
    if (final)
      request->send(499, "text/plain", "Upload cancelled");
    return;
  }

  if (len > 0) {
    if (this->upload_file_) {
      size_t written = fwrite(data, 1, len, this->upload_file_);
      if (written != len) {
        fclose(this->upload_file_);
        this->upload_file_ = nullptr;
        std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);
        remove(upload_path.c_str());
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
        if (final)
          request->send(500, "text/plain", "Failed to write file");
        return;
      }
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.in_progress)
        this->progress_.transferred_bytes += written;
      portEXIT_CRITICAL(&this->progress_mutex_);
      this->upload_bytes_since_yield_ += written;
      if (this->upload_bytes_since_yield_ >= 128 * 1024) {
        vTaskDelay(10);
        this->upload_bytes_since_yield_ = 0;
      }
    } else if (this->chunk_buffer_) {
      if (current_transferred + len > this->chunk_buffer_size_) {
        free(this->chunk_buffer_);
        this->chunk_buffer_ = nullptr;
        this->chunk_buffer_size_ = 0;
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
        if (final)
          request->send(500, "text/plain", "Upload buffer overflow");
        return;
      }
      memcpy(this->chunk_buffer_ + current_transferred, data, len);
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.in_progress)
        this->progress_.transferred_bytes += len;
      portEXIT_CRITICAL(&this->progress_mutex_);
      this->upload_bytes_since_yield_ += len;
      if (this->upload_bytes_since_yield_ >= 128 * 1024) {
        vTaskDelay(10);
        this->upload_bytes_since_yield_ = 0;
      }
    } else {
      if (final)
        request->send(500, "text/plain", "Upload state corrupted");
      return;
    }
  }

  if (final) {
    if (this->upload_file_) {
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
      request->send(201, "text/plain", "File uploaded successfully");
    } else if (this->chunk_buffer_) {
      std::string upload_path = Path::join(this->upload_directory_, this->upload_filename_);
      portENTER_CRITICAL(&this->progress_mutex_);
      size_t final_size = this->progress_.transferred_bytes;
      portEXIT_CRITICAL(&this->progress_mutex_);

      storage::NetworkStorage *net_storage = this->find_network_for_path_(upload_path);
      bool success = false;
      if (net_storage != nullptr) {
        std::string relative = strip_network_mount_prefix(net_storage, upload_path);
        size_t transferred = 0;
        success = (net_storage->write_chunk(relative.c_str(), this->chunk_buffer_, 0, final_size, &transferred) ==
                   storage::StorageError::OK);
      }
      free(this->chunk_buffer_);
      this->chunk_buffer_ = nullptr;
      this->chunk_buffer_size_ = 0;
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
      if (success)
        request->send(201, "text/plain", "File uploaded successfully");
      else
        request->send(500, "text/plain", "Failed to write file to network storage");
    } else {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
      request->send(500, "text/plain", "Upload failed");
    }
  }
}

void HttpFileBrowser::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                  size_t total) {
  if (this->auth_enabled_) {
    if (!request->authenticate(this->username_.c_str(), this->password_.c_str()))
      return;
  }
  if (index == 0) {
    this->body_buffer_.clear();
    this->body_buffer_.reserve(total);
  }
  this->body_buffer_.append(reinterpret_cast<char *>(data), len);
}

void HttpFileBrowser::handle_file_upload(AsyncWebServerRequest *request, const std::string &filename) {
  if (!this->upload_enabled_) {
    request->send(403, "text/plain", "File upload is disabled");
    return;
  }

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef uri = request->url_to(url_buf);
  std::string dir_path = this->uri_to_filepath(uri);
  std::string upload_path = Path::join(dir_path, filename);

  struct stat file_stat;
  if (stat(dir_path.c_str(), &file_stat) != 0 || !S_ISDIR(file_stat.st_mode)) {
    request->send(400, "text/plain", "Upload target must be a directory");
    return;
  }

  size_t content_len = request->contentLength();
  portENTER_CRITICAL(&this->progress_mutex_);
  this->progress_.operation = "upload";
  this->progress_.source = filename;
  this->progress_.destination = upload_path;
  this->progress_.total_bytes = content_len;
  this->progress_.transferred_bytes = 0;
  this->progress_.in_progress = true;
  this->progress_.cancelled = false;
  this->progress_.start_time = millis();
  portEXIT_CRITICAL(&this->progress_mutex_);

  FILE *file = fopen(upload_path.c_str(), "wb");
  if (!file) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(500, "text/plain", "Failed to create file");
    return;
  }

  httpd_req_t *req = static_cast<httpd_req_t *>(*request);
  uint8_t *buffer = this->upload_buffer_;
  size_t remaining = content_len;
  bool success = true;

  while (remaining > 0) {
    App.feed_wdt();
    size_t to_read = (remaining > FILE_BUFFER_SIZE) ? FILE_BUFFER_SIZE : remaining;
    int received = httpd_req_recv(req, reinterpret_cast<char *>(buffer), to_read);
    if (received <= 0) {
      success = false;
      break;
    }
    size_t written = fwrite(buffer, 1, received, file);
    if (written != static_cast<size_t>(received)) {
      success = false;
      break;
    }
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.transferred_bytes += written;
    portEXIT_CRITICAL(&this->progress_mutex_);
    remaining -= received;
  }
  fclose(file);

  portENTER_CRITICAL(&this->progress_mutex_);
  this->progress_.in_progress = false;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (success) {
    request->send(200, "text/plain", "File uploaded successfully");
  } else {
    remove(upload_path.c_str());
    request->send(500, "text/plain", "Upload failed");
  }
}


// Directory helpers
bool HttpFileBrowser::is_directory_empty(const std::string &path) {
  storage::NetworkStorage *net = this->find_network_for_path_(path);
  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, path);
    size_t count = 0;
    net->list_dir(relative.c_str(), [](const storage::FileStat *, void *c) { (*static_cast<size_t *>(c))++; }, &count);
    return count == 0;
  }
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return true;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      closedir(dir);
      return false;
    }
  }
  closedir(dir);
  return true;
}

void HttpFileBrowser::count_directory_contents(const std::string &path, int &file_count, int &dir_count) {
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    std::string entry_path = Path::join(path, entry->d_name);
    struct stat st;
    if (stat(entry_path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        dir_count++;
        this->count_directory_contents(entry_path, file_count, dir_count);
      } else {
        file_count++;
      }
    }
  }
  closedir(dir);
}

void HttpFileBrowser::count_directory_contents_network(storage::NetworkStorage *net_storage, const std::string &path,
                                                         int &file_count, int &dir_count) {
  if (net_storage == nullptr)
    return;

  struct CountCtx {
    storage::NetworkStorage *net;
    HttpFileBrowser *self;
    const std::string &base;
    int &file_count;
    int &dir_count;
  } ctx{net_storage, this, path, file_count, dir_count};

  net_storage->list_dir(
      path.c_str(),
      [](const storage::FileStat *e, void *c) {
        auto *ctx = static_cast<CountCtx *>(c);
        if (e->is_dir) {
          ctx->dir_count++;
          std::string sub = Path::join(ctx->base, e->name);
          ctx->self->count_directory_contents_network(ctx->net, sub, ctx->file_count, ctx->dir_count);
        } else {
          ctx->file_count++;
        }
      },
      &ctx);
}

bool HttpFileBrowser::recursive_delete_directory(const std::string &path, bool track_progress) {
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return false;

  bool success = true;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      bool cancelled = this->progress_.cancelled;
      portEXIT_CRITICAL(&this->progress_mutex_);
      if (cancelled) {
        closedir(dir);
        return false;
      }
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    std::string entry_path = Path::join(path, entry->d_name);
    struct stat st;
    if (stat(entry_path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        if (!this->recursive_delete_directory(entry_path, track_progress)) {
          success = false;
          break;
        }
      } else {
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.current_file = entry_path;
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
        if (remove(entry_path.c_str()) != 0) {
          success = false;
          break;
        }
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.processed_items++;
          portEXIT_CRITICAL(&this->progress_mutex_);
        }
      }
    }
  }
  closedir(dir);

  if (success) {
    if (rmdir(path.c_str()) != 0)
      return false;
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.processed_items++;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
  }
  return success;
}

bool HttpFileBrowser::recursive_delete_directory_network(storage::NetworkStorage *net_storage, const std::string &path,
                                                          bool track_progress) {
  if (net_storage == nullptr)
    return false;

  struct DelCtx {
    storage::NetworkStorage *net;
    HttpFileBrowser *self;
    const std::string &base;
    bool track;
    bool success;
  } ctx{net_storage, this, path, track_progress, true};

  net_storage->list_dir(
      path.c_str(),
      [](const storage::FileStat *e, void *c) {
        auto *ctx = static_cast<DelCtx *>(c);
        if (!ctx->success)
          return;
        App.feed_wdt();
        if (ctx->track) {
          portENTER_CRITICAL(&ctx->self->progress_mutex_);
          bool cancelled = ctx->self->progress_.cancelled;
          portEXIT_CRITICAL(&ctx->self->progress_mutex_);
          if (cancelled) {
            ctx->success = false;
            return;
          }
        }
        std::string entry_path = Path::join(ctx->base, e->name);
        if (e->is_dir) {
          if (!ctx->self->recursive_delete_directory_network(ctx->net, entry_path, ctx->track))
            ctx->success = false;
        } else {
          if (ctx->track) {
            portENTER_CRITICAL(&ctx->self->progress_mutex_);
            ctx->self->progress_.current_file = entry_path;
            portEXIT_CRITICAL(&ctx->self->progress_mutex_);
          }
          if (ctx->net->remove(entry_path.c_str()) != storage::StorageError::OK) {
            ctx->success = false;
          } else if (ctx->track) {
            portENTER_CRITICAL(&ctx->self->progress_mutex_);
            ctx->self->progress_.processed_items++;
            portEXIT_CRITICAL(&ctx->self->progress_mutex_);
          }
        }
      },
      &ctx);

  if (!ctx.success)
    return false;

  if (net_storage->rmdir(path.c_str(), false) != storage::StorageError::OK)
    return false;
  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.processed_items++;
    portEXIT_CRITICAL(&this->progress_mutex_);
  }
  return true;
}

// Form data parsing helper
bool HttpFileBrowser::parse_json_request(const uint8_t *body, size_t body_len, ApiRequest &req) {
  std::string form_data((const char *) body, body_len);
  size_t pos = 0;
  while (pos < form_data.length()) {
    size_t amp_pos = form_data.find('&', pos);
    if (amp_pos == std::string::npos)
      amp_pos = form_data.length();
    std::string pair = form_data.substr(pos, amp_pos - pos);
    size_t eq_pos = pair.find('=');
    if (eq_pos != std::string::npos) {
      std::string key = pair.substr(0, eq_pos);
      std::string value = this->url_decode(pair.substr(eq_pos + 1));
      if (key == "source") req.source = value;
      else if (key == "destination") req.destination = value;
      else if (key == "name") req.name = value;
      else if (key == "path") req.path = value;
      else if (key == "mount_point") req.mount_point = value;
      else if (key == "device") req.device = value;
    }
    pos = amp_pos + 1;
  }
  return !req.source.empty() || !req.destination.empty() || !req.name.empty() || !req.path.empty() ||
         !req.mount_point.empty() || !req.device.empty();
}

// File operation helpers
struct FileCloser {
  FILE *fp;
  explicit FileCloser(FILE *f) : fp(f) {}
  ~FileCloser() { if (fp) fclose(fp); }
  FileCloser(const FileCloser &) = delete;
  FileCloser &operator=(const FileCloser &) = delete;
};

bool HttpFileBrowser::perform_file_copy(const std::string &src_path, const std::string &dst_path, off_t file_size,
                                         bool track_progress) {
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

  storage::NetworkStorage *src_net = this->find_network_for_path_(src_path);
  storage::NetworkStorage *dst_net = this->find_network_for_path_(dst_path);

  if (src_net != nullptr || dst_net != nullptr) {
    std::string src_rel = src_net ? strip_network_mount_prefix(src_net, src_path) : src_path;
    std::string dst_rel = dst_net ? strip_network_mount_prefix(dst_net, dst_path) : dst_path;

    uint8_t *buffer = this->copy_buffer_;
    size_t total_copied = 0;
    bool success = true;
    bool first_chunk = true;

    FILE *src_file = nullptr;
    FILE *dst_file = nullptr;
    if (src_net == nullptr) {
      src_file = fopen(src_path.c_str(), "rb");
      if (!src_file) {
        if (track_progress) { portENTER_CRITICAL(&this->progress_mutex_); this->progress_.in_progress = false; portEXIT_CRITICAL(&this->progress_mutex_); }
        return false;
      }
    }
    if (dst_net == nullptr) {
      dst_file = fopen(dst_path.c_str(), "wb");
      if (!dst_file) {
        if (src_file) fclose(src_file);
        if (track_progress) { portENTER_CRITICAL(&this->progress_mutex_); this->progress_.in_progress = false; portEXIT_CRITICAL(&this->progress_mutex_); }
        return false;
      }
    }

    while (success) {
      App.feed_wdt();
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        bool cancelled = this->progress_.cancelled;
        portEXIT_CRITICAL(&this->progress_mutex_);
        if (cancelled) { success = false; break; }
      }

      size_t bytes_read = 0;
      if (src_net != nullptr) {
        if (src_net->read_chunk(src_rel.c_str(), buffer, total_copied, FILE_BUFFER_SIZE, &bytes_read) !=
            storage::StorageError::OK) {
          success = false;
          break;
        }
      } else {
        bytes_read = fread(buffer, 1, FILE_BUFFER_SIZE, src_file);
        if (bytes_read == 0 && ferror(src_file)) { success = false; break; }
      }
      if (bytes_read == 0)
        break;

      if (dst_net != nullptr) {
        size_t written = 0;
        if (dst_net->write_chunk(dst_rel.c_str(), buffer, total_copied, bytes_read, &written) !=
            storage::StorageError::OK) {
          success = false;
          break;
        }
      } else {
        size_t written = fwrite(buffer, 1, bytes_read, dst_file);
        if (written != bytes_read) { success = false; break; }
      }

      total_copied += bytes_read;
      first_chunk = false;

      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.transferred_bytes = total_copied;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    }

    if (src_file) fclose(src_file);
    if (dst_file) fclose(dst_file);
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return success;
  }

  // Both local
  FILE *src = fopen(src_path.c_str(), "rb");
  if (!src) {
    if (track_progress) { portENTER_CRITICAL(&this->progress_mutex_); this->progress_.in_progress = false; portEXIT_CRITICAL(&this->progress_mutex_); }
    return false;
  }
  FileCloser src_closer(src);

  FILE *dst = fopen(dst_path.c_str(), "wb");
  if (!dst) {
    if (track_progress) { portENTER_CRITICAL(&this->progress_mutex_); this->progress_.in_progress = false; portEXIT_CRITICAL(&this->progress_mutex_); }
    return false;
  }
  FileCloser dst_closer(dst);

  uint8_t *buffer = this->copy_buffer_;
  size_t bytes_read;
  size_t total_copied = 0;
  bool copy_success = true;
  size_t buffer_count = 0;

  while ((bytes_read = fread(buffer, 1, FILE_BUFFER_SIZE, src)) > 0) {
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      bool cancelled = this->progress_.cancelled;
      portEXIT_CRITICAL(&this->progress_mutex_);
      if (cancelled) { copy_success = false; break; }
    }
    size_t written = fwrite(buffer, 1, bytes_read, dst);
    if (written != bytes_read) { copy_success = false; break; }
    total_copied += written;
    buffer_count++;
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.transferred_bytes = total_copied;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    if (buffer_count % 16 == 0)
      vTaskDelay(0);
  }

  if (ferror(src))
    copy_success = false;
  if (fflush(dst) != 0)
    copy_success = false;

  if (copy_success && total_copied == static_cast<size_t>(file_size)) {
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      if (this->progress_.operation == "copy") {
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
      }
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return true;
  }

  remove(dst_path.c_str());
  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    if (this->progress_.operation == "copy") {
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;
    }
    portEXIT_CRITICAL(&this->progress_mutex_);
  }
  return false;
}

bool HttpFileBrowser::perform_file_move(const std::string &src_path, const std::string &dst_path, off_t file_size,
                                         bool track_progress) {
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

  storage::NetworkStorage *src_net = this->find_network_for_path_(src_path);
  storage::NetworkStorage *dst_net = this->find_network_for_path_(dst_path);
  bool is_network = (src_net != nullptr || dst_net != nullptr);

  if (!is_network && rename(src_path.c_str(), dst_path.c_str()) == 0) {
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.transferred_bytes = file_size;
      this->progress_.in_progress = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return true;
  }

  if (src_net != nullptr && dst_net != nullptr && src_net == dst_net) {
    std::string src_rel = strip_network_mount_prefix(src_net, src_path);
    std::string dst_rel = strip_network_mount_prefix(dst_net, dst_path);
    if (src_net->rename(src_rel.c_str(), dst_rel.c_str()) == storage::StorageError::OK) {
      if (track_progress) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.transferred_bytes = file_size;
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
      return true;
    }
  }

  if (is_network || errno == EXDEV) {
    bool ok = perform_file_copy(src_path, dst_path, file_size, track_progress);
    if (ok)
      this->delete_path(src_path, false);
    if (track_progress) {
      portENTER_CRITICAL(&this->progress_mutex_);
      this->progress_.in_progress = false;
      this->progress_.cancelled = false;
      portEXIT_CRITICAL(&this->progress_mutex_);
    }
    return ok;
  }

  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
  }
  return false;
}

bool HttpFileBrowser::perform_directory_copy(const std::string &src_path, const std::string &dst_path,
                                              bool track_progress) {
  if (track_progress) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.operation = "copy";
    this->progress_.source = src_path;
    this->progress_.destination = dst_path;
    this->progress_.total_bytes = 0;
    this->progress_.transferred_bytes = 0;
    this->progress_.in_progress = true;
    this->progress_.cancelled = false;
    this->progress_.start_time = millis();
    portEXIT_CRITICAL(&this->progress_mutex_);
  }

  if (!this->create_dir(dst_path)) {
    if (track_progress) { portENTER_CRITICAL(&this->progress_mutex_); this->progress_.in_progress = false; portEXIT_CRITICAL(&this->progress_mutex_); }
    return false;
  }

  bool success = true;
  storage::NetworkStorage *src_net = this->find_network_for_path_(src_path);

  if (src_net != nullptr) {
    std::string src_rel = strip_network_mount_prefix(src_net, src_path);
    struct CopyCtx {
      HttpFileBrowser *self;
      storage::NetworkStorage *net;
      const std::string &src_base;
      const std::string &dst_base;
      bool track;
      bool success;
    } ctx{this, src_net, src_path, dst_path, track_progress, true};

    src_net->list_dir(
        src_rel.c_str(),
        [](const storage::FileStat *e, void *c) {
          auto *ctx = static_cast<CopyCtx *>(c);
          if (!ctx->success) return;
          App.feed_wdt();
          if (ctx->track) {
            portENTER_CRITICAL(&ctx->self->progress_mutex_);
            bool cancelled = ctx->self->progress_.cancelled;
            portEXIT_CRITICAL(&ctx->self->progress_mutex_);
            if (cancelled) { ctx->success = false; return; }
          }
          std::string src_entry = Path::join(ctx->src_base, e->name);
          std::string dst_entry = Path::join(ctx->dst_base, e->name);
          if (e->is_dir) {
            if (!ctx->self->perform_directory_copy(src_entry, dst_entry, false))
              ctx->success = false;
          } else {
            if (!ctx->self->perform_file_copy(src_entry, dst_entry, e->size, false))
              ctx->success = false;
            else if (ctx->track) {
              portENTER_CRITICAL(&ctx->self->progress_mutex_);
              ctx->self->progress_.transferred_bytes += e->size;
              portEXIT_CRITICAL(&ctx->self->progress_mutex_);
            }
          }
        },
        &ctx);
    success = ctx.success;
  } else {
    DIR *dir = opendir(src_path.c_str());
    if (!dir) {
      success = false;
    } else {
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr && success) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
          continue;
        App.feed_wdt();
        if (track_progress) {
          portENTER_CRITICAL(&this->progress_mutex_);
          bool cancelled = this->progress_.cancelled;
          portEXIT_CRITICAL(&this->progress_mutex_);
          if (cancelled) { success = false; break; }
        }
        std::string src_entry = Path::join(src_path, entry->d_name);
        std::string dst_entry = Path::join(dst_path, entry->d_name);
        struct stat st;
        if (stat(src_entry.c_str(), &st) != 0) { success = false; break; }
        if (S_ISDIR(st.st_mode)) {
          if (!this->perform_directory_copy(src_entry, dst_entry, false)) { success = false; break; }
        } else {
          if (!this->perform_file_copy(src_entry, dst_entry, st.st_size, false)) { success = false; break; }
          if (track_progress) {
            portENTER_CRITICAL(&this->progress_mutex_);
            this->progress_.transferred_bytes += st.st_size;
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
  return success;
}

bool HttpFileBrowser::perform_directory_move(const std::string &src_path, const std::string &dst_path,
                                              bool track_progress) {
  storage::NetworkStorage *src_net = this->find_network_for_path_(src_path);
  storage::NetworkStorage *dst_net = this->find_network_for_path_(dst_path);
  bool is_network = (src_net != nullptr || dst_net != nullptr);

  if (!is_network && rename(src_path.c_str(), dst_path.c_str()) == 0)
    return true;

  if (src_net != nullptr && dst_net != nullptr && src_net == dst_net) {
    std::string src_rel = strip_network_mount_prefix(src_net, src_path);
    std::string dst_rel = strip_network_mount_prefix(dst_net, dst_path);
    if (src_net->rename(src_rel.c_str(), dst_rel.c_str()) == storage::StorageError::OK)
      return true;
  }

  if (is_network || errno == EXDEV) {
    if (!this->perform_directory_copy(src_path, dst_path, track_progress))
      return false;
    if (src_net != nullptr) {
      std::string src_rel = strip_network_mount_prefix(src_net, src_path);
      this->recursive_delete_directory_network(src_net, src_rel, false);
    } else {
      this->recursive_delete_directory(src_path, false);
    }
    return true;
  }
  return false;
}

// FreeRTOS task functions
void HttpFileBrowser::copy_task(void *params) {
  auto *p = static_cast<CopyTaskParams *>(params);
  if (p->is_directory)
    p->server->perform_directory_copy(p->source, p->destination, p->track_progress);
  else
    p->server->perform_file_copy(p->source, p->destination, p->file_size, p->track_progress);
  delete p;
  vTaskDelete(nullptr);
}

void HttpFileBrowser::move_task(void *params) {
  auto *p = static_cast<MoveTaskParams *>(params);
  if (p->is_directory)
    p->server->perform_directory_move(p->source, p->destination, p->track_progress);
  else
    p->server->perform_file_move(p->source, p->destination, p->file_size, p->track_progress);
  delete p;
  vTaskDelete(nullptr);
}

void HttpFileBrowser::download_task(void *params) {
  auto *p = static_cast<DownloadTaskParams *>(params);
  httpd_req_t *req = p->req;

  FILE *file = fopen(p->filepath.c_str(), "rb");
  if (!file) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    if (p->caller_task)
      xTaskNotifyGive(p->caller_task);
    delete p;
    vTaskDelete(nullptr);
    return;
  }

  std::string content_disposition = "attachment; filename=\"" + p->filename + "\"";
  httpd_resp_set_type(req, p->mime_type.c_str());
  httpd_resp_set_hdr(req, "Content-Disposition", content_disposition.c_str());
  httpd_resp_set_hdr(req, "Content-Length", std::to_string(p->file_size).c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
  uint8_t *buffer = (uint8_t *) heap_caps_malloc(FILE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer)
    buffer = (uint8_t *) malloc(FILE_BUFFER_SIZE);
#else
  uint8_t *buffer = (uint8_t *) malloc(FILE_BUFFER_SIZE);
#endif

  if (!buffer) {
    fclose(file);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    if (p->caller_task)
      xTaskNotifyGive(p->caller_task);
    delete p;
    vTaskDelete(nullptr);
    return;
  }

  size_t total_sent = 0;
  bool success = true;
  while (total_sent < p->file_size) {
    App.feed_wdt();
    size_t to_read = std::min(FILE_BUFFER_SIZE, p->file_size - total_sent);
    size_t bytes_read = fread(buffer, 1, to_read, file);
    if (bytes_read == 0) { success = false; break; }
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer), bytes_read) != ESP_OK) {
      success = false;
      break;
    }
    total_sent += bytes_read;
    if ((total_sent / FILE_BUFFER_SIZE) % 4 == 0)
      vTaskDelay(1);
  }
  fclose(file);
  if (success)
    httpd_resp_send_chunk(req, nullptr, 0);
  free(buffer);

  if (p->caller_task)
    xTaskNotifyGive(p->caller_task);
  delete p;
  vTaskDelete(nullptr);
}

// API handlers
void HttpFileBrowser::handle_api_copy(AsyncWebServerRequest *request) {
  auto *src_p = request->getParam("source");
  auto *dst_p = request->getParam("destination");
  if (!src_p || !dst_p) {
    request->send(400, "application/json", "{\"error\":\"Missing source or destination\"}");
    return;
  }
  std::string source = this->uri_to_filepath(src_p->value().c_str());
  std::string destination = this->uri_to_filepath(dst_p->value().c_str());

  bool is_directory = false;
  if (!this->path_exists(source, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Source not found\"}");
    return;
  }

  off_t file_size = 0;
  if (!is_directory) {
    struct stat st;
    if (stat(source.c_str(), &st) == 0) {
      file_size = st.st_size;
    } else {
      storage::NetworkStorage *net = this->find_network_for_path_(source);
      if (net != nullptr) {
        std::string rel = strip_network_mount_prefix(net, source);
        storage::FileStat fs{};
        if (net->stat(rel.c_str(), &fs) == storage::StorageError::OK)
          file_size = (off_t) fs.size;
      }
    }
  }

  if (!this->is_directory_writable(Path::dirname(destination))) {
    request->send(403, "application/json", "{\"error\":\"Destination directory is not writable\"}");
    return;
  }

  portENTER_CRITICAL(&this->progress_mutex_);
  bool in_progress = this->progress_.in_progress;
  portEXIT_CRITICAL(&this->progress_mutex_);
  if (in_progress) {
    request->send(409, "application/json", "{\"error\":\"Another operation is already in progress\"}");
    return;
  }

  auto *task_params = new CopyTaskParams{this, source, destination, file_size, true, is_directory};
  if (xTaskCreate(copy_task, "http_copy", 4096, task_params, 1, nullptr) == pdPASS) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    delete task_params;
    request->send(500, "application/json", "{\"error\":\"Failed to start copy operation\"}");
  }
}

void HttpFileBrowser::handle_api_move(AsyncWebServerRequest *request) {
  auto *src_p = request->getParam("source");
  auto *dst_p = request->getParam("destination");
  if (!src_p || !dst_p) {
    request->send(400, "application/json", "{\"error\":\"Missing source or destination\"}");
    return;
  }
  std::string source = this->uri_to_filepath(src_p->value().c_str());
  std::string destination = this->uri_to_filepath(dst_p->value().c_str());

  bool is_directory = false;
  if (!this->path_exists(source, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Source not found\"}");
    return;
  }

  off_t file_size = 0;
  if (!is_directory) {
    struct stat st;
    if (stat(source.c_str(), &st) == 0) {
      file_size = st.st_size;
    } else {
      storage::NetworkStorage *net = this->find_network_for_path_(source);
      if (net != nullptr) {
        std::string rel = strip_network_mount_prefix(net, source);
        storage::FileStat fs{};
        if (net->stat(rel.c_str(), &fs) == storage::StorageError::OK)
          file_size = (off_t) fs.size;
      }
    }
  }

  if (!this->is_directory_writable(Path::dirname(destination))) {
    request->send(403, "application/json", "{\"error\":\"Destination directory is not writable\"}");
    return;
  }

  portENTER_CRITICAL(&this->progress_mutex_);
  bool in_progress = this->progress_.in_progress;
  portEXIT_CRITICAL(&this->progress_mutex_);
  if (in_progress) {
    request->send(409, "application/json", "{\"error\":\"Another operation is already in progress\"}");
    return;
  }

  auto *task_params = new MoveTaskParams{this, source, destination, file_size, true, is_directory};
  if (xTaskCreate(move_task, "http_move", 4096, task_params, 1, nullptr) == pdPASS) {
    request->send(200, "application/json", "{\"success\":true}");
  } else {
    delete task_params;
    request->send(500, "application/json", "{\"error\":\"Failed to start move operation\"}");
  }
}

void HttpFileBrowser::handle_api_rename(AsyncWebServerRequest *request) {
  auto *src_p = request->getParam("source");
  auto *name_p = request->getParam("name");
  if (!src_p || !name_p) {
    request->send(400, "application/json", "{\"error\":\"Missing source or name\"}");
    return;
  }
  std::string source = this->uri_to_filepath(src_p->value().c_str());
  std::string new_name = name_p->value().c_str();
  std::string dir_path = source.substr(0, source.find_last_of('/'));
  std::string new_path = Path::join(dir_path, new_name);

  bool is_directory = false;
  if (!this->path_exists(source, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Source file not found\"}");
    return;
  }

  storage::NetworkStorage *net = this->find_network_for_path_(source);
  bool success = false;

  if (net != nullptr) {
    storage::NetworkStorage *dst_net = this->find_network_for_path_(new_path);
    if (dst_net != nullptr && dst_net == net) {
      std::string src_rel = strip_network_mount_prefix(net, source);
      std::string dst_rel = strip_network_mount_prefix(net, new_path);
      success = (net->rename(src_rel.c_str(), dst_rel.c_str()) == storage::StorageError::OK);
      if (!success) {
        if (is_directory)
          success = this->perform_directory_move(source, new_path, false);
        else {
          storage::FileStat fs{};
          off_t sz = 0;
          if (net->stat(src_rel.c_str(), &fs) == storage::StorageError::OK)
            sz = (off_t) fs.size;
          success = this->perform_file_move(source, new_path, sz, false);
        }
      }
    } else {
      if (is_directory)
        success = this->perform_directory_move(source, new_path, false);
      else {
        std::string src_rel = strip_network_mount_prefix(net, source);
        storage::FileStat fs{};
        off_t sz = 0;
        if (net->stat(src_rel.c_str(), &fs) == storage::StorageError::OK)
          sz = (off_t) fs.size;
        success = this->perform_file_move(source, new_path, sz, false);
      }
    }
  } else {
    success = (rename(source.c_str(), new_path.c_str()) == 0);
  }

  if (success)
    request->send(200, "application/json", "{\"success\":true}");
  else
    request->send(500, "application/json", "{\"error\":\"Rename operation failed\"}");
}

void HttpFileBrowser::handle_api_mkdir(AsyncWebServerRequest *request) {
  auto *name_p = request->getParam("name");
  if (!name_p) {
    request->send(400, "application/json", "{\"error\":\"Missing directory name\"}");
    return;
  }
  std::string dir_path = this->uri_to_filepath(name_p->value().c_str());
  if (this->create_dir(dir_path))
    request->send(200, "application/json", "{\"success\":true}");
  else
    request->send(500, "application/json", "{\"error\":\"Mkdir operation failed\"}");
}

void HttpFileBrowser::handle_api_delete(AsyncWebServerRequest *request) {
  if (!this->deletion_enabled_) {
    request->send(403, "application/json", "{\"error\":\"File deletion is disabled\"}");
    return;
  }
  auto *path_p = request->getParam("path");
  if (!path_p) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }
  std::string filepath = this->uri_to_filepath(path_p->value().c_str());

  bool is_directory = false;
  if (!this->path_exists(filepath, is_directory)) {
    request->send(404, "application/json", "{\"error\":\"Path not found\"}");
    return;
  }

  bool success = false;
  if (is_directory) {
    storage::NetworkStorage *net = this->find_network_for_path_(filepath);
    if (net != nullptr) {
      std::string relative = strip_network_mount_prefix(net, filepath);
      int file_count = 0, dir_count = 0;
      this->count_directory_contents_network(net, relative, file_count, dir_count);
      int total_items = file_count + dir_count;
      bool track = (total_items > 5);
      if (track) {
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
      }
      success = this->recursive_delete_directory_network(net, relative, track);
      if (track) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    } else {
      int file_count = 0, dir_count = 0;
      this->count_directory_contents(filepath, file_count, dir_count);
      int total_items = file_count + dir_count;
      bool track = (total_items > 5);
      if (track) {
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
      }
      success = this->recursive_delete_directory(filepath, track);
      if (track) {
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        this->progress_.cancelled = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
      }
    }
  } else {
    success = this->delete_path(filepath, false);
  }

  if (success)
    request->send(200, "application/json", "{\"success\":true}");
  else
    request->send(500, "application/json", "{\"error\":\"Delete operation failed\"}");
}

void HttpFileBrowser::handle_api_mount(AsyncWebServerRequest *request) {
  auto *mp = request->getParam("mount_point");
  if (!mp) {
    request->send(400, "application/json", "{\"error\":\"Missing mount_point parameter\"}");
    return;
  }
  this->deferred_mount_op_.type = DeferredMountOp::MOUNT;
  this->deferred_mount_op_.mount_point = mp->value().c_str();
  this->deferred_mount_op_.schedule_time = millis() + 100;
  request->send(200, "application/json", "{\"success\":true}");
}

void HttpFileBrowser::handle_api_unmount(AsyncWebServerRequest *request) {
  auto *mp = request->getParam("mount_point");
  if (!mp) {
    request->send(400, "application/json", "{\"error\":\"Missing mount_point parameter\"}");
    return;
  }
  this->deferred_mount_op_.type = DeferredMountOp::UNMOUNT;
  this->deferred_mount_op_.mount_point = mp->value().c_str();
  this->deferred_mount_op_.schedule_time = millis() + 100;
  request->send(200, "application/json", "{\"success\":true}");
}

void HttpFileBrowser::handle_api_remount(AsyncWebServerRequest *request) {
  auto *mp = request->getParam("mount_point");
  if (!mp) {
    request->send(400, "application/json", "{\"error\":\"Missing mount_point parameter\"}");
    return;
  }
  this->deferred_mount_op_.type = DeferredMountOp::REMOUNT;
  this->deferred_mount_op_.mount_point = mp->value().c_str();
  this->deferred_mount_op_.schedule_time = millis() + 100;
  request->send(200, "application/json", "{\"success\":true}");
}

void HttpFileBrowser::handle_api_progress(AsyncWebServerRequest *request) {
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
  if (in_progress && start_time > 0 && (millis() - start_time) > 300000) {
    this->progress_.in_progress = false;
    in_progress = false;
  }
  portEXIT_CRITICAL(&this->progress_mutex_);

  std::string json = "{";
  json += "\"in_progress\":" + std::string(in_progress ? "true" : "false");
  if (in_progress) {
    json += ",\"operation\":\"" + operation + "\"";
    json += ",\"source\":\"" + source + "\"";
    json += ",\"destination\":\"" + destination + "\"";
    if (operation == "delete") {
      json += ",\"total_items\":" + std::to_string(total_items);
      json += ",\"processed_items\":" + std::to_string(processed_items);
      if (!current_file.empty())
        json += ",\"current_file\":\"" + current_file + "\"";
      float pct = total_items > 0 ? (processed_items * 100.0f) / total_items : 0.0f;
      char buf[16];
      snprintf(buf, sizeof(buf), "%.1f", pct);
      json += ",\"percentage\":" + std::string(buf);
    } else {
      json += ",\"total_bytes\":" + std::to_string(total_bytes);
      json += ",\"transferred_bytes\":" + std::to_string(transferred_bytes);
      float pct = total_bytes > 0 ? (transferred_bytes * 100.0f) / total_bytes : 0.0f;
      char buf[16];
      snprintf(buf, sizeof(buf), "%.1f", pct);
      json += ",\"percentage\":" + std::string(buf);
    }
    uint32_t elapsed_ms = millis() - start_time;
    json += ",\"elapsed_ms\":" + std::to_string(elapsed_ms);
    if (elapsed_ms > 0 && transferred_bytes > 0) {
      uint64_t speed = ((uint64_t) transferred_bytes * 1000) / elapsed_ms;
      json += ",\"avg_speed\":" + std::to_string(speed);
    }
    if (operation != "delete" && transferred_bytes > 0 && total_bytes > 0) {
      uint64_t total_est = ((uint64_t) elapsed_ms * total_bytes) / transferred_bytes;
      uint64_t remaining = total_est > elapsed_ms ? total_est - elapsed_ms : 0;
      json += ",\"remaining_ms\":" + std::to_string(remaining);
    }
  }
  json += "}";

  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Access-Control-Allow-Credentials", "true");
  auto origin = request->get_header("Origin");
  if (origin.has_value()) {
    response->addHeader("Access-Control-Allow-Origin", origin.value().c_str());
  } else {
    auto host = request->get_header("Host");
    if (host.has_value())
      response->addHeader("Access-Control-Allow-Origin", ("http://" + host.value()).c_str());
  }
  request->send(response);
}

void HttpFileBrowser::handle_api_cancel(AsyncWebServerRequest *request) {
  portENTER_CRITICAL(&this->progress_mutex_);
  if (this->progress_.in_progress) {
    this->progress_.cancelled = true;
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

  auto *fn_p = request->getParam("filename");
  auto *ci_p = request->getParam("chunkIndex");
  auto *tc_p = request->getParam("totalChunks");
  auto *path_p = request->getParam("path");
  auto *fs_p = request->getParam("fileSize");
  if (!fn_p || !ci_p || !tc_p || !path_p || !fs_p) {
    request->send(400, "application/json", "{\"error\":\"Missing required parameters\"}");
    return;
  }

  std::string filename = fn_p->value().c_str();
  int chunk_index = std::stoi(ci_p->value().c_str());
  int total_chunks = std::stoi(tc_p->value().c_str());
  size_t file_size = std::stoull(fs_p->value().c_str());
  std::string dir_path = this->uri_to_filepath(path_p->value().c_str());
  std::string upload_path = Path::join(dir_path, filename);

  portENTER_CRITICAL(&this->progress_mutex_);
  bool cancelled = this->progress_.cancelled;
  portEXIT_CRITICAL(&this->progress_mutex_);

  if (cancelled) {
    if (this->upload_file_) {
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
      remove(upload_path.c_str());
    }
    this->upload_is_network_ = false;
    this->upload_network_storage_ = nullptr;
    this->upload_network_path_.clear();
    this->upload_network_offset_ = 0;
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(200, "application/json", "{\"success\":false,\"cancelled\":true}");
    return;
  }

  if (chunk_index == 0) {
    if (this->upload_file_) {
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
    }
    this->upload_is_network_ = false;
    this->upload_network_storage_ = nullptr;
    this->upload_network_path_.clear();
    this->upload_network_offset_ = 0;

    storage::NetworkStorage *net = this->find_network_for_path_(dir_path);
    if (net != nullptr) {
      std::string rel_dir = strip_network_mount_prefix(net, dir_path);
      std::string rel_upload = strip_network_mount_prefix(net, upload_path);
      storage::FileStat fs{};
      if (net->stat(rel_dir.c_str(), &fs) != storage::StorageError::OK || !fs.is_dir) {
        request->send(400, "application/json", "{\"error\":\"Target is not a directory\"}");
        return;
      }
      this->upload_is_network_ = true;
      this->upload_network_storage_ = net;
      this->upload_network_path_ = rel_upload;
      this->upload_network_offset_ = 0;
    } else {
      struct stat st;
      if (stat(dir_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        request->send(400, "application/json", "{\"error\":\"Target is not a directory\"}");
        return;
      }
      if (!this->is_directory_writable(dir_path)) {
        request->send(403, "application/json", "{\"error\":\"Upload directory is not writable\"}");
        return;
      }
      this->upload_file_ = fopen(upload_path.c_str(), "wb");
      if (!this->upload_file_) {
        request->send(500, "application/json", "{\"error\":\"Failed to open file\"}");
        return;
      }
    }

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
  }

  if (!this->upload_file_ && !this->upload_is_network_) {
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(500, "application/json", "{\"error\":\"Upload not initialized\"}");
    return;
  }

  size_t bytes_to_write = 0;
  const uint8_t *data_ptr = nullptr;

  if (!this->body_buffer_.empty()) {
    data_ptr = reinterpret_cast<const uint8_t *>(this->body_buffer_.data());
    bytes_to_write = this->body_buffer_.size();
  } else {
    httpd_req_t *req = *request;
    size_t content_len = req->content_len;
    if (content_len > 0) {
      if (!this->chunk_buffer_ || this->chunk_buffer_size_ < content_len) {
        if (this->chunk_buffer_)
          free(this->chunk_buffer_);
#if defined(USE_ESP_IDF) && defined(HTTP_FILE_BROWSER_USE_PSRAM)
        this->chunk_buffer_ = (uint8_t *) heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!this->chunk_buffer_)
          this->chunk_buffer_ = (uint8_t *) malloc(content_len);
#else
        this->chunk_buffer_ = (uint8_t *) malloc(content_len);
#endif
        this->chunk_buffer_size_ = content_len;
      }
      size_t total_received = 0;
      while (total_received < content_len) {
        int ret = httpd_req_recv(req, reinterpret_cast<char *>(this->chunk_buffer_) + total_received,
                                 content_len - total_received);
        if (ret <= 0) {
          if (this->upload_file_) {
            fclose(this->upload_file_);
            this->upload_file_ = nullptr;
            remove(upload_path.c_str());
          }
          portENTER_CRITICAL(&this->progress_mutex_);
          this->progress_.in_progress = false;
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

  if (bytes_to_write > 0) {
    bool write_success = false;
    size_t written = 0;

    if (this->upload_is_network_ && this->upload_network_storage_ != nullptr) {
      if (this->upload_network_storage_->write_chunk(this->upload_network_path_.c_str(), data_ptr,
                                                      this->upload_network_offset_, bytes_to_write,
                                                      &written) == storage::StorageError::OK) {
        write_success = true;
        this->upload_network_offset_ += written;
      }
      if (!write_success) {
        this->upload_network_storage_->remove(this->upload_network_path_.c_str());
        this->upload_is_network_ = false;
        this->upload_network_storage_ = nullptr;
        this->upload_network_path_.clear();
        this->upload_network_offset_ = 0;
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
        request->send(500, "application/json", "{\"error\":\"Network storage write failed\"}");
        return;
      }
    } else {
      written = fwrite(data_ptr, 1, bytes_to_write, this->upload_file_);
      write_success = (written == bytes_to_write);
      if (!write_success) {
        fclose(this->upload_file_);
        this->upload_file_ = nullptr;
        remove(upload_path.c_str());
        portENTER_CRITICAL(&this->progress_mutex_);
        this->progress_.in_progress = false;
        portEXIT_CRITICAL(&this->progress_mutex_);
        request->send(500, "application/json", "{\"error\":\"Write failed\"}");
        return;
      }
      if (chunk_index % 10 == 0)
        fflush(this->upload_file_);
    }

    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.transferred_bytes += written;
    portEXIT_CRITICAL(&this->progress_mutex_);
  }

  this->body_buffer_.clear();

  if (chunk_index == total_chunks - 1) {
    if (this->upload_file_) {
      fflush(this->upload_file_);
      fclose(this->upload_file_);
      this->upload_file_ = nullptr;
    }
    if (this->upload_is_network_) {
      this->upload_is_network_ = false;
      this->upload_network_storage_ = nullptr;
      this->upload_network_path_.clear();
      this->upload_network_offset_ = 0;
    }
    portENTER_CRITICAL(&this->progress_mutex_);
    this->progress_.in_progress = false;
    this->progress_.cancelled = false;
    portEXIT_CRITICAL(&this->progress_mutex_);
    request->send(200, "application/json", "{\"success\":true,\"complete\":true}");
  } else {
    request->send(200, "application/json", "{\"success\":true,\"complete\":false}");
  }
}

void HttpFileBrowser::handle_api_exists(AsyncWebServerRequest *request) {
  auto *path_p = request->getParam("path");
  if (!path_p) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }
  std::string filepath = this->uri_to_filepath(path_p->value().c_str());
  struct stat st;
  bool exists = (stat(filepath.c_str(), &st) == 0);
  request->send(200, "application/json", (std::string("{\"exists\":") + (exists ? "true" : "false") + "}").c_str());
}

void HttpFileBrowser::handle_api_dirisempty(AsyncWebServerRequest *request) {
  auto *path_p = request->getParam("path");
  if (!path_p) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }
  std::string dirpath = this->uri_to_filepath(path_p->value().c_str());
  storage::NetworkStorage *net = this->find_network_for_path_(dirpath);

  if (net != nullptr) {
    std::string relative = strip_network_mount_prefix(net, dirpath);
    storage::FileStat fs{};
    if (net->stat(relative.c_str(), &fs) != storage::StorageError::OK || !fs.is_dir) {
      request->send(400, "application/json", "{\"error\":\"Path is not a directory\"}");
      return;
    }
    bool empty = this->is_directory_empty(dirpath);
    request->send(200, "application/json", (std::string("{\"is_empty\":") + (empty ? "true" : "false") + "}").c_str());
  } else {
    struct stat st;
    if (stat(dirpath.c_str(), &st) != 0) {
      request->send(404, "application/json", "{\"error\":\"Path not found\"}");
      return;
    }
    if (!S_ISDIR(st.st_mode)) {
      request->send(400, "application/json", "{\"error\":\"Path is not a directory\"}");
      return;
    }
    bool empty = this->is_directory_empty(dirpath);
    request->send(200, "application/json", (std::string("{\"is_empty\":") + (empty ? "true" : "false") + "}").c_str());
  }
}

void HttpFileBrowser::handle_api_dirinfo(AsyncWebServerRequest *request) {
  auto *path_p = request->getParam("path");
  if (!path_p) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }
  std::string dirpath = this->uri_to_filepath(path_p->value().c_str());
  struct stat st;
  if (stat(dirpath.c_str(), &st) != 0) {
    request->send(404, "application/json", "{\"error\":\"Path not found\"}");
    return;
  }
  if (!S_ISDIR(st.st_mode)) {
    request->send(400, "application/json", "{\"error\":\"Path is not a directory\"}");
    return;
  }
  int file_count = 0, dir_count = 0;
  this->count_directory_contents(dirpath, file_count, dir_count);
  bool is_empty = (file_count == 0 && dir_count == 0);
  std::string json = "{";
  json += "\"is_empty\":" + std::string(is_empty ? "true" : "false");
  json += ",\"file_count\":" + std::to_string(file_count);
  json += ",\"dir_count\":" + std::to_string(dir_count);
  json += ",\"total_items\":" + std::to_string(file_count + dir_count);
  json += "}";
  request->send(200, "application/json", json.c_str());
}

void HttpFileBrowser::handle_api_fileinfo(AsyncWebServerRequest *request) {
  auto *path_p = request->getParam("path");
  if (!path_p) {
    request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
    return;
  }
  std::string filepath = this->uri_to_filepath(path_p->value().c_str());
  storage::NetworkStorage *net = this->find_network_for_path_(filepath);

  std::string storage_type = "local";
  size_t size = 0;
  bool is_dir = false;
  time_t mtime = 0;
  bool stat_ok = false;

  if (net != nullptr) {
    storage::StorageInfo info{};
    net->get_info(&info);
    if (!info.is_mounted) {
      request->send(503, "application/json", "{\"error\":\"Network storage not connected\"}");
      return;
    }
    std::string relative = strip_network_mount_prefix(net, filepath);
    storage::FileStat fs{};
    if (net->stat(relative.c_str(), &fs) == storage::StorageError::OK) {
      stat_ok = true;
      size = fs.size;
      is_dir = fs.is_dir;
      if (info.name != nullptr)
        storage_type = std::string(info.name);
    }
  } else {
    struct stat st;
    if (stat(filepath.c_str(), &st) == 0) {
      stat_ok = true;
      size = st.st_size;
      is_dir = S_ISDIR(st.st_mode);
      mtime = st.st_mtime;
    }
  }

  if (!stat_ok) {
    request->send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
  }

  std::string json = "{";
  json += "\"name\":\"" + get_filename_from_path(filepath) + "\"";
  json += ",\"path\":\"" + filepath + "\"";
  json += ",\"size\":" + std::to_string(size);
  json += ",\"is_directory\":" + std::string(is_dir ? "true" : "false");
  json += ",\"last_modified\":" + std::to_string(mtime);
  json += ",\"storage_type\":\"" + storage_type + "\"";
  json += "}";
  request->send(200, "application/json", json.c_str());
}

// Path utility implementations
std::string Path::file_name(const std::string &path) {
  size_t pos = path.rfind(separator);
  return pos != std::string::npos ? path.substr(pos + 1) : path;
}

std::string Path::dirname(const std::string &path) {
  size_t pos = path.rfind(separator);
  if (pos != std::string::npos)
    return pos == 0 ? "/" : path.substr(0, pos);
  return ".";
}

bool Path::is_absolute(const std::string &path) { return !path.empty() && path[0] == separator; }

bool Path::has_trailing_slash(const std::string &path) { return !path.empty() && path.back() == separator; }

std::string Path::join(const std::string &first, const std::string &second) {
  if (first.empty()) return second;
  if (second.empty()) return first;
  std::string result = first;
  if (!has_trailing_slash(first) && !is_absolute(second))
    result.push_back(separator);
  if (has_trailing_slash(first) && is_absolute(second))
    result.pop_back();
  result.append(second);
  return result;
}

std::string Path::remove_root_path(const std::string &path, const std::string &root) {
  if (path.find(root) != 0) return path;
  if (path.size() <= root.size()) return "/";
  std::string result = path.substr(root.size());
  if (result.empty() || result[0] != '/') result = "/" + result;
  return result;
}

std::vector<std::string> Path::split_path(const std::string &path) {
  std::vector<std::string> parts;
  std::string cur = path;
  size_t pos = 0;
  while ((pos = cur.find(separator)) != std::string::npos) {
    std::string part = cur.substr(0, pos);
    if (!part.empty()) parts.push_back(part);
    cur.erase(0, pos + 1);
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

std::string Path::extension(const std::string &file) {
  size_t pos = file.find_last_of('.');
  if (pos == std::string::npos || pos == file.size() - 1) return "";
  return file.substr(pos + 1);
}

std::string Path::file_type(const std::string &file) {
  static const std::array<std::pair<const char *, const char *>, 20> types = {{
      {"mp3", "Audio (MP3)"}, {"wav", "Audio (WAV)"}, {"flac", "Audio (FLAC)"}, {"png", "Image (PNG)"},
      {"jpg", "Image (JPG)"}, {"jpeg", "Image (JPEG)"}, {"gif", "Image (GIF)"}, {"bmp", "Image (BMP)"},
      {"txt", "Text (TXT)"}, {"log", "Text (LOG)"}, {"csv", "Text (CSV)"}, {"html", "Web (HTML)"},
      {"css", "Web (CSS)"}, {"js", "Web (JS)"}, {"json", "Data (JSON)"}, {"xml", "Data (XML)"},
      {"zip", "Archive (ZIP)"}, {"gz", "Archive (GZ)"}, {"tar", "Archive (TAR)"}, {"mp4", "Video (MP4)"},
  }};
  std::string ext = extension(file);
  if (ext.empty()) return "File";
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const auto &[e, t] : types)
    if (ext == e) return t;
  return "File (" + ext + ")";
}

std::string Path::mime_type(const std::string &file) {
  static const std::array<std::pair<const char *, const char *>, 25> mimes = {{
      {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"flac", "audio/flac"},
      {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
      {"gif", "image/gif"}, {"bmp", "image/bmp"}, {"txt", "text/plain"},
      {"log", "text/plain"}, {"csv", "text/csv"}, {"html", "text/html"},
      {"htm", "text/html"}, {"css", "text/css"}, {"js", "application/javascript"},
      {"json", "application/json"}, {"xml", "application/xml"}, {"pdf", "application/pdf"},
      {"zip", "application/zip"}, {"gz", "application/gzip"}, {"tar", "application/x-tar"},
      {"mp4", "video/mp4"}, {"avi", "video/x-msvideo"}, {"webm", "video/webm"},
      {"mkv", "video/x-matroska"},
  }};
  std::string ext = extension(file);
  if (ext.empty()) return "application/octet-stream";
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const auto &[e, m] : mimes)
    if (ext == e) return m;
  return "application/octet-stream";
}

}  // namespace esphome::http_file_browser
