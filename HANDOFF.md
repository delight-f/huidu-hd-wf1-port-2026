# Handoff — HD-WF1 Tronbyt port bring-up

**Audience:** the next coding agent picking this up.
**Goal:** make the Huidu HD-WF1 render Tronbyt WebP images correctly on its 64×32 HUB75E panel.

**Current state in one line:** everything works except one allocation — the device boots, renders on the panel, joins WiFi, fetches real images from the Tronbyt server over HTTP, and the WebP decode logic is correct and verified; but libwebp needs a **single contiguous 12,544-byte block** for its ARGB working buffer and the largest free block the board can currently offer is **7,936 bytes**, so `WebPDecode()` fails on frame 1 of every image.

Read `README.md` first for the narrative, particularly finding 6. This document is the engineering continuation.

---

## 1. Acceptance criteria and what is already done

Done when pointing `REMOTE_URL` at a Tronbyt server produces a **recognisable image** on the 64×32 panel, stable across reboots, with the boot animation and config screens correct.

Already verified on hardware — **do not regress, and do not re-chase**:

- builds clean for `esp32s2` with native ESP-IDF v5.5; flashes; boots (reset reason 1 = power-on, no crashes)
- **panel rendering is correct**: boot version screen legible, full-screen R/G/B fills the right colours
- WiFi station join, DHCP
- **HTTP fetch from the Tronbyt server works** — the log shows `remote: Content-Length Header : 226` and `main: HTTP fetch returned in 26 ms`, repeatedly, for 226–360 byte images
- config portal, `/diag`, `/panel` all reachable over **both** the AP and the station address
- **the WebP decode logic in `main/gfx.c` is correct** — verified on the host, byte-identical to libwebp's own `WebPAnimDecoder` (see §6)

The single remaining blocker is the contiguous allocation. §4 is the whole problem.

> **The panel is not the problem.** An earlier phase swept panel driver / latch blanking / clock phase / pixel clock and those knobs do not fix this. They still exist (§8) but are not where the work is.

---

## 2. The board is observable over the network — use this

**This is the single biggest change from the previous handoff.** It said the panel was the only console. It is not: the firmware's `/diag` endpoint is reachable on the **station** address, so you can read the device's full captured log and heap figures from your workstation.

```sh
curl -s http://192.168.10.138/diag
```

`reset_reason`, `free_heap`, `free_internal`, **`largest_internal`**, `free_dma`, `largest_dma`, then `--- captured log ---` followed by the in-RAM log ring (1 KB). The IP is DHCP; if it changes, scan the /24 for a box serving `/diag`.

Decode failures show up as `gfx: frame 1 decode failed`, and this is the fastest way to see them. Use it instead of asking a human to read the panel.

**Panel diagnostics still exist** and are readable by eye. `diag_panel()` in `gfx.c` draws four lines, each truncated to 10 chars:

```
stk  NNNN    gfx task stack high-water mark
webp NNNN    length of the WebP handed to the decoder
dec  X       stage and result: OK | info ERR | buf ERR | dmux ERR | dec ERR
hNNk bNNk    free heap and LARGEST FREE BLOCK, both in KiB
```

`h`/`b` is the pair that matters — `b` (largest contiguous block) is what decides whether a decode fits, and it is reported precisely because free heap alone was misleading.

There is also a boot screen (`display_initialize()`) showing `heap/int/dma/cfg`, and `/diag` over the AP at `10.10.0.1` as a fallback.

**There is no serial console.** The WF1 has no UART bridge and `CONFIG_ESP_CONSOLE_USB_CDC` does not enumerate on ESP32-S2 in ESP-IDF without TinyUSB. Do not spend time on it.

---

## 3. Environment

| Item | Value |
| --- | --- |
| Repo | `~/dev/huidu-hd-wf1-port-2026` (remote `github.com/delight-f/huidu-hd-wf1-port-2026`, branch `main`) |
| ESP-IDF | `~/esp/esp-idf` (v5.5) |
| Device port | `/dev/ttyACM0` in download mode |
| Device (on LAN) | `192.168.10.138` — serves `/diag` |
| Tronbyt server | `192.168.10.201:8000`, device id `led-screen-1` |
| Device | HD-WF1, **ESP32-S2 rev v1.0, no PSRAM**, 4 MB flash, 64×32 HUB75E, **FM6124** LED driver ICs |

### Build / flash

```bash
export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1
. $IDF_PATH/export.sh
cd ~/dev/huidu-hd-wf1-port-2026
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
idf.py -p /dev/ttyACM0 flash      # board must be in download mode
```

`IDF_PATH_FORCE=1` is needed because the shell is `dash`, where `export.sh` cannot detect `IDF_PATH` from `BASH_SOURCE`.

Download mode: bridge the two GPIO0 pads near the Micro-USB port, power-cycle, confirm `303a:0002` / `/dev/ttyACM0` appears. USB-A-to-USB-A cable into the **USB-A** port.

---

## 4. The remaining blocker, with numbers

`WebPDecode()` for one 64×32 lossless frame allocates **~18–26 KB transiently**, as two allocations that are live at the same time:

| Allocation | Size | Note |
| ---------- | ---- | ---- |
| `dec->pixels` (ARGB working buffer) | **12,544 B** | must be **contiguous** |
| Huffman tables / htree groups | ~5–12 KB | data-dependent |

The board reports:

```
free_internal    19,924 B
largest_internal  7,936 B     <-- must be >= 12,544
```

**~20 KB is free; ~18 KB is needed.** Total capacity is sufficient — the failure is that the free space is shredded and the largest usable run is under 8 KB. Every image therefore fails at `frame 1`, however small (they are 226–360 bytes).

The heap is **stable**, not leaking: sampled repeatedly at 20,364 / 20,368 / 20,364 / 20,376 B free, largest block 9,216 B throughout.

So this is a **heap-layout** problem, not a capacity one. That distinction is the whole basis for the next steps: capacity cannot be conjured, layout can be influenced.

**The decoder's requirement is irreducible.** Measured on the host (`tools/webp-host-harness`):

- cropping: peak **unchanged** (18,200 B) — cropping is applied on output, the full image is still decoded internally
- scaling to half: peak **increases** (18,200 → 19,456 B) — the rescaler adds memory and does not shrink the full-size pixel buffer

Do not spend time trying to talk libwebp into a smaller working set. It cannot.

---

## 5. Ranked next steps

Cheapest and safest first.

1. **Move our own buffers out of the middle of the heap.** `gfx.c` allocates its 6 KB canvas inside the first `draw_webp()`, which runs *after* WiFi, the HTTP server, the WebSocket client and all the task stacks have come up — so it lands mid-heap and may itself be splitting the largest free run. Allocating it (and the RGBA scratch) before the network stack starts costs nothing and could recover most of the missing contiguity. This is the cheapest untried lever and the first one to try.
2. **Measure the heap's shape instead of inferring it.** Add a free-block dump to `/diag` — `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)` logs a per-region summary including free-block counts, which fits the 1 KB ring. Three previous fixes were aimed from totals rather than from the layout and all three missed. One `?heap=1` query parameter on the existing `diag_handler()` in `main/ap.c` is enough. **Do this before the next speculative fix.**
3. **Retire the portal's SoftAP once the station link is up.** The device runs APSTA permanently by deliberate choice (`main.c`: "AP portal left running (auto-shutdown disabled)"). While the station link is up the SoftAP is pure overhead: radio buffers, a DHCP server, and a 4 KB captive-portal DNS task. Upstream shuts it down for this reason. `ap_stop()` is too blunt — it also stops the web server and would kill `/diag` — so stop the DNS task and switch to `WIFI_MODE_STA` while leaving `s_server` running; `/diag` then remains reachable on the station address. **Log free/largest before and after so the effect is measured rather than assumed.**
4. **Force a contiguous arena** if the layout turns out to be immovable: allocate a block early (while DRAM is intact), release it immediately before `WebPDecode()` so libwebp's allocations land in a known-good run, then re-acquire best-effort. Costs heap while held, so it needs care not to starve the HTTP client — the fetches need working TCP buffers.

---

## 6. What was already tried — do not repeat

Each of these was implemented and measured on the bench. None helped. The measurements matter more than the conclusions, because several of them are counter-intuitive:

| Tried | Result |
| --- | --- |
| Trim WiFi buffers (`ESP_WIFI_STATIC_TX_BUFFER_NUM` 16→8, `STATIC_RX_BUFFER_NUM` 10→8, `MGMT_SBUF_NUM` 32→16, AMPDU off) | **no effect** — 20,368 → 19,924 B free, 9,216 → 7,936 B largest, i.e. noise. Those buffers evidently are not in the measured region. |
| Move WiFi code out of IRAM (`CONFIG_ESP_WIFI_IRAM_OPT=n`, `RX_IRAM_OPT=n`) | frees 23 KB of DIRAM at link time (40,372 → 63,532 remain) but makes the **runtime heap worse**: 24,948 → 20,368 B free, 14,848 → 7,936 B largest. The extra DIRAM does not reach the data heap. |
| Lower the panel's BCM colour depth 8 → 5 (should be 6 KB) | `free_internal` did not move at all. |
| Disable lwIP IPv6 | not pursued: `main/ota.c` uses IPv6 socket types unconditionally, and OTA is impossible on this board anyway. |
| Decoder cropping / scaling options | see §4 — irreducible. |
| Reduce the canvas by decoding to a smaller scratch | still needed; but note the *still-image* path already avoids the RGBA scratch entirely. |
| `WebPAnimDecoder` | needs two full canvases **plus** the same working set. Strictly worse. |

Two traps worth naming explicitly, because both cost real time:

- **The panel comes up showing collapsed rows immediately after a flash.** Full-screen fills light one or two rows, no text is legible. **A power-cycle clears it.** This reproduces on stock config, so it is a post-flash initialisation transient, not a firmware fault — do not go hunting it in the code, and always power-cycle before judging the panel.
- **tronbyt-manager reporting the device "offline" does not mean it is not connected.** The UI's "last seen" tracks the WebSocket, while the display is driven by HTTP polling. The device was fetching images successfully throughout a period when the UI showed it 16 minutes stale. Use `/diag` or the server's own access log, not the UI's presence indicator.

---

## 7. Code map

| File | Why it matters |
| --- | --- |
| `main/gfx.c` | **the file to work in.** `draw_webp()` — per-frame decode at each frame's own offset, optional RGBA scratch + key-frame/blend/dispose compositing, `GFX_DECODE_HEADROOM` gate, panel diagnostic; `gfx_state.buf_is_static` for the flash-resident boot asset; `GFX_TASK_CORE 0` (S2 is single-core); `GFX_TASK_STACK_SIZE 6144` |
| `main/display.cpp` | WF1 pin map (~135-151); `PANEL_*_DEF` defaults + `panel_cfg_get()` NVS overrides; `display_fill_screen()`, `display_diag_show()`; NULL-`_matrix` guards; **temporary R/G/B bench fills + boot diagnostic** in `display_initialize()` |
| `main/main.c` | `diag_init()` at top of `app_main`; display-init failure non-fatal; AP auto-shutdown disabled (~683) |
| `main/ap.c` / `.h` | `/diag`, `/panel`, `/save`, `/update` endpoints; `ap_start()`, `ap_stop()`; `max_uri_handlers` raised |
| `main/wifi.c` | STA config, reconnect logic, `wifi_shutdown()` |
| `main/diag.c` / `.h` | in-RAM log ring (1 KB) served by `/diag` |
| `main/mem_compat.h` | `IMAGE_BUF_CAPS` — SPIRAM when available, internal RAM otherwise |
| `main/panel_sweep.c` / `.h` | GPIO11/GPIO0 button → panel-config cycler + boot indicator colour |
| `sdkconfig.defaults.huidu-wf1` | S2, 4 MB, no SPIRAM, small boot asset, `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, HTTP buffers 60000/4000 |
| `boards/max_app_4mb.csv` | single `factory` slot — **OTA impossible** without a new table |
| `tools/webp-host-harness/` | **build this first.** Measures the decoder's transient heap and renders PPM montages comparing decode strategies against libwebp's own `WebPAnimDecoder`. All the numbers in §4 and §6 came from it, and the correctness of `gfx.c`'s compositing was established here. Seconds per iteration instead of a flash. |

### Panel-config tunables (exist, but are NOT the current problem)

`/panel?drv=&line=&spd=&lat=&ph=&dbfr=` (and `?clear=1`) override the panel config in NVS and reboot; the GPIO11/GPIO0 button cycles presets. **These live in NVS and survive reflashing** — use `/panel?clear=1` or `idf.py erase-flash` before comparing against compiled defaults. Current WF1 defaults: `FM6124`, `TYPE138`, 20 MHz, latch blanking 1, double buffering off.

---

## 8. Gotchas

- **NVS persists across reflashes** — panel overrides, WiFi creds and `image_url` all survive. `idf.py erase-flash` for a clean slate (but you will then need to re-enter WiFi credentials through the portal).
- **No serial console.** Use `/diag` over the network (§2) or the panel diagnostics.
- **Power-cycle after flashing, before judging the panel** (§6).
- The device runs an **APSTA** config: SoftAP `TRON-CONFIG` at `10.10.0.1` permanently, plus the station link. `/diag` works on both.
- `secrets.json` / `secrets.cmake` hold real credentials and are gitignored.
- Untracked by choice: the two spec PDFs and `.commandcode/`.
- `managed_components/` is gitignored and fetched by the component manager — do not edit it; changes will not survive.

---

## 9. Cleanup before merging

1. Remove the temporary R/G/B fills and boot diagnostic from `display_initialize()` (`display.cpp`).
2. Decide the fate of the panel-config tunables and the button cycler (probably keep; they cost little).
3. Add `huidu-wf1` to the GitHub Actions matrix **and** an `esp32s2` case to its chip dispatch (it currently maps everything non-S3 to `esp32`).
4. Add `huidu-wf1` to `esp_idf_project_configuration.json`.
5. Update `README.md` status once images render.

---

## 10. Commits so far (branch `main`)

- `459a09b` fix(wf1): decode WebP frames at their own offset and composite them
- `a22a2fe` docs: record that the panel is fine and libwebp memory is the real blocker
- `04f1a1f` feat(wf1): low-memory WebP decode path and on-panel diagnostics
- `6a38acf` feat(wf1): button-driven panel-config cycler for bench bring-up
- `2fb0644` docs: add HANDOFF.md for the next agent
- `1c64019` docs: record HD-WF1 bring-up status, findings and remaining work
- `6774141` fix(wf1): survive the no-PSRAM S2 and add on-device diagnostics
- `611a34f` fix(wf1): run on the no-PSRAM S2 and align HUB75 panel timing
