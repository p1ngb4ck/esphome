#pragma once
#include "esphome/core/defines.h"
#ifdef USE_ESP_IDF
#ifdef USE_COMPONENT_AS_LIB

#include <cstdint>
#include "esphome/core/component.h"

namespace esphome {
namespace module_host {

// Generic placeholder Component for a component_as_lib .so component. It is registered normally in
// main.cpp (via cg.register_component), so App drives it and it holds a slot in looping_components_.
//
// At setup() it calls disable_loop(): it moves to LOOP_DONE / the inactive section of
// looping_components_, so App never calls its loop() -- it costs nothing until the real component is
// loaded. When module_host has loaded the .so and __lib_construct has produced the real component(s),
// it calls attach(); enable_loop() moves the stub back to active, and from then on the stub's loop()
// drives each real component through its normal state machine via call() (CONSTRUCTION -> SETUP ->
// LOOP), exactly as App would. The stub is type-agnostic: it only ever holds and drives Component*,
// so it needs no per-target subclassing.
class LibComponentStub : public Component {
 public:
  void setup() override { this->disable_loop(); }
  void loop() override {
    for (uint32_t i = 0; i < this->real_count_; i++)
      this->reals_[i]->call();
  }
  // Forward config dump to the real component(s). App drives the stub's dump_config (it is in
  // components_); the reals are not, so without this their config would never be printed. The stub is
  // registered late, so its dump turn comes after the .so has loaded and attach() ran.
  void dump_config() override {
    for (uint32_t i = 0; i < this->real_count_; i++)
      this->reals_[i]->dump_config();
  }
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Called by module_host (main-loop context) after the .so is loaded and __lib_construct produced
  // the real component(s). enable_loop() is safe from here.
  void attach(Component **reals, uint32_t count) {
    this->reals_ = reals;
    this->real_count_ = count;
    this->enable_loop();
  }
  bool attached() const { return this->reals_ != nullptr; }

 protected:
  Component **reals_{nullptr};
  uint32_t real_count_{0};
};

// Registry keyed by the .so basename (e.g. "mcp4461"). component_as_lib registers, from main.cpp
// (where the entity statics are visible), the per-target stub + the external-dependency array that
// __lib_construct needs. module_host looks the entry up by the .so basename when it loads the file.
struct LibEntry {
  const char *name;
  LibComponentStub *stub;
  void **deps;
  uint32_t deps_count;
};

void register_lib(const char *name, LibComponentStub *stub, void **deps, uint32_t deps_count);
const LibEntry *find_lib(const char *name);

}  // namespace module_host
}  // namespace esphome

#endif  // USE_COMPONENT_AS_LIB
#endif  // USE_ESP_IDF
