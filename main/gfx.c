#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <http_parser.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include "assets.h"
#include "display.h"
#include "esp_timer.h"
#include "nvs_settings.h"
#include "version.h"

static const char *TAG = "gfx";

#if CONFIG_IDF_TARGET_ESP32S2
#define GFX_TASK_CORE 0  // S2 is single-core; only core 0 exists.
#else
#define GFX_TASK_CORE 1
#endif
#define GFX_TASK_PRIO 2
// The decode path runs libwebp inside this task, and libwebp's recursive
// Huffman table construction adds to the depth. 4092 left under 1.2 KB spare on
// the panel's stack watermark, and that reading was taken while decodes were
// still failing early, so it understates a successful decode. The extra KiB is
// cheap now that the WiFi code has been moved out of DIRAM.
#define GFX_TASK_STACK_SIZE 6144

struct gfx_state {
  TaskHandle_t task;
  SemaphoreHandle_t mutex;
  void *buf;
  size_t len;
  bool buf_is_static;  // buf points into flash rodata and must not be freed
  int32_t dwell_secs;
  int counter;
  int loaded_counter;  // Counter that tracks which image has been loaded by gfx
                       // task
  esp_websocket_client_handle_t
      ws_handle;  // Websocket handle for sending notifications
  volatile bool paused;
};

static struct gfx_state *_state = NULL;

static void gfx_loop(void *arg);
static int draw_webp(const uint8_t *buf, size_t len, int32_t dwell_secs,
                     volatile int32_t *isAnimating);
static void send_websocket_notification(int counter);

// The panel is 64 px wide and the font is a 5 px glyph plus a 1 px gap, so at
// most 10 characters fit on a line. The URL host / path / version strings can
// be longer than that (e.g. an IPv4 host is 14 chars), so clamp them.
#define GFX_MAX_TEXT_CHARS (64 / 6)
static void display_text_fitted(const char *text, int x, int y, uint8_t r,
                                uint8_t g, uint8_t b) {
  if (text == NULL) {
    return;
  }
  char buf[GFX_MAX_TEXT_CHARS + 1];
  size_t n = strlen(text);
  if (n > GFX_MAX_TEXT_CHARS) {
    n = GFX_MAX_TEXT_CHARS;
  }
  memcpy(buf, text, n);
  buf[n] = '\0';
  display_text(buf, x, y, r, g, b, 1);
}

// TEMP BENCH DIAGNOSTIC: render decode state on the panel. The text renderer
// works even when the WebP path does not, so this is how we read state on a
// board with no usable console. `tag` names the stage, `ok` the outcome.
static void diag_panel(const char *tag, size_t len, bool ok) {
  char a[32], b[32], c[32], d[32];
  snprintf(a, sizeof(a), "stk %u",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  snprintf(b, sizeof(b), "webp %u", (unsigned)len);
  snprintf(c, sizeof(c), "%s %s", tag, ok ? "OK" : "ERR");
  // Total free heap is not the number that decides whether a decode fits: the
  // decoder needs one large *contiguous* block, so report the largest free
  // block too. 'h' = free heap, 'b' = largest free block, both in KiB.
  snprintf(d, sizeof(d), "h%uk b%uk",
           (unsigned)(esp_get_free_heap_size() / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) /
                      1024));
  display_diag_show(a, b, c, d);
}

int gfx_initialize(const char *img_url) {
  // Only initialize once
  if (_state) {
    ESP_LOGE(TAG, "Already initialized");
    return 1;
  }

  int heapl = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

  ESP_LOGI(TAG, "largest heap %d", heapl);
  // ESP_LOGI(TAG, "calling calloc");
  // Initialize state
  ESP_LOGI(TAG, "Allocating buffer of size: %d", ASSET_BOOT_WEBP_LEN);

  _state = calloc(1, sizeof(struct gfx_state));
  _state->paused = false;
  if (!nvs_get_skip_boot_animation()) {
    _state->len = ASSET_BOOT_WEBP_LEN;
    // The asset already lives in flash rodata, which is memory-mapped, so hand
    // the decoder that pointer instead of copying it into internal RAM. On a
    // no-PSRAM S2 every KiB of heap counts, and the copied buffer would be dead
    // weight for the whole boot animation.
    _state->buf = (void *)ASSET_BOOT_WEBP;
    _state->buf_is_static = true;
  }

  _state->mutex = xSemaphoreCreateMutex();
  if (_state->mutex == NULL) {
    ESP_LOGE(TAG, "Could not create gfx mutex");
    return 1;
  }
  ESP_LOGI(TAG, "done with gfx init");

  // Initialize the display
  if (display_initialize()) {
    return 1;
  }

  if (nvs_get_skip_boot_animation()) {
    display_clear();
  }

  // Display version if not skipped
  if (!nvs_get_skip_display_version()) {
    // Display version and image_url for 1 second
    display_clear();
    char version_text[32];
    snprintf(version_text, sizeof(version_text), "v%s", FIRMWARE_VERSION);

    // Parse URL to extract host and last two path components
    if (img_url != NULL && strlen(img_url) > 0) {
      ESP_LOGI(TAG, "Full URL: %s", img_url);
      char host_only[64] = {0};
      char last_two_components[32] = {0};

      struct http_parser_url u;
      http_parser_url_init(&u);

      if (http_parser_parse_url(img_url, strlen(img_url), 0, &u) == 0) {
        if (u.field_set & (1 << UF_HOST)) {
          size_t host_len = u.field_data[UF_HOST].len;
          if (host_len >= sizeof(host_only)) host_len = sizeof(host_only) - 1;
          memcpy(host_only, img_url + u.field_data[UF_HOST].off, host_len);
          host_only[host_len] = '\0';
        }

        if (u.field_set & (1 << UF_PATH)) {
          const char *path = img_url + u.field_data[UF_PATH].off;
          size_t path_len = u.field_data[UF_PATH].len;
          const char *last_slash = NULL;
          const char *second_last_slash = NULL;

          for (size_t i = 0; i < path_len; i++) {
            if (path[i] == '/') {
              second_last_slash = last_slash;
              last_slash = path + i;
            }
          }

          if (second_last_slash != NULL) {
            size_t len = (path + path_len) - second_last_slash;
            if (len >= sizeof(last_two_components))
              len = sizeof(last_two_components) - 1;
            memcpy(last_two_components, second_last_slash, len);
            last_two_components[len] = '\0';
          } else {
            size_t len = path_len;
            if (len >= sizeof(last_two_components))
              len = sizeof(last_two_components) - 1;
            memcpy(last_two_components, path, len);
            last_two_components[len] = '\0';
          }
        }
      }

      // Display host at the top, left-aligned
      if (strlen(host_only) > 0) {
        ESP_LOGI(TAG, "Displaying host: '%s' at y=0", host_only);
        display_text_fitted(host_only, 0, 0, 255, 255, 255);
      }

      // Display last 11 chars of path components in the middle, left-aligned
      if (strlen(last_two_components) > 0) {
        const char *display_path = last_two_components;
        size_t path_len = strlen(last_two_components);

        // If longer than 11 chars, show only the last 11
        if (path_len > 11) {
          display_path = last_two_components + (path_len - 11);
        }

        ESP_LOGI(TAG, "Displaying path components: '%s' at y=10", display_path);
        display_text_fitted(display_path, 0, 10, 255, 255, 255);
      } else {
        ESP_LOGW(TAG, "No path components found to display");
      }
    }

    // Display 3 colored boxes RGB horizontally centered above version
    int box_x = (64 - 11) / 2;  // Center 11 pixels (3 boxes + 2 gaps)
    display_fill_rect(box_x, 20, 3, 3, 255, 0, 0);      // Red box
    display_fill_rect(box_x + 4, 20, 3, 3, 0, 255, 0);  // Green box
    display_fill_rect(box_x + 8, 20, 3, 3, 0, 0, 255);  // Blue box

    // Display version at the bottom, centered (clamped to the panel width)
    size_t version_chars = strlen(version_text);
    if (version_chars > GFX_MAX_TEXT_CHARS) {
      version_chars = GFX_MAX_TEXT_CHARS;
    }
    int text_width = (int)version_chars * 6;
    int x = (64 - text_width) / 2;
    if (x < 0) {
      x = 0;
    }
    display_text_fitted(version_text, x, 24, 255, 255, 255);

    // Flip the buffer once to show all three text lines at the same time
    display_flip();

    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // Launch the graphics loop in separate task
  BaseType_t ret =
      xTaskCreatePinnedToCore(gfx_loop,              // pvTaskCode
                              "gfx_loop",            // pcName
                              GFX_TASK_STACK_SIZE,   // usStackDepth
                              (void *)&isAnimating,  // pvParameters
                              GFX_TASK_PRIO,         // uxPriority
                              &_state->task,         // pxCreatedTask
                              GFX_TASK_CORE          // xCoreID
      );
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Could not create gfx task");
    return 1;
  }
  ESP_LOGI(TAG, "Gfx task created successfully");

  return 0;
}

void gfx_set_websocket_handle(esp_websocket_client_handle_t ws_handle) {
  if (_state) {
    _state->ws_handle = ws_handle;
    ESP_LOGI(TAG, "Websocket handle set for notifications");
  } else {
    ESP_LOGW(TAG, "Cannot set websocket handle - gfx not initialized");
  }
}

static void send_websocket_notification(int counter) {
  if (!_state || !_state->ws_handle) {
    // No websocket handle set, skip notification
    return;
  }

  if (!esp_websocket_client_is_connected(_state->ws_handle)) {
    ESP_LOGW(TAG, "Websocket not connected, skipping notification");
    return;
  }

  // Create JSON message: {"displaying": 42}
  char message[128];
  int len = snprintf(message, sizeof(message), "{\"displaying\":%d}", counter);

  if (len < 0 || len >= sizeof(message)) {
    ESP_LOGE(TAG, "Failed to format websocket notification message");
    return;
  }

  int sent = esp_websocket_client_send_text(_state->ws_handle, message, len,
                                            portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGE(TAG, "Failed to send websocket notification");
  } else {
    ESP_LOGI(TAG, "WS send: %s", message);
  }
}

int gfx_update(void *webp, size_t len, int32_t dwell_secs) {
  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "Could not take gfx mutex");
    return -1;  // Return negative on error
  }

  // If a new frame arrives before the previous one is consumed by the gfx task,
  // free the old buffer here to prevent a memory leak (frame-dropping
  // strategy). The boot asset is flash rodata, so it must not be freed.
  if (_state->buf) {
    ESP_LOGW(TAG,
             "Dropping queued image (counter %d) - new image arrived before it "
             "was displayed",
             _state->counter);
    if (!_state->buf_is_static) {
      free(_state->buf);
    }
    _state->buf = NULL;
  }

  // Take ownership of new buffer (no copy)
  _state->buf = webp;
  _state->buf_is_static = false;
  _state->len = len;
  _state->dwell_secs = dwell_secs;
  _state->counter++;
  int counter = _state->counter;
  ESP_LOGI(TAG, "Queued image counter=%d size=%zu dwell=%d", counter, len,
           dwell_secs);

  if (pdTRUE != xSemaphoreGive(_state->mutex)) {
    ESP_LOGE(TAG, "Could not give gfx mutex");
    return -1;  // Return negative on error
  }

  // Send "queued" notification immediately when image is queued
  if (_state->ws_handle &&
      esp_websocket_client_is_connected(_state->ws_handle)) {
    char message[64];
    int msg_len =
        snprintf(message, sizeof(message), "{\"queued\":%d}", counter);
    if (msg_len > 0 && msg_len < sizeof(message)) {
      esp_websocket_client_send_text(_state->ws_handle, message, msg_len,
                                     portMAX_DELAY);
      ESP_LOGI(TAG, "WS Send: %s", message);
    }
  }

  return counter;  // Return the counter value (>= 0) so caller can wait for it
                   // to be loaded
}

int gfx_get_loaded_counter(void) {
  if (!_state) return -1;

  if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
    ESP_LOGE(TAG, "Could not take gfx mutex");
    return -1;
  }

  int loaded = _state->loaded_counter;

  if (pdTRUE != xSemaphoreGive(_state->mutex)) {
    ESP_LOGE(TAG, "Could not give gfx mutex");
    return -1;
  }

  return loaded;
}

int gfx_display_asset(const char *asset_type) {
  const uint8_t *asset_data = NULL;
  size_t asset_len = 0;

  // Determine which asset to display
  if (strcmp(asset_type, "config") == 0) {
    asset_data = ASSET_CONFIG_WEBP;
    asset_len = ASSET_CONFIG_WEBP_LEN;
  } else if (strcmp(asset_type, "error_404") == 0) {
    asset_data = ASSET_404_WEBP;
    asset_len = ASSET_404_WEBP_LEN;
  } else if (strcmp(asset_type, "no_connect") == 0) {
    asset_data = ASSET_NOCONNECT_WEBP;
    asset_len = ASSET_NOCONNECT_WEBP_LEN;
  } else if (strcmp(asset_type, "oversize") == 0) {
    ESP_LOGI(TAG, "DISPLAYING OVERSIZE GRAPHIC");
    asset_data = ASSET_OVERSIZE_WEBP;
    asset_len = ASSET_OVERSIZE_WEBP_LEN;
  } else {
    ESP_LOGE(TAG, "Unknown asset type: %s", asset_type);
    return 1;
  }

  // Allocate heap memory and copy asset data
  uint8_t *asset_heap_copy = (uint8_t *)malloc(asset_len);
  if (asset_heap_copy == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for %s asset copy", asset_type);
    return 1;
  }

  memcpy(asset_heap_copy, asset_data, asset_len);

  // Interrupt current animation to display asset immediately
  isAnimating = -1;

  // Display the asset with no dwell time (static display)
  int result = gfx_update(asset_heap_copy, asset_len, 0);
  if (result < 0) {
    // Only free if gfx_update failed to take ownership (returned negative
    // error)
    ESP_LOGE(TAG, "Failed to update graphics with %s asset", asset_type);
    free(asset_heap_copy);
    return 1;
  }

  // gfx_update now owns the asset_heap_copy buffer (returns counter >= 0 on
  // success)
  return 0;
}

void gfx_display_text(const char *text, int x, int y, uint8_t r, uint8_t g,
                      uint8_t b, int scale) {
  display_text(text, x, y, r, g, b, scale);
}

void gfx_shutdown(void) { display_shutdown(); }

static void gfx_loop(void *args) {
  ESP_LOGI(TAG, "gfx_loop ENTERED");
  void *webp = NULL;
  bool webp_is_static = false;
  size_t len = 0;
  int32_t dwell_secs = 0;
  int counter = -1;
  ESP_LOGI(TAG, "Graphics loop running on core %d", xPortGetCoreID());

  for (;;) {
    if (_state->paused) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (pdTRUE != xSemaphoreTake(_state->mutex, portMAX_DELAY)) {
      ESP_LOGE(TAG, "Could not take gfx mutex");
      if (webp && !webp_is_static) {
        free(webp);
        webp = NULL;
      }
      break;
    }

    // If there's new data, take ownership of buffer
    if (counter != _state->counter) {
      ESP_LOGI(TAG, "Displaying image counter=%d", _state->counter);
      if (webp && !webp_is_static) free(webp);
      webp = _state->buf;
      webp_is_static = _state->buf_is_static;
      len = _state->len;
      dwell_secs = _state->dwell_secs;
      _state->buf = NULL;  // gfx_loop now owns the buffer
      counter = _state->counter;
      _state->loaded_counter = counter;  // Signal that we've loaded this image
      if (isAnimating == -1 && !_state->paused) isAnimating = 1;

      // Send websocket notification that we're now displaying this image
      send_websocket_notification(counter);
    }

    if (pdTRUE != xSemaphoreGive(_state->mutex)) {
      ESP_LOGE(TAG, "Could not give gfx mutex");
      continue;
    }

    static UBaseType_t last_stack_free = 0;
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free != last_stack_free) {
      ESP_LOGI(TAG, "Stack remaining: %u bytes", stack_free);
      last_stack_free = stack_free;
    }

    if (webp && len > 0) {
      if (draw_webp(webp, len, dwell_secs, &isAnimating)) {
        ESP_LOGE(TAG, "Could not draw webp");
        draw_error_indicator_pixel();
        // draw_webp() already left its stage ("new"/"info") on the panel; do not
        // overwrite it here or we lose which stage failed.
        vTaskDelay(pdMS_TO_TICKS(1 * 1000));
        isAnimating = 0;
        // Free the invalid buffer to prevent re-drawing it
        if (!webp_is_static) free(webp);
        webp = NULL;
        webp_is_static = false;
        len = 0;
      }
      // keep webp around to loop until the next image arrives
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// Decode targets.
//
// Two buffers, both allocated once and reused for every image:
//
//   s_canvas  w*h*3  the composited frame, which is what gets pushed to the
//                    panel (RGB, opaque).
//   s_frame   w*h*4  one animation frame decoded to RGBA.
//
// Why the canvas and not WebPAnimDecoder: that needs two full canvas buffers
// plus the VP8/VP8L working set, which does not fit the ESP32-S2's ~172 KB of
// data RAM.
//
// Two ways to fill the canvas, chosen per image:
//
//   still (single frame)  decode MODE_RGB straight into the canvas at the
//                         frame's offset. Cost: the RGB canvas only (6 KB at
//                         64x32). This is what a Tronbyt server sends most of
//                         the time, and it is exact.
//   animation             decode each frame to RGBA in a scratch and composite
//                         it, which is what libwebp's own animation decoder
//                         does - but it needs a second buffer, because libwebp
//                         decodes in place and therefore has to keep a
//                         disposed copy of the canvas to blend against.
//                         Decoding into a scratch instead of in place lets the
//                         canvas itself hold the previous frame, so one extra
//                         buffer is enough and no second canvas is ever held.
//                         The result is byte-identical to WebPAnimDecoder's.
//
// The scratch is only taken when the decoder can still afford its own ~26 KB
// working set afterwards (GFX_DECODE_HEADROOM); otherwise frames are drawn
// directly, which positions them correctly but does not blend partial frames
// over their predecessors.
#define GFX_DECODE_HEADROOM (30 * 1024)

static uint8_t *s_canvas = NULL;
static size_t s_canvas_size = 0;
static uint8_t *s_frame = NULL;
static size_t s_frame_size = 0;

static bool grow_buffer(uint8_t **buf, size_t *capacity, size_t need) {
  if (*capacity >= need) {
    return true;
  }
  uint8_t *grown = (uint8_t *)realloc(*buf, need);
  if (grown == NULL) {
    return false;
  }
  *buf = grown;
  *capacity = need;
  return true;
}

// Mirror of libwebp's IsKeyFrame (anim_decode.c). A key frame means the canvas
// is cleared before the frame is decoded rather than carried over.
static bool is_key_frame(const WebPIterator *cur, const WebPIterator *prev,
                         bool prev_was_key, int canvas_w, int canvas_h) {
  if (cur->frame_num == 1) {
    return true;
  }
  if ((!cur->has_alpha || cur->blend_method == WEBP_MUX_NO_BLEND) &&
      cur->width == canvas_w && cur->height == canvas_h) {
    return true;
  }
  return prev->dispose_method == WEBP_MUX_DISPOSE_BACKGROUND &&
         ((prev->width == canvas_w && prev->height == canvas_h) ||
          prev_was_key);
}

// Alpha-composite one decoded RGBA frame onto the RGB canvas at the frame's own
// offset. `blend` mirrors libwebp: only frames after the first, flagged
// WEBP_MUX_BLEND and not key frames, are blended; everything else replaces.
static void composite_frame(uint8_t *canvas, int canvas_w, int canvas_h,
                            const uint8_t *frame, const WebPIterator *it,
                            bool blend) {
  for (int y = 0; y < it->height; y++) {
    const int cy = it->y_offset + y;
    if (cy < 0 || cy >= canvas_h) continue;
    for (int x = 0; x < it->width; x++) {
      const int cx = it->x_offset + x;
      if (cx < 0 || cx >= canvas_w) continue;
      const uint8_t *src = frame + ((size_t)y * it->width + x) * 4;
      uint8_t *dst = canvas + ((size_t)cy * canvas_w + cx) * 3;
      const uint8_t a = src[3];
      if (!blend || a == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
      } else if (a != 0) {
        dst[0] = (uint8_t)((src[0] * a + dst[0] * (255 - a)) / 255);
        dst[1] = (uint8_t)((src[1] * a + dst[1] * (255 - a)) / 255);
        dst[2] = (uint8_t)((src[2] * a + dst[2] * (255 - a)) / 255);
      }
    }
  }
}

// A frame that disposes to background clears its rectangle from the canvas so
// the next frame blends against the cleared value.
static void dispose_frame(uint8_t *canvas, int canvas_w, int canvas_h,
                          const WebPIterator *it) {
  if (it->dispose_method != WEBP_MUX_DISPOSE_BACKGROUND) return;
  for (int y = 0; y < it->height; y++) {
    const int cy = it->y_offset + y;
    if (cy < 0 || cy >= canvas_h) continue;
    for (int x = 0; x < it->width; x++) {
      const int cx = it->x_offset + x;
      if (cx < 0 || cx >= canvas_w) continue;
      memset(canvas + ((size_t)cy * canvas_w + cx) * 3, 0, 3);
    }
  }
}

static int draw_webp(const uint8_t *buf, size_t len, int32_t dwell_secs,
                     volatile int32_t *isAnimating) {
  int64_t dwell_us = (int64_t)(dwell_secs <= 0 ? 1 : dwell_secs) * 1000000;

  int w = 0, h = 0;
  if (!WebPGetInfo(buf, len, &w, &h) || w <= 0 || h <= 0) {
    ESP_LOGE(TAG, "WebPGetInfo failed");
    diag_panel("info", len, false);
    return 1;
  }

  const size_t canvas_need = (size_t)w * (size_t)h * 3;
  const size_t frame_need = (size_t)w * (size_t)h * 4;
  if (!grow_buffer(&s_canvas, &s_canvas_size, canvas_need)) {
    ESP_LOGE(TAG, "canvas alloc failed (%u bytes)", (unsigned)canvas_need);
    diag_panel("buf", len, false);
    return 1;
  }
  uint8_t *const canvas = s_canvas;

  WebPData webpData;
  WebPDataInit(&webpData);
  webpData.bytes = buf;
  webpData.size = len;

  WebPDemuxer *demux = WebPDemux(&webpData);
  if (demux == NULL) {
    ESP_LOGE(TAG, "WebPDemux failed");
    diag_panel("dmux", len, false);
    return 1;
  }

  // Compositing needs an RGBA scratch, so only multi-frame images can want it,
  // and only when taking it still leaves the decoder room to work. A still
  // image therefore costs nothing but the canvas.
  bool composite = false;
  if (WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT) > 1) {
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (free_now >= frame_need + GFX_DECODE_HEADROOM &&
        grow_buffer(&s_frame, &s_frame_size, frame_need)) {
      composite = true;
    } else {
      ESP_LOGW(TAG,
               "only %u bytes free - drawing frames directly without "
               "compositing",
               (unsigned)free_now);
    }
  }

  const int64_t start_us = esp_timer_get_time();
  bool decoded_any = false;
  bool failed_any = false;
  WebPIterator prev;
  memset(&prev, 0, sizeof(prev));
  bool prev_was_key = false;
  WebPIterator iter;

  if (WebPDemuxGetFrame(demux, 1, &iter)) {
    do {
      const bool key = is_key_frame(&iter, &prev, prev_was_key, w, h);
      if (composite && key) {
        memset(canvas, 0, canvas_need);
      }

      WebPDecoderConfig cfg;
      bool ok = false;
      if (WebPInitDecoderConfig(&cfg)) {
        cfg.output.is_external_memory = 1;
        cfg.options.no_fancy_upsampling = 1;
        if (composite) {
          // Decode to RGBA in the scratch so the canvas still holds the
          // previous frame while we blend.
          cfg.output.colorspace = MODE_RGBA;
          cfg.output.u.RGBA.rgba = s_frame;
          cfg.output.u.RGBA.stride = iter.width * 4;
          cfg.output.u.RGBA.size = (size_t)iter.height * iter.width * 4;
        } else {
          // Decode straight into the canvas at the frame's own offset. This is
          // where the previous code went wrong: it decoded every frame at the
          // canvas origin with the canvas stride, so any frame smaller than the
          // canvas (which is most animation frames) landed in the wrong place.
          cfg.output.colorspace = MODE_RGB;
          cfg.output.u.RGBA.rgba = canvas +
                                   (size_t)iter.y_offset * (size_t)w * 3 +
                                   (size_t)iter.x_offset * 3;
          cfg.output.u.RGBA.stride = w * 3;
          cfg.output.u.RGBA.size = (size_t)iter.height * (size_t)w * 3;
        }
        ok = WebPDecode(iter.fragment.bytes, iter.fragment.size, &cfg) ==
             VP8_STATUS_OK;
      }

      if (ok) {
        if (composite) {
          composite_frame(canvas, w, h, s_frame, &iter,
                          iter.frame_num > 1 &&
                              iter.blend_method == WEBP_MUX_BLEND && !key);
          dispose_frame(canvas, w, h, &iter);
        }
        display_draw(canvas, w, h, 3, 0, 1, 2);
        decoded_any = true;
      } else {
        ESP_LOGE(TAG, "frame %d decode failed", iter.frame_num);
        failed_any = true;
      }

      prev = iter;
      prev_was_key = key;

      vTaskDelay(pdMS_TO_TICKS(iter.duration ? iter.duration : 100));
    } while (WebPDemuxNextFrame(&iter) && *isAnimating != -1 &&
             !_state->paused && esp_timer_get_time() - start_us < dwell_us);
  }

  WebPDemuxDelete(demux);

  if (!decoded_any && failed_any) {
    // Nothing rendered, so the panel is free to carry the diagnostic instead.
    // On success we deliberately do not touch it: the picture is the output.
    draw_error_indicator_pixel();
    diag_panel("dec", len, false);
    return 1;
  }

  if (*isAnimating != -1) {
    *isAnimating = 0;
  }
  return 0;
}

void gfx_stop(void) {
  if (_state) {
    isAnimating = -1;  // Signal current draw to stop
    _state->paused = true;
    ESP_LOGI(TAG, "Graphics loop paused");
  }
}

void gfx_start(void) {
  if (_state) {
    isAnimating = 0;
    _state->paused = false;
    ESP_LOGI(TAG, "Graphics loop resumed");
  }
}
