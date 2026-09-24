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
> **Bottom line after bench bring-up: the firmware runs, the display works, and the device talks to the Tronbyt server — but WebP images do not render yet.** The panel, its timing and its colour order are all demonstrably correct, and the network path is now confirmed end to end (the log shows real image fetches from the Tronbyt server returning in 26–75 ms). What fails is the **WebP decoder**, and it fails on a single allocation: libwebp needs one *contiguous* 12,544-byte block for its ARGB working buffer, and the largest free block the board can offer is 7,936–9,216 bytes. That is a **heap fragmentation** problem, not a capacity problem — ~20 KB is free, and ~18 KB is all the decoder needs. See [Bring-up status](#bring-up-status) and [Work left to do](#work-left-to-do).

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
| **HTTP fetch from the Tronbyt server** | ✅ works — 226–360 byte images in 26–75 ms |
| WebSocket connect / registration in tronbyt-manager | ⚠️ unverified (see note) |
| Boot animation + version screen drawn on the panel | ✅ works (colours and blanking correct) |
| Config portal, `/diag`, `/panel` | ✅ works, over both the AP and the station address |
| **WebP decode + rendering** | ❌ **one contiguous 12,544 B allocation fails — heap fragmentation** |

Colour channel order and blanking are correct — a full-screen solid fill renders as the right colour, and the boot version text is legible. The failure is confined to the WebP decode path. See [Work left to do](#work-left-to-do).

> **Note on "last seen".** The tronbyt-manager UI reports this device as offline even while `remote: Content-Length Header : 226` / `main: HTTP fetch returned in 26 ms` lines stream in the on-device log. The UI appears to track the **WebSocket**, not the HTTP polling that actually drives the display, so treat its "last seen" as a WebSocket health indicator rather than a connectivity test.

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

### 6. The real blocker: one contiguous allocation libwebp can never get

Text and solid fills render correctly, which proves the panel, its timing and its colour order are all fine. The network path is fine too — the log shows real fetches from the Tronbyt server. What fails is a **single allocation inside the WebP decoder**, and it fails on the *first frame* of every image, however small.

`WebPAnimDecoder` was the first casualty: it allocates **two full canvas buffers** internally (`anim_decode.c:140-145`) plus VP8 state, and `WebPAnimDecoderNew()` returned NULL — reported on the panel as `new ERR`. `gfx.c` therefore stopped using it and drives the demuxer directly (`WebPDemux` + per-frame `WebPDecode` with `output.is_external_memory = 1`), which gets past the allocation it controls.

**That decode logic is now correct and verified.** Each animation frame is decoded at its own offset with the canvas stride and frame-height size, and multi-frame images are composited through an RGBA scratch using libwebp's own key-frame / blend / dispose rules. Rendered on the host against the vendored sources, the output is **byte-identical to `WebPAnimDecoder`'s** for still and animated assets (the only divergence is the deliberate `no_fancy_upsampling` choice, on lossy frames).

What remains is purely the memory the decoder needs *internally*, which no option can shrink. On a 64×32 frame it wants **~18–26 KB transient**, as two allocations that are live at once:

| Allocation | Size | Note |
| ---------- | ---- | ---- |
| `dec->pixels` (ARGB working buffer) | **12,544 B** | must be **contiguous** |
| Huffman tables / htree groups | ~5–12 KB | data-dependent |

Cropping does not help (it is applied on output only; measured peak unchanged) and scaling makes it *worse* (18,200 → 19,456 B), because the rescaler adds memory without shrinking the full-size pixel buffer.

So the requirement reduces to: **one free block of ≥ 12,544 bytes, plus ~6 KB more from anywhere.** The board reports:

```
free_internal  19,924 B
largest_internal 7,936 B      <-- must be >= 12,544
```

~20 KB is free — enough in total — but the largest usable run is under 8 KB. Hence `Could not draw webp` / `frame 1 decode failed`, every time, with a completely stable heap (no leak; verified flat across repeated samples).

That makes this a **fragmentation** problem rather than a capacity one, which is a much better place to be: capacity cannot be conjured, but the layout can be influenced. See [Work left to do](#work-left-to-do).

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

> **These settings are not the cause of the current failure.** They were swept before it was established that the panel renders correctly (text and fills) and that the real fault is the WebP decoder's memory footprint. They are documented here so nobody repeats the sweep. The tunables still exist — see [Runtime panel tuning](#runtime-panel-tuning-panel).

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

### Button-driven config cycler (no network needed)

The single push button on the board is wired to **GPIO 11** — the same button the
firmware reads at boot to force config mode. Once the firmware is running, a
short press **steps to the next candidate panel configuration**, saves it to NVS
and reboots, so the right combination can be found with nothing but the board
(presses are ignored for the first 5 s after boot so config mode still works).

Each candidate is identified at boot by a **solid fill colour**. Solid fills
render correctly even when the panel is otherwise mis-configured — a full-screen
fill is invariant under row/timing errors — so the colour is a reliable
"which config is live" indicator where on-screen text is not.

| Idx | Config | Boot colour |
| --- | ------ | ----------- |
| 0 | board defaults | white |
| 1 | `drv=1` (FM6124) `lat=4` | red |
| 2 | `drv=1` `ph=0` | green |
| 3 | `drv=1` `spd=0` (8 MHz) | blue |
| 4 | `drv=2` (FM6126A) `lat=4` | yellow |
| 5 | `drv=3` (ICN2038S) `lat=1` | magenta |
| 6 | `drv=0` (SHIFTREG) `lat=4` 20 MHz | cyan |
| 7 | `drv=1` `ph=0` `spd=0` | orange |

The active index, name and parameters are also logged and shown on `/diag` as
`panel config N/7 '<name>' drv=… spd=… lat=… ph=… dbfr=…`.

> **Note:** these overrides live in NVS and survive reflashing. Clear them with
> `/panel?clear=1` (or cycle back to index 0) before comparing against the
> compiled-in defaults.

## Work left to do

1. **Get one contiguous 12,544-byte free block** — the sole remaining blocker, and a heap-layout problem rather than a capacity one (see [finding 6](#6-the-real-blocker-one-contiguous-allocation-libwebp-can-never-get)). Everything else on the image path is done and verified. Ranked levers, cheapest and safest first — full detail and the reasoning for each is in [HANDOFF.md](HANDOFF.md):
   - **Move our own buffers out of the middle of the heap.** `gfx.c` allocates its 6 KB canvas in the first `draw_webp()`, which is *after* WiFi, the HTTP server and the task stacks have come up — so it lands mid-heap and may be splitting the largest free run. Allocating it before the network stack starts costs nothing and could recover most of the missing contiguity.
   - **Measure the heap's shape, don't infer it.** Add a free-block dump (`heap_caps_print_heap_info`) reachable from `/diag`. Three earlier fixes were aimed from totals instead of from the layout, and all three missed.
   - **Retire the config portal's SoftAP once the station is up** (upstream already does this). It costs radio buffers, a DHCP server and a 4 KB DNS task stack. The station link works, so `/diag` stays reachable on the station address even without the AP. Keep the web server itself running.
   - **Force a contiguous arena** if the layout turns out to be immovable: allocate a block early and release it immediately before `WebPDecode`, so libwebp's allocations land in a known-good run. Costs heap while held, so it needs care.
2. **Do not re-chase these** — each was tried on the bench and measured, and none helped:
   - trimming the WiFi buffer counts (`STATIC_TX/RX_BUFFER_NUM`, `MGMT_SBUF_NUM`, AMPDU) — moved the heap by noise only, so those buffers evidently are not in the measured region;
   - moving the WiFi code out of IRAM (`CONFIG_ESP_WIFI_IRAM_OPT=n`) — frees ~23 KB of DIRAM at link time but made the *runtime* heap worse (24,948 → 20,368 B free);
   - lowering the panel's BCM colour depth 8 → 5 to reclaim 6 KB — `free_internal` did not move at all;
   - decoder cropping and scaling options — see finding 6.
3. **Remove the temporary bench diagnostic** (`main/display.cpp`: the R/G/B fill after `begin()`), or gate it behind a Kconfig option. Note the panel comes up showing collapsed rows **immediately after a flash** and a power-cycle clears it — that is a post-flash transient, not a firmware fault, and it reproduces on stock config.
4. **CI + IDE**: add a `huidu-wf1` entry to the GitHub Actions matrix (and an `esp32s2` case in its chip dispatch, which currently maps everything non-S3 to `esp32`), and to `esp_idf_project_configuration.json`.
5. **OTA** — currently impossible: `boards/max_app_4mb.csv` has a single `factory` app slot. Two ~1.75 MB slots would fit in 4 MB (the app is ~1.3 MB).
6. **Brightness ceiling** for third-party panels (`BRIGHTNESS_8BIT_MAX`) — the WF1 currently inherits the legacy `230`.
7. **`swap_colors`** — still excluded for the WF1; revisit if the colours ever come out wrong.

## Changed files vs upstream

| File | Change |
| ---- | ------ |
| `main/Kconfig.projbuild` | `BOARD_HUIDU_WF1` choice |
| `main/display.cpp` | WF1 HUB75 pin map; per-board + runtime-tunable panel driver/timing; NULL-`_matrix` guards; text clamped to panel width; temporary R/G/B bench test |
| `main/gfx.c` | S2 single-core guard; per-frame WebP decode at each frame's own offset with libwebp's key-frame/blend/dispose compositing; optional RGBA scratch, only taken when the heap can afford it; boot asset decoded in place from flash instead of copied to RAM; boot-debug text clamped to 10 chars so it fits 64 px; panel diagnostic now reports free heap *and largest free block* |
| `main/main.c` | Display-init failure no longer fatal; AP portal left running; log capture init |
| `main/mem_compat.h` **(new)** | `IMAGE_BUF_CAPS` — SPIRAM when available, internal RAM otherwise |
| `main/diag.c` / `main/diag.h` **(new)** | In-RAM log ring buffer, served by `/diag` |
| `main/ap.c` | `/diag` and `/panel` endpoints; `max_uri_handlers` raised |
| `sdkconfig.defaults.huidu-wf1` | S2 target, 4 MB flash, no SPIRAM, WF1 board, small boot asset, flash core dumps |
| `Makefile` | `huidu-wf1` build target |

## Other boards

This repo keeps full upstream history — Gen1, Gen2, Tronbyt-S3, MatrixPortal-S3, Waveshare-S3 and Pixoticker targets all still build via `make <board>` (see `make help`).

## Contributing

The most valuable contribution right now is help with the single remaining blocker: **getting libwebp a contiguous ~12.5 KB free block on a 320 KB, no-PSRAM ESP32-S2** while keeping the WiFi stack alive. Concretely:

- a heap free-block dump from a running board (the layout, not the totals — see [Work left to do](#work-left-to-do));
- measurements of what the WiFi driver and the config portal actually cost in `MALLOC_CAP_INTERNAL`;
- any working arrangement of task stacks, buffers or a pre-allocated decode arena that produces one large contiguous run.

Note that the panel itself is **not** in question any more — a full-screen fill and legible text prove the pinout, timing and colour order are correct, so panel-driver sweeps and logic-analyser captures of the HUB75 bus are no longer where the problem lives. PRs welcome.

## License & credit

Apache-2.0 — see [LICENSE](LICENSE). Upstream firmware by the [Tronbyt](https://github.com/tronbyt/firmware-esp32) community. HD-WF1 pin map and board research by [mrcodetastic](https://github.com/mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA). WF1 port by [@delight-f](https://github.com/delight-f).
