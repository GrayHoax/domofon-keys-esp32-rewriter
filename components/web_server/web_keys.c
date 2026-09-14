/**
 * @file web_keys.c
 * @brief REST handlers for the key database.
 *
 * Listing and export are streamed record by record and import is parsed
 * line by line, so the database size is bounded by flash, not by RAM.
 *
 * File format (JSON Lines, UTF-8): one object per line,
 *   {"id":"01A1B2C3D4E5F65C","name":"Подъезд 3"}
 * Lines starting with '#' and blank lines are ignored on import.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"

#include "ibutton.h"
#include "keydb.h"
#include "web_internal.h"

static const char *TAG = "web_keys";

#define IMPORT_LINE_MAX  1024 /* Longest accepted line: 16-hex id + 360-byte name + JSON overhead. */
#define IMPORT_CHUNK     512
#define EXPORT_FILENAME  "rw1990-keys.jsonl"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/** Parse the key id from "/api/keys/<id>[?query]". */
static esp_err_t key_from_uri(httpd_req_t *req, ibutton_key_t *out)
{
    const char *prefix = "/api/keys/";
    const char *p = strstr(req->uri, prefix);
    if (p == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    p += strlen(prefix);

    char id[64];
    size_t n = strcspn(p, "?");
    if (n == 0 || n >= sizeof(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    return ibutton_key_from_str(id, out);
}

/**
 * Make an arbitrary UTF-8 string acceptable to keydb: drop control characters,
 * cut at the code-point / byte limits on a character boundary.
 */
static void sanitize_name(const char *in, char out[KEYDB_NAME_MAX_BYTES + 1])
{
    size_t bytes = 0, chars = 0;
    const unsigned char *p = (const unsigned char *)in;

    while (*p && chars < KEYDB_NAME_MAX_CHARS) {
        int len = 1;
        if (*p >= 0x80) {
            if ((*p & 0xE0) == 0xC0) {
                len = 2;
            } else if ((*p & 0xF0) == 0xE0) {
                len = 3;
            } else if ((*p & 0xF8) == 0xF0) {
                len = 4;
            } else {
                p++; /* stray continuation byte */
                continue;
            }
            bool ok = true;
            for (int i = 1; i < len; i++) {
                ok = ok && ((p[i] & 0xC0) == 0x80);
            }
            if (!ok) {
                p++;
                continue;
            }
        } else if (*p < 0x20 || *p == 0x7F) {
            p++;
            continue;
        }
        if (bytes + len > KEYDB_NAME_MAX_BYTES) {
            break;
        }
        memcpy(out + bytes, p, len);
        bytes += len;
        chars++;
        p += len;
    }
    out[bytes] = '\0';
}

static cJSON *entry_to_json(const keydb_entry_t *e)
{
    char id[IBUTTON_ROM_STR_LEN];
    ibutton_key_to_str(&e->key, id);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddNumberToObject(o, "seq", e->seq);
    uint16_t cyfral;
    if (ibutton_key_cyfral_code(&e->key, &cyfral)) {
        char code[ACTIVEKEY_CODE_STR_LEN];
        activekey_code_to_str(ACTIVEKEY_PROTO_CYFRAL, cyfral, code);
        cJSON_AddStringToObject(o, "cyfral", code);
    }
    return o;
}

static esp_err_t send_entry_response(httpd_req_t *req, const ibutton_key_t *key, const char *message)
{
    keydb_entry_t e;
    esp_err_t err = keydb_get(key, &e);
    if (err != ESP_OK) {
        return web_send_error(req, "404 Not Found", "not_found", "Ключ не найден в базе");
    }
    cJSON *root = entry_to_json(&e);
    cJSON_AddTrueToObject(root, "ok");
    if (message) {
        cJSON_AddStringToObject(root, "message", message);
    }
    return web_send_json(req, root, NULL);
}

static void add_stats(cJSON *root)
{
    keydb_stats_t st;
    keydb_get_stats(&st);
    cJSON *o = cJSON_AddObjectToObject(root, "stats");
    cJSON_AddNumberToObject(o, "count", st.count);
    cJSON_AddNumberToObject(o, "used_entries", st.used_entries);
    cJSON_AddNumberToObject(o, "free_entries", st.free_entries);
    cJSON_AddNumberToObject(o, "total_entries", st.total_entries);
}

/* ------------------------------------------------------------------------- */
/* Streaming list / export                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    httpd_req_t *req;
    bool json_array; /* true: ",{...}" items of a JSON array; false: JSON Lines */
    bool first;
    esp_err_t err;
} stream_ctx_t;

static bool stream_entry_cb(const keydb_entry_t *e, void *arg)
{
    stream_ctx_t *ctx = arg;
    cJSON *o = entry_to_json(e);
    if (!ctx->json_array) {
        cJSON_DeleteItemFromObject(o, "seq");
    }
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text == NULL) {
        ctx->err = ESP_ERR_NO_MEM;
        return false;
    }

    esp_err_t err = ESP_OK;
    if (ctx->json_array) {
        if (!ctx->first) {
            err = httpd_resp_send_chunk(ctx->req, ",", 1);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(ctx->req, text, HTTPD_RESP_USE_STRLEN);
        }
    } else {
        err = httpd_resp_send_chunk(ctx->req, text, HTTPD_RESP_USE_STRLEN);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(ctx->req, "\n", 1);
        }
    }
    free(text);
    ctx->first = false;
    ctx->err = err;
    return err == ESP_OK;
}

esp_err_t web_keys_list(httpd_req_t *req)
{
    /* Head of the document carries the stats so the UI gets them in one request. */
    cJSON *head = cJSON_CreateObject();
    cJSON_AddTrueToObject(head, "ok");
    add_stats(head);
    char *head_text = cJSON_PrintUnformatted(head);
    cJSON_Delete(head);
    if (head_text == NULL) {
        return web_send_error(req, "500 Internal Server Error", "no_mem", "Out of memory");
    }
    /* Replace the closing brace with the start of the array. */
    size_t hl = strlen(head_text);
    head_text[hl - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, head_text, HTTPD_RESP_USE_STRLEN);
    free(head_text);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, ",\"keys\":[", HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        return err;
    }

    stream_ctx_t ctx = {.req = req, .json_array = true, .first = true, .err = ESP_OK};
    keydb_iterate(stream_entry_cb, &ctx);
    if (ctx.err != ESP_OK) {
        return ctx.err;
    }
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t web_keys_export(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/x-ndjson; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"" EXPORT_FILENAME "\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const char *header = "# RW1990 key database v1: one JSON object per line {\"id\":\"<16 hex>\",\"name\":\"...\"}\n";
    esp_err_t err = httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }

    stream_ctx_t ctx = {.req = req, .json_array = false, .first = true, .err = ESP_OK};
    keydb_iterate(stream_entry_cb, &ctx);
    if (ctx.err != ESP_OK) {
        return ctx.err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------------- */
/* Single-record operations                                                   */
/* ------------------------------------------------------------------------- */

esp_err_t web_keys_add(httpd_req_t *req)
{
    cJSON *body = web_read_json_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    ibutton_key_t key;
    esp_err_t err = ibutton_key_from_str(web_json_string(body, "id", ""), &key);
    bool fix_crc = web_json_bool(body, "fix_crc", false);
    char name[KEYDB_NAME_MAX_BYTES + 1];
    sanitize_name(web_json_string(body, "name", ""), name);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad_id", "ID ключа должен содержать 16 шестнадцатеричных цифр");
    }
    if (!ibutton_key_crc_ok(&key)) {
        if (!fix_crc) {
            return web_send_error(req, "400 Bad Request", "bad_crc", "Контрольная сумма ID неверна");
        }
        ibutton_key_fix_crc(&key);
    }

    bool added = false;
    err = keydb_add(&key, name, &added);
    if (err == ESP_ERR_NO_MEM) {
        return web_send_error(req, "507 Insufficient Storage", "db_full", "База ключей заполнена");
    }
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "db_error", esp_err_to_name(err));
    }
    return send_entry_response(req, &key, added ? "Ключ добавлен в базу" : "Ключ уже есть в базе");
}

esp_err_t web_keys_update(httpd_req_t *req)
{
    ibutton_key_t key;
    if (key_from_uri(req, &key) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad_id", "Некорректный ID в адресе");
    }
    cJSON *body = web_read_json_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    const char *raw = web_json_string(body, "name", NULL);
    if (raw == NULL) {
        cJSON_Delete(body);
        return web_send_error(req, "400 Bad Request", "bad_name", "Поле name обязательно");
    }
    if (!keydb_name_valid(raw)) {
        cJSON_Delete(body);
        return web_send_error(req, "400 Bad Request", "bad_name",
                              "Название: до 120 символов, без управляющих символов");
    }
    esp_err_t err = keydb_set_name(&key, raw);
    cJSON_Delete(body);

    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_error(req, "404 Not Found", "not_found", "Ключ не найден в базе");
    }
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "db_error", esp_err_to_name(err));
    }
    return send_entry_response(req, &key, "Название сохранено");
}

esp_err_t web_keys_delete(httpd_req_t *req)
{
    ibutton_key_t key;
    if (key_from_uri(req, &key) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad_id", "Некорректный ID в адресе");
    }
    esp_err_t err = keydb_remove(&key);
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_error(req, "404 Not Found", "not_found", "Ключ не найден в базе");
    }
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "db_error", esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "message", "Ключ удалён из базы");
    add_stats(root);
    return web_send_json(req, root, NULL);
}

esp_err_t web_keys_clear(httpd_req_t *req)
{
    esp_err_t err = keydb_clear();
    if (err != ESP_OK) {
        return web_send_error(req, "500 Internal Server Error", "db_error", esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "message", "База ключей очищена");
    add_stats(root);
    return web_send_json(req, root, NULL);
}

/* ------------------------------------------------------------------------- */
/* Import                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    unsigned total;   /* non-empty, non-comment lines */
    unsigned added;
    unsigned skipped; /* already present */
    unsigned invalid;
    bool full;        /* storage exhausted: remaining lines are not attempted */
} import_stats_t;

static void import_line(char *line, import_stats_t *st)
{
    /* Trim CR and surrounding whitespace. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0' || *line == '#') {
        return;
    }
    st->total++;
    if (st->full) {
        return;
    }

    cJSON *obj = cJSON_Parse(line);
    if (obj == NULL) {
        st->invalid++;
        return;
    }
    ibutton_key_t key;
    esp_err_t err = ibutton_key_from_str(web_json_string(obj, "id", ""), &key);
    char name[KEYDB_NAME_MAX_BYTES + 1];
    sanitize_name(web_json_string(obj, "name", ""), name);
    cJSON_Delete(obj);

    if (err != ESP_OK || !ibutton_key_crc_ok(&key)) {
        st->invalid++;
        return;
    }

    bool added = false;
    err = keydb_add(&key, name, &added);
    if (err == ESP_ERR_NO_MEM) {
        st->full = true;
        st->invalid++;
    } else if (err != ESP_OK) {
        st->invalid++;
    } else if (added) {
        st->added++;
    } else {
        st->skipped++;
    }
}

esp_err_t web_keys_import(httpd_req_t *req)
{
    /* Mode: ?mode=merge (default) keeps existing keys, ?mode=replace wipes first. */
    char query[32] = "", mode[16] = "merge";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "mode", mode, sizeof(mode));
    }
    bool replace = strcmp(mode, "replace") == 0;
    if (!replace && strcmp(mode, "merge") != 0) {
        return web_send_error(req, "400 Bad Request", "bad_mode", "mode must be merge or replace");
    }
    if (req->content_len == 0) {
        return web_send_error(req, "400 Bad Request", "empty", "Файл пуст");
    }

    char *buf = malloc(IMPORT_LINE_MAX + IMPORT_CHUNK + 1);
    if (buf == NULL) {
        return web_send_error(req, "500 Internal Server Error", "no_mem", "Out of memory");
    }

    if (replace) {
        esp_err_t err = keydb_clear();
        if (err != ESP_OK) {
            free(buf);
            return web_send_error(req, "500 Internal Server Error", "db_error", esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "import: database replaced");
    }

    import_stats_t st = {0};
    size_t fill = 0;        /* bytes buffered and not yet consumed */
    size_t remaining = req->content_len;
    bool line_overflow = false;

    while (remaining > 0) {
        size_t want = remaining < IMPORT_CHUNK ? remaining : IMPORT_CHUNK;
        int n = httpd_req_recv(req, buf + fill, want);
        if (n <= 0) {
            free(buf);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "timeout");
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            }
            return ESP_FAIL;
        }
        remaining -= n;
        fill += n;
        buf[fill] = '\0';

        /* Consume every complete line in the buffer. */
        char *start = buf;
        char *nl;
        while ((nl = memchr(start, '\n', fill - (start - buf))) != NULL) {
            *nl = '\0';
            if (line_overflow) {
                line_overflow = false; /* tail of an oversized line: drop it */
            } else {
                import_line(start, &st);
            }
            start = nl + 1;
        }

        /* Keep the partial tail; an over-long line is discarded as invalid. */
        fill -= (start - buf);
        if (fill > IMPORT_LINE_MAX) {
            if (!line_overflow) {
                st.total++;
                st.invalid++;
                line_overflow = true;
            }
            fill = 0;
        } else if (fill > 0 && start != buf) {
            memmove(buf, start, fill);
        }
    }
    if (fill > 0 && !line_overflow) {
        buf[fill] = '\0';
        import_line(buf, &st);
    }
    free(buf);

    ESP_LOGI(TAG, "import (%s): %u lines, %u added, %u duplicates, %u invalid%s", mode, st.total, st.added,
             st.skipped, st.invalid, st.full ? ", storage full" : "");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "mode", mode);
    cJSON_AddNumberToObject(root, "total", st.total);
    cJSON_AddNumberToObject(root, "added", st.added);
    cJSON_AddNumberToObject(root, "skipped", st.skipped);
    cJSON_AddNumberToObject(root, "invalid", st.invalid);
    cJSON_AddBoolToObject(root, "full", st.full);
    add_stats(root);
    return web_send_json(req, root, NULL);
}
