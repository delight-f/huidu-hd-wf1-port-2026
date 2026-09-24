#pragma once

#include "esp_heap_caps.h"
#include "sdkconfig.h"

// Allocation caps for the HTTP/WebSocket image buffers and the config-portal
// scratch buffers.
//
// Upstream uses MALLOC_CAP_SPIRAM unconditionally to keep these buffers out of
// internal RAM. That is fine on the ESP32/S3 targets, which have PSRAM, but on a
// no-PSRAM chip (e.g. the ESP32-S2 in the Huidu HD-WF1, where CONFIG_SPIRAM=n)
// heap_caps_malloc(..., MALLOC_CAP_SPIRAM) returns NULL with no fallback, which
// kills every image fetch and the config portal. Fall back to internal RAM when
// PSRAM is unavailable so the same code works on both.
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#define IMAGE_BUF_CAPS MALLOC_CAP_SPIRAM
#else
#define IMAGE_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
