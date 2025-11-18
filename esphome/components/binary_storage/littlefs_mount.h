#if defined(USE_ESP_IDF)

#include <string>
#include <memory>
#include "esphome/core/component.h"
#include "lfs.h"

namespace esphome {
namespace binary_storage {

class LittleFSMount : public Component {
 public:
  void set_storage(BinaryStorage *storage) { storage_ = storage; }
  void set_mount_path(const std::string &mount_path) { mount_path_ = mount_path; }
  void set_auto_format(bool auto_format) { auto_format_ = auto_format; }

  void setup() override;
  void dump_config() override;

  bool format();
  bool remount();
  void list_files() const;

  bool mount();
  bool unmount();

 protected:
  BinaryStorage *storage_{nullptr};
  std::string mount_path_;
  bool auto_format_{false};

  std::unique_ptr<lfs_t> lfs_;
  std::unique_ptr<lfs_config> lfs_cfg_;
  std::unique_ptr<uint8_t[]> read_buffer_;
  std::unique_ptr<uint8_t[]> prog_buffer_;
  std::unique_ptr<uint8_t[]> lookahead_buffer_;
  void *lfs_context_{nullptr};

  bool mounted_{false};

  bool init_lfs_config_();
  bool mount_();
  bool unmount();
  void register_with_storage_host_();
  void register_with_vfs_();
};

}  // namespace binary_storage
}  // namespace esphome

#endif  // USED_ESP_IDF
