# Configuration reference

The first steps are in [README.md](README.md).

## `messenger`

| Option | Default | Description |
|---|---|---|
| `telegram` / `whatsapp` / `facebook` | - | The channels, see below. |
| `default_channel` | the first configured | `telegram`, `whatsapp` or `facebook` for actions without `channel`. |
| `api_version` | `v26.0` | Graph API version (WhatsApp, Facebook). |
| `spool_dir` | - | A storage folder for outgoing and downloaded files. |
| `max_upload` | `4MB` | Largest file sent, 1 KB - 50 MB. |
| `camera` | - | A camera for `send_camera_photo` and video calls. |
| `microphone` | - | A microphone source for voice messages (Opus). |
| `max_voice` | `60s` | Longest voice message. |

### Channels

Every channel takes:

| Option | Default | Description |
|---|---|---|
| `transport` | `direct` | `direct` (the platform's bot / business API from the device), `home_assistant` (through Home Assistant), `account` (Telegram only: a user account through `mtproto`). |
| `token` | - | Bot / access token for `direct`. |
| `allowed_chats` | all | Comma-separated chat IDs the device accepts messages from. |
| `default_chat` | `0` | Chat for actions without `chat_id`. |

Telegram also: `poll_timeout` (`50s`, 1-60 s, long polling) and `call` (`phone`: a `sip_phone` whose media path
the calls use; `video_relay`: `host`, `port` (`5063`) of a relay for video calls) - calls need `transport: account`. WhatsApp also: `phone_number_id`.

### Triggers

| Trigger | Variables |
|---|---|
| `on_message` | `channel`, `chat_id`, `sender`, `text` |
| `on_file` | `channel`, `chat_id`, `sender`, `kind`, `file_ref`, `file_name`, `size` |
| `on_downloaded` | `path`, `error` |
| `on_sent` | `channel`, `kind`, `error` |
| `on_outbound` | `channel`, `chat_id`, `kind`, `text`, `path` |
| `on_photo` / `on_history` | - |
| `on_account` | `changes` (account transport) |
| `on_avatar` | `peer_id`, `ok` |
| `on_thumb` | `message_id`, `ok` |
| `on_call` | `state` |

### Actions

All take `channel` and `chat_id` (optional, templatable; defaults above).

| Action | Options |
|---|---|
| `messenger.send_message` | `text` |
| `messenger.send_file` | `path`, `caption`, `kind` (`document` (default), `photo`, `voice`, `audio`, `video`) |
| `messenger.send_camera_photo` | `caption` |
| `messenger.download_file` | `file_ref`, `path` |
| `messenger.voice_start` / `voice_cancel` / `voice_send` | Record and send a voice message. |
| `messenger.receive` | Feed a message received elsewhere: `channel`, `chat_id` (required), `sender`, `text`, `kind`, `file_ref`, `file_name`, `size`. |
| `messenger.receive_webhook` | `payload`: a webhook body (WhatsApp / Facebook through Home Assistant). |

Condition `messenger.is_recording`.

```yaml
messenger:
  telegram:
    token: !secret telegram_bot_token
    default_chat: 123456789
  camera: front_camera
  microphone:
    microphone: call_mic
  spool_dir: /sdcard/messenger

binary_sensor:
  - platform: gpio
    pin: GPIO0
    on_press:
      - messenger.send_camera_photo:
          caption: Doorbell
```

## `mtproto`

The Telegram user-account client behind `transport: account`.

| Option | Default | Description |
|---|---|---|
| `api_id` / `api_hash` | - | Your Telegram API credentials (my.telegram.org); can also be set at runtime (`mtproto.set_api`). |
| `device_model` | - | Shown in Telegram's device list. |
| `app_version` | `1.0` | Shown in Telegram's device list. |
| `lang_code` | `en` | Language code sent to Telegram. |
| `on_state` | - | The login state changed (`state`): no API credentials, connecting, QR login (scan the QR code the device shows), two-step password needed, logged in, error. |

Actions: `mtproto.check_password` (`password`), `mtproto.set_api` (`api_id`, `api_hash`), `mtproto.logout`. API
actions `mtproto_check_password`, `mtproto_set_api`, `mtproto_logout`.
