# usb_ethernet: guide for AI agents

USB Ethernet adapters (CDC-ECM, CDC-NCM) as an esp_netif interface. Read `esphome/components/usb_host/AGENTS.md`
and the network topic's `network` changes (interface priority, default route) first.

- `usb_ethernet.*`: control interface (link state notifications, MAC address from the descriptor), data interface
  (bulk IN / OUT, NCM transfer blocks), an esp_netif glue with DHCP or `manual_ip`.
- The interface takes part in `network: priority` like WiFi and Ethernet: the default route and DNS go to the first
  connected interface in that order.
- Received frames are handed from the USB task to lwIP; nothing on the USB task blocks on the network stack.
- Link up / down follow the adapter's notifications, no polling.
