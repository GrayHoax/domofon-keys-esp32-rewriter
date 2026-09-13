#include "keydb.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "keydb";

#define NS_KEYS  "keys"
#define NS_META  "meta"
#define META_SEQ "seq"

/* NVS key = family + 6 serial bytes as hex: 14 chars, within the 15-char limit. */
#define ID_BYTES 7
#define ID_LEN   (ID_BYTES * 2)

/** On-flash record: fixed header followed by a NUL-terminated UTF-8 name. */
typedef struct __attribute__((packed)) {
    uint8_t rom[IBUTTON_ROM_LEN];
    uint32_t seq;
} record_hdr_t;

#define RECORD_MAX (sizeof(record_hdr_t) + KEYDB_NAME_MAX_BYTES + 1)

static struct {
    SemaphoreHandle_t lock;
    nvs_handle_t keys;
    nvs_handle_t meta;
    size_t count;
    bool ready;
} s;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void make_id(const ibutton_key_t *key, char out[ID_LEN + 1])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < ID_BYTES; i++) {
        out[i * 2] = hex[key->rom[i] >> 4];
        out[i * 2 + 1] = hex[key->rom[i] & 0x0F];
    }
    out[ID_LEN] = '\0';
}

bool keydb_name_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t bytes = 0, chars = 0;
    const unsigned char *p = (const unsigned char *)name;

    while (*p) {
        int len;
        if (*p < 0x20 || *p == 0x7F) {
            return false; /* control characters */
        } else if (*p < 0x80) {
            len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;
        }
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                return false; /* truncated / malformed sequence */
            }
        }
        p += len;
        bytes += len;
        chars++;
        if (chars > KEYDB_NAME_MAX_CHARS || bytes > KEYDB_NAME_MAX_BYTES) {
            return false;
        }
    }
    return true;
}

/** Read a record by NVS key. Caller holds the lock. */
static esp_err_t read_record(const char *id, keydb_entry_t *out)
{
    uint8_t buf[RECORD_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(s.keys, id, buf, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len < sizeof(record_hdr_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const record_hdr_t *hdr = (const record_hdr_t *)buf;
    memcpy(out->key.rom, hdr->rom, IBUTTON_ROM_LEN);
    out->seq = hdr->seq;

    size_t name_len = len - sizeof(record_hdr_t);
    if (name_len > KEYDB_NAME_MAX_BYTES) {
        name_len = KEYDB_NAME_MAX_BYTES;
    }
    memcpy(out->name, buf + sizeof(record_hdr_t), name_len);
    out->name[name_len] = '\0';
    /* Records are stored NUL-terminated; strlen guards against a stray one. */
    out->name[strnlen(out->name, name_len)] = '\0';
    return ESP_OK;
}

/** Write a record. Caller holds the lock. */
static esp_err_t write_record(const char *id, const ibutton_key_t *key, uint32_t seq, const char *name)
{
    uint8_t buf[RECORD_MAX];
    record_hdr_t *hdr = (record_hdr_t *)buf;
    memcpy(hdr->rom, key->rom, IBUTTON_ROM_LEN);
    hdr->seq = seq;

    size_t name_len = strlen(name);
    memcpy(buf + sizeof(record_hdr_t), name, name_len + 1);

    esp_err_t err = nvs_set_blob(s.keys, id, buf, sizeof(record_hdr_t) + name_len + 1);
    if (err == ESP_OK) {
        err = nvs_commit(s.keys);
    }
    return err;
}

static uint32_t next_seq(void)
{
    uint32_t seq = 0;
    nvs_get_u32(s.meta, META_SEQ, &seq); /* Not-found leaves 0. */
    seq++;
    nvs_set_u32(s.meta, META_SEQ, seq);
    nvs_commit(s.meta);
    return seq;
}

static size_t count_records(void)
{
    size_t n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(KEYDB_PARTITION, NS_KEYS, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        n++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t keydb_init(void)
{
    ESP_RETURN_ON_FALSE(!s.ready, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    s.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    esp_err_t err = nvs_flash_init_partition(KEYDB_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "partition is stale or corrupt, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(KEYDB_PARTITION), TAG, "erase");
        err = nvs_flash_init_partition(KEYDB_PARTITION);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init partition '%s'", KEYDB_PARTITION);

    ESP_RETURN_ON_ERROR(nvs_open_from_partition(KEYDB_PARTITION, NS_KEYS, NVS_READWRITE, &s.keys), TAG,
                        "open keys");
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(KEYDB_PARTITION, NS_META, NVS_READWRITE, &s.meta), TAG,
                        "open meta");

    s.count = count_records();
    s.ready = true;

    keydb_stats_t st;
    keydb_get_stats(&st);
    ESP_LOGI(TAG, "%u keys stored, %u/%u NVS entries used", (unsigned)st.count, (unsigned)st.used_entries,
             (unsigned)st.total_entries);
    return ESP_OK;
}

esp_err_t keydb_add(const ibutton_key_t *key, const char *name, bool *added)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(key != NULL && ibutton_key_crc_ok(key), ESP_ERR_INVALID_ARG, TAG, "bad key");
    if (name == NULL) {
        name = "";
    }
    ESP_RETURN_ON_FALSE(keydb_name_valid(name), ESP_ERR_INVALID_ARG, TAG, "bad name");
    if (added) {
        *added = false;
    }

    char id[ID_LEN + 1];
    make_id(key, id);

    xSemaphoreTake(s.lock, portMAX_DELAY);
    size_t len = 0;
    esp_err_t err = nvs_get_blob(s.keys, id, NULL, &len);
    if (err == ESP_OK) {
        xSemaphoreGive(s.lock);
        return ESP_OK; /* Already stored: uniqueness is enforced here. */
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        xSemaphoreGive(s.lock);
        return err;
    }

    err = write_record(id, key, next_seq(), name);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        err = ESP_ERR_NO_MEM; /* Generic code: callers need not know about NVS. */
    }
    if (err == ESP_OK) {
        s.count++;
        if (added) {
            *added = true;
        }
    }
    xSemaphoreGive(s.lock);

    if (err == ESP_OK) {
        char str[IBUTTON_ROM_STR_LEN];
        ibutton_key_to_str(key, str);
        ESP_LOGI(TAG, "added %s%s%s", str, name[0] ? " as " : "", name);
    } else {
        ESP_LOGW(TAG, "add failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t keydb_get(const ibutton_key_t *key, keydb_entry_t *out)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(key != NULL && out != NULL, ESP_ERR_INVALID_ARG, TAG, "bad args");

    char id[ID_LEN + 1];
    make_id(key, id);

    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = read_record(id, out);
    xSemaphoreGive(s.lock);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
}

bool keydb_contains(const ibutton_key_t *key)
{
    if (!s.ready || key == NULL) {
        return false;
    }
    char id[ID_LEN + 1];
    make_id(key, id);

    xSemaphoreTake(s.lock, portMAX_DELAY);
    size_t len = 0;
    esp_err_t err = nvs_get_blob(s.keys, id, NULL, &len);
    xSemaphoreGive(s.lock);
    return err == ESP_OK;
}

esp_err_t keydb_set_name(const ibutton_key_t *key, const char *name)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "bad key");
    if (name == NULL) {
        name = "";
    }
    ESP_RETURN_ON_FALSE(keydb_name_valid(name), ESP_ERR_INVALID_ARG, TAG, "bad name");

    char id[ID_LEN + 1];
    make_id(key, id);

    xSemaphoreTake(s.lock, portMAX_DELAY);
    keydb_entry_t cur;
    esp_err_t err = read_record(id, &cur);
    if (err == ESP_OK) {
        err = write_record(id, &cur.key, cur.seq, name);
    }
    xSemaphoreGive(s.lock);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t keydb_remove(const ibutton_key_t *key)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "bad key");

    char id[ID_LEN + 1];
    make_id(key, id);

    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = nvs_erase_key(s.keys, id);
    if (err == ESP_OK) {
        err = nvs_commit(s.keys);
        if (s.count > 0) {
            s.count--;
        }
    }
    xSemaphoreGive(s.lock);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t keydb_clear(void)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = nvs_erase_all(s.keys);
    if (err == ESP_OK) {
        err = nvs_commit(s.keys);
    }
    if (err == ESP_OK) {
        s.count = 0;
    }
    xSemaphoreGive(s.lock);

    ESP_LOGW(TAG, "database cleared (%s)", esp_err_to_name(err));
    return err;
}

esp_err_t keydb_iterate(keydb_iter_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "no callback");

    keydb_entry_t entry;
    nvs_iterator_t it = NULL;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = nvs_entry_find(KEYDB_PARTITION, NS_KEYS, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (read_record(info.key, &entry) == ESP_OK && !cb(&entry, ctx)) {
            break;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    xSemaphoreGive(s.lock);

    return (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_OK) ? ESP_OK : err;
}

void keydb_get_stats(keydb_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s.ready) {
        return;
    }
    nvs_stats_t st;
    if (nvs_get_stats(KEYDB_PARTITION, &st) == ESP_OK) {
        out->used_entries = st.used_entries;
        out->free_entries = st.free_entries;
        out->total_entries = st.total_entries;
    }
    out->count = s.count;
}
