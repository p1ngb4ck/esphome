#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <sys/stat.h>

// Forward declaration
namespace esphome {
namespace storage {
class Storage;
class NetworkStorage;
}  // namespace storage
}  // namespace esphome

namespace esphome {
namespace http_file_browser {

static const char *const TAG = "http_file_browser";

// Forward declaration
class HttpFileBrowser;

// Architecture-specific buffer sizes
// Use larger buffers if PSRAM is guaranteed available (network latency > PSRAM speed difference)
#if defined(USE_ESP32_VARIANT_ESP32P4)
#if defined(HTTP_FILE_BROWSER_USE_PSRAM)
static constexpr size_t FILE_BUFFER_SIZE = 65536;  // 64KB for P4 with guaranteed PSRAM
#else
static constexpr size_t FILE_BUFFER_SIZE = 16384;  // 16KB for P4 (SRAM)
#endif
static constexpr size_t MAX_DIR_ENTRIES = 512;
#elif defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)
#if defined(HTTP_FILE_BROWSER_USE_PSRAM)
static constexpr size_t FILE_BUFFER_SIZE = 32768;  // 32KB for S2/S3 with guaranteed PSRAM
#else
static constexpr size_t FILE_BUFFER_SIZE = 8192;  // 8KB for S2/S3 (SRAM)
#endif
static constexpr size_t MAX_DIR_ENTRIES = 512;
#else
static constexpr size_t FILE_BUFFER_SIZE = 4096;  // 4KB default
static constexpr size_t MAX_DIR_ENTRIES = 256;
#endif

// Path utilities
struct Path {
  static constexpr char separator = '/';

  // Return the name of the file/directory
  static std::string file_name(const std::string &path);

  // Get directory name (parent directory of path)
  static std::string dirname(const std::string &path);

  // Is the path an absolute path?
  static bool is_absolute(const std::string &path);

  // Does the path have a trailing slash?
  static bool has_trailing_slash(const std::string &path);

  // Join two paths
  static std::string join(const std::string &first, const std::string &second);

  // Remove root path from absolute path
  static std::string remove_root_path(const std::string &path, const std::string &root);

  // Split path into components
  static std::vector<std::string> split_path(const std::string &path);

  // Get file extension
  static std::string extension(const std::string &file);

  // Get human-readable file type
  static std::string file_type(const std::string &file);

  // Get MIME type for file
  static std::string mime_type(const std::string &file);
};

// File info structure for directory listings
struct FileInfo {
  std::string name;
  std::string path;
  bool is_directory;
  bool is_mount_point{false};
  bool mounted{true};  // Only relevant for mount points
  size_t size;
  time_t modified;
  // Space information (only for mount points)
  uint64_t total_space{0};
  uint64_t free_space{0};
  bool has_space_info{false};
};

class HttpFileBrowser : public Component, public AsyncWebHandler {
 public:
  HttpFileBrowser(web_server_base::WebServerBase *base) : base_(base) {}
  ~HttpFileBrowser();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // AsyncWebHandler interface
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleUpload(AsyncWebServerRequest *request, const PlatformString &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

  // Configuration setters
  void set_storage(storage::Storage *storage) { this->storage_ = storage; }
  void set_root_path(const std::string &root_path) { this->root_path_ = root_path; }
  void set_url_prefix(const std::string &url_prefix) { this->url_prefix_ = url_prefix; }
  void set_upload_enabled(bool enabled) { this->upload_enabled_ = enabled; }
  void set_download_enabled(bool enabled) { this->download_enabled_ = enabled; }
  void set_deletion_enabled(bool enabled) { this->deletion_enabled_ = enabled; }
  void set_auth(const std::string &username, const std::string &password) {
    this->username_ = username;
    this->password_ = password;
    this->auth_enabled_ = true;
  }

  // Getters
  const std::string &get_root_path() const { return this->root_path_; }
  const std::string &get_url_prefix() const { return this->url_prefix_; }
  bool is_upload_enabled() const { return this->upload_enabled_; }
  bool is_download_enabled() const { return this->download_enabled_; }
  bool is_deletion_enabled() const { return this->deletion_enabled_; }
  bool get_network_file_stat(storage::NetworkStorage *net_storage, const std::string &path, struct stat &file_stat);
  bool handle_network_directory_listing(AsyncWebServerRequest *request, storage::NetworkStorage *net_storage,
                                        const std::string &path);
  bool handle_network_file_download(AsyncWebServerRequest *request, storage::NetworkStorage *net_storage,
                                    const std::string &path);

 protected:
  // Web server base reference
  web_server_base::WebServerBase *base_;

  // Storage reference
  storage::Storage *storage_{nullptr};

  // Configuration
  std::string root_path_{};
  std::string url_prefix_;
  bool upload_enabled_{false};
  bool download_enabled_{true};
  bool deletion_enabled_{false};

  // Authentication
  std::string username_;
  std::string password_;
  bool auth_enabled_{false};

  // Upload state (for handleUpload)
  std::string upload_directory_;
  std::string upload_filename_;
  FILE *upload_file_{nullptr};

  // Body buffer (for handleBody - stores POST request body)
  std::string body_buffer_;

  // Deferred mount/unmount operations (to avoid memory corruption during HTTP response)
  struct DeferredMountOp {
    enum Type { NONE, MOUNT, UNMOUNT, REMOUNT };
    Type type{NONE};
    std::string mount_point;
    uint32_t schedule_time{0};
  };
  DeferredMountOp deferred_mount_op_;

  // Progress tracking for long operations
  struct {
    std::string operation;  // "copy", "move", "upload", or "delete"
    std::string source;
    std::string destination;
    std::string current_file;  // Current file being processed (for delete operations)
    size_t total_bytes{0};
    size_t transferred_bytes{0};
    int total_items{0};      // Total files/dirs to process (for delete operations)
    int processed_items{0};  // Files/dirs processed so far (for delete operations)
    bool in_progress{false};
    bool cancelled{false};
    uint32_t start_time{0};
  } progress_;

  // Mutex to protect progress structure when accessed from multiple threads
  portMUX_TYPE progress_mutex_ = portMUX_INITIALIZER_UNLOCKED;

  // Task parameter structures for background operations
  struct CopyTaskParams {
    HttpFileBrowser *server;
    std::string source;
    std::string destination;
    off_t file_size;
    bool track_progress;
    bool is_directory;
  };

  struct MoveTaskParams {
    HttpFileBrowser *server;
    std::string source;
    std::string destination;
    off_t file_size;
    bool track_progress;
    bool is_directory;
  };

  struct DownloadTaskParams {
    httpd_req_t *req;
    std::string filepath;
    std::string filename;
    std::string mime_type;
    size_t file_size;
    TaskHandle_t caller_task;
  };

  // Static task functions for FreeRTOS
  static void copy_task(void *params);
  static void move_task(void *params);
  static void download_task(void *params);

  // Upload progress tracking
  size_t upload_total_size_{0};
  size_t upload_received_size_{0};
  size_t upload_bytes_since_yield_{0};  // Track bytes to yield CPU periodically

  // Buffer pool (allocated once in setup, partitioned for different operations)
  // Layout: [upload_buffer | download_buffer | copy_buffer]
  uint8_t *buffer_pool_{nullptr};
  size_t buffer_pool_size_{0};

  // Partitioned buffers (pointers into buffer_pool_)
  uint8_t *upload_buffer_{nullptr};
  uint8_t *download_buffer_{nullptr};
  uint8_t *copy_buffer_{nullptr};

  // Reusable chunk buffer for uploads (avoids repeated allocations)
  std::unique_ptr<uint8_t[], void (*)(void *)> chunk_buffer_;
  size_t chunk_buffer_size_{0};

  // Network storage chunked upload state
  bool upload_is_network_{false};                             // True if current chunked upload targets network storage
  storage::NetworkStorage *upload_network_storage_{nullptr};  // Network storage pointer for chunked upload
  std::string upload_network_path_;                           // Relative path for network storage upload
  size_t upload_network_offset_{0};                           // Current byte offset for next chunk

  // Helper methods
  bool is_directory_writable(const std::string &dir_path);
  std::string uri_to_filepath(const std::string &uri);
  std::string url_decode(const std::string &src);
  esphome::FixedVector<FileInfo> list_directory(const std::string &path);

  // Network storage aware file operation helpers
  // These automatically route to NetworkStorage API when path is on network storage
  bool path_exists(const std::string &path, bool &is_directory);
  bool delete_path(const std::string &path, bool is_directory);
  bool create_dir(const std::string &path);
  bool read_file_content(const std::string &path, std::vector<uint8_t> &data);
  bool write_file_content(const std::string &path, const uint8_t *data, size_t length);

  // HTML generation helpers
  std::string generate_html_header(const std::string &title);
  std::string generate_html_footer();
  std::string generate_breadcrumb(const std::string &current_path);
  std::string generate_file_row(const FileInfo &info, const std::string &uri_prefix);

  // Specific handlers for different GET requests
  void handle_directory_listing(AsyncWebServerRequest *request, const std::string &filepath);
  void handle_file_download(AsyncWebServerRequest *request, const std::string &filepath);
  void handle_file_upload(AsyncWebServerRequest *request, const std::string &filename);

  // API handlers
  void handle_api_copy(AsyncWebServerRequest *request);
  void handle_api_move(AsyncWebServerRequest *request);
  void handle_api_rename(AsyncWebServerRequest *request);
  void handle_api_mkdir(AsyncWebServerRequest *request);
  void handle_api_delete(AsyncWebServerRequest *request);
  void handle_api_exists(AsyncWebServerRequest *request);
  void handle_api_dirisempty(AsyncWebServerRequest *request);
  void handle_api_dirinfo(AsyncWebServerRequest *request);
  void handle_api_progress(AsyncWebServerRequest *request);
  void handle_api_cancel(AsyncWebServerRequest *request);
  void handle_api_upload_chunk(AsyncWebServerRequest *request);
  void handle_api_mount(AsyncWebServerRequest *request);
  void handle_api_unmount(AsyncWebServerRequest *request);
  void handle_api_remount(AsyncWebServerRequest *request);
  void handle_api_fileinfo(AsyncWebServerRequest *request);

  // Directory helpers
  bool is_directory_empty(const std::string &path);
  void count_directory_contents(const std::string &path, int &file_count, int &dir_count);
  void count_directory_contents_network(storage::NetworkStorage *net_storage, const std::string &path, int &file_count,
                                        int &dir_count);
  bool recursive_delete_directory(const std::string &path, bool track_progress = false);
  bool recursive_delete_directory_network(storage::NetworkStorage *net_storage, const std::string &path,
                                          bool track_progress = false);

  // Mount status helper
  bool is_mount_point_mounted(const std::string &mount_path);
  bool get_mount_space_info(const std::string &mount_path, uint64_t &total, uint64_t &free);

  // Path helpers
  static std::string get_filename_from_path(const std::string &path);
  static std::string strip_network_mount_prefix(storage::NetworkStorage *net_storage, const std::string &path);

  // JSON parsing helper
  struct ApiRequest {
    std::string source;
    std::string destination;
    std::string name;
    std::string path;
    std::string mount_point;
    std::string device;
  };
  bool parse_json_request(const uint8_t *body, size_t body_len, ApiRequest &req);

  // File operation helpers (with progress tracking)
  bool perform_file_copy(const std::string &src_path, const std::string &dst_path, off_t file_size,
                         bool track_progress = false);
  bool perform_file_move(const std::string &src_path, const std::string &dst_path, off_t file_size,
                         bool track_progress = false);

  // Directory operation helpers (recursive copy/move)
  bool perform_directory_copy(const std::string &src_path, const std::string &dst_path, bool track_progress = false);
  bool perform_directory_move(const std::string &src_path, const std::string &dst_path, bool track_progress = false);
};

}  // namespace http_file_browser
}  // namespace esphome
