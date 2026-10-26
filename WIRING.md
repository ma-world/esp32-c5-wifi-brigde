# Wiring: Waveshare ESP32-C5-WIFI6-KIT ↔ W5500

The GPIO numbers refer to the labels on the **Waveshare board**. Connect the W5500 module by its **signal labels**; ARCELI and WIZ850io-compatible boards may arrange their headers differently.

| Waveshare ESP32-C5 | W5500 module | Purpose |
|---|---|---|
| `3V3` | `3.3V` / `VCC`* | Power |
| `GND` | `GND` | Common ground |
| `GPIO8` | `SCLK` / `SCK` | SPI clock |
| `GPIO10` | `MOSI` | Data from C5 to W5500 |
| `GPIO9` | `MISO` | Data from W5500 to C5 |
| `GPIO3` | `CS` / `SCSn` | Chip select |
| `GPIO4` | `INT` / `INTn` | Interrupt |
| `GPIO24` | `RST` / `RSTn` | Reset |
| W5500 RJ45 | UniFi Express **WAN** | Ethernet cable |

`*` **Before applying power**, check the supply label on your exact W5500 module. Connect a pin marked `3.3V` only to 3.3 V. If the module explicitly has a regulated `5V` input, follow its documentation. Never apply 5 V directly to a C5 GPIO. Both boards must share ground.

~~~text
Hotel Wi-Fi (2.4 or 5 GHz)
           ))))
   Waveshare ESP32-C5
       │  SPI + 3.3 V + GND
       ▼
   W5500 module ── RJ45 cable ──► UniFi Express WAN
~~~

GPIO8, GPIO9 and GPIO10 are next to each other on the left Waveshare header. GPIO3 is farther up on the left; GPIO4 and GPIO24 are on the right. **BOOT** is the board's built-in button on GPIO28 and needs no extra wire. Waveshare uses GPIO6 and GPIO15 on the board, so this project does not use them. Configure Wi-Fi through the `Wifi2Eth_XXXXXX` hotspot with the current test password `esp32_eth` at **http://192.168.44.1/**.

Keep the wires short for the first build. Power the C5 from a stable 5 V USB-C supply. The W5500 draws power from the 3.3 V pin; Waveshare specifies a maximum of 2 A for the 3.3 V regulator, but the usable margin depends on the board and supply. If the connection is unstable, measure the supply voltage and voltage drop.

## Serial debug without resetting the board

The firmware logs to UART0 at **115200 baud, 8N1**. For a read-only debug connection, two wires are enough:

| Waveshare ESP32-C5 | USB-to-TTL adapter |
|---|---|
| `GPIO11` / `U0TXD` | `RX` |
| `GND` | `GND` |

Adapter `TX` to `GPIO12` / `U0RXD` is needed only for input and can be left disconnected for debugging. **Do not connect VCC/5V, DTR or RTS.** Use an adapter with **3.3 V TTL levels** and continue powering the C5 through USB-C. This avoids a reset through EN/BOOT when opening the adapter. Do not save Wi-Fi credentials or full packet captures in debug logs.

Sources: [Waveshare pinout](https://docs.waveshare.com/ESP32-C5-WIFI6-KIT), [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-C5-WIFI6-KIT-NXRX/ESP32-C5-WIFI6-KIT-NXRX-Schematic.pdf), [ESP-IDF Ethernet initialization](https://github.com/espressif/esp-idf/blob/v5.5.3/examples/ethernet/basic/components/ethernet_init/Kconfig.projbuild).
