#pragma once
#include <stdint.h>

// ABI contract between the ESPHome firmware (host) and a loadable module (.so).
//
// Both sides compile against this EXACT header. The abi_version field is the guard against drift:
// a module built for one firmware must refuse to run on another. The host passes a table of
// function pointers to the module; the module may only call the host through this table (plus
// whatever the elf_loader's generated symbol table, esp_all_symbol.c, exposes).
//
// The entry points the module exports are declared extern "C" so their names are unmangled and
// dlsym(handle, "module_init") resolves them by plain name against the module's symbol table
// (confirmed in the loader: dlsym matches mod->elf->symtab[i].name by strcmp).

#ifdef __cplusplus
extern "C" {
#endif

#define ESPHOME_MODULE_ABI_VERSION 1u

// project_so links the module with -fvisibility=hidden and --strip-all, so every symbol the host
// resolves with dlsym MUST be explicitly exported with default visibility. Mark each entry point
// with MODULE_EXPORT.
#define MODULE_EXPORT __attribute__((visibility("default")))

// Functions the host exposes to the module. Keep this small and stable -- every entry is an ABI
// commitment. For L1 it is just enough to prove the host<-module call direction works.
typedef struct {
  uint32_t abi_version;          // must equal ESPHOME_MODULE_ABI_VERSION
  void (*log)(const char *msg);  // host logging, info level
  uint32_t (*millis)(void);      // host millis()
} module_host_api_t;

// Entry-point signatures the module MUST export (all extern "C"):
//   module_init      -- runs the module's own .init_array (C++ global ctors -- the loader does NOT,
//                       confirmed in esp_elf.c), stores the host table, returns the ABI version on
//                       success or 0 on mismatch.
//   module_loop      -- called from the host loop once ACTIVE (optional for L1; may be absent).
//   module_add       -- L1 proof function (pure compute, no globals).
//   module_ctor_ran  -- L1 probe: did a file-scope C++ ctor run BEFORE module_init? (see
//                       demo_module.cpp). Returns 1 if the loader ran init_array, 0 if not.
typedef uint32_t (*module_init_fn)(const module_host_api_t *host);
typedef void (*module_loop_fn)(void);
typedef int (*module_add_fn)(int a, int b);
typedef int (*module_ctor_ran_fn)(void);

// The canonical exported names, so host and module never disagree on the strings.
#define MODULE_SYM_INIT "module_init"
#define MODULE_SYM_LOOP "module_loop"
#define MODULE_SYM_ADD "module_add"
#define MODULE_SYM_CTOR_RAN "module_ctor_ran"

#ifdef __cplusplus
}
#endif
