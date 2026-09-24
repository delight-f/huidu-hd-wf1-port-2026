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

// Candidate panel configurations. A field of -1 means "leave the board default
// alone" (erase the NVS override). drv: 0=SHIFTREG 1=FM6124 2=FM6126A
// 3=ICN2038S 4=MBI5124 5=DP3246. spd: 0=8MHz 1=20MHz.
typedef struct {
  const char *name;
  int drv, spd, lat, ph, dbfr;
  uint8_t r, g, b;  // boot indicator colour
} panel_cfg_t;

static const panel_cfg_t s_cfgs[] = {
    {"board defaults", -1, -1, -1, -1, -1, 255, 255, 255}, /* white  */
    {"fm6124 lat4", 1, 1, 4, 1, 0, 255, 0, 0},             /* red    */
    {"fm6124 ph0", 1, 1, 1, 0, 0, 0, 255, 0},              /* green  */
    {"fm6124 8mhz", 1, 0, 1, 1, 0, 0, 0, 255},             /* blue   */
    {"fm6126a lat4", 2, 1, 4, 1, 0, 255, 255, 0},          /* yellow */
    {"icn2038s lat1", 3, 1, 1, 1, 0, 255, 0, 255},         /* magenta*/
    {"shiftreg lat4 20m", 0, 1, 4, 1, 0, 0, 255, 255},     /* cyan   */
    {"fm6124 ph0 8mhz", 1, 0, 1, 0, 0, 255, 128, 0},       /* orange */
};
#define N_CFGS ((int)(sizeof(s_cfgs) / sizeof(s_cfgs[0])))

static const char *const s_keys[] = {"panel_drv", "panel_spd", "panel_lat",
                                     "panel_ph", "panel_dbfr"};

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
  const int vals[] = {c->drv, c->spd, c->lat, c->ph, c->dbfr};
  for (int i = 0; i < 5; i++) {
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
  ESP_LOGW(TAG, "panel config %d/%d '%s' drv=%d spd=%d lat=%d ph=%d dbfr=%d",
           idx, N_CFGS - 1, c->name, c->drv, c->spd, c->lat, c->ph, c->dbfr);

  /* Solid fills render correctly even when the panel is otherwise
   * mis-configured, so use one as the "which config is live" indicator. */
  display_fill_rect(0, 0, 64, 32, c->r, c->g, c->b);
  display_flip();
}

#if CONFIG_BUTTON_PIN >= 0

static void button_task(void *arg) {
  (void)arg;

  // Let the boot-time button read (config-mode check) happen first, and do not
  // fire on a button that is already held when we start polling.
  vTaskDelay(pdMS_TO_TICKS(5000));
  bool was_pressed = (gpio_get_level(CONFIG_BUTTON_PIN) == 0);

  for (;;) {
    bool pressed = (gpio_get_level(CONFIG_BUTTON_PIN) == 0);
    if (pressed && !was_pressed) {
      vTaskDelay(pdMS_TO_TICKS(60));  // debounce
      if (gpio_get_level(CONFIG_BUTTON_PIN) == 0) {
        const int next = (panel_sweep_index() + 1) % N_CFGS;
        ESP_LOGW(TAG, "button press -> panel config %d/%d '%s'", next,
                 N_CFGS - 1, s_cfgs[next].name);
        sweep_apply(next);
        vTaskDelay(pdMS_TO_TICKS(600));  // let the log flush
        esp_restart();
      }
      was_pressed = true;
    } else {
      was_pressed = pressed;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

#endif /* CONFIG_BUTTON_PIN >= 0 */

void panel_sweep_start(void) {
#if CONFIG_BUTTON_PIN >= 0
  xTaskCreate(button_task, "panel_sweep", 3072, NULL, 3, NULL);
#else
  ESP_LOGW(TAG, "no button pin configured; cycler disabled");
#endif
}
