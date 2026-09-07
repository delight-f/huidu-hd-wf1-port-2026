# Huidu HD-WF1 Port — Tronbyt Firmware for ESP32-S2

[![ESP-IDF v5.5](https://img.shields.io/badge/ESP--IDF-v5.5-blue?logo=espressif&style=flat-square)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s2/)
[![Target ESP32-S2](https://img.shields.io/badge/target-ESP32--S2-red?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s2)
[![Panel 64×32 HUB75E](https://img.shields.io/badge/panel-64%C3%9732_HUB75E-orange?style=flat-square)](.)
[![Status Hardware-Untested](https://img.shields.io/badge/status-hardware--untested-yellow?style=flat-square)](.)
[![License Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-green?style=flat-square)](LICENSE)
[![Year 2026](https://img.shields.io/badge/year-2026-darkgreen?style=flat-square)](.)
[![Upstream Tronbyt](https://img.shields.io/badge/upstream-tronbyt%2Ffirmware--esp32-lightgrey?style=flat-square)](https://github.com/tronbyt/firmware-esp32)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square)](https://github.com/delight-f/huidu-hd-wf1-port-2026/pulls)

Tronbyt/WebP LED-matrix firmware ([tronbyt/firmware-esp32](https://github.com/tronbyt/firmware-esp32)) ported to the **Huidu HD-WF1** — an **ESP32-S2** (single-core, 4 MB flash, no PSRAM) driving a **64×32 HUB75E** panel. Fetches WebP images from a URL or over WebSocket, with a WiFi captive-portal for setup.

> [!WARNING]
> **Hardware-untested.** This port compiles (`image_info` passes) but has not yet been flashed to a real HD-WF1. Expect to iterate on clock phase, latch blanking, and colour swap on the bench.

## Hardware

| Item | Detail |
| ---- | ------ |
| Controller | Huidu HD-WF1 (ESP32-S2, 4 MB flash, no PSRAM) |
| Panel | 64×32 HUB75E |
| Button GPIO | 11 |
| Partition table | `boards/max_app_4mb.csv` |
| Download mode | Bridge the two pads near the Micro-USB port, connect via USB-A |

### WF1 → HUB75 pin map (`main/display.cpp`)

| Signal | GPIO | Signal | GPIO |
| ------ | ---- | ------ | ---- |
| R1 | 2 | G1 | 6 |
| B1 | 3 | R2 | 4 |
| G2 | 8 | B2 | 5 |
| A | 39 | B | 38 |
| C | 37 | D | 36 |
| E | 12 | LAT | 33 |
| OE | 35 | CLK | 34 |

## Quickstart

**Prerequisites:** [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (native install) + Python 3.

```bash
git clone https://github.com/delight-f/huidu-hd-wf1-port-2026.git
cd huidu-hd-wf1-port-2026

cp secrets.json.example secrets.json   # add WiFi + REMOTE_URL
# {"WIFI_SSID": "mySSID", "WIFI_PASSWORD": "<PASSWORD>",
#  "REMOTE_URL": "http://homeServer.local:8000/<deviceID>/next"}

. $HOME/esp-idf/export.sh
make huidu-wf1
```

Or with `idf.py` directly:

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
```

**Flash** (once bench-ready):

```bash
esptool --chip esp32s2 --before no_reset --after hard_reset \
  write_flash 0x0 build/merged_firmware.bin
```

## Port notes

- **Single-core fix** (`main/gfx.c`): `GFX_TASK_CORE` forced to `0` on ESP32-S2 (`xTaskCreatePinnedToCore` has no core 1).
- **No PSRAM**: `CONFIG_SPIRAM=n`; HTTP buffers capped (`60000` max / `10000` default) to fit S2 RAM.
- **`swap_colors` excluded** from the WF1 pin block per original spec — revisit after bench test.
- Based on `sdkconfig.defaults.pixoticker` (closest analogue: same flash, no-PSRAM family), retargeted to S2.

### Changed files vs upstream

| File | Change |
| ---- | ------ |
| `main/Kconfig.projbuild` | `BOARD_HUIDU_WF1` choice |
| `main/display.cpp` | WF1 HUB75 pin map |
| `main/gfx.c` | S2 single-core guard |
| `sdkconfig.defaults.huidu-wf1` | S2 target, 4 MB flash, no SPIRAM, WF1 board |
| `Makefile` | `huidu-wf1` build target |

## Other boards

This repo keeps full upstream history — Gen1, Gen2, Tronbyt-S3, MatrixPortal-S3, Waveshare-S3, and Pixoticker targets all still build via `make <board>` (see `make help`).

## Contributing

Bench results (photos, logic captures, working pin tweaks) are the most valuable contribution right now. PRs welcome.

## License & credit

Apache-2.0 — see [LICENSE](LICENSE). Upstream firmware by the [Tronbyt](https://github.com/tronbyt/firmware-esp32) community; WF1 port by [@delight-f](https://github.com/delight-f).
