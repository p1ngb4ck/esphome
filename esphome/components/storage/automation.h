#pragma once

#include "esphome/core/automation.h"
#include "storage.h"
#include "storage_device.h"

namespace esphome {
namespace storage {

/// Trigger that fires when a storage device is registered
class DeviceAddedTrigger : public Trigger<StorageDevice *> {
 public:
  explicit DeviceAddedTrigger(Storage *parent) {
    parent->add_on_device_added_callback([this](StorageDevice *device) { this->trigger(device); });
  }
};

/// Trigger that fires when a storage device is unregistered
class DeviceRemovedTrigger : public Trigger<StorageDevice *> {
 public:
  explicit DeviceRemovedTrigger(Storage *parent) {
    parent->add_on_device_removed_callback([this](StorageDevice *device) { this->trigger(device); });
  }
};

/// Trigger that fires when a storage device status changes (mount/unmount)
class DeviceChangedTrigger : public Trigger<StorageDevice *> {
 public:
  explicit DeviceChangedTrigger(Storage *parent) {
    parent->add_on_device_changed_callback([this](StorageDevice *device) { this->trigger(device); });
  }
};

}  // namespace storage
}  // namespace esphome
