# ESP32-C5 Wi-Fi to Ethernet bridge (proof of concept)

Firmware for a Waveshare **ESP32-C5-WIFI6-KIT** with a W5500 SPI Ethernet module, based on Espressif's [ESP-IDF 5.5.3 sta2eth example](https://github.com/espressif/esp-idf/tree/v5.5.3/examples/network/sta2eth). The Wi-Fi station forwards Ethernet frames to **one** device through the W5500 LAN port. The ESP does not perform NAT. Configure the connected device as a DHCP client.

The setup page is available in **English by default** and can be switched to **German**. It also supports application firmware updates through the browser.

## First installation: USB flash

The initial USB flash installs the [partition table](partitions.csv) with two OTA slots. Future application updates can then be installed through the setup page.

Install **ESP-IDF v5.5.3** with its ESP32-C5 toolchain, activate the IDF environment, then run from this directory:

~~~sh
idf.py build
idf.py -p /dev/cu.YOUR_PORT flash
~~~

The project targets 16 MB flash. The flash operation writes the bootloader, partition table, application and initial OTA metadata separately. It leaves the NVS partition at offset `0x9000` untouched, so saved Wi-Fi settings remain available when reflashing. Do **not** erase the entire flash if you want to preserve them. The factory app is at `0x10000`.

For manual flashing, use the four generated files and their offsets:

~~~sh
python3 -m esptool --chip esp32c5 -p /dev/cu.YOUR_PORT write_flash \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/sta_to_eth.bin \
  0x210000 build/ota_data_initial.bin
~~~

The wiring is documented in [WIRING.md](WIRING.md).

## Automated GitHub releases

Push a Git tag whose name contains the case-sensitive word `Release`, for example:

~~~sh
git tag Release-v1.0.0
git push origin Release-v1.0.0
~~~

The [release workflow](.github/workflows/release.yml) builds the tagged commit with ESP-IDF 5.5.3 and publishes two binary assets on the corresponding GitHub Release:

- `esp32-c5-wifi-brigde-factory.bin`: complete image for USB flashing at offset `0x0`. This overwrites NVS and clears saved Wi-Fi settings.
- `esp32-c5-wifi-brigde-app.bin`: application-only image for the setup page's firmware uploader. It preserves saved Wi-Fi settings and requires the OTA partition table installed by the factory image or an earlier USB flash.

Binary files are release assets; they are not committed to the source repository.

## Wi-Fi setup

1. On first boot, connect to **`Wifi2Eth_XXXXXX`**. The final characters come from the C5 MAC address. The current proof-of-concept hotspot password is **`esp32_eth`**. The hotspot runs on 2.4 GHz.
2. The captive portal should open automatically. Otherwise open **http://192.168.44.1/** in a regular browser. Select **English** or **Deutsch** at the top of the page.
3. The page scans nearby networks and fills the SSID field when you choose one. Use **Scan again** to refresh, or type the SSID for a hidden network. Leave the password empty for an open network.
4. Once the ESP joins Wi-Fi, connect the W5500 **LAN** port to the Ethernet device. Set the device to DHCP so it can obtain an address from the upstream Wi-Fi network.
5. To switch Wi-Fi networks or update firmware, hold the Waveshare **BOOT** button for two seconds. If the saved Wi-Fi cannot be reached, the ESP returns to the setup hotspot after about 10–40 seconds.

## Update firmware in the browser

Connect to the setup hotspot, open **http://192.168.44.1/** in a regular browser, choose the new **application image** (`build/sta_to_eth.bin`) under **Update firmware**, and select **Upload and install**. Wait for the success message and automatic restart. The file must fit the 2 MB OTA slot. Existing Wi-Fi settings are kept. Binary images are generated locally and are not stored in this repository.

Only upload an ESP32-C5 application `.bin` file built for this partition layout. **Do not upload a merged/factory image**, bootloader, or partition table. Browser updates replace the application only; changes to the bootloader or partition table require USB flashing. Keep the ESP powered throughout the upload.

## Current limitations

- The setup page and firmware upload are available **only through the setup hotspot** at `192.168.44.1`. The ESP has no fixed management IP on the RJ45 port while bridging.
- Captive portal popups depend on the phone or laptop. DNS redirection and a DHCP captive portal hint are present, but opening the IP directly is more reliable. HTTPS pages cannot be redirected.
- Exactly **one** Ethernet client is supported. Do not place a switch or multiple devices behind the W5500.
- Hotel captive portals, MAC registration, client isolation or 802.1X may require extra steps or prevent this bridge from working. If needed, open the hotel login page from the connected Ethernet device.
- Setup and firmware upload use local HTTP on the hotspot. Wi-Fi credentials are sent by POST, not in URLs or logs. NVS encryption is not enabled. Anyone with access to the hotspot can upload firmware. The fixed test hotspot password offers limited protection; change it before travel use.
- The firmware stores one SSID. Setting up a new network replaces the previous one.

The Espressif example files are under `Unlicense OR CC0-1.0` (see their source headers). Local changes use the same terms.
