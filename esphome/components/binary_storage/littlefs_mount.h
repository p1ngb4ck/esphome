if defined (USE_ESP_IDF)

#include <string>
#include <memory>
#include "esphome/core/component.h"
#include "lfs.h"

  namespace esphome {
  namespace binary_storage {

  class BlockDeviceConfig {
   public:
    uint32_t block_size;
    uint32_t block_count;
    uint32_t read_size;
    uint32_t prog_size;
    uint32_t lookahead_size;
  };

  class BinaryStorage {
   public:
    virtual ~BinaryStorage() = default;
    virtual int block_read(uint32_t lba, void *buffer, uint32_t size) = 0;
    virtual int block_prog(const void *buffer, uint32_t lba, uint32_t size) = 0;
    virtual int block_erase(uint32_t lba) = 0;
    virtual int block_sync() = 0;
    virtual BlockDeviceConfig get_block_config() const = 0;
    virtual std::string get_device_name() const = 0;
    virtual std::string get_device_type() const = 0;
  };

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

    // Make mount and unmount public
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
