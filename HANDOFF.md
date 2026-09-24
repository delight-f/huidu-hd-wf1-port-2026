# Handoff — HD-WF1 Tronbyt port bring-up

**Audience:** the next coding agent picking this up.
**Goal:** make the Huidu HD-WF1 render Tronbyt WebP images correctly on its 64×32 HUB75E panel.

**Current state in one line:** the firmware builds, flashes, boots, joins WiFi, reaches the Tronbyt server, and the panel itself is *demonstrably fine* (text and solid fills render correctly) — the remaining blocker is that **libwebp cannot decode on this chip's memory budget**, and the custom low-memory decode path added to work around it currently fails at the per-frame decode step (`dec1 ERR`).

Read `README.md` first for the narrative. This document is the engineering continuation.

---

## 1. Objective and acceptance criteria

Done when pointing `REMOTE_URL` at a Tronbyt server produces a **recognisable image** on the 64×32 panel, stable across reboots, with the boot animation and config screens correct.

Already accepted as working (**do not regress, and do not re-chase**):
- builds clean for `esp32s2` with native ESP-IDF v5.5; flashes; boots
- WiFi station join + DHCP; reaches the Tronbyt server (device visible in tronbyt-manager)
- config portal reachable
- **panel rendering is correct**: the boot version screen (host IP, `vdev`) is legible, and full-screen R/G/B fills are the right colours

> **Important:** because text and fills render correctly, the framebuffer→panel mapping, row addressing, pins, colour order and blanking are **all correct**. An entire earlier investigation into panel driver/line-decoder/phase/latch/speed settings was a misdirection — those knobs do not fix this and should not be revisited. (The tunables still exist; see §6.)

---

## 2. Environment

| Item | Value |
| --- | --- |
| Repo | `~/dev/huidu-hd-wf1-port-2026` (remote `github.com/delight-f/huidu-hd-wf1-port-2026`, branch `main`) |
| ESP-IDF | `~/esp/esp-idf` (v5.5) — `. ~/esp/esp-idf/export.sh` |
| Device port | `/dev/ttyACM0` in download mode |
| Device | HD-WF1, **ESP32-S2 rev v1.0, no PSRAM**, 4 MB flash, 64×32 HUB75E panel, **FM6124** LED driver ICs |

### Build / flash

```bash
. ~/esp/esp-idf/export.sh
cd ~/dev/huidu-hd-wf1-port-2026
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
idf.py -p /dev/ttyACM0 flash      # board must be in download mode
```

Download mode: bridge the two GPIO0 pads near the Micro-USB port, power-cycle, confirm `303a:0002` / `/dev/ttyACM0` appears. USB-A-to-USB-A cable into the **USB-A** port.

**There is no serial console.** `idf.py monitor` cannot work: no UART bridge, and `CONFIG_ESP_CONSOLE_USB_CDC` does not enumerate on ESP32-S2 in ESP-IDF without TinyUSB (known IDF defect). Do not spend time on it.

---

## 3. Read state off the panel (the only working console)

Two temporary diagnostics print to the panel, because **text renders correctly even when everything else fails**:

**Boot screen** (in `display_initialize()`, right after the R/G/B fills):
```
heap NNNk   free heap
int  NNNk   free internal RAM
dma  NNNk   free DMA-capable RAM
cfg  D L B  driver / line decoder / latch blanking in use
```

**Decode screen** (`diag_panel()` in `gfx.c`, drawn per frame):
```
stk  NNNN    gfx task stack high-water mark (bytes free)
webp NNNN    length of the WebP handed to the decoder
dec  X       stage and result:  OK | info ERR | buf ERR | dmux ERR | dec1 ERR | cfg ERR
heap NNNk
```

The decode screen **clears the screen** each frame, so it masks the picture. If you get a `dec OK`, remove the `diag_panel("dec", len, true)` call in `draw_webp()` so the image is visible — it was left in deliberately while the decode was unproven.

`/diag` (HTTP) additionally serves the captured log, reset reason, heap/DMA figures, and the two button-pin levels.

### Measured readings (latest build)

| Where | Reading |
| --- | --- |
| Boot | `heap 29k`, `int 29k`, `dma 21k`, `cfg 1 0 1` |
| At decode (parrot, 4608 B) | `stk 1652`, `webp 4608`, `dec1 ERR`, `heap 24k` |

`stk 1652` **rules out stack exhaustion** — there is 1.6 KB still free of the 4092-byte gfx task stack.

---

## 4. Root-cause analysis: libwebp vs 172 KB of DIRAM

`idf.py size` on this build:

```
DIRAM   134,872 used (78.4%)   remain  37,160   total 172,032
```

**The ESP32-S2 exposes only ~172 KB of data-capable RAM in total** (~90 KB of which is IRAM-resident code), leaving ~26–29 KB of heap after WiFi. Every other Tronbyt target has PSRAM or far more DRAM, and the MrCodeTastic reference never decodes a WebP at all (it draws its own graphics) — which is why that firmware is happy on this board while ours is not.

`WebPAnimDecoder` was the first blocker. `managed_components/libwebp/src/demux/anim_decode.c:140-145`:

```c
dec->curr_frame          = WebPSafeCalloc(canvas_width * NUM_CHANNELS, canvas_height);
dec->prev_frame_disposed = WebPSafeCalloc(canvas_width * NUM_CHANNELS, canvas_height);
```

Two full canvas buffers (12 KB at RGB, 16 KB at RGBA for 64×32) **plus** VP8 decoder state, all from ~24 KB of fragmented heap → `WebPAnimDecoderNew()` returns NULL. That was the panel-reported **`new ERR`**.

### The workaround now in place (and where it fails)

`draw_webp()` in `main/gfx.c` no longer uses `WebPAnimDecoder`. It:

1. `WebPGetInfo()` for canvas size,
2. allocates **one** buffer we own (`decode_buffer()`, ~6 KB for 64×32 RGB, allocated once, so it lands before the heap fragments),
3. `WebPDemux()` + `WebPDemuxGetFrame()` to iterate frames,
4. `WebPDecode()` per frame with `output.is_external_memory = 1`.

That gets us past allocation (`info`/`buf`/`dmux` all succeed) but `WebPDecode()` fails → **`dec1 ERR`**.

---

## 5. The open problem and exactly where to look next

**Symptom:** `dec1 ERR` — `WebPDecode(iter.fragment.bytes, iter.fragment.size, &cfg)` does not return `VP8_STATUS_OK`.

The implementation diverged from libwebp's own frame decoder. `anim_decode.c:367-385` should be mirrored exactly:

```c
const uint8_t* in = iter.fragment.bytes;
const size_t in_size = iter.fragment.size;
const uint32_t stride = width * NUM_CHANNELS;              // width = CANVAS width
const uint64_t out_offset = (uint64_t)iter.y_offset * stride +
                            (uint64_t)iter.x_offset * NUM_CHANNELS;
const uint64_t size = (uint64_t)iter.height * stride;      // FRAME height, not canvas
WebPDecoderConfig* const config = &dec->config;
WebPRGBABuffer* const buf = &config->output.u.RGBA;
buf->stride = (int)stride;
buf->size   = (size_t)size;
buf->rgba   = dec->curr_frame + out_offset;                // offset per frame!
if (WebPDecode(in, in_size, config) != VP8_STATUS_OK) goto Error;
```

Concrete suspects, in order:

1. **`NUM_CHANNELS` / colour mode.** libwebp's animation path decodes to **RGBA (4 channels)**; this port switched to `MODE_RGB` to save memory. Try `MODE_RGBA` with 4-channel stride — it may simply be that the animation canvas model requires alpha.
2. **Per-frame offset/stride/size.** Decoding every frame at the canvas origin with canvas size (as now) is wrong for any frame smaller than the canvas, and `size` should be `iter.height * stride`. Fix this to match the snippet above.
3. **Persistent canvas.** libwebp keeps a canvas across frames and applies `blend_method` / `dispose_method` (`anim_decode.c:387-410`). Partial frames and blends will not composite correctly without it — implement at least the `IsFullFrame` fast path and, ideally, `CopyCanvas`/`ZeroFillCanvas` semantics.
4. **If `WebPDecode` still fails on a fragment**, verify what `iter.fragment` actually contains on this libwebp build (it may or may not include a RIFF header) — dump `iter.fragment.size` and the first bytes via the panel diagnostic.

**Memory budget to keep in mind when fixing:** we have ~24 KB at decode time. A persistent RGBA canvas (64×32×4 = 8 KB) plus a per-frame decode target is affordable *only* if we stay away from `WebPAnimDecoder`'s second canvas. Consider RGBA canvas (8 KB) + decoding directly into it at the frame offset (as libwebp does) — that is one buffer, not two.

**Do not** go back to `WebPAnimDecoder` unless you first free the RAM for its two canvases.

### Ranked fallbacks if the fragment approach cannot be made to work

- Decode only the **first frame** with the one-shot `WebPDecodeRGB()` (still images are the common Tronbyt case; animation is lost).
- Reduce `CONFIG_HTTP_BUFFER_SIZE_DEFAULT` further and/or disable lwIP IPv6 (`CONFIG_LWIP_IPV6=n`) to buy a few KB so `WebPAnimDecoder`'s two canvases fit — but this is unproven and fragmentation-dependent.

---

## 6. Code map (what changed and where)

| File | Why it matters |
| --- | --- |
| `main/gfx.c` | `draw_webp()` — **the file to work in**: low-memory decode path (`decode_buffer`, `WebPDemux`, `WebPDecode`), `diag_panel()` on-panel diagnostic; S2 single-core guard `GFX_TASK_CORE 0`; `GFX_TASK_STACK_SIZE 4092`; boot-debug text clamped to 10 chars |
| `main/display.cpp` | WF1 pin map (~135-151); `PANEL_*_DEF` defaults + `panel_cfg_get()` NVS overrides; `display_fill_screen()`, `display_diag_show()`; NULL-`_matrix` guards; **temporary R/G/B bench fills + boot diagnostic** in `display_initialize()` |
| `main/main.c` | `diag_init()` at top of `app_main`; display-init failure non-fatal; AP auto-shutdown disabled |
| `main/mem_compat.h` | `IMAGE_BUF_CAPS` — SPIRAM when available, internal RAM otherwise |
| `main/diag.c` / `.h` | in-RAM log ring (1 KB) served by `/diag` |
| `main/ap.c` | `/diag` and `/panel` endpoints |
| `main/panel_sweep.c` / `.h` | GPIO11/GPIO0 button → panel-config cycler + boot indicator colour |
| `sdkconfig.defaults.huidu-wf1` | S2, 4 MB, no SPIRAM, small boot asset, `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, HTTP buffers 60000/4000 |
| `boards/max_app_4mb.csv` | single `factory` slot — **OTA impossible** without a new table |

### Panel-config tunables (exist, but are NOT the current problem)

`/panel?drv=&line=&spd=&lat=&ph=&dbfr=` (and `?clear=1`) override the panel config in NVS and reboot; the GPIO11/GPIO0 button cycles presets. **These live in NVS and survive reflashing** — use `/panel?clear=1` or `idf.py erase-flash` before comparing against compiled defaults. Current WF1 defaults: `FM6124`, `TYPE138`, 20 MHz, latch blanking 1, double buffering off.

---

## 7. What was already fixed (do not re-introduce)

1. **PSRAM allocations on a no-PSRAM chip** — `main/mem_compat.h` (`IMAGE_BUF_CAPS`), used in `remote.c` (×2), `main.c`, `ap.c` (×2).
2. **174 KB boot asset** (`CONFIG_BOOT_WEBP_TRONBYT`) could not be `calloc`'d → `gfx_initialize()` failed → `app_main` returned before `ap_start()`, silently killing the portal while the AP SSID kept beaconing from hardware. Now `CONFIG_BOOT_WEBP_PARROT` (~4.5 KB), and display-init failure is non-fatal.
3. **Double buffering** — the second BCM framebuffer + DMA descriptors did not fit in DMA-capable RAM, so `setupDMA()` failed and `begin()` returned false (panel never initialised). `PANEL_DBUFF_DEF 0` for the WF1.
4. **NULL `_matrix` guards** in `display_clear()`/`display_draw()`.
5. **`FM6124`** is the correct driver for this panel (the library only runs `fm6124init()` for FM6124/FM6126A/ICN2038S).

---

## 8. Gotchas

- **NVS persists across reflashes** — panel overrides, WiFi creds and `image_url` all survive. `idf.py erase-flash` for a clean slate.
- **No serial console.** Use the panel diagnostics (§3) or `/diag`.
- The device may be on an **isolated IoT network** and therefore absent from your LAN — expected, not a fault.
- AP auto-shutdown is currently **disabled** so the portal stays reachable.
- `secrets.json`/`secrets.cmake` hold real credentials and are gitignored.
- Untracked by choice: the two spec PDFs and `.commandcode/`.

---

## 9. Cleanup before merging

1. Remove the temporary R/G/B fills and boot diagnostic from `display_initialize()` (`display.cpp`).
2. Remove `diag_panel()` calls from `gfx.c` (keep `display_diag_show()` + `/diag` — on a board with no console they earn their keep).
3. Decide the fate of the panel-config tunables and the button cycler (probably keep; they cost little).
4. Add `huidu-wf1` to the GitHub Actions matrix **and** an `esp32s2` case to its chip dispatch (it currently maps everything non-S3 to `esp32`).
5. Add `huidu-wf1` to `esp_idf_project_configuration.json`.
6. Update the README status once images render.

---

## 10. Commits so far (branch `main`)

- `6a38acf` feat(wf1): button-driven panel-config cycler for bench bring-up
- `2fb0644` docs: add HANDOFF.md for the next agent
- `1c64019` docs: record HD-WF1 bring-up status, findings and remaining work
- `6774141` fix(wf1): survive the no-PSRAM S2 and add on-device diagnostics
- `611a34f` fix(wf1): run on the no-PSRAM S2 and align HUB75 panel timing
