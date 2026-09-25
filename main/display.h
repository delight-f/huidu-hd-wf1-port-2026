#pragma once

#include <stddef.h>
#include <stdint.h>

#define DISPLAY_MAX_BRIGHTNESS 100
#define DISPLAY_MIN_BRIGHTNESS 0
#define DISPLAY_DEFAULT_BRIGHTNESS 20
extern volatile int32_t isAnimating; // Declare the variable
#ifdef __cplusplus
extern "C" {
#endif
int display_initialize(void);
void display_set_brightness(uint8_t brightness_pct);
void display_shutdown(void);

// Compile-time panel geometry for the active board. Lets other modules size
// their buffers to the panel (see gfx_reserve_decode_buffers) without waiting
// for display_initialize() to have run.
int display_panel_width(void);
int display_panel_height(void);

// The RGB565 canvas the decoder writes into, and the layout it writes.
//
// libwebp emits MODE_RGB_565 as two bytes per pixel in a fixed order: byte 0 is
// `rg` (red in bits 7..3, the top three bits of green in 2..0) and byte 1 is `gb`
// (the rest of green, then blue). Both the lossy (VP8YuvToRgb565) and lossless
// (VP8LConvertBGRAToRGB565_C) paths use it, and it is *not* a native uint16_t -
// WEBP_SWAP_16BIT_CSP defaults to 0, so reading the pair as a uint16_t on this
// little-endian part transposes the channels. That mistake shows up as near-white
// pixels turning blue or green, while pure white and black survive it, because
// the errors cancel when both bytes are equal.
//
// These two helpers are the only place that layout is expressed.
static inline uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t rg = (uint8_t)((r & 0xF8) | (g >> 5));
  const uint8_t gb = (uint8_t)(((g << 3) & 0xE0) | (b >> 3));
  return (uint16_t)((uint16_t)rg | ((uint16_t)gb << 8));
}

static inline void rgb_from_565(uint16_t p, uint8_t *r, uint8_t *g,
                                uint8_t *b) {
  const uint8_t rg = (uint8_t)(p & 0xFF);
  const uint8_t gb = (uint8_t)(p >> 8);
  const uint8_t r5 = (uint8_t)(rg >> 3);
  const uint8_t g6 = (uint8_t)(((rg & 0x07) << 3) | (gb >> 5));
  const uint8_t b5 = (uint8_t)(gb & 0x1F);
  // Replicate the high bits into the low ones so the full 8-bit range is used
  // rather than leaving everything dark.
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

void display_draw_565(const uint16_t* pix, int width, int height);

void display_clear(void);
void display_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void display_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g,
                       uint8_t b);
void draw_error_indicator_pixel(void);
void display_text(const char* text, int x, int y, uint8_t r, uint8_t g,
                  uint8_t b, int scale);
void display_flip(void);

#ifdef __cplusplus
}
#endif
