# Tronbyt on the Huidu HD-WF1

**Tronbyt LED-matrix firmware, ported to an ESP32-S2 board with no PSRAM — and the memory engineering that took.**

[![ESP-IDF v5.5](https://img.shields.io/badge/ESP--IDF-v5.5-blue?logo=espressif&style=flat-square)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s2/)
[![Target ESP32-S2](https://img.shields.io/badge/target-ESP32--S2-red?style=flat-square)](https://www.espressif.com/en/products/socs/esp32-s2)
[![Panel 64×32 HUB75E](https://img.shields.io/badge/panel-64%C3%9732_HUB75E-orange?style=flat-square)](.)
[![Status bench bring-up](https://img.shields.io/badge/status-bench_bring--up-yellow?style=flat-square)](.)
[![License Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-green?style=flat-square)](LICENSE)
[![Upstream](https://img.shields.io/badge/upstream-tronbyt%2Ffirmware--esp32-lightgrey?style=flat-square)](https://github.com/tronbyt/firmware-esp32)

The [Tronbyt](https://github.com/tronbyt/firmware-esp32) firmware pulls WebP artwork from a server and paints it onto an LED matrix — over plain HTTP polling or a WebSocket push, with a captive-portal WiFi setup. This repository brings that firmware to the **Huidu HD-WF1**: an ESP32-S2 with **320 KB of SRAM, no PSRAM, 4 MB of flash, and a single core**, driving its stock **64×32 HUB75E** panel.

That hardware is the interesting part. Every board Tronbyt already supported has PSRAM and, usually, two cores. The WF1 has neither, so a stock build does not survive it — the differences are not tuning but structural, and they land hardest on **animated WebP**, which is what this document spends most of its length on.

> [!NOTE]
> **Where this stands.** This is a working bench bring-up, not a finished release. The panel, the pinout, the network path and the WebP decode logic are all verified; the memory problem that blocked earlier sessions is solved and measured. What is *not* finished is stability — the board resets after roughly a minute for a reason that is not yet established — and large animated apps cannot render at all, for a reason that looks fundamental to the chip. See [Status](#status-and-remaining-work) for the honest summary of what is verified and what is still a hypothesis.

---

## At a glance

| | |
| --- | --- |
| **Board** | Huidu HD-WF1 — ESP32-S2, 4 MB flash, no PSRAM, single core |
| **Panel** | 64×32 HUB75E, FM6124 driver ICs |
| **Display path** | `WebPDemux` + per-frame `WebPDecode`, composited in a reused RGB canvas |
| **Transports** | HTTP polling (`http://`) and WebSocket push (`ws://` / `wss://`) |
| **Config** | `secrets.json` → Kconfig defaults, or the on-device WiFi captive portal (NVS) |
| **Observability** | `/diag` (log ring, heap layout, reset reason), `/panel` (panel tuning), on-panel diagnostics |
| **Verified on hardware** | build & flash, boot, WiFi join, HTTP fetch, boot screen, config portal, panel output |
| **Known open** | ~1-minute reset loop (cause unknown), refusing 20–33 KB animated apps, receive path stalls near 14 KB |
| **Upstream base** | `tronbyt/firmware-esp32`, rebased onto `5013f42` (PR #161) |

---

## Hardware

| Item | Detail |
| ---- | ------ |
| Controller | Huidu HD-WF1 (ESP32-S2, single core, 4 MB flash, **no PSRAM**) |
| Panel | 64×32 HUB75E |
| Panel driver ICs | **FM6124** (`fm6124init()` is run; the ICs must be initialised) |
| Address lines | A–D (1/16 scan); the E line is wired to GPIO 12 |
| Button | GPIO 11 (config mode at boot; panel-config cycler while running) |
| Partition table | `boards/max_app_4mb.csv` — one `factory` slot, **no OTA** |
| Serial console | **none** (no UART bridge; ESP32-S2 USB-CDC does not enumerate without TinyUSB) |
| Download mode | Bridge the two GPIO0 pads beside the Micro-USB port, then power-cycle |

### WF1 → HUB75E pin map

Reverse-engineered and checked byte-for-byte against [mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA](https://github.com/mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA) (`hd-wf1-esp32s2-config.h`):

| Signal | GPIO | Signal | GPIO |
| ------ | ---- | ------ | ---- |
| R1 | 2 | G1 | 6 |
| B1 | 3 | R2 | 4 |
| G2 | 8 | B2 | 5 |
| A | 39 | B | 38 |
| C | 37 | D | 36 |
| E | 12 | LAT | 33 |
| OE | 35 | CLK | 34 |

---

## Quick start

**Prerequisites:** [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) (native install) and Python 3.

```bash
git clone https://github.com/delight-f/huidu-hd-wf1-port-2026.git
cd huidu-hd-wf1-port-2026

cp secrets.json.example secrets.json      # WiFi + the URL the server serves
# { "WIFI_SSID": "mySSID", "WIFI_PASSWORD": "<PASSWORD>",
#   "REMOTE_URL": "http://yourServer:8000/<deviceID>/next" }

. $HOME/esp-idf/export.sh
make huidu-wf1                            # configures, builds, emits a merged image
idf.py -p /dev/ttyACM0 flash
```

Prefer `idf.py` directly? The Makefile target is just a convenience wrapper:

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
```

**Flashing needs download mode.** Bridge the GPIO0 pads, power-cycle, and confirm `303a:0002` (esptool) plus `/dev/ttyACM0` before flashing. The running application deliberately does not enumerate USB, so a missing `/dev/ttyACM0` during normal operation is expected, not a fault.

> [!WARNING]
> Two traps that have each cost a debugging cycle:
> - **`sdkconfig` is not re-derived from the defaults once it exists.** After editing `sdkconfig.defaults.huidu-wf1` you must `rm -f sdkconfig` and re-run `set-target`, or you are building the old configuration.
> - **Power-cycle before judging the panel.** A warm reset leaves the FM6124s in a state where the panel shows collapsed rows. Every flash ends in a reset, so this looks like a firmware regression when it is not. Only a cold power-cycle clears it.

---

## Fitting Tronbyt onto a 320 KB, no-PSRAM ESP32-S2

The WF1 is not a smaller version of a supported board; it removes two things the firmware was quietly built around — **PSRAM** and a **second core** — and shrinks the third. Getting Tronbyt to boot, light the panel and talk to the server took a series of changes that are structural rather than cosmetic. They are listed here in the order of how much they matter.

### 1. Every image and portal buffer was being allocated from PSRAM that does not exist

Upstream allocates the HTTP receive buffer, the WebSocket reassembly buffer and the config-portal scratch with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`. On a no-PSRAM chip this returns `NULL` **with no fallback**, so HTTP fetch, WebSocket reassembly and portal save all fail silently.

**Fix:** `main/mem_compat.h` defines `IMAGE_BUF_CAPS` as `MALLOC_CAP_SPIRAM` when PSRAM is present and `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` when it is not. PSRAM boards keep the old behaviour; the S2 falls back to internal RAM.

### 2. The boot animation could not be allocated, and that failure was fatal

`gfx_initialize()` copied the selected boot WebP into one `calloc()`. The WF1 inherited `CONFIG_BOOT_WEBP_TRONBYT` — **174,330 bytes** — which cannot be a single contiguous block once WiFi is up. Worse, `main.c` treated a failed `gfx_initialize()` as fatal and returned from `app_main`, taking down the config portal with it (the AP SSID still broadcasts because the radio does that, not the CPU, so the device looked healthy while the app had already exited).

**Fix:** the WF1 uses the ~4.5 KB `BOOT_WEBP_PARROT` asset, and a display-init failure is no longer fatal. The boot asset is also handed to the decoder **in place from flash rodata** rather than copied into RAM — rodata is memory-mapped and `WebPDecode` only reads it.

### 3. Double buffering could not be allocated, which kept the panel dark

The HUB75 driver allocates its BCM framebuffer in **DMA-capable internal RAM** — roughly 1 KB per row per bit depth, and **double** that with double buffering. The port had hard-coded `double_buff = true` for every board. On the S2 that allocation fails, `setupDMA()` returns false, `_matrix` stays `NULL`, and nothing is ever drawn.

**Fix:** double buffering is off for the WF1 (matching the driver library's own default). Turning it off is what finally lit the panel.

### 4. A `NULL` matrix then crashed the device

With display init made non-fatal, execution continued into `display_clear()`, which was a bare `_matrix->clearScreen()`. That panicked the device about a minute after boot — and because the panic happened *after* the portal came up, it presented as "the config portal is flaky" rather than "the display failed".

**Fix:** `NULL` guards on the draw path.

### 5. Single-core pinning

The S2 has one core, and upstream pinned the graphics task to core 1. `GFX_TASK_CORE` is now `0` on `CONFIG_IDF_TARGET_ESP32S2`.

### 6. The decode canvas is reserved *before* the network stack

This is the change that actually solved the memory problem. `gfx_reserve_decode_buffers()` runs first in `app_main` — before WiFi, the HTTP server, the WebSocket client and every task stack have carved up the heap — so the 6 KB RGB canvas (64×32×3) lands early and the network stack works around it.

The canvas is allocated once and held for the life of the program either way, so reserving it early costs nothing. It only changes *where* it lands, and *where* is the whole game: libwebp needs one **contiguous** 12,544-byte block, and a canvas allocated lazily inside the first `draw_webp()` is quite capable of splitting the largest free run in two.

### 7. WiFi receive buffers are paid for early, not on demand

The WiFi driver's RX pool is drawn from **on demand**, and a large image response makes the device allocate a large receive buffer first — fragmenting exactly the heap the receive path is about to need. The symptom was transfers dying after 1–4 KB and taking 30–50 s.

**Fix:** the same budget spent *up front*, while the heap is still one piece — `STATIC_RX=12`, `DYNAMIC_RX=4`, `STATIC_TX=10`, A-MPDU off. Transfers now reach ~14 KB where nothing previously passed 4 KB.

> [!IMPORTANT]
> The stock counts (static 10 / dynamic 32) fetch reliably but cost 20.6 KB — enough to starve the boot animation's own decode. Trimming hard (static 6 / dynamic 8) frees the memory but cannot receive a 22 KB body at all. **The middle, paid early, is the working point.** The comments in `sdkconfig.defaults.huidu-wf1` carry the measurement behind every one of these numbers.

### 8. The setup SoftAP is retired once the station link is up

While the station link works, the SoftAP is pure overhead: radio buffers, a DHCP server and a 4 KB DNS task stack, all competing with the decoder. `ap_retire_softap()` stops the DNS hijack and the AP's DHCP server and switches to station mode — but deliberately **leaves the web server running**, so `/diag` and the config pages stay reachable on the station address. Upstream's equivalent takes the web server down with the AP, which on this board would remove its only console. The captive-portal DNS hijack is likewise deferred until the portal is actually going to be used.

### 9. The composite scratch is a cache, not a fixture

Multi-frame images need an 8 KB RGBA scratch to composite correctly. It used to be allocated on first use and held forever — so one animation early in a session permanently taxed every still frame afterwards, and that 8 KB was the difference between a 31 KB payload decoding and not. It is now released whenever a decode will not composite, and re-taken when one can.

### 10. A receive-buffer ceiling, so oversized apps are refused instead of half-fetched

`CONFIG_HTTP_BUFFER_SIZE_MAX` dropped from 60,000 to **16,384**, which turns an oversized app into an immediate `413` and the built-in "too big" screen rather than a half-fetched 22 KB animation that holds its payload and grinds through every frame failing.

### 11. Task stacks trimmed against measurements, not guesses

Main task `6144` (watermark logged on every fetch: ~3.3 KB headroom), graphics task `6144`, timer task `2048`. The numbers are only trimmed once a watermark says they can be.

### What that bought

Boot heap, before and after (free / **largest contiguous block**):

| Stage | Before | After |
| --- | --- | --- |
| After WiFi init | 68,440 / 57,344 | **89,040 / 77,824** |
| After display init | 19,284 / 11,264 | **31,696 / 23,552** |
| After `ap_start` | 6,148 / 3,584 | **41,084 / 32,768** |

Largest free block went **7,936 → 36,864 bytes**; total internal heap roughly **20 KB → 52 KB**. The earlier blocker — libwebp unable to obtain a single contiguous 12,544-byte allocation — is gone.

### Tried, measured, and rejected

Recorded so the next person does not repeat them:

| Attempt | Result |
| --- | --- |
| Move WiFi code out of IRAM (`IRAM_OPT=n`) | Frees DIRAM at link time, makes the **runtime** heap worse (24,948 → 20,368 free) |
| Lower the panel's BCM colour depth 8 → 6 | Works, and is a bigger lever than expected (panel stage 44,808 → 22,100 B) — **but it changes panel timing**, since `nsPerRow` scales with depth |
| Raise `TCP_WND` to 11,680 | Much worse: 1,226-byte payloads went from 36 ms to 39–42 s |
| Over-trim `STATIC_RX` / `DYNAMIC_RX` | Breaks the transport entirely (see §7) |
| Decoder cropping / scaling options | Cropping is output-only (peak unchanged); scaling is **worse** (18,200 → 19,456 B) |
| `WebPAnimDecoder` | Needs two full canvases plus the VP8 working set — strictly worse than driving the demuxer |
| libwebp's internal 12,544-byte buffer | Not shrinkable by any decoder option (`AllocateInternalBuffers32b` allocates it regardless) |
| Disable lwIP IPv6 | Not viable — `main/ota.c` uses IPv6 socket types unconditionally |
| Panel timing sweeps | Not the cause; text and solid fills render correctly throughout |

---

## Animated WebP: the constraint that shapes everything

Once the panel is known-good and the network path is known-good, one problem remains, and it is the one that has to be understood before touching this port.

**The compressed payload must stay resident while libwebp decodes it.** The requirement is therefore not "the larger of the payload and the decoder" — it is

```
   payload  +  decoder working set      (both live at once)
```

Decoding a single 64×32 lossless frame costs **~18–26 KB transient**, in two allocations that are live simultaneously:

| Allocation | Size | Note |
| --- | --- | --- |
| `dec->pixels` — the ARGB working buffer | **12,544 B** | must be a single **contiguous** block |
| Huffman tables / htree groups | ~5–12 KB | data-dependent |

Measured on the host harness against the assets the server actually serves (`tools/webp-host-harness`):

| Payload | Decoder transient | Required live | Board free | Outcome |
| --- | --- | --- | --- | --- |
| 360 B | ~11.9 KB | ~12 KB | ~41 KB | decodes |
| 11,305 B | ~18 KB | ~30 KB | ~41 KB | decodes |
| 22,000 B | ~20–26 KB | ~42–48 KB | ~41 KB | **fails** |
| 33,084 B | 25,864 B | ~59 KB | ~41 KB | **fails** |

Read the sharpness of that table: **11 KB fits with room, 22 KB does not.** The server emits 226–360 byte stills, ~11 KB mid-size apps, and **20–33 KB animated apps** for a 64×32 panel. So:

- **The oversized apps are animations, and they cannot be made to fit by tuning.** No buffer arrangement moves a 59 KB requirement into a 41 KB budget. Hence `CONFIG_HTTP_BUFFER_SIZE_MAX = 16384` (≈15 KB practical ceiling) and the built-in oversize screen.
- **Streaming decode is the right structural fix — but only for stills.** Feeding the bitstream to libwebp as it arrives means the file is never resident, which solves the still-image case. It does **not** help animations, which need the complete file for frame iteration. That is precisely the case that is oversized here.
- **The real fix belongs on the server.** Artwork for a 64×32 panel should be a few KB. If the Tronbyt server exposes a quality or size setting for constrained devices, that solves this properly rather than working around it in firmware.

### How decode works, and why it is built this way

`WebPAnimDecoder` was the first casualty: it allocates two full canvas buffers internally plus VP8 state, and its constructor returns `NULL` on this board. `gfx.c` therefore drives the container directly — `WebPDemux` to walk frames, `WebPDecode` per frame with `output.is_external_memory = 1` — into two buffers it owns and reuses:

| Buffer | Size at 64×32 | Lifetime |
| --- | --- | --- |
| `s_canvas` — composited RGB, what the panel shows | 6 KB | held for life, **reserved before WiFi** |
| `s_frame` — one frame decoded to RGBA, for blending | 8 KB | taken only when collapsing is affordable, else released |

Two facts make this exact rather than approximate:

- **libwebp decodes in place**, so its own animation decoder needs a second canvas to keep a disposed copy to blend against. Decoding each frame into a *scratch* instead lets the canvas itself hold the previous frame — one extra buffer instead of two canvases.
- Key-frame / blend / dispose semantics are mirrored from `anim_decode.c` (`is_key_frame()`, `composite_frame()`, `dispose_frame()`). **The result is byte-identical to `WebPAnimDecoder`'s** on the still and animated assets, verified with the host harness; the only divergence is the deliberate `no_fancy_upsampling` choice on lossy frames.

When the heap cannot afford the scratch (`GFX_DECODE_HEADROOM`, 30 KB), frames are decoded **directly into the canvas at each frame's own x/y offset**. That is correct — and it was a bug once: the original code decoded every frame at the canvas *origin* with the canvas stride, so any frame smaller than the canvas (most animation frames) landed in the wrong place. The direct path positions frames correctly but does not blend partial frames over their predecessors, which is the visible trade-off when memory is short.

One more failure mode was closed: if nothing has decoded, the frame loop now **breaks out** rather than grinding through all remaining frames of an undecodable animation while holding its payload — which is what made the board look hung instead of merely showing an error.

> [!TIP]
> `tools/webp-host-harness` is the fastest way to work on any of this. It compiles against the same vendored libwebp, measures the decoder's transient heap with `malloc` intercepted, and renders side-by-side montages (`ref` / `naive` / `offset` / `scratch`) so the compositing can be diffed on a workstation in seconds instead of by flashing the board. Every decoder number on this page came from it.

### A related bug worth knowing about

The board was fetching about **1.2 images a second against a `Tronbyt-Dwell-Secs: 10`** header — roughly 70 polls a minute, indefinitely. The cause: `gfx` consumes the dwell itself for animations, but a still image is a single frame, so the graphics loop returned immediately and the main loop fetched again at once. The main loop now sleeps only the *remainder* of the dwell, so animations are not held for twice the requested time.

---

## Seeing inside a board with no console

The WF1 has no UART-to-USB bridge, and ESP32-S2 USB-CDC does not enumerate in ESP-IDF without TinyUSB. `idf.py monitor` has nothing to attach to, so observability is part of the port rather than throwaway debug code.

| Surface | What it does |
| --- | --- |
| `GET /diag` | Captured log ring, reset reason, free heap / free DMA heap |
| `GET /diag?heap=1` | Free-block **distribution** and httpd/DNS task stack headroom |
| `GET /panel?...` | HUB75 driver/timing overrides in NVS, then reboot |
| On-panel `diag_panel()` | Four short lines: `stk`, `webp`, decode stage (`OK` / `info ERR` / `buf ERR` / `dmux ERR` / `dec ERR`), `hNNk bNNk` = free heap and **largest free block** |
| Button cycler | Short GPIO 11 press steps through candidate panel configs, saves to NVS, reboots — identifiable by solid boot colour |
| Coredumps | `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`; a panic can be decoded later with `espcoredump.py` |

`reset_reason` is the quickest stability signal: `1` = power-on, `3` = software reset, `4` = **panic**.

The on-panel diagnostic deliberately reports the **largest free block**, not just total heap. Three earlier fixes were aimed from totals rather than from the heap's shape and all three missed — a 12,544-byte contiguous requirement is a statement about the shape, not the total.

> [!NOTE]
> A red dot plus `dec ERR` on the panel may be **stale**. Nothing redraws the panel when an image later succeeds, so read `/diag` before believing the panel.

### Runtime panel tuning

```
http://<device-ip>/panel?drv=1&lat=1     # driver / latch blanking
http://<device-ip>/panel?ph=0            # clock phase
http://<device-ip>/panel?spd=0           # 8 MHz instead of 20 MHz
http://<device-ip>/panel?dbfr=1          # double buffering
http://<device-ip>/panel?clear=1         # back to compiled-in defaults
```

`drv`: `0`=SHIFTREG, `1`=FM6124, `2`=FM6126A, `3`=ICN2038S, `4`=MBI5124, `5`=DP3246.

The compiled defaults for the WF1 are **FM6124**, `TYPE138` line addressing, 20 MHz, latch blanking 1, double buffering **off**, 8-bit BCM. These overrides live in NVS and **survive reflashing** — clear them before comparing against the compiled defaults. Note that GPIO0 is both the download-mode strap and a panel-sweep probe pin.

---

## Configuration

Two tiers, by design:

- **`secrets.json`** — deployment-specific values (WiFi credentials, `REMOTE_URL`, API key, custom CA). Parsed at build time by `generate_secrets_cmake.py` and injected as macros. At runtime the firmware prefers the **NVS** values written by the config portal, so a device can be re-pointed without a rebuild.
- **Kconfig / `sdkconfig.defaults.<board>`** — structural settings: target, flash size, partition table, presence of PSRAM, panel defaults, and the memory knobs above. Use `idf.py menuconfig` (or edit the defaults file) for these.

Channel order is now software-configurable on **every** board via `COLOR_ORDER` (Kconfig, the portal dropdown, or the WebSocket), applied to pixel data after the board's pin map and any `swap_colors` exchange — so a panel with permuted RGB lines no longer needs a board-specific pin hack.

---

## Project layout

```
main/
  main.c            app_main: boot order, HTTP poll loop, WebSocket client, dwell handling
  gfx.c             the decode path — canvas reservation, demux + per-frame decode, compositing
  display.cpp       per-board pin maps and panel defaults; WF1 HUB75 map; NULL guards
  remote.c          HTTP fetch; receive-buffer sizing; completeness checks
  ap.c / ap.h       config portal, /diag, /panel, /save, /update; SoftAP retirement
  diag.c / diag.h   log ring, boot heap trace, free-block histogram
  mem_compat.h      PSRAM-or-internal allocation caps
  panel_sweep.c/.h  bench panel-config cycler (button-driven)
  nvs_settings.c/.h persisted settings
  Kconfig.projbuild board choices and feature flags
components/assets/  built-in WebP screens (boot, config, 404, oversize, no-connect)
boards/             partition tables (max_app_4mb.csv = single factory slot, no OTA)
tools/webp-host-harness/  host-side decoder measurement and render comparison
sdkconfig.defaults.huidu-wf1  the WF1's tunables, each with its measurement
```

---

## Status and remaining work

**Verified on hardware:** builds and flashes under ESP-IDF v5.5 for `esp32s2`; boots and pins tasks on the single core; joins WiFi and gets DHCP; fetches real images from the Tronbyt server in tens to a few hundred milliseconds (43–326 ms measured, including 11 KB bodies); renders the boot animation and version screen with correct colour order and blanking; serves the config portal, `/diag` and `/panel` over both interfaces; the decode/compositing logic is byte-identical to libwebp's own animation decoder on the host harness.

**Open, in priority order:**

1. **The ~1-minute reset.** One panic was captured and decoded and turned out to be a bug introduced during the bring-up (a `NULL` dereference in `remote.c`); a second was captured but not decoded, and no evidence yet says whether a second cause remains. **Preserve the ELF and decode the coredump before changing anything else** — a backtrace names the frame, which beats reasoning from symptoms.
2. **Large animated apps.** 20–33 KB apps cannot fit (see [the animated-WebP section](#animated-webp-the-constraint-that-shapes-everything)). The options are to cap what the board attempts — the oversize screen now works, since it decodes in place from flash — or to fix the asset sizes at the server, which is where the problem really belongs.
3. **The receive path** reaches ~14 KB and stalls on a 33 KB body; why it stops there is not established.
4. **The WebSocket path** is unverified against `tronbyt-manager` (the UI's "last seen" tracks the WebSocket, not the HTTP polling that actually drives the display).
5. **Nothing above is validated end-to-end.** Treat everything outside the verified list as a hypothesis, and do not trust a build until it has run for ten minutes without a reset.

Smaller items: CI entry and `esp_idf_project_configuration.json` for `huidu-wf1`; OTA (impossible today — `boards/max_app_4mb.csv` has one `factory` slot, and two ~1.75 MB slots would fit a ~1.2 MB app in 4 MB); a third-party brightness ceiling for the WF1 (it currently falls back to the legacy 230 ≈ 90% duty); and removing the temporary bench diagnostics or gating them behind Kconfig.

---

## Other boards

Full upstream history is preserved, so every upstream target still builds: `tidbyt-gen1`, `tidbyt-gen1_swap`, `tidbyt-gen2` (and their `-patched` variants), `tronbyt-s3`, `tronbyt-s3-wide`, `pixoticker`, `matrixportal-s3`, `matrixportal-s3-square`, `matrixportal-s3-waveshare` and `waveshare-s3`. Run `make help` for the list.

---

## Credits & license

Apache-2.0 — see [LICENSE](LICENSE).

Upstream firmware by the [Tronbyt](https://github.com/tronbyt/firmware-esp32) community. HD-WF1 board research and pin map by [mrcodetastic](https://github.com/mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA). WF1 port by [@delight-f](https://github.com/delight-f).
