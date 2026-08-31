#include <gtest/gtest.h>

#include "esphome/components/storage/automation.h"

#include <functional>
#include <string>
#include <vector>

// Covers perform_list_dir(), the helper behind storage.list_dir. Everything interesting about it
// is in what it does around the driver call rather than in the call itself: the mount point has to
// become a path a driver accepts, the entry limit has to stop the walk without turning it into an
// error, and four separate refusals have to come back as errors instead of as an empty listing.
//
// An empty listing is exactly what all of those look like from the outside -- on_complete with
// count 0 -- which is why they are pinned here. A directory that could not be read must never be
// indistinguishable from one that is empty.
//
// The triggers are driven through a real Automation, not observed directly, so the test exercises
// the same path the generated code does: Trigger::trigger() is a no-op without an attached parent.

namespace esphome::storage::testing {

namespace {

// A PathStorage whose list_dir() replays a scripted set of entries and records the path it was
// asked for. Everything else only has to exist.
class ScriptedStorage : public FilesystemStorage {
 public:
  explicit ScriptedStorage(const char *mount) { this->set_mount_path_(mount); }

  void add_entry(const char *name, bool is_dir, uint64_t size, uint32_t mtime) {
    FileStat entry{};
    // strncpy with the field size leaves no terminator on an over-long name; the field is
    // value-initialized above, so the last byte stays 0.
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.is_dir = is_dir;
    entry.size = size;
    entry.mtime = mtime;
    this->entries_.push_back(entry);
  }
  void fail_with(StorageError err) { this->err_ = err; }
  const std::string &last_path() const { return this->last_path_; }

  StorageError list_dir(const char *path, bool (*callback)(const FileStat *entry, void *ctx), void *ctx) override {
    this->last_path_ = path;
    if (this->err_ != StorageError::STORAGE_ERROR_OK)
      return this->err_;
    for (const auto &entry : this->entries_) {
      if (!callback(&entry, ctx))
        break;  // Early stop is not an error, per the list_dir() contract.
    }
    return StorageError::STORAGE_ERROR_OK;
  }

  StorageError get_info(StorageInfo *info) override {
    *info = StorageInfo{};
    return StorageError::STORAGE_ERROR_OK;
  }
  StorageError stat(const char *, FileStat *) override { return StorageError::STORAGE_ERROR_NOT_FOUND; }
  StorageError mkdir(const char *) override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }
  StorageError rmdir(const char *) override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }
  StorageError remove(const char *) override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }
  StorageError rename(const char *, const char *) override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }
  StorageError mount() override { return StorageError::STORAGE_ERROR_OK; }
  StorageError unmount() override { return StorageError::STORAGE_ERROR_OK; }
  StorageError format() override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }
  StorageError sync() override { return StorageError::STORAGE_ERROR_OK; }
  StorageError open(const char *, FileHandle *&, OpenMode) override {
    return StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  }
  StorageError close(FileHandle *) override { return StorageError::STORAGE_ERROR_OK; }
  StorageError read(FileHandle *, uint8_t *, size_t, size_t *) override {
    return StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  }
  StorageError write(FileHandle *, const uint8_t *, size_t, size_t *) override {
    return StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  }
  StorageError seek(FileHandle *, int64_t, SeekMode) override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }
  StorageError tell(FileHandle *, uint64_t *) override { return StorageError::STORAGE_ERROR_NOT_SUPPORTED; }

 protected:
  std::vector<FileStat> entries_;
  StorageError err_{StorageError::STORAGE_ERROR_OK};
  std::string last_path_;
};

// What one on_entry invocation carried.
struct Reported {
  std::string name;
  uint64_t size;
  bool is_dir;
  uint32_t mtime;
};

// Stands in for the generated automation body. The callback lets one test re-enter
// perform_list_dir() from inside the walk, which is the case the re-entrancy guard exists for.
class EntryRecorder : public Action<std::string, uint64_t, bool, uint32_t> {
 public:
  std::vector<Reported> entries;
  std::function<void()> on_each;

  void play(const std::string &name, const uint64_t &size, const bool &is_dir, const uint32_t &mtime) override {
    this->entries.push_back(Reported{name, size, is_dir, mtime});
    if (this->on_each)
      this->on_each();
  }
};

}  // namespace

class ListDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    this->registry_.set_device_count(1);
    ASSERT_EQ(this->registry_.register_storage(&this->sd_), StorageError::STORAGE_ERROR_OK);
    global_storage_registry = &this->registry_;
    this->automation_.add_action(&this->recorder_);
  }

  void TearDown() override { global_storage_registry = nullptr; }

  // Runs a listing and hands back the sink so a test can assert on count and truncation.
  ListDirSink run(const std::string &path, uint32_t max_entries = 256) {
    ListDirSink sink{&this->entry_trigger_, max_entries, 0, false};
    this->last_error_ = perform_list_dir(path, &sink);
    return sink;
  }

  StorageRegistry registry_;
  ScriptedStorage sd_{"/sd"};
  Trigger<std::string, uint64_t, bool, uint32_t> entry_trigger_;
  Automation<std::string, uint64_t, bool, uint32_t> automation_{&this->entry_trigger_};
  EntryRecorder recorder_;
  StorageError last_error_{StorageError::STORAGE_ERROR_OK};
};

// ---------------------------------------------------------------------------
// The happy paths
// ---------------------------------------------------------------------------

TEST_F(ListDirTest, ReportsEveryFieldOfEveryEntry) {
  this->sd_.add_entry("track.flac", false, 4096, 1700000000);
  this->sd_.add_entry("albums", true, 0, 0);

  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_OK);
  EXPECT_EQ(sink.count, 2u);
  EXPECT_FALSE(sink.truncated);
  ASSERT_EQ(this->recorder_.entries.size(), 2u);
  EXPECT_EQ(this->recorder_.entries[0].name, "track.flac");
  EXPECT_EQ(this->recorder_.entries[0].size, 4096u);
  EXPECT_FALSE(this->recorder_.entries[0].is_dir);
  EXPECT_EQ(this->recorder_.entries[0].mtime, 1700000000u);
  EXPECT_EQ(this->recorder_.entries[1].name, "albums");
  EXPECT_TRUE(this->recorder_.entries[1].is_dir);
}

TEST_F(ListDirTest, AsksTheDriverForItsRootWhenGivenTheMountPoint) {
  // resolve_path() yields "" for the mount point itself, which is not a path any driver accepts.
  this->sd_.add_entry("a.txt", false, 1, 0);

  this->run("/sd");

  EXPECT_EQ(this->sd_.last_path(), "/");
}

TEST_F(ListDirTest, PassesTheRelativePathForASubdirectory) {
  this->sd_.add_entry("a.txt", false, 1, 0);

  this->run("/sd/music/rock");

  EXPECT_EQ(this->sd_.last_path(), "/music/rock");
}

TEST_F(ListDirTest, AnEmptyDirectoryIsNotAnError) {
  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_OK);
  EXPECT_EQ(sink.count, 0u);
  EXPECT_FALSE(sink.truncated);
  EXPECT_TRUE(this->recorder_.entries.empty());
}

// ---------------------------------------------------------------------------
// The entry limit
// ---------------------------------------------------------------------------

TEST_F(ListDirTest, StopsAtMaxEntriesWithoutReportingAnError) {
  for (int i = 0; i < 10; i++)
    this->sd_.add_entry(("f" + std::to_string(i)).c_str(), false, 1, 0);

  const ListDirSink sink = this->run("/sd", 3);

  // The walk stopping early is not a failure: on_complete still fires, with the count it got.
  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_OK);
  EXPECT_EQ(sink.count, 3u);
  EXPECT_TRUE(sink.truncated);
  EXPECT_EQ(this->recorder_.entries.size(), 3u);
}

TEST_F(ListDirTest, ReportsExactlyMaxEntriesWithoutFlaggingTruncation) {
  // The boundary: a directory that exactly fills the limit was not truncated, and reporting it as
  // truncated would send a false warning on every listing of a full-but-complete directory.
  for (int i = 0; i < 3; i++)
    this->sd_.add_entry(("f" + std::to_string(i)).c_str(), false, 1, 0);

  const ListDirSink sink = this->run("/sd", 3);

  EXPECT_EQ(sink.count, 3u);
  EXPECT_FALSE(sink.truncated);
}

// ---------------------------------------------------------------------------
// The refusals -- each would otherwise look like an empty directory
// ---------------------------------------------------------------------------

TEST_F(ListDirTest, RefusesAPathWithNoStorageMounted) {
  const ListDirSink sink = this->run("/nowhere/music");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_NOT_READY);
  EXPECT_EQ(sink.count, 0u);
  EXPECT_TRUE(this->recorder_.entries.empty());
}

TEST_F(ListDirTest, RefusesWithoutARegistry) {
  global_storage_registry = nullptr;

  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_NOT_READY);
  EXPECT_EQ(sink.count, 0u);
}

TEST_F(ListDirTest, PassesTheDriverErrorThrough) {
  this->sd_.add_entry("a.txt", false, 1, 0);
  this->sd_.fail_with(StorageError::STORAGE_ERROR_READ_ERROR);

  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_READ_ERROR);
  EXPECT_EQ(sink.count, 0u);
  EXPECT_TRUE(this->recorder_.entries.empty());
}

TEST_F(ListDirTest, RefusesAWalkStartedFromInsideAnotherWalk) {
  // on_entry runs user automations while the driver's directory handle is open, and one of them
  // may be a second storage.list_dir. Drivers are not re-entrant, so the inner call is refused --
  // and the outer one has to survive it and finish normally.
  this->sd_.add_entry("a.txt", false, 1, 0);
  this->sd_.add_entry("b.txt", false, 2, 0);

  StorageError inner_error = StorageError::STORAGE_ERROR_OK;
  uint32_t inner_calls = 0;
  Trigger<std::string, uint64_t, bool, uint32_t> inner_trigger;
  this->recorder_.on_each = [&]() {
    ListDirSink inner{&inner_trigger, 256, 0, false};
    inner_error = perform_list_dir("/sd", &inner);
    inner_calls++;
  };

  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(inner_calls, 2u);
  EXPECT_EQ(inner_error, StorageError::STORAGE_ERROR_NOT_READY);
  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_OK);
  EXPECT_EQ(sink.count, 2u);
}

TEST_F(ListDirTest, AllowsAWalkAfterARefusedNestedOne) {
  // The guard must be cleared on the way out, or one nested call would leave every later listing
  // refused for the rest of the run.
  this->sd_.add_entry("a.txt", false, 1, 0);
  Trigger<std::string, uint64_t, bool, uint32_t> inner_trigger;
  this->recorder_.on_each = [&]() {
    ListDirSink inner{&inner_trigger, 256, 0, false};
    perform_list_dir("/sd", &inner);
  };
  this->run("/sd");

  this->recorder_.on_each = nullptr;
  this->recorder_.entries.clear();
  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_OK);
  EXPECT_EQ(sink.count, 1u);
}

// ---------------------------------------------------------------------------
// The guard must also survive the paths that return early
// ---------------------------------------------------------------------------

TEST_F(ListDirTest, LeavesTheGuardClearAfterADriverError) {
  this->sd_.add_entry("a.txt", false, 1, 0);
  this->sd_.fail_with(StorageError::STORAGE_ERROR_READ_ERROR);
  this->run("/sd");

  this->sd_.fail_with(StorageError::STORAGE_ERROR_OK);
  const ListDirSink sink = this->run("/sd");

  EXPECT_EQ(this->last_error_, StorageError::STORAGE_ERROR_OK);
  EXPECT_EQ(sink.count, 1u);
}

}  // namespace esphome::storage::testing
