#pragma once

#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)

#include "esphome/core/automation.h"
#include "usb_storage.h"

namespace esphome::usb_storage {

// Triggers: an entry's stick was mounted
class DeviceMountedTrigger : public Trigger<const char *> {
 public:
  DeviceMountedTrigger(USBStorageClient *parent, int entry) {
    parent->add_on_mounted_callback([this, entry](int mounted_entry, const char *mount_path) {
      if (mounted_entry == entry)
        this->trigger(mount_path);
    });
  }
};

// Actions and the condition: the stick mounted at a path ("" : the first mounted one); none mounted, nothing done
template<typename... Ts> class RemountDeviceAction : public Action<Ts...> {
 public:
  RemountDeviceAction(USBStorageClient *client, const char *mount_path) : client_(client), mount_path_(mount_path) {}

  void play(Ts... x) override {
    if (USBStorageDevice *stick = this->client_->find_mounted(this->mount_path_))
      stick->remount_device();
  }

 protected:
  USBStorageClient *client_;
  const char *mount_path_;
};

template<typename... Ts> class UnmountDeviceAction : public Action<Ts...> {
 public:
  UnmountDeviceAction(USBStorageClient *client, const char *mount_path) : client_(client), mount_path_(mount_path) {}

  void play(Ts... x) override {
    if (USBStorageDevice *stick = this->client_->find_mounted(this->mount_path_))
      stick->unmount_device();
  }

 protected:
  USBStorageClient *client_;
  const char *mount_path_;
};

template<typename... Ts> class ListFilesAction : public Action<Ts...> {
 public:
  ListFilesAction(USBStorageClient *client, const char *mount_path) : client_(client), mount_path_(mount_path) {}

  TEMPLATABLE_VALUE(const char *, path)

  void play(Ts... x) override {
    USBStorageDevice *stick = this->client_->find_mounted(this->mount_path_);
    if (stick == nullptr)
      return;
    const char *path = this->path_.value(x...);
    if (path == nullptr || path[0] == '\0')
      path = stick->get_mount_path();

    stick->log_list_dir_start_(path);
    stick->list_dir(path, &USBStorageDevice::log_list_dir_entry, nullptr);
  }

 protected:
  USBStorageClient *client_;
  const char *mount_path_;
};

// Conditions
template<typename... Ts> class DeviceMountedCondition : public Condition<Ts...> {
 public:
  DeviceMountedCondition(USBStorageClient *client, const char *mount_path) : client_(client), mount_path_(mount_path) {}

  bool check(Ts... x) override { return this->client_->is_mounted(this->mount_path_); }

 protected:
  USBStorageClient *client_;
  const char *mount_path_;
};

}  // namespace esphome::usb_storage

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 || USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S31 || USE_ESP32_VARIANT_ESP32H4
