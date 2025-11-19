#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace esphome {
namespace storage {

//========================================================================
// Enums
//========================================================================

enum class StorageType : uint8_t {
  BINARY_STORAGE,  // FRAM, EEPROM, Flash
  SD_CARD,         // SD/MMC cards
  USB_MSC,         // USB mass storage
  NETWORK,         // NFS, SMB, etc.
};

enum class FilesystemType : uint8_t {
  NONE,      // Raw access only
  FAT,       // FAT16/FAT32
  EXFAT,     // exFAT
  LITTLEFS,  // LittleFS
};

//========================================================================
// Info Structures
//========================================================================

/// Storage device information
struct StorageInfo {
  std::string id;             // Unique identifier
  std::string name;           // Human-readable name
  StorageType type;           // Type of storage
  FilesystemType filesystem;  // Filesystem type (NONE if raw only)
  std::string mount_path;     // VFS mount path (empty if raw only)
  uint64_t total_bytes;       // Total capacity
  uint64_t free_bytes;        // Available space
  uint32_t block_size;        // Block/sector size
  bool is_mounted;            // Currently mounted
  bool is_removable;          // Can be hot-plugged
  bool is_read_only;          // Write-protected
  bool supports_raw_access;   // Has raw read/write
  bool supports_filesystem;   // Has filesystem operations
};

/// File/directory information for filesystem operations
struct StorageFileInfo {
  std::string name;        // File/directory name
  std::string path;        // Full path
  uint64_t size;           // Size in bytes (0 for directories)
  bool is_directory;       // Is a directory
  uint32_t modified_time;  // Last modification (Unix timestamp, 0 if unavailable)
};

//========================================================================
// StorageDevice Abstract Interface
//========================================================================

class StorageDevice {
 public:
  virtual ~StorageDevice() = default;

  //========================================================================
  // Device Information (ALL must implement)
  //========================================================================

  /// Get device information
  virtual StorageInfo get_info() = 0;

  /// Check if device is currently available/mounted
  virtual bool is_available() = 0;

  //========================================================================
  // Raw/Binary Access (implement if supports_raw_access)
  //========================================================================

  /// Check if device supports raw binary access
  virtual bool supports_raw_access() { return false; }

  /// Read binary data from address
  /// @param address Starting address
  /// @param data Buffer to store data
  /// @param length Number of bytes to read
  /// @return true on success
  virtual bool raw_read(uint32_t address, uint8_t *data, size_t length) { return false; }

  /// Write binary data to address
  /// @param address Starting address
  /// @param data Data to write
  /// @param length Number of bytes to write
  /// @return true on success
  virtual bool raw_write(uint32_t address, const uint8_t *data, size_t length) { return false; }

  /// Erase block at address (for Flash devices)
  /// @param address Address within block to erase
  /// @return true on success
  virtual bool raw_erase(uint32_t address) { return true; }

  /// Get raw storage capacity in bytes
  virtual uint32_t get_raw_capacity() { return 0; }

  //========================================================================
  // Filesystem Access (implement if supports_filesystem)
  //========================================================================

  /// Check if device supports filesystem operations
  virtual bool supports_filesystem() { return false; }

  /// Get VFS mount path (e.g., "/sd", "/fram")
  virtual std::string get_mount_path() { return ""; }

  //------------------------------------------------------------------------
  // File Operations
  //------------------------------------------------------------------------

  /// Check if file exists
  virtual bool file_exists(const char *path) { return false; }

  /// Get file size
  /// @param path File path
  /// @param size Output: file size in bytes
  /// @return true if file exists
  virtual bool get_file_size(const char *path, size_t *size) { return false; }

  /// Read entire file into buffer
  /// @param path File path
  /// @param data Buffer to store data
  /// @param length In: buffer size, Out: bytes read
  /// @return true on success
  virtual bool read_file(const char *path, uint8_t *data, size_t *length) { return false; }

  /// Write data to file (creates or overwrites)
  /// @param path File path
  /// @param data Data to write
  /// @param length Number of bytes
  /// @return true on success
  virtual bool write_file(const char *path, const uint8_t *data, size_t length) { return false; }

  /// Append data to file
  virtual bool append_file(const char *path, const uint8_t *data, size_t length) { return false; }

  /// Delete file
  virtual bool delete_file(const char *path) { return false; }

  /// Rename/move file
  virtual bool rename_file(const char *old_path, const char *new_path) { return false; }

  /// Copy file
  virtual bool copy_file(const char *src_path, const char *dst_path) { return false; }

  //------------------------------------------------------------------------
  // Directory Operations
  //------------------------------------------------------------------------

  /// Check if directory exists
  virtual bool dir_exists(const char *path) { return false; }

  /// Create directory
  virtual bool create_dir(const char *path) { return false; }

  /// Delete directory
  /// @param path Directory path
  /// @param recursive If true, delete contents first
  /// @return true on success
  virtual bool delete_dir(const char *path, bool recursive = false) { return false; }

  /// List directory contents
  /// @param path Directory path
  /// @param entries Output: list of files/directories
  /// @return true on success
  virtual bool list_dir(const char *path, std::vector<StorageFileInfo> *entries) { return false; }

  //------------------------------------------------------------------------
  // Space Information
  //------------------------------------------------------------------------

  /// Get filesystem space info
  /// @param total Output: total bytes
  /// @param free Output: free bytes
  /// @return true on success
  virtual bool get_space_info(uint64_t *total, uint64_t *free) { return false; }

  /// Check if file can be written (path valid + enough space)
  /// @param path Target file path
  /// @param size Required size in bytes
  /// @return true if write is possible
  virtual bool can_write_file(const char *path, size_t size) { return false; }

  //------------------------------------------------------------------------
  // Streaming File Access (for large files)
  //------------------------------------------------------------------------

  /// Open file for streaming access
  /// @param path File path
  /// @param mode Mode string ("r", "w", "a", "r+", "w+", "a+")
  /// @return File handle (nullptr on failure)
  virtual void *open_file(const char *path, const char *mode) { return nullptr; }

  /// Read chunk from open file
  /// @param handle File handle from open_file()
  /// @param buffer Buffer to store data
  /// @param size Maximum bytes to read
  /// @return Bytes actually read (0 on EOF or error)
  virtual size_t read_file_chunk(void *handle, uint8_t *buffer, size_t size) { return 0; }

  /// Write chunk to open file
  /// @param handle File handle from open_file()
  /// @param data Data to write
  /// @param size Number of bytes to write
  /// @return Bytes actually written (0 on error)
  virtual size_t write_file_chunk(void *handle, const uint8_t *data, size_t size) { return 0; }

  /// Seek to position in file
  /// @param handle File handle from open_file()
  /// @param offset Byte offset from start
  /// @return true on success
  virtual bool seek_file(void *handle, size_t offset) { return false; }

  /// Get current position in file
  /// @param handle File handle from open_file()
  /// @return Current byte offset
  virtual size_t tell_file(void *handle) { return 0; }

  /// Close file handle
  /// @param handle File handle from open_file()
  /// @return true on success
  virtual bool close_file(void *handle) { return false; }

  //------------------------------------------------------------------------
  // Maintenance
  //------------------------------------------------------------------------

  /// Format the storage (WARNING: destroys all data)
  virtual bool format() { return false; }

  /// Sync/flush pending writes
  virtual bool sync() { return true; }
};

}  // namespace storage
}  // namespace esphome
