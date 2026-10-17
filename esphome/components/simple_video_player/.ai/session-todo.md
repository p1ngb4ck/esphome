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

Olli's additions to item 1 (same night): "zur hintergrund kopie: die ist in PSRam ? der existierende
verwaltete Puffer wird minimal größer ? - mach es so" / "nein kein Zeilenumbruch. achte eher darauf
nicht den gesamten Pfad, sondern nur den Dateinamen - oder besser gar - wenn verfügbar - den Titel
aus den Metadaten der Datei anzuzeigen" / "keine erlaubnisse abfragen ich bin jetzt abwesend".
So: one line, cut with dots; the title from the file's metadata (AVI LIST INFO INAM) when it has
one, else the file name only. Ask nothing, decide and note the decision here and in the summary.

Found while doing item 1 (decision, to report): the bar's font on Olli's device is montserrat_16
(LVGL's built-in fmt_txt font, not an esphome::font::Font), so the glyphs are read through LVGL's
font functions (`lv_font_get_glyph_dsc`, `lv_font_get_glyph_bitmap`: font data only, no widget,
no display), which serve both kinds of font.

Status: (update as items finish)
- 1 done (commit "svp: video bar title and duration without LVGL"), awaiting Olli's build/test.
- 2 done (status "video", relay 0.9.46 zip sent), awaiting Olli's build/test.
- 3 done (audio file duration/position/seek, started/finished events), awaiting Olli's build/test. All three items done: nothing else to do this session.
