# nfs_client: guide for AI agents

NFS v3 exports as `NetworkStorage` + `MountableStorage` (read `esphome/components/storage/AGENTS.md` first).

- `nfs_client.*`: an own ONC RPC / XDR implementation over one lwIP socket per share (`RPCClient`, `XDRBuffer`),
  MOUNT and NFS3 procedures, file handles and attributes, `read_chunk` / `write_chunk` / `truncate`.
- Calls arrive from the main loop, the storage worker's task and consumers' own tasks (an audio reader); a
  `CallLock` serializes them over the one socket and reply buffer. The share reports `IO_TASK_SAFE`.
- The reply buffer lives in PSRAM where there is PSRAM and is decoded in place; keep it that way (no copies).
- `auto_connect` connects once the network is up; reconnects follow network events, not timers.
- A rename never crosses file systems on the server; the storage worker's `move_fallback_copy` covers that case.
