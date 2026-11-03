# Configuration reference

The first steps are in [README.md](README.md). ESP-IDF only; package code runs from PSRAM, so the board's `psram:`
must be configured, and the partition table needs a data partition named `packages`.

## `package_manager`

| Option | Default | Description |
|---|---|---|
| `directory` | - | A storage folder (full path without a trailing `/`, at most 63 characters) for `<name>.cfg` settings files, downloads and `<name>.files`. |
| `max_packages` | `8` | Package slots, 1 to 15. |
| `http_request_id` | - | An `http_request` component: enables repositories (needed by `repositories:`). Loads `noise` for libsodium. |
| `repositories` | - | Up to 8 repositories from the configuration: `name` (1 to 23 characters), `url` (`http://` / `https://`, at most 127 characters), `public_key` (optional, a usign / signify Ed25519 public key: the repository's indexes must be signed with it). |
| `packages` | - | `name: file` (or `name: {file:, depends: [...]}`): YAML documents built as packages during the firmware build. Names: a-z first, then a-z, 0-9, `_`, at most 23 characters. |
| `api` | when `api:` exists | The Home Assistant API actions below. |
| `mqtt` | when `mqtt:` exists | The MQTT interface below. |

### Firmware id

Every compile gets its own firmware id (8 hex digits). A package carries the id of the firmware it was built with
and is installed only into exactly that firmware. Repositories hold one directory per firmware id.

### Actions

| Action | Options | What it does |
|---|---|---|
| `package_manager.load` | - | Starts every enabled installed package. |
| `package_manager.install` | `name`, `source`, `version`, `abi`, `target`, `settings` | Installs the `.ipk` at `source` (full path on any storage). `settings`: `preferences` (default, NVS namespace `pk` + FNV-1 hash of the name) or `storage` (`<name>.cfg` in `directory`). |
| `package_manager.enable` / `package_manager.disable` | `name` | Starts / stops the package and keeps the state. |
| `package_manager.recovery_enable` / `package_manager.recovery_disable` | `name` | Whether the package also starts in safe mode (recovery). |
| `package_manager.remove` | `name` | Stops the package and frees its slot; refused while another installed package needs it. Its settings stay. |

## Repositories

```
index.json                                 every firmware: id, board, device, version, date, packages
<firmware id>/Packages                     opkg index of that firmware's packages
<firmware id>/Packages.gz                  the same, gzip
<firmware id>/Packages.sig                 Ed25519 signature (usign format) of the SHA-256 of Packages
<firmware id>/<name>_<version>_<arch>.ipk  the packages
```

Refresh reads `Packages.sig` (repositories with a `public_key`), then `Packages.gz`, else `Packages`, as it streams
in; only this firmware's entries are kept. An index whose signature does not verify is refused. Install from a
repository downloads the `.ipk` with progress, checks its SHA-256 from the index and installs the packages it needs
first.

Publishing: the device builder publishes after every compile on a host where `~/.config/esphome-packages/` holds
`repository` (the repository's git URL), `deploy_key` (push access) and optionally `signing.sec`: only the packages
of that compile's firmware id go into `<firmware id>/`, signed, then pushed. The same publisher
(`python -m esphome_device_builder.helpers.package_publish`) has `keygen --out NAME` (`NAME.sec`, `NAME.pub`; the
`.pub` line is the repository's `public_key`) and `publish` for other build tools. The repository itself holds only
the packages.

## Interfaces

Every function on each interface:

| Function | HA API action | MQTT `op` | REST |
|---|---|---|---|
| installed list | `package_list` | `list` | `GET /api/packages/list` |
| install (`source` empty: from the repositories) | `package_install` (`source`, `name`, `package_version`, `abi`, `target`) | `install` | `POST /api/packages/install` |
| enable / disable / remove | `package_enable` / `package_disable` / `package_remove` (`name`) | `enable` / `disable` / `remove` | `POST /api/packages/<op>` |
| recovery flag | `package_recovery` (`name`, `recovery`) | `recovery` | `POST /api/packages/recovery` |
| hold | `package_hold` (`name`, `hold`) | `hold` / `unhold` | `POST /api/packages/hold`, `unhold` |
| info | `package_info` (`name`) | `info` | `GET /api/packages/info?name=` |
| files | `package_files` (`name`) | `files` | `GET /api/packages/files?name=` |
| find | `package_find` (`query`) | `find` | `GET /api/packages/find?q=` |
| depends / whatdepends / whatdependsrec | `package_depends` / `package_whatdepends` / `package_whatdependsrec` (`name`) | same names | `GET /api/packages/<op>?name=` |
| architecture | `package_architecture` | `architecture` | `GET /api/packages/architecture` |
| available | `package_available` | `available` | `GET /api/packages/available` |
| refresh | `package_refresh` | `refresh` | `POST /api/packages/refresh` |
| repositories | `package_repo_add` (`name`, `url`, `public_key`), `package_repo_delete` (`name`), `package_repo_enable` (`name`, `enabled`) | `repo_add`, `repo_delete`, `repo_enable` | `POST /api/packages/repo_add`, ... |
| download | `package_download` (`name`, `folder`) | `download` | `POST /api/packages/download` |

MQTT: commands as JSON on `<prefix>/package/cmd` (`op`, `name`, the options), results on `<prefix>/package/result`,
the installed list on `<prefix>/package/state`. The HA actions answer with JSON (`return_response`).

## `files`

At install the `.ipk`'s data entries (from its `sha256sums`) are kept with the package's settings (NVS key `.files`,
or `<name>.files` in `directory`) and removed with the package.
