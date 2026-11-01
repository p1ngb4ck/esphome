# Configuration reference

The first steps are in [README.md](README.md). Every driver needs `usb_host:`
([esphome-usb-host](https://github.com/p1ngb4ck/esphome-usb-host/blob/main/CONFIGURATION.md), which also documents
the `vid` / `pid` / `blacklist` options every driver has). With `vid` / `pid` left at `0x0000`, an entry takes any
device of its class.

- [`usb_storage`](#usb_storage) - USB drives
- [`usb_cdrom`](#usb_cdrom) - optical drives
- [`usb_hid`](#usb_hid) - keyboards, mice, gamepads, remotes
- [`usb_uvc`](#usb_uvc) - cameras
- [`usb_bluetooth`](#usb_bluetooth) - Bluetooth dongles
- [`usb_audio`](#usb_audio) - sound cards
- [`usb_uart`](#usb_uart) - serial adapters
- [`usb_ethernet`](#usb_ethernet) - Ethernet adapters

## `usb_storage`

USB mass storage (bulk-only transport, SCSI) as FAT / exFAT storages of the storage interface. Needs `storage:`.

| Option | Default | Description |
|---|---|---|
| `devices` | - | Drives with their own mount path; each `mount_path` (required), `id`, `vid` / `pid` (`0x0000` = any), `format_on_mismatch` (`false`), `file_system` (exFAT builds: `auto` / `fat32` / `exfat`), `on_mounted`. |
| `max_devices` | `4` | Drives at once, 1-9. Drives without an entry get `/usb`, `/usb2`, ... |
| `blacklist` | - | Devices never taken. |

```yaml
usb_storage:
  max_devices: 2
  devices:
    - id: backup_drive
      mount_path: /backup
      vid: 0x0781
      pid: 0x5581
      on_mounted:
        - logger.log: backup drive ready
    - mount_path: /usb
```

Actions and condition (`mount_path` selects the drive, empty = the first):

```yaml
- usb_storage.unmount:
    mount_path: /usb
- usb_storage.remount:
    mount_path: /usb
- usb_storage.list_files:
    mount_path: /usb
    path: /music
- if:
    condition:
      usb_storage.is_mounted:
        mount_path: /usb
    then:
      - logger.log: drive mounted
```

`usb_storage.unmount` (like `storage.unmount`) ejects the drive safely.

## `usb_cdrom`

USB CD / DVD drives (on `usb_storage`) as read-only ISO 9660 storages.

| Option | Default | Description |
|---|---|---|
| `mount_path` | `/cdrom` | Mount point of the first drive (up to 29 characters). |
| `max_drives` | `1` | Drives at once, 1-9. |
| `media_check_interval` | `2s` | How often a drive is asked for a disc while one is plugged in (drives report no events). |
| `on_media_mounted` / `on_media_removed` | - | A disc was mounted / taken out. |

```yaml
usb_cdrom:
  on_media_mounted:
    - logger.log: disc inserted

button:
  - platform: template
    name: Eject
    on_press:
      - usb_cdrom.eject:
          mount_path: /cdrom
```

`usb_cdrom.load` closes the tray.

## `usb_hid`

USB keyboards, mice, gamepads and remotes through the `hid` component; any number at once. A list.

| Option | Default | Description |
|---|---|---|
| `vid` / `pid` | `0x0000` | Pin one device to this entry. |
| `blacklist` | - | Devices never taken. |
| `device_name` | - | A text sensor with the connected device's name. |
| `raw` | - | Devices without a usable report descriptor: raw reports, see below. |

Plus every option of a `hid` input: `layout`, `on_key`, `on_text`, `on_pointer`, `on_control`, ... - see
[esphome-hid](https://github.com/p1ngb4ck/esphome-hid/blob/main/CONFIGURATION.md).

```yaml
usb_hid:
  - id: keyboard
    layout: de(nodeadkeys)
    device_name:
      name: Keyboard
  - id: mouse
```

### `raw`

| Option | Default | Description |
|---|---|---|
| `log_reports` | `false` | Log every report (to find bytes and masks). |
| `on_report` | - | Every report. |
| `buttons` | - | Buttons in the report: `byte`, `mask` (both required), `on_short_press`, `on_long_press`. |
| `long_press_time` | `600ms` | Time for a long press. |

```yaml
usb_hid:
  - vid: 0x1234
    pid: 0x5678
    raw:
      buttons:
        - byte: 2
          mask: 0x01
          on_short_press:
            - logger.log: button 1
```

## `usb_uvc`

USB video class cameras (MJPEG over isochronous streams) as `camera` entities; any number at once.

| Option | Default | Description |
|---|---|---|
| `vid` / `pid` | `0x0000` | Pin one camera to this entry. |
| `format` | `MJPEG` | `MJPEG`, `UNCOMPRESSED` or `FRAME_BASED`. |
| `width` / `height` / `fps` | `0` (the camera's best) | Requested resolution and frame rate. |
| `num_urbs` | `3` | Isochronous transfers in flight, 1-8. |
| `camera` | - | The camera entity (`name`, ...) with `encoder`: `jpeg` (default) or `h264` (ESP32-P4: `bitrate` 2000000, `gop` 30, `fps` 30). |
| `lvgl_id` | - | ESP32-P4: show the frames through LVGL's video output (needs `video_output:` in `lvgl:`, esphome-esp-video). |
| `display_id` | - | The display for the video output. |

```yaml
usb_uvc:
  id: usb_cam
  width: 1280
  height: 720
  camera:
    name: USB camera
    encoder:
      type: h264
      bitrate: 4000000
```

## `usb_bluetooth`

A Bluetooth dongle as HCI controller.

| Option | Default | Description |
|---|---|---|
| `vid` / `pid` | `0x0000` | The dongle. |
| `bluedroid` | `false` | Run ESP-IDF's Bluedroid host on the dongle (Classic and BLE on the dongle's radio). |
| `c6_offload` | `false` | ESP32-P4 with an ESP32-C6 (`esp32_hosted`): the C6 runs the host stack for the dongle. Exclusive with `bluedroid`. |
| `firmware` | - | Realtek firmware file next to the YAML, downloaded to the dongle at start (checked at build time). |
| `firmware_config` | - | The matching Realtek config file; needs `firmware`. |

```yaml
usb_bluetooth:
  id: usb_bt
  vid: 0x3625
  pid: 0x010B
  firmware: rtl8851bu_fw.bin
  bluedroid: true
```

Chips with Bluetooth on chip (ESP32, ESP32-S31) need no dongle; a dongle can still be used there as an external
radio. The Bluetooth components that use it are in
[esphome-bluetooth](https://github.com/p1ngb4ck/esphome-bluetooth).

## `usb_audio`

USB Audio Class devices: speakers, microphones, volume, media player. Several devices; each in a format it offers.

| Option | Default | Description |
|---|---|---|
| `vid` / `pid` | `0x0000` | Pin one device. |
| `speaker` | 48000 Hz, 16 bit, 2 ch | Wanted playback format: `sample_rate` (8000-96000), `bits_per_sample`, `num_channels` (1-8). The nearest format the device offers is used. |
| `microphone` | 16000 Hz, 16 bit, 1 ch | Wanted capture format, same options. |
| `speaker_buffer_size` / `microphone_buffer_size` | from the format | Ring buffer sizes in bytes. |
| `channel_pair` | `front` | Which pair of a multichannel device stereo plays on: `front`, `center_lfe`, `back`, `side`. |
| `feedback` | `true` | Use the device's feedback endpoint (asynchronous devices) to pace playback. |
| `stream_idle_timeout` | `30s` | Close the stream after this long without audio (the device can sleep). |
| `volume_curve` | `linear` | `linear` or `logarithmic` mapping of the volume to the device's range. |
| `device_quirks` | - | Per device (`vid`, `pid`): `volume_encoding` `db` (default) or `byte` for devices that take the volume as a raw byte. |

Platforms (all take `usb_audio_id`):

| Platform | Options |
|---|---|
| `speaker` | `sample_rate` (48000), `bits_per_sample` (16bit), `num_channels` (2) |
| `microphone` | `sample_rate` (16000), `bits_per_sample` (16bit), `num_channels` (1), `channels` |
| `audio_dac` | the device's volume and mute |
| `media_player` | the options of ESPHome's speaker media player (`announcement_pipeline` required, `media_pipeline`, `buffer_size` 1000000, `codec_support_enabled`, `files`, `task_stack_in_psram`, `volume_*`, `on_mute` / `on_unmute` / `on_volume`) |

```yaml
usb_audio:
  - id: usb_sound
    speaker:
      sample_rate: 48000
      num_channels: 2
    stream_idle_timeout: 30s

speaker:
  - platform: usb_audio
    id: usb_speaker
    usb_audio_id: usb_sound

microphone:
  - platform: usb_audio
    id: usb_mic
    usb_audio_id: usb_sound

audio_dac:
  - platform: usb_audio
    usb_audio_id: usb_sound
```

## `usb_uart`

USB serial adapters as `uart` buses. One client serves every adapter; an adapter without an entry gets its type
from its `vid` / `pid`, else CDC ACM.

`devices` is a list; `type` selects the chip (and its default `vid` / `pid`):

| Type | Channels | Max baud |
|---|---|---|
| `CDC_ACM`, `ESP_JTAG`, `STM32_VCP` | 1 | - (baud rate optional) |
| `CH340` / `CH34X` | 1 / 4 | 2 000 000 |
| `CH9344`, `CH9344L`, `CH9344Q` | 4 | |
| `CH348`, `CH348L`, `CH348Q` | 8 | |
| `CP210X` | 3 | 2 000 000 |
| `FT232` / `FT2232` / `FT4232` | 1 / 2 / 4 | 3 000 000 / 12 000 000 |
| `PL2303`, `PL2303GB` / `GC` / `GE` / `GL` / `GS` / `GT` | 1 | 6 000 000 |

Per device: `vid`, `pid`, `channels` (required), and for CDC types `claim_comm_interface` (`true`). Per channel:

| Option | Default | Description |
|---|---|---|
| `id` | - | The UART ID other components use. |
| `baud_rate` | required (CDC types: 9600) | 300 up to the chip's maximum; changeable at runtime. |
| `data_bits` / `parity` / `stop_bits` | `8` / `NONE` / `1` | Line settings. |
| `buffer_size` | `256` | RX buffer, 64-8192 bytes; allocated on connect, freed after the device is gone. |
| `flush_timeout` | `100ms` | How long `flush()` waits. |
| `dummy_receiver` | `false` | Read and drop incoming data (keeps the adapter from stalling). |
| `claim_notification_ep` | `false` | Also claim the notification endpoint (modem status). |
| `debug` / `debug_prefix` / `debug_add_uart_settings` | `false` / `""` / `false` | Log the traffic; with the line settings in each line. |

```yaml
usb_uart:
  devices:
    - type: CH348
      channels:
        - id: port1
          baud_rate: 115200
        - id: port2
          baud_rate: 9600
    - type: CDC_ACM
      channels:
        - id: usb_serial
```

## `usb_ethernet`

USB Ethernet adapters (CDC-ECM, CDC-NCM) as a network interface beside WiFi and Ethernet (order:
`network: priority`, esphome-network).

| Option | Default | Description |
|---|---|---|
| `vid` / `pid` | `0x0000` | Pin one adapter. |
| `manual_ip` | DHCP | `static_ip`, `gateway`, `subnet` (required), `dns1`, `dns2`. |

```yaml
usb_ethernet:
  manual_ip:
    static_ip: 192.168.1.50
    gateway: 192.168.1.1
    subnet: 255.255.255.0
```
