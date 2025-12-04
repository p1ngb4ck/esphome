#pragma once

#include "esphome/core/component.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace esphome {
namespace sd_storage {

enum class CardType : uint8_t {
  UNKNOWN = 0,
  SDIO = 1,
  MMC = 2,
  SDHC = 3,
  SDXC = 3,
  SDSC = 4,
};

struct FileInfo {
  std::string path;
  uint32_t size;
  bool is_directory;
};

// Forward declaration for mount callback
using mount_ready_callback_t = std::function<void(const std::string &mount_path)>;

// Base class for both SDMMC and SPI implementations
class SdStorageBase : public Component {
 public:
  // Public mount/unmount methods
  virtual bool mount_card() = 0;
  virtual void unmount_card() = 0;

  // Directory operations
  virtual std::vector<FileInfo> list_directory(const std::string &path) = 0;

  // Card info
  virtual bool is_mounted() const = 0;
  virtual const std::string &get_mount_path() const = 0;

  // Mount notification system
  virtual void add_mount_ready_callback(const mount_ready_callback_t &callback) = 0;
};

}  // namespace sd_storage
}  // namespace esphome
