/**
 * @file keydb.h
 * @brief Persistent database of known iButton keys.
 *
 * Lives in its own NVS partition ("keydb") so it neither competes with
 * Wi-Fi settings for space nor is lost when the main NVS is erased. Every
 * key is one NVS blob addressed by family+serial, which makes uniqueness
 * a property of the storage itself: adding an existing key is a no-op.
 *
 * All functions are thread-safe.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "ibutton.h"

#ifdef __cplusplus
extern "C" {
#endif

/** NVS partition label; must match partitions.csv. */
#define KEYDB_PARTITION "keydb"

/** Names are limited in Unicode code points and, separately, in UTF-8 bytes. */
#define KEYDB_NAME_MAX_CHARS 120
#define KEYDB_NAME_MAX_BYTES (KEYDB_NAME_MAX_CHARS * 3)

typedef struct {
    ibutton_key_t key;
    uint32_t seq;                          /**< Monotonic insertion counter, for stable ordering. */
    char name[KEYDB_NAME_MAX_BYTES + 1];   /**< UTF-8, may be empty. */
} keydb_entry_t;

typedef struct {
    size_t count;          /**< Keys stored. */
    size_t used_entries;   /**< NVS entries occupied (32 bytes each). */
    size_t free_entries;   /**< NVS entries still available. */
    size_t total_entries;
} keydb_stats_t;

/** @brief Iteration callback; return false to stop early. */
typedef bool (*keydb_iter_cb_t)(const keydb_entry_t *entry, void *ctx);

/** @brief Mount the partition and count stored keys. */
esp_err_t keydb_init(void);

/**
 * @brief Insert a key unless it is already stored.
 * @param name   Optional (NULL or "" for unnamed). Validated with keydb_name_valid().
 * @param added  Set to true if a new record was written, false if the key already existed.
 * @return ESP_ERR_INVALID_ARG on bad name / CRC, ESP_ERR_NO_MEM when the partition is full.
 */
esp_err_t keydb_add(const ibutton_key_t *key, const char *name, bool *added);

/** @return ESP_OK and the record, or ESP_ERR_NOT_FOUND. */
esp_err_t keydb_get(const ibutton_key_t *key, keydb_entry_t *out);
bool      keydb_contains(const ibutton_key_t *key);

esp_err_t keydb_set_name(const ibutton_key_t *key, const char *name);
esp_err_t keydb_remove(const ibutton_key_t *key);
esp_err_t keydb_clear(void);

/**
 * @brief Visit every stored key (unordered). The database is locked for the
 *        whole walk, so keep the callback quick or accept blocking writers.
 */
esp_err_t keydb_iterate(keydb_iter_cb_t cb, void *ctx);

void keydb_get_stats(keydb_stats_t *out);

/**
 * @brief Check a name: valid UTF-8, no control characters, at most
 *        KEYDB_NAME_MAX_CHARS code points and KEYDB_NAME_MAX_BYTES bytes.
 */
bool keydb_name_valid(const char *name);

#ifdef __cplusplus
}
#endif
