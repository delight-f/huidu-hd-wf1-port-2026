# Handoff — HD-WF1 Tronbyt port bring-up

**Audience:** the next coding agent. Read this whole file before touching anything.
**Goal:** make the Huidu HD-WF1 (ESP32-S2, 4 MB flash, **no PSRAM**) render Tronbyt WebP images on its 64×32 HUB75E panel.

**Written after a session that did not finish the job.** It made real, measured progress on the memory problem the previous handoff was stuck on, and it also introduced regressions and left the board in a worse state than it found it. Both are documented below with the evidence, because the negative results are the most valuable thing here.

The previous version of this file is in git: `git show HEAD:HANDOFF.md` (or `git checkout HEAD -- HANDOFF.md`). This rewrite replaces it rather than appending, because the old framing ("everything works except one allocation") is no longer true.

---

## 0. State right now — read this first

| | |
| --- | --- |
| **Board is crashing** | It runs for roughly a minute, then resets. **Cause not established.** See §2. |
| Fragmentation blocker | **Solved and verified.** Largest free block **7,936 → 36,864 bytes**; `free_internal` ~20 KB → ~52 KB. |
| Images rendering | Not reliably. Small/mid apps (≤ ~11 KB) have decoded and displayed; large apps (20–33 KB) cannot, for a reason that looks fundamental (§3). |
| Receive path | **Improved but not fixed.** Large bodies used to die after 1–4 KB; they now reach ~14 KB, then stall. |
| Flashed firmware | Build with in-place asset decoding (§5.4). Untested for stability because of the crashes. |

**Do not trust any build in this tree until you have watched it run for >10 minutes without a reset.** Several changes here look correct in isolation and were never validated end-to-end.

---

## 1. Working with the hardware — copy these commands

Build and flash (this shell is `dash`; `IDF_PATH_FORCE=1` is required):

```sh
export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1
. $IDF_PATH/export.sh
cd ~/dev/huidu-hd-wf1-port-2026
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
idf.py -p /dev/ttyACM0 flash
```

`sdkconfig` is gitignored and **is not re-applied from `sdkconfig.defaults.huidu-wf1` unless you delete it**. After editing the defaults you must `rm -f sdkconfig` and re-run `set-target`. This has caused at least two wrong conclusions, here and in the previous session.

**Download mode:** the board must be put there by hand — bridge the two GPIO0 pads near the Micro-USB port, then power-cycle. Confirm `303a:0002` (esptool) and `/dev/ttyACM0` before flashing. While the app is running it does not enumerate USB at all, so absence of the device is normal.

**POWER-CYCLE before judging the panel.** Every flash ends in a reset, and see §7.1 — a warm reset leaves the panel showing collapsed rows. This is not a firmware fault and will waste your time exactly as it wasted mine.

**The device is observable over the network.** This is the single most useful thing about this port:

```sh
curl -s http://192.168.10.138/diag          # reset_reason, heap, then the in-RAM log ring
curl -s "http://192.168.10.138/diag?heap=1" # + free-block histogram
```

`reset_reason` is the fastest crash/stability signal: `1` = power-on, `3` = software reset, `4` = **panic**. If it reads `4`, the board panicked — see §2. The IP is DHCP; scan the /24 if it moves.

**The panel is also a console.** `diag_panel()` in `gfx.c` draws four lines, each truncated to 10 chars:

```
stk  NNNN    gfx task stack high-water mark
webp NNNN    number of bytes handed to the decoder
dec  X       stage and result: OK | info ERR | buf ERR | dmux ERR | dec ERR
hNNk bNNk    free heap and LARGEST FREE BLOCK, in KiB
```

`dec ERR` + a red dot in the top-left is `draw_error_indicator_pixel()` plus a stale `diag_panel` from an earlier failure. **It is not necessarily the current state** — nothing redraws the panel when an image fails, so a single early failure can persist on screen indefinitely. Read `/diag` before believing the panel.

---

## 2. The crash — first thing to fix

**Symptom:** the board runs for roughly a minute, then resets. It was observed on several builds.

**What is known:**

- One panic was captured and decoded. It was **a bug introduced during this session**: `payload_declared_size(buf=0x0, len=6953)` in `main/remote.c` dereferenced a NULL pointer (`LoadProhibited`). The receive buffer is freed when a resize fails while `state.len` still holds the partial byte count, so `buf == NULL` with `len > 0` is reachable. Fixed with a NULL guard, plus an explicit `buf_lost` flag so a mid-transfer buffer loss fails cleanly instead of being inferred from a NULL pointer.
- **A second panic was captured but could not be decoded**, because the app SHA embedded in the dump (`3a61d3f3…`) matched none of the saved ELFs. That build's ELF is gone. **Whether a second cause remains is unknown.**

**How to find it — and the two traps that cost a cycle each:**

```sh
# Copy the ELF FIRST. info_corefile refuses a dump whose app SHA differs from the ELF's,
# so a rebuild destroys your ability to decode an existing dump.
cp build/firmware.elf /somewhere/keep-this.elf

esptool.py --chip esp32s2 -p /dev/ttyACM0 --after no_reset read_flash 0x3F0000 0x10000 cd.bin
espcoredump.py info_corefile -c cd.bin /somewhere/keep-this.elf
```

- `--after no_reset` is essential: without it the read hard-resets the board and you lose the download-mode session you needed for the flash.
- The coredump partition is at `0x3F0000`, 64 KB (`boards/max_app_4mb.csv`), and `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` is already set.

**Do this before changing anything else.** A panic backtrace names the exact frame, which is worth more than any amount of reasoning from symptoms — that is how the NULL dereference was found after several rounds of wrong guesses.

**If the dump will not decode,** bisect instead: `git stash` the whole change set, build stock, and confirm stock survives >10 minutes. Then re-apply changes in groups — the two memory wins plus the dwell fix first, one at a time, watching `reset_reason` after each. Most of the current risk lives in `main/remote.c`, which changed by ~190 lines.

---

## 3. The image-size ceiling — likely the fundamental limit

The payload must stay **resident while libwebp decodes it**. So the requirement is `payload + decoder working set`, not the larger of the two. Measured with `tools/webp-host-harness` against assets the server actually serves:

| payload | decoder transient | required live | board free | outcome |
| --- | --- | --- | --- | --- |
| 360 B | ~11.9 KB | ~12 KB | ~41 KB | decodes and displays |
| 11,305 B | ~18 KB | ~30 KB | ~41 KB | decodes and displays |
| 22,000 B | ~20–26 KB | ~42–48 KB | ~41 KB | **fails** |
| 33,084 B | 25,864 B | ~59 KB | ~41 KB | **fails** |

(The 33 KB figure is the server's own asset, measured directly: `peak 25864 B, largest single 12552 B`.) The server serves 64×32 displays and emits 20–33 KB animated WebPs; the small stills are 226–360 bytes and mid apps ~11 KB.

The sharpness of that table is the point: 11 KB fits with room, 22 KB does not, and **no buffer tuning moves a 59 KB requirement into a 41 KB budget.** A no-PSRAM S2 has ~320 KB of DRAM total, ~82 KB of which goes to WiFi bring-up.

Consequences:

- **Server-side is the real fix.** Assets for a 64×32 panel should be a few KB. If the Tronbyt server has a quality/size setting, or the apps are authored oversized, that solves this properly. Worth raising with the owner before more firmware work.
- **Streaming decode is the one structural improvement** for *still* images (feed the bitstream to libwebp as it arrives so the whole file is never resident). It does **not** help animations, which need the complete file for frame iteration — and the oversized apps here are animations.
- Enforce a ceiling so oversized apps are refused early and show a screen; see §5.4.

---

## 4. What is verified working (measured, not assumed)

**The fragmentation blocker from the previous handoff is solved.** Largest free block **7,936 → 36,864 bytes**, `free_internal` ~20 KB → ~52 KB. Boot heap trace, before vs after, at the same marks:

| stage | before | after |
| --- | --- | --- |
| after WiFi init | 68,440 / 57,344 | **89,040 / 77,824** |
| after display init | 19,284 / 11,264 | **31,696 / 23,552** |
| after ap_start | 6,148 / 3,584 | **41,084 / 32,768** |

The changes that did the real work:

1. **WiFi driver buffers — 20.6 KB, the biggest single win.** The old handoff records these as "no effect"; that is wrong, and the reason is instructive: **none of the settings were in `sdkconfig.defaults.huidu-wf1`** — every knob was at the IDF default, so the original measurement came from a build that never had the change. Verify a config change took effect before concluding it does not work.
2. **The composite scratch was cached for the life of the program — 8 KB.** `grow_buffer(&s_frame, …)` in `gfx.c` allocated the RGBA scratch for multi-frame images and never released it, so one animation early in a session cost 8 KB permanently, paid by every still frame afterwards.
3. **Canvas reserved before the network stack** (instead of inside the first `draw_webp()`), which costs nothing and keeps the biggest long-lived buffer out of the middle of the heap.
4. **SoftAP retired once the station link is up** — worth 8 KB. The web server is deliberately left running so `/diag` survives on the station address.

**A real firmware bug, unrelated to memory:** the main loop in `main.c` never honoured the server's dwell. `gfx` consumes the dwell itself for animations, but a still image is one frame, so that loop returns immediately and the main loop fetched again at once — **~1.2 images/second against a `Tronbyt-Dwell-Secs: 10`.** The board was polling ~70×/minute indefinitely. It now sleeps only the *remainder* of the dwell, so animations are not held for twice the requested time.

**Fetches from the Tronbyt server work** and are fast when the board is healthy: `HTTP fetch returned in 43–326 ms`, `Content-Length Header : 360`, repeated.

**The receive path was substantially improved** by paying for RX buffers early rather than on demand — see §5.3. Bodies now reach ~14 KB where they previously died at 1–4 KB.

---

## 5. Regressions and mis-steps from this session — review these before trusting them

Every item here is a change that is in the tree. Some are genuine fixes; some made things worse. **I changed configuration before measuring, twice, and both times that is what broke the board.** Do not repeat it: change one thing, measure fetch latency and `reset_reason`, then change the next.

### 5.1 WiFi buffer *over*-trimming — broke the transport
Trimming to `STATIC_RX=6` / `DYNAMIC_RX=8` frees the most memory and **cannot receive a 20 KB body at all**: every attempt died after 1–4 KB, taking 30–50 s, while the same server handed a wired host 22 KB in 0.6 s. Restoring stock counts (static 10 / dynamic 32 / static TX 16) brought fetches back to 141–326 ms including 11 KB bodies.

Mechanism that fits: `DYNAMIC_RX_BUFFER_NUM` is a pool the driver draws from **on demand**, and a large image response makes us allocate a large receive buffer first, fragmenting exactly that heap — so the receive path loses buffers mid-transfer. Hence §5.3.

### 5.2 Raising `TCP_WND` to 11,680 — made it worse
1,226-byte payloads went from 36 ms to **39–42 s**. Reverted to the 5,744 default. A window the receive path cannot back with buffers is a way to make things worse, not better.

### 5.3 Pre-allocated RX buffers — helped, did not finish
`STATIC_RX=12` / `DYNAMIC_RX=4`, i.e. same memory budget as the over-trimmed build but paid at WiFi init while the heap is still one piece. **Verified improvement:** transfer progress now reaches `body 12729 bytes at +4000 ms` where previously nothing passed 4 KB. **Still not enough for a 33 KB body** — it stalls around 14 KB, logged as `Short payload: holding 14169 of 33302`. Why it stalls there is not established.

### 5.4 Oversized images — the fallback was itself oversized
`ASSET_OVERSIZE_WEBP_LEN` is **36,724** and `ASSET_404_WEBP_LEN` is **27,180**. `gfx_display_asset()` malloc'd and memcpy'd the asset before decoding, so the screen whose entire job is to say "that image is too big for me" **could not allocate on the board it was meant to protect**. The result was `Failed to allocate memory for oversize asset copy`, no screen drawn, and a stale error left on the panel while the log correctly reported the ceiling had fired.

Fixed by decoding assets **in place from flash rodata** (the same arrangement the boot animation already uses; rodata is memory-mapped and `WebPDecode` only reads). `gfx_update()` became a wrapper over `gfx_queue(..., is_static)`. `CONFIG_HTTP_BUFFER_SIZE_MAX` also dropped 60,000 → **16,384**, so oversized apps are refused at the `Content-Length` check instead of being half-fetched. **Neither is verified on hardware — the board crashes before it can be judged.**

### 5.5 My own NULL dereference — panicked the board
See §2. This is why "stock buffers panic at boot" was briefly and wrongly concluded to be a hardware limit: the panic was mine, triggered by a failed resize that the tighter heap made more likely.

### 5.6 The frame loop ground through every frame of an undecodable animation
Observed at `frame 141 decode failed (out of memory)` and still counting, holding the 22 KB payload the whole time. That is what made the board *look* hung. Now breaks out when nothing has decoded.

### 5.7 Panel and colour depth
The colour depth was briefly lowered 8 → 6 in an attempt to free memory and then reverted when the panel showed collapsed rows — **which was the post-flash transient (§7.1), not the depth.** The revert was based on a wrong diagnosis. Note for whoever revisits it: the panel stage of boot dropped from 44,808 to 22,100 bytes at depth 6, so it *is* a large lever, but `nsPerRow` scales with depth, so it changes BCM bitplane timing and the mask offset — it is a panel-behaviour change, not just a size change. The tree is currently back at the 8-bit default.

---

## 6. Ruled out — do not re-try without new evidence

| Tried | Result |
| --- | --- |
| Trim WiFi buffers | **Works — 20.6 KB — but do not over-trim.** See §5.1. The old handoff's "no effect" was measured on a build without the change. |
| Move WiFi code out of IRAM (`IRAM_OPT=n`, `RX_IRAM_OPT=n`) | Frees 23 KB of DIRAM at link time but makes the runtime heap *worse* (24,948 → 20,368 free). The extra DIRAM does not reach the data heap. |
| Lower panel colour depth 8 → 6 | **Works, and is bigger than the 6 KB the old handoff predicted** (panel stage 44,808 → 22,100 bytes) — but it changes panel timing. See §5.7. |
| Raise `TCP_WND` | Made the transport much worse. See §5.2. |
| Trim `STATIC_RX` / `DYNAMIC_RX` hard | Broke the transport. See §5.1. |
| Decoder cropping / scaling options | Irreducible — cropping does not shrink the internal buffer, scaling increases peak. Measured. |
| `WebPAnimDecoder` | Needs two full canvases plus the same working set. Strictly worse. |
| libwebp's internal 12,544-byte buffer | Not shrinkable by any decoder option (`AllocateInternalBuffers32b` allocates it regardless of `is_external_memory`). Read the source; it is irreducible. |
| Disable lwIP IPv6 | Not pursued: `main/ota.c` uses IPv6 socket types unconditionally. |
| The link itself | **Not the problem.** At failure: `rssi -44 dBm`, and the device serves `/diag` to the host in 25–40 ms throughout. |
| The server | **Not the problem.** Same server answers a wired host in 4.9 ms / 21,988 bytes. |
| RSSI-modulated / power-save effects | `CONFIG_ENABLE_WIFI_POWER_SAVE` is not set; power save is `WIFI_PS_NONE`. |

---

## 7. Hardware traps that cost real time

1. **The panel shows collapsed rows after any warm reset** — every flash ends in one, and `/panel` calls `esp_restart()`. The FM6124 driver ICs keep state across an MCU reset, so the driver's own init lands differently than from cold. **Only a power-cycle clears it** (unplug, wait, restore — not a button reset). The old handoff documented this; I still lost a cycle to it and reverted a working change because of it.
2. **The panel timing config lives in NVS and survives reflashing.** `/panel?drv=&line=&spd=&lat=&ph=&dbfr=` overrides it, the GPIO0/GPIO11 button cycles presets, and **the `clear` handler ignores its value** — `?clear=0` clears just as `?clear=1` does. `/panel` with no query params still commits and **reboots**. Note that GPIO0 is also the pin you bridge for download mode. Panel overrides were cleared during this session, so compiled defaults are live: `FM6124`, `TYPE138`, 20 MHz, latch blanking 1, double buffering off, 8-bit.
3. **NVS generally persists** — WiFi credentials, `image_url`, brightness. `idf.py erase-flash` for a clean slate, but you will have to re-enter WiFi credentials through the portal.
4. **There is no serial console.** `/diag` and the panel are it. USB-CDC is unusable on this board.
5. **`managed_components/` is gitignored** and fetched by the component manager — do not edit it; changes will not survive.
6. **`CONFIG_BOOT_WEBP_PARROT` must stay a small asset.** `gfx_initialize()` needs the boot animation decodable; `BOOT_WEBP_TRONBYT` is 174 KB and fails, which makes `gfx_initialize()` return non-zero and `app_main` exit before the display or config portal start.

---

## 8. Code map

| File | Why it matters |
| --- | --- |
| `main/gfx.c` | **the file to work in.** `draw_webp()` (per-frame decode at each frame's own offset, optional RGBA scratch + key-frame/blend/dispose compositing, `GFX_DECODE_HEADROOM` gate); `gfx_queue()`/`gfx_update()`; `gfx_display_asset()` now decodes flash rodata in place; `release_frame_scratch()`; `vp8_status_name()`; the boot heap trace. `GFX_TASK_CORE 0` (single core), `GFX_TASK_STACK_SIZE 6144`. |
| `main/remote.c` | HTTP fetch. ~190 lines changed this session: buffer right-sizing, `payload_declared_size()` completeness check, `buf_lost`, RSSI/connect logging, need-based growth instead of doubling. **Most of the crash risk lives here.** |
| `main/main.c` | `app_main`, the fetch loop, the dwell sleep, fetch-latency + main-task stack watermark logging. |
| `main/ap.c` / `.h` | `/diag`, `/panel`, `/save`, `/update`; `ap_start()`, `ap_retire_softap()`. |
| `main/diag.c` / `.h` | In-RAM log ring (1 KB) served by `/diag`; the boot heap trace; the `?heap=1` free-block histogram. |
| `main/display.cpp` | WF1 pin map; `PANEL_*_DEF` defaults + `panel_cfg_get()` NVS overrides; `display_panel_width()/height()`. |
| `sdkconfig.defaults.huidu-wf1` | S2, 4 MB, no PSRAM, small boot asset, coredumps to flash. **Every tuned value here carries its measurement and its rationale — read them before changing.** |
| `boards/max_app_4mb.csv` | Single `factory` slot — **OTA impossible** without a new table. Coredump at `0x3F0000`. |
| `tools/webp-host-harness/` | **Use this first.** Measures the decoder's transient heap and renders comparisons against libwebp's own `WebPAnimDecoder`. Every decoder number in this document came from it, in seconds rather than a flash cycle. `memtrack.c` now also reports the largest single allocation, which is the figure that decides whether a decode fits. |

---

## 9. What I would do next, in order

1. **Preserve the ELF, then read the coredump** (§2). Find the second panic. Everything else is guesswork until the board stops resetting.
2. **If it will not decode, bisect** (`git stash` → stock → confirm >10 min → re-apply in groups). The two memory wins and the dwell fix are the ones worth keeping; `remote.c` is the most likely source of the regression.
3. **Get a stable build that boots and runs**, with `resets at 0` for ten minutes. Do not add anything to a board that resets.
4. **Then** decide the size question (§3) with the owner: either cap what the board attempts and show the oversize screen (the fallback now works, §5.4), or fix it at the server, which is where it actually belongs.
5. **Only then** revisit the panel colour depth as a memory lever (§5.7) — it is the largest remaining controllable block, and it is a visible trade.

## 10. Honest summary of this session

Real progress, delivered messily. The fragmentation problem the previous handoff was blocked on **is solved and measured** — that is worth keeping and it is the hardest thing here to rediscover. Four individual bugs were found and fixed with evidence (the composite scratch retention, the dwell violation, the receive-buffer doubling, and the oversize screen that was too big to display), plus the old handoff's two incorrect "no effect" conclusions were corrected.

Against that: two configuration changes were made before measuring and both regressed the transport; the panel's post-flash transient was misdiagnosed and a working change was reverted because of it; and the board now resets after about a minute for a reason not yet established. The tree has ~790 added lines, several of which are unvalidated. A fresh agent should treat everything outside §4 as a hypothesis, and should read the coredump before believing anything in this document about the crash.
