/**
 * @file web_server.h
 * @brief HTTP server: single-page UI plus JSON REST API.
 *
 * Endpoints (all JSON unless noted):
 *   GET    /                    UI (embedded index.html)
 *   GET    /api/status          firmware, uptime, Wi-Fi and reader state
 *   GET    /api/key             reader snapshot maintained by the poll task
 *   POST   /api/key/read        read the attached key right now
 *   POST   /api/key/write       {id, variant, fix_crc}  program an RW1990 blank and verify
 *   POST   /api/key/verify      {id, reads}             repeated read-back check
 *   GET    /api/wifi/scan       list nearby networks
 *   POST   /api/wifi/sta        {ssid, password}        save credentials and connect
 *   DELETE /api/wifi/sta        forget credentials, stay in AP mode
 *   POST   /api/wifi/ap         {ssid, password}        access-point settings
 *   POST   /api/system/reboot
 *   *      anything else        302 -> /  (captive-portal probes)
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(void);
void      web_server_stop(void);

#ifdef __cplusplus
}
#endif
