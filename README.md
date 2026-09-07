# firmware-esp32 (Huidu HD-WF1 Branch — 2026)

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.2-blue?logo=espressif)
![License](https://img.shields.io/github/license/tronbyt/firmware-esp32)
![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen)
![Year](https://img.shields.io/badge/year-2026-darkgreen)

This branch adds experimental support for the **Huidu HD-WF1** 64×32 HUB75E LED matrix panel on an **ESP32-S2** (4MB flash, no PSRAM), alongside the existing ESP32-based boards.

## ⚠️ Status
**Hardware-untested.** All changes compile and pass `image_info`, but this firmware has **not** been flashed to or validated against real HD-WF1 hardware. Use at your own risk.

## Build
```bash
export PATH=$HOME/.idf-venv/bin:$PATH
. $HOME/esp-idf/export.sh
cd $HOME/firmware-esp32

# Target the HD-WF1 configuration specifically
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.huidu-wf1" set-target esp32s2
idf.py build
```

## New Files / Changes
| File                       | Change                                                   |
|----------------------------|----------------------------------------------------------|
| `main/Kconfig.projbuild`   | Added `BOARD_HUIDU_WF1` choice block                     |
| `main/display.cpp`         | Added pin map for HD-WF1 (GPIO 2–39 per datasheet)     |
| `main/gfx.c`               | Core-pinning fix: `#if CONFIG_IDF_TARGET_ESP32S2` guard |
| `sdkconfig.defaults.huidu-wf1` | New file — S2 target, SPIRAM disabled, panel pins      |

## Flash (untested)
```bash
esptool --chip esp32s2 --before no_reset --after hard_reset write_flash 0x0 build/merged_firmware.bin
```
Bridge the two pads near the Micro-USB port on the WF1 to enter download mode, then connect via the USB-A port.

## Notes
- Single-core S2 fix for `xTaskCreatePinnedToCore`: `GFX_TASK_CORE` forced to `0`.
- `CONFIG_SPIRAM=n` is set in defaults, but check `sdkconfig` after configure.
- `swap_colors` excluded from `display.cpp` per original spec; revisit after bench test.

## Upstream
This branch is intended for submission as a PR to [tronbyt/firmware-esp32](https://github.com/tronbyt/firmware-esp32).
