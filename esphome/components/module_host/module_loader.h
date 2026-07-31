#pragma once
#ifdef USE_ESP_IDF

#include "esp_elf.h"  // esp_elf_t, esp_elf_init/relocate/deinit (pulls the symtab struct in)

namespace esphome {
namespace module_host {

// A module loaded from a buffer via esp_elf_relocate(). Symbol resolution walks the ELF's own
// dynamic symbol table (esp_elf_t.symtab), the same table dlsym uses internally -- no dlopen, so no
// loader-owned VFS access and no CONFIG_ELF_FILE_SYSTEM_BASE_PATH prefix.
class LoadedModule {
 public:
  bool ok() const { return this->loaded_; }
  // Resolve an exported symbol by name (entry points are MODULE_EXPORT / visibility default).
  void *symbol(const char *name) const;

  esp_elf_t elf_{};           // the relocated ELF
  uint8_t *buffer_{nullptr};  // the .so bytes, kept alive while loaded (symtab may point into it)
  bool loaded_{false};
};

class ModuleLoader {
 public:
  // Read the .so THROUGH the ESPHome storage interface: resolve the VFS path to its
  // FilesystemStorage (global_storage_registry->resolve_path -> PathStorage::as_filesystem), read
  // the whole file into a PSRAM buffer with the storage read() API, then esp_elf_relocate() from
  // that buffer. Works with any FilesystemStorage backend, not just VFS-mounted filesystems.
  LoadedModule load(const char *vfs_path);
  void unload(LoadedModule &mod);
  const char *last_error() const { return this->last_error_; }

 protected:
  const char *last_error_{nullptr};
};

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
