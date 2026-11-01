# boot_announce: guide for AI agents

Tells a waiting receiver that the device is back after a restart (OTA, crash, power cycle) - instead of the receiver
guessing with timeouts.

- `watch()` (action `boot_announce.watch`) sets a mark in RTC memory, kept over a restart, a crash and an OTA
  reboot. Only a boot with that mark announces.
- Sending starts on the network component's active-interface event (or wifi / ethernet IP events with one
  interface); the packet carries the boot's facts and is resent by the scheduler only until the receiver's ack
  arrives on the `udp` component's listen socket. No loop, no polling.
- The `udp` component creates only the sockets its users request; `to_code()` requests broadcast and listen.
