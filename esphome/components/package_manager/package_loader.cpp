#include "package_loader.h"

#if defined(USE_ESP_IDF) && defined(USE_PACKAGE_MANAGER)

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_mmu_map.h>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

extern "C" __attribute__((weak)) uintptr_t component_as_lib_find_symbol(const char *name);
extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

namespace esphome::package_manager {

static const char *const TAG = "package_manager.loader";
static constexpr uintptr_t DATA_WINDOW = 0x08000000;
static constexpr uintptr_t PAGE = 0x10000;
static constexpr uint16_t EM_RISCV = 243;
static constexpr uint16_t ET_DYN = 3;
static constexpr uint32_t PT_LOAD = 1;
static constexpr uint32_t PT_DYNAMIC = 2;
static constexpr int32_t DT_NULL = 0;
static constexpr int32_t DT_PLTRELSZ = 2;
static constexpr int32_t DT_HASH = 4;
static constexpr int32_t DT_STRTAB = 5;
static constexpr int32_t DT_SYMTAB = 6;
static constexpr int32_t DT_RELA = 7;
static constexpr int32_t DT_RELASZ = 8;
static constexpr int32_t DT_INIT = 12;
static constexpr int32_t DT_FINI = 13;
static constexpr int32_t DT_TEXTREL = 22;
static constexpr int32_t DT_JMPREL = 23;
static constexpr int32_t DT_INIT_ARRAY = 25;
static constexpr int32_t DT_FINI_ARRAY = 26;
static constexpr int32_t DT_INIT_ARRAYSZ = 27;
static constexpr int32_t DT_FINI_ARRAYSZ = 28;
static constexpr uint32_t R_RISCV_NONE = 0;
static constexpr uint32_t R_RISCV_32 = 1;
static constexpr uint32_t R_RISCV_RELATIVE = 3;
static constexpr uint32_t R_RISCV_JUMP_SLOT = 5;
static constexpr uint8_t STB_WEAK = 2;

struct ElfSym {
  uint32_t name;
  uint32_t value;
  uint32_t size;
  uint8_t info;
  uint8_t other;
  uint16_t shndx;
};

struct ElfRela {
  uint32_t offset;
  uint32_t info;
  int32_t addend;
};

static LoadedPackage *loaded_head = nullptr;

template<typename T> static T read_at(const uint8_t *base, size_t offset) {
  T value;
  std::memcpy(&value, base + offset, sizeof(T));
  return value;
}

static uint32_t elf_hash(const char *name) {
  uint32_t h = 0;
  for (const auto *p = reinterpret_cast<const uint8_t *>(name); *p != 0; p++) {
    h = (h << 4) + *p;
    const uint32_t g = h & 0xf0000000;
    if (g != 0)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

bool LoadedPackage::owns(const void *address) const {
  const auto value = reinterpret_cast<uintptr_t>(address);
  return this->ok() && value >= this->base_ + this->data_start_ && value < this->base_ + this->data_end_;
}

void *LoadedPackage::symbol(const char *name) const {
  if (!this->ok() || this->symtab_ == nullptr)
    return nullptr;
  auto match = [this, name](uint32_t index) -> void * {
    ElfSym sym;
    std::memcpy(&sym, this->symtab_ + index * sizeof(ElfSym), sizeof(sym));
    if (sym.shndx == 0 || std::strcmp(this->strtab_ + sym.name, name) != 0)
      return nullptr;
    return reinterpret_cast<void *>(this->base_ + sym.value);
  };
  if (this->hash_ != nullptr && this->hash_[0] != 0) {
    const uint32_t buckets = this->hash_[0];
    const uint32_t *chain = this->hash_ + 2 + buckets;
    for (uint32_t i = this->hash_[2 + elf_hash(name) % buckets]; i != 0; i = chain[i]) {
      if (void *found = match(i))
        return found;
    }
    return nullptr;
  }
  for (uint32_t i = 1; i < this->symbols_; i++) {
    if (void *found = match(i))
      return found;
  }
  return nullptr;
}

bool PackageLoader::fail_(const char *error) {
  this->error_ = error;
  return false;
}

PackageLoader::DataPage *PackageLoader::page_(uintptr_t vaddr) {
  for (uint16_t i = 0; i < this->page_count_; i++) {
    if (this->pages_[i].vaddr == vaddr)
      return &this->pages_[i];
  }
  return nullptr;
}

bool PackageLoader::map_pages_(uintptr_t start, uintptr_t end) {
  const uintptr_t first = start & ~(PAGE - 1);
  const uintptr_t last = (end + PAGE - 1) & ~(PAGE - 1);
  for (uintptr_t vaddr = first; vaddr < last; vaddr += PAGE) {
    if (DataPage *page = this->page_(vaddr)) {
      page->users++;
      continue;
    }
    if (this->page_count_ == this->page_capacity_) {
      const uint16_t capacity = this->page_capacity_ == 0 ? 8 : this->page_capacity_ * 2;
      RAMAllocator<DataPage> allocator(RAMAllocator<DataPage>::ALLOC_EXTERNAL);
      DataPage *grown = allocator.allocate(capacity);
      if (grown == nullptr) {
        this->unmap_pages_(first, vaddr);
        return this->fail_("no PSRAM for the page table");
      }
      if (this->pages_ != nullptr) {
        std::memcpy(grown, this->pages_, this->page_count_ * sizeof(DataPage));
        allocator.deallocate(this->pages_, this->page_capacity_);
      }
      this->pages_ = grown;
      this->page_capacity_ = capacity;
    }
    void *memory = heap_caps_aligned_alloc(PAGE, PAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      this->unmap_pages_(first, vaddr);
      return this->fail_("no PSRAM for the package data");
    }
    esp_cache_msync(memory, PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_paddr_t paddr = 0;
    mmu_target_t target{};
    void *mapped = nullptr;
    esp_err_t err = esp_mmu_vaddr_to_paddr(memory, &paddr, &target);
    if (err == ESP_OK) {
      err = esp_mmu_map_virt(vaddr, paddr, PAGE, MMU_TARGET_PSRAM0,
                             static_cast<mmu_mem_caps_t>(MMU_MEM_CAP_READ | MMU_MEM_CAP_WRITE | MMU_MEM_CAP_8BIT |
                                                         MMU_MEM_CAP_32BIT),
                             ESP_MMU_MMAP_FLAG_PADDR_SHARED, &mapped);
    }
    if (err != ESP_OK) {
      heap_caps_free(memory);
      this->unmap_pages_(first, vaddr);
      snprintf(this->detail_, sizeof(this->detail_), "mapping data page 0x%08x failed: %s",
               static_cast<unsigned>(vaddr), esp_err_to_name(err));
      return this->fail_(this->detail_);
    }
    this->pages_[this->page_count_++] = DataPage{vaddr, memory, mapped, 1};
  }
  return true;
}

void PackageLoader::unmap_pages_(uintptr_t start, uintptr_t end) {
  const uintptr_t first = start & ~(PAGE - 1);
  const uintptr_t last = (end + PAGE - 1) & ~(PAGE - 1);
  for (uintptr_t vaddr = first; vaddr < last; vaddr += PAGE) {
    DataPage *page = this->page_(vaddr);
    if (page == nullptr || --page->users != 0)
      continue;
    esp_cache_msync(reinterpret_cast<void *>(vaddr), PAGE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_mmu_unmap(page->mapped);
    heap_caps_free(page->memory);
    *page = this->pages_[--this->page_count_];
  }
  if (this->page_count_ == 0 && this->pages_ != nullptr) {
    RAMAllocator<DataPage>(RAMAllocator<DataPage>::ALLOC_EXTERNAL).deallocate(this->pages_, this->page_capacity_);
    this->pages_ = nullptr;
    this->page_capacity_ = 0;
  }
}

uintptr_t PackageLoader::resolve_(const LoadedPackage &pkg, uint32_t index, SymbolLookup lookup, void *ctx) {
  ElfSym sym;
  std::memcpy(&sym, pkg.symtab_ + index * sizeof(ElfSym), sizeof(sym));
  if (sym.shndx != 0)
    return pkg.base_ + sym.value;
  const char *name = pkg.strtab_ + sym.name;
  if (std::strcmp(name, "__cxa_atexit") == 0)
    return reinterpret_cast<uintptr_t>(&PackageLoader::at_exit);
  uintptr_t address = component_as_lib_find_symbol != nullptr ? component_as_lib_find_symbol(name) : 0;
  if (address == 0 && lookup != nullptr)
    address = lookup(name, ctx);
  if (address == 0 && (sym.info >> 4) != STB_WEAK) {
    snprintf(this->detail_, sizeof(this->detail_), "symbol %s not found", name);
    this->error_ = this->detail_;
  }
  return address;
}

bool PackageLoader::relocate_(LoadedPackage &pkg, const uint8_t *table, uint32_t bytes, SymbolLookup lookup,
                              void *ctx) {
  for (uint32_t offset = 0; offset + sizeof(ElfRela) <= bytes; offset += sizeof(ElfRela)) {
    const auto rela = read_at<ElfRela>(table, offset);
    const uint32_t type = rela.info & 0xff;
    if (type == R_RISCV_NONE)
      continue;
    if (rela.offset < pkg.data_start_ || rela.offset + sizeof(uint32_t) > pkg.data_end_)
      return this->fail_("relocation outside the package data");
    uint32_t value;
    if (type == R_RISCV_RELATIVE) {
      value = pkg.base_ + rela.addend;
    } else if (type == R_RISCV_32 || type == R_RISCV_JUMP_SLOT) {
      const uint32_t index = rela.info >> 8;
      this->error_ = nullptr;
      const uintptr_t address = index != 0 ? this->resolve_(pkg, index, lookup, ctx) : 0;
      if (this->error_ != nullptr)
        return false;
      value = address + (type == R_RISCV_32 ? rela.addend : 0);
    } else {
      snprintf(this->detail_, sizeof(this->detail_), "relocation type %u not supported", static_cast<unsigned>(type));
      return this->fail_(this->detail_);
    }
    std::memcpy(reinterpret_cast<void *>(pkg.base_ + rela.offset), &value, sizeof(value));
  }
  return true;
}

bool PackageLoader::load(const uint8_t *image, size_t size, LoadedPackage &pkg, SymbolLookup lookup, void *ctx) {
  this->error_ = nullptr;
  if (pkg.ok())
    return true;
  if (size < 52 || std::memcmp(image, "\x7f" "ELF", 4) != 0 || image[4] != 1 || image[5] != 1 ||
      read_at<uint16_t>(image, 16) != ET_DYN || read_at<uint16_t>(image, 18) != EM_RISCV)
    return this->fail_("not a RISC-V shared object");
  const uint32_t phoff = read_at<uint32_t>(image, 28);
  const uint16_t phentsize = read_at<uint16_t>(image, 42);
  const uint16_t phnum = read_at<uint16_t>(image, 44);
  if (phentsize < 32 || phoff + static_cast<size_t>(phnum) * phentsize > size)
    return this->fail_("program headers outside the image");
  const uintptr_t base = reinterpret_cast<uintptr_t>(image);
  uint32_t data_start = UINT32_MAX;
  uint32_t data_end = 0;
  uint32_t dyn_offset = 0;
  uint32_t dyn_bytes = 0;
  for (uint16_t i = 0; i < phnum; i++) {
    const uint8_t *ph = image + phoff + i * phentsize;
    const uint32_t type = read_at<uint32_t>(ph, 0);
    const uint32_t offset = read_at<uint32_t>(ph, 4);
    const uint32_t vaddr = read_at<uint32_t>(ph, 8);
    const uint32_t filesz = read_at<uint32_t>(ph, 16);
    const uint32_t memsz = read_at<uint32_t>(ph, 20);
    if (offset + static_cast<size_t>(filesz) > size)
      return this->fail_("segment outside the image");
    if (type == PT_DYNAMIC) {
      dyn_offset = offset;
      dyn_bytes = filesz;
    } else if (type == PT_LOAD && vaddr >= DATA_WINDOW) {
      data_start = std::min(data_start, vaddr);
      data_end = std::max(data_end, vaddr + memsz);
    } else if (type == PT_LOAD && (offset != vaddr || memsz != filesz)) {
      return this->fail_("code segment is not its file image");
    }
  }
  if (dyn_bytes == 0)
    return this->fail_("no dynamic section");
  if (data_start == UINT32_MAX)
    data_start = data_end = DATA_WINDOW;
  if (data_end > data_start && !this->map_pages_(base + data_start, base + data_end))
    return false;
  pkg.base_ = base;
  pkg.image_ = image;
  pkg.data_start_ = data_start;
  pkg.data_end_ = data_end;
  for (uint16_t i = 0; i < phnum; i++) {
    const uint8_t *ph = image + phoff + i * phentsize;
    const uint32_t vaddr = read_at<uint32_t>(ph, 8);
    if (read_at<uint32_t>(ph, 0) != PT_LOAD || vaddr < DATA_WINDOW)
      continue;
    const uint32_t filesz = read_at<uint32_t>(ph, 16);
    const uint32_t memsz = read_at<uint32_t>(ph, 20);
    std::memcpy(reinterpret_cast<void *>(base + vaddr), image + read_at<uint32_t>(ph, 4), filesz);
    std::memset(reinterpret_cast<void *>(base + vaddr + filesz), 0, memsz - filesz);
  }
  uint32_t rela = 0, rela_bytes = 0, jmprel = 0, jmprel_bytes = 0, init = 0, init_array = 0, init_bytes = 0;
  for (uint32_t offset = 0; offset + 8 <= dyn_bytes; offset += 8) {
    const int32_t tag = read_at<int32_t>(image, dyn_offset + offset);
    const uint32_t value = read_at<uint32_t>(image, dyn_offset + offset + 4);
    if (tag == DT_NULL)
      break;
    switch (tag) {
      case DT_HASH:
        pkg.hash_ = reinterpret_cast<const uint32_t *>(base + value);
        break;
      case DT_STRTAB:
        pkg.strtab_ = reinterpret_cast<const char *>(base + value);
        break;
      case DT_SYMTAB:
        pkg.symtab_ = reinterpret_cast<const uint8_t *>(base + value);
        break;
      case DT_RELA:
        rela = value;
        break;
      case DT_RELASZ:
        rela_bytes = value;
        break;
      case DT_JMPREL:
        jmprel = value;
        break;
      case DT_PLTRELSZ:
        jmprel_bytes = value;
        break;
      case DT_INIT:
        init = value;
        break;
      case DT_FINI:
        pkg.fini_ = value != 0 ? base + value : 0;
        break;
      case DT_INIT_ARRAY:
        init_array = value;
        break;
      case DT_INIT_ARRAYSZ:
        init_bytes = value;
        break;
      case DT_FINI_ARRAY:
        pkg.fini_array_ = base + value;
        break;
      case DT_FINI_ARRAYSZ:
        pkg.fini_count_ = value / sizeof(uintptr_t);
        break;
      case DT_TEXTREL:
        this->unload(pkg);
        return this->fail_("package needs relocations in its code");
      default:
        break;
    }
  }
  if (pkg.symtab_ == nullptr || pkg.strtab_ == nullptr) {
    this->unload(pkg);
    return this->fail_("no dynamic symbol table");
  }
  pkg.symbols_ = pkg.hash_ != nullptr ? pkg.hash_[1]
                                      : static_cast<uint32_t>(reinterpret_cast<const uint8_t *>(pkg.strtab_) -
                                                              pkg.symtab_) / sizeof(ElfSym);
  if ((rela_bytes != 0 && !this->relocate_(pkg, image + rela, rela_bytes, lookup, ctx)) ||
      (jmprel_bytes != 0 && !this->relocate_(pkg, image + jmprel, jmprel_bytes, lookup, ctx))) {
    const char *error = this->error_;
    this->unload(pkg);
    return this->fail_(error);
  }
  pkg.next_ = loaded_head;
  loaded_head = &pkg;
  if (init != 0)
    reinterpret_cast<void (*)()>(base + init)();
  for (uint32_t i = 0; i < init_bytes / sizeof(uintptr_t); i++) {
    uintptr_t fn;
    std::memcpy(&fn, reinterpret_cast<const void *>(base + init_array + i * sizeof(uintptr_t)), sizeof(fn));
    if (fn != 0 && fn != UINTPTR_MAX)
      reinterpret_cast<void (*)()>(fn)();
  }
  ESP_LOGD(TAG, "loaded at 0x%08x, data 0x%08x-0x%08x", static_cast<unsigned>(base),
           static_cast<unsigned>(base + data_start), static_cast<unsigned>(base + data_end));
  return true;
}

void PackageLoader::unload(LoadedPackage &pkg) {
  if (!pkg.ok())
    return;
  for (LoadedPackage **link = &loaded_head; *link != nullptr; link = &(*link)->next_) {
    if (*link != &pkg)
      continue;
    *link = pkg.next_;
    while (pkg.exit_count_ > 0) {
      const PackageExit exit = pkg.exits_[--pkg.exit_count_];
      exit.fn(exit.arg);
    }
    for (uint32_t i = pkg.fini_count_; i > 0; i--) {
      uintptr_t fn;
      std::memcpy(&fn, reinterpret_cast<const void *>(pkg.fini_array_ + (i - 1) * sizeof(uintptr_t)), sizeof(fn));
      if (fn != 0 && fn != UINTPTR_MAX)
        reinterpret_cast<void (*)()>(fn)();
    }
    if (pkg.fini_ != 0)
      reinterpret_cast<void (*)()>(pkg.fini_)();
    break;
  }
  if (pkg.exits_ != nullptr)
    RAMAllocator<PackageExit>(RAMAllocator<PackageExit>::ALLOC_EXTERNAL).deallocate(pkg.exits_, pkg.exit_capacity_);
  if (pkg.data_end_ > pkg.data_start_)
    this->unmap_pages_(pkg.base_ + pkg.data_start_, pkg.base_ + pkg.data_end_);
  pkg = LoadedPackage{};
}

int PackageLoader::at_exit(void (*fn)(void *), void *arg, void *dso) {
  LoadedPackage *pkg = loaded_head;
  while (pkg != nullptr && !pkg->owns(dso))
    pkg = pkg->next_;
  if (pkg == nullptr)
    return __cxa_atexit(fn, arg, dso);
  if (pkg->exit_count_ == pkg->exit_capacity_) {
    const uint16_t capacity = pkg->exit_capacity_ == 0 ? 8 : pkg->exit_capacity_ * 2;
    RAMAllocator<PackageExit> allocator(RAMAllocator<PackageExit>::ALLOC_EXTERNAL);
    PackageExit *grown = allocator.allocate(capacity);
    if (grown == nullptr)
      return -1;
    if (pkg->exits_ != nullptr) {
      std::memcpy(grown, pkg->exits_, pkg->exit_count_ * sizeof(PackageExit));
      allocator.deallocate(pkg->exits_, pkg->exit_capacity_);
    }
    pkg->exits_ = grown;
    pkg->exit_capacity_ = capacity;
  }
  pkg->exits_[pkg->exit_count_++] = PackageExit{fn, arg};
  return 0;
}

}

#endif
