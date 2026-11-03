#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include <cstddef>
#include <cstdint>

namespace esphome::package_manager {

using SymbolLookup = uintptr_t (*)(const char *name, void *ctx);

struct PackageExit {
  void (*fn)(void *);
  void *arg;
};

class LoadedPackage {
 public:
  bool ok() const { return this->base_ != 0; }
  void *symbol(const char *name) const;
  uint32_t data_bytes() const { return this->data_end_ - this->data_start_; }
  bool owns(const void *address) const;

 protected:
  friend class PackageLoader;

  uintptr_t base_{0};
  const uint8_t *image_{nullptr};
  uint32_t data_start_{0};
  uint32_t data_end_{0};
  const uint8_t *symtab_{nullptr};
  const char *strtab_{nullptr};
  const uint32_t *hash_{nullptr};
  uint32_t symbols_{0};
  uintptr_t fini_array_{0};
  uint32_t fini_count_{0};
  uintptr_t fini_{0};
  PackageExit *exits_{nullptr};
  uint16_t exit_count_{0};
  uint16_t exit_capacity_{0};
  LoadedPackage *next_{nullptr};
};

class PackageLoader {
 public:
  bool load(const uint8_t *image, size_t size, LoadedPackage &pkg, SymbolLookup lookup, void *ctx);
  void unload(LoadedPackage &pkg);
  const char *last_error() const { return this->error_; }
  static int at_exit(void (*fn)(void *), void *arg, void *dso);

 protected:
  struct DataPage {
    uintptr_t vaddr;
    void *memory;
    void *mapped;
    uint16_t users;
  };

  bool fail_(const char *error);
  bool map_pages_(uintptr_t start, uintptr_t end);
  void unmap_pages_(uintptr_t start, uintptr_t end);
  DataPage *page_(uintptr_t vaddr);
  bool relocate_(LoadedPackage &pkg, const uint8_t *table, uint32_t bytes, SymbolLookup lookup, void *ctx);
  uintptr_t resolve_(const LoadedPackage &pkg, uint32_t index, SymbolLookup lookup, void *ctx);

  DataPage *pages_{nullptr};
  uint16_t page_count_{0};
  uint16_t page_capacity_{0};
  const char *error_{nullptr};
  char detail_[96]{};
};

}

#endif
