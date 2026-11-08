#!/usr/bin/env python3
"""Check the USB bytes actually linked into the ESP32-S3 firmware (no device I/O)."""
import argparse
import json
import struct
import shlex
from pathlib import Path

from elftools.elf.elffile import ELFFile


def descriptors(data):
    offset = 0
    while offset < len(data):
        length = data[offset]
        assert length >= 2 and offset + length <= len(data), (offset, length)
        yield data[offset : offset + length]
        offset += length
    assert offset == len(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--profile", choices=["rndis", "ecm"], required=True)
    parser.add_argument("--security", choices=["wpa2", "wpa1", "open"], default="wpa2")
    parser.add_argument("--mode", choices=["router", "bridge", "bridge-proxy"], default="router")
    args = parser.parse_args()
    build = args.build.resolve()
    project = json.loads((build / "project_description.json").read_text())
    assert project["target"] == "esp32s3"
    config = Path(project["config_file"]).read_text()
    wpa_wrapper = args.security == "wpa1"
    bridge = args.mode != "router"
    bridge_proxy = args.mode == "bridge-proxy"
    required = [
        'CONFIG_ESPTOOLPY_FLASHSIZE="4MB"',
        "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y",
        "CONFIG_SPIRAM_MODE_QUAD=y", "CONFIG_SPIRAM_SPEED_40M=y",
        "CONFIG_SPIRAM_MEMTEST=y",
        "CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=1", "CONFIG_LWIP_L2_TO_L3_COPY=y",
        "CONFIG_TINYUSB_NET_MODE_ECM_RNDIS=y", "# CONFIG_TINYUSB_CDC_ENABLED is not set",
        "CONFIG_ESP_CONSOLE_NONE=y", "CONFIG_LOG_DEFAULT_LEVEL_NONE=y",
        "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y",
        "CONFIG_COMPILER_OPTIMIZATION_SIZE=y",
        f"CONFIG_PSP_USB_{args.profile.upper()}=y",
        f"CONFIG_PSP_AP_{args.security.upper()}=y",
        f"CONFIG_PSP_MODE_{'BRIDGE' if bridge else 'ROUTER'}=y",
    ]
    proxy_enabled = "CONFIG_PSP_HTTP_PROXY=y" in config.splitlines()
    rgb_enabled = "CONFIG_PSP_STATUS_RGB=y" in config.splitlines()
    assert ("CONFIG_PSP_BRIDGE_PROXY=y" in config.splitlines()) == bridge_proxy
    if bridge:
        assert proxy_enabled == bridge_proxy, "Use a separate bridge-proxy build for the gateway"
    required += (["CONFIG_LWIP_IP_FORWARD=y", "CONFIG_LWIP_IPV4_NAPT=y", "CONFIG_LWIP_DHCPS=y",
                  "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240"] if args.mode == "router"
                 else ["CONFIG_ESP_NETIF_BRIDGE_EN=y"])
    if proxy_enabled:
        required += ["CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y", "CONFIG_MBEDTLS_HAVE_TIME_DATE=y",
                     "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y",
                     "CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y", "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240"]
    if bridge_proxy or (args.mode == "router" and args.security == "wpa2"):
        required += ["CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y"]
    for setting in required:
        assert setting in config.splitlines(), f"Missing setting: {setting}"
    commands = json.loads((build / "compile_commands.json").read_text())
    for security in ("wpa2", "wpa1", "open"):
        if security != args.security:
            assert f"CONFIG_PSP_AP_{security.upper()}=y" not in config.splitlines(), "Multiple security profiles"
    if wpa_wrapper:
        wpa_defines = []
        for name in ("wpa_compat.c", "esp_hostap.c"):
            command = next(c for c in commands if c["file"].endswith("/" + name))
            assert "esp-idf/wpa_supplicant/" in command["command"]
            wpa_defines.append({s for s in shlex.split(command["command"]) if s.startswith("-D")})
        assert wpa_defines[0] == wpa_defines[1], "WPA compatibility wrapper ABI differs from authenticator"
    else:
        assert not any(c["file"].endswith("/wpa_compat.c") for c in commands), "Legacy EAPOL wrapper in non-WPA1 build"
    for setting in ["CONFIG_SECURE_BOOT=y", "CONFIG_SECURE_FLASH_ENC_ENABLED=y",
                    "CONFIG_ESP_CONSOLE_USB_CDC=y", "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
                    "CONFIG_ESP_TLS_INSECURE=y", "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y"]:
        assert setting not in config.splitlines(), f"Unexpected setting: {setting}"
    with (build / "psp_usb_router.elf").open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols()}
        for name in ("rmt_new_tx_channel", "start_dns", "probe_task"):
            assert (name in symbols) == rgb_enabled, f"Wrong RGB/probe configuration: {name}"
        assert ("sync_lan_neighbors" in symbols) == (args.mode == "router" and args.security == "wpa2"), \
            "DHCP-derived ARP compatibility must be confined to WPA2 router builds"

        def read_symbol(name):
            s = symbols[name]
            section = elf.get_section(s["st_shndx"])
            start = s["st_value"] - section["sh_addr"]
            return section.data()[start : start + s["st_size"]]

        device = read_symbol("device_descriptor")
        assert len(device) == 18 and device[0:2] == bytes([18, 1])
        assert device[4:7] == bytes([0xEF, 2, 1]) and device[17] == 1
        assert struct.unpack_from("<H", device, 8)[0] == 0x303A
        assert struct.unpack_from("<H", device, 10)[0] == (0x4011 if args.profile == "rndis" else 0x4012)
        config_bytes = read_symbol("configuration_descriptor")
        entries = list(descriptors(config_bytes))
        assert entries[0][1] == 2 and entries[0][4] == 2
        assert struct.unpack_from("<H", entries[0], 2)[0] == len(config_bytes)
        assert entries[0][8] == 250  # 500 mA, bus powered
        interfaces = [e for e in entries if e[1] == 4]
        control = next(e for e in interfaces if e[2] == 0)
        assert control[5:8] == (bytes([0xE0, 1, 3]) if args.profile == "rndis" else bytes([2, 6, 0]))
        assert all(e[2] in (0, 1) for e in interfaces)
        endpoints = [e[2] for e in entries if e[1] == 5]
        assert sorted(endpoints) == [2, 0x81, 0x82]
        if args.profile == "ecm":
            assert "guard_control" in symbols and "usb_ecm_data_ready" in symbols
            assert [(e[3], e[4]) for e in interfaces if e[2] == 1] == [(0, 0), (1, 2)]
            ethernet = next(e for e in entries if e[1:3] == bytes([0x24, 15]))
            assert ethernet[3] == 5 and struct.unpack_from("<H", ethernet, 8)[0] == 1514
        else:
            ms = read_symbol("microsoft_os_20")
            assert len(ms) == 46 and struct.unpack_from("<H", ms, 8)[0] == 46
            assert struct.unpack_from("<H", ms, 16)[0] == 36
            assert ms[22] == 0 and struct.unpack_from("<H", ms, 24)[0] == 28
            assert ms[30:38] == b"RNDIS\0\0\0" and ms[38:] == b"5162001\0"
            bos = read_symbol("bos_descriptor")
            assert len(bos) == 33 and bos[-2] == 0x20
        if bridge:
            assert "bridgeif_add_port" in symbols and "ip_napt_enable_netif" not in symbols
            assert "router_dns_start" not in symbols
            assert "CONFIG_LWIP_IP_FORWARD=y" not in config.splitlines()
        else:
            assert "bridgeif_add_port" not in symbols and "ip_napt_enable_netif" in symbols
            assert "router_dns_start" in symbols
        assert ("browser_proxy_start" in symbols) == proxy_enabled
        if proxy_enabled:
            assert "esp_crt_bundle_attach" in symbols
        for name in ("bridge_proxy_intercept", "bridge_dns_frame_reply", "bridge_sync_neighbors"):
            assert (name in symbols) == bridge_proxy, f"Wrong bridge DNS/ARP mode: {name}"
        assert ("__wrap_wpa_init" in symbols) == wpa_wrapper
        for name in ["__wrap_wpa_receive", "__wrap_hostapd_send_eapol", "__wrap_esp_wifi_internal_tx",
                     "wifi_diag_json", "wifi_diag_frame", "wifi_diag_set_eapol_version",
                     "router_console_poll", "wifi_trace_start", "usb_diag_start", "cdcd_open"]:
            assert name not in symbols, f"Debug/forced-rate code in release: {name}"
        for section in elf.iter_sections():
            if section["sh_type"] == "SHT_PROGBITS" and section["sh_flags"] & 2:
                assert b"/debug/wifi" not in section.data(), "Diagnostic HTTP route in release"
    link_map = (build / "psp_usb_router.map").read_text()
    if wpa_wrapper:
        init_refs = link_map.split("\n__wrap_wpa_init ", 1)[1].split("\n", 2)[:2]
        assert "esp_hostap.c.obj" in "\n".join(init_refs), "Authenticator initialization bypasses compatibility wrapper"
    else:
        assert "__wrap_wpa_init" not in link_map, "Legacy EAPOL linker override in non-WPA1 build"
    print(f"PASS: {args.profile}/{args.security}/{args.mode}: target, memory/recovery, USB descriptors, network mode")


if __name__ == "__main__":
    main()
