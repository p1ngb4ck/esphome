# esphome-package-manager

Packages for ESPHome: parts of a configuration built together with the firmware as separate packages (opkg `.ipk`),
installed, started, stopped and removed at runtime - from a storage or from online repositories with one package
directory per firmware.

- `package_manager`: the package system. Builds the packages a configuration names into `.ipk` files during the
  firmware build; installs a package into the `packages` flash partition (SHA-256 checked, dependencies first) and
  runs its code from PSRAM; enable / disable / hold / remove at runtime; settings per package in the preferences or a
  file. Repositories: `<url>/<firmware id>/Packages(.gz)`, the firmware id unique per compile (a package matches
  exactly the firmware it was built with), indexes signed with Ed25519 and checked with ESPHome's libsodium.
- Every function on Home Assistant API actions, MQTT, the web server (REST and a card on the version 3 page) and
  YAML actions.

## Getting started

```yaml
external_components:
  - source: github://p1ngb4ck/esphome-package-manager@main
    components: [package_manager, storage, web_server]

http_request:
  id: http

package_manager:
  http_request_id: http
  repositories:
    - name: packages
      url: https://example.org/esphome-packages
      public_key: RWQ...
  packages:
    my_sensor: my_sensor.yaml
```

The partition table needs a data partition named `packages` for the installed code. `packages:` builds every named
YAML document as its own package; the firmware build leaves them as `.ipk` files next to the firmware.

## More

**[CONFIGURATION.md](CONFIGURATION.md)**: every option, action and interface, the repository layout, signing.

## Branches

One branch per change; `main` contains every branch. Base: ESPHome `dev` at `dddacfa239`.

| Branch | Change | Needs | Chips |
|---|---|---|---|
| `pr/package-manager` | `package_manager`: packages built with the firmware (`.ipk`), installed into the `packages` partition and run from PSRAM; install from a storage or from repositories (per-compile firmware id, `Packages` / `Packages.gz`, Ed25519 signatures, dependencies, download progress); enable, disable, hold, recovery flag, remove; info, files, find, depends / whatdepends, architecture, download; HA API actions and MQTT | `pr/storage` ([esphome-storage-interface](https://github.com/p1ngb4ck/esphome-storage-interface)) | ESP32-S3, ESP32-P4 with PSRAM (ESP-IDF) |
| `pr/package-manager-web` | `web_server`: the package REST interface (`/api/packages/...`) and the Packages card on the version 3 page | `pr/package-manager` | as above |

The package system in other components, one branch per component (`pr/<component>-packages`, each on
`pr/package-manager`): the component as in esphome-testing with the package system's changes - registrations undone
when a package stops (`api`, `lvgl`, `script`, `interval`, `mqtt`, `wifi`, `ethernet`, `network`, `camera`,
`esp32_camera`, `mipi_csi`, `usb_uvc`, `runtime_language`), runtime entity lists (`api`, `web_server`,
`web_server_idf`), one firmware with safe mode (`safe_mode`, `ota`, `esphome`, `esp32`, `esp32_hosted`,
`improv_serial`, `firmware_update`, `http_request`):

| Branch | Component |
|---|---|
| `pr/api-packages` | `api` |
| `pr/lvgl-packages` | `lvgl` |
| `pr/script-packages` | `script` |
| `pr/interval-packages` | `interval` |
| `pr/mqtt-packages` | `mqtt` |
| `pr/wifi-packages` | `wifi` |
| `pr/ethernet-packages` | `ethernet` |
| `pr/network-packages` | `network` |
| `pr/camera-packages` | `camera` |
| `pr/esp32-camera-packages` | `esp32_camera` |
| `pr/mipi-csi-packages` | `mipi_csi` |
| `pr/usb-uvc-packages` | `usb_uvc` |
| `pr/runtime-language-packages` | `runtime_language` |
| `pr/web-server-packages` | `web_server` |
| `pr/web-server-idf-packages` | `web_server_idf` |
| `pr/esp32-packages` | `esp32` |
| `pr/esp32-hosted-packages` | `esp32_hosted` |
| `pr/improv-serial-packages` | `improv_serial` |
| `pr/firmware-update-packages` | `firmware_update` |
| `pr/safe-mode-packages` | `safe_mode` |
| `pr/ota-packages` | `ota` |
| `pr/esphome-packages` | `esphome` (OTA platform) |
| `pr/http-request-packages` | `http_request` |

The core changes the package system also uses (`esphome/core`, runtime entity lists and undoable registrations)
are in esphome-testing; `external_components` brings components only.

All topics together: `github://p1ngb4ck/esphome@dev`. `external_components` replaces components only; the ESPHome
core of the build host stays as installed.

## License

- ESPHome's own files, also where this repository changes them: the [ESPHome License](LICENSE) (MIT for the Python
  code and everything else, GPLv3 for the C++ runtime files).
- New files written for this repository (new components, their code, tests and documentation): MIT,
  [LICENSE-MIT](LICENSE-MIT).

## Thanks

Package code is loaded through Espressif's elf_loader (Apache-2.0); signatures are checked with libsodium (ISC).
Everyone this project builds on: [ATTRIBUTIONS.md](https://github.com/p1ngb4ck/esphome/blob/dev/ATTRIBUTIONS.md).
