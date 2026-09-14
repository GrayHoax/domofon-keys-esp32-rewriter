#include "web_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "ibutton.h"
#include "activekey.h"
#include "wifi_manager.h"
#include "status_led.h"
#include "keydb.h"
#include "web_internal.h"

static const char *TAG = "web";

#define SCAN_MAX_ENTRIES   20
#define VERIFY_DEFAULT_N   5
#define VERIFY_MAX_N       50
#define REBOOT_DELAY_MS    500
#define WRITE_WAIT_DEFAULT_S 30 /* How long an armed write waits for a blank. */
#define WRITE_WAIT_MAX_S     120

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

/* ------------------------------------------------------------------------- */
/* Response helpers                                                           */
/* ------------------------------------------------------------------------- */

esp_err_t web_send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json alloc failed");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status ? status : "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddFalseToObject(root, "ok");
    cJSON_AddStringToObject(root, "error", code);
    cJSON_AddStringToObject(root, "message", message);
    return web_send_json(req, root, status);
}

/** Read the request body and parse it as JSON. Sends the error response itself on failure. */
cJSON *web_read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return cJSON_CreateObject();
    }
    if (req->content_len > WEB_MAX_JSON_BODY) {
        web_send_error(req, "413 Payload Too Large", "body_too_large", "Request body exceeds limit");
        return NULL;
    }

    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        web_send_error(req, "500 Internal Server Error", "no_mem", "Out of memory");
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) {
            free(buf);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "timeout");
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            }
            return NULL;
        }
        received += n;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (json == NULL) {
        web_send_error(req, "400 Bad Request", "bad_json", "Malformed JSON body");
    }
    return json;
}

const char *web_json_string(const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

bool web_json_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

int web_json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static void add_reader_state(cJSON *obj, const ibutton_reader_state_t *st)
{
    char id[IBUTTON_ROM_STR_LEN];
    cJSON_AddBoolToObject(obj, "present", st->present);
    cJSON_AddBoolToObject(obj, "crc_ok", st->crc_ok);
    cJSON_AddBoolToObject(obj, "bus_shorted", st->bus_shorted);
    cJSON_AddBoolToObject(obj, "tm01_timing", st->tm01_timing);
    cJSON_AddStringToObject(obj, "proto",
                            st->present && st->active_proto != ACTIVEKEY_PROTO_NONE
                                ? activekey_proto_str(st->active_proto) : "dallas");
    if (st->present && st->active_proto != ACTIVEKEY_PROTO_NONE) {
        char code[ACTIVEKEY_CODE_STR_LEN];
        activekey_code_to_str(st->active_proto, st->active_code, code);
        cJSON_AddNullToObject(obj, "id");
        cJSON_AddStringToObject(obj, "code", code);
    } else if (st->present) {
        ibutton_key_to_str(&st->key, id);
        cJSON_AddStringToObject(obj, "id", id);
        cJSON_AddNumberToObject(obj, "family", st->key.rom[0]);

        keydb_entry_t entry;
        bool known = st->crc_ok && keydb_get(&st->key, &entry) == ESP_OK;
        cJSON_AddBoolToObject(obj, "known", known);
        cJSON_AddStringToObject(obj, "name", known ? entry.name : "");
    } else {
        cJSON_AddNullToObject(obj, "id");
    }
    cJSON_AddNumberToObject(obj, "age_ms", (double)((esp_timer_get_time() - st->updated_us) / 1000));
}

static const char *auth_mode_str(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa3";
    default:
        return "other";
    }
}

/* ------------------------------------------------------------------------- */
/* Handlers: UI and status                                                    */
/* ------------------------------------------------------------------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    wifi_mgr_status_t w;
    ibutton_reader_state_t r;
    wifi_manager_get_status(&w);
    ibutton_get_state(&r);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "idf", esp_app_get_description()->idf_ver);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", w.mac[0], w.mac[1], w.mac[2], w.mac[3], w.mac[4],
             w.mac[5]);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "state", wifi_manager_state_str(w.state));
    cJSON_AddBoolToObject(wifi, "ap_active", w.ap_active);
    cJSON_AddBoolToObject(wifi, "sta_configured", w.sta_configured);
    cJSON_AddStringToObject(wifi, "sta_ssid", w.sta_ssid);
    cJSON_AddStringToObject(wifi, "sta_ip", w.sta_ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.sta_rssi);
    cJSON_AddStringToObject(wifi, "ap_ssid", w.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_ip", w.ap_ip);
    cJSON_AddNumberToObject(wifi, "ap_clients", w.ap_clients);
    cJSON_AddStringToObject(wifi, "mac", mac);
    cJSON_AddStringToObject(wifi, "hostname", w.hostname);

    cJSON *reader = cJSON_AddObjectToObject(root, "reader");
    add_reader_state(reader, &r);

    return web_send_json(req, root, NULL);
}

/* ------------------------------------------------------------------------- */
/* Handlers: key operations                                                   */
/* ------------------------------------------------------------------------- */

static esp_err_t h_key_get(httpd_req_t *req)
{
    ibutton_reader_state_t st;
    ibutton_get_state(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    add_reader_state(root, &st);
    return web_send_json(req, root, NULL);
}

static esp_err_t h_key_read(httpd_req_t *req)
{
    ibutton_key_t key;
    esp_err_t err = ibutton_read(&key);

    switch (err) {
    case ESP_OK:
    case ESP_ERR_INVALID_CRC: {
        char id[IBUTTON_ROM_STR_LEN];
        ibutton_key_to_str(&key, id);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddTrueToObject(root, "ok");
        cJSON_AddStringToObject(root, "id", id);
        cJSON_AddBoolToObject(root, "crc_ok", err == ESP_OK);
        cJSON_AddNumberToObject(root, "family", key.rom[0]);
        if (err == ESP_OK) {
            status_led_flash(STATUS_LED_FLASH_KEY_SEEN);
        }
        return web_send_json(req, root, NULL);
    }
    case ESP_ERR_NOT_SUPPORTED:
    case ESP_ERR_INVALID_RESPONSE: {
        /* A Cyfral/Metakom key (or an undecoded stream): report it the way /api/key does. */
        ibutton_reader_state_t st;
        ibutton_get_state(&st);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddTrueToObject(root, "ok");
        add_reader_state(root, &st);
        status_led_flash(STATUS_LED_FLASH_KEY_SEEN);
        return web_send_json(req, root, NULL);
    }
    case ESP_ERR_NOT_FOUND:
        return web_send_error(req, "404 Not Found", "no_device", "Ключ не обнаружен");
    case ESP_ERR_INVALID_STATE:
        return web_send_error(req, "409 Conflict", "bus_shorted", "Линия данных замкнута");
    case ESP_ERR_TIMEOUT:
        return web_send_error(req, "503 Service Unavailable", "busy", "Шина занята другой операцией");
    default:
        return web_send_error(req, "500 Internal Server Error", "internal", esp_err_to_name(err));
    }
}

/** GET /api/key/analog - one raw Cyfral/Metakom capture, for bring-up on new hardware. */
static esp_err_t h_key_analog(httpd_req_t *req)
{
    if (!activekey_available()) {
        return web_send_error(req, "501 Not Implemented", "no_adc",
                              "Аналоговый вход не настроен (RW_ANALOG_SENSE_GPIO)");
    }
    activekey_result_t r;
    esp_err_t err = ibutton_active_probe(&r);
    if (err == ESP_ERR_TIMEOUT) {
        return web_send_error(req, "503 Service Unavailable", "busy", "Шина занята другой операцией");
    }

    char code[ACTIVEKEY_CODE_STR_LEN];
    activekey_code_to_str(r.proto, r.code, code);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddBoolToObject(root, "decoded", err == ESP_OK);
    cJSON_AddStringToObject(root, "proto", activekey_proto_str(r.proto));
    cJSON_AddStringToObject(root, "code", code);
    cJSON *sig = cJSON_AddObjectToObject(root, "signal");
    cJSON_AddNumberToObject(sig, "adc_min", r.adc_min);
    cJSON_AddNumberToObject(sig, "adc_max", r.adc_max);
    cJSON_AddNumberToObject(sig, "swing", r.adc_max - r.adc_min);
    cJSON_AddNumberToObject(sig, "edges", r.edges);
    cJSON_AddNumberToObject(sig, "period_us", r.period_us);
    cJSON_AddNumberToObject(sig, "gpio_level", gpio_get_level(CONFIG_RW_ONEWIRE_GPIO));
    return web_send_json(req, root, NULL);
}

/** Human-readable outcome of a write plus the HTTP status it maps to. */
static const char *write_result_message(ibutton_write_result_t res, const char **status)
{
    switch (res) {
    case IBUTTON_WRITE_OK:
        *status = "200 OK";
        return "Ключ записан и проверен";
    case IBUTTON_WRITE_ERR_NO_DEVICE:
        *status = "404 Not Found";
        return "Заготовка не обнаружена или потерян контакт во время записи";
    case IBUTTON_WRITE_ERR_BUS_SHORTED:
        *status = "409 Conflict";
        return "Линия данных замкнута";
    case IBUTTON_WRITE_ERR_VERIFY:
        *status = "422 Unprocessable Entity";
        return "Проверка после записи не прошла: попробуйте другой тип заготовки";
    case IBUTTON_WRITE_ERR_BUSY:
        *status = "503 Service Unavailable";
        return "Шина занята другой операцией";
    case IBUTTON_WRITE_ERR_TIMEOUT:
        *status = "408 Request Timeout";
        return "Заготовка не приложена за отведённое время";
    default:
        *status = "500 Internal Server Error";
        return "Ошибка записи";
    }
}

static const char *job_state_str(ibutton_job_state_t st)
{
    switch (st) {
    case IBUTTON_JOB_WAITING:
        return "waiting";
    case IBUTTON_JOB_WRITING:
        return "writing";
    case IBUTTON_JOB_DONE:
        return "done";
    default:
        return "idle";
    }
}

static cJSON *job_to_json(const ibutton_write_job_t *job)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", job_state_str(job->state));
    if (job->state == IBUTTON_JOB_IDLE) {
        return j;
    }

    char id[IBUTTON_ROM_STR_LEN];
    ibutton_key_to_str(&job->key, id);
    cJSON_AddNumberToObject(j, "job_id", job->id);
    cJSON_AddStringToObject(j, "id", id);
    cJSON_AddStringToObject(j, "variant", ibutton_write_variant_str(job->variant));

    if (job->state == IBUTTON_JOB_WAITING) {
        int64_t left_us = job->deadline_us - esp_timer_get_time();
        cJSON_AddNumberToObject(j, "remaining_s", left_us > 0 ? (double)(left_us + 999999) / 1000000.0 : 0);
    }
    if (job->state == IBUTTON_JOB_DONE) {
        const char *status;
        cJSON_AddBoolToObject(j, "ok", job->result == IBUTTON_WRITE_OK);
        cJSON_AddStringToObject(j, "result", ibutton_write_result_str(job->result));
        cJSON_AddStringToObject(j, "message", write_result_message(job->result, &status));
    }
    return j;
}

/** GET /api/key/write — progress of the armed write, if any. */
static esp_err_t h_key_write_status(httpd_req_t *req)
{
    ibutton_write_job_t job;
    ibutton_write_job_get(&job);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "job", job_to_json(&job));
    return web_send_json(req, root, NULL);
}

/** DELETE /api/key/write — stop waiting for a blank. */
static esp_err_t h_key_write_cancel(httpd_req_t *req)
{
    esp_err_t err = ibutton_write_cancel();
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_error(req, "409 Conflict", "writing", "Идёт запись, дождитесь окончания");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    return web_send_json(req, root, NULL);
}

/**
 * POST /api/key/write — {id, variant, fix_crc, wait, timeout_s}.
 * With "wait": true the device arms the write and answers at once; the
 * blank is programmed when it is placed on the pad (poll GET to follow).
 * Without it the write happens immediately against whatever is attached.
 */
static esp_err_t h_key_write(httpd_req_t *req)
{
    cJSON *body = web_read_json_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }

    /* Everything needed from the body is copied out before it is freed:
     * web_json_string() returns pointers into the cJSON tree. */
    ibutton_key_t key;
    ibutton_write_variant_t variant = IBUTTON_WRITE_RW1990_V1;
    esp_err_t err = ibutton_key_from_str(web_json_string(body, "id", ""), &key);
    esp_err_t variant_err = ibutton_write_variant_from_str(web_json_string(body, "variant", "rw1990v1"), &variant);
    bool fix_crc = web_json_bool(body, "fix_crc", false);
    bool wait = web_json_bool(body, "wait", false);
    int timeout_s = web_json_int(body, "timeout_s", WRITE_WAIT_DEFAULT_S);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad_id", "ID ключа должен содержать 16 шестнадцатеричных цифр");
    }
    if (variant_err != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad_variant", "Неизвестный тип заготовки");
    }
    if (!ibutton_key_crc_ok(&key)) {
        if (!fix_crc) {
            return web_send_error(req, "400 Bad Request", "bad_crc",
                              "Контрольная сумма ID неверна (включите автоисправление CRC)");
        }
        ibutton_key_fix_crc(&key);
    }

    char id[IBUTTON_ROM_STR_LEN];
    ibutton_key_to_str(&key, id);

    if (wait) {
        if (timeout_s < 1 || timeout_s > WRITE_WAIT_MAX_S) {
            timeout_s = WRITE_WAIT_DEFAULT_S;
        }
        err = ibutton_write_arm(&key, variant, (uint32_t)timeout_s * 1000);
        if (err == ESP_ERR_INVALID_STATE) {
            return web_send_error(req, "409 Conflict", "busy", "Уже ожидается запись другого ключа");
        }
        if (err != ESP_OK) {
            return web_send_error(req, "500 Internal Server Error", "internal", esp_err_to_name(err));
        }
        ibutton_write_job_t job;
        ibutton_write_job_get(&job);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddTrueToObject(root, "ok");
        cJSON_AddTrueToObject(root, "armed");
        cJSON_AddStringToObject(root, "id", id);
        cJSON_AddItemToObject(root, "job", job_to_json(&job));
        return web_send_json(req, root, "202 Accepted");
    }

    ibutton_write_result_t res = ibutton_write(&key, variant);
    const char *status;
    const char *message = write_result_message(res, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", res == IBUTTON_WRITE_OK);
    cJSON_AddStringToObject(root, "result", ibutton_write_result_str(res));
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "variant", ibutton_write_variant_str(variant));
    cJSON_AddStringToObject(root, "message", message);

    status_led_flash(res == IBUTTON_WRITE_OK ? STATUS_LED_FLASH_SUCCESS : STATUS_LED_FLASH_ERROR);
    return web_send_json(req, root, status);
}

static esp_err_t h_key_verify(httpd_req_t *req)
{
    cJSON *body = web_read_json_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }

    ibutton_key_t expected;
    esp_err_t err = ibutton_key_from_str(web_json_string(body, "id", ""), &expected);
    int reads = web_json_int(body, "reads", VERIFY_DEFAULT_N);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad_id", "ID ключа должен содержать 16 шестнадцатеричных цифр");
    }
    if (reads < 1) {
        reads = 1;
    } else if (reads > VERIFY_MAX_N) {
        reads = VERIFY_MAX_N;
    }

    ibutton_key_t actual = {0};
    int reads_ok = 0;
    err = ibutton_verify(&expected, &actual, &reads_ok, reads);

    char expected_str[IBUTTON_ROM_STR_LEN], actual_str[IBUTTON_ROM_STR_LEN];
    ibutton_key_to_str(&expected, expected_str);
    ibutton_key_to_str(&actual, actual_str);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "match", err == ESP_OK);
    cJSON_AddNumberToObject(root, "reads_ok", reads_ok);
    cJSON_AddNumberToObject(root, "reads_total", reads);
    cJSON_AddStringToObject(root, "expected", expected_str);
    cJSON_AddBoolToObject(root, "expected_crc_ok", ibutton_key_crc_ok(&expected));

    const char *status = "200 OK";
    switch (err) {
    case ESP_OK:
        cJSON_AddStringToObject(root, "actual", actual_str);
        cJSON_AddStringToObject(root, "message", "Ключ отвечает стабильно, ID совпадает");
        status_led_flash(STATUS_LED_FLASH_SUCCESS);
        break;
    case ESP_ERR_INVALID_RESPONSE:
        cJSON_AddStringToObject(root, "actual", actual_str);
        cJSON_AddStringToObject(root, "message", "ID ключа не совпадает с ожидаемым");
        break;
    case ESP_ERR_INVALID_CRC:
        cJSON_AddStringToObject(root, "actual", actual_str);
        cJSON_AddStringToObject(root, "message", "Ошибки CRC при чтении: плохой контакт или неисправный ключ");
        break;
    case ESP_ERR_NOT_FOUND:
        cJSON_AddNullToObject(root, "actual");
        cJSON_AddStringToObject(root, "message", "Ключ не обнаружен (или отвечает нестабильно)");
        break;
    case ESP_ERR_INVALID_STATE:
        cJSON_AddNullToObject(root, "actual");
        cJSON_AddStringToObject(root, "message", "Линия данных замкнута");
        break;
    case ESP_ERR_TIMEOUT:
        cJSON_AddNullToObject(root, "actual");
        cJSON_AddStringToObject(root, "message", "Шина занята другой операцией");
        status = "503 Service Unavailable";
        break;
    default:
        cJSON_AddNullToObject(root, "actual");
        cJSON_AddStringToObject(root, "message", esp_err_to_name(err));
        status = "500 Internal Server Error";
        break;
    }
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        status_led_flash(STATUS_LED_FLASH_ERROR);
    }
    return web_send_json(req, root, status);
}

/* ------------------------------------------------------------------------- */
/* Handlers: Wi-Fi                                                            */
/* ------------------------------------------------------------------------- */

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    wifi_mgr_scan_entry_t *entries = calloc(SCAN_MAX_ENTRIES, sizeof(*entries));
    if (entries == NULL) {
        return web_send_error(req, "500 Internal Server Error", "no_mem", "Out of memory");
    }

    size_t count = 0;
    esp_err_t err = wifi_manager_scan(entries, SCAN_MAX_ENTRIES, &count);
    if (err != ESP_OK) {
        free(entries);
        if (err == ESP_ERR_INVALID_STATE) {
            return web_send_error(req, "503 Service Unavailable", "busy",
                              "Идёт подключение к сети, повторите сканирование позже");
        }
        return web_send_error(req, "500 Internal Server Error", "scan_failed", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON *list = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < count; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", entries[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", entries[i].rssi);
        cJSON_AddNumberToObject(n, "channel", entries[i].channel);
        cJSON_AddStringToObject(n, "auth", auth_mode_str(entries[i].auth));
        cJSON_AddItemToArray(list, n);
    }
    free(entries);
    return web_send_json(req, root, NULL);
}

static esp_err_t h_wifi_sta_post(httpd_req_t *req)
{
    cJSON *body = web_read_json_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    const char *ssid = web_json_string(body, "ssid", "");
    const char *pass = web_json_string(body, "password", "");
    esp_err_t err = wifi_manager_set_sta_credentials(ssid, pass);
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_error(req, "400 Bad Request", "bad_ssid", "Укажите имя сети");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return web_send_error(req, "400 Bad Request", "bad_length",
                          "SSID до 32 символов, пароль пустой или 8..64 символа");
    }
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "save_failed", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "message", "Настройки сохранены, выполняется подключение");
    return web_send_json(req, root, NULL);
}

static esp_err_t h_wifi_sta_delete(httpd_req_t *req)
{
    esp_err_t err = wifi_manager_clear_sta_credentials();
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "save_failed", esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "message", "Настройки сети удалены, устройство работает как точка доступа");
    return web_send_json(req, root, NULL);
}

static esp_err_t h_wifi_ap_post(httpd_req_t *req)
{
    cJSON *body = web_read_json_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    const char *ssid = web_json_string(body, "ssid", "");
    const char *pass = web_json_string(body, "password", "");
    esp_err_t err = wifi_manager_set_ap_credentials(ssid, pass);
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_SIZE) {
        return web_send_error(req, "400 Bad Request", "bad_length",
                          "SSID до 32 символов, пароль пустой (открытая сеть) или 8..64 символа");
    }
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "save_failed", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "message", "Настройки точки доступа сохранены");
    return web_send_json(req, root, NULL);
}

/* ------------------------------------------------------------------------- */
/* Handlers: system                                                           */
/* ------------------------------------------------------------------------- */

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "message", "Перезагрузка...");
    esp_err_t err = web_send_json(req, root, NULL);

    const esp_timer_create_args_t args = {.callback = reboot_timer_cb, .name = "reboot"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, REBOOT_DELAY_MS * 1000);
    }
    return err;
}

/** Captive-portal probes and unknown paths are redirected to the UI. */
static esp_err_t h_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* Server lifecycle                                                           */
/* ------------------------------------------------------------------------- */

esp_err_t web_server_start(void)
{
    ESP_RETURN_ON_FALSE(s_server == NULL, ESP_ERR_INVALID_STATE, TAG, "already running");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_RW_HTTP_PORT;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start failed");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/key", .method = HTTP_GET, .handler = h_key_get},
        {.uri = "/api/key/read", .method = HTTP_POST, .handler = h_key_read},
        {.uri = "/api/key/analog", .method = HTTP_GET, .handler = h_key_analog},
        {.uri = "/api/key/write", .method = HTTP_POST, .handler = h_key_write},
        {.uri = "/api/key/write", .method = HTTP_GET, .handler = h_key_write_status},
        {.uri = "/api/key/write", .method = HTTP_DELETE, .handler = h_key_write_cancel},
        {.uri = "/api/key/verify", .method = HTTP_POST, .handler = h_key_verify},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_wifi_scan},
        {.uri = "/api/wifi/sta", .method = HTTP_POST, .handler = h_wifi_sta_post},
        {.uri = "/api/wifi/sta", .method = HTTP_DELETE, .handler = h_wifi_sta_delete},
        {.uri = "/api/wifi/ap", .method = HTTP_POST, .handler = h_wifi_ap_post},
        {.uri = "/api/keys", .method = HTTP_GET, .handler = web_keys_list},
        {.uri = "/api/keys", .method = HTTP_POST, .handler = web_keys_add},
        {.uri = "/api/keys", .method = HTTP_DELETE, .handler = web_keys_clear},
        {.uri = "/api/keys/export", .method = HTTP_GET, .handler = web_keys_export},
        {.uri = "/api/keys/import", .method = HTTP_POST, .handler = web_keys_import},
        {.uri = "/api/keys/*", .method = HTTP_PUT, .handler = web_keys_update},
        {.uri = "/api/keys/*", .method = HTTP_DELETE, .handler = web_keys_delete},
        {.uri = "/api/system/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/*", .method = HTTP_GET, .handler = h_redirect},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "route %s failed: %s", routes[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", cfg.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
