# runtime_language: guide for AI agents

The UI language at runtime. Based on Alexey Titov's ESPHome i18n component (file format, `tr()`, the completeness
check); the runtime is this component's own.

Build time (`__init__.py`):
- `register_text(value)` (called by `lvgl/widgets/label.py` for static label texts) and the `runtime_language::tr("...")`
  literals found in lambdas collect every English text; web_server adds the entity names.
- One sorted key table in flash (key id = position) is emitted; nothing of it is in RAM.

Runtime (`runtime_language.*`):
- A language file is read through storage worker streams (or fetched through the relay link's task) into one PSRAM
  block (`max_file_size`); each line's key is looked up in the flash table; an index of offsets (4 bytes
  per key) lives in the same block. A file that misses a key of the build is refused and the old language stays.
- `tr()` returns a pointer into that block (or the English text) - no allocation per call.
- Labels bound to their English text are updated together on a change (`add_on_change_callback`), on the main loop.
- `write_template` streams the template through the borrowed transfer buffer in chunks, deferred between chunks.
- The chosen language is saved in the preferences and restored at boot.
