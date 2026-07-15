#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "audio.h"
#include "audio_transfer_buffer.h"

#include "esphome/components/ring_buffer/ring_buffer.h"

#include "esp_err.h"

#include <esp_http_client.h>

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome::audio {

enum class AudioReaderState : uint8_t {
  READING = 0,  // More data is available to read
  FINISHED,     // All data has been read and transferred
  FAILED,       // Encountered an error
};

class AudioReader {
  /*
   * @brief Class that facilitates reading a raw audio file.
   * Files can be read from flash (stored in a AudioFile struct), from an http source, or —
   * with the storage component in the build — from any mounted storage via file:// URIs
   * (file:///sdcard/music/track.flac). Storage reads use the DATA-PLANE handle API and are
   * therefore safe from the speaker task.
   * The file data is sent to a ring buffer sink.
   */
 public:
  /// @brief Constructs an AudioReader object.
  /// The transfer buffer isn't allocated here, but only if necessary (an http source) in the start function.
  /// @param buffer_size Transfer buffer size in bytes.
  AudioReader(size_t buffer_size) : buffer_size_(buffer_size) {}
  ~AudioReader();

  /// @brief Adds a sink ring buffer for audio data. Takes ownership of the ring buffer in a shared_ptr
  /// @param output_ring_buffer weak_ptr of a shared_ptr of the sink ring buffer to transfer ownership
  /// @return  ESP_OK if successful, ESP_ERR_INVALID_STATE otherwise
  esp_err_t add_sink(const std::weak_ptr<ring_buffer::RingBuffer> &output_ring_buffer);

  /// @brief Starts reading an audio file from an http source. The transfer buffer is allocated here.
  /// @param uri Web url to the http file.
  /// @param file_type AudioFileType variable passed-by-reference indicating the type of file being read.
  /// @return ESP_OK if successful, an ESP_ERR* code otherwise.
  esp_err_t start(const std::string &uri, AudioFileType &file_type);

  /// @brief Starts reading an audio file from flash. No transfer buffer is allocated.
  /// @param audio_file AudioFile struct containing the file.
  /// @param file_type AudioFileType variable passed-by-reference indicating the type of file being read.
  /// @return ESP_OK
  esp_err_t start(AudioFile *audio_file, AudioFileType &file_type);

  /// @brief Reads new file data from the source and sends to the ring buffer sink.
  /// @return AudioReaderState
  AudioReaderState read();

 protected:
  /// @brief Monitors the http client events to attempt determining the file type from the Content-Type header
  static esp_err_t http_event_handler(esp_http_client_event_t *evt);

  AudioReaderState file_read_();
#ifdef USE_STORAGE
  AudioReaderState storage_read_();
#endif
  AudioReaderState http_read_();

  std::shared_ptr<ring_buffer::RingBuffer> file_ring_buffer_;
  std::unique_ptr<AudioSinkTransferBuffer> output_transfer_buffer_;
  void cleanup_connection_();

  size_t buffer_size_;
  uint32_t last_data_read_ms_;

  esp_http_client_handle_t client_{nullptr};

  AudioFile *current_audio_file_{nullptr};
  AudioFileType audio_file_type_{AudioFileType::NONE};
  const uint8_t *file_current_{nullptr};
#ifdef USE_STORAGE
  // FILESYSTEM storages stream through a handle; NETWORK storages (NFS) are
  // stateless by design — chunked reads by path + self-tracked offset.
  storage::PathStorage *storage_{nullptr};
  storage::FileHandle *storage_handle_{nullptr};  // FILESYSTEM only
  std::string storage_path_;                      // NETWORK only (owned copy)
  uint64_t storage_offset_{0};                    // NETWORK only
#endif
};
}  // namespace esphome::audio

#endif
