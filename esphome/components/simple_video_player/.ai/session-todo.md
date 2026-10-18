# Session order (Olli, 2026-10-18, before he went to sleep) — binding

Olli: "only that fs/ls issue suxx really. while I go to sleep. please stick on that issue - it is
absolutely for sure the fs/ls translation if device is on a hub". His facts, never questioned: not
power (USB audio speaker works fine); not the USB SD card reader with its single hub port; keyboards
work fine; NonaSuomy did not use a hub; the issue is precisely the LS-FS translation in the hub layer.
Only this issue; no compiles/tests; commits as usual.

Status: (update as work finishes)
- usb_host 45997a9633: a pipe halt that ends in a channel error now wakes its waiter (was: the task
  halting the pipe waited for good; an unplugged LS device behind the hub hits it through the
  interrupt retry; usb_hid halts its IN endpoints on the main loop). waiting_halt is cleared too.
- usb_host e654bab510: LS control transfer picks its frame before taking the hold (the hold had
  emptied the frame list the check reads).
- Finding, not changed (Olli decides): both root port losses in the last log happen while usb_hid
  reads interface 1's report descriptor and the hub's own port request for its next port is on
  the bus. ext_port handles one port at a time and starts the next one when the keyboard's
  enumeration ends, which is exactly when usb_hid starts its control transfers. Options: the
  hold also keeps the hub's control requests back while an LS control transfer holds the bus;
  or all hub ports powered at hub configuration before any port is handled.

---

# Session order (Olli, 2026-10-17, evening) — binding

Inputs and settings, in short steps Olli tests one by one; design approved in the design artifact
("ESP32-P4 Panel UI Design": Settings, SetMice, SetRemotes, SetGamepads, SetGamepad, SetGestures).
Olli: "please keep current design mostly, just "add" the new settings submenu entries and -pages".

Olli's list:
1. Keyboards section lists all HID devices, should list keyboards only. (done, e9b53f823e)
2. Bluetooth disconnect icon wrong. (done, lvgl_media.yaml)
3. Allowed paired BT devices: as many as possible (paired up to max_devices, Classic HID slots
   shared). (done, 0a292aeadc, 4b111c82aa)
4. Mice and remote controls: settings pages of their own.
5. Remote controls: pointer mode, with a configurable mode key.
6. Gamepads section with test functions. Mice, remote controls and gamepads list their devices as
   sub-entries under the section (Olli: keyboards keep their current page, no sub-entry per keyboard).
7. Key bindings for every input that makes sense (gamepad buttons, ...).
8. Gestures / swipe control next to key bindings ("general" gesture/touch/pad/analog settings).
9. Long-pressed direction keys speed up navigation/selection/change, with sane settings and a max.
10. Gamepad touchpads usable as display touch or for swiping (excluding each other).
11. Gamepad test page: a drawing per connected controller (Xbox, PlayStation, Steam, a common one
    for the rest), in the style of their calibration websites.

Steps: a) Settings nav: Mice, Remote controls, Gamepads (sections 13-15) with their devices as
sub-entries. b) Gamepads pages. c) Mice page. d) Remote controls page. e) Key bindings for any
input, hold-to-speed-up. f) Gestures and touch page.

Status:
- a) done (lvgl_media.yaml), awaiting Olli's test. The device lists under the sections are made by
  settings_show (no empty containers in the YAML).
- b) Gamepads pages (lvgl_media.yaml; hid: get_gamepad_state/get_gamepad_driver/device_input):
  overview with a drawing per connected controller, one controller's page (drawing, sticks,
  triggers, player light, light bar, Test rumble). Touchpad mode comes with item 10. Mice and
  Remote controls pages list their devices; their settings are steps c and d. Awaiting Olli's test.
- c) Mice: pointer speed, wheel lines, acceleration, reverse wheel (lvgl 2272faf7cb; entities
  mouse_speed, mouse_wheel_lines, mouse_acceleration, mouse_reverse_wheel). Awaiting Olli's test.
- d) Remote controls: pointer mode (lvgl 7d098f19c3; key action pointer_mode, default MENU;
  remote_pointer_step, remote_hold_after, remote_hold_most). Awaiting Olli's test.
- Gamepad page (2026-10-18): DualSense picture with shape overlays (L2/R2 fill, Create/Options,
  touchpad, mute), light bar colour picker; touchpad fingers (hid 4aa4b8b1f2, controls
  FINGER1_X.. 0..65535); Details box: connection, firmware, signal (read on opening), battery (hid
  43bf2fac7a); the test page captures the pad, combo key bindings, action leave_gamepad_test
  (L1+R1+Options; lvgl 0ea294a0e8); touchpad as the display's touch, select "Gamepad touchpad"
  (lvgl 91835c4cc1). Open: touchpad "for swiping" waits for step f (Gestures); per-pad touchpad
  setting (the design's "unless a gamepad sets its own") with it. Awaiting Olli's test.

---

# Previous session order (all done)

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
