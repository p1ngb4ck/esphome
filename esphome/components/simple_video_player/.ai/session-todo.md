# Session order (Olli, 2026-10-17, given before he went to sleep) — binding

Read this file, README.md, architecture.md, open-issues.md and CLAUDE.md FIRST after every
compaction, before any other step. Rules are laws, not options. Do NOTHING else than the three items
below, invent no new architecture, keep to every svp rule (README section 3 and 4, CLAUDE.md).

Olli's words: "Das so umsetzen ja. Danach die beiden letzten Punkte MEINER Liste: Status lokaler
Videos fürs Relay: echte Lücke. Audiodateien: Dauer, Seek, Start- und Ende-Events, als ein
Arbeitspaket. So erschöpfend und sauber implementieren wie du kannst, während ich abwesend bin
(schlafen) - bitte NICHTS anderes machen und keine Architektur neu erfinden dabei und an alle
skills / knowledgebase und regeln zu svp halten bitte. Insbesondere sicherstellen, dass kein Compact
diese Aufgabenstellung aufweicht und jeder Compact zuerst die SVP Skills und Regeln liest. Regeln
sind Gesetze, keine Optionen!"

1. Video controls bar (DSI direct), C2 a only: title and duration of the next playlist item without
   LVGL. Agreed design: the bar's snapshots are taken without title and duration text; a copy of
   the empty background under both fields is kept in `ctl_mem_` (its share grows by that, no new
   buffer); per item the main loop restores the background and writes title and duration straight
   from the ESPHome font's glyph bitmaps (`esphome::font::Font` via `lv_font_t::dsc`,
   `find_glyph`), in the label's text colour; one line, cut at the field's end. The Font* and the
   field areas are taken once while LVGL runs. Position digits, buttons, progress stay as they are.
   C2 b, c, d are NOT part of this order.
2. E7: the status JSON reports a local video / playlist to the relay (a real gap).
3. E2: audio files: duration, seek, on_playback_started/finished for their sessions — one piece
   of work.

Never: compile, run checks/tests, review agents, own branches, amend, trailers. Commit each item on
its own (`git -c user.name=p1ngb4ck -c user.email=kleinecke.oliver@googlemail.com commit --no-verify`,
push origin testing_dev), docs updated in the same commit. Changed device YAMLs: send as files.
No ESP_LOG in hot paths, no blocking waits there, no LVGL while paused, event-driven, PSRAM pool
only, no buffer beyond the agreed one. Stop-hook messages: answer only ".".

Status: (update as items finish)
