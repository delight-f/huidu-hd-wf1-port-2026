#include "display.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>

#include "font5x7.h"
#include "nvs_settings.h"
#if CONFIG_BOARD_TIDBYT_GEN2
#define R1 5
#define G1 23
#define BL1 4
#define R2 2
#define G2 22
#define BL2 32

#define CH_A 25
#define CH_B 21
#define CH_C 26
#define CH_D 19
#define CH_E -1  // assign to pin 14 if using more than two panels

#define LAT 18
#define OE 27
#define CLK 15

#elif CONFIG_BOARD_TRONBYT_S3_WIDE
#define R1 4
#define G1 5
#define BL1 6
#define R2 7
#define G2 15
#define BL2 16

#define CH_A 17
#define CH_B 18
#define CH_C 8
#define CH_D 3
#define CH_E 46
#define LAT 9
#define OE 10
#define CLK 11

#define WIDTH 128
#define HEIGHT 64
#elif CONFIG_BOARD_TRONBYT_S3
#define R1 4
#define G1 6
#define BL1 5
#define R2 7
#define G2 16
#define BL2 15

#define CH_A 17
#define CH_B 18
#define CH_C 8
#define CH_D 3
#define CH_E -1

#define LAT 9
#define OE 10
#define CLK 11
#elif CONFIG_BOARD_PIXOTICKER
#define R1 2
#define G1 4
#define BL1 15
#define R2 16
#define G2 17
#define BL2 27
#define CH_A 5
#define CH_B 18
#define CH_C 19
#define CH_D 21
#define CH_E 12
#define CLK 22
#define LAT 26
#define OE 25

#elif CONFIG_BOARD_WAVESHARE_S3
#define R1 4
#define G1 5
#define BL1 6
#define R2 7
#define G2 15
#define BL2 16

#define CH_A 18
#define CH_B 8
#define CH_C 3
#define CH_D 42
#define CH_E 9

#define LAT 40
#define OE 2
#define CLK 41

#elif CONFIG_BOARD_MATRIXPORTAL_S3_WIDE
#define R1 42
#define G1 41
#define BL1 40
#define R2 38
#define G2 39
#define BL2 37
#define CH_A 45
#define CH_B 36
#define CH_C 48
#define CH_D 35
#define CH_E 8      // The crucial update mapping address E to GPIO 8
#define CLK 2
#define LAT 47
#define OE 14

#define WIDTH 128
#define HEIGHT 64

#elif CONFIG_BOARD_MATRIXPORTAL_S3
//                     R1, G1, B1, R2, G2, B2
// uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
// uint8_t addrPins[] = {45, 36, 48, 35, 21};
// uint8_t clockPin = 2;
// uint8_t latchPin = 47;
// uint8_t oePin = 14;
#define R1 42
#define G1 41
#define BL1 40
#define R2 38
#define G2 39
#define BL2 37
#define CH_A 45
#define CH_B 36
#define CH_C 48
#define CH_D 35
#define CH_E 21
#define CLK 2
#define LAT 47
#define OE 14
#elif CONFIG_BOARD_HUIDU_WF1
#define R1 2
#define G1 6
#define BL1 3
#define R2 4
#define G2 8
#define BL2 5

#define CH_A 39
#define CH_B 38
#define CH_C 37
#define CH_D 36
#define CH_E 12

#define LAT 33
#define OE 35
#define CLK 34
#else  // GEN1 from here down.
#define R1 2
#define G1 22
#define BL1 21
#define R2 4
#define G2 27
#define BL2 23
#define CH_A 26
#define CH_B 5
#define CH_C 25
#define CH_D 18
#define CH_E -1  // assign to pin 14 if using more than two panels

#define LAT 19
#define OE 32
#define CLK 33

// Genuine Tidbyt hardware: match the stock HDK brightness convention
// (0-100% feeds setBrightness8() 1:1, ~39% max panel PWM duty).
#define BRIGHTNESS_8BIT_MAX 100
#endif

#ifndef WIDTH
#define WIDTH 64
#endif

// Boards that drive a fixed 128x64 panel set HEIGHT in their block above.
// Everything else is a 64-wide panel that is either 32 or 64 rows tall, which
// is the same wiring either way plus the E address line, so the row count
// comes from the build target rather than from a separate board definition.
#ifndef HEIGHT
#if CONFIG_PANEL_HEIGHT_64
#define HEIGHT 64
#else
#define HEIGHT 32
#endif
#endif

static MatrixPanel_I2S_DMA *_matrix;
static uint8_t _brightness = DISPLAY_DEFAULT_BRIGHTNESS;
static const char *TAG = "display";

// Per-board ceiling for the 0-100% -> setBrightness8() (0-255) mapping.
// Genuine Tidbyt hardware (Gen1/Gen2) defines this as 100 in its board block
// above, matching the stock HDK convention where the brightness percentage
// feeds setBrightness8() 1:1 (max ~39% panel PWM duty). Third-party panels with
// no Tidbyt reference fall back to the legacy 230 (~90% duty) and can be tuned
// empirically per board.
#ifndef BRIGHTNESS_8BIT_MAX
#define BRIGHTNESS_8BIT_MAX 230
#endif

// Panel driver/timing defaults per board. Overridable at runtime via the
// /panel endpoint (NVS keys panel_drv/spd/lat/ph/dbfr) so the right combination
// for a given panel can be found on the bench without reflashing.
//   drv: 0=SHIFTREG 1=FM6124 2=FM6126A 3=ICN2038S 4=MBI5124 5=DP3246
//   spd: 0=8MHz 1=20MHz
// The WF1 BCM framebuffer lives in DMA-capable internal RAM and this S2 has no
// PSRAM, so double buffering stays off there (the reference uses the library
// default off as well).
#if CONFIG_BOARD_HUIDU_WF1
#define PANEL_DRIVER_DEF 1 /* FM6124 - the panel's LED driver ICs */
#define PANEL_SPEED_DEF 1  /* 20 MHz */
#define PANEL_LATCH_DEF 1
#define PANEL_DBUFF_DEF 0
#define PANEL_LINE_DEF 0 /* TYPE138 */
#else
#define PANEL_DRIVER_DEF 2 /* FM6126A */
#define PANEL_SPEED_DEF 0  /* 8 MHz */
#define PANEL_LATCH_DEF 1
#define PANEL_DBUFF_DEF 1
#define PANEL_LINE_DEF 0 /* TYPE138 */
#endif

#if CONFIG_NO_INVERT_CLOCK_PHASE
#define PANEL_PHASE_DEF 0
#else
#define PANEL_PHASE_DEF 1
#endif

static int panel_cfg_get(const char *key, int def) {
  nvs_handle_t h;
  if (nvs_open("wifi_config", NVS_READONLY, &h) != ESP_OK) {
    return def;
  }
  int32_t v = def;
  if (nvs_get_i32(h, key, &v) != ESP_OK) {
    v = def;
  }
  nvs_close(h);
  return (int)v;
}

static inline uint8_t brightness_percent_to_8bit(uint8_t pct) {
  return (uint8_t)(((uint32_t)pct * BRIGHTNESS_8BIT_MAX + 50) / 100);
}

int display_initialize(void) {
  // Get swap_colors setting
  bool swap_colors = nvs_get_swap_colors();

  // Initialize all pins to their default configuration
  int8_t pin_R1 = R1, pin_G1 = G1, pin_BL1 = BL1;
  int8_t pin_R2 = R2, pin_G2 = G2, pin_BL2 = BL2;

  // Apply board-specific color swap
  if (swap_colors) {
#if CONFIG_BOARD_MATRIXPORTAL_S3 || CONFIG_BOARD_MATRIXPORTAL_S3_WIDE || CONFIG_BOARD_TRONBYT_S3
    // Swap green and blue channels
    int8_t tmp = pin_G1; pin_G1 = pin_BL1; pin_BL1 = tmp;
    tmp = pin_G2; pin_G2 = pin_BL2; pin_BL2 = tmp;
#elif CONFIG_BOARD_TIDBYT_GEN1
    // Rotate R -> BL -> G -> R
    int8_t tmp = pin_R1; pin_R1 = pin_BL1; pin_BL1 = pin_G1; pin_G1 = tmp;
    tmp = pin_R2; pin_R2 = pin_BL2; pin_BL2 = pin_G2; pin_G2 = tmp;
#endif
  }

  ESP_LOGI(TAG, "Initializing display with swap_colors=%s",
           swap_colors ? "true" : "false");

  // Initialize the panel.
  HUB75_I2S_CFG::i2s_pins pins = {pin_R1,  pin_G1, pin_BL1, pin_R2, pin_G2,
                                  pin_BL2, CH_A,   CH_B,    CH_C,   CH_D,
                                  CH_E,    LAT,    OE,      CLK};

  int drv = panel_cfg_get("panel_drv", PANEL_DRIVER_DEF);
  int spd = panel_cfg_get("panel_spd", PANEL_SPEED_DEF);
  int latch = panel_cfg_get("panel_lat", PANEL_LATCH_DEF);
  int ph = panel_cfg_get("panel_ph", PANEL_PHASE_DEF);
  int dbfr = panel_cfg_get("panel_dbfr", PANEL_DBUFF_DEF);
  int line = panel_cfg_get("panel_line", PANEL_LINE_DEF);
  if (drv < 0 || drv > 5) {
    drv = PANEL_DRIVER_DEF;
  }
  if (line < 0 || line > 3) {
    line = PANEL_LINE_DEF;
  }
  if (latch < 1) {
    latch = 1;
  }
  if (latch > 8) {
    latch = 8;
  }
  ESP_LOGI(TAG,
           "Panel config: driver=%d line=%d speed=%d latch_blanking=%d phase=%d "
           "double_buff=%d",
           drv, line, spd, latch, ph, dbfr);

  // Panel colour depth, in bits per channel.
  //
  // THIS MUST MATCH PIXEL_COLOR_DEPTH_BITS, which the top-level CMakeLists sets to
  // 6 for this board. The driver picks its CIE correction table from that macro at
  // COMPILE time and then reduces a colour by reading its LOW depth bits:
  //
  //     red_val = lumConvTab[red];                  // 0..4095 at 12 bits, etc.
  //     for (i = depth-1; i >= 0; i--)              // masks 1<<(depth-1) .. 1<<0
  //       bitplane[i] = (red_val & (1 << i)) ? 1 : 0;
  //
  // which is only correct when the run-time depth equals the build-time macro,
  // because that is what makes the table's output range match the bits being read.
  // When the depth was trimmed here without rebuilding, every channel became a
  // sawtooth rather than a scale: each channel wrapped at a different point, so a
  // pixel's channels came out reordered - reported from the panel as brown
  // rendering pink and blue rendering green. tools/webp-host-harness/depth_sim
  // reproduces it (at depth 5, input 200 mapped to 82 while 139 mapped to 205).
  //
  // 6 rather than 8 because the cost is not the ~2 KB per bit the framebuffer
  // maths suggests: measured on this board, the panel-framebuffer stage is 16,160
  // bytes at 5 bits and 31,544 at 8, because the library also sizes its DMA
  // descriptor allocation from the depth. At 8 the extra ~15 KB pushed free heap
  // below the compositing threshold and frames stopped decoding; 6 is a native
  // table depth, so it is still monotonic and still correct.
  //
  // Changing this means changing PIXEL_COLOR_DEPTH_BITS with it, and vice versa.
  // The depth also sets the BCM bitplane timing (nsPerRow scales with it), and the
  // panel shows collapsed rows after a flash until it has been power-cycled
  // (HANDOFF.md).
  constexpr uint8_t kPanelColorDepthBits = 6;

  HUB75_I2S_CFG mxconfig(
      WIDTH, HEIGHT, 1, pins,
      (HUB75_I2S_CFG::shift_driver)drv,  // driver chip
      (HUB75_I2S_CFG::line_driver)line,  // row decoder
      dbfr != 0,                         // double-buffering
      spd ? HUB75_I2S_CFG::HZ_20M : HUB75_I2S_CFG::HZ_10M,  // clock speed
      (uint8_t)latch,                                        // latch blanking
      ph != 0,                                               // invert clock phase
      60,  // minimum refresh rate; the library default, passed only to reach
           // the colour depth argument
      kPanelColorDepthBits
  );

  _matrix = new MatrixPanel_I2S_DMA(mxconfig);

  if (!_matrix->begin()) {
    ESP_LOGE(TAG, "MatrixPanel_I2S_DMA begin() failed");
    delete _matrix;
    _matrix = NULL;
    return 1;
  }

  // Apply stored brightness immediately so reboots (especially at night with
  // brightness 0) don't flash the boot animation at full brightness.
  uint8_t brightness_pct = nvs_get_brightness();
  _matrix->setBrightness8(brightness_percent_to_8bit(brightness_pct));
  _brightness = brightness_pct;
  ESP_LOGI(TAG, "Restored brightness to %d%%", brightness_pct);

  return 0;
}

int display_panel_width(void) { return WIDTH; }

int display_panel_height(void) { return HEIGHT; }

void display_set_brightness(uint8_t brightness_pct) {
  if (brightness_pct > DISPLAY_MAX_BRIGHTNESS) {
    ESP_LOGW(TAG, "Ignoring invalid brightness %u (valid range %d-%d)",
             brightness_pct, DISPLAY_MIN_BRIGHTNESS, DISPLAY_MAX_BRIGHTNESS);
    return;
  }
  if (brightness_pct != _brightness) {
    uint8_t brightness_8bit = brightness_percent_to_8bit(brightness_pct);

    ESP_LOGI(TAG, "Setting brightness to %d%% (%d)", brightness_pct,
             brightness_8bit);
    _matrix->setBrightness8(brightness_8bit);
    _matrix->clearScreen();
    _brightness = brightness_pct;
    esp_err_t err = nvs_set_brightness(brightness_pct);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to store brightness: %s", esp_err_to_name(err));
    } else if ((err = nvs_persist_brightness()) != ESP_OK) {
      ESP_LOGW(TAG, "Failed to persist brightness: %s", esp_err_to_name(err));
    }
  }
}

void display_shutdown(void) {
  _matrix->clearScreen();
  _matrix->stopDMAoutput();
  delete _matrix;
  _matrix = NULL;
}

// For each output channel in R,G,B order, which source channel feeds it.
// Indexed by color_order_t; "gbr" means red is driven from the source's green,
// green from blue, blue from red. Applied to the data rather than the pins so
// a single table covers every board, and composes with any board-specific pin
// swap done in display_initialize().
static const uint8_t kChannelOrder[COLOR_ORDER_MAX][3] = {
    {0, 1, 2},  // rgb
    {0, 2, 1},  // rbg
    {1, 0, 2},  // grb
    {1, 2, 0},  // gbr
    {2, 0, 1},  // brg
    {2, 1, 0},  // bgr
};

// Remap a colour triple for panels whose RGB lines are permuted. display_draw_565()
// applies the same table as it unpacks; the helpers below receive a colour
// directly and permute once.
static inline void apply_color_order(uint8_t *r, uint8_t *g, uint8_t *b) {
  color_order_t order = nvs_get_color_order();
  if (order >= COLOR_ORDER_MAX || order == COLOR_ORDER_RGB) return;
  const uint8_t ch[3] = {*r, *g, *b};
  *r = ch[kChannelOrder[order][0]];
  *g = ch[kChannelOrder[order][1]];
  *b = ch[kChannelOrder[order][2]];
}

// Draw a 16-bit (RGB565) canvas to the panel.
//
// The canvas is 565 rather than 888 because it is the largest buffer the firmware
// holds for the life of the session and the panel is driven at 5 bits per channel,
// so the extra byte per pixel bought nothing visible. The channel permutation is
// applied here as the pixels are unpacked, mirroring the old display_draw()'s
// index permutation: kChannelOrder[order][n] names the source channel that feeds
// output channel n.
void display_draw_565(const uint16_t *pix, int width, int height) {
  if (_matrix == NULL) {
    return;
  }
  color_order_t order = nvs_get_color_order();
  if (order >= COLOR_ORDER_MAX) order = COLOR_ORDER_RGB;
  const uint8_t *map = kChannelOrder[order];

  int scale = 1;
#if CONFIG_BOARD_TRONBYT_S3_WIDE || CONFIG_BOARD_MATRIXPORTAL_S3_WIDE
  if (width == 64 && height == 32) {
    scale = 2;  // Scale up to 128x64
  }
#endif

  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      uint8_t ch[3];
      rgb_from_565(pix[i * width + j], &ch[0], &ch[1], &ch[2]);
      const uint8_t r = ch[map[0]];
      const uint8_t g = ch[map[1]];
      const uint8_t b = ch[map[2]];

      // Draw each pixel scaled up (2x2 pixels for each original pixel)
      for (int sy = 0; sy < scale; sy++) {
        for (int sx = 0; sx < scale; sx++) {
          _matrix->drawPixelRGB888(j * scale + sx, i * scale + sy, r, g, b);
        }
      }
    }
  }
  _matrix->flipDMABuffer();
}

void display_clear(void) {
  if (_matrix != NULL) {
    _matrix->clearScreen();
  }
}

void display_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  apply_color_order(&r, &g, &b);
  if (_matrix != NULL) {
    _matrix->drawPixelRGB888(x, y, r, g, b);
    _matrix->flipDMABuffer();
  }
}

void draw_error_indicator_pixel(void) { display_draw_pixel(0, 0, 100, 0, 0); }

void display_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g,
                       uint8_t b) {
  apply_color_order(&r, &g, &b);
  if (_matrix != NULL) {
    for (int iy = y; iy < y + h; iy++) {
      for (int ix = x; ix < x + w; ix++) {
        _matrix->drawPixelRGB888(ix, iy, r, g, b);
      }
    }
  }
}

void display_text(const char *text, int x, int y, uint8_t r, uint8_t g,
                  uint8_t b, int scale) {
  apply_color_order(&r, &g, &b);
  if (_matrix == NULL || text == NULL) {
    return;
  }

  int cursor_x = x;
  int cursor_y = y;

  // Iterate through each character in the string
  for (int i = 0; text[i] != '\0'; i++) {
    char c = text[i];

    // Check if character is in font range
    if (c < FONT5X7_FIRST_CHAR || c > FONT5X7_LAST_CHAR) {
      c = ' ';  // Replace unsupported characters with space
    }

    // Get font data for this character
    int char_index = c - FONT5X7_FIRST_CHAR;
    const uint8_t *char_data = font5x7[char_index];

    // Draw each column of the character
    for (int col = 0; col < FONT5X7_CHAR_WIDTH; col++) {
      uint8_t column_data = char_data[col];

      // Draw each row in the column
      for (int row = 0; row < FONT5X7_CHAR_HEIGHT; row++) {
        if (column_data & (1 << row)) {
          // Draw pixel(s) based on scale
          for (int sy = 0; sy < scale; sy++) {
            for (int sx = 0; sx < scale; sx++) {
              int px = cursor_x + (col * scale) + sx;
              int py = cursor_y + (row * scale) + sy;

              // Check bounds
              if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
                _matrix->drawPixelRGB888(px, py, r, g, b);
              }
            }
          }
        }
      }
    }

    // Move cursor to next character position (5 pixels + 1 pixel spacing)
    cursor_x += (FONT5X7_CHAR_WIDTH + 1) * scale;
  }

  // Note: Not flipping buffer here anymore - caller must call display_flip()
}

void display_flip(void) {
  if (_matrix != NULL) {
    _matrix->flipDMABuffer();
  }
}
