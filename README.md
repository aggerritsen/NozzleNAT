# ESP32 NAT Router (PlatformIO • ESP-IDF 5.1.2)

Robust Wi‑Fi NAT router firmware for ESP32. Works as a range extender, guest network, or a bridge from WPA2‑Enterprise to a regular WPA2/WPA3 PSK network. This repo is pre‑configured for PlatformIO and a known‑good ESP‑IDF **5.1.2** (Tasmota “org” build) with LWIP NAT enabled.

## Features

- AP+STA router (NAT) with IPv4 NAPT  
- Optional WPA2‑Enterprise (username/identity/password) on STA side  
- Web UI at `http://192.168.4.1` for configuration  
- Serial CLI (console) for scripting and recovery  
- Compile‑time defaults for AP/STA SSIDs & passwords  
- Boot banner prints the most important runtime parameters

---
**Original standard README:** [README-STD.md](README-STD.md)


## Hardware tested

- ESP32 Dev Module (4 MB flash)  
- (Originally developed on TTGO‑T‑Journal; any ESP32‑D0WD chip should work)

---

## Quick start

```bash
# 1) Clone this repo
git clone <this-repo-url>
cd <repo>

# 2) Open in VS Code + PlatformIO

# 3) (Optional) Adjust compile-time defaults in platformio.ini build_flags
#    e.g. AP SSID, STA SSID/PASS

# 4) Build & Flash
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor  # 115200 baud by default
```

On first boot you’ll see a banner. Connect to the AP and visit:

```
SSID: (from defaults or NVS)
URL : http://192.168.4.1
```

---

## Project layout

```
.
├─ main/                  # firmware sources (incl. wifi_init with EAP + NAT)
├─ components/            # CLI commands (cmd_nvs, cmd_router, cmd_system)
├─ sdkconfig.esp32dev     # known-good SDK config (LWIP NAT, HTTPD sizes, etc.)
├─ platformio.ini         # pins the toolchain + compile-time defaults
└─ README.md
```

---

## PlatformIO setup (what this repo already uses)

`platformio.ini`:

```ini
[platformio]
default_envs = esp32dev
src_dir = main

[env]
platform = espressif32@5.4.0
framework = espidf
platform_packages =
    framework-espidf @ https://github.com/tasmota/esp-idf/releases/download/v5.1.2-org/esp-idf-v5.1.2-org.zip
monitor_speed = 115200

[env:esp32dev]
board = esp32dev
build_flags =
  -DDEFAULT_AP_SSID=\"NozzleNAT\"
  -DDEFAULT_AP_PASS=\"\"           ; empty = OPEN AP
  -DDEFAULT_STA_SSID=\"MARIJA\"
  -DDEFAULT_STA_PASS=\"cicero14\"
  -DAPPLY_DEFAULTS_EVERY_BOOT=1      ; let compile-time defaults override NVS each boot
```

> **Why this combo?**  
> ESP‑IDF 5.1.2 “org” avoids the newlib/NVS API breakages seen with 5.2+ and keeps WPA2‑Enterprise, LWIP NAT and HTTP server behaving like the upstream project this is based on.

---

## SDK config highlights (already set)

These live in `sdkconfig.esp32dev`. The key bits for NAT & HTTPD are:

```ini
# LWIP / NAT
CONFIG_LWIP_L2_TO_L3_COPY=y
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y

# HTTP Server (avoid “Header field too long”)
CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048
CONFIG_HTTPD_MAX_URI_LEN=1024
```

> If you change SDK settings, reconfigure with `pio run -t menuconfig` (PIO will write `sdkconfig`) or keep using the provided `sdkconfig.esp32dev`.

---

## Defaults & persistence (NVS)

- **Compile‑time defaults** are set via `build_flags` (see above) and used at boot.  
- **NVS** (flash) stores settings changed via Web UI or CLI.  
- With `-DAPPLY_DEFAULTS_EVERY_BOOT=1`, compile‑time defaults **override** NVS each boot. Remove that flag to let changes persist.

### Useful CLI (serial console)

Open the serial monitor (115200) and type:

```text
help
nvs_namespace esp32_nat
nvs_list nvs
nvs_get ssid str
nvs_get passwd str
```

Set STA (uplink) to your 2.4 GHz SSID:

```text
set_sta <STA_SSID> <STA_PASS>
restart
```

Set AP (downlink) SSID/pass:

```text
set_ap <AP_SSID> <AP_PASS>   # pass must be 8–63 chars; empty = OPEN AP
restart
```

WPA2‑Enterprise (optional):

```text
set_sta <STA_SSID> <STA_PASS> --username=<ENT_USER> --anan=<ENT_IDENTITY>
restart
```

Erase only this app’s namespace:

```text
nvs_namespace esp32_nat
nvs_erase_namespace esp32_nat
restart
```

---

## Web UI

1. Connect to the AP SSID printed at boot (default `NozzleNAT` if not overridden).  
2. Browse to `http://192.168.4.1`.  
3. Configure STA (“uplink”) and AP (“downlink”) & save.

> If you ever see “Header field too long”, this repo already increases `CONFIG_HTTPD_MAX_REQ_HDR_LEN` to **2048** and URI length to **1024**.

---

## Boot banner (what you’ll see)

Example:

```
================== ESP32 NAT Boot ==================
HTTPD: MAX_REQ_HDR_LEN=2048, MAX_URI_LEN=1024
LWIP: IP_FORWARD=1  IPV4_NAPT=1
Defaults applied each boot: YES (compile-time)
AP:  SSID="ESP32_NAT_Router"  auth=OPEN  ch=6  ip=192.168.4.1/24
STA: SSID="MARIJA"  pass_len=8  static_ip=NO
Build defaults: AP_SSID="NozzleNAT"  AP_pass_len=0  STA_SSID="MARIJA"
====================================================
```

---

## Troubleshooting

- **Stuck on connecting to AP / retries**  
  Ensure STA SSID is **2.4 GHz**, not a 5 GHz SSID. Use `set_sta ...` and `restart`.

- **Web says: Header field too long**  
  Already mitigated via HTTPD settings. If your browser/extensions add very large headers, try a different browser or private window.

- **Serial shows “Your terminal application does not support escape sequences.”**  
  Use **PuTTY** (Windows) or the PlatformIO monitor is fine; the console still works.

- **`Failed to mount FATFS (storage)`**  
  Harmless unless you need on-device file storage. Our default partition table is single-app and **does not** include a `storage` FAT partition. Ignore, or switch to a custom partition CSV that has a `data,fat,storage` region.

- **Old SSID (e.g. `MARIJA_5G`) keeps showing**  
  That was in NVS. Either `nvs_erase_namespace esp32_nat` or build with `-DAPPLY_DEFAULTS_EVERY_BOOT=1`.

---

## License & credits

This work builds on the public ESP32 NAT Router examples and console component from Espressif; adapted to PlatformIO with a stable ESP‑IDF 5.1.2 toolchain and additional boot/status logging.
