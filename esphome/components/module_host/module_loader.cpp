#ifdef USE_ESP_IDF

#include "module_loader.h"
#include "esphome/core/log.h"
#include "esphome/components/storage/storage.h"

#include <cstring>
#include <esp_heap_caps.h>

namespace esphome {
namespace module_host {

static const char *const TAG = "module_host.loader";

// Read the whole file at rel_path on fs into a freshly allocated PSRAM buffer (caller frees).
static uint8_t *read_file_psram_(storage::FilesystemStorage *fs, const char *rel_path, size_t *out_size,
                                 const char **err) {
  storage::FileStat st{};
  if (fs->stat(rel_path, &st) != storage::StorageError::OK) {
    *err = "module file not found";
    return nullptr;
  }
  if (st.is_dir || st.size == 0) {
    *err = "module path is not a regular file";
    return nullptr;
  }
  // Prefer PSRAM (modules can be large and this is control-plane), fall back to internal RAM.
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(st.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr)
    buf = static_cast<uint8_t *>(heap_caps_malloc(st.size, MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    *err = "out of memory for module buffer";
    return nullptr;
  }

  storage::FileHandle *handle = nullptr;
  if (fs->open(rel_path, handle, storage::OpenMode::READ) != storage::StorageError::OK) {
    heap_caps_free(buf);
    *err = "failed to open module file";
    return nullptr;
  }
  size_t off = 0;
  while (off < st.size) {
    size_t n = 0;
    storage::StorageError e = fs->read(handle, buf + off, st.size - off, &n);
    if (e != storage::StorageError::OK) {
      *err = "read error while loading module";
      break;
    }
    if (n == 0)
      break;  // short read: loop condition below decides success
    off += n;
  }
  fs->close(handle);
  if (off != st.size) {
    heap_caps_free(buf);
    if (*err == nullptr)
      *err = "short read while loading module";
    return nullptr;
  }
  *out_size = st.size;
  return buf;
}

LoadedModule ModuleLoader::load(const char *vfs_path) {
  LoadedModule mod;
  this->last_error_ = nullptr;
  if (vfs_path == nullptr) {
    this->last_error_ = "null module path";
    return mod;
  }

  // Resolve the path through OUR storage registry, not the loader's own VFS.
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(vfs_path, &rel);
  if (ps == nullptr) {
    this->last_error_ = "no storage mounted for module path";
    return mod;
  }
  storage::FilesystemStorage *fs = ps->as_filesystem();
  if (fs == nullptr) {
    this->last_error_ = "storage backing module path is not a filesystem";
    return mod;
  }

  size_t size = 0;
  uint8_t *buf = read_file_psram_(fs, rel, &size, &this->last_error_);
  if (buf == nullptr)
    return mod;

  // Allocate the ELF handle on the heap so it stays put for the module's lifetime (the loader keeps
  // pointers into it; a value copy of the handle would dangle). This mirrors dlmod's elf_dl.
  mod.elf_ = static_cast<esp_elf_t *>(heap_caps_calloc(1, sizeof(esp_elf_t), MALLOC_CAP_8BIT));
  if (mod.elf_ == nullptr) {
    heap_caps_free(buf);
    this->last_error_ = "out of memory for ELF handle";
    return mod;
  }

  // Relocate from the buffer -- no dlopen, no base-path prefix.
  if (esp_elf_init(mod.elf_) != 0) {
    heap_caps_free(mod.elf_);
    mod.elf_ = nullptr;
    heap_caps_free(buf);
    this->last_error_ = "esp_elf_init failed";
    return mod;
  }
  if (esp_elf_relocate(mod.elf_, buf) != 0) {
    esp_elf_deinit(mod.elf_);
    heap_caps_free(mod.elf_);
    mod.elf_ = nullptr;
    heap_caps_free(buf);
    this->last_error_ = "esp_elf_relocate failed";
    return mod;
  }

  mod.buffer_ = buf;  // keep alive: the dynamic symtab may reference strings in the buffer
  mod.loaded_ = true;
  ESP_LOGD(TAG, "loaded module '%s' (%u bytes, %u symbols)", vfs_path, static_cast<unsigned>(size),
           static_cast<unsigned>(mod.elf_->num));
  return mod;
}

void ModuleLoader::unload(LoadedModule &mod) {
  if (mod.elf_ != nullptr) {
    if (mod.loaded_)
      esp_elf_deinit(mod.elf_);
    heap_caps_free(mod.elf_);
    mod.elf_ = nullptr;
  }
  mod.loaded_ = false;
  if (mod.buffer_ != nullptr) {
    heap_caps_free(mod.buffer_);
    mod.buffer_ = nullptr;
  }
}

void *LoadedModule::symbol(const char *name) const {
  if (!this->loaded_ || this->elf_ == nullptr || this->elf_->symtab == nullptr || name == nullptr)
    return nullptr;
  // Same lookup dlsym performs internally: linear scan of the module's dynamic symbol table.
  for (uint16_t i = 0; i < this->elf_->num; i++) {
    if (this->elf_->symtab[i].name != nullptr && std::strcmp(this->elf_->symtab[i].name, name) == 0)
      return this->elf_->symtab[i].addr;
  }
  return nullptr;
}

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF
