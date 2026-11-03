# Attributions and thanks

This fork stands on the work of many people. Thank you all - without you a lot of this would not exist, or would
have taken far longer to get right.

## ESPHome

First of all, the [ESPHome](https://esphome.io) team, its many contributors and the
[Open Home Foundation](https://www.openhomefoundation.org/): everything here is built on ESPHome, its components,
its code generation and its review culture.

## People

- **Alexey Titov** ([alaltitov](https://github.com/alaltitov)) - `runtime_language` is based on his ESPHome
  [i18n component](https://github.com/alaltitov/esphome) (MIT). The YAML language files, `tr()`, switching the
  language and the check that refuses a language with missing texts come from his component. Thank you for making
  it public.
- **Bascht74** ([Bascht74](https://github.com/Bascht74)) - the virtual UART base, the exclusive UART claim, the
  encrypted noise stream for `tcp_uart` / `uart_tcp`, the runtime key, the idle close and several links and
  connections per port ([esphome-uart-tcp](https://github.com/p1ngb4ck/esphome-uart-tcp)). A great collaboration -
  thank you for all the pull requests and the patience.
- **NonaSuomy** ([NonaSuomy](https://github.com/NonaSuomy)) - many of the ideas behind the Bluetooth and USB HID work,
  among them offloading Bluetooth to the ESP32-C6, parts of `usb_hid`, and his esphomebrew, through which the GSL3680
  touch driver came here. Thank you for the ideas and for sharing your work so openly.
- **Mallow** ([youkorr](https://github.com/youkorr)) - his SD card component was the starting point of `sd_storage`,
  the camera and the dashboard build on his ESP32-P4 work, and the extra OV02C10 resolutions come from his
  esp_video. Thank you for sharing so much ESP32-P4 work early on.
- **Horstexplorer** ([Horstexplorer](https://github.com/Horstexplorer)) - the MIPI-CSI camera component
  (esphome/esphome#19480) that `mipi_csi` is built on.
- **Konstantin** ([kvj](https://github.com/kvj)) - the GSL3680 touch driver (`jd9365_gsl3680` in
  [kvj/esphome](https://github.com/kvj/esphome)) that `gsl3680` comes from.
- **Phil Schatzmann** ([pschatzmann](https://github.com/pschatzmann)) - the
  [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) library (Apache-2.0) inside `a2dp_source`.
- **Alex Duchesne** ([ducalex](https://github.com/ducalex)) and the [Retro-Go](https://github.com/ducalex/retro-go)
  contributors - the emulator cores of `emulator` come in their Retro-Go ports.

## Emulator cores

The cores in `esphome/components/emulator/emu` keep their own licence files; thank you to their authors:

| Core | Systems | Authors | Licence |
|---|---|---|---|
| nofrendo | NES | Matthew Conte | GPL-2.0 |
| gnuboy | Game Boy / Color | Laguna, Gilgamesh | GPL-2.0 |
| SMS Plus | Master System, Game Gear | Charles MacDonald (and [smsplus-gx](https://github.com/ekeeke/smsplus-gx)) | GPL-2.0 |
| PCE-GO | PC Engine | Alex Duchesne, based on [HuExpress](https://github.com/kallisti5/huexpress) / Hu-Go! | GPL-2.0 |
| Handy | Lynx | K. Wilkins | zlib-style |
| fMSX | MSX | Marat Fayzullin | fMSX licence (non-commercial) |
| Snes9x | SNES | Gary Henderson, Jerremy Koot and the Snes9x team ([snes9x2010](https://github.com/libretro/snes9x2010)) | Snes9x licence |
| Gwenesis | Mega Drive / Genesis | [bzhxx](https://github.com/bzhxx/gwenesis) | GPL-3.0 |
| LCD-Game-Emulator | Game & Watch | [bzhxx](https://github.com/bzhxx/LCD-Game-Emulator) | GPL-3.0 |

## Libraries and other third-party code

- [CubicleSoft](https://github.com/cubiclesoft) - the [js-fileexplorer](https://github.com/cubiclesoft/js-fileexplorer)
  widget (MIT) of the web server's advanced file browser.
- [Zstandard](https://github.com/facebook/zstd) (Meta Platforms, BSD) - the decoder `store_yaml` uses.
- [Espressif](https://github.com/espressif) - ESP-IDF, the OV02C10 driver and esp_ipa of esp-video-components
  (Apache-2.0), esp-sr's echo cancellation, esp_h264, esp_new_jpeg, ESP-Hosted and elf_loader (the loader of
  `package_manager`'s packages).
- Silead - the GSL3680 firmware and driver notice, kept in `gsl3680`'s files (GPL).
- [usb_modeswitch](https://www.draisberghof.de/usb_modeswitch/) (Josua Dietze) and BlueZ's `hid2hci` - the mode
  switch methods of `usb_host`'s `mode_switch` follow them.
- Telegram - the TL schema of `mtproto`.
- [xkeyboard-config](https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config) (freedesktop.org) - the
  keyboard layouts of `hid` are generated from its symbols files.

## Everyone else

Thank you to the whole ESPHome team and to everyone whose work is in here without being named above - the authors of
the libraries, drivers, datasheets, protocols and examples this builds on, the people who answered questions and
reported problems. If your work is in here and you are missing from this list, please open an issue - it is an
oversight, and it will be fixed.

## Licences

ESPHome's files keep the [ESPHome License](LICENSE) (MIT for Python and everything else, GPLv3 for the C++ runtime
files). New files written for the topic repositories are MIT ([LICENSE-MIT](LICENSE-MIT)). Every third-party part
above keeps the licence its authors chose; its licence file or notice stays next to it.
