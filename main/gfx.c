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
#include "diag.h"
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
// the stack watermark, and that reading was taken while decodes were still
// failing early, so it understates a successful decode - hence the margin. This
// stack is 4.5 KB of the same heap the decoder itself wants, so it is trimmed
// against the watermark rather than left generous.
#define GFX_TASK_STACK_SIZE 4608

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

// Set by the main task before each fetch: "release the image you are holding".
// A plain flag written by one task and re-read continuously by the other is
// enough for a one-way request like this.
static volatile bool s_shed_wanted = false;
static volatile bool s_shed_done = true;

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

  // The panel's framebuffer is the biggest buffer this firmware owns, and it is
  // allocated here - after WiFi, lwIP and the HTTP server have already carved
  // up the heap. Trace either side of it: this is the stage most likely to be
  // splitting the largest free block that libwebp needs.
  diag_log_heap("before panel fb");
  // Initialize the display
  if (display_initialize()) {
    return 1;
  }
  diag_log_heap("after panel fb");

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

// Queue an image for the gfx task. `is_static` says the buffer points into flash
// rodata, which is memory-mapped and must not be freed - the same arrangement the
// boot animation uses, and the reason the built-in screens can be decoded without
// ever being resident in RAM.
static int gfx_queue(void *webp, size_t len, int32_t dwell_secs,
                     bool is_static) {
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
  _state->buf_is_static = is_static;
  _state->len = len;
  _state->dwell_secs = dwell_secs;
  _state->counter++;
  int counter = _state->counter;

  // A new image supersedes any pending shed request: there is nothing stale left
  // to release, and the loop should go back to holding what it is displaying.
  s_shed_wanted = false;
  s_shed_done = true;
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

int gfx_update(void *webp, size_t len, int32_t dwell_secs) {
  return gfx_queue(webp, len, dwell_secs, false);
}

void gfx_shed_retained(void) {
  if (!_state) {
    return;
  }

  // The flag goes up unconditionally: the gfx task also checks it between
  // animation frames, so it doubles as "cut this pass short". Both are needed -
  // one releases the bytes, the other bounds how long the release takes.
  s_shed_done = false;
  s_shed_wanted = true;

  // Bounded wait. In the normal path this returns on the next tick: the main task
  // only fetches once the animation's pass has finished, so the gfx task is
  // already back at the top of its loop with nothing to do. The timeout only
  // bites when the task is mid-frame or paused, and then the frame's own duration
  // is the floor - so give up and fetch anyway rather than stalling the display.
  for (int i = 0; i < 250 && !s_shed_done; i++) {
    vTaskDelay(pdMS_TO_TICKS(4));
  }
  if (!s_shed_done) {
    ESP_LOGW(TAG, "gfx task did not release the displayed image in time");
  }
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

  // Decode the asset from flash rodata, in place, with no heap copy.
  //
  // The previous code malloc'd `asset_len` and memcpy'd before queueing, which
  // meant every built-in screen needed internal RAM at least as large as the
  // asset. That is survivable for the small ones and impossible for the two that
  // matter most on a board this tight: the oversize screen is 36,724 bytes and the
  // 404 screen is 27,180. So the fallback whose entire job is to say "that image
  // is too big for me" was itself too big to allocate - it failed with
  // "Failed to allocate memory for oversize asset copy", the screen was never
  // drawn, and the panel sat on a stale decode error with a red dot while the log
  // said the ceiling had worked correctly.
  //
  // rodata is memory-mapped and WebPDecode only reads, so the copy buys nothing.
  // This is the same arrangement the boot animation already uses.
  isAnimating = -1;
  if (gfx_queue((void *)asset_data, asset_len, 0, true) < 0) {
    ESP_LOGE(TAG, "Failed to queue %s asset", asset_type);
    return 1;
  }
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

    // Hand the displayed image back before the main task fetches the next one.
    // See gfx_shed_retained() for why that is worth doing and why it is
    // invisible on the panel. `webp`/`len`/`webp_is_static` are this task's own
    // locals, so this needs no lock.
    if (s_shed_wanted && webp != NULL && !webp_is_static) {
      ESP_LOGI(TAG, "released the %u-byte displayed image for the next fetch",
               (unsigned)len);
      free(webp);
      webp = NULL;
      len = 0;
      webp_is_static = false;
    }
    // Set either way: the request is satisfied whether this task was holding an
    // image or not, and a static asset (a flash rodata screen) cannot be freed.
    s_shed_done = true;

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
//   s_canvas  w*h*2  the composited frame, which is what gets pushed to the
//                    panel (RGB565, opaque).
//   s_frame   w*h*4  one animation frame decoded to RGBA.
//
// Why the canvas and not WebPAnimDecoder: that needs two full canvas buffers
// plus the VP8/VP8L working set, which does not fit the ESP32-S2's ~172 KB of
// data RAM.
//
// Two ways to fill the canvas, chosen per image:
//
//   still (single frame)  decode MODE_RGB_565 straight into the canvas at the
//                         frame's offset. Cost: the canvas only (4 KB at 64x32).
//                         This is what a Tronbyt server sends most of the time,
//                         and it is exact.
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
// The scratch is only taken when the decoder can still afford its own working set
// afterwards (GFX_DECODE_MIN_RUN); otherwise frames are drawn directly, which
// positions them correctly but does not blend partial frames over their
// predecessors.
//
// libwebp needs that set as a single contiguous run: measured at 21,320 bytes for
// a 64x32 lossless frame, as an 11,816-byte block plus a ~9,504-byte one. So the
// gate tests the largest free run against this figure, with a little margin,
// rather than testing total free memory - totals only correlate with it, and
// three fixes in this port were aimed from totals and all three missed.
#define GFX_DECODE_MIN_RUN (22 * 1024)

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

// Take the canvas now, while the heap is still one piece.
//
// The canvas is allocated once and held for the life of the program, so *where*
// it lands matters far more than how big it is. Allocating it lazily - inside
// the first draw_webp(), after WiFi, the HTTP server, the WebSocket client and
// every task stack have already carved up the heap - lands it in the middle of
// whatever large run is left. libwebp then wants one free contiguous block for a
// 64x32 lossless frame and cannot get it. Reserving the canvas first makes the
// network stack work around it instead.
//
// The size comes from the panel rather than from the first image, so this can
// run before anything else, including display_initialize().
void gfx_reserve_decode_buffers(void) {
  const int w = display_panel_width();
  const int h = display_panel_height();
  if (w <= 0 || h <= 0) {
    return;
  }

  const size_t need = (size_t)w * (size_t)h * 2;
  diag_log_heap("canvas reserve: before");
  if (!grow_buffer(&s_canvas, &s_canvas_size, need)) {
    ESP_LOGW(TAG, "could not pre-allocate %u-byte canvas", (unsigned)need);
    return;
  }
  ESP_LOGI(TAG, "pre-allocated %u-byte canvas (%dx%d)", (unsigned)need, w, h);
  diag_log_heap("canvas reserve: after");
}

// A reserved runway for the decoder's own allocations.
//
// libwebp needs ~21.3 KB live at once for a 64x32 lossless frame - measured with
// the host harness as an 11,816-byte buffer plus a ~9,504-byte one - and both have
// to come out of *contiguous* free space. On this board the largest free run
// swings between roughly 19 KB and 30 KB as tasks and buffers come and go, so the
// same image decodes or fails depending only on heap luck.
//
// Holding a block of this size keeps that region from being broken up by
// everything that allocates while an image is in flight, and handing it back for
// the duration of the decode gives libwebp one clean run to allocate in.
//
// It is deliberately NOT taken before WiFi. An earlier attempt to reserve decoder
// space that early stopped the station associating at all - the join needs a large
// contiguous allocation of its own - which is why this waits until the link is up
// and the display is initialised, where the boot trace shows 32-38 KB free in one
// run.
//
// DISABLED, and the reason is worth keeping. Holding this much for the life of the
// session starves the receive path: the fetch grows its buffer to 6-8 KB in one
// piece while the arena and the composite scratch are held, and with the arena
// reserved the largest free run fell to ~4 KB - so the board could no longer
// receive a 4 KB image at all. That is worse than the intermittent decode failure
// it fixes: a fetch that cannot complete shows nothing, where a decode that cannot
// fit falls back to drawing frames unblended. The mechanism does work for the
// decode; what does not fit is a ~22 KB reservation, an 8 KB scratch and the
// receive buffer occupying this chip at the same time. Re-enable it only alongside
// enough freed memory that those three stop competing - see the budget notes in
// sdkconfig.defaults.huidu-wf1.
#define GFX_DECODE_ARENA_ENABLED 0
#define GFX_DECODE_ARENA (22 * 1024)

static uint8_t *s_arena = NULL;
static bool s_arena_wanted = false;

void gfx_reserve_decode_arena(void) {
#if !GFX_DECODE_ARENA_ENABLED
  return;
#endif
  if (s_arena != NULL || s_arena_wanted) {
    return;
  }
  diag_log_heap("arena: before");
  s_arena = malloc(GFX_DECODE_ARENA);
  if (s_arena == NULL) {
    // No worse than before: the decode just goes back to depending on the shape
    // of the heap. Do not set s_arena_wanted, so nothing juggles a block that was
    // never obtained.
    ESP_LOGW(TAG, "could not reserve %u-byte decode arena", (unsigned)GFX_DECODE_ARENA);
  } else {
    s_arena_wanted = true;
    ESP_LOGI(TAG, "reserved %u-byte decode arena", (unsigned)GFX_DECODE_ARENA);
  }
  diag_log_heap("arena: after");
}

// Hand the runway back for the duration of one decode, then take it again. Freed,
// it merges with whatever is adjacent, so the decoder sees the largest run this
// heap can offer rather than the broken-up shape it would otherwise get. If
// another task claims it in the gap the decode simply behaves as it did before the
// arena existed, so that is reported once rather than on every frame.
static void arena_give(void) {
  if (!s_arena_wanted || s_arena == NULL) {
    return;
  }
  free(s_arena);
  s_arena = NULL;
}

static void arena_take(void) {
  static bool warned = false;
  if (!s_arena_wanted || s_arena != NULL) {
    return;
  }
  s_arena = malloc(GFX_DECODE_ARENA);
  if (s_arena == NULL && !warned) {
    warned = true;
    ESP_LOGW(TAG, "decode arena lost to another task; decoding without it");
  }
}

// Release the composite scratch.
//
// This buffer is a cache, not a fixture. It is only useful to frame sequences
// that can afford it, but once allocated it was held for the life of the
// program - so a single animation early in a session permanently cost 8 KB
// (w*h*4), to be paid by every still frame and every large frame afterwards.
// That is the difference between a 31 KB payload decoding and not: with the
// scratch held, the largest free run sits under the ~12.5 KB libwebp needs,
// and the on-panel diagnostic reads exactly that ("dec ERR, h20k b11k").
//
// Releasing it before a decode that will not composite hands the space to the
// decoder; a later animation that can afford it simply allocates it again.
static void release_frame_scratch(void) {
  if (s_frame == NULL) {
    return;
  }
  ESP_LOGI(TAG, "released %u-byte composite scratch (free %u largest %u)",
           (unsigned)s_frame_size,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  free(s_frame);
  s_frame = NULL;
  s_frame_size = 0;
}

// Names libwebp's failure so the log says which kind of problem it was.
// "out of memory" means the heap could not hand over one contiguous block;
// "bitstream error" / "not enough data" mean the bytes were wrong - and those
// two have nothing to do with each other as fixes.
static const char *vp8_status_name(VP8StatusCode s) {
  switch (s) {
    case VP8_STATUS_OK:
      return "ok";
    case VP8_STATUS_OUT_OF_MEMORY:
      return "out of memory";
    case VP8_STATUS_INVALID_PARAM:
      return "invalid param";
    case VP8_STATUS_BITSTREAM_ERROR:
      return "bitstream error";
    case VP8_STATUS_UNSUPPORTED_FEATURE:
      return "unsupported feature";
    case VP8_STATUS_SUSPENDED:
      return "suspended";
    case VP8_STATUS_USER_ABORT:
      return "user abort";
    case VP8_STATUS_NOT_ENOUGH_DATA:
      return "not enough data";
    default:
      return "unknown";
  }
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

// The canvas is RGB565 rather than RGB888: it is the largest buffer held for the
// life of the session, so two bytes per pixel instead of three is 2 KB back, and
// 5/6/5 resolves finer than the panel itself can show (it is driven at 5 bits per
// channel). rgb_to_565()/rgb_from_565() live in display.h, which is the single
// place that byte layout is expressed - it is deliberately not a native uint16_t.

// Alpha-composite one decoded RGBA frame onto the canvas at the frame's own
// offset. `blend` mirrors libwebp: only frames after the first, flagged
// WEBP_MUX_BLEND and not key frames, are blended; everything else replaces.
static void composite_frame(uint16_t *canvas, int canvas_w, int canvas_h,
                            const uint8_t *frame, const WebPIterator *it,
                            bool blend) {
  for (int y = 0; y < it->height; y++) {
    const int cy = it->y_offset + y;
    if (cy < 0 || cy >= canvas_h) continue;
    for (int x = 0; x < it->width; x++) {
      const int cx = it->x_offset + x;
      if (cx < 0 || cx >= canvas_w) continue;
      const uint8_t *src = frame + ((size_t)y * it->width + x) * 4;
      uint16_t *dst = &canvas[(size_t)cy * canvas_w + cx];
      const uint8_t a = src[3];
      if (!blend || a == 255) {
        *dst = rgb_to_565(src[0], src[1], src[2]);
      } else if (a != 0) {
        // Unpack the destination to blend against it, then repack. The precision
        // round-tripped here is below what the panel can display.
        uint8_t dr, dg, db;
        rgb_from_565(*dst, &dr, &dg, &db);
        *dst = rgb_to_565((uint8_t)((src[0] * a + dr * (255 - a)) / 255),
                          (uint8_t)((src[1] * a + dg * (255 - a)) / 255),
                          (uint8_t)((src[2] * a + db * (255 - a)) / 255));
      }
    }
  }
}

// A frame that disposes to background clears its rectangle from the canvas so
// the next frame blends against the cleared value.
static void dispose_frame(uint16_t *canvas, int canvas_w, int canvas_h,
                          const WebPIterator *it) {
  if (it->dispose_method != WEBP_MUX_DISPOSE_BACKGROUND) return;
  for (int y = 0; y < it->height; y++) {
    const int cy = it->y_offset + y;
    if (cy < 0 || cy >= canvas_h) continue;
    for (int x = 0; x < it->width; x++) {
      const int cx = it->x_offset + x;
      if (cx < 0 || cx >= canvas_w) continue;
      canvas[(size_t)cy * canvas_w + cx] = 0;
    }
  }
}

static int draw_webp(const uint8_t *buf, size_t len, int32_t dwell_secs,
                     volatile int32_t *isAnimating) {
  int64_t dwell_us = (int64_t)(dwell_secs <= 0 ? 1 : dwell_secs) * 1000000;

  int w = 0, h = 0;
  if (!WebPGetInfo(buf, len, &w, &h) || w <= 0 || h <= 0) {
    ESP_LOGE(TAG, "WebPGetInfo failed");
    return 1;
  }

  const size_t canvas_need = (size_t)w * (size_t)h * 2;
  const size_t frame_need = (size_t)w * (size_t)h * 4;
  if (!grow_buffer(&s_canvas, &s_canvas_size, canvas_need)) {
    ESP_LOGE(TAG, "canvas alloc failed (%u bytes)", (unsigned)canvas_need);
    return 1;
  }
  uint16_t *const canvas = (uint16_t *)s_canvas;

  WebPData webpData;
  WebPDataInit(&webpData);
  webpData.bytes = buf;
  webpData.size = len;

  WebPDemuxer *demux = WebPDemux(&webpData);
  if (demux == NULL) {
    // WebPGetInfo has already succeeded by now, which means the first bytes
    // looked like a WebP and the declared dimensions were readable - so a demux
    // failure means the container is either short or misframed. Print the magic
    // and the RIFF-declared size against the length we actually hold: those two
    // numbers separate "truncated transfer" from "corrupt or wrongly framed
    // payload", which point at completely different fixes.
    if (len >= 12) {
      const uint8_t *p = (const uint8_t *)buf;
      const unsigned riff_size = (unsigned)p[4] | ((unsigned)p[5] << 8) |
                                 ((unsigned)p[6] << 16) | ((unsigned)p[7] << 24);
      ESP_LOGE(TAG, "WebPDemux failed: len=%u riff=%u magic=%.4s%.4s", 
               (unsigned)len, riff_size, (const char *)p, (const char *)p + 8);
    } else {
      ESP_LOGE(TAG, "WebPDemux failed: len=%u too short for a container",
               (unsigned)len);
    }
    return 1;
  }

  // Compositing needs an RGBA scratch, so only multi-frame images can want it,
  // and only when taking it still leaves the decoder room to work. A still
  // image therefore costs nothing but the canvas.
  //
  // The test is on the LARGEST FREE RUN, not on total free memory. Total free is a
  // proxy for the wrong quantity, and this port has now missed three fixes by
  // aiming from totals: what decides whether libwebp can decode is one contiguous
  // ~21,320-byte region. So: take the scratch first, then ask the heap whether the
  // decoder can still get its run, and hand the scratch straight back if it
  // cannot. Refusing to composite is not a neutral failure - an unblended partial
  // frame is exactly what a missing-pixels animation looks like - but engaging
  // when the decode then fails is worse, because nothing appears at all.
  bool composite = false;
  if (WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT) > 1) {
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    // A reserved arena already holds the decoder's runway back, so it is the
    // decoder's own need that matters there rather than decoder-plus-margin.
    const size_t need = s_arena_wanted ? GFX_DECODE_MIN_RUN
                                       : GFX_DECODE_MIN_RUN + frame_need;
    if (grow_buffer(&s_frame, &s_frame_size, frame_need)) {
      const size_t largest =
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
      if (largest >= need) {
        composite = true;
      } else {
        ESP_LOGW(TAG,
                 "largest free run %u < %u needed (free %u) - drawing frames "
                 "directly without compositing",
                 (unsigned)largest, (unsigned)need, (unsigned)free_now);
      }
    } else {
      ESP_LOGW(TAG,
               "no room for the %u-byte composite scratch (free %u) - drawing "
               "frames directly without compositing",
               (unsigned)frame_need, (unsigned)free_now);
    }
  }

  // Anything that will not composite gives the scratch back before decoding, so
  // the decoder gets it rather than it idling as a cache. This includes the
  // still-image path, which never wants the scratch at all.
  if (!composite) {
    release_frame_scratch();
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
      VP8StatusCode status = VP8_STATUS_OK;
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
          // MODE_RGB_565 writes two bytes per pixel through u.RGBA - libwebp has
          // no separate 565 union member - which is what makes the 16-bit canvas
          // worth it: the decoder packs into it directly, with no conversion pass.
          cfg.output.colorspace = MODE_RGB_565;
          cfg.output.u.RGBA.rgba =
              (uint8_t *)canvas +
              ((size_t)iter.y_offset * (size_t)w + (size_t)iter.x_offset) * 2;
          cfg.output.u.RGBA.stride = w * 2;
          cfg.output.u.RGBA.size = (size_t)iter.height * (size_t)w * 2;
        }
        // Hand the reserved runway back for the duration of the decode. libwebp
        // allocates its working buffers inside this call and nothing else
        // allocates here, so it gets the run the arena was holding.
        arena_give();
        status = WebPDecode(iter.fragment.bytes, iter.fragment.size, &cfg);
        arena_take();
        ok = status == VP8_STATUS_OK;
      }

      if (ok) {
        if (composite) {
          composite_frame(canvas, w, h, s_frame, &iter,
                          iter.frame_num > 1 &&
                              iter.blend_method == WEBP_MUX_BLEND && !key);
          dispose_frame(canvas, w, h, &iter);
        }
        display_draw_565(canvas, w, h);
        decoded_any = true;
      } else {
        // The status separates "the heap could not give us a contiguous block"
        // from "the bytes were not a valid frame" - two failures that are
        // indistinguishable from a bare "decode failed", and which point at
        // completely different fixes. The heap figures are read here rather than
        // at the top because this is the only moment that matters.
        ESP_LOGE(TAG, "frame %d decode failed (%s), free %u largest %u", 
                 iter.frame_num, vp8_status_name(status),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        failed_any = true;
        // Nothing has decoded, so this frame will not decode on the next pass
        // either - and an animation can carry hundreds of frames. Without this
        // the loop grinds through every one of them on every pass of the dwell,
        // holding the payload for the whole ride, which is how a single
        // too-large asset turns a board into one that looks hung instead of one
        // that reports an error and moves on. Observed on the bench at frame 141
        // and still counting.
        if (!decoded_any) {
          break;
        }
      }

      prev = iter;
      prev_was_key = key;

      vTaskDelay(pdMS_TO_TICKS(iter.duration ? iter.duration : 100));
    } while (WebPDemuxNextFrame(&iter) && *isAnimating != -1 &&
             !_state->paused && !s_shed_wanted &&
             esp_timer_get_time() - start_us < dwell_us);
  }

  WebPDemuxDelete(demux);

  if (!decoded_any && failed_any) {
    draw_error_indicator_pixel();
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
