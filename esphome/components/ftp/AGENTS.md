# ftp: guide for AI agents

FTP / FTPS / TFTP clients as network storages, and an FTP server (with a TFTP server) that serves a local storage.
Read `esphome/components/storage/AGENTS.md` first.

Files:
- `ftp_client.*`: `FTPClient` (`NetworkStorage` + `MountableStorage`), control and data connections (passive mode),
  `FtpStream` for transfers. FTPS (`USE_FTP_CLIENT_TLS`) uses cert_store's `TlsRefs` / `apply_async()` for the CA
  and client certificate; without TLS nothing of cert_store is linked.
- `tftp_client.cpp`: the TFTP protocol of the same client class (`USE_FTP_TFTP_CLIENT`, ESP32, BSD sockets).
- `ftp_server.*`: `FtpServer` (`USE_FTP_SERVER`, ESP32) and the TFTP server (`USE_FTP_TFTP_SERVER`).

Threading and memory:
- Clients report `IO_TASK_SAFE`; their blocking socket I/O runs on the storage worker's task or a consumer's task,
  never on the main loop.
- The server runs on one `StaticTask` (stack in PSRAM) with `select()` over its sessions and waits on task
  notifications; it never polls. File data goes through storage worker streams; metadata operations (stat, list,
  mkdir, delete, rename) are handed to the main loop with `defer()` and the task waits for their result. Directory
  listings are collected into a PSRAM vector (`RAMAllocator`).
- `max_sessions` (1-4) bounds the per-session state; nothing grows with traffic.
- Users and their access (`read_only` / `read_write`) are checked per command; paths are resolved inside `root` and
  never leave it.
