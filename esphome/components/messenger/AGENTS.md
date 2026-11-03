# messenger: guide for AI agents

One messenger for one user: channels (Telegram, WhatsApp, Facebook Messenger) with transports (`direct`,
`home_assistant`, `account`), never several parallel implementations of the same function.

- `messenger.*`: channels, sending (texts, files, camera photos, voice messages encoded to Opus), receiving
  (polling the bot API with long polling, Home Assistant / webhook input through `receive` / `receive_webhook`),
  downloads, the spool folder on a storage, triggers.
- `telegram_account.*`: the `account` transport on `mtproto` - dialogs, history, files, avatars, thumbnails;
  `tl_read.h`: helpers for reading TL objects.
- `telegram_call.*`: Telegram calls over `esp_peer`; audio Opus (the `audio` component's `OpusVoiceEncoder` /
  `OpusVoiceDecoder`, encoded on the call task) from the messenger's microphone source to the call's speaker, video
  from the camera's H.264 stream
  (`camera::CameraListener`), an optional video relay.
- Network and file work runs on tasks / the storage worker; results reach the main loop through `defer()`; LVGL UIs
  are built in YAML and fed by the triggers.
- Files are streamed through the storage worker with the borrowed transfer buffer; nothing holds a whole file in RAM.
