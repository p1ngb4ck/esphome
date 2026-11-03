# firmware_update: guide for AI agents

Main firmware update from an image on a storage, as an OTA component (`OTAComponent`), so the usual OTA triggers
and state apply.

- `firmware_update.*`: optional JSON manifest (`load_manifest()`: version, name, summary, MD5), then the image is
  read through a storage worker stream chunk by chunk (`read_next_()` / `on_chunk_()`) and written through the OTA
  backend; the MD5 is checked at the end. Nothing blocks the main loop; the stream callbacks run on it.
- The flash engine (app slot, partition table, combined images with `allow_partition_access`) belongs to the `ota`
  component and is linked, not duplicated.
- `automation.h`: `firmware_update.flash` with an optional templated path.
- One update at a time (`is_running()`); a failure reports through the OTA error trigger and leaves the running
  firmware untouched.
