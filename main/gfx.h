#pragma once

#include <esp_websocket_client.h>
#include <stddef.h>

int gfx_initialize(const char* img_url);

// Pre-allocate the decode canvas, sized from the panel geometry. Called from
// app_main *before* wifi_initialize() so the buffer lands early in the heap
// instead of splitting the largest free run once WiFi, the HTTP server and
// every task stack are already up. Cheap and safe: the canvas is allocated once
// and held for the life of the program either way, so this only changes where
// it lands.
void gfx_reserve_decode_buffers(void);

// Reserve the decoder's working runway. Call once the station link is up and the
// display is initialised - not before, or the WiFi join loses the large
// contiguous allocation it needs. See the comment in gfx.c.
void gfx_reserve_decode_arena(void);

void gfx_set_websocket_handle(esp_websocket_client_handle_t ws_handle);
int gfx_update(void* webp, size_t len, int32_t dwell_secs);
int gfx_get_loaded_counter(void);
int gfx_display_asset(const char* asset_type);
void gfx_display_text(const char* text, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b, int scale);
// Release the displayed image so the next fetch is not competing with it.
//
// The gfx task normally keeps the compressed image so the animation can keep
// looping between fetches. That retention is what stops a later animation frame
// from decoding: at decode time the heap has 33-42 KB free but only ~14-18 KB of
// it contiguous, because the retained image and the incoming payload are two
// separate blocks and libwebp wants ~21 KB in one piece.
//
// Dropping it is invisible - the HUB75 driver keeps its own framebuffer, so the
// panel holds the last drawn frame while the loop is paused, and the loop resumes
// as soon as the next image is queued. Blocks briefly for the task to honour it.
void gfx_shed_retained(void);

void gfx_stop(void);
void gfx_start(void);
void gfx_shutdown(void);
