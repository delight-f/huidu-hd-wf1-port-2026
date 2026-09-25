#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_tls.h>
#include <esp_wifi.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "gfx.h"
#include "mem_compat.h"
#include "nvs_settings.h"
#include "sdkconfig.h"
#include "version.h"

static const char* TAG = "remote";

struct remote_state {
  void* buf;
  size_t len;
  size_t size;
  size_t max;
  size_t expected;  // Content-Length, 0 if the response did not announce one
  int16_t brightness;
  int32_t dwell_secs;
  char* ota_url;
  char* image_url;
  bool reboot_requested;
  bool oversize_detected;
  bool connected;  // did the TCP connection to the server ever come up?
  bool buf_lost;   // receive buffer was freed mid-transfer; body is unusable
  size_t logged_to;   // progress-log high-water mark, to keep the ring readable
  int64_t started_us; // when perform() began, for per-chunk timing
};

static bool parse_header_bool(const char* value) {
  if (value == NULL || value[0] == '\0') {
    return false;
  }
  return strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
         strcasecmp(value, "yes") == 0;
}

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static esp_err_t _httpCallback(esp_http_client_event_t* event) {
  esp_err_t err = ESP_OK;
  struct remote_state* state = (struct remote_state*)event->user_data;

  switch (event->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
      break;

    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      state->connected = true;
      break;

    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;

    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", event->header_key,
               event->header_value);

      // Check for the Content-Length header
      if (strcasecmp(event->header_key, "Content-Length") == 0) {
        size_t content_length = (size_t)atoi(event->header_value);
        if (content_length > state->max) {
          ESP_LOGE(TAG,
                   "Content-Length (%d bytes) exceeds allowed max (%d bytes)",
                   content_length, state->max);
          // Display the oversize graphic
          if (gfx_display_asset("oversize") != 0) {
            ESP_LOGE(TAG, "Failed to display oversize graphic");
          }
          state->oversize_detected = true;
          err = ESP_ERR_NO_MEM;
          esp_http_client_close(event->client);  // Abort the HTTP request
        } else {
          ESP_LOGI(TAG, "Content-Length Header : %d", content_length);
          state->expected = content_length;

          // Match the receive buffer to the payload. This buffer is handed to
          // gfx and stays live for the whole decode, so an oversized one is not
          // merely wasted heap: it is an allocation sitting in the middle of the
          // free space at the exact moment libwebp needs one contiguous block.
          // Tronbyt images here are 226-360 bytes, so the 4 KB default was
          // spending most of a decode's worth of fragmentation on nothing.
          //
          // Sized once, here, in both directions. Doing it in the header rather
          // than letting the body's overflow path grow it means the body needs
          // no reallocation at all - and a resize that happens before the
          // payload has been buffered can draw on the whole heap instead of one
          // already holding most of the image. Growing here also matters: a
          // shrunk buffer that is left to grow mid-body has to be reached
          // through the very fragmentation this is trying to avoid.
          if (content_length > 0 && content_length != state->size) {
            void* resized =
                heap_caps_realloc(state->buf, content_length, IMAGE_BUF_CAPS);
            if (resized != NULL) {
              state->buf = resized;
              state->size = content_length;
            } else {
              // Keep the buffer we have. The body's overflow path can still
              // grow it, and a too-small buffer is recoverable where a NULL one
              // is not.
              ESP_LOGW(TAG,
                       "Could not right-size response buffer to %d bytes "
                       "(free %u largest %u)",
                       content_length,
                       (unsigned)heap_caps_get_free_size(IMAGE_BUF_CAPS),
                       (unsigned)heap_caps_get_largest_free_block(
                           IMAGE_BUF_CAPS));
            }
          }
        }
      }

      // Check for the specific header key
      if (strcasecmp(event->header_key, "Tronbyt-Brightness") == 0) {
        int value = atoi(event->header_value);
        if (value >= DISPLAY_MIN_BRIGHTNESS && value <= DISPLAY_MAX_BRIGHTNESS) {
          state->brightness = (int16_t)value;
          ESP_LOGD(TAG, "Tronbyt-Brightness value: %d%%", value);
        } else {
          ESP_LOGW(TAG, "Ignoring invalid Tronbyt-Brightness: %s",
                   event->header_value);
        }
      } else if (strcasecmp(event->header_key, "Tronbyt-Dwell-Secs") == 0) {
        state->dwell_secs = (int)atoi(event->header_value);
        // ESP_LOGI(TAG, "Tronbyt-Dwell-Secs value: %i", dwell_secs_value);
      } else if (strcasecmp(event->header_key, "Tronbyt-OTA-URL") == 0) {
        if (state->ota_url != NULL) free(state->ota_url);
        state->ota_url = strdup(event->header_value);
        ESP_LOGI(TAG, "Found OTA URL: %s", state->ota_url);
      } else if (strcasecmp(event->header_key, "Tronbyt-Image-URL") == 0) {
        if (state->image_url != NULL) free(state->image_url);
        state->image_url = strdup(event->header_value);
        ESP_LOGI(TAG, "Found Image URL: %s", state->image_url);
      } else if (strcasecmp(event->header_key, "Tronbyt-Reboot") == 0) {
        state->reboot_requested = parse_header_bool(event->header_value);
        ESP_LOGI(TAG, "Tronbyt-Reboot value: %s", event->header_value);
      }
      break;

    case HTTP_EVENT_ON_DATA:

      if (event->user_data == NULL) {
        ESP_LOGW(TAG, "Discarding HTTP response due to missing state");
        break;
      }

      // If oversize was detected, don't process any data
      if (state->oversize_detected) {
        ESP_LOGD(TAG, "Discarding HTTP data due to oversize detection");
        break;
      }

      // If buffer was freed, don't process any data
      if (state->buf == NULL) {
        ESP_LOGD(TAG, "Discarding HTTP data due to freed buffer");
        break;
      }

      // if (event->data_len > max_data_size) {
      //   ESP_LOGW(TAG, "Discarding HTTP response due to missing state");
      //   break;
      // }

      // If needed, resize the buffer to fit the new data
      if (event->data_len + state->len > state->size) {
        const size_t need = state->len + event->data_len;

        if (need > state->max) {
          ESP_LOGE(TAG, "Response size exceeds allowed max (%d bytes)",
                   state->max);
          // Display the oversize graphic
          if (gfx_display_asset("oversize") != 0) {
            ESP_LOGE(TAG, "Failed to display oversize graphic");
          }
          free(state->buf);
          state->buf = NULL;
          state->oversize_detected = true;
          err = ESP_ERR_NO_MEM;
          esp_http_client_close(event->client);  // Abort the HTTP request
          break;
        }

        // Grow to what this response actually needs, rather than doubling.
        // Doubling overshoots what this heap can serve: a 15,658-byte image asks
        // for 32,000 bytes, while the largest free block runs around 17 KB - so
        // the fetch fails on a request twice the size of the one it needs, and
        // the image never arrives to be decoded. Rounded up to a KiB so that a
        // body arriving in many segments does not reallocate on every one.
        state->size = (need + 1023) & ~(size_t)1023;
        if (state->size > state->max) {
          state->size = state->max;
        }

        // And reallocate
        void* new =
            heap_caps_realloc(state->buf, state->size, IMAGE_BUF_CAPS);
        if (new == NULL) {
          ESP_LOGE(TAG,
                   "Resizing response buffer to %u bytes failed (free %u "
                   "largest %u)",
                   (unsigned)state->size,
                   (unsigned)heap_caps_get_free_size(IMAGE_BUF_CAPS),
                   (unsigned)heap_caps_get_largest_free_block(IMAGE_BUF_CAPS));
          free(state->buf);
          state->buf = NULL;
          state->buf_lost = true;
          err = ESP_ERR_NO_MEM;
          break;
        }
        state->buf = new;
      }

      // Copy over the new data
      memcpy(state->buf + state->len, event->data, event->data_len);
      state->len += event->data_len;

      // Progress through the body, sparsely (the log ring holds ~20 lines, so
      // this is one line per 4 KB). A transfer that trickles slowly and one
      // that stops dead look identical from outside - both surface as "short
      // payload" after tens of seconds - but they have opposite causes: a slow
      // reader (this task starved of CPU by the panel's DMA refresh) versus a
      // receive path that has run out of buffers and simply never opens the
      // window again. Chunk sizes and the gaps between them separate the two.
      if (state->len >= state->logged_to + 4096) {
        ESP_LOGI(TAG, "  body %u bytes at +%d ms (last chunk %u)", 
                 (unsigned)state->len,
                 (int)((esp_timer_get_time() - state->started_us) / 1000),
                 (unsigned)event->data_len);
        state->logged_to = state->len;
      }
      break;

    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;

    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");

      int mbedtlsErr = 0;
      esp_err_t err =
          esp_tls_get_and_clear_last_error(event->data, &mbedtlsErr, NULL);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP error - %s (mbedtls: 0x%x)", esp_err_to_name(err),
                 mbedtlsErr);
      }
      break;

    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      esp_http_client_set_redirection(event->client);
      break;
  }

  return err;
}

// Is this the whole file, according to the file itself?
//
// The server streams multi-KB animated WebPs, and when our read stalls it closes
// the connection early and we are left holding a prefix of the image. The
// client reports success, because from its side the connection ended cleanly, and
// the payload's header is intact - so every naive check passes and the decoder is
// handed a truncated file. WebPGetInfo succeeds on that header and WebPDemux then
// fails, which surfaces on the panel as a decoder error with the heap sitting
// idle: a transfer fault that reads as anything but one.
//
// The RIFF size field at offset 4 is the authority on how long the file is, so
// compare it against what we actually hold. This catches the truncation at the
// point it happens, where the cause is legible.
static size_t payload_declared_size(const void *buf, size_t len) {
  // A NULL buffer means "we have no payload", not "a payload of length len". The
  // receive buffer is freed on a failed resize while state.len still holds the
  // bytes accumulated so far, so this combination is reachable - and treating it
  // as readable data panics the board with a LoadProhibited at the memcmp below.
  // Found exactly that way, from the coredump.
  if (buf == NULL || len < 12) {
    return 0;
  }
  const uint8_t *p = (const uint8_t *)buf;
  if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WEBP", 4) != 0) {
    return 0;
  }
  return (size_t)p[4] | ((size_t)p[5] << 8) | ((size_t)p[6] << 16) |
         ((size_t)p[7] << 24);
}

int remote_get(const char* url, uint8_t** buf, size_t* len,
               uint8_t* brightness_pct, int32_t* dwell_secs,
               int* return_status_code, char** ota_url, char** image_url,
               bool* reboot_requested) {
  // State for processing the response
  struct remote_state state = {
      .buf =
          heap_caps_malloc(CONFIG_HTTP_BUFFER_SIZE_DEFAULT, IMAGE_BUF_CAPS),
      .len = 0,
      .size = CONFIG_HTTP_BUFFER_SIZE_DEFAULT,
      .max = CONFIG_HTTP_BUFFER_SIZE_MAX,
      .brightness = -1,
      .dwell_secs = -1,
      .ota_url = NULL,
      .image_url = NULL,
      .reboot_requested = false,
      .oversize_detected = false,
  };

  if (state.buf == NULL) {
    ESP_LOGE(TAG, "couldn't allocate HTTP receive buffer");
    return 1;
  }

  // Set up http client
  esp_http_client_config_t config = {
      .url = url,
      .event_handler = _httpCallback,
      .user_data = &state,
      .timeout_ms =
          30e3,  // 20s was not enough for the 33 KB animated WebPs the server
                 // serves once the link is marginal; see the TCP window note in
                 // sdkconfig.defaults.huidu-wf1
      .crt_bundle_attach = esp_crt_bundle_attach,
  };

  esp_http_client_handle_t http = esp_http_client_init(&config);
  if (http == NULL) {
    ESP_LOGE(TAG, "HTTP client initialization failed for URL: %s", url);
    free(state.buf);
    return 1;
  }

  if (esp_http_client_set_header(http, "X-Firmware-Version",
                                 FIRMWARE_VERSION) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set firmware version header");
  }

  char api_key[MAX_API_KEY_LEN + 1];
  if (nvs_get_api_key(api_key, sizeof(api_key)) == ESP_OK &&
      strlen(api_key) > 0) {
    char auth_header[64 + MAX_API_KEY_LEN];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    ESP_LOGD(TAG, "Using Authorization Bearer header");
    if (esp_http_client_set_header(http, "Authorization", auth_header) !=
        ESP_OK) {
      ESP_LOGE(TAG, "Failed to set Authorization header");
    }
  }

  // Do the request
  state.started_us = esp_timer_get_time();
  esp_err_t err = esp_http_client_perform(http);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "couldn't reach %s: %s", url, esp_err_to_name(err));
    // Link quality is worth measuring rather than inferring: a marginal RSSI
    // explains a large body timing out while small ones succeed, which is
    // otherwise indistinguishable from a server or firmware problem.
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      // `connected` is the figure that matters: it separates "the TCP
      // connection never came up" (routing, ARP, or the peer refusing us) from
      // "we connected and then got no reply" (a silent peer, or a path that
      // only carries in one direction). Those look identical in a timeout.
      ESP_LOGE(TAG,
               "  link: rssi %d dBm, channel %u, connected=%s, got %u bytes",
               ap.rssi, (unsigned)ap.primary, state.connected ? "yes" : "no",
               (unsigned)state.len);
    }
    if (state.buf != NULL) {
      free(state.buf);
    }
    if (state.image_url != NULL) {
      free(state.image_url);
      state.image_url = NULL;
    }
    esp_http_client_cleanup(http);
    return 1;
  }

  // A body shorter than the announced Content-Length is a transfer that was cut
  // off - the client's own timeout, a dropped connection, or the server closing
  // early. Handing that to the decoder produces a WebP whose RIFF header is
  // intact but whose chunks are missing, so WebPGetInfo succeeds and WebPDemux
  // fails; on the panel that reads as "dmux err" with kilobytes of free heap,
  // which looks like anything except a network problem. Fail it here instead,
  // where the cause is legible.
  if (state.expected > 0 && state.len != state.expected) {
    ESP_LOGE(TAG,
             "Truncated response: got %u of %u bytes - discarding",
             (unsigned)state.len, (unsigned)state.expected);
    if (state.buf != NULL) {
      free(state.buf);
    }
    if (state.image_url != NULL) {
      free(state.image_url);
    }
    if (state.ota_url != NULL) {
      free(state.ota_url);
    }
    esp_http_client_cleanup(http);
    return 1;
  }

  // The buffer was freed mid-transfer (a resize could not be satisfied), so what
  // we hold is a prefix of the body in no buffer at all. `perform` can still
  // report success, so this has to be checked explicitly rather than assumed -
  // and it must be checked before anything tries to read the payload.
  if (state.buf_lost) {
    ESP_LOGE(TAG,
             "Receive buffer lost mid-transfer after %u bytes - discarding",
             (unsigned)state.len);
    esp_http_client_cleanup(http);
    return 1;
  }

  // Refuse a payload the file itself says is incomplete, so a stalled transfer
  // becomes an explicit fetch failure the loop can retry, rather than a decoder
  // error that sends the next person hunting through gfx.c. Seen on the bench:
  // a 33 KB animated WebP arriving as 2,666 bytes with an intact header.
  const size_t declared = payload_declared_size(state.buf, state.len);
  if (declared > 0 && state.len < declared + 8) {
    ESP_LOGE(TAG,
             "Short payload: holding %u of %u bytes - the transfer was cut "
             "off, discarding",
             (unsigned)state.len, (unsigned)(declared + 8));
    if (state.buf != NULL) {
      free(state.buf);
    }
    if (state.image_url != NULL) {
      free(state.image_url);
    }
    if (state.ota_url != NULL) {
      free(state.ota_url);
    }
    esp_http_client_cleanup(http);
    return 1;
  }

  // Check if oversize was detected during the request
  if (state.oversize_detected) {
    ESP_LOGI(TAG, "Request aborted due to oversize content");
    if (state.buf != NULL) {
      free(state.buf);
    }
    if (state.ota_url != NULL) {
      free(state.ota_url);
    }
    if (state.image_url != NULL) {
      free(state.image_url);
    }
    esp_http_client_cleanup(http);
    *return_status_code = 413;  // HTTP 413 Payload Too Large
    return 1;  // Return error so main loop doesn't process the result
  }

  int status_code = esp_http_client_get_status_code(http);
  *return_status_code = status_code;
  if (status_code != 200) {
    ESP_LOGE(TAG, "Server returned HTTP status %d", status_code);
    if (state.buf != NULL) {
      free(state.buf);
    }
    if (state.ota_url != NULL) {
      free(state.ota_url);
    }
    if (state.image_url != NULL) {
      free(state.image_url);
    }
    esp_http_client_cleanup(http);
    return 1;
  }

  // Write back the results.
  *buf = state.buf;
  *len = state.len;
  if (state.brightness >= 0) {
    *brightness_pct = (uint8_t)state.brightness;
  }
  if (state.dwell_secs > -1 && state.dwell_secs < 300)
    *dwell_secs = state.dwell_secs;  // 5 minute max ?
  *ota_url = state.ota_url;
  *image_url = state.image_url;
  *reboot_requested = state.reboot_requested;

  esp_http_client_cleanup(http);
  // ESP_LOGI(TAG,"fetched new webp");
  return 0;
}
