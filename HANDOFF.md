# Handoff — HD-WF1 Tronbyt port bring-up

**Audience:** the next coding agent picking this up.
**Goal:** make the Huidu HD-WF1 render Tronbyt WebP images correctly on its 64×32 HUB75E panel.

**Current state in one line:** everything works *except* the panel image — the firmware builds, flashes, boots, joins WiFi as a station, talks to the Tronbyt server, and its web portal is reachable, but the panel renders WebP frames as random red/green/blue noise.

Read `README.md` first — it has the public-facing narrative. This document is the engineering continuation: exact commands, evidence, hypotheses, and gotchas.

---

## 1. Objective and acceptance criteria

Done when: pointing `REMOTE_URL` at a Tronbyt server produces a **recognisable image** on the 64×32 panel (not noise), stable across reboots, with the boot animation and config screens also correct.

Already accepted as working (do not regress):
- builds clean for `esp32s2` with native ESP-IDF v5.5
- boots; single-core task pinning correct
- WiFi station join + DHCP
- reaches the Tronbyt server (device appears in tronbyt-manager)
- config portal reachable; `/diag` and `/panel` reachable
- **solid full-screen R/G/B fills render with the correct colours** (this is the baseline that proves pins, OE/LAT/CLK and colour order are right)

---

## 2. Environment on the machine where this was developed

| Item | Value |
| --- | --- |
| Repo | `~/dev/huidu-hd-wf1-port-2026` (remote: `github.com/delight-f/huidu-hd-wf1-port-2026`, branch `main`) |
| ESP-IDF | `~/esp/esp-idf` (v5.5) — `. ~/esp/esp-idf/export.sh` |
| Toolchain | `~/.espressif` (idf5.5_py3.12_env) |
| Device port | `/dev/ttyACM0` when in download mode (user must be in `dialout`) |
| Device | Huidu HD-WF1, ESP32-S2 rev v1.0, **no embedded PSRAM**, 4 MB flash, 64×32 HUB75E panel with **FM6124** LED driver ICs |

### Build

```bash
. ~/esp/esp-idf/export.sh
cd ~/dev/huidu-hd-wf1-port-2026
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
```

(`make huidu-wf1` does the same thing.)

### Flash

The board only enters download mode with **GPIO0 pulled low at power-up**: bridge the two small pads near the Micro-USB port, power-cycle, then `ls /dev/ttyACM*` should show `303a:0002`. Only then:

```bash
idf.py -p /dev/ttyACM0 flash
```

It is a **USB-A-to-USB-A** cable into the WF1's USB-A port (not Micro-USB). `esptool` can also be used directly. If the device wedges during flashing, unplug/replug.

### Monitoring — there is none over serial

`idf.py monitor` **will not work**. The board has no UART bridge, and `CONFIG_ESP_CONSOLE_USB_CDC` does not enumerate on ESP32-S2 in ESP-IDF without TinyUSB (known IDF defect: `device not accepting address`, `error -71`, descriptor read timeouts — reproducible). Do not spend time on it; use `/diag` instead.

---

## 3. On-device diagnostics (already built in — use these)

The web server listens on **both** the AP and station interfaces, so these work over the normal LAN at the device's own IP, or over `TRON-CONFIG` at `10.10.0.1`. Log level is INFO.

| Endpoint | Purpose |
| --- | --- |
| `GET /diag` | reset reason, free heap / free internal / free DMA + largest blocks, stored brightness, and the **captured log ring** (this is the only console you have) |
| `GET /panel?drv=&spd=&lat=&ph=&dbfr=` | writes HUB75 overrides to NVS then reboots in ~1 s |
| `GET /panel?clear=1` | removes the overrides, back to board defaults |

`drv`: `0`=SHIFTREG, `1`=FM6124, `2`=FM6126A, `3`=ICN2038S, `4`=MBI5124, `5`=DP3246. `spd`: `0`=8 MHz, `1`=20 MHz.

`/diag` prints the active configuration as `Panel config: driver=N speed=N latch_blanking=N phase=N double_buff=N` — always check this before interpreting a panel result.

> **Critical gotcha:** the `/panel` overrides live in **NVS**, and `idf.py flash` does **not** erase NVS. A device that has been tuned will keep those overrides across reflashes and silently ignore the board defaults in `display.cpp`. Run `/panel?clear=1` (or `idf.py erase-flash`) before comparing builds. The same applies to WiFi credentials and `image_url` saved via the portal.

---

## 4. What was already fixed (do not re-introduce these)

Full detail is in `README.md`; the short version:

1. **PSRAM allocations on a no-PSRAM chip.** `main/mem_compat.h` defines `IMAGE_BUF_CAPS` = SPIRAM when `CONFIG_SPIRAM`, else internal RAM. Used by `main/remote.c` (×2), `main/main.c` (×1), `main/ap.c` (×2).
2. **174 KB boot asset.** `CONFIG_BOOT_WEBP_TRONBYT` (174,330 bytes) cannot be `calloc`'d in internal RAM → `gfx_initialize()` failed → `app_main` returned before `ap_start()`. Now uses `CONFIG_BOOT_WEBP_PARROT` (~4.5 KB), and a display-init failure is non-fatal (`main/main.c`).
3. **Double buffering.** `setupDMA()` allocates the BCM framebuffer (~1 KB/row × 16 rows) plus DMA descriptors in **DMA-capable internal RAM**, doubled when `double_buff` is on. The port hard-coded `true`; the library default and the reference are `false`. `PANEL_DBUFF_DEF 0` for the WF1. **This is what stopped `begin()` failing** — before it, `_matrix` was NULL and nothing was ever drawn.
4. **NULL `_matrix` guards** in `display_clear()` / `display_draw()` (`main/display.cpp`) — without them the device panicked ~60 s after boot once (2) made init failure non-fatal.
5. **FM6124 driver.** The library's `shiftDriver()` only runs `fm6124init()` for FM6124/FM6126A/ICN2038S; `SHIFTREG` runs no init. The panel's ICs are **FM6124**. Now the WF1 default (`PANEL_DRIVER_DEF 1`).

---

## 5. The open problem — corrupted rendering

### Evidence gathered so far

| Config | Result |
| --- | --- |
| `SHIFTREG`, latch 4, phase inv, 20 MHz, dbuff off | random coloured dots |
| `SHIFTREG`, latch 1, phase inv, 20 MHz, dbuff off | random coloured dots |
| `FM6124`, latch 1, phase inv, 20 MHz, dbuff off | **still corrupted** (no better, no worse) |
| `FM6124`, latch 4, phase inv, 20 MHz, dbuff off | **still corrupted** |
| any of the above | ✅ solid R/G/B full-screen fills are correct; ❌ images are noise |

Key deduction: a full-screen solid fill is **invariant under row-addressing and timing errors**, so it can look perfect while real content is scrambled. Because the fills *are* correct, the data pins, OE/LAT/CLK and colour order are right — the fault is in the **scan/timing path or the DMA data stream**, not wiring or channel mapping.

The reference config for this exact board (`mrcodetastic/HD-WF1-WF2-LED-MatrixPanel-DMA`: SHIFTREG default, `HZ_20M`, `latch_blanking = 4`, dbuff off, 64×32 chain 1, `TYPE138`) was tried and did **not** work here.

### Ranked hypotheses

**H0 — Establish a hardware baseline first (do this before anything else).**
Flash mrcodetastic's own firmware for this board (`HD-WF1-WF2-LED-MatrixPanel-DMA`, PlatformIO env `huidu_hd_wf1`) and see whether the panel renders correctly with *known-good* code. If it does, the board + panel + ribbon are fine and the problem is entirely in this port's configuration/library build. If it also fails, suspect the panel, ribbon seating, or panel power. This one step removes a whole class of uncertainty and is the cheapest high-value experiment — do not skip it.

**H1 — HUB75 library version (highest suspicion).**
This port pins `esp32-hub75-matrixpanel-dma` **v3.0.14** (git `f17fb7f`) in `main/idf_component.yml` / `dependencies.lock`. The reference project depends on `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA` **unpinned master**, built under Arduino / IDF 5.3, while this is native IDF 5.5. The ESP32-S2 I²S-parallel backend (`managed_components/.../platforms/esp32/esp32_i2s_parallel_dma.cpp`) differs across versions. Test: bump the component to a newer tag/commit and rebuild.
Also note the S2 branch hard-codes `_div_num = (freq > 8000000) ? 2 : 4` and forces `clk_sel = 2` (160 MHz PLL) — check this against the version the reference resolves.

**H2 — Clock phase.** `ph=0` (stop inverting). The enum is forced to `true` for MBI5124 in `ESP32-HUB75-MatrixPanel-leddrivers.cpp`; for FM6124 it uses the configured value.

**H3 — Pixel clock.** `spd=0` (8 MHz). A too-fast DMA stream on the S2 would corrupt data rather than shift it.

**H4 — Latch blanking.** `lat=4` vs `lat=1` (note the library header warns that values > 1 cause artefacts on ICS-driven panels).

**H5 — Scan rate / row addressing.** Confirm the panel really is **1/16 scan** (normal for 64×32; `ROWS_PER_FRAME = mx_height/2 = 16`, address lines A–D). If it is a different scan, the geometry handling is wrong. Related: `CH_E = 12` is wired but unused at 1/16.

**H6 — I²S divider patch.** Upstream has a `-patched` variant for the ESP32/S2 I²S backend (`CONFIG_PATCH_I2S_DIVIDER`, applied by `tools/patch_i2s_divider.py`). It is off for the WF1. Upstream's README says the library otherwise snaps the clock rather than honouring the requested frequency. Worth enabling to see if it changes anything.

### Useful experiment protocol

Change one variable at a time via `/panel?...`, reboot, and check `/diag` to confirm which config is live before judging the panel. Remember the NVS-persistence gotcha (§3). To get a stable comparison, hold the server frame constant (the test server returns a fixed 226-byte 64×32 WebP).

**No network? Use the button cycler.** The board's single push button is **GPIO 11** (`CONFIG_BUTTON_PIN`) — the same one read at boot for config mode. `main/panel_sweep.c` starts a polling task that, on a short press (after a 5 s grace period), advances to the next candidate config, writes it to NVS and reboots. The candidate list and its boot indicator colours are in `s_cfgs[]` in that file; the active one is logged as `panel config N/7 '<name>' drv=… spd=… lat=… ph=… dbfr=…` and shown on `/diag`. Because a solid fill renders correctly even when the panel is mis-configured, the boot colour is the "which config is live" indicator.

This makes the whole sweep possible with nothing but the board and a power switch — no portal, no IP hunting. Add or reorder candidates directly in `s_cfgs[]`.

---

## 6. Code map (what changed and where)

| File | Why it matters |
| --- | --- |
| `main/display.cpp` | WF1 pin map (lines ~135–151); per-board **and** NVS-overridable panel driver/timing (`PANEL_*_DEF`, `panel_cfg_get()`, constructor call ~line 282); NULL guards; text width clamp; **temporary R/G/B bench test just after `begin()` — remove before merging** |
| `main/main.c` | `diag_init()` at the top of `app_main`; display-init failure is non-fatal; AP auto-shutdown disabled |
| `main/gfx.c` | S2 single-core guard (`GFX_TASK_CORE 0`); boot-debug text clamped to 10 chars (`display_text_fitted`) |
| `main/mem_compat.h` | `IMAGE_BUF_CAPS` |
| `main/diag.c` / `main/diag.h` | log ring buffer + `diag_init()` / `diag_log_copy()` |
| `main/ap.c` | `/diag` and `/panel` handlers; `max_uri_handlers = 12` |
| `main/panel_sweep.c` / `.h` | GPIO11 button → candidate panel-config cycler; boot indicator colour; NVS read/write of the overrides |
| `main/CMakeLists.txt` | `diag.c` and `panel_sweep.c` added to `SRCS` |
| `sdkconfig.defaults.huidu-wf1` | S2 target, 4 MB, no SPIRAM, WF1 board, **small boot asset**, `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` |
| `boards/max_app_4mb.csv` | single `factory` app slot — **OTA is impossible** without a new table |

Core dumps are enabled; if the board panics, the dump is in the coredump partition at `0x3F0000` (`esptool read_flash` it and decode with `esp-coredump`).

---

## 7. Gotchas checklist

- **NVS persists across reflashes.** `/panel` overrides, WiFi creds and `image_url` survive. `idf.py erase-flash` or `/panel?clear=1` to reset.
- **Download mode requires the GPIO0 pad bridge** at power-up. Without it the board runs the app and no `ttyACM` appears.
- **No serial console.** Use `/diag`.
- **The device may be on an isolated IoT network.** It will not appear on the LAN you are sitting on; check the router's DHCP list or use the Tronbyt manager to find it. That is expected, not a fault.
- **AP auto-shutdown is currently disabled** (it is what kept `/diag` reachable). Upstream shuts the AP down ~2 min after STA connects and switches to STA-only — decide whether to restore that. Relevant code: `main/main.c` around the old `ap_start_shutdown_timer()` call, and `main/ap.c` `ap_shutdown_timer_callback()`.
- **`secrets.json` / `secrets.cmake` hold real credentials** and are gitignored. Never commit them.
- Untracked in the working tree: two spec PDFs and `.commandcode/` — deliberately not committed.
- The upstream `main/CMakeLists.txt` compiles `touch_control.c` only for `CONFIG_BOARD_TIDBYT_GEN2`; on the S2 `TOUCH_PAD_NUM8` would clash with the WF1's `G2 = GPIO8`, so do not enable it.

---

## 8. Cleanup before merging

1. Remove the temporary R/G/B fill from `display_init()` in `main/display.cpp` (or gate it behind a Kconfig option).
2. Decide what to keep from the diagnostics: **keep `/diag`** (it is the only console this board has), keep or drop `/panel` once the default config is settled.
3. Add a `huidu-wf1` entry to the GitHub Actions matrix in `.github/workflows/main.yml` — and note its chip dispatch currently maps everything non-S3 to `esp32`; an `esp32s2` case is required or it will build an S2 config for an ESP32 target.
4. Add `huidu-wf1` to `esp_idf_project_configuration.json`.
5. Update the README status callout once the panel renders correctly.
6. Consider `BRIGHTNESS_8BIT_MAX` for third-party panels (WF1 currently inherits the legacy `230`).

---

## 9. Commits so far (branch `main`)

- `1c64019` docs: record HD-WF1 bring-up status, findings and remaining work
- `6774141` fix(wf1): survive the no-PSRAM S2 and add on-device diagnostics
- `611a34f` fix(wf1): run on the no-PSRAM S2 and align HUB75 panel timing
