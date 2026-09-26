#pragma once

#include <stdbool.h>
#include <stdint.h>

// A reserved receive buffer, so a fetch does not carve one out of a heap the
// display is already using.
//
// The payload buffer stays live for the whole decode and is placed wherever it
// happens to fit, so allocating it per request puts it in the middle of the free
// space at exactly the moment libwebp needs one contiguous ~21.3 KB block. Taken
// at boot it comes off a ~139 KB block before WiFi or the HTTP server have run
// and costs the run-time heap nothing - the same argument as the composite
// scratch in gfx_reserve_decode_buffers(). Measured before this: a decode failed
// against `free 37028 largest 16384`, i.e. plenty free, nothing usable.
//
// Call once, early, next to gfx_reserve_decode_buffers().
void remote_reserve_payload_buffer(void);

// Give a payload buffer back. The reserved slot returns to the pool; anything
// else - a one-off allocation, taken when the slot was still busy - is freed.
//
// remote_get()'s buffer must be released with this and not free(): the pointer
// may be the reservation, and free() would both destroy it and leave the cached
// pointer dangling for the next acquire.
void remote_payload_release(void* buf);

// Retrieves url via HTTP GET.
//
// The caller takes ownership of *buf and must return it with
// remote_payload_release(). ota_url and image_url (if not NULL) are ordinary heap
// strings and are free()d normally. `buf` is NULL on failure.
int remote_get(const char* url, uint8_t** buf, size_t* len,
               uint8_t* brightness_pct, int32_t* dwell_secs, int* return_code,
               char** ota_url, char** image_url, bool* reboot_requested);