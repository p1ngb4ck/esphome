#pragma once

#include "esphome/core/defines.h"

#ifdef USE_FTP_SERVER

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

namespace esphome::ftp {

class FtpServer : public Component {
 public:
  static FtpServer *create();

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { this->port_ = port; }
  void set_root(const char *root) { this->root_ = root; }
  void set_max_sessions(uint8_t count) { this->max_sessions_ = count; }
  void add_user(const char *name, const char *password, bool write) {
    if (this->user_count_ < MAX_USERS)
      this->users_[this->user_count_++] = User{name, password, write};
  }
#ifdef USE_FTP_TFTP_SERVER
  void set_tftp(uint16_t port, bool write, uint32_t max_file_size) {
    this->tftp_port_ = port;
    this->tftp_write_ = write;
    this->tftp_max_ = max_file_size;
  }
#endif

 protected:
  static constexpr uint8_t MAX_USERS = 8;
  static constexpr uint8_t MAX_SESSIONS = 4;
  static constexpr size_t PATH_MAX_LEN = 256;
  static constexpr size_t LINE_MAX_LEN = 512;
  static constexpr size_t CHUNK = 4096;
  static constexpr size_t LIST_MAX = 2000;

  struct User {
    const char *name;
    const char *password;
    bool write;
  };
  struct Session {
    int ctrl{-1};
    int pasv{-1};
    int user{-1};
    bool logged{false};
    uint64_t rest{0};
    size_t line_len{0};
    char cwd[PATH_MAX_LEN]{};
    char rnfr[PATH_MAX_LEN]{};
    char name[64]{};
    char line[LINE_MAX_LEN]{};
  };
  using Entries = std::vector<storage::FileStat, RAMAllocator<storage::FileStat>>;

  static void task_entry_(void *arg);
  void run_();
  bool open_listener_(int *fd, uint16_t port, int type);
  void accept_();
  void close_session_(Session &s);
  void input_(Session &s);
  void command_(Session &s, char *line);
  void reply_(Session &s, const char *text);
  bool path_of_(const Session &s, const char *arg, char *out) const;
  storage::PathStorage *resolve_(const char *full, const char **rel) const;
  void on_loop_(std::function<void()> &&op);
  storage::StorageError await_(std::function<storage::StorageError(storage::CompletionCallback &&)> &&start);
  bool may_write_(const Session &s) const { return s.user >= 0 && this->users_[s.user].write; }
  void pasv_(Session &s, bool extended);
  int data_accept_(Session &s);
  void list_(Session &s, const char *arg, uint8_t style);
  void retr_(Session &s, const char *arg);
  void stor_(Session &s, const char *arg);
  storage::StorageError stat_(const char *full, storage::FileStat *st);
  uint8_t *take_buffer_();
  void give_buffer_();
#ifdef USE_FTP_TFTP_SERVER
  void tftp_request_();
  void tftp_error_(int fd, const void *to, uint32_t to_len, uint16_t code, const char *text);
  void tftp_send_file_(int fd, const void *to, uint32_t to_len, const char *full);
  void tftp_receive_file_(int fd, const void *to, uint32_t to_len, const char *full);
#endif

  uint16_t port_{21};
  const char *root_{"/"};
  uint8_t max_sessions_{2};
  User users_[MAX_USERS]{};
  uint8_t user_count_{0};
  Session *sessions_{nullptr};
  int listen_fd_{-1};
  StaticTask task_;
  storage::StorageError cb_err_{storage::StorageError::STORAGE_ERROR_OK};
  uint8_t *buf_{nullptr};
#ifdef USE_FTP_TFTP_SERVER
  uint16_t tftp_port_{69};
  bool tftp_write_{false};
  uint32_t tftp_max_{0};
  int tftp_fd_{-1};
#endif
};

}

#endif
