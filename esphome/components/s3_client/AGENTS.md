# s3_client: guide for AI agents

An S3 bucket as a `NetworkStorage` + `MountableStorage` (read `esphome/components/storage/AGENTS.md` first).

- `s3_client.*`: `S3Client` maps the storage calls to S3 requests, signed with AWS Signature V4. Folders are key
  prefixes; `base_path` is a prefix inside the bucket.
- `S3Connection`: one TCP connection, optionally TLS (`USE_S3_CLIENT_TLS`, mbedTLS with cert_store material), kept
  open across requests (keep-alive); a saved TLS session lets a reconnect resume instead of a full handshake.
- A written file is uploaded when the storage interface reports it complete (`write_done()`).
- Reports `IO_TASK_SAFE`: requests run on the storage worker's task or a consumer's task. Request / response
  buffers come from PSRAM (`RAMAllocator`) for the duration of one request and are given back.
- `endpoint` is a bare host; scheme and path are rejected at validation (`base_path` takes a proxy prefix).
