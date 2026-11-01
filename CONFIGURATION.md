# Configuration reference

The first steps are in [README.md](README.md). ESP-IDF only.

## `cert_store`

| Option | Default | Description |
|---|---|---|
| `directory` | - | A storage folder with certificates (`/sdcard/certs`). Named certificates are looked up there first. |
| `relay` | `false` | Look named certificates up in Home Assistant through the relay link (`/cert?name=<name>`). Needs `relay_link` ([esphome-relay-link](https://github.com/p1ngb4ck/esphome-relay-link)). |
| `http_sources` | `[]` | Up to 4 base URLs of web servers asked for named certificates (`<url>/cert?name=<name>`), in order. Needs `relay_link`. |

`cert_store:` also turns on ESP-IDF's certificate bundle (with SHA-384 / SHA-512 certificates).

## TLS options of a client

Every client that takes certificates from cert_store (`ftp` clients with `auth_tls: true`, `s3_client` with
`tls: true`) has these options:

| Option | Description |
|---|---|
| `ca` | The CA certificate (PEM) that signs the server's certificate. Without it the built-in CA bundle is used. |
| `client_certificate` | The device's certificate (PEM) for mutual TLS. Together with `client_certificate_key`. |
| `client_certificate_key` | The private key (PEM) of the client certificate. |

Each value is one of:

| Value | Meaning |
|---|---|
| `/sdcard/certs/ca.pem` (starts with `/`) | A file on a storage, read when the connection is set up. Replace the file, no new build. |
| `certs/ca.pem` (contains `/` or `.`) | A file next to the YAML, built into the firmware. Must exist at build time. |
| `home-ca` (a plain name, no `/` or `.`) | Looked up in order: the file `<directory>/<name>`, Home Assistant (`relay: true`), each of `http_sources`. |

A certificate is at most 10 KB. It is read into a PSRAM buffer that is freed as soon as it is parsed.

## Examples

Mutual TLS with files on the SD card:

```yaml
cert_store:

s3_client:
  - endpoint: s3.example.com
    bucket: device-backups
    access_key: !secret s3_access_key
    secret_key: !secret s3_secret_key
    mount_path: /s3
    ca: /sdcard/certs/ca.pem
    client_certificate: /sdcard/certs/device.pem
    client_certificate_key: /sdcard/certs/device.key
```

Names resolved by a folder, then Home Assistant, then a web server:

```yaml
cert_store:
  directory: /sdcard/certs
  relay: true
  http_sources:
    - http://certs.local:8080

ftp:
  clients:
    - server: nas.local
      username: !secret ftp_user
      password: !secret ftp_password
      mount_path: /ftp
      auth_tls: true
      ca: home-ca
```

A public server with a Let's Encrypt certificate needs no `ca` at all:

```yaml
s3_client:
  - endpoint: s3.amazonaws.com
    region: eu-central-1
    bucket: my-bucket
    access_key: !secret s3_access_key
    secret_key: !secret s3_secret_key
    mount_path: /s3
```
