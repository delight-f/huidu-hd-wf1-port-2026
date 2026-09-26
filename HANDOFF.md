# Handoff — HD-WF1 Tronbyt port

**Audience:** the next coding agent. Read this before touching anything.
**Goal:** render Tronbyt WebP artwork on the Huidu HD-WF1 (ESP32-S2, 4 MB flash, **no PSRAM**, one core) driving its stock 64×32 HUB75E panel.

The port works: it boots, joins WiFi, fetches from the Tronbyt server and puts pictures on the panel. **What is not solved is memory.** Everything below is about that, because everything on this board eventually is.

---

## 0. State right now

| | |
| --- | --- |
| Board | Runs for many minutes with `reset_reason=1`; the old ~1-minute reset has not reproduced recently and is **not explained** |
| Fetching | Works, IPv4 only, 29–326 ms typical. A 30 s timeout has been seen when the server was busy rendering |
| Stills | Decode and display |
| Animations | Decode and composite. The displayed image is now released before each fetch, so the decode gets one large contiguous block instead of competing with it (§3) |
| Colour | Canvas is RGB565 and the panel runs at **6 bits/channel with a matched CIE table**. Two separate colour traps — see traps 10 and 11 |
| Bench scaffolding | Removed (cycler, boot fills, on-panel readout). `/diag` and `/panel` remain |
| Decode arena | Implemented, **deliberately disabled** — see §5 |
| Depth / stacks / WiFi RX | Depth **6, matched by `PIXEL_COLOR_DEPTH_BITS=6` injected from the top-level CMakeLists** — the two must move together, see trap 11. Main 5120, gfx 4608, static RX 10 |
| Upstream | Rebased onto `tronbyt/firmware-esp32` `5013f42` (PR #161) — see §9 |

**Nothing here is finished.** Treat every number as a measurement with a date on it, not a guarantee.

---

## 1. The one number that governs this board

At the moment of a decode, all of this has to be **live simultaneously**:

| Live at decode time | Size |
| --- | --- |
| Received payload — resident until the decode finishes | up to 16 KB (the fetch ceiling) |
| libwebp's working set — an 11,816 B buffer plus a ~9,504 B one | ~21.3 KB |
| Composited RGB canvas (RGB565 now, was RGB888) | 4–6 KB |
| RGBA scratch, when frames are to be blended | up to 8 KB |
| | **~45–51 KB worst case** |

Free memory after WiFi and the display are up is roughly **43 KB**, and less at runtime. **It does not fit**, and — this is the part that costs time — the shortfall does not present as an error:

1. **Compositing is refused.** `only NNNNN bytes free - drawing frames directly without compositing`. The fallback decodes each frame to `MODE_RGB`, which **drops alpha**. An animation with partial or transparent frames then paints wrong pixels. *This was reported by the board owner as "missing pixels".*
2. **A later frame fails.** `frame 2 decode failed (out of memory)`. Frame 1 (full canvas) decodes; the heap is then too chopped for frame 2; the animation renders **partially**. Also reported as missing pixels.

Both are memory-shape failures wearing a rendering costume. Do not go looking for a display bug until `largest_internal` on `/diag` is comfortably above 21,320 with compositing on.

**The requirement is about shape, not size.** Three fixes in earlier sessions were aimed from total free memory and all three missed. `heap_caps_get_largest_free_block()`, not `esp_get_free_heap_size()`, is the number that predicts a decode.

---

## 2. Working with the hardware

This shell is `dash`; `IDF_PATH_FORCE=1` is required. `sdkconfig` is gitignored and **is not re-derived from the defaults once it exists** — after editing `sdkconfig.defaults.huidu-wf1` you must `rm -f sdkconfig` and re-run `set-target`.

```sh
export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1
. $IDF_PATH/export.sh
cd ~/dev/huidu-hd-wf1-port-2026
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
```

**Flashing.** `idf.py flash` re-runs the build, and the app version string comes from `git describe` — so a commit since the last build changes the ELF and invalidates a coredump you kept. To flash exactly what was built, use esptool on the build's own args:

```sh
cd build && esptool.py --chip esp32s2 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
```

**Download mode is manual.** Bridge the two GPIO0 pads beside the Micro-USB port, then power-cycle. Confirm `303a:0002` and `/dev/ttyACM0`. The running app does not enumerate USB at all, so a missing `/dev/ttyACM0` is normal and means you need download mode, not that the board is broken.

**Power-cycle before judging the panel.** A warm reset — and every flash ends in one — leaves the FM6124s showing collapsed rows. This is not a firmware fault and has already cost at least one session a wrong conclusion.

**`/diag` is the console.** There is no UART bridge and S2 USB-CDC does not enumerate without TinyUSB:

```sh
curl -s http://192.168.10.138/diag           # reset_reason, heap, boot heap trace, log ring
curl -s "http://192.168.10.138/diag?heap=1"  # free-block list + httpd/dns stack headroom
```

`reset_reason`: `1` power-on, `3` software, `4` **panic**. The IP is DHCP; the board's MAC is `30:30:f9:8d:65:68`. **The boot heap trace is the most valuable thing here** and it is served from its own fixed array, because the 1 KB log ring is overwritten within seconds of boot. It renders only 768 bytes, so its newest marks can be truncated — widen `DIAG_HEAP_TRACE_MAX` if you need them.

**Keep the ELF.** Before flashing anything new:

```sh
cp build/firmware.elf artifacts/$(date +%Y%m%d)-<what>.elf
```

`espcoredump.py info_corefile` refuses a dump whose app SHA does not match the ELF, so a rebuild destroys your ability to decode an existing dump. Coredumps land at `0x3F0000` (`boards/max_app_4mb.csv`), 64 KB.

---

## 3. Measured, so you do not have to re-derive it

- **Fragmentation, solved.** Largest free block at boot went **7,936 → 36,864 B**; internal heap **~20 KB → ~52 KB**. The original blocker — libwebp unable to get one contiguous 12,544 B block — is gone.
- **The decoder's real cost**, from `tools/webp-host-harness`: peak **21,320 B** for a 64×32 lossless frame, largest single allocation **11,816 B**. Cropping does not reduce it (output-only); scaling makes it worse (22,576 B). libwebp's internal 12,544 B buffer is not shrinkable by any option.
- **`WebPAnimDecoder` is unusable** — two full canvases plus VP8 state. `gfx.c` drives `WebPDemux` + per-frame `WebPDecode` with `output.is_external_memory = 1` and composites itself; the host harness proves that output is **byte-identical** to `WebPAnimDecoder` for the still and animated assets.
- **The dwell was being ignored.** The main loop never waited, so the board polled ~1.2 images/second against a `Tronbyt-Dwell-Secs: 10`. Fixed by sleeping only the remainder.
- **The server serves 64×32**, 3-frame animations at 268–1,316 B for this device; larger apps are 20–33 KB and are refused at the 16 KB ceiling. Its config lists this device as `type: "other"` with an **empty `info`** block, because `client_info` is only ever sent over WebSocket and this device polls HTTP.
- **Small frames fail more readily than big ones.** Frame 1 is full-canvas; frame 2 is often an 18×22 patch — and it is frame 2 that OOMs, because the heap is more chopped after the first decode, not less.
- **After the depth/canvas/stack/RX trims, verified on device:** boot heap after `ap_start` is **56,216 / 47,104** (was 41,084 / 32,768), so **+15.1 KB free and +14.3 KB of largest** at boot; `firmware.bin` is 1,258,096 B; and over a 2-minute sample all 18 reads showed `reset_reason=1`, so no resets. Small animations now composite and display with no warning.
- **It is still not enough, and now the reason is specific.** At decode time the heap shows 33–42 KB *free* but only **14,336–18,432 B contiguous**, because `gfx.c` deliberately **keeps the previous image resident so its animation can loop** (*"keep webp around to loop until the next image arrives"*, `gfx_loop()`) while `main.c` receives the next payload. libwebp wants ~21.3 KB contiguous. So `frame 2 decode failed (out of memory)` persists for the ~13 KB app, and the compositing gate — which needs 38,912 B free — is still refused at 32,916–37,656.
- **Fixed by removing the coexistence, not by finding more bytes.** `gfx_shed_retained()` (called by the main loop just before each fetch) asks the gfx task to drop the displayed image and cut its current pass short; the task frees it at the top of its loop and sets `s_shed_done`, and `gfx_queue()` clears the request when the next image arrives. The wait is bounded (~1 s), so a mid-frame shed degrades to the previous behaviour rather than stalling the display.
- **Why that is safe — the thing to hold on to:** the **HUB75 driver keeps its own framebuffer**, and the matrix is refreshed from that framebuffer by DMA, not from anything the application holds. Once a frame has been pushed, the compressed WebP is dead weight. So the panel simply holds its last drawn frame for the length of a fetch (tens to a few hundred ms) and resumes when the next image is queued. It also explains why the arena in §5 was never needed: the memory that had to be given back was already unreferenced.
- **Verified on device after flashing this and the 565 fix.** The shed fires on every fetch — 28 releases in a 3.5-minute window — and **`frame 2 decode failed` is gone**, though it had been the dominant failure. All 24 samples in that window showed `reset_reason=1`. `largest_internal` now mostly sits at 38,912–47,104 instead of 14,336–18,432.
- **Two residuals, both about contiguous size rather than free size.** (1) The compositing gate still refuses from time to time — `only 38472 bytes free` against its 38,912 threshold, a 440-byte miss — because it tests *total free* when the real requirement is *one big run*. (2) One decode still failed frame 1: `frame 1 decode failed (out of memory), free 41308 largest 19456`. Plenty free, not enough contiguous, against libwebp's ~21,320.
- **The gate is the cheap fix and the honest one.** Allocate the scratch first, then require `heap_caps_get_largest_free_block()` to clear ~22 KB — just over the measured 21,320 — and give the scratch back if it does not. That gates on the number that actually predicts a decode instead of a proxy for it, and it is the same lesson this port keeps relearning. The frame-1 residual is a separate, harder problem: genuine fragmentation, not the gate.
- **A second and larger colour bug, independent of the 565 layout: the panel depth.** It was trimmed 8 → 6 → 5 for memory, and the driver's colour path does not survive that — see trap 11. `depth_sim` shows depth 5 mapping input 200 → 82 and 139 → 205, non-monotonic, which is the hue inversion reported from the panel.
- **Depth 8 fixed the colour, and cost far more than expected.** Verified on device: colours correct, 22/22 samples stable — but the panel-framebuffer stage is 16,160 bytes at 5 bits against 31,544 at 8, so ~15.3 KB, not the ~6 KB the "2 KB per bit" note implied. The consequence was immediate and is worth remembering: free heap settled at 37,9xx–38,6xx against a 38,912 composite threshold, so compositing was refused essentially always (alpha dropped → incomplete and black pixels), and frame 1 stopped decoding 16 times in 3.5 minutes whenever the largest run dipped under ~21,320. Fixing the colour had *re-broken the memory*.
- **Compositing is now unconditional, and that is what fixed the black squares.** The gate is gone; the scratch is reserved at boot by gfx_reserve_decode_buffers(). Verified on device: **zero compositing refusals** in a 3.3-minute window (was ~30), 22/22 samples stable, and the reservation costs the runtime heap *nothing* — `canvas reserve: after` shows free 168,716 → 156,420 with largest **unchanged at 139,264**, and `after ap_start` stays 51,140/43,008, identical to the build without it. Holding the 8 KB did take the run-time largest from ~24,576 to ~21,504, which is still clear of the 21,320 a frame needs.
- **Why the gate had to go rather than be tuned, since two commits were spent tuning it.** The scratch is 8 KB and, allocated per fetch, it lands wherever there is room: the largest free run read 24,576 released and 17,408–26,624 held. So holding it was splitting in half the very block the next frame's decode needed, and every version of the gate was reading that self-inflicted drop. Worse, the server serves **hundreds of unrelated apps** — stills, animations, scrolls, of very different weight — so any threshold on available memory is one that some app trips, and a tripped app renders with alpha dropped, silently and permanently. An unpredictable workload wants the working set guaranteed, not tested for.
- **Two decode failures remain** (`frame 1 decode failed (out of memory), free 37028 largest 16384`) out of 22 samples — genuine fragmentation at the moment of the fetch, and the last structural item on this board.
- **The failed-render loop, found and fixed.** The "freezing" was never a stall: the main-loop clock advanced every 10.2 s throughout, and 51 draws out of ~152 failed. The server serves a **pinned** app, so `/next` returned the same 12,552-byte image every cycle, and that image could not decode — `free 37012 largest 17408`, reproducibly. `draw_webp` failed, gfx painted the error pixel and dropped the image, main held the stale frame for the rest of the dwell, and it repeated. **Pinning is what makes a decode failure permanent rather than transient** — every retry fetches the identical bytes. Worth remembering for any app, and for how a memory failure presents: as a freeze, not as an error.
- **Cause: the payload buffer was splitting the heap.** 17,408 + the 12,552-byte payload + smaller blocks accounted for the 37,012 free. It is the last large allocation before the decode and lives across all of it, so it lands in the middle of the free space at exactly the wrong moment. It is now reserved at boot alongside the canvas and the scratch and reused per request, so a fetch allocates nothing at all. Verified: **zero** decode failures and zero failed draws over 4 minutes on the same image that had been failing 51 times, 60/60 samples `reset_reason=1`.
- **Ownership changed with it.** `remote_get()`'s buffer may now be the reservation, so it must be returned with `remote_payload_release()` and never `free()` — freeing it would destroy the slot and leave the cached pointer dangling. Every payload release in `remote.c` and `gfx.c` is routed through it. The WebSocket path is untouched: separate ownership domain, never the slot.
- **The margin is thin, and that is the honest state.** Runtime `largest_internal` now sits at 22,528–23,552 against the 21,320 a frame needs — 1.2–2.2 KB, with occasional dips to 7,680–9,216. It held for the whole window, but a hungrier app could still tip it. The reservation also cost real boot headroom where the scratch did not: `after ap_start` went from 51,140/43,008 to 34,728/26,624.
- **The server moved, and it is a harder target: 21,320 → 25,656 bytes.** The device now talks to `http://192.168.10.233:8000/panel/next` (one URL, scheme picks the transport — `ws://` for WebSocket, and note the new URL carries no device id, so the panel is identified by the API key that lives only in NVS). Its artwork measures a **25,656-byte** decoder working set on the host harness where the old server's measured 21,320 — against a `largest_internal` of 22,528. So ~40% of draws failed, and each failure held the panel for the server's `Tronbyt-Dwell-Secs: 15`. That is what "hung on the loading pigeon" was: not a hang, a run of failed draws, with the boot animation's last frame left on screen. The immediate cause was the payload reservation being too small to hold the body, so the decode had the payload and its working set competing for the same space; the sizing is now known to be **still wrong in the other direction** — see the note on `REMOTE_PAYLOAD_RESERVE` below.
- **Changing servers also exposed a receive-path stall that the old server did not.** The new one uses `Transfer-Encoding: chunked`; the old one sent `Content-Length`. Against it the receive path stalls with a signature that is always identical: exactly **1,225 bytes** received, then nothing until the 30 s timeout — one TCP window's worth, and the same 1,226-byte figure already recorded in `sdkconfig.defaults.huidu-wf1` from the TCP_WND experiment. Roughly a fifth of fetches, 30 s each.
- **What is established about that stall, and what is not.** Established: `STATIC_RX` 12 **and** `STATIC_TX` 10 are both load-bearing — trimming RX to 10 or TX to 8 both made it markedly worse (TX to 8 gave 10 short payloads and 16 timeouts in two minutes, against 2 and 1). The reason TX matters is easy to get wrong: **ACKs go out through the TX path, so transmit buffers bound the receive window.** Raising `TCP_WND` was already known to make it worse. Not established: why the window never *reopens*. The two candidates are CPU starvation of the lwIP/tcpip task — the panel's DMA refresh plus a decode competing for one core — and an lwIP pool running dry. `TCPIP_RECVMBOX_SIZE`, `TCP_QUEUE_OOSEQ` and the boot heap trace are the places to look, and `diag_dump_heap_layout()` is available to fire at the moment of a stall.
- **With RX 12 and TX 10 the transport is clean** — a later two-minute window showed **0 short payloads and 0 timeouts**. That fault is closed unless a knob moves again.
- **But the new server's app set is much bigger than this board was tuned for, and that is the open problem now.** Measured over two minutes: **18** bodies outgrew the payload reservation, **15** fetches were refused at the 16,384-byte ceiling (`413` → oversize graphic), and **12** draws failed out of memory, against 15 images displayed. Bodies run from 4 KB to **beyond 16 KB** — one was watched growing 4,392 → 8,920 → 13,112 → past the ceiling. The 16 KB ceiling and the whole memory budget were sized against the *old* server's app set.
- **The decoder's working set scales with the image, and 25,656 was the small case.** That figure came from a 4,082-byte body. The 12 failures are the larger apps, whose working set is correspondingly larger — while `largest_internal` sat between 14,336 and 29,696 during the window.
- **`REMOTE_PAYLOAD_RESERVE` is undersized at 6 KB and should go back up.** It was set from a bad sample: ten consecutive fetches all returned 4,082 bytes, so 6 KB looked like ample coverage, but the server rotates apps and **18 bodies in two minutes outgrew it**, each one falling back to the per-request allocation the reservation exists to avoid. Size it against the real body range, not a sample of one app.
- **The horizontal lines across the panel are probably single-buffer tearing, and that is a hypothesis, not a finding.** Double buffering is off (`sdkconfig`/`display.cpp`; turning it on costs ~21 KB), so drawing writes into the memory the DMA is scanning out and partial updates appear as horizontal streaks. That is invisible at one image per 15 s and very visible under the current redraw storm — error pixels, the oversize graphic, and the 500 ms retry added in `ffac747` all redraw the panel frequently, so that change likely made the tearing worse. Confirm by watching the panel while nothing is failing before treating it as a separate fault.
- **The budget says this is structural, which is the honest place to leave it.** For a 13 KB app the board needs roughly 13 KB payload + a working set above 25,656 + 8,192 scratch + 4,096 canvas ≈ 51 KB against ~40 KB free. The old server's apps sat just inside that line; these do not, and no firmware setting closes it. The fix is server-side: serve this device panel-sized assets of a few KB each — the same conclusion the README already reaches about oversized assets.

---

## 4. What was removed, and why it matters

The bench scaffolding is gone: the GPIO11 panel-config cycler (`panel_sweep.c/.h`), its solid boot colour, the R/G/B fills and heap readout in `display_initialize()`, and the on-panel decode diagnostic (`diag_panel` / `display_diag_show`). `/diag` over the network replaced all of it.

Removing the cycler was worth **3,072 bytes** — not because of the code, but because its polling task held a **heap stack**. That is the sort of accounting this board demands: the graphics task's 4.5 KB stack and the main task's 5 KB are heap, and they compete directly with the decoder.

IPv6 is compiled out (`CONFIG_LWIP_IPV6=n`). Nothing here needs it — the image URL is an IPv4 literal and OTA is impossible with a single factory slot. It was worth ~2 KB of RAM and ~31 KB of flash. Four places depended on it: `ota.c`'s `AF_INET6` branch and its `INET6_ADDRSTRLEN` buffer, `wifi.c`'s `esp_netif_create_ip6_linklocal` call and its `IP_EVENT_GOT_IP6` handler. All are `#if LWIP_IPV6`-guarded, so the tree still builds with IPv6 on.

---

## 5. The arena experiment — read this before trying it again

The idea: reserve ~22 KB once the station is linked and the display is initialised, hold it so the region cannot be fragmented, then hand it back immediately before each `WebPDecode` and re-take it after. It **works for the decode** — the intermittent OOM goes away entirely.

It also **breaks fetching**, and that is worse. Holding 22 KB plus the 8 KB scratch left `largest_internal` at **4,096**, and the receive path needs 6–8 KB in one piece to grow its buffer:

```
E remote: Resizing response buffer to 6144 bytes failed (free 11224 largest 4096)
E remote: Receive buffer lost mid-transfer after 4905 bytes - discarding
```

A fetch that cannot complete shows *nothing*; a decode that cannot fit falls back to unblended frames. Net regression, reverted.

It is kept in `gfx.c` behind `GFX_DECODE_ARENA_ENABLED 0` with the full reasoning. Two things to know if you pick it up:

- It was **lost to a concurrent allocation** on its first decode (`decode arena lost to another task`) — the main task was setting up the next fetch in the gap. Any give/take design has this race; the gfx task can be preempted at any point, including immediately after the free.
- The real problem is coexistence. Something has to give: the reservation, the scratch, or the payload ceiling. Pick deliberately rather than adding a third mechanism.

---

## 6. What I would do next, in order

1. **The trims landed — confirmed on device.** Boot heap after `ap_start` is 56,216 / 47,104, and the board is stable across minutes. See §3.
2. **Done — the retained-image shed and the 565 fix are on the board and verified** (§3). Frame-2 failures are gone and 24/24 samples were stable.
3. **Done and verified: depth 6 with a matched CIE table, unconditional compositing, and a boot-reserved payload buffer.** Colours are true, and the failed-render loop is gone — zero decode failures and zero failed draws over 4 minutes on the image that had been failing 51 times. See §3.
4. **What is left is the receive-path stall, and it is the only known fault.** The decoder now has the room this server needs (largest 32,768–36,864 against a 25,656-byte requirement), and draws no longer hold the panel when they fail. But roughly a fifth of fetches stall at exactly 1,225 bytes and cost 30 s each — see §3. Both WiFi buffer counts are load-bearing. What is unknown is why the TCP window never reopens after that first one: fire `diag_dump_heap_layout()` at a stall, and look at `TCPIP_RECVMBOX_SIZE` / `TCP_QUEUE_OOSEQ`, and at whether the lwIP task is being starved by the panel's DMA refresh plus the decode sharing one core.
5. **Re-open the arena only if 4 fails**, sized by those numbers. It was parked for starving the *receive* path, so any give/take design has to leave a fetch its 6–8 KB.
6. **Treat the payload ceiling as a last resort.** Dropping it to ~8 KB shrinks the payload term but refuses more apps, and the server is where oversized assets genuinely belong.
7. **Measure the gfx task's stack watermark on a *successful* decode.** It was trimmed to 4,608 on a reading taken while decodes were failing, which understates the real peak. If it is tight, that shows up as a reset, and you are back in §7.
8. **Keep the ELF before every flash.**

---

## 7. Traps that have each cost a session

1. **Warm reset leaves the panel collapsed.** Power-cycle before believing anything you see on the matrix.
2. **`sdkconfig` is not re-applied from the defaults.** `rm -f sdkconfig` and `set-target` after editing defaults, or you are testing the old configuration.
3. **Panel overrides live in NVS and survive reflashing.** `/panel?...` sets them; `/panel` with no params still **commits and reboots**. Clear with `/panel?clear=1`.
4. **`main/` is also a directory, so `git` arguments like `main` are ambiguous** — use `refs/heads/main`.
5. **The shell is `dash`** — no process substitution, no `${PIPESTATUS[@]}`.
6. **`managed_components/` is gitignored** and re-fetched; do not edit it.
7. **`CONFIG_BOOT_WEBP_PARROT` must stay a small asset.** The boot WebP is decoded at startup; `BOOT_WEBP_TRONBYT` is 174 KB and cannot be allocated, which fails display init before the portal starts.
8. **gpio0 is both the download strap and the button.** Keep that in mind before wiring anything to it.
9. **`idf.py flash` rebuilds.** See §2 — it silently invalidates a preserved ELF.
10. **libwebp's `MODE_RGB_565` output is not a native `uint16_t`.** `WEBP_SWAP_16BIT_CSP` defaults to 0, so both the lossy (`VP8YuvToRgb565`) and lossless (`VP8LConvertBGRAToRGB565_C`) paths write two bytes per pixel as `[rg][gb]`: byte 0 is red in bits 7..3 plus the top three bits of green, byte 1 is the rest of green plus blue. Reading the pair as a `uint16_t` on this little-endian part transposes the channels — it shows up as **near-white pixels turning blue or green**, while pure white and black survive, because the error cancels when both bytes are equal. `rgb_to_565()`/`rgb_from_565()` in `display.h` are the only place that layout is expressed; do not reimplement it inline. `tools/webp-host-harness/rgb565_check` proves it against an RGBA decode (byte-wise: exact, worst error 5; word-wise: wrong on 306 of 2061 opaque pixels of a real asset).
11. **The panel colour depth is a compile-time contract, not a run-time knob.** `PIXEL_COLOR_DEPTH_BITS` — 6 in this build — selects the CIE table *and*, through `updateMatrixDMABuffer`, the bits read back out of it: it takes the **low** `depth` bits, which is only right if the table's output range matches. Pass any run-time depth below 8 and every channel becomes a sawtooth, each wrapping at a different point, so a pixel's channels reorder. Reported from the panel as brown rendering pink and blue rendering green. The depth was trimmed 8 → 6 → 5 for memory and **all of it was colour-broken**; 6 merely failed less often, because the sawtooth period is longer. **The port runs 6, and the top-level `CMakeLists.txt` injects `PIXEL_COLOR_DEPTH_BITS=6` into the library to match** — if you touch either, dump the macro to check the other took (see §2). 6 rather than 8 because 8, for all that it is upstream's default and needs no rebuild, **costs ~15.3 KB here rather than the ~2 KB per bit the framebuffer maths suggests**: the panel-framebuffer stage measures 16,160 bytes at 5 bits and 31,544 at 8, because the library sizes its DMA descriptor allocation from the depth too. At 8 that starved the composite gate and frames stopped decoding. `cie_luts.h` has native tables for 4/6/7/8/10/12 only, and its 5-bit fallback is dead code, because the .cpp tests `#ifdef LUT_NATIVE_BIT_DEPTH` rather than its value. `tools/webp-host-harness/depth_sim` prints the mapping: at depth 5, input 200 mapped to 82 while input 139 mapped to 205.

---

## 8. Code map

| File | Why it matters |
| --- | --- |
| `main/gfx.c` | **the file to work in.** `draw_webp()` (per-frame decode at each frame's own offset, optional RGBA scratch + key-frame/blend/dispose compositing, the `GFX_DECODE_HEADROOM` gate, the `s_shed_wanted` early-exit); `gfx_queue()`/`gfx_update()`; **`gfx_shed_retained()`** (the release-before-fetch handshake, §3); `gfx_reserve_decode_buffers()`; the disabled arena; `vp8_status_name()`; the boot heap trace. `GFX_TASK_STACK_SIZE 4608`. |
| `main/display.cpp` / `.h` | WF1 pin map; per-board panel defaults and `kPanelColorDepthBits` (now **5**); `display_draw_565()`; and **`rgb_to_565()`/`rgb_from_565()` — the only place the 565 byte layout is written down** (trap 10). NULL-`_matrix` guards. |
| `main/remote.c` | HTTP fetch; receive-buffer growth (this is what the arena starved); completeness checks. |
| `main/main.c` | `app_main` boot order, the poll loop, the dwell sleep, fetch latency + stack watermark logging. |
| `main/ap.c` / `.h` | `/diag`, `/panel`, `/save`, `/update`; `ap_start()`, `ap_retire_softap()`, `ap_start_dns()`. |
| `main/diag.c` / `.h` | 1 KB log ring; the boot heap trace (**16 slots, 768-byte report**); `?heap=1` probe. |
| `main/ota.c` | Now IPv4-only capable; guarded for `LWIP_IPV6` both ways. |
| `sdkconfig.defaults.huidu-wf1` | S2, 4 MB, no PSRAM, **IPv6 off**, small boot asset, coredumps to flash, RX 10, main stack 5120. Every tuned value carries its measurement — read them before changing. |
| `boards/max_app_4mb.csv` | Single `factory` slot — **OTA impossible**. Coredump at `0x3F0000`. |
| `tools/webp-host-harness/` | **Use this first.** Measures the decoder's transient heap and renders comparisons against `WebPAnimDecoder`. Every decoder number above came from it, in seconds rather than a flash cycle. |

---

## 9. Repository state

The port sits on a **grafted history**: the local root `8da4032` was parentless, with a tree identical to upstream `d77c076` (PR #157). It has since been rebased onto upstream `5013f42` (#161), so the port's commits now have real upstream ancestry. Backups of the pre-rebase state: branch `backup/pre-upstream-sync` and a matching tag.

Because of that rebase, **local `main` and `origin/main` have diverged and updating the remote needs a force-push.** Do not do that without asking the owner.

`artifacts/` is gitignored and holds preserved ELFs, one per flashed build, named for what changed. Keep that up.
