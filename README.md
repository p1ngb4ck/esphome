# esphome-cert-store

`cert_store`: TLS certificates for every TLS client of a device from one place. A client names its CA, client
certificate and key; each can be a file built into the firmware, a file on a storage (replaceable without a new
build), or a name that is looked up in a certificate folder, in Home Assistant (through the relay link) or on HTTP
servers. Without a CA the built-in Mozilla CA bundle is used, which covers public CAs such as Let's Encrypt.

Used by `ftp` (FTPS) and `s3_client` in
[esphome-storage-drivers](https://github.com/p1ngb4ck/esphome-storage-drivers).

## Getting started

A private CA for an FTPS server, from a file next to the YAML:

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-cert-store@main
    components: [cert_store, storage]
  - source: github://p1ngb4ck/esphome-storage-drivers@main
    components: [ftp]

cert_store:

ftp:
  clients:
    - server: nas.local
      username: !secret ftp_user
      password: !secret ftp_password
      mount_path: /ftp
      auth_tls: true
      ca: certs/home-ca.pem
```

Certificates kept on the SD card instead, looked up by name (the file `/sdcard/certs/home-ca`):

```yaml
cert_store:
  directory: /sdcard/certs

ftp:
  clients:
    - server: nas.local
      username: !secret ftp_user
      password: !secret ftp_password
      mount_path: /ftp
      auth_tls: true
      ca: home-ca
```

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option, the lookup order, mutual TLS, certificates from Home
Assistant and HTTP servers.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `1f70f86b47`.

| Branch | Change | Chips | Needs |
|---|---|---|---|
| `pr/cert-store` | `cert_store`: CA, client certificate and key per TLS client - embedded, from a storage, or by name from a folder, Home Assistant or HTTP sources; the CA bundle as fallback | ESP32 (ESP-IDF) | `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)), `pr/esp32-cert-bundle-sha512` ([esphome-esp32-platform](https://github.com/p1ngb4ck/esphome-esp32-platform)) |

`relay` and `http_sources` also need `relay_link` from
[esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link).

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
