# store_yaml: guide for AI agents

Embeds the device's YAML (zstd-compressed) in the firmware and writes it back to a storage on request.

- `__init__.py` / `yaml_discovery.py`: collect the configuration with its included files, replace `!secret` values
  by a fill-in skeleton unless `include_secrets`, compress at build time into a flash blob (`raw_data_id`).
- `store_yaml.*`, `store_yaml_export.cpp`: decompression with the bundled Zstandard decoder (`zstddeclib.c`,
  unmodified) in chunks to a storage file; `raw: true` writes the compressed blob as it is.
- Security: with an API without encryption the build is refused unless `allow_unencrypted`, since the YAML can hold
  credentials.
- The blob stays in flash; decompression buffers are allocated for the export and freed afterwards.
