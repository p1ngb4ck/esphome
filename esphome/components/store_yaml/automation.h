#pragma once

#include "esphome/core/defines.h"
#if defined(USE_STORE_YAML) && defined(USE_STORE_YAML_EXPORT)

#include "esphome/core/automation.h"
#include "store_yaml.h"

namespace esphome::store_yaml {

// store_yaml.export_to_storage: write the embedded YAML to a storage path —
// raw compressed envelope (raw: true) or decompressed original file tree
// (raw: false, default). Fork extension on top of upstream PR #16445.
template<typename... Ts> class ExportToStorageAction : public Action<Ts...> {
 public:
  explicit ExportToStorageAction(StoreYamlComponent *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, path)
  void set_raw(bool raw) { this->raw_ = raw; }

  void play(Ts... x) override {
    const std::string path = this->path_.value(x...);
    this->parent_->export_to_storage(path.c_str(), this->raw_);
  }

 protected:
  StoreYamlComponent *parent_;
  bool raw_{false};
};

}  // namespace esphome::store_yaml

#endif  // USE_STORE_YAML && USE_STORE_YAML_EXPORT
