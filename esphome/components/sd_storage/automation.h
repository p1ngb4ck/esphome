#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "sd_storage_base.h"

// Include the appropriate header based on which mode is enabled
#if defined(USE_SD_STORAGE_SDMMC)
#include "sd_storage.h"
#endif

#if defined(USE_SD_STORAGE_SPI)
#include "sd_storage_spi.h"
#endif

namespace esphome {
namespace sd_storage {

// Triggers - templated to work with both SdMmc and SdSpi
template<typename T> class CardMountedTrigger : public Trigger<std::string> {
 public:
  explicit CardMountedTrigger(T *parent) {
    parent->add_mount_ready_callback([this](const std::string &mount_path) { this->trigger(mount_path); });
  }
};

// Actions - templated to work with both SdMmc and SdSpi
template<typename T, typename... Ts> class MountCardAction : public Action<Ts...> {
 public:
  explicit MountCardAction(T *sd_storage) : sd_storage_(sd_storage) {}

  void play(Ts... x) override {
    if (this->sd_storage_->mount_card()) {
      ESP_LOGI("sd_storage", "Card mounted successfully via automation");
    } else {
      ESP_LOGE("sd_storage", "Failed to mount card via automation");
    }
  }

 protected:
  T *sd_storage_;
};

template<typename T, typename... Ts> class UnmountCardAction : public Action<Ts...> {
 public:
  explicit UnmountCardAction(T *sd_storage) : sd_storage_(sd_storage) {}

  void play(Ts... x) override {
    this->sd_storage_->unmount_card();
    ESP_LOGI("sd_storage", "Card unmounted via automation");
  }

 protected:
  T *sd_storage_;
};

template<typename T, typename... Ts> class ListFilesAction : public Action<Ts...> {
 public:
  explicit ListFilesAction(T *sd_storage) : sd_storage_(sd_storage) {}

  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) override {
    auto path = this->path_.value(x...);
    if (path.empty()) {
      path = this->sd_storage_->get_mount_path();
    }

    ESP_LOGI("sd_storage", "Listing files in: %s", path.c_str());
    auto files = this->sd_storage_->list_directory(path);
    for (const auto &file : files) {
      if (file.is_directory) {
        ESP_LOGI("sd_storage", "  [DIR]  %s", file.path.c_str());
      } else {
        ESP_LOGI("sd_storage", "  [FILE] %s (%u bytes)", file.path.c_str(), file.size);
      }
    }
    ESP_LOGI("sd_storage", "Total: %zu items", files.size());
  }

 protected:
  T *sd_storage_;
};

// Conditions - templated to work with both SdMmc and SdSpi
template<typename T, typename... Ts> class CardMountedCondition : public Condition<Ts...> {
 public:
  explicit CardMountedCondition(T *sd_storage) : sd_storage_(sd_storage) {}

  bool check(Ts... x) override { return this->sd_storage_->is_mounted(); }

 protected:
  T *sd_storage_;
};

}  // namespace sd_storage
}  // namespace esphome
