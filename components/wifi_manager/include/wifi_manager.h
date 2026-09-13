/**
 * @file wifi_manager.h
 * @brief Wi-Fi connection manager with access-point fallback.
 *
 * Behaviour:
 *  - With stored station credentials the device tries to join the network.
 *    If it fails within CONFIG_RW_STA_CONNECT_TIMEOUT_S it raises its own
 *    access point (SSID "<prefix>-XXXXXX") and keeps retrying the station
 *    link every CONFIG_RW_STA_RETRY_INTERVAL_S while the AP stays up.
 *  - Without credentials the access point is started immediately.
 *  - Once the station link is established the AP is kept alive for
 *    CONFIG_RW_AP_LINGER_S so an operator connected to it can see the new
 *    IP address, then it is shut down.
 *
 * State transitions are published on the default event loop with the
 * WIFI_MGR_EVENT base so other modules (LED, captive DNS) can react.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(WIFI_MGR_EVENT);

typedef enum {
    WIFI_MGR_EVENT_AP_STARTED,       /**< Access point is up; data: esp_ip4_addr_t AP address. */
    WIFI_MGR_EVENT_AP_STOPPED,
    WIFI_MGR_EVENT_STA_CONNECTING,
    WIFI_MGR_EVENT_STA_CONNECTED,    /**< Got IP; data: esp_ip4_addr_t STA address. */
    WIFI_MGR_EVENT_STA_DISCONNECTED,
} wifi_mgr_event_id_t;

typedef enum {
    WIFI_MGR_STATE_IDLE = 0,
    WIFI_MGR_STATE_CONNECTING, /**< Trying to join the configured network.     */
    WIFI_MGR_STATE_CONNECTED,  /**< Station link up with an IP address.        */
    WIFI_MGR_STATE_AP_ONLY,    /**< No credentials: serving own network only.  */
    WIFI_MGR_STATE_AP_FALLBACK,/**< Credentials exist but the link is down.    */
} wifi_mgr_state_t;

#define WIFI_MGR_SSID_MAX 32
#define WIFI_MGR_PASS_MAX 64

typedef struct {
    wifi_mgr_state_t state;
    bool ap_active;
    bool sta_configured;
    char sta_ssid[WIFI_MGR_SSID_MAX + 1];
    char sta_ip[16];
    int8_t sta_rssi;
    char ap_ssid[WIFI_MGR_SSID_MAX + 1];
    char ap_ip[16];
    uint8_t ap_clients;
    uint8_t mac[6];
    char hostname[32];
} wifi_mgr_status_t;

typedef struct {
    char ssid[WIFI_MGR_SSID_MAX + 1];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t auth;
} wifi_mgr_scan_entry_t;

/** @brief Initialise Wi-Fi stack, load configuration from NVS and start the state machine. */
esp_err_t wifi_manager_init(void);

/**
 * @brief Store station credentials and (re)connect immediately.
 * @param password may be NULL or empty for an open network.
 */
esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *password);

/** @brief Forget station credentials and fall back to access-point mode. */
esp_err_t wifi_manager_clear_sta_credentials(void);

/**
 * @brief Change access-point SSID/password. Applied immediately if the AP is running.
 * @param ssid     NULL or empty restores the default "<prefix>-XXXXXX" name.
 * @param password NULL or empty makes the AP open; otherwise 8..63 characters (WPA2).
 */
esp_err_t wifi_manager_set_ap_credentials(const char *ssid, const char *password);

void wifi_manager_get_status(wifi_mgr_status_t *out);
const char *wifi_manager_state_str(wifi_mgr_state_t state);

/**
 * @brief Blocking scan for nearby access points (takes a few seconds).
 * @return ESP_ERR_INVALID_STATE if a connection attempt is in progress; try again later.
 */
esp_err_t wifi_manager_scan(wifi_mgr_scan_entry_t *entries, size_t max_entries, size_t *count);

#ifdef __cplusplus
}
#endif
