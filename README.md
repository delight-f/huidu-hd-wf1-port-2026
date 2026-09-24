# Huidu HD-WF1 Port — Tronbyt Firmware for ESP32-S2

[![ESP-IDF v5.5](https://img.shields.io/badge/ESP--IDF-v5.5-blue?logo=espressif&style=flat-square)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s2/)
[![Target ESP32-S2](https://img.shields.io/badge/target-ESP32--S2-red?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s2)
[![Panel 64×32 HUB75E](https://img.shields.io/badge/panel-64%C3%9732_HUB75E-orange?style=flat-square)](.)
[![Status Bench bring-up](https://img.shields.io/badge/status-bench_bring--up-yellow?style=flat-square)](.)
[![License Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-green?style=flat-square)](LICENSE)
[![Upstream Tronbyt](https://img.shields.io/badge/upstream-tronbyt%2Ffirmware--esp32-lightgrey?style=flat-square)](https://github.com/tronbyt/firmware-esp32)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square)](https://github.com/delight-f/huidu-hd-wf1-port-2026/pulls)

Tronbyt/WebP LED-matrix firmware ([tronbyt/firmware-esp32](https://github.com/tronbyt/firmware-esp32)) ported to the **Huidu HD-WF1** — an **ESP32-S2** (single-core, 4 MB flash, no PSRAM) driving a **64×32 HUB75E** panel. It fetches WebP images from a URL or over a WebSocket, with a WiFi captive portal for setup.

> [!IMPORTANT]
> **Bottom line after first bench bring-up: the firmware runs, joins WiFi and talks to the Tronbyt server, but the panel still renders corrupted output** (random red/green/blue dots) rather than a recognisable image. Everything up to and including the fetch loop is verified working on real hardware. See [Bring-up status](#bring-up-status) and [Work left to do](#work-left-to-do).

## Hardware

| Item | Detail |
| ---- | ------ |
| Controller | Huidu HD-WF1 (ESP32-S2, 4 MB flash, no PSRAM) |
| Panel | 64×32 HUB75E (LED driver ICs: **FM6124**) |
| Button GPIO | 11 |
| Partition table | `boards/max_app_4mb.csv` (single factory app slot — **no OTA**) |
| Download mode | Bridge the two GPIO0 pads near the Micro-USB port, then connect the **USB-A** port with a USB-A-to-USB-A cable |

### WF1 → HUB75 pin map (`main/display.cpp`)

Reverse-engineered and **verified byte-for-byte against [mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA](https://github.com/mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA)** (`hd-wf1-esp32s2-config.h`):

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
#  "REMOTE_URL": "http://yourServer:8000/<deviceID>/next"}

. $HOME/esp-idf/export.sh
make huidu-wf1
```

Or with `idf.py` directly:

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
```

**Flash** (bridge the GPIO0 pads first so the board enters download mode):

```bash
idf.py -p /dev/ttyACM0 flash
```

## Bring-up status

Verified on real hardware (ESP32-S2 rev v1.0, no embedded PSRAM):

| Area | State |
| ---- | ----- |
| Build + flash (native ESP-IDF v5.5, esp32s2) | ✅ works |
| Boots, single-core task pinning | ✅ works |
| WiFi **station** join, DHCP | ✅ works |
| Reaches the Tronbyt server and registers (visible in tronbyt-manager) | ✅ works |
| Boot animation + version screen drawn on the panel | ✅ works (colours and blanking correct) |
| Config portal, `/diag`, `/panel` | ✅ works |
| **WebP image rendering** | ❌ **corrupted — random coloured dots, no recognisable image** |

Colour channel order and blanking are correct — a full-screen solid fill renders as the right colour — but any real image comes out as noise. See [Work left to do](#work-left-to-do).

## Bring-up findings

This section is the point of the port so far: the HD-WF1 is a much more constrained target than any upstream board (320 KB SRAM, no PSRAM, single core), and a stock Tronbyt build does not survive it. Each item below was a real failure reproduced on the bench.

### 1. Every image/portal buffer was allocated from PSRAM that does not exist

Upstream allocates the HTTP receive buffer, the WebSocket reassembly buffer and the config-portal scratch buffers with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`. With `CONFIG_SPIRAM=n` on the S2 there is no SPIRAM region, so those calls return `NULL` with no fallback — HTTP fetch, WebSocket reassembly and portal save all fail silently.

**Fix:** added `main/mem_compat.h` defining `IMAGE_BUF_CAPS` = `MALLOC_CAP_SPIRAM` when `CONFIG_SPIRAM` is enabled, otherwise `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. PSRAM boards keep the previous behaviour; the S2 falls back to internal RAM.

### 2. The 174 KB boot animation could not be allocated, and the failure was fatal

`gfx_initialize()` copies the selected boot WebP into a single `calloc()`. The WF1 config selected `CONFIG_BOOT_WEBP_TRONBYT`, which is **174,330 bytes** — impossible as one contiguous block in the S2's internal RAM once WiFi is up (the no-PSRAM `pixoticker` target uses the ~4.5 KB `BOOT_WEBP_PARROT`).

Worse, `main.c` treated that failure as fatal:

```c
if (gfx_initialize(image_url)) {
  ESP_LOGE(TAG, "failed to initialize gfx");
  return;            // app_main exits before ap_start()
}
```

So a failed display init also took down the **config portal** — the AP SSID was still broadcast (that is done by the WiFi hardware, not the CPU), which made it look like the app was healthy when it had already exited.

**Fix:** switched the WF1 to the small boot asset, and made a display-init failure non-fatal so the portal still comes up.

### 3. Double buffering could not be allocated on a no-PSRAM S2 (this kept the panel dark)

`setupDMA()` allocates the BCM framebuffer in **DMA-capable internal RAM**: one `rowBitStruct` per row per colour depth, ~1 KB each × 16 rows, and **×2 when double buffering is on**. The port hard-coded `double_buff = true` for every board; the library default (and the MrCodeTastic reference) is `false`.

With `true`, `setupDMA()` returned false → `begin()` returned false → `_matrix == NULL` → nothing was ever drawn. Turning double buffering off for the WF1 was what finally lit the panel.

### 4. A NULL `_matrix` then crashed the device

Once the display-init failure was made non-fatal (finding 2), the code continued and later called `display_clear()`, which was a bare `_matrix->clearScreen()` with no NULL guard. The device panicked ~60 s after boot, which is what made the config portal appear "flaky and disconnecting".

**Fix:** NULL guards on `display_clear()` and `display_draw()`.

### 5. The panel's LED driver ICs are FM6124, not shift-registers

The reference demo runs its panel as a plain shift-register type, and this port was aligned to that. But the library's `shiftDriver()` only runs the `fm6124init()` register-init sequence for **`FM6124` / `FM6126A` / `ICN2038S`** — `SHIFTREG` runs **no init at all**. The ICs on this panel are marked **FM6124**, so they were being driven uninitialised.

**Fix:** `FM6124` is now the WF1 default driver (and `fm6124init()` does run — confirmed on the bench). This changed the output but did not solve the corruption.

### 6. Open issue: image rendering is still corrupted

With FM6124 init in place, the panel still renders noise. Solid fills look correct, which means the data pins, OE/LAT/CLK and the colour order are all right — but a full-screen fill is *invariant* under row-addressing and timing errors, so it cannot validate them. The corruption is therefore in the scan/timing path: wrong scan rate, clock phase, latch blanking, pixel clock, or the I²S/DMA data stream itself.

### Tooling note: there is no usable serial console on this board

The WF1 has no UART-to-USB bridge, and `CONFIG_ESP_CONSOLE_USB_CDC` does **not enumerate on ESP32-S2** in ESP-IDF without TinyUSB (a known IDF defect: `device not accepting address`, `error -71`, descriptor timeouts). `idf.py monitor` therefore has nothing to attach to.

To keep the board observable, two small additions were made (both are now part of the port, not throwaway debug code):

- **`/diag`** — captures ESP-IDF log output into a RAM ring buffer and serves it as plain text, together with the reset reason and the free heap / free DMA heap. This is the *only* console this board has.
- **`/panel`** — sets HUB75 driver/timing overrides in NVS and reboots, so the panel configuration can be swept without re-flashing.

## Panel configuration matrix

Tried so far (WF1 defaults otherwise: 64×32, chain 1, `TYPE138`, double-buffering off):

| Driver | Latch blanking | Clock phase | Speed | Result |
| ------ | -------------- | ----------- | ----- | ------ |
| `SHIFTREG` (no init) | 4 | inverted | 20 MHz | random coloured dots |
| `SHIFTREG` (no init) | 1 | inverted | 20 MHz | random coloured dots |
| `FM6124` (init runs) | 1 | inverted | 20 MHz | **still corrupted** |
| `FM6124` (init runs) | 4 | inverted | 20 MHz | **still corrupted** |
| any | any | either | either | ✅ solid R/G/B fills correct; ❌ images corrupted |

## Runtime panel tuning (`/panel`)

The web server listens on both the AP and station interfaces, so these work over your normal network at the device's own IP, or over `TRON-CONFIG` at `10.10.0.1`.

```
http://<device-ip>/panel?drv=1&lat=1     # driver / latch blanking
http://<device-ip>/panel?ph=0            # clock phase
http://<device-ip>/panel?spd=0           # 8 MHz instead of 20 MHz
http://<device-ip>/panel?dbfr=1          # double buffering
http://<device-ip>/panel?clear=1         # revert to board defaults
http://<device-ip>/diag                  # captured log + active panel config
```

`drv`: `0`=SHIFTREG, `1`=FM6124, `2`=FM6126A, `3`=ICN2038S, `4`=MBI5124, `5`=DP3246.

## Work left to do

1. **Fix the corrupted image rendering** — the main blocker. Ranked candidates:
   - **HUB75 library version.** The reference project pulls the library's **master** while this port pins **v3.0.14** (`f17fb7f`), and the reference builds under Arduino/IDF 5.3 vs native IDF 5.5 here. The ESP32-S2 I²S-parallel backend differs between versions; try bumping the library.
   - **Clock phase / latch blanking / pixel clock** sweep via `/panel` (no re-flash needed once you can reach the device).
   - **Panel scan rate / row addressing** — confirm the panel really is 1/16 scan for 64×32.
   - Consider whether the upstream `-patched` I²S divider fix (`CONFIG_PATCH_I2S_DIVIDER`) is relevant for this S2 backend.
2. **Find the device's IP / regain portal access** for tuning — the station link hides the board from the AP's subnet; a phone on `TRON-CONFIG` or the router's DHCP list is the easiest route. Consider adding the board's own IP to the boot screen.
3. **Remove the temporary bench diagnostic** (`main/display.cpp`: the R/G/B fill after `begin()`), or gate it behind a Kconfig option.
4. **Decide the AP auto-shutdown behaviour** — it is currently disabled so `/diag` stays reachable; upstream shuts the AP down ~2 minutes after STA connects (and switches to STA-only, which leaves the board unreachable if the station link then drops).
5. **CI + IDE**: add a `huidu-wf1` entry to the GitHub Actions matrix (and an `esp32s2` case in its chip dispatch, which currently maps everything non-S3 to `esp32`), and to `esp_idf_project_configuration.json`.
6. **OTA** — currently impossible: `boards/max_app_4mb.csv` has a single `factory` app slot. Two ~1.75 MB slots would fit in 4 MB (the app is ~1.3 MB).
7. **Brightness ceiling** for third-party panels (`BRIGHTNESS_8BIT_MAX`) — the WF1 currently inherits the legacy `230`.
8. **`swap_colors`** — still excluded for the WF1; revisit if the colours ever come out wrong.

## Changed files vs upstream

| File | Change |
| ---- | ------ |
| `main/Kconfig.projbuild` | `BOARD_HUIDU_WF1` choice |
| `main/display.cpp` | WF1 HUB75 pin map; per-board + runtime-tunable panel driver/timing; NULL-`_matrix` guards; text clamped to panel width; temporary R/G/B bench test |
| `main/gfx.c` | S2 single-core guard; boot-debug text clamped to 10 chars so it fits 64 px |
| `main/main.c` | Display-init failure no longer fatal; AP portal left running; log capture init |
| `main/mem_compat.h` **(new)** | `IMAGE_BUF_CAPS` — SPIRAM when available, internal RAM otherwise |
| `main/diag.c` / `main/diag.h` **(new)** | In-RAM log ring buffer, served by `/diag` |
| `main/ap.c` | `/diag` and `/panel` endpoints; `max_uri_handlers` raised |
| `sdkconfig.defaults.huidu-wf1` | S2 target, 4 MB flash, no SPIRAM, WF1 board, small boot asset, flash core dumps |
| `Makefile` | `huidu-wf1` build target |

## Other boards

This repo keeps full upstream history — Gen1, Gen2, Tronbyt-S3, MatrixPortal-S3, Waveshare-S3 and Pixoticker targets all still build via `make <board>` (see `make help`).

## Contributing

Bench results are the most valuable contribution right now — specifically, any working combination of driver / latch blanking / clock phase / pixel clock for the FM6124 panel, logic-analyser captures of the HUB75 bus, and photos showing the corruption pattern. PRs welcome.

## License & credit

Apache-2.0 — see [LICENSE](LICENSE). Upstream firmware by the [Tronbyt](https://github.com/tronbyt/firmware-esp32) community. HD-WF1 pin map and board research by [mrcodetastic](https://github.com/mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA). WF1 port by [@delight-f](https://github.com/delight-f).
