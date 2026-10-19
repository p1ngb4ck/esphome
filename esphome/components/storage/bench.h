#pragma once

// WARNING: This component is EXPERIMENTAL. The API may change at any time
// without following the normal breaking changes policy. Use at your own risk.

// A storage speed test on top of the storage interface and its worker, for a settings UI: in a folder of
// its own on the storage, a large file written and read back (verified), small files written, the folder
// listed, everything deleted again. Each step's state and figure in steps(), a change reported to one
// listener (main loop). The same Step list serves any multi-step test a UI shows (a relay's tests).
//
// RAM: the data goes through the transfer buffer (global_transfer_buffer, PSRAM; borrowed for the run,
// given back at its end) - no buffer of its own; without it the test does not start. The step list is a
// few small strings (internal, ~0.5 KB while a run is shown).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/hal.h"
#include "storage.h"
#ifdef USE_STORAGE_WORKER
#include "storage_worker.h"
#endif
#ifdef USE_STORAGE_TRANSFER_BUFFER
#include "transfer_buffer.h"
#endif

namespace esphome::storage::bench {

namespace st = esphome::storage;

enum StepState : uint8_t { STEP_PENDING = 0, STEP_RUNNING, STEP_OK, STEP_WARN, STEP_FAILED, STEP_SKIPPED };

struct Step {
  std::string name;
  uint8_t state{STEP_PENDING};
  std::string detail;
};

static constexpr size_t CHUNK = 1024 * 1024;  // per stream call, from the transfer buffer
static constexpr size_t BIG = 16;             // chunks of the large file (16 MB)
static constexpr size_t SMALL_FILES = 100;
static constexpr size_t SMALL_SIZE = 4096;
static constexpr const char *FOLDER = "/.p4-speedtest";

struct Run {
  std::vector<Step> steps;
  std::function<void()> on_change;
  bool busy{false};
  std::string root;  // the storage's mount path (VFS)
#ifdef USE_STORAGE_WORKER
  st::PathStorage *ps{nullptr};
  std::string dir;  // the test folder, driver-relative
  uint8_t *buf{nullptr};
  st::StreamHandle stream{};
  size_t index{0};
  size_t step_bytes{0};
  uint32_t t0{0};
  uint32_t sum{0};       // of what was written
  uint32_t sum_read{0};  // of what was read back
  size_t entries{0};
#endif
};

inline Run &run() {
  static Run r;
  return r;
}

inline void changed_() {
  if (run().on_change)
    run().on_change();
}

inline void set_(size_t i, uint8_t state, const std::string &detail = "") {
  Run &r = run();
  if (i < r.steps.size()) {
    r.steps[i].state = state;
    r.steps[i].detail = detail;
  }
  changed_();
}

inline std::string rate_(size_t bytes, uint32_t ms) {
  char text[32];
  snprintf(text, sizeof(text), "%.1f MB/s", ms == 0 ? 0.0 : bytes / 1048576.0 / (ms / 1000.0));
  return text;
}

#ifdef USE_STORAGE_WORKER
inline void finish_(bool failed) {
  Run &r = run();
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (r.buf != nullptr && st::global_transfer_buffer != nullptr)
    st::global_transfer_buffer->release();
#endif
  r.buf = nullptr;
  // Whatever happened: the test folder goes (guarded like the actions)
  if (r.ps != nullptr && !st::global_storage_worker->has_active_task_io(r.ps))
    st::remove_recursive(r.ps, r.dir.c_str());
  if (failed) {
    for (auto &s : r.steps) {
      if (s.state == STEP_PENDING)
        s.state = STEP_SKIPPED;
    }
  }
  r.busy = false;
  changed_();
}

inline void fill_(uint8_t *p, size_t n, uint32_t seed) {
  for (size_t i = 0; i + 4 <= n; i += 4) {
    const uint32_t v = seed ^ static_cast<uint32_t>(i * 2654435761u);
    memcpy(p + i, &v, 4);
  }
}

inline uint32_t sum_(const uint8_t *p, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++)
    s = (s << 5) + s + p[i];
  return s;
}

inline std::string big_path_() { return run().dir + "/big.bin"; }

inline void step_delete_();
inline void step_list_();
inline void step_small_();
inline void step_read_();

// 1: the large file, BIG chunks of CHUNK
inline void write_next_() {
  Run &r = run();
  if (r.index >= BIG) {
    st::global_storage_worker->end_write(r.stream, [](st::StorageError e) {
      Run &r = run();
      if (e != st::StorageError::STORAGE_ERROR_OK) {
        set_(0, STEP_FAILED, st::error_to_string(e));
        finish_(true);
        return;
      }
      set_(0, STEP_OK, rate_(BIG * CHUNK, millis() - r.t0));
      step_read_();
    });
    return;
  }
  fill_(r.buf, CHUNK, static_cast<uint32_t>(r.index));
  r.sum = r.sum * 31 + sum_(r.buf, CHUNK);
  const st::StorageError err = st::global_storage_worker->write_chunk(r.stream, r.buf, CHUNK, [](st::StorageError e) {
    Run &r = run();
    if (e != st::StorageError::STORAGE_ERROR_OK) {
      set_(0, STEP_FAILED, st::error_to_string(e));
      st::global_storage_worker->end_write(r.stream, [](st::StorageError) { finish_(true); });
      return;
    }
    r.index++;
    write_next_();
  });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    set_(0, STEP_FAILED, st::error_to_string(err));
    st::global_storage_worker->end_write(r.stream, [](st::StorageError) { finish_(true); });
  }
}

// 2: read back and compared
inline void read_next_() {
  Run &r = run();
  r.step_bytes = 0;
  const st::StorageError err = st::global_storage_worker->read_chunk(
      r.stream, r.buf, CHUNK, &r.step_bytes, [](st::StorageError e) {
        Run &r = run();
        if (e != st::StorageError::STORAGE_ERROR_OK || r.step_bytes == 0) {
          const bool ok = e == st::StorageError::STORAGE_ERROR_OK && r.index == BIG && r.sum_read == r.sum;
          const std::string why = e != st::StorageError::STORAGE_ERROR_OK ? std::string(st::error_to_string(e))
                                  : !ok ? std::string("the data read back differs") : std::string();
          const uint32_t ms = millis() - r.t0;
          st::global_storage_worker->end_read(r.stream, [ok, why, ms](st::StorageError) {
            if (!ok) {
              set_(1, STEP_FAILED, why);
              finish_(true);
              return;
            }
            set_(1, STEP_OK, rate_(BIG * CHUNK, ms) + ", verified");
            step_small_();
          });
          return;
        }
        // Whole chunks are compared by their sum (as written)
        if (r.step_bytes == CHUNK) {
          r.sum_read = r.sum_read * 31 + sum_(r.buf, CHUNK);
          r.index++;
        } else {
          r.index = BIG + 1;  // a short chunk: not as written
        }
        read_next_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    set_(1, STEP_FAILED, st::error_to_string(err));
    st::global_storage_worker->end_read(r.stream, [](st::StorageError) { finish_(true); });
  }
}

inline void step_read_() {
  Run &r = run();
  set_(1, STEP_RUNNING);
  r.index = 0;
  r.sum_read = 0;
  r.t0 = millis();
  const st::StorageError err =
      st::global_storage_worker->begin_read(r.ps, big_path_().c_str(), &r.stream, [](st::StorageError e) {
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          set_(1, STEP_FAILED, st::error_to_string(e));
          finish_(true);
          return;
        }
        read_next_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    set_(1, STEP_FAILED, st::error_to_string(err));
    finish_(true);
  }
}

// 3: small files, one stream each
inline void small_next_() {
  Run &r = run();
  if (r.index >= SMALL_FILES) {
    const uint32_t ms = millis() - r.t0;
    char text[48];
    snprintf(text, sizeof(text), "%.0f files/s", ms == 0 ? 0.0 : SMALL_FILES * 1000.0 / ms);
    set_(2, STEP_OK, text);
    step_list_();
    return;
  }
  char name[32];
  snprintf(name, sizeof(name), "/s%03u.bin", static_cast<unsigned>(r.index));
  const std::string path = r.dir + name;
  auto fail = [](st::StorageError e) {
    set_(2, STEP_FAILED, st::error_to_string(e));
    finish_(true);
  };
  const st::StorageError err =
      st::global_storage_worker->begin_write(r.ps, path.c_str(), &r.stream, [fail](st::StorageError e) {
        Run &r = run();
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          fail(e);
          return;
        }
        const st::StorageError w =
            st::global_storage_worker->write_chunk(r.stream, r.buf, SMALL_SIZE, [fail](st::StorageError e2) {
              Run &r = run();
              st::global_storage_worker->end_write(r.stream, [fail, e2](st::StorageError e3) {
                if (e2 != st::StorageError::STORAGE_ERROR_OK || e3 != st::StorageError::STORAGE_ERROR_OK) {
                  fail(e2 != st::StorageError::STORAGE_ERROR_OK ? e2 : e3);
                  return;
                }
                run().index++;
                small_next_();
              });
            });
        if (w != st::StorageError::STORAGE_ERROR_OK) {
          st::global_storage_worker->end_write(r.stream, [fail, w](st::StorageError) { fail(w); });
        }
      });
  if (err != st::StorageError::STORAGE_ERROR_OK)
    fail(err);
}

inline void step_small_() {
  Run &r = run();
  set_(2, STEP_RUNNING);
  r.index = 0;
  r.t0 = millis();
  fill_(r.buf, SMALL_SIZE, 7);
  small_next_();
}

// 4: the folder listed
inline void step_list_() {
  Run &r = run();
  set_(3, STEP_RUNNING);
  r.entries = 0;
  r.t0 = millis();
  const st::StorageError err = st::global_storage_worker->async_list_dir(
      r.ps, r.dir.c_str(),
      [](const st::FileStat *, void *ctx) {
        (*static_cast<size_t *>(ctx))++;
        return true;
      },
      &r.entries,
      [](st::StorageError e) {
        Run &r = run();
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          set_(3, STEP_FAILED, st::error_to_string(e));
          finish_(true);
          return;
        }
        char text[48];
        snprintf(text, sizeof(text), "%u entries in %u ms", static_cast<unsigned>(r.entries),
                 static_cast<unsigned>(millis() - r.t0));
        set_(3, r.entries == SMALL_FILES + 1 ? STEP_OK : STEP_WARN, text);
        step_delete_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    set_(3, STEP_FAILED, st::error_to_string(err));
    finish_(true);
  }
}

// 5: everything deleted (a blocking call on the main loop, guarded; deferred one loop pass)
inline void step_delete_() {
  set_(4, STEP_RUNNING);
  Run &r = run();
  if (st::global_storage_worker->has_active_task_io(r.ps)) {
    set_(4, STEP_FAILED, "the storage is busy");
    finish_(true);
    return;
  }
  const uint32_t t0 = millis();
  const st::StorageError err = st::remove_recursive(r.ps, r.dir.c_str());
  char text[48];
  snprintf(text, sizeof(text), "%u ms", static_cast<unsigned>(millis() - t0));
  set_(4, err == st::StorageError::STORAGE_ERROR_OK ? STEP_OK : STEP_FAILED,
       err == st::StorageError::STORAGE_ERROR_OK ? std::string(text) : std::string(st::error_to_string(err)));
  finish_(err != st::StorageError::STORAGE_ERROR_OK);
}
#endif

// Starts the test on the storage mounted at `root` (a VFS mount path). false (nothing started) when a run
// is going, the storage is unknown, busy or read only, or the transfer buffer is missing or busy.
inline bool start(const std::string &root, std::function<void()> on_change, std::string *why) {
  Run &r = run();
  auto refuse = [why](const char *text) {
    if (why != nullptr)
      *why = text;
    return false;
  };
#if defined(USE_STORAGE_WORKER) && defined(USE_STORAGE_TRANSFER_BUFFER)
  if (r.busy)
    return refuse("A test runs");
  if (st::global_storage_registry == nullptr || st::global_storage_worker == nullptr)
    return refuse("No storage worker");
  const char *rel = nullptr;
  st::PathStorage *ps = st::global_storage_registry->resolve_path(root.c_str(), &rel);
  st::StorageInfo info{};
  if (ps == nullptr || ps->get_info(&info) != st::StorageError::STORAGE_ERROR_OK || !info.is_mounted)
    return refuse("Not mounted");
  if (info.is_read_only)
    return refuse("Read only");
  if (st::global_storage_worker->is_busy_with(ps))
    return refuse("The storage is busy");
  if (st::global_transfer_buffer == nullptr)
    return refuse("No transfer buffer");
  uint8_t *buf = st::global_transfer_buffer->try_acquire(CHUNK);
  if (buf == nullptr)
    return refuse("The transfer buffer is in use");
  r.root = root;
  r.ps = ps;
  r.dir = FOLDER;
  r.buf = buf;
  r.sum = 0;
  r.on_change = std::move(on_change);
  r.steps = {{"Write 16 MB"}, {"Read 16 MB back"}, {"100 small files (4 KB)"}, {"List the folder"},
             {"Delete the test files"}};
  r.busy = true;
  st::remove_recursive(ps, r.dir.c_str());  // a folder left from a run cut off
  const st::StorageError mk = ps->mkdir(r.dir.c_str());
  if (mk != st::StorageError::STORAGE_ERROR_OK) {
    set_(0, STEP_FAILED, std::string("No test folder: ") + st::error_to_string(mk));
    finish_(true);
    return true;
  }
  set_(0, STEP_RUNNING);
  r.index = 0;
  r.t0 = millis();
  const st::StorageError err =
      st::global_storage_worker->begin_write(ps, big_path_().c_str(), &r.stream, [](st::StorageError e) {
        if (e != st::StorageError::STORAGE_ERROR_OK) {
          set_(0, STEP_FAILED, st::error_to_string(e));
          finish_(true);
          return;
        }
        write_next_();
      });
  if (err != st::StorageError::STORAGE_ERROR_OK) {
    set_(0, STEP_FAILED, st::error_to_string(err));
    finish_(true);
  }
  return true;
#else
  return refuse("No storage worker / transfer buffer");
#endif
}

inline const char *state_text(uint8_t state) {
  switch (state) {
    case STEP_RUNNING:
      return "running";
    case STEP_OK:
      return "ok";
    case STEP_WARN:
      return "check";
    case STEP_FAILED:
      return "failed";
    case STEP_SKIPPED:
      return "skipped";
    default:
      return "waiting";
  }
}

}  // namespace esphome::storage::bench
