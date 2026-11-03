# sip_phone: guide for AI agents

A SIP user agent with RTP media.

- `sip_message.*`: SIP message parsing / building; `sdp.*`: offers and answers (codec negotiation, late offers);
  `sip_phone.*`: registration (digest auth), call states, DTMF (RTP events or SIP INFO), MESSAGE, contacts, API actions, triggers.
- RTP in `sip_phone.*`: microphone frames from the resampler microphone through a ring to the signalling task, encoded
  (Opus with the `audio` component's `OpusVoiceEncoder` / `OpusVoiceDecoder`, `g722.*`, `g711.h`) and sent; received
  packets decoded on the same task and played on the speaker after `playout_delay` of silence.
- Threading: SIP signalling and RTP run on one task (`sig_task`, `StaticTask`); the account
  the task works with is a copy (`task_account_`) handed over from the main loop. Triggers and entities are updated
  on the main loop.
- Ports: `sip_port`, `rtp_port`.
- No buffers when idle: media buffers exist only during a call.
