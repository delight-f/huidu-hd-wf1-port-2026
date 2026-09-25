#pragma once

#include <esp_err.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include "wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Access Point and start services
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ap_start(void);

/**
 * @brief Stop the Access Point services
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ap_stop(void);

/**
 * @brief Retire the setup SoftAP once the station link is up.
 *
 * Stops the captive-portal DNS server and the AP's DHCP server and switches the
 * radio to WIFI_MODE_STA, but deliberately leaves the web server running so
 * /diag and the config pages stay reachable on the station address. The SoftAP
 * is pure overhead while the station link is up - radio buffers, a DHCP server
 * and a 4 KB DNS task stack - and that overhead competes with the WebP decoder
 * for the one contiguous 12,544-byte block it needs. Logs free/largest before
 * and after so the effect is measured rather than assumed. Idempotent; a no-op
 * if the AP was never started.
 *
 * Callers must only retire the AP when a reboot cannot strand the device, i.e.
 * when the station link is up and the device is not in config mode.
 */
esp_err_t ap_retire_softap(void);

/**
 * @brief Initialize the AP network interface
 */
void ap_init_netif(void);

/**
 * @brief Configure the Access Point settings
 */
void ap_configure(void);

/**

 * @brief Start the AP auto-shutdown timer

 */

void ap_start_shutdown_timer(void);

/**
 * @brief Start the captive-portal DNS hijack.
 *
 * Not part of ap_start(): it costs ~5 KB of the internal heap the WebP decoder
 * needs one large contiguous run from, and it only earns that while the config
 * portal is in use. Callers start it where they know the portal will be needed
 * (no station link, or the boot button asked for config mode). Idempotent.
 */
void ap_start_dns(void);

/**
 * @brief Log the stack headroom of the httpd and DNS tasks.
 *
 * Both stacks come out of the same internal heap the WebP decoder needs a large
 * contiguous run from, so they are the first place to look when a decode does
 * not fit - but only a measured margin says whether either can be trimmed.
 * Served through /diag?heap=1.
 */
void ap_report_task_stacks(void);

#ifdef __cplusplus
}

#endif
