# PSP Router

[Русский](README.ru.md)

ESP-IDF firmware for **Super Mini ESP32-S3 HW-747**: share a computer's internet
connection over USB with a PSP over Wi-Fi. No additional hardware modules required.
Target hardware: **ESP32-S3FH4R2, 4 MB Flash, 2 MB QSPI PSRAM**; native USB-C
uses GPIO19 (D−) and GPIO20 (D+). The firmware checks memory sizes before starting Wi-Fi.

## Modes

| Build mode | Network | PSP browser proxy |
| --- | --- | --- |
| `router` | USB WAN gets DHCP from the computer; Wi-Fi uses a separate subnet with DHCP, DNS and NAT. | `192.168.50.1:8080` by default |
| `bridge` | Computer assigns ESP and PSP addresses in the same subnet. | None |
| `bridge-proxy` | Same transparent bridge, with an HTTP→HTTPS gateway. | `psp-proxy:8080` |

In router mode the LAN defaults to `192.168.50.0/24` and changes if it overlaps
the USB subnet. Use the PSP's assigned gateway as the proxy address in that case.
In bridge-proxy mode, ESP resolves `psp-proxy` for Wi-Fi clients to its current
DHCP address. Use the name without `http://` or `.local`; it may not resolve on the computer.

## Build

Pinned dependencies:

- ESP-IDF **v6.1-dev-4811-gfce70533fa**, commit `fce70533fa1fdeef9f903ce5e87ff8d292be992b`.
- `espressif/esp_tinyusb` **2.0.1~1** and `espressif/tinyusb` **0.21.0~4**.

Versions are enforced by CMake, [idf_component.yml](main/idf_component.yml)
and [dependencies.lock](dependencies.lock).

```sh
. "$HOME/Work/ESP/esp-idf/export.sh"
sh tools/build.sh ecm wpa2 bridge-proxy  # macOS/Linux, bridge + proxy
sh tools/build.sh rndis wpa2 bridge-proxy # Windows, bridge + proxy
sh tools/build.sh ecm wpa2 router        # separate WAN/LAN + NAT + proxy
sh tools/build.sh ecm wpa1 bridge        # bridge for stock PSP firmware
```

Arguments: `[ecm|rndis] [wpa2|wpa1|open] [router|bridge|bridge-proxy]`.
Defaults: `rndis wpa2 router`. Each combination has its own
`build-<usb>-<security>-<mode>` directory. `open` explicitly selects an unsecured
diagnostic network; security never switches automatically.

To change the password, proxy port or RGB settings, open **PSP USB router**:

```sh
idf.py -B build-ecm-wpa2-bridge-proxy \
  -DSDKCONFIG=build-ecm-wpa2-bridge-proxy/sdkconfig menuconfig
```

## Flash and recovery

1. Connect the native USB-C port with a data cable.
2. Hold **BOOT**, press and release **RESET**, then release **BOOT**.
3. Flash the selected build, replacing the ROM serial port:

   ```sh
   idf.py -B build-ecm-wpa2-bridge-proxy \
     -DSDKCONFIG=build-ecm-wpa2-bridge-proxy/sdkconfig \
     -p /dev/cu.usbmodemXXXX flash
   ```

4. Press **RESET** without BOOT if the application does not start automatically.

Use the matching build directory and a `COMx` port on Windows. During normal
operation USB appears as a network adapter, without a serial console.
BOOT/RESET recovery remains available. `idf.py flash` preserves NVS;
the application binary alone belongs at `0x10000`, not `0x0`.

## Computer internet sharing

The computer needs a separate working internet connection. Share it **to the ESP USB adapter**:

- **Windows / RNDIS:** open `ncpa.cpl`, then the internet adapter's Properties →
  Sharing. Enable Internet Connection Sharing and select the ESP RNDIS adapter.
  It uses the built-in Remote NDIS driver.
- **macOS / ECM:** System Settings → General → Sharing → Internet Sharing.
  Select the upstream internet connection and the ESP USB Ethernet adapter as the
  destination. The ESP creates its own Wi-Fi access point.
- **Ubuntu / ECM:** in NetworkManager, set the USB Ethernet connection's IPv4
  method to **Shared to other computers**. Keep the upstream connection enabled.

In bridge modes the computer also supplies DHCP, gateway and DNS directly to the PSP.

## Connect the PSP

Wi-Fi: **PSP-NET**, password **psp3000wifi**, 2.4 GHz / 802.11b / channel 6.

1. For **WPA2**, enable **WPA2 Support** in ARK-5, fully power off and restart the
   PSP, then create an **AES** connection (the PSP may label it WPA-PSK (AES)).
   For **WPA1**, use a `wpa1` build and **TKIP**; disable ARK-5 WPA2 Support and
   restart the PSP. WPA1 supports stock PSP firmware.
2. Set IP and DNS to **Automatic**.
3. For direct access, select **Proxy → Do Not Use**. To use the browser gateway,
   select **Use** and enter the address and port from the modes table.
4. Open `http://psp-proxy:8080/` in bridge-proxy mode, or
   `http://192.168.50.1:8080/` with the default router configuration.

Enter website URLs with **http://**, for example `http://zond.am/`. ESP fetches
the website over HTTPS and verifies its certificate; PSP receives HTTP over the
protected Wi-Fi connection. Wait for network time synchronization after boot.

The gateway is for simple public pages: GET/HEAD, two concurrent requests,
up to 4 MiB per response. It does not adapt modern CSS/JavaScript, logins or video,
and does not support POST or CONNECT. HTTP-only sites require direct access.

## RGB status

The onboard RGB LED uses GPIO48. Brightness and enable/disable are in `menuconfig`.

| Indication | Meaning |
| --- | --- |
| Red | USB network inactive |
| Yellow | USB active; internet not yet verified |
| Green | External HTTP connectivity check succeeded |
| Brief cyan pulse every 2 seconds | A Wi-Fi client is connected; the base colour remains visible between pulses |

Internet checks run in the background; allow about 30 seconds for a change.
The separate blue charging LED can blink when no battery is connected.

## Development

`tools/build.sh` checks the resulting firmware configuration and USB descriptors.
Run `sh tools/test_host.sh` for host tests. Build output and local test reports
are excluded from Git.
