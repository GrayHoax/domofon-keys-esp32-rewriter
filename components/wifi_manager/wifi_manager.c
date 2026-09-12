#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_mgr";

ESP_EVENT_DEFINE_BASE(WIFI_MGR_EVENT);

#define NVS_NAMESPACE    "wifi"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"
#define NVS_KEY_AP_SSID  "ap_ssid"
#define NVS_KEY_AP_PASS  "ap_pass"

#define AP_CHANNEL         1
#define AP_MAX_CONNECTIONS 4
#define US_PER_S           1000000LL

static struct {
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    SemaphoreHandle_t lock;
    wifi_mgr_status_t status;
    char sta_pass[WIFI_MGR_PASS_MAX + 1];
    char ap_pass[WIFI_MGR_PASS_MAX + 1];
    bool ap_ssid_custom;
    bool sta_disabled;          /* Credentials cleared: ignore STA events.      */
    int64_t connect_deadline_us;/* Keep reconnecting until this timestamp.      */
    esp_timer_handle_t retry_timer;
    esp_timer_handle_t ap_linger_timer;
} s;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static inline void lock(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static inline void unlock(void)
{
    xSemaphoreGive(s.lock);
}

static void set_state(wifi_mgr_state_t state)
{
    lock();
    if (s.status.state != state) {
        ESP_LOGI(TAG, "state: %s -> %s", wifi_manager_state_str(s.status.state), wifi_manager_state_str(state));
        s.status.state = state;
    }
    unlock();
}

static void post_event(wifi_mgr_event_id_t id, const void *data, size_t len)
{
    esp_err_t err = esp_event_post(WIFI_MGR_EVENT, id, data, len, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event %d not posted: %s", id, esp_err_to_name(err));
    }
}

const char *wifi_manager_state_str(wifi_mgr_state_t state)
{
    switch (state) {
    case WIFI_MGR_STATE_IDLE:
        return "idle";
    case WIFI_MGR_STATE_CONNECTING:
        return "connecting";
    case WIFI_MGR_STATE_CONNECTED:
        return "connected";
    case WIFI_MGR_STATE_AP_ONLY:
        return "ap_only";
    case WIFI_MGR_STATE_AP_FALLBACK:
        return "ap_fallback";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* Persistent configuration                                                   */
/* ------------------------------------------------------------------------- */

static esp_err_t nvs_get_string(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static void default_ap_ssid(char *out, size_t cap)
{
    const uint8_t *m = s.status.mac;
    snprintf(out, cap, "%s-%02X%02X%02X", CONFIG_RW_AP_SSID_PREFIX, m[3], m[4], m[5]);
}

static esp_err_t load_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot: nothing stored yet. */
        s.status.sta_ssid[0] = '\0';
        s.sta_pass[0] = '\0';
        s.ap_ssid_custom = false;
        strlcpy(s.ap_pass, CONFIG_RW_AP_PASSWORD, sizeof(s.ap_pass));
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");
        ESP_RETURN_ON_ERROR(nvs_get_string(h, NVS_KEY_STA_SSID, s.status.sta_ssid, sizeof(s.status.sta_ssid)), TAG,
                            "read sta_ssid");
        ESP_RETURN_ON_ERROR(nvs_get_string(h, NVS_KEY_STA_PASS, s.sta_pass, sizeof(s.sta_pass)), TAG,
                            "read sta_pass");
        ESP_RETURN_ON_ERROR(nvs_get_string(h, NVS_KEY_AP_SSID, s.status.ap_ssid, sizeof(s.status.ap_ssid)), TAG,
                            "read ap_ssid");
        s.ap_ssid_custom = (s.status.ap_ssid[0] != '\0');

        size_t len = sizeof(s.ap_pass);
        err = nvs_get_str(h, NVS_KEY_AP_PASS, s.ap_pass, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            strlcpy(s.ap_pass, CONFIG_RW_AP_PASSWORD, sizeof(s.ap_pass));
        } else if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
        nvs_close(h);
    }

    if (!s.ap_ssid_custom) {
        default_ap_ssid(s.status.ap_ssid, sizeof(s.status.ap_ssid));
    }
    s.status.sta_configured = (s.status.sta_ssid[0] != '\0');
    return ESP_OK;
}

static esp_err_t save_pair(const char *ssid_key, const char *ssid, const char *pass_key, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open failed");

    esp_err_t err = nvs_set_str(h, ssid_key, ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(h, pass_key, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Interface configuration                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t apply_ap_config(void)
{
    wifi_config_t cfg = {0};
    lock();
    strlcpy((char *)cfg.ap.ssid, s.status.ap_ssid, sizeof(cfg.ap.ssid));
    strlcpy((char *)cfg.ap.password, s.ap_pass, sizeof(cfg.ap.password));
    unlock();

    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONNECTIONS;
    cfg.ap.authmode = (cfg.ap.password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.ap.pmf_cfg.required = false;

    return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

static esp_err_t apply_sta_config(void)
{
    wifi_config_t cfg = {0};
    lock();
    strlcpy((char *)cfg.sta.ssid, s.status.sta_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s.sta_pass, sizeof(cfg.sta.password));
    unlock();

    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = (cfg.sta.password[0] != '\0') ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static esp_err_t ap_start(void)
{
    wifi_mode_t mode;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "get_mode");
    if (mode == WIFI_MODE_APSTA) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(apply_ap_config(), TAG, "ap config");
    ESP_LOGI(TAG, "starting access point \"%s\"", s.status.ap_ssid);
    return esp_wifi_set_mode(WIFI_MODE_APSTA);
}

static esp_err_t ap_stop(void)
{
    wifi_mode_t mode;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "get_mode");
    if (mode == WIFI_MODE_STA) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "stopping access point");
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

/** Begin a fresh connection session with a full timeout budget. */
static void sta_connect_begin(void)
{
    if (!s.status.sta_configured) {
        return;
    }
    s.sta_disabled = false;
    s.connect_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_RW_STA_CONNECT_TIMEOUT_S * US_PER_S;
    esp_timer_stop(s.retry_timer);

    esp_err_t err = apply_sta_config();
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect start failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", s.status.sta_ssid);
    set_state(WIFI_MGR_STATE_CONNECTING);
    post_event(WIFI_MGR_EVENT_STA_CONNECTING, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* Timers                                                                     */
/* ------------------------------------------------------------------------- */

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s.status.sta_configured && !s.sta_disabled && s.status.state != WIFI_MGR_STATE_CONNECTED) {
        ESP_LOGI(TAG, "periodic reconnect attempt");
        sta_connect_begin();
    }
}

static void ap_linger_timer_cb(void *arg)
{
    (void)arg;
    if (s.status.state == WIFI_MGR_STATE_CONNECTED) {
        ap_stop();
    }
}

/* ------------------------------------------------------------------------- */
/* Event handling                                                             */
/* ------------------------------------------------------------------------- */

static void on_sta_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    bool was_connected;
    lock();
    was_connected = (s.status.state == WIFI_MGR_STATE_CONNECTED);
    s.status.sta_ip[0] = '\0';
    s.status.sta_rssi = 0;
    unlock();

    if (was_connected) {
        post_event(WIFI_MGR_EVENT_STA_DISCONNECTED, NULL, 0);
        /* Link dropped: open a new reconnect window before falling back. */
        s.connect_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_RW_STA_CONNECT_TIMEOUT_S * US_PER_S;
    }

    if (s.sta_disabled || !s.status.sta_configured) {
        return;
    }

    ESP_LOGW(TAG, "disconnected from \"%s\" (reason %d)", s.status.sta_ssid, ev ? ev->reason : -1);

    if (esp_timer_get_time() < s.connect_deadline_us) {
        set_state(WIFI_MGR_STATE_CONNECTING);
        esp_wifi_connect();
        return;
    }

    /* Timeout exhausted: raise the AP and retry in the background. */
    ESP_LOGW(TAG, "connection timeout, falling back to access point");
    ap_start();
    set_state(WIFI_MGR_STATE_AP_FALLBACK);
    esp_timer_stop(s.retry_timer);
    esp_timer_start_once(s.retry_timer, (uint64_t)CONFIG_RW_STA_RETRY_INTERVAL_S * US_PER_S);
}

static void on_got_ip(const ip_event_got_ip_t *ev)
{
    lock();
    snprintf(s.status.sta_ip, sizeof(s.status.sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    unlock();

    ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
    esp_timer_stop(s.retry_timer);
    set_state(WIFI_MGR_STATE_CONNECTED);
    post_event(WIFI_MGR_EVENT_STA_CONNECTED, &ev->ip_info.ip, sizeof(ev->ip_info.ip));

    if (s.status.ap_active) {
        ESP_LOGI(TAG, "access point will be closed in %d s", CONFIG_RW_AP_LINGER_S);
        esp_timer_stop(s.ap_linger_timer);
        esp_timer_start_once(s.ap_linger_timer, (uint64_t)CONFIG_RW_AP_LINGER_S * US_PER_S);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s.status.sta_configured) {
                sta_connect_begin();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            on_sta_disconnected((const wifi_event_sta_disconnected_t *)data);
            break;

        case WIFI_EVENT_AP_START: {
            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(s.ap_netif, &ip);
            lock();
            s.status.ap_active = true;
            s.status.ap_clients = 0;
            snprintf(s.status.ap_ip, sizeof(s.status.ap_ip), IPSTR, IP2STR(&ip.ip));
            unlock();
            ESP_LOGI(TAG, "access point \"%s\" up, IP " IPSTR, s.status.ap_ssid, IP2STR(&ip.ip));
            post_event(WIFI_MGR_EVENT_AP_STARTED, &ip.ip, sizeof(ip.ip));
            break;
        }

        case WIFI_EVENT_AP_STOP:
            lock();
            s.status.ap_active = false;
            s.status.ap_clients = 0;
            unlock();
            post_event(WIFI_MGR_EVENT_AP_STOPPED, NULL, 0);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            lock();
            s.status.ap_clients++;
            unlock();
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            lock();
            if (s.status.ap_clients > 0) {
                s.status.ap_clients--;
            }
            unlock();
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip((const ip_event_got_ip_t *)data);
    }
}

/* ------------------------------------------------------------------------- */
/* mDNS                                                                       */
/* ------------------------------------------------------------------------- */

static void mdns_setup(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_RW_MDNS_HOSTNAME);
    mdns_instance_name_set("RW1990 key programmer");
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_RW_HTTP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", CONFIG_RW_MDNS_HOSTNAME);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t wifi_manager_init(void)
{
    ESP_RETURN_ON_FALSE(s.lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    s.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");

    s.sta_netif = esp_netif_create_default_wifi_sta();
    s.ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s.sta_netif && s.ap_netif, ESP_FAIL, TAG, "netif create failed");

    strlcpy(s.status.hostname, CONFIG_RW_MDNS_HOSTNAME, sizeof(s.status.hostname));
    esp_netif_set_hostname(s.sta_netif, s.status.hostname);
    esp_netif_set_hostname(s.ap_netif, s.status.hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");

    ESP_RETURN_ON_ERROR(esp_read_mac(s.status.mac, ESP_MAC_WIFI_STA), TAG, "read mac");
    ESP_RETURN_ON_ERROR(load_config(), TAG, "load config");

    const esp_timer_create_args_t retry_args = {.callback = retry_timer_cb, .name = "wifi_retry"};
    const esp_timer_create_args_t linger_args = {.callback = ap_linger_timer_cb, .name = "ap_linger"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s.retry_timer), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&linger_args, &s.ap_linger_timer), TAG, "linger timer");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "ip events");

    /* Interfaces always include STA so scanning works even in AP mode. */
    if (s.status.sta_configured) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
        s.status.state = WIFI_MGR_STATE_IDLE;
    } else {
        ESP_RETURN_ON_ERROR(apply_ap_config(), TAG, "ap config");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode");
        s.status.state = WIFI_MGR_STATE_AP_ONLY;
        ESP_LOGI(TAG, "no station credentials, starting in AP mode");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    mdns_setup();
    return ESP_OK;
}

esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "empty ssid");
    ESP_RETURN_ON_FALSE(strlen(ssid) <= WIFI_MGR_SSID_MAX, ESP_ERR_INVALID_SIZE, TAG, "ssid too long");
    if (password == NULL) {
        password = "";
    }
    ESP_RETURN_ON_FALSE(strlen(password) <= WIFI_MGR_PASS_MAX, ESP_ERR_INVALID_SIZE, TAG, "password too long");
    ESP_RETURN_ON_FALSE(password[0] == '\0' || strlen(password) >= 8, ESP_ERR_INVALID_SIZE, TAG,
                        "password shorter than 8 chars");

    ESP_RETURN_ON_ERROR(save_pair(NVS_KEY_STA_SSID, ssid, NVS_KEY_STA_PASS, password), TAG, "save");

    lock();
    strlcpy(s.status.sta_ssid, ssid, sizeof(s.status.sta_ssid));
    strlcpy(s.sta_pass, password, sizeof(s.sta_pass));
    s.status.sta_configured = true;
    wifi_mgr_state_t state = s.status.state;
    unlock();

    ESP_LOGI(TAG, "station credentials updated for \"%s\"", ssid);
    esp_timer_stop(s.ap_linger_timer);

    if (state == WIFI_MGR_STATE_CONNECTED || state == WIFI_MGR_STATE_CONNECTING) {
        /* The disconnect event re-enters sta_connect logic with the new config. */
        s.sta_disabled = false;
        s.connect_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_RW_STA_CONNECT_TIMEOUT_S * US_PER_S;
        apply_sta_config();
        esp_wifi_disconnect();
    } else {
        sta_connect_begin();
    }
    return ESP_OK;
}

esp_err_t wifi_manager_clear_sta_credentials(void)
{
    ESP_RETURN_ON_ERROR(save_pair(NVS_KEY_STA_SSID, "", NVS_KEY_STA_PASS, ""), TAG, "save");

    lock();
    s.status.sta_ssid[0] = '\0';
    s.sta_pass[0] = '\0';
    s.status.sta_configured = false;
    s.status.sta_ip[0] = '\0';
    s.status.sta_rssi = 0;
    unlock();

    s.sta_disabled = true;
    esp_timer_stop(s.retry_timer);
    esp_timer_stop(s.ap_linger_timer);
    ESP_LOGI(TAG, "station credentials cleared");

    ap_start();
    esp_wifi_disconnect();
    set_state(WIFI_MGR_STATE_AP_ONLY);
    return ESP_OK;
}

esp_err_t wifi_manager_set_ap_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        ssid = "";
    }
    if (password == NULL) {
        password = "";
    }
    ESP_RETURN_ON_FALSE(strlen(ssid) <= WIFI_MGR_SSID_MAX, ESP_ERR_INVALID_SIZE, TAG, "ssid too long");
    ESP_RETURN_ON_FALSE(password[0] == '\0' || (strlen(password) >= 8 && strlen(password) <= WIFI_MGR_PASS_MAX),
                        ESP_ERR_INVALID_SIZE, TAG, "AP password must be empty or 8..64 chars");

    ESP_RETURN_ON_ERROR(save_pair(NVS_KEY_AP_SSID, ssid, NVS_KEY_AP_PASS, password), TAG, "save");

    lock();
    s.ap_ssid_custom = (ssid[0] != '\0');
    if (s.ap_ssid_custom) {
        strlcpy(s.status.ap_ssid, ssid, sizeof(s.status.ap_ssid));
    } else {
        default_ap_ssid(s.status.ap_ssid, sizeof(s.status.ap_ssid));
    }
    strlcpy(s.ap_pass, password, sizeof(s.ap_pass));
    bool active = s.status.ap_active;
    unlock();

    ESP_LOGI(TAG, "access point credentials updated: \"%s\" (%s)", s.status.ap_ssid,
             password[0] ? "WPA2" : "open");
    if (active) {
        return apply_ap_config();
    }
    return ESP_OK;
}

void wifi_manager_get_status(wifi_mgr_status_t *out)
{
    lock();
    *out = s.status;
    unlock();

    if (out->state == WIFI_MGR_STATE_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->sta_rssi = ap.rssi;
        }
    }
}

esp_err_t wifi_manager_scan(wifi_mgr_scan_entry_t *entries, size_t max_entries, size_t *count)
{
    ESP_RETURN_ON_FALSE(entries != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");
    *count = 0;

    const wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 100, .max = 300},
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&cfg, true), TAG, "scan start");

    uint16_t found = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&found), TAG, "scan count");
    if (found == 0) {
        esp_wifi_clear_ap_list();
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(*records));
    if (records == NULL) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_wifi_scan_get_ap_records(&found, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    /* Records come sorted by RSSI; keep the strongest instance of every SSID. */
    size_t n = 0;
    for (uint16_t i = 0; i < found && n < max_entries; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < n; j++) {
            if (strcmp(entries[j].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(entries[n].ssid, ssid, sizeof(entries[n].ssid));
        entries[n].rssi = records[i].rssi;
        entries[n].channel = records[i].primary;
        entries[n].auth = records[i].authmode;
        n++;
    }
    free(records);
    *count = n;
    return ESP_OK;
}
