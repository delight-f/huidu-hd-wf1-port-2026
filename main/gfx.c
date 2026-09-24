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
#define GFX_TASK_STACK_SIZE 4092

struct gfx_state {
  TaskHandle_t task;
  SemaphoreHandle_t mutex;
  void *buf;
  size_t len;
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
  char a[16], b[16], c[16], d[16];
  snprintf(a, sizeof(a), "stk %u",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  snprintf(b, sizeof(b), "webp %u", (unsigned)len);
  snprintf(c, sizeof(c), "%s %s", tag, ok ? "OK" : "ERR");
  snprintf(d, sizeof(d), "heap %uk",
           (unsigned)(esp_get_free_heap_size() / 1024));
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
    ESP_LOGI(TAG, "calloc buff");
    _state->buf = calloc(1, ASSET_BOOT_WEBP_LEN);
    ESP_LOGI(TAG, "done calloc, copying");
    if (_state->buf == NULL) {
      ESP_LOGE("gfx", "Memory allocation failed!");
      return 1;
    }
    memcpy(_state->buf, ASSET_BOOT_WEBP, ASSET_BOOT_WEBP_LEN);
    ESP_LOGI(TAG, "done, copying");
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
  // strategy).
  if (_state->buf) {
    ESP_LOGW(TAG,
             "Dropping queued image (counter %d) - new image arrived before it "
             "was displayed",
             _state->counter);
    free(_state->buf);
    _state->buf = NULL;
  }

  // Take ownership of new buffer (no copy)
  _state->buf = webp;
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
      if (webp) {
        free(webp);
        webp = NULL;
      }
      break;
    }

    // If there's new data, take ownership of buffer
    if (counter != _state->counter) {
      ESP_LOGI(TAG, "Displaying image counter=%d", _state->counter);
      if (webp) free(webp);
      webp = _state->buf;
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
        free(webp);
        webp = NULL;
        len = 0;
      }
      // keep webp around to loop until the next image arrives
    } else {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// Reusable decode target.
//
// WebPAnimDecoder allocates two full canvas buffers internally
// (curr_frame and prev_frame_disposed, 2 x w*h*3 bytes) plus the VP8 decoder
// state. On this board that allocation fails outright ("new ERR" on the panel):
// the ESP32-S2 has only ~172 KB of data RAM in total and ~24 KB free here.
// Decoding each frame into a single buffer we own - allocated once, before the
// heap fragments - needs ~6 KB for a 64x32 frame and fits comfortably. This is
// the same approach libwebp's own anim decoder uses per frame
// (anim_decode.c: WebPDecode(fragment, size, config) with external memory),
// minus its canvas bookkeeping.
static uint8_t *s_dec_buf = NULL;
static size_t s_dec_buf_size = 0;

static uint8_t *decode_buffer(size_t need) {
  if (s_dec_buf != NULL && s_dec_buf_size >= need) {
    return s_dec_buf;
  }
  if (s_dec_buf != NULL) {
    free(s_dec_buf);
    s_dec_buf = NULL;
    s_dec_buf_size = 0;
  }
  s_dec_buf = (uint8_t *)malloc(need);
  if (s_dec_buf != NULL) {
    s_dec_buf_size = need;
  }
  return s_dec_buf;
}

static int draw_webp(const uint8_t *buf, size_t len, int32_t dwell_secs,
                     volatile int32_t *isAnimating) {
  int64_t dwell_us = (int64_t)(dwell_secs <= 0 ? 1 : dwell_secs) * 1000000;

  int w = 0, h = 0;
  if (!WebPGetInfo(buf, len, &w, &h) || w <= 0 || h <= 0) {
    ESP_LOGE(TAG, "WebPGetInfo failed");
    draw_error_indicator_pixel();
    diag_panel("info", len, false);
    return 1;
  }

  const size_t need = (size_t)w * (size_t)h * 3;
  uint8_t *out = decode_buffer(need);
  if (out == NULL) {
    ESP_LOGE(TAG, "decode buffer alloc failed (%u bytes)", (unsigned)need);
    draw_error_indicator_pixel();
    diag_panel("buf", len, false);
    return 1;
  }

  WebPData webpData;
  WebPDataInit(&webpData);
  webpData.bytes = buf;
  webpData.size = len;

  WebPDemuxer *demux = WebPDemux(&webpData);
  if (demux == NULL) {
    ESP_LOGE(TAG, "WebPDemux failed");
    draw_error_indicator_pixel();
    diag_panel("dmux", len, false);
    return 1;
  }

  const int64_t start_us = esp_timer_get_time();
  while (esp_timer_get_time() - start_us < dwell_us && *isAnimating != -1 &&
         !_state->paused) {
    WebPIterator iter;
    if (!WebPDemuxGetFrame(demux, 1, &iter)) {
      break;
    }

    do {
      WebPDecoderConfig cfg;
      if (!WebPInitDecoderConfig(&cfg)) {
        diag_panel("cfg", len, false);
        break;
      }
      cfg.output.colorspace = MODE_RGB;
      cfg.output.is_external_memory = 1;
      cfg.output.u.RGBA.rgba = out;
      cfg.output.u.RGBA.stride = w * 3;
      cfg.output.u.RGBA.size = need;
      cfg.options.no_fancy_upsampling = 1;

      if (WebPDecode(iter.fragment.bytes, iter.fragment.size, &cfg) ==
          VP8_STATUS_OK) {
        display_draw(out, w, h, 3, 0, 1, 2);
        diag_panel("dec", len, true);
      } else {
        diag_panel("dec1", len, false);
      }
      WebPFreeDecBuffer(&cfg.output);

      vTaskDelay(pdMS_TO_TICKS(iter.duration ? iter.duration : 100));
    } while (WebPDemuxNextFrame(&iter) && *isAnimating != -1 &&
             !_state->paused &&
             esp_timer_get_time() - start_us < dwell_us);

    WebPDemuxReleaseIterator(&iter);
  }

  WebPDemuxDelete(demux);

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
