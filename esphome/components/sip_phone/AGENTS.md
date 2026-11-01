# sip_phone: guide for AI agents

A SIP user agent with RTP media.

- `sip_message.*`: SIP message parsing / building; `sdp.*`: offers and answers (codec negotiation, late offers);
  `sip_phone.*`: registration (digest auth), call states, DTMF (RTP events or SIP INFO), MESSAGE, contacts, API actions, triggers.
- `media.*` (`CallMedia`): the RTP session - microphone frames from the resampler microphone encoded (Opus through
  ESPHome's audio component, `g722.*`, `g711.h`), received packets through a playout buffer (`playout_delay`) to
  the speaker. Other transports plug in through `sip_phone::MediaTransport` (the messenger's Telegram calls) and
  reuse the same media path instead of a second pipeline.
- Threading: SIP signalling runs on its own task (`sig_task`, `StaticTask`), media on the call's tasks; the account
  the task works with is a copy (`task_account_`) handed over from the main loop. Triggers and entities are updated
  on the main loop.
- Ports: `sip_port`, `rtp_port`.
- No buffers when idle: media buffers exist only during a call.
