#include "panel_sweep.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sdkconfig.h>
#include <string.h>

#include "display.h"

static const char *TAG = "sweep";

#define NVS_NS "wifi_config"
#define NVS_IDX "panel_sweep"

// Secondary candidate for the board's push button. The reference for this
// board documents GPIO11, but the physical button has not been confirmed on
// this unit, so GPIO0 (the other common single-button pin, also the strapping
// pin used for download mode) is polled too and reported on /diag.
#define PROBE_GPIO0 0

// Candidate panel configurations. A field of -1 means "leave the board default
// alone" (erase the NVS override). drv: 0=SHIFTREG 1=FM6124 2=FM6126A
// 3=ICN2038S 4=MBI5124 5=DP3246. line (row decoder): 0=TYPE138 1=TYPE595
// 2=DIRECT 3=SM5266P. spd: 0=8MHz 1=20MHz.
typedef struct {
  const char *name;
  int drv, line, spd, lat, ph, dbfr;
  uint8_t r, g, b;  // boot indicator colour
} panel_cfg_t;

// A full-screen fill renders correctly even when the scan path is wrong, which
// is why the row-decoder (`line`) candidates come first: uniform fills look
// perfect under row permutation, so the corruption we see points at row
// addressing above everything else.
static const panel_cfg_t s_cfgs[] = {
    {"board defaults", -1, -1, -1, -1, -1, -1, 255, 255, 255}, /* white   */
    {"line type595", -1, 1, -1, -1, -1, -1, 255, 0, 0},        /* red     */
    {"line direct", -1, 2, -1, -1, -1, -1, 0, 255, 0},         /* green   */
    {"line sm5266p", -1, 3, -1, -1, -1, -1, 255, 0, 255},      /* magenta */
    {"fm6124 lat4", 1, -1, 1, 4, 1, 0, 255, 255, 0},           /* yellow  */
    {"fm6124 ph0", 1, -1, 1, 1, 0, 0, 0, 0, 255},              /* blue    */
    {"fm6124 8mhz", 1, -1, 0, 1, 1, 0, 0, 255, 255},           /* cyan    */
    {"fm6124 ph0 8mhz lat4", 1, -1, 0, 4, 0, 0, 255, 128, 0},  /* orange  */
};
#define N_CFGS ((int)(sizeof(s_cfgs) / sizeof(s_cfgs[0])))

static const char *const s_keys[] = {"panel_drv",  "panel_spd", "panel_lat",
                                     "panel_ph",   "panel_dbfr",
                                     "panel_line"};

int panel_sweep_index(void) {
  nvs_handle_t h;
  int32_t v = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return 0;
  }
  if (nvs_get_i32(h, NVS_IDX, &v) != ESP_OK) {
    v = 0;
  }
  nvs_close(h);
  if (v < 0 || v >= N_CFGS) {
    v = 0;
  }
  return (int)v;
}

static void sweep_apply(int idx) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "nvs open failed");
    return;
  }

  const panel_cfg_t *c = &s_cfgs[idx];
  const int vals[] = {c->drv, c->spd, c->lat, c->ph, c->dbfr, c->line};
  for (int i = 0; i < 6; i++) {
    if (vals[i] < 0) {
      nvs_erase_key(h, s_keys[i]);
    } else {
      nvs_set_i32(h, s_keys[i], vals[i]);
    }
  }
  nvs_set_i32(h, NVS_IDX, idx);
  nvs_commit(h);
  nvs_close(h);
}

void panel_sweep_indicator(void) {
  const int idx = panel_sweep_index();
  const panel_cfg_t *c = &s_cfgs[idx];
  ESP_LOGW(TAG,
           "panel config %d/%d '%s' drv=%d line=%d spd=%d lat=%d ph=%d dbfr=%d",
           idx, N_CFGS - 1, c->name, c->drv, c->line, c->spd, c->lat, c->ph,
           c->dbfr);

  /* Use the library's full-screen fill: a uniform fill renders correctly even
   * when the scan path is wrong, so it is the reliable "which config is live"
   * indicator. */
  display_fill_screen(c->r, c->g, c->b);
  display_flip();
}

int panel_sweep_button_level(void) {
#if CONFIG_BUTTON_PIN >= 0
  return gpio_get_level(CONFIG_BUTTON_PIN);
#else
  return -1;
#endif
}

int panel_sweep_gpio0_level(void) { return gpio_get_level(PROBE_GPIO0); }

#if CONFIG_BUTTON_PIN >= 0

static void button_task(void *arg) {
  (void)arg;

  // Let the boot-time button read (config-mode check) happen first, and do not
  // fire on a button that is already held when we start polling.
  vTaskDelay(pdMS_TO_TICKS(5000));

  int last_btn = gpio_get_level(CONFIG_BUTTON_PIN);
  int last_gpio0 = gpio_get_level(PROBE_GPIO0);
  // Treat either candidate pin as "the button": the physical button has not
  // been confirmed on this unit, so accept a press on whichever one goes low.
  bool was_pressed = (last_btn == 0) || (last_gpio0 == 0);
  ESP_LOGI(TAG, "button cycler polling GPIO%d (idle %d) and GPIO0 (idle %d)",
           CONFIG_BUTTON_PIN, last_btn, last_gpio0);

  for (;;) {
    int btn = gpio_get_level(CONFIG_BUTTON_PIN);
    int g0 = gpio_get_level(PROBE_GPIO0);

    if (btn != last_btn) {
      ESP_LOGW(TAG, "GPIO%d %d -> %d", CONFIG_BUTTON_PIN, last_btn, btn);
      last_btn = btn;
    }
    if (g0 != last_gpio0) {
      ESP_LOGW(TAG, "GPIO0 %d -> %d", last_gpio0, g0);
      last_gpio0 = g0;
    }

    bool pressed = (btn == 0) || (g0 == 0);
    if (pressed && !was_pressed) {
      vTaskDelay(pdMS_TO_TICKS(60));  // debounce
      if (gpio_get_level(CONFIG_BUTTON_PIN) == 0 ||
          gpio_get_level(PROBE_GPIO0) == 0) {
        const int next = (panel_sweep_index() + 1) % N_CFGS;
        ESP_LOGW(TAG, "button press -> panel config %d/%d '%s'", next,
                 N_CFGS - 1, s_cfgs[next].name);
        sweep_apply(next);
        vTaskDelay(pdMS_TO_TICKS(600));  // let the log flush
        esp_restart();
      }
      was_pressed = true;
    } else if (!pressed) {
      was_pressed = false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

#endif /* CONFIG_BUTTON_PIN >= 0 */

void panel_sweep_start(void) {
#if CONFIG_BUTTON_PIN >= 0
  // Configure the second candidate pin as an input so it can be probed. GPIO0
  // is a strapping pin, only sampled at boot, so reading it at runtime is safe.
  gpio_config_t probe = {.pin_bit_mask = (1ULL << PROBE_GPIO0),
                         .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE,
                         .pull_down_en = GPIO_PULLDOWN_DISABLE,
                         .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&probe);

  xTaskCreate(button_task, "panel_sweep", 3072, NULL, 3, NULL);
#else
  ESP_LOGW(TAG, "no button pin configured; cycler disabled");
#endif
}
