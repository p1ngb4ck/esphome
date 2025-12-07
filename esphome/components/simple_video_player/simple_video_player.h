#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/storage/file_manager.h"
#include "esphome/components/transcoder/transcoder.h"
#include "lvgl.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace esphome::simple_video_player {

// Forward declarations
class SimpleVideoPlayer;

typedef struct {
  void *sub_src; /*!< Sub source to keep media source extra data */
} media_src_t;

int media_src_storage_open(media_src_t *src);
int media_src_storage_connect(media_src_t *src, const char *uri);
int media_src_storage_disconnect(media_src_t *src);
int media_src_storage_read(media_src_t *src, void *data, size_t len);
int media_src_storage_seek(media_src_t *src, uint64_t position);
int media_src_storage_get_position(media_src_t *src, uint64_t *position);
int media_src_storage_get_size(media_src_t *src, uint64_t *size);
int media_src_storage_close(media_src_t *src);

/**
 * @brief Player states
 */
typedef enum {
  PLAYER_STATE_PLAYING,
  PLAYER_STATE_PAUSED,
  PLAYER_STATE_STOPPED,
} player_state_t;

/**
 * @brief Player configuration structure
 */
typedef struct {
  const char *video_path;   /* File path to play */
  const char *bgm_path;     /* File path to play */
  lv_obj_t *screen;         /* LVGL screen to put the player */
  uint32_t buff_size;       /* Size of the buffer for one video frame */
  uint32_t cache_buff_size; /* Size of the buffer for one video frame */
  bool cache_buff_in_psram; /* Use PSRAM for split buffer */
  uint32_t screen_width;    /* Width of the video player object */
  uint32_t screen_height;   /* Height of the video player object */
  struct {
    unsigned int hide_controls : 1; /* Hide control buttons */
    unsigned int hide_slider : 1;   /* Hide indication slider */
    unsigned int hide_status : 1;   /* Hide status icons in video (paused, stopped) */

    unsigned int auto_width : 1;  /* Set automatic width by video size */
    unsigned int auto_height : 1; /* Set automatic height by video size */
  } flags;
} lvgl_simple_player_cfg_t;

static const jpeg_decode_cfg_t jpeg_decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};

class SimpleVideoPlayer : public Component {
 public:
  SimpleVideoPlayer() = default;
  ~SimpleVideoPlayer();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // =====================================================
  // Configuration
  // =====================================================

  void set_transcoder(transcoder::Transcoder *tc) { this->transcoder_ = tc; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }

 protected:
  // =====================================================
  // Begin LVGL Simple Video Player API
  // =====================================================
  /**
   * @brief Create Player
   * This function creates LVGL objects and starts handling task.
   * @return ESP_OK on success
   */
  lv_obj_t *lvgl_simple_player_create(lvgl_simple_player_cfg_t *params);

  /**
   * @brief Get player state
   */
  player_state_t lvgl_simple_player_get_state(void);

  /**
   * @brief Change file for playing
   */
  void lvgl_simple_player_change_file(const char *video_file);

  /**
   * @brief Play player
   */
  void lvgl_simple_player_play(void);

  /**
   * @brief Pause player
   */
  void lvgl_simple_player_pause(void);
  /**
   * @brief Resume player
   */
  void lvgl_simple_player_resume(void);

  /**
   * @brief Stop player
   */
  void lvgl_simple_player_stop(void);

  /**
   * @brief Set repeat playing
   */
  void lvgl_simple_player_repeat(bool repeat);

  /**
   * @brief Delete Player
   * @return ESP_OK on success
   */
  esp_err_t lvgl_simple_player_del(void);

  esp_err_t lvgl_simple_player_wait_task_stop(int timeout_ms);

  // =====================================================
  // End LVGL Simple Video Player API
  // =====================================================

  transcoder::Transcoder *transcoder_{nullptr};
  lv_obj_t *canvas_{nullptr};
};

}  // namespace esphome::simple_video_player
