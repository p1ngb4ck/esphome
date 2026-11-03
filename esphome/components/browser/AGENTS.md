# browser: guide for AI agents

A browser app as a thin layer on `remote_desktop`: `open()` connects the remote desktop to the configured browser
server and remembers the session number; `owns_session()` / `is_open()` compare it with the remote desktop's current
session, so closing the browser never ends a session someone else opened. The object lives in PSRAM. All
rendering, input and protocol work is `remote_desktop`'s - add features there, not here.
