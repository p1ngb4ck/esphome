# esphome-messenger

`messenger`: a messenger app for ESPHome - send and receive texts, photos (also straight from a camera), voice
messages and files with Telegram, WhatsApp and Facebook Messenger, and make calls. Telegram works as a bot, through
Home Assistant, or as a full user account through `mtproto` - an MTProto client that runs on the device (QR login,
two-step password); calls run through the `sip_phone` media path, video calls with the camera's H.264 stream.

## Getting started

A Telegram bot that the device can write to and that can write to the device:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-messenger@main
    components: [messenger, storage]

messenger:
  id: chat
  telegram:
    token: !secret telegram_bot_token
    allowed_chats: "123456789"
    default_chat: 123456789
  on_message:
    - logger.log:
        format: "%s: %s"
        args: [sender.c_str(), text.c_str()]

button:
  - platform: template
    name: Say hello
    on_press:
      - messenger.send_message:
          text: Hello from the device
```

Telegram as a user account (the device shows a QR code to scan in the Telegram app):

```yaml
mtproto:
  api_id: !secret telegram_api_id
  api_hash: !secret telegram_api_hash

messenger:
  telegram:
    transport: account
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option - channels and transports, voice messages, photos from a
camera, files and downloads, calls and video calls, all triggers and actions, `mtproto`.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/mtproto` | `mtproto`: Telegram MTProto client (crypto, TL schema, QR login) | `pr/network-priority` ([esphome-network](https://github.com/p1ngb4ck/esphome-network)) | ESP32 |
| `pr/messenger` | `messenger`: the messenger app (Telegram, WhatsApp, Facebook Messenger; bot, Home Assistant or account transport; calls through sip_phone and the camera's H.264 stream) | `pr/mtproto`, `pr/sip-phone` ([esphome-phone](https://github.com/p1ngb4ck/esphome-phone)), `pr/camera-encoder-h264` ([esphome-esp-video](https://github.com/p1ngb4ck/esphome-esp-video)), `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | ESP32 (camera H.264: P4) |

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## Thanks

`mtproto` is built from Telegram's published MTProto documentation and TL schema. Everyone this project builds on:
[ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
