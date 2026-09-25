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
