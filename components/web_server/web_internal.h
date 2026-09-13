/**
 * @file web_internal.h
 * @brief Helpers shared between the HTTP handler source files of this component.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

/** Largest JSON request body accepted by web_read_json_body(). */
#define WEB_MAX_JSON_BODY 1024

/** Serialise @p root (consumed), send it with @p status ("200 OK" when NULL). */
esp_err_t web_send_json(httpd_req_t *req, cJSON *root, const char *status);
esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *code, const char *message);

/** Read and parse the body. On failure the error response is already sent and NULL is returned. */
cJSON *web_read_json_body(httpd_req_t *req);

const char *web_json_string(const cJSON *obj, const char *key, const char *fallback);
bool        web_json_bool(const cJSON *obj, const char *key, bool fallback);
int         web_json_int(const cJSON *obj, const char *key, int fallback);

/* Key database handlers (web_keys.c). */
esp_err_t web_keys_list(httpd_req_t *req);
esp_err_t web_keys_add(httpd_req_t *req);
esp_err_t web_keys_clear(httpd_req_t *req);
esp_err_t web_keys_update(httpd_req_t *req);
esp_err_t web_keys_delete(httpd_req_t *req);
esp_err_t web_keys_export(httpd_req_t *req);
esp_err_t web_keys_import(httpd_req_t *req);
