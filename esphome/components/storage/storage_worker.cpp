#include "storage_worker.h"
#include "esphome/core/log.h"
#include "esphome/core/wake.h"

#ifdef USE_STORAGE_WORKER

namespace esphome::storage {

static const char *const TAG = "storage_worker";

StorageWorker *global_storage_worker = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void StorageWorker::setup() {
  // Pool and (on ESP32) task creation are deferred to the first submit_() call — see
  // ensure_started_() — so that a driver merely linking in the worker (because it's
  // path-based) but never actually issuing an async transfer pays no RAM/task cost at all.
  // Only the hotplug subscription happens here: it must be in place before any storage could
  // possibly be unregistered, and it is a single lambda capture, not an allocation of note.
  if (global_storage_registry != nullptr) {
    global_storage_registry->add_on_unregistered_callback([this](Storage *s) { this->on_storage_unregistered_(s); });
  }
}

void StorageWorker::ensure_started_() {
  if (this->started_)
    return;
  this->started_ = true;

  this->pool_.init(this->max_pending_);
  for (size_t i = 0; i < this->max_pending_; i++) {
    this->pool_.emplace_back();  // default-construct in place — TransferRequest isn't movable
  }
  ESP_LOGCONFIG(TAG, "Request pool size: %zu", this->max_pending_);

  this->stream_pool_.init(this->max_streams_);
  for (size_t i = 0; i < this->max_streams_; i++) {
    this->stream_pool_.emplace_back();
  }
  ESP_LOGCONFIG(TAG, "Stream pool size: %zu", this->max_streams_);

  // The define is derived purely from drivers' task_safe flags in codegen; the platform
  // condition lives here so a task-safe driver on a non-FreeRTOS target degrades to
  // loop-sliced instead of failing to compile.
#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
  // One shared queue sized for the worst case of both pools being simultaneously dispatched to
  // the task — QueueEntry tags which pool/index each entry refers to (see task_loop_() below).
  this->task_queue_ = xQueueCreate(this->max_pending_ + this->max_streams_, sizeof(QueueEntry));
  if (this->task_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create request queue — falling back to loop-sliced mode only");
    return;
  }

  // ESP-IDF's xTaskCreate takes the stack size in bytes (unlike vanilla FreeRTOS, which uses
  // words) — task_stack_size_ is passed through unconverted, same as other components' task
  // creation (see e.g. zigbee_esp32.cpp, usb_cdc_acm_esp32.cpp).
  BaseType_t ok = xTaskCreate(&StorageWorker::task_fn, "storage_worker", this->task_stack_size_, this,
                              this->task_priority_, &this->task_handle_);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create worker task — falling back to loop-sliced mode only");
    this->task_handle_ = nullptr;
    vQueueDelete(this->task_queue_);
    this->task_queue_ = nullptr;
    return;
  }
  this->task_running_ = true;
#endif
}

bool StorageWorker::is_task_safe_(const TransferRequest &req) const {
  (void) req;  // unused on platforms without a background task (see the #else branch below)
#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
  if (!this->task_running_)
    return false;
  uint8_t src_caps = req.src_storage->get_capabilities();
  uint8_t dst_caps = req.dst_storage->get_capabilities();
  return (src_caps & StorageCaps::STORAGE_CAP_IO_TASK_SAFE) != 0 &&
         (dst_caps & StorageCaps::STORAGE_CAP_IO_TASK_SAFE) != 0;
#else
  return false;
#endif
}

// A stateless network storage answers NOT_READY while it is (re)connecting — the very call
// that got the answer has just woken it (see NFSClient::wake_if_unmounted_()). Within this
// window the request simply tries again next pass instead of dying; after it, NOT_READY is
// the honest final answer (server truly gone).
static constexpr uint32_t NETWORK_READY_WINDOW_MS = 20000;

bool StorageWorker::wait_for_network_ready_(TransferRequest &req, StorageError err, const Storage *side) {
  if (err != StorageError::NOT_READY || side == nullptr || side->get_storage_type() != StorageType::NETWORK)
    return false;
  if (millis() - req.submitted_ms > NETWORK_READY_WINDOW_MS)
    return false;
  if (!req.waiting_logged) {
    req.waiting_logged = true;
    ESP_LOGD(TAG, "Network storage not ready yet — waiting for it to come up");
  }
  return true;  // stay RUNNING; the loop retries this request on the next pass
}

bool StorageWorker::overlaps_active_(const TransferRequest &candidate) const {
  for (const auto &req : this->pool_) {
    if (&req == &candidate)
      continue;
    RequestState state = req.state.load();
    if (state != RequestState::RUNNING && state != RequestState::CANCELLED)
      continue;
    if (req.src_storage == candidate.src_storage || req.src_storage == candidate.dst_storage ||
        req.dst_storage == candidate.src_storage || req.dst_storage == candidate.dst_storage)
      return true;
  }
  return false;
}

bool StorageWorker::is_busy_with(const storage::Storage *storage) const {
  if (storage == nullptr)
    return false;
  // PENDING counts too: a submitted-but-not-yet-started job already "owns" the device from
  // the caller's point of view, so unmounting out from under it must wait.
  for (const auto &req : this->pool_) {
    RequestState state = req.state.load();
    if (state == RequestState::FREE || state == RequestState::DONE)
      continue;
    if (req.src_storage == storage || req.dst_storage == storage)
      return true;
  }
  return false;
}

StorageError StorageWorker::submit_(RequestOp op, PathStorage *src, const char *src_path, PathStorage *dst,
                                    const char *dst_path, CompletionCallback &&on_done, TransferJob *job_out,
                                    bool overwrite) {
  this->ensure_started_();

  if (strlen(src_path) >= STORAGE_WORKER_MAX_PATH || strlen(dst_path) >= STORAGE_WORKER_MAX_PATH)
    return StorageError::INVALID_ARGS;

  // Find a free slot. Backpressure: a full pool returns NOT_READY immediately, callback not
  // invoked — this reuses the existing "device not ready to serve calls" error rather than
  // adding a new value to the frozen StorageError enum (see the PR notes for the rationale).
  TransferRequest *slot = nullptr;
  for (auto &req : this->pool_) {
    RequestState expected = RequestState::FREE;
    if (req.state.compare_exchange_strong(expected, RequestState::PENDING)) {
      slot = &req;
      break;
    }
  }
  if (slot == nullptr)
    return StorageError::NOT_READY;

  // Work is pending from here on: ask for the component phase, both for the loop-sliced
  // engine's chunks and for delivering the completion afterwards. Released again in loop()
  // once every slot is free.
  this->loop_requester_.start();

  slot->op = op;
  slot->src_storage = src;
  slot->dst_storage = dst;
  strncpy(slot->src_path, src_path, STORAGE_WORKER_MAX_PATH - 1);
  slot->src_path[STORAGE_WORKER_MAX_PATH - 1] = '\0';
  strncpy(slot->dst_path, dst_path, STORAGE_WORKER_MAX_PATH - 1);
  slot->dst_path[STORAGE_WORKER_MAX_PATH - 1] = '\0';
  slot->callback = std::move(on_done);
  slot->result = StorageError::OK;
  slot->offset = 0;
  slot->src_handle = nullptr;
  slot->dst_handle = nullptr;
  slot->handles_open = false;
  slot->chunk_buf.reset();
  slot->chunk_size = 0;
  slot->submitted_ms = millis();
  slot->waiting_logged = false;
  slot->overwrite = overwrite;
  slot->pre_phase_done = false;
  slot->src_is_fs = src->get_storage_type() == StorageType::FILESYSTEM;
  slot->dst_is_fs = dst->get_storage_type() == StorageType::FILESYSTEM;
  // Tree ops keep their roots aside: src_path/dst_path are reused for the file currently in
  // flight, so everything below this point works on a tree exactly as it does on one file.
  if (op == RequestOp::COPY_TREE || op == RequestOp::MOVE_TREE) {
    slot->tree = make_unique<TreeWalk>();
    if (slot->tree == nullptr) {
      slot->state = RequestState::FREE;
      return StorageError::NO_SPACE;  // same answer the chunk buffer gives when it cannot be had
    }
    strncpy(slot->tree->src_root, src_path, STORAGE_WORKER_MAX_PATH - 1);
    strncpy(slot->tree->dst_root, dst_path, STORAGE_WORKER_MAX_PATH - 1);
  } else {
    slot->tree.reset();
  }
  slot->bytes_done = 0;
  slot->bytes_total = 0;
  slot->file_done = 0;
  slot->file_total = 0;
  // Bump the generation on claim (skipping 0) so stale TransferJob handles from a previous
  // occupant of this slot stop resolving. Main loop only — submissions never race each other.
  if (++slot->generation == 0)
    slot->generation = 1;
  if (job_out != nullptr) {
    *job_out = (slot->generation << 8) | static_cast<uint32_t>(slot - this->pool_.begin());
  }

#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
  // Skip task dispatch if another active (RUNNING/CANCELLED) request already shares a storage
  // with this one — two engines must never call the same storage instance concurrently. A
  // task-safe request that loses this race simply degrades to the loop-sliced engine below
  // instead of being re-dispatched to the task once the conflict clears; simpler, and the
  // request still completes correctly, just via the other engine.
  if (this->is_task_safe_(*slot) && !this->overlaps_active_(*slot)) {
    QueueEntry entry{QueueEntryKind::TRANSFER, static_cast<size_t>(slot - this->pool_.begin())};
    slot->state = RequestState::RUNNING;
    if (xQueueSend(this->task_queue_, &entry, 0) == pdTRUE) {
      return StorageError::OK;
    }
    // Queue send failed despite a free slot (shouldn't normally happen since the queue and
    // pool are sized identically) — fall through to loop-sliced handling instead of losing
    // the request.
    slot->state = RequestState::PENDING;
  }
#endif

  // Loop-sliced mode: leave the request PENDING; loop() picks up requests FIFO.
  return StorageError::OK;
}

StorageError StorageWorker::async_copy(PathStorage *src, const char *src_path, PathStorage *dst, const char *dst_path,
                                       CompletionCallback &&on_done, TransferJob *job_out, bool overwrite) {
  return this->submit_(RequestOp::COPY, src, src_path, dst, dst_path, std::move(on_done), job_out, overwrite);
}

StorageError StorageWorker::async_move(PathStorage *src, const char *src_path, PathStorage *dst, const char *dst_path,
                                       CompletionCallback &&on_done, TransferJob *job_out, bool overwrite) {
  return this->submit_(RequestOp::MOVE, src, src_path, dst, dst_path, std::move(on_done), job_out, overwrite);
}

StorageError StorageWorker::async_copy_tree(PathStorage *src, const char *src_path, PathStorage *dst,
                                            const char *dst_path, CompletionCallback &&on_done, TransferJob *job_out) {
  return this->submit_(RequestOp::COPY_TREE, src, src_path, dst, dst_path, std::move(on_done), job_out);
}

StorageError StorageWorker::async_move_tree(PathStorage *src, const char *src_path, PathStorage *dst,
                                            const char *dst_path, CompletionCallback &&on_done, TransferJob *job_out) {
  return this->submit_(RequestOp::MOVE_TREE, src, src_path, dst, dst_path, std::move(on_done), job_out);
}

bool StorageWorker::get_transfer_status(TransferJob job, TransferStatus *out) const {
  if (job == INVALID_TRANSFER_JOB || out == nullptr)
    return false;
  size_t slot_index = job & 0xFF;
  uint32_t generation = job >> 8;
  // Lazy init: before the first submission the pool is empty and no job can exist.
  if (slot_index >= this->pool_.size())
    return false;
  const TransferRequest &req = this->pool_[slot_index];
  // A recycled slot bumped its generation on claim; a freed slot is FREE. Either way the
  // handle is expired — the final DONE snapshot is only observable until loop() releases the
  // slot after the completion callback (see the header comment on capturing final results).
  if (req.generation != generation)
    return false;
  RequestState state = req.state.load();
  if (state == RequestState::FREE)
    return false;
  out->state = state;
  out->result = req.result;
  out->bytes_done = req.bytes_done.load();
  out->bytes_total = req.bytes_total.load();
  out->file_done = req.file_done.load();
  out->file_total = req.file_total.load();
  // Label of the file in flight: for a tree src_path holds exactly that (tree_step_ reuses
  // the single-file fields); basename only and truncated — this feeds a status line. The
  // worker task may be rewriting src_path between files while we copy; the counters above
  // are atomic, the name is not — a torn read yields at worst one garbled label for one
  // poll frame, never out-of-bounds (copy is bounded, termination forced below).
  out->file[0] = '\0';
  if (state == RequestState::RUNNING && req.file_total.load() != 0) {
    const char *slash = strrchr(req.src_path, '/');
    strncpy(out->file, slash != nullptr ? slash + 1 : req.src_path, sizeof(out->file) - 1);
    out->file[sizeof(out->file) - 1] = '\0';
  }
  return true;
}

void StorageWorker::on_storage_unregistered_(Storage *s) {
  // Synchronous cancel-and-drain: by the time this returns, no data-plane call into `s` is
  // still in flight, so the caller (unregister_storage(), on the main loop) can safely proceed
  // to unmount/tear down the driver right afterward.
  for (size_t i = 0; i < this->pool_.size(); i++) {
    TransferRequest &req = this->pool_[i];
    if (req.src_storage != s && req.dst_storage != s)
      continue;

    RequestState state = req.state.load();
    if (state == RequestState::PENDING) {
      // Not started yet — finish it immediately, no partial I/O to unwind.
      req.result = StorageError::NOT_READY;
      req.state = RequestState::DONE;
    } else if (state == RequestState::RUNNING) {
      if (i == this->loop_active_index_) {
        // Owned by the loop-sliced engine, which only ever advances from inside loop() — we
        // are on the main loop right now too, so nothing else can touch this request
        // concurrently. Cancel and drain it in place: run_chunk_()'s entry check sees
        // CANCELLED and calls finish_request() immediately, closing any open handles before
        // this function returns — i.e. before the driver's unmount() runs.
        req.state = RequestState::CANCELLED;
        this->run_chunk_(req);
        if (this->loop_active_index_ == i)
          this->loop_active_index_ = SIZE_MAX;
      } else {
#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
        // Owned by the worker task (or, if it hasn't dequeued this index off task_queue_ yet,
        // about to be — run_chunk_()'s entry check still sees CANCELLED as soon as the task
        // does pick it up, so no special-casing is needed for that race). Set CANCELLED and
        // wait for the task to actually reach DONE — it can only observe the flag between
        // chunks, so this is bounded by at most one chunk's read+write (low single-digit ms
        // for SDMMC). Yield with vTaskDelay(1) rather than busy-spinning so the task (which
        // may run at the same or a lower priority) actually gets CPU time to reach that
        // check — a tight spin here on a single-core target would starve it and deadlock
        // until the timeout below.
        req.state = RequestState::CANCELLED;
        uint32_t start = millis();
        while (req.state.load() != RequestState::DONE) {
          if (millis() - start > 500) {
            ESP_LOGE(TAG, "Timed out waiting for in-flight transfer to cancel — the storage medium "
                          "is likely gone; proceeding with unmount anyway. Behavior from here is "
                          "undefined if the task is still touching it (e.g. still holding a file "
                          "handle) when it does eventually finish.");
            // Discard the callback now: if the task does reach DONE later, loop() must only
            // free the slot, not fire a completion into a context whose storage object may
            // no longer exist post-unmount. The slot stays stuck (RUNNING/CANCELLED, never
            // recycled) if the task never finishes — acceptable since the medium is dead
            // anyway at that point.
            req.callback = nullptr;
            break;
          }
          // pdMS_TO_TICKS(1) rounds down to 0 ticks at the default 100 Hz tick rate (10 ms per
          // tick), which would make this a pure yield with no actual delay. Passing 1 directly
          // requests one full tick (10 ms at the default rate) instead.
          vTaskDelay(1);
        }
#endif
      }
    }
  }

  // Same drain contract for streams: by the time this returns, no data-plane call into `s` is
  // still in flight and any handle it held is closed.
  for (size_t i = 0; i < this->stream_pool_.size(); i++) {
    StreamRequest &req = this->stream_pool_[i];
    if (req.storage != s)
      continue;
    StreamState state = req.state.load();
    if (state == StreamState::FREE || state == StreamState::DONE)
      continue;
    if (state == StreamState::IDLE) {
      // No I/O in flight and no pending step queued — finish immediately, no drain needed.
      if (req.is_fs && req.handle != nullptr)
        static_cast<FilesystemStorage *>(req.storage)->close(req.handle);
      req.result = StorageError::NOT_READY;
      req.state = StreamState::DONE;
      continue;
    }
    // A step (OPENING/WRITING/READING/CLOSING) is in flight or queued. Mark CANCELLED;
    // run_stream_step_()'s entry check sees it and closes/finishes immediately. If it's
    // already running on the worker task, wait (bounded) for it to reach DONE — same
    // rationale/timeout as the TransferRequest drain above.
    req.state = StreamState::CANCELLED;
#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
    uint32_t start = millis();
    while (req.state.load() != StreamState::DONE) {
      if (millis() - start > 500) {
        ESP_LOGE(TAG, "Timed out waiting for in-flight stream to cancel — the storage medium is "
                      "likely gone; proceeding with unmount anyway.");
        req.callback = nullptr;
        break;
      }
      vTaskDelay(1);
    }
#else
    // Loop-sliced only: run the (now-cancelled) step directly here rather than waiting for
    // the next loop() pass — we're on the main loop right now too (this callback only ever
    // fires from unregister_storage(), main-loop-only per StorageRegistry's contract), so
    // nothing else can be touching this slot concurrently. run_stream_step_()'s entry check
    // sees CANCELLED and closes the handle immediately, before this function returns.
    req.pending_step_ = false;
    this->run_stream_step_(req);
#endif
  }
}

void StorageWorker::loop() {
  // Nothing to do until the first async transfer is submitted. Cheap early-out every main
  // loop pass — far simpler and race-free compared to disable_loop()/enable_loop() gymnastics
  // around lazy start (an enable_loop() issued while the component is still in LOOP state is a
  // no-op, so a pre-start disable could never be undone — that was the stall).
  if (!this->started_)
    return;

  // Deliver completions and free slots. Runs regardless of which engine finished the
  // request, so this is the single place user callbacks are invoked — always on the main
  // loop, per the public API's contract.
  for (auto &req : this->pool_) {
    if (req.state.load() == RequestState::DONE) {
#ifdef USE_STORAGE_CHANGE_FEED
      // Every transfer completes here, whoever submitted it — YAML automations included —
      // so this is where the change feed learns about it, while the request's fields are
      // still intact. Whatever the result: a partially landed tree is still a change.
      // (Streaming requests are not fed: reads change nothing, and the write stream's
      // consumers note their own completion — they know the path, this loop does not.)
      if (global_storage_registry != nullptr && req.dst_storage != nullptr && req.src_storage != nullptr) {
        const bool is_tree = req.tree != nullptr;
        const char *dst_rel = is_tree ? req.tree->dst_root : req.dst_path;
        std::string dst = std::string(req.dst_storage->get_mount_path()) + "/" + dst_rel;
        global_storage_registry->note_parent_changed(dst);
        if (is_tree) {
          // Merging into an existing (possibly open) directory changes its listing too.
          global_storage_registry->note_dir_changed(dst);
        }
        if (req.op == RequestOp::MOVE || req.op == RequestOp::MOVE_TREE) {
          const char *src_rel = is_tree ? req.tree->src_root : req.src_path;
          global_storage_registry->note_parent_changed(std::string(req.src_storage->get_mount_path()) + "/" + src_rel);
        }
      }
#endif
      CompletionCallback cb = std::move(req.callback);
      StorageError result = req.result;
      req.callback = nullptr;
      req.src_storage = nullptr;
      req.dst_storage = nullptr;
      req.stuck_warned = false;
      req.state = RequestState::FREE;
      if (cb)
        cb(result);
    }
  }

  if (this->loop_active_index_ == SIZE_MAX) {
    // Pick the next PENDING request, FIFO by pool position (pool order matches submission
    // order well enough in practice — this component makes no stronger ordering promise).
    // Skip any request that overlaps a storage with something already RUNNING/CANCELLED (e.g.
    // on the worker task) — it must wait its turn rather than run concurrently with it.
    for (size_t i = 0; i < this->pool_.size(); i++) {
      TransferRequest &candidate = this->pool_[i];
      if (candidate.state.load() != RequestState::PENDING)
        continue;
      if (this->overlaps_active_(candidate)) {
        // Known limitation: if the request it overlaps with is stuck (its owning engine never
        // reached DONE after a drain timeout — see on_storage_unregistered_()), this request
        // waits forever and its callback never fires. There is no reclaim path for the stuck
        // slot itself: by the time a drain times out, the medium is presumed gone and the
        // engine that owns it may still be touching it, so freeing the slot out from under
        // that engine would be unsafe. Log once per request so this is at least diagnosable.
        if (!candidate.stuck_warned) {
          ESP_LOGW(TAG, "Request pending indefinitely — blocked by another request on the same "
                        "storage that never completed (likely a stuck slot after a drain "
                        "timeout)");
          candidate.stuck_warned = true;
        }
        continue;
      }
      candidate.state = RequestState::RUNNING;
      this->loop_active_index_ = i;
      break;
    }
  }

  if (this->loop_active_index_ != SIZE_MAX) {
    TransferRequest &req = this->pool_[this->loop_active_index_];
    this->run_chunk_(req);
    if (req.state.load() == RequestState::DONE)
      this->loop_active_index_ = SIZE_MAX;
  }

  // Idle again? Then stop asking for the phase — the next submit_() asks for it anew.
  bool busy = this->loop_active_index_ != SIZE_MAX;
  if (!busy) {
    for (const auto &req : this->pool_) {
      if (req.state.load() != RequestState::FREE) {
        busy = true;
        break;
      }
    }
  }
  if (!busy) {
    for (const auto &req : this->stream_pool_) {
      if (req.state.load() != StreamState::FREE) {
        busy = true;
        break;
      }
    }
  }
  if (!busy)
    this->loop_requester_.stop();

  // Streams: two jobs, both per-slot and independent of any other slot (no FIFO ordering
  // needed — unlike TransferRequest's loop_active_index_, every stream's step is
  // self-contained, since nothing auto-advances without the caller supplying the next chunk).
  //  1. Run any step a loop-sliced dispatch queued but hasn't executed yet (see
  //     dispatch_stream_step_()'s loop-sliced branch — it only flips req.pending_step_ rather
  //     than calling run_stream_step_() inline, so the callback always fires from here, never
  //     reentrantly from inside the caller's own begin_*/write_chunk/read_chunk/end_* call).
  //  2. Deliver completions for whichever streams finished a step (task or loop-sliced).
  for (auto &req : this->stream_pool_) {
    if (req.pending_step_) {
      req.pending_step_ = false;
      this->run_stream_step_(req);
    }

    StreamState state = req.state.load();
    bool step_done = (state == StreamState::IDLE && req.callback) || state == StreamState::DONE;
    if (!step_done)
      continue;

    CompletionCallback cb = std::move(req.callback);
    StorageError result = req.result;
    req.callback = nullptr;
    bool freed = state == StreamState::DONE;
    if (freed) {
      req.storage = nullptr;
      req.state = StreamState::FREE;
    }
    if (cb)
      cb(result);
  }
}

#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
void StorageWorker::task_fn(void *arg) { static_cast<StorageWorker *>(arg)->task_loop_(); }

void StorageWorker::task_loop_() {
  // No ESPHome facilities are touched from this task besides is_registered() (mutex-guarded)
  // and ESP_LOG (task-safe on IDF, kept to errors) — see run_chunk_()'s cancellation check.
  // Deliberately NOT subscribed to the task watchdog: this task blocks on I/O by design, and
  // the chunked design here (like the synchronous copy() helper) already keeps any single
  // blocking call short.
  for (;;) {
    QueueEntry entry;
    if (xQueueReceive(this->task_queue_, &entry, portMAX_DELAY) != pdTRUE)
      continue;

    if (entry.kind == QueueEntryKind::TRANSFER) {
      TransferRequest &req = this->pool_[entry.index];
      // Loop until DONE, not just while RUNNING: if the hotplug handler flips the state to
      // CANCELLED between iterations, run_chunk_()'s entry check is what actually closes
      // handles, frees the chunk buffer, and transitions to DONE. Stopping on state != RUNNING
      // would exit before that ever happens, leaking the handles/buffer and leaving the slot
      // (and the pending callback) stuck forever.
      while (req.state.load() != RequestState::DONE) {
        this->run_chunk_(req);
      }
    } else {
      // STREAM: single step per queue entry, unlike TransferRequest above — the caller submits
      // exactly one queue entry per write_chunk()/read_chunk()/begin_*()/end_*() call, so there
      // is no "loop until DONE" here; DONE only happens on end_write()/end_read()'s step.
      this->run_stream_step_(this->stream_pool_[entry.index]);
    }
    // DONE (or IDLE, for streams) reached, and this task is the producer: loop() delivers the
    // completion, but the component phase it runs in is gated (loop_interval_, high-frequency
    // request, or an explicit wake). Nothing else asks for it — an HTTP request does not drive
    // it, it only reads. So wake it here, which is exactly what wake_loop_threadsafe() is for:
    // a background producer queued work for its component's loop() to drain.
    //
    // Without this the completion sits in the slot until the phase happens to come around.
    // A caller that polls for a result eventually sees it; a caller that *chains* on it — the
    // file browser's directory walker submits file N+1 only once file N's completion has been
    // delivered — stops dead after the first file.
    wake_loop_threadsafe();
  }
}
#endif

namespace {

// Picks entry #target out of a directory listing; the walk re-lists per step rather than
// holding an open directory handle across calls, so nothing has to stay valid in between.
struct WalkEntryCtx {
  uint16_t target;
  uint16_t seen{0};
  bool found{false};
  FileStat entry{};
};

bool walk_entry_cb(const FileStat *entry, void *ctx_raw) {
  auto *ctx = static_cast<WalkEntryCtx *>(ctx_raw);
  if (ctx->seen == ctx->target) {
    ctx->entry = *entry;
    ctx->found = true;
    return false;  // stop enumeration — not an error per the list_dir contract
  }
  ctx->seen++;
  return true;
}

// Bounded "<root>[/<sub>][/<name>]" join; false on truncation.
bool join_walk_path(char *out, size_t out_size, const char *root, const char *sub, const char *name) {
  int n;
  if (name != nullptr) {
    n = (sub[0] != '\0') ? snprintf(out, out_size, "%s/%s/%s", root, sub, name)
                         : snprintf(out, out_size, "%s/%s", root, name);
  } else {
    n = (sub[0] != '\0') ? snprintf(out, out_size, "%s/%s", root, sub) : snprintf(out, out_size, "%s", root);
  }
  return n > 0 && static_cast<size_t>(n) < out_size;
}

// Finishes a request: closes any open handles (best-effort — a close failure only overrides
// the result if the transfer itself had otherwise succeeded, mirroring storage::copy()'s
// close-error propagation) and marks it DONE for loop()/task_loop_() to deliver.
// Closes whatever handles the request holds. A close failure only overrides an otherwise
// successful result, mirroring storage::copy()'s close-error propagation.
void close_handles(TransferRequest &req, StorageError *result) {
  if (!req.handles_open)
    return;
  if (req.src_is_fs && req.src_handle != nullptr)
    static_cast<FilesystemStorage *>(req.src_storage)->close(req.src_handle);
  if (req.dst_is_fs && req.dst_handle != nullptr) {
    StorageError close_err = static_cast<FilesystemStorage *>(req.dst_storage)->close(req.dst_handle);
    if (*result == StorageError::OK)
      *result = close_err;
  }
  req.src_handle = nullptr;
  req.dst_handle = nullptr;
}

void finish_request(TransferRequest &req, StorageError result) {
  close_handles(req, &result);
  req.chunk_buf.reset();
  req.result = result;
  req.state = RequestState::DONE;
}

}  // namespace

// One step of a directory walk. Everything here is control plane — list, mkdir, rmdir, remove —
// so it costs one call each and never blocks for long; the bytes go through run_chunk_()'s
// chunk loop exactly as they do for a single file. Returns false once the request is finished.
bool StorageWorker::tree_step_(TransferRequest &req) {
  TreeWalk &w = *req.tree;
  const bool is_move = req.op == RequestOp::MOVE_TREE;

  if (!w.root_created) {
    StorageError err = req.dst_storage->mkdir(w.dst_root);
    if (err != StorageError::OK && err != StorageError::ALREADY_EXISTS) {
      if (this->wait_for_network_ready_(req, err, req.dst_storage))
        return false;  // not finished — retried next pass, the knock has woken the mount
      finish_request(req, err);
      return false;
    }
    w.root_created = true;
  }

  while (true) {
    char dir[STORAGE_WORKER_MAX_PATH];
    if (!join_walk_path(dir, sizeof(dir), w.src_root, w.sub, nullptr)) {
      finish_request(req, StorageError::INVALID_ARGS);
      return false;
    }

    WalkEntryCtx ctx{w.index_stack[w.depth]};
    StorageError err = req.src_storage->list_dir(dir, walk_entry_cb, &ctx);
    if (err == StorageError::NOT_READY && this->wait_for_network_ready_(req, err, req.src_storage))
      return false;
    if (err != StorageError::OK) {
      finish_request(req, err);
      return false;
    }

    if (!ctx.found) {
      // Drained. A move takes the emptied source directory with it.
      if (is_move) {
        err = req.src_storage->rmdir(dir);
        if (err != StorageError::OK) {
          finish_request(req, err);
          return false;
        }
      }
      if (w.depth == 0) {
        finish_request(req, StorageError::OK);
        return false;
      }
      char *slash = strrchr(w.sub, '/');
      if (slash != nullptr) {
        *slash = '\0';
      } else {
        w.sub[0] = '\0';
      }
      w.depth--;
      continue;
    }

    // Entry bookkeeping differs by mode: a copy leaves the source alone, so positions are
    // stable and the index counts up. A move removes each entry once it is done, so what is
    // left slides down and the next unprocessed entry is always #0 — advancing would skip one.
    if (!is_move)
      w.index_stack[w.depth]++;

    if (ctx.entry.is_dir) {
      if (w.depth + 1 >= TreeWalk::MAX_DEPTH) {
        finish_request(req, StorageError::NOT_SUPPORTED);
        return false;
      }
      char dst_dir[STORAGE_WORKER_MAX_PATH];
      if (!join_walk_path(dst_dir, sizeof(dst_dir), w.dst_root, w.sub, ctx.entry.name)) {
        finish_request(req, StorageError::INVALID_ARGS);
        return false;
      }
      err = req.dst_storage->mkdir(dst_dir);
      if (err != StorageError::OK && err != StorageError::ALREADY_EXISTS) {
        finish_request(req, err);
        return false;
      }
      size_t sub_len = strlen(w.sub);
      size_t name_len = strlen(ctx.entry.name);
      if (sub_len + name_len + 2 >= sizeof(w.sub)) {
        finish_request(req, StorageError::INVALID_ARGS);
        return false;
      }
      if (sub_len != 0)
        w.sub[sub_len++] = '/';
      memcpy(w.sub + sub_len, ctx.entry.name, name_len + 1);
      w.depth++;
      w.index_stack[w.depth] = 0;
      continue;
    }

    // A file: hand it to the chunk loop below by putting it where a single-file request keeps
    // its paths. Nothing else in run_chunk_() has to know it is part of a tree.
    if (!join_walk_path(req.src_path, sizeof(req.src_path), w.src_root, w.sub, ctx.entry.name) ||
        !join_walk_path(req.dst_path, sizeof(req.dst_path), w.dst_root, w.sub, ctx.entry.name)) {
      finish_request(req, StorageError::INVALID_ARGS);
      return false;
    }
    req.offset = 0;
    // Deliberately no bytes_total: it belongs to the request, and bytes_done counts the whole
    // tree, so a per-file total would have the two describe different things — progress ran
    // past 100% for every file after the first. What a tree will weigh in total is unknown
    // without walking it twice, and 0 already means exactly that (see TransferRequest).
    w.file_in_flight = true;
    return true;
  }
}

void StorageWorker::run_chunk_(TransferRequest &req) {
  // Cancellation / hotplug check, before doing any I/O this call.
  if (req.state.load() == RequestState::CANCELLED) {
    finish_request(req, StorageError::NOT_READY);
    return;
  }
  if (global_storage_registry != nullptr && (!global_storage_registry->is_registered(req.src_storage) ||
                                             !global_storage_registry->is_registered(req.dst_storage))) {
    finish_request(req, StorageError::NOT_READY);
    return;
  }

  // The relocated handler pre-phase, executed once inside the engine that owns the storages
  // (the file API is a pure translator and performs no driver I/O of its own): classify the
  // source, honor the overwrite contract, clear an occupied destination, and decide
  // tree-vs-file HERE by materializing the walk for a directory source.
  if (!req.pre_phase_done) {
    req.pre_phase_done = true;
    FileStat src_st{};
    StorageError serr = req.src_storage->stat(req.src_path, &src_st);
    if (this->wait_for_network_ready_(req, serr, req.src_storage)) {
      req.pre_phase_done = false;  // not decided yet — redo the whole phase once ready
      return;
    }
    if (serr != StorageError::OK) {
      finish_request(req, StorageError::NOT_FOUND);
      return;
    }
    const bool src_is_dir = src_st.is_dir;
    const bool is_move_op = req.op == RequestOp::MOVE || req.op == RequestOp::MOVE_TREE;
    const bool same_storage_move = is_move_op && req.src_storage == req.dst_storage;
    // Explicit tree submissions (async_copy_tree()/async_move_tree(), e.g. automations) keep
    // their historical contract: an existing destination root is merged into, entries below
    // it overwritten by the walk itself. Only the self-classifying API enforces overwrite.
    const bool explicit_tree = req.tree != nullptr;
    FileStat dst_st{};
    StorageError derr = req.dst_storage->stat(req.dst_path, &dst_st);
    if (this->wait_for_network_ready_(req, derr, req.dst_storage)) {
      req.pre_phase_done = false;
      return;
    }
    if (derr == StorageError::OK && !explicit_tree) {
      if (!req.overwrite) {
        finish_request(req, StorageError::ALREADY_EXISTS);
        return;
      }
      if (dst_st.is_dir != src_is_dir) {
        // Never trade a tree for a file or the other way round, no matter what was asked for.
        finish_request(req, StorageError::INVALID_ARGS);
        return;
      }
      // Clear the destination where the operation cannot replace it by itself — same
      // reasoning the handler used to apply, now in the right context: a same-storage move
      // needs a free name for rename(); a directory copy was asked to REPLACE, and merging
      // would leave the old tree's files behind. A plain file write truncates by itself.
      if (same_storage_move || src_is_dir) {
        StorageError cerr =
            src_is_dir ? remove_recursive(req.dst_storage, req.dst_path) : req.dst_storage->remove(req.dst_path);
        if (cerr != StorageError::OK) {
          finish_request(req, cerr);
          return;
        }
      }
    }
    if (src_is_dir && !same_storage_move && req.tree == nullptr) {
      // A directory source that rename() cannot take in one step: become a tree job.
      req.tree = make_unique<TreeWalk>();
      if (req.tree == nullptr) {
        finish_request(req, StorageError::NO_SPACE);
        return;
      }
      strncpy(req.tree->src_root, req.src_path, STORAGE_WORKER_MAX_PATH - 1);
      strncpy(req.tree->dst_root, req.dst_path, STORAGE_WORKER_MAX_PATH - 1);
      req.op = req.op == RequestOp::MOVE ? RequestOp::MOVE_TREE : RequestOp::COPY_TREE;
    }
  }

  // A tree between files: take the next walk step. It either sets up the next file (and falls
  // through to the chunk loop below) or finishes the request — no caller involved either way.
  if (req.tree != nullptr && !req.tree->file_in_flight) {
    if (!this->tree_step_(req))
      return;
  }

  // First call for this request: do the cheap same-storage rename() fast path for MOVE, or
  // open handles / size-check for everything else that needs the chunk loop.
  if (!req.handles_open) {
    if (req.op == RequestOp::MOVE && req.src_storage == req.dst_storage) {
      StorageError err = req.src_storage->rename(req.src_path, req.dst_path);
      // A refusal (an NFS export spanning file systems answers NOT_SUPPORTED) is the one error
      // worth redoing the long way: fall through to the chunk loop below, which copies and then
      // removes the source. Directories are not ours to salvage — the loop moves file bytes, so
      // the caller's per-file walker has to take that one.
      if (this->wait_for_network_ready_(req, err, req.src_storage))
        return;
      bool salvageable = err == StorageError::NOT_SUPPORTED && global_storage_registry != nullptr &&
                         global_storage_registry->get_move_fallback_copy();
      if (salvageable) {
        FileStat st{};
        salvageable = req.src_storage->stat(req.src_path, &st) == StorageError::OK && !st.is_dir;
      }
      if (!salvageable) {
        finish_request(req, err);
        return;
      }
      ESP_LOGD(TAG, "rename refused for '%s' — moving it as copy + remove instead", req.src_path);
    }

    // Progress total for get_transfer_status(): one cheap stat on the source. A failure here
    // is not fatal — bytes_total stays 0, which consumers must treat as "unknown/indeterminate"
    // (the transfer itself still detects a truly missing source at open()/read time).
    {
      FileStat src_stat{};
      if (req.src_storage->stat(req.src_path, &src_stat) == StorageError::OK && !src_stat.is_dir) {
        // bytes_total belongs to the request as a whole and stays 0 for a tree (unknown
        // without walking it twice); the file in flight is one cheap stat either way.
        if (req.tree == nullptr)
          req.bytes_total.store(src_stat.size);
        req.file_total.store(src_stat.size);
      }
    }

    size_t chunk_size = STORAGE_COPY_CHUNK_SIZE;
    uint8_t *raw = nullptr;
    while (chunk_size >= 4096) {
      raw = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::PREFER_INTERNAL).allocate(chunk_size);
      if (raw != nullptr)
        break;
      chunk_size /= 2;
    }
    if (raw == nullptr) {
      finish_request(req, StorageError::NO_SPACE);
      return;
    }
    req.chunk_buf = RamBuffer(raw, RamBufferDeleter{chunk_size});
    req.chunk_size = chunk_size;

    if (req.src_is_fs) {
      StorageError err =
          static_cast<FilesystemStorage *>(req.src_storage)->open(req.src_path, req.src_handle, OpenMode::READ);
      if (err != StorageError::OK) {
        finish_request(req, err);
        return;
      }
    }
    if (req.dst_is_fs) {
      StorageError err =
          static_cast<FilesystemStorage *>(req.dst_storage)->open(req.dst_path, req.dst_handle, OpenMode::WRITE);
      if (err != StorageError::OK) {
        req.handles_open = true;  // src handle (if any) is open — let finish_request() close it
        finish_request(req, err);
        return;
      }
    }
    req.handles_open = true;
  }

  // One chunk: read, then write it out fully (write_chunk/write can themselves return partial
  // writes — loop here same as the synchronous copy() helper does).
  size_t bytes_read = 0;
  StorageError err;
  if (req.src_is_fs) {
    err = static_cast<FilesystemStorage *>(req.src_storage)
              ->read(req.src_handle, req.chunk_buf.get(), req.chunk_size, &bytes_read);
  } else {
    err = static_cast<NetworkStorage *>(req.src_storage)
              ->read_chunk(req.src_path, req.chunk_buf.get(), req.offset, req.chunk_size, &bytes_read);
  }
  if (err != StorageError::OK) {
    if (this->wait_for_network_ready_(req, err, req.src_storage))
      return;
    finish_request(req, err);
    return;
  }
  if (bytes_read == 0) {
    // EOF of the file in flight.
    if (req.tree != nullptr) {
      // Close it before touching the entry itself, then let the walk pick the next one on the
      // next call. The chunk buffer stays — the next file reuses it.
      close_handles(req, &err);
      req.handles_open = false;
      if (req.op == RequestOp::MOVE_TREE && err == StorageError::OK)
        err = req.src_storage->remove(req.src_path);
      if (err != StorageError::OK) {
        finish_request(req, err);
        return;
      }
      req.tree->bytes_base += req.offset;
      req.tree->files_done++;
      req.tree->file_in_flight = false;
      req.offset = 0;
      req.file_done.store(0);
      req.file_total.store(0);
      return;
    }
    if (req.op == RequestOp::MOVE) {
      // Cross-storage move: the copy succeeded, now remove the source. Per move()'s
      // documented semantics, if this remove fails the destination copy is kept rather than
      // rolled back — the caller ends up with the file in both places, not neither.
      err = req.src_storage->remove(req.src_path);
    }
    finish_request(req, err);
    return;
  }

  size_t total_written = 0;
  while (total_written < bytes_read) {
    size_t bytes_written = 0;
    if (req.dst_is_fs) {
      err =
          static_cast<FilesystemStorage *>(req.dst_storage)
              ->write(req.dst_handle, req.chunk_buf.get() + total_written, bytes_read - total_written, &bytes_written);
    } else {
      err = static_cast<NetworkStorage *>(req.dst_storage)
                ->write_chunk(req.dst_path, req.chunk_buf.get() + total_written, req.offset + total_written,
                              bytes_read - total_written, &bytes_written);
    }
    if (err != StorageError::OK) {
      if (this->wait_for_network_ready_(req, err, req.dst_storage))
        return;
      finish_request(req, err);
      return;
    }
    if (bytes_written == 0) {
      finish_request(req, StorageError::WRITE_ERROR);
      return;
    }
    total_written += bytes_written;
  }

  req.offset += bytes_read;
  // Progress for get_transfer_status(): offset equals bytes fully read AND written at this
  // point (the write loop above completed), so it doubles as bytes_done. Atomic store because
  // the main loop may snapshot progress while the worker task runs this transfer.
  req.bytes_done.store(req.tree != nullptr ? req.tree->bytes_base + req.offset : req.offset);
  req.file_done.store(req.offset);
  // Request stays RUNNING; the next call (next loop() iteration, or the task's own loop)
  // picks up at the new offset. No watchdog feed here by design — see task_loop_()'s comment
  // for the task path; the loop-sliced path returns to the main loop's own feed_wdt() between
  // iterations same as any other component's loop().
}

// ===========================================================================================
// Streaming (begin_write/write_chunk/end_write, begin_read/read_chunk/end_read) — shares this
// StorageWorker's pool machinery pattern (FixedVector, compare_exchange slot claim, same task/
// queue) but is its own independent state machine; see StreamRequest's comment in the header
// for why it isn't folded into TransferRequest/run_chunk_().

bool StorageWorker::is_task_safe_(const StreamRequest &req) const {
  (void) req;
#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
  if (!this->task_running_)
    return false;
  return (req.storage->get_capabilities() & StorageCaps::STORAGE_CAP_IO_TASK_SAFE) != 0;
#else
  return false;
#endif
}

namespace {

// Advances one stream by exactly the operation implied by its current state — open, one
// write chunk, one read chunk, or close — then transitions to IDLE (more chunks expected,
// open()/close() are one-shot so those always move straight past IDLE) or DONE. Unlike
// run_chunk_(), this never decides what to do next on its own: state is set by the caller's
// begin_*()/write_chunk()/read_chunk()/end_*() call, this function just executes it once.
void run_stream_step(StreamRequest &req) {
  if (req.state.load() == StreamState::CANCELLED) {
    if (req.is_fs && req.handle != nullptr)
      static_cast<FilesystemStorage *>(req.storage)->close(req.handle);
    req.result = StorageError::NOT_READY;
    req.state = StreamState::DONE;
    return;
  }

  StreamState current = req.state.load();
  StorageError err = StorageError::OK;

  switch (current) {
    case StreamState::OPENING: {
      if (req.is_fs) {
        OpenMode mode = req.op == StreamOp::WRITE ? OpenMode::WRITE : OpenMode::READ;
        err = static_cast<FilesystemStorage *>(req.storage)->open(req.path, req.handle, mode);
      }
      // NetworkStorage has no open() — write_chunk()/read_chunk() below address by offset
      // directly, so OPENING is a no-op for it beyond the state transition itself.
      req.state = (err == StorageError::OK) ? StreamState::IDLE : StreamState::DONE;
      req.result = err;
      break;
    }

    case StreamState::WRITING: {
      size_t bytes_written = 0;
      if (req.is_fs) {
        err = static_cast<FilesystemStorage *>(req.storage)
                  ->write(req.handle, req.pending_write_data, req.pending_len, &bytes_written);
      } else {
        err = static_cast<NetworkStorage *>(req.storage)
                  ->write_chunk(req.path, req.pending_write_data, req.offset, req.pending_len, &bytes_written);
      }
      if (err == StorageError::OK && bytes_written < req.pending_len && bytes_written > 0) {
        // Partial write — keep going within this same call rather than surfacing a short
        // write to the caller, mirroring storage::write_file()'s own retry-until-full loop.
        size_t total = bytes_written;
        while (err == StorageError::OK && total < req.pending_len) {
          size_t chunk_written = 0;
          if (req.is_fs) {
            err = static_cast<FilesystemStorage *>(req.storage)
                      ->write(req.handle, req.pending_write_data + total, req.pending_len - total, &chunk_written);
          } else {
            err = static_cast<NetworkStorage *>(req.storage)
                      ->write_chunk(req.path, req.pending_write_data + total, req.offset + total,
                                    req.pending_len - total, &chunk_written);
          }
          if (chunk_written == 0)
            break;
          total += chunk_written;
        }
        bytes_written = total;
      }
      if (err == StorageError::OK && bytes_written < req.pending_len)
        err = StorageError::WRITE_ERROR;  // 0 bytes accepted mid-stream — treat as failure
      req.offset += bytes_written;
      req.pending_write_data = nullptr;
      req.pending_len = 0;
      req.result = err;
      req.state = StreamState::IDLE;
      break;
    }

    case StreamState::READING: {
      size_t bytes_read = 0;
      if (req.is_fs) {
        err = static_cast<FilesystemStorage *>(req.storage)
                  ->read(req.handle, req.pending_read_buf, req.pending_len, &bytes_read);
      } else {
        err = static_cast<NetworkStorage *>(req.storage)
                  ->read_chunk(req.path, req.pending_read_buf, req.offset, req.pending_len, &bytes_read);
      }
      if (err == StorageError::OK)
        req.offset += bytes_read;
      if (req.bytes_transferred_out != nullptr)
        *req.bytes_transferred_out = bytes_read;
      req.pending_read_buf = nullptr;
      req.pending_len = 0;
      req.result = err;
      req.state = StreamState::IDLE;
      break;
    }

    case StreamState::CLOSING: {
      if (req.is_fs && req.handle != nullptr)
        err = static_cast<FilesystemStorage *>(req.storage)->close(req.handle);
      req.handle = nullptr;
      req.result = err;
      req.state = StreamState::DONE;
      break;
    }

    default:
      // IDLE/FREE/DONE: nothing to do — shouldn't be dispatched in these states.
      break;
  }
}

}  // namespace

void StorageWorker::run_stream_step_(StreamRequest &req) { run_stream_step(req); }

namespace {
bool find_free_stream_slot(FixedVector<StreamRequest> &pool, size_t *out_index) {
  for (size_t i = 0; i < pool.size(); i++) {
    StreamState expected = StreamState::FREE;
    if (pool[i].state.compare_exchange_strong(expected, StreamState::OPENING)) {
      *out_index = i;
      return true;
    }
  }
  return false;
}

}  // namespace

// Dispatches one step: hands off to the shared task queue if task-safe, otherwise marks it
// pending for loop() to run on its next pass. Never runs the step inline from this call — the
// callback must always fire from loop() (or the task), never reentrantly from inside the
// caller's own begin_*/write_chunk/read_chunk/end_* call frame.
void StorageWorker::dispatch_stream_step_(StreamRequest &req, size_t index) {
#if defined(USE_ESP32) && defined(USE_STORAGE_WORKER_TASK)
  if (this->is_task_safe_(req)) {
    QueueEntry entry{QueueEntryKind::STREAM, index};
    if (xQueueSend(this->task_queue_, &entry, 0) == pdTRUE)
      return;
  }
#endif
  // Loop-sliced fallback: mark pending so loop() runs it on its next pass rather than here.
  req.pending_step_ = true;
}

StorageError StorageWorker::begin_write(PathStorage *storage, const char *path, StreamHandle *out_handle,
                                        CompletionCallback &&on_open) {
  this->ensure_started_();
  if (strlen(path) >= STORAGE_WORKER_MAX_PATH)
    return StorageError::INVALID_ARGS;

  size_t index;
  if (!find_free_stream_slot(this->stream_pool_, &index))
    return StorageError::NOT_READY;

  StreamRequest &req = this->stream_pool_[index];
  req.op = StreamOp::WRITE;
  req.storage = storage;
  strncpy(req.path, path, STORAGE_WORKER_MAX_PATH - 1);
  req.path[STORAGE_WORKER_MAX_PATH - 1] = '\0';
  req.is_fs = storage->get_storage_type() == StorageType::FILESYSTEM;
  req.handle = nullptr;
  req.offset = 0;
  req.callback = std::move(on_open);
  req.result = StorageError::OK;

  out_handle->index = index;
  this->dispatch_stream_step_(req, index);
  return StorageError::OK;
}

StorageError StorageWorker::begin_read(PathStorage *storage, const char *path, StreamHandle *out_handle,
                                       CompletionCallback &&on_open) {
  this->ensure_started_();
  if (strlen(path) >= STORAGE_WORKER_MAX_PATH)
    return StorageError::INVALID_ARGS;

  size_t index;
  if (!find_free_stream_slot(this->stream_pool_, &index))
    return StorageError::NOT_READY;

  StreamRequest &req = this->stream_pool_[index];
  req.op = StreamOp::READ;
  req.storage = storage;
  strncpy(req.path, path, STORAGE_WORKER_MAX_PATH - 1);
  req.path[STORAGE_WORKER_MAX_PATH - 1] = '\0';
  req.is_fs = storage->get_storage_type() == StorageType::FILESYSTEM;
  req.handle = nullptr;
  req.offset = 0;
  req.callback = std::move(on_open);
  req.result = StorageError::OK;

  out_handle->index = index;
  this->dispatch_stream_step_(req, index);
  return StorageError::OK;
}

StorageError StorageWorker::write_chunk(const StreamHandle &handle, const uint8_t *data, size_t len,
                                        CompletionCallback &&on_written) {
  if (handle.index >= this->stream_pool_.size())
    return StorageError::INVALID_ARGS;
  StreamRequest &req = this->stream_pool_[handle.index];
  StreamState expected = StreamState::IDLE;
  if (!req.state.compare_exchange_strong(expected, StreamState::WRITING))
    return StorageError::NOT_READY;

  req.pending_write_data = data;
  req.pending_len = len;
  req.callback = std::move(on_written);

  this->dispatch_stream_step_(req, handle.index);
  return StorageError::OK;
}

StorageError StorageWorker::read_chunk(const StreamHandle &handle, uint8_t *buf, size_t len, size_t *bytes_read,
                                       CompletionCallback &&on_read) {
  if (handle.index >= this->stream_pool_.size())
    return StorageError::INVALID_ARGS;
  StreamRequest &req = this->stream_pool_[handle.index];
  StreamState expected = StreamState::IDLE;
  if (!req.state.compare_exchange_strong(expected, StreamState::READING))
    return StorageError::NOT_READY;

  req.pending_read_buf = buf;
  req.pending_len = len;
  req.bytes_transferred_out = bytes_read;
  req.callback = std::move(on_read);

  this->dispatch_stream_step_(req, handle.index);
  return StorageError::OK;
}

StorageError StorageWorker::end_write(const StreamHandle &handle, CompletionCallback &&on_closed) {
  if (handle.index >= this->stream_pool_.size())
    return StorageError::INVALID_ARGS;
  StreamRequest &req = this->stream_pool_[handle.index];
  StreamState expected = StreamState::IDLE;
  if (!req.state.compare_exchange_strong(expected, StreamState::CLOSING))
    return StorageError::NOT_READY;

  req.callback = std::move(on_closed);
  this->dispatch_stream_step_(req, handle.index);
  return StorageError::OK;
}

StorageError StorageWorker::end_read(const StreamHandle &handle, CompletionCallback &&on_closed) {
  return this->end_write(handle, std::move(on_closed));  // identical close path
}

}  // namespace esphome::storage

#endif  // USE_STORAGE_WORKER
