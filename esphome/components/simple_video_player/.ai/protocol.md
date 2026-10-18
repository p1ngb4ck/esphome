# Panel <-> svp_relay protocol (svp_relay 0.9.36, firmware 6a5877bba8)

`P/` = this component, `R/` = svp_relay `custom_components/svp_relay/` (not in this repo).

## 1. Stream port (TCP, `stream_port`, device 5000)

- One listener, backlog 1; one sender at a time, served to the end before the next accept
  (P/stream_receiver.cpp:277-298). `TCP_NODELAY`, `SO_KEEPALIVE`.
- While a desktop session runs, only the relay's address is admitted, and only when the panel asked
  for a desktop sender (R:306-313).
- On accept the panel sends 'C','V','A','L','N','F','S' (stream_receiver.cpp:334-350, 754-845).
- First 4 bytes: `"SVPM"` = sender wants 'M' statistics (consumed); `"RIFF"` = AVI, played through
  the file playback (refused while PLAYING/PAUSED or a session is busy after 10 s); anything else
  = udisp.
- AVI end: relay half-closes, the panel plays out its ring, sends 'E' (1 = played to the end) and
  closes. Flow control: the socket isn't read while the ring has < 32 KiB room.
- Panel drops a sender after 30 s of silence (the AVI sender sends no heartbeats).

### udisp header (16 bytes, little-endian `<HBBHHHHI`)

crc16 (ignored) · type u8 · cmd u8 (ignored) · x u16 · y u16 · w u16 · h u16 ·
u32 = frame_id (low 10 bits) | payload size (high 22 bits).

| Type | Relay -> panel |
|---|---|
| 0 | heartbeat (every 10 s) |
| 3 | baseline JPEG rectangle (relay always sends the full picture at 0,0); must fit the output and `stream_max_frame_bytes_`; 4 buffers, waits <=250 ms for one, else skipped; not drawn while a file plays |
| 0x10 | PCM s16 interleaved, w = channels, h = rate; played only while no file plays |
| 0x20 | stats on/off ('M' every second) |
| 0x21 | input on ('K','X','P') |
| 0x22 | echo -> 'R' |
| 0x23 / 0x24 | desktop cursor shape (BGRA) / position, only while a desktop session runs |
| 0x7E | filler (throughput tests) |

### Panel -> relay messages (tag byte + payload)

| Tag | Payload | Meaning |
|---|---|---|
| 'C' | bpp·8 | DSI colour depth |
| 'V' | w:2 h:2 turns:1 still:4 | picture size, quarter turns, max still bytes; resent on output change -> relay refits/restarts |
| 'A' / 'L' / 'N' / 'F' | rate / min-max rate / channels / codec mask (1 PCM, 2 MP3, 4 FLAC) | sound format of the selected output (resent on router switch) |
| 'S' | 0/1 | shown (0 while a file plays or the panel sleeps) |
| 'T' | n × (id, x, y) | touches |
| 'K' | action page:2 usage:2 mods pressed codepoint:4 level3 | a key (after 0x21); codepoint: what the keyboard's layout puts on it now (`HIDInput::layout_char()`, bit 31 a dead key's combining mark, 0 none), level3 1: Right Alt is the layout's AltGr. VNC keysyms from it (relay desktop/keys.py), SPICE scancodes ignore it |
| 'X' / 'P' | codepoint / mouse | input (after 0x21) |
| 'M' | n × u32 | statistics |
| 'R' | seq | echo answer |
| 'E' | 1/0 | AVI playback ended |

## 2. HTTP, panel -> relay (channel task, `http_request` component and its timeout, `/desktop` and `/image` 15 s, Bearer token, <=256 KiB, 200 only)

| Endpoint | Request | Response |
|---|---|---|
| GET `/api/svp_relay/channels?refresh=1` | – | `{"channels":[{name, ref}]}` |
| POST `/play` | `{ref, port}` | `{ok}` |
| POST `/stop` | `{}` | `{ok}` |
| GET `/browse?type=audio\|video\|image[&id=]` | – | `{title, path, parent, items:[{title,id,dir}] (<=500), total}` |
| GET `/image?id&w&h&max` | w,h = display/picture size, max = file capacity | baseline JPEG, downscaled only |
| GET `/cover?port&w&h&max` | canvas size, max 512 KiB | JPEG fitted up or down; empty = none |
| GET/POST `/options?port` | `{port, key, value}` | panel's relay options, among them the keyboard layout sources `keyboard_source` (xkeyboard-config archive URL) and `keyboard_keysyms` (xkbcommon-keysyms.h URL), kept in the panel's entry like the others (also in HA's Settings form; empty = default); GET also `keyboard_status`; POST `keyboard_install` (only the panel triggers it) installs from this panel's sources for all panels and answers once done (panel: 180 s timeout, then the options again) |
| POST `/desktop` | `{port, action:start, kind, host, server_port, password, connection_id}` / `{action:stop}` | answered once the desktop is up |
| GET `/api/svp_relay/audio/<key>.<fmt>` | (media player, URL via `svp_play_audio`) | ffmpeg audio stream |
| POST `/cast` | `{port, action:start, fps, width, height, sound}` / `{port, action:stop}` | `{ok, port, token}`: svp_relay's port for this screencast (one connection, from the panel's address); the panel connects (svp_cast_tx), sends `SVPCAST <token>\n`, then the AVI (MJPEG + PCM, sizes 0, empty chunks for a repeated picture); svp_relay saves it with ffmpeg per the panel's options `cast_dir`, `cast_name`, `cast_format` (mkv/avi copy, mp4 H.264+AAC); stop or the connection's end completes it |
| GET `/keyboard[?layout=<name>]` | – / an XKB name (`de(nodeadkeys)`) | `name\tdescription` lines (<=32 KiB) / the layout's `.kbl` (hid::parse_layout, <=8 KiB); files installed from xkeyboard-config (`keyboard_layouts.py`, `<config>/svp_relay/keyboard`), till then the bundled `keyboard/` (ESPHome's `script/export_hid_layouts.py`) |

## 3. ESPHome services, relay -> panel

`esphome.<device>_svp_play_audio(url, announce)`, `_svp_audio_command(command)`,
`_svp_set_volume(volume)`, `_svp_cover()`, `_svp_stream_statistics(enable)` (never called by the relay).
Status JSON fields read by the relay: `svp`, `port`, `state`, `volume`, `muted`, `pause`, `formats`, `playlist`,
`video` (a video file of the panel's own plays or is paused; relay 0.9.46: no dashboard meanwhile, as for `playlist`).

## 4. Relay transcoding (R/svp_core/avi.py `ffmpeg_args`)

- Video: `yadif`, `scale` to the 'V' upright size (keep aspect, pad), transpose by turns, `fps=panel.fps`
  (default 25), `-c:v mjpeg -q:v panel.quality` (default 7), `yuvj420p`.
- Audio: `-ar link.stream_rate()` within 'L', channels from 'N', codec PCM if 'F'&1 else MP3/FLAC.
- Live inputs: `-probesize 1000000 -analyzeduration 1000000 -flags low_delay`; nothing is sent before
  the first picture; 64 KiB reads + `drain()`.
- Stills: dashboard = Chromium screencast (q80) re-encoded by Pillow to fit `still`; desktop = adaptive
  quality; `/image` downscale only; `/cover` fill.

## 5. Relay state deciding what the panel shows

`_sessions` (AVI), `_dashboards` (Chromium page), `_desktops`, `_art` (cover via /cover),
`_benchmarks`, `_casts` (screencasts taken in), `playlist_active` (status `playlist`). `_busy` = any of
sessions/covers/art/benchmarks/desktops/casts/playlist -> no dashboard. `_covers`/`_show_cover` are dead code since 0.9.36.
