/**
 * @file buffered_file_reader.h
 * @brief Optimized buffered file reader for video playback from storage devices
 *
 * Provides efficient buffered reading from SD/USB storage with:
 * - 64KB aligned cache buffer for optimal SD card performance
 * - Cache-aware seeking to avoid unnecessary re-reads
 * - DMA-aligned internal buffer (128-byte alignment)
 * - Works with standard FILE* handles from VFS-mounted storage
 *
 * Based on ESP Brookesia video player's media_src_storage implementation.
 */

#pragma once

#include <cstdio>
#include <cstdint>
#include <memory>

namespace esphome {
namespace simple_video_player {

/**
 * @brief Buffered file reader optimized for SD/USB storage access
 *
 * This class provides an efficient buffering layer on top of standard FILE*
 * handles, optimizing for storage device characteristics:
 * - Reads in large 64KB chunks to minimize storage access overhead
 * - Aligns reads to 1024-byte boundaries for optimal SD card performance
 * - Caches data to avoid redundant reads
 * - Provides cache-aware seeking to skip re-reads when possible
 */
class BufferedFileReader {
 public:
  BufferedFileReader();
  ~BufferedFileReader();

  // Disable copy/move
  BufferedFileReader(const BufferedFileReader &) = delete;
  BufferedFileReader &operator=(const BufferedFileReader &) = delete;

  /**
   * @brief Open a file for buffered reading
   * @param path File path (VFS path like "/sdcard/video.mjpeg")
   * @return true if file opened successfully
   */
  bool open(const char *path);

  /**
   * @brief Close the file and release resources
   */
  void close();

  /**
   * @brief Check if file is currently open
   * @return true if open
   */
  bool is_open() const { return this->file_handle_ != nullptr; }

  /**
   * @brief Read data from file into buffer
   * @param buffer Destination buffer
   * @param size Number of bytes to read
   * @return Number of bytes actually read (0 on EOF, -1 on error)
   */
  int read(uint8_t *buffer, size_t size);

  /**
   * @brief Seek to position in file
   * @param position Absolute byte offset from start
   * @return true on success
   */
  bool seek(uint64_t position);

  /**
   * @brief Get current file position
   * @return Current byte offset from start
   */
  uint64_t tell() const { return this->current_position_; }

  /**
   * @brief Get file size
   * @param size Output: file size in bytes
   * @return true on success
   */
  bool get_size(uint64_t *size);

  /**
   * @brief Flush internal cache (call before seeking)
   */
  void flush_cache();

  /**
   * @brief Pre-fill cache for optimal startup performance
   * Call immediately after open() to avoid first-read overhead
   * @return true if cache was filled successfully
   */
  bool prefill_cache();

 protected:
  /**
   * @brief Fill cache with next chunk of data from file
   * @return Number of bytes read into cache (0 on EOF, -1 on error)
   */
  int fill_cache_();

  /**
   * @brief Read data from cache, refilling if needed
   * @param buffer Destination buffer
   * @param size Number of bytes to read
   * @return Number of bytes read (0 on EOF, -1 on error)
   */
  int read_from_cache_(uint8_t *buffer, size_t size);

  // File handle
  FILE *file_handle_{nullptr};

  // Cache buffer (allocated in internal RAM with DMA alignment)
  std::unique_ptr<uint8_t[]> cache_buffer_;
  static constexpr size_t CACHE_SIZE = 64 * 1024;  // 64KB cache
  static constexpr size_t ALIGN_SIZE = 1024;       // SD card optimal alignment

  // Cache state
  size_t cache_filled_{0};      // Number of valid bytes in cache
  size_t cache_offset_{0};      // Current read position within cache
  uint64_t cache_file_pos_{0};  // File position where cache starts
  bool eof_{false};             // End of file reached

  // Current file position tracking
  uint64_t current_position_{0};  // Logical file position for user
  uint64_t align_position_{0};    // Aligned file position for actual reads
};

}  // namespace simple_video_player
}  // namespace esphome
