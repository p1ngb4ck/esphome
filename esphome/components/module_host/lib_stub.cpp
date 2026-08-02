#include "lib_stub.h"
#if defined(USE_ESP_IDF) && defined(USE_COMPONENT_AS_LIB)

#include <cstring>
#include <vector>

namespace esphome {
namespace module_host {

namespace {
// Function-local static avoids any static-init-order dependency: component_as_lib registers entries
// from main.cpp setup() (before App.setup()), module_host reads them later from its loop().
std::vector<LibEntry> &registry() {
  static std::vector<LibEntry> r;
  return r;
}
}  // namespace

void register_lib(const char *name, LibComponentStub *stub, void **deps, uint32_t deps_count) {
  registry().push_back(LibEntry{name, stub, deps, deps_count});
}

const LibEntry *find_lib(const char *name) {
  for (auto &e : registry()) {
    if (std::strcmp(e.name, name) == 0)
      return &e;
  }
  return nullptr;
}

}  // namespace module_host
}  // namespace esphome

#endif  // USE_ESP_IDF && USE_COMPONENT_AS_LIB
