/**
 * @file ibutton.h
 * @brief iButton (DS1990A) reading and RW1990 programming service.
 *
 * The service owns the 1-Wire bus. A low-priority background task keeps
 * polling the contact pad so the UI can show the key currently attached;
 * write/verify operations take the bus mutex and therefore pause polling.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IBUTTON_ROM_LEN       8
#define IBUTTON_FAMILY_DS1990 0x01

/** Length of a printable ROM code "01AABBCCDDEEFF12" plus terminator. */
#define IBUTTON_ROM_STR_LEN (IBUTTON_ROM_LEN * 2 + 1)

/** Writable key families. */
typedef enum {
    IBUTTON_WRITE_RW1990_V1 = 0, /**< RW1990.1: 0xD1 unlock, inverted data */
    IBUTTON_WRITE_RW1990_V2,     /**< RW1990.2: 0x1D unlock, direct data   */
    IBUTTON_WRITE_VARIANT_MAX,
} ibutton_write_variant_t;

/** ROM code as sent on the bus: [0] = family, [1..6] = serial, [7] = CRC-8. */
typedef struct {
    uint8_t rom[IBUTTON_ROM_LEN];
} ibutton_key_t;

/** Snapshot of the reader state maintained by the polling task. */
typedef struct {
    bool present;       /**< A device answered the last reset pulse. */
    bool crc_ok;        /**< ROM CRC of the last read matched.       */
    bool bus_shorted;   /**< Data line held low: contacts shorted.   */
    ibutton_key_t key;  /**< Last ROM read while present.            */
    int64_t updated_us; /**< esp_timer timestamp of the last change. */
} ibutton_reader_state_t;

/** Result of a write operation, detailed enough for the UI to explain failures. */
typedef enum {
    IBUTTON_WRITE_OK = 0,
    IBUTTON_WRITE_ERR_NO_DEVICE,   /**< No presence pulse before programming.    */
    IBUTTON_WRITE_ERR_BUS_SHORTED, /**< Data line stuck low.                     */
    IBUTTON_WRITE_ERR_VERIFY,      /**< Programming finished, read-back differs. */
    IBUTTON_WRITE_ERR_BAD_CRC,     /**< Requested ROM has invalid CRC.           */
    IBUTTON_WRITE_ERR_BUSY,        /**< Bus busy (another operation running).    */
    IBUTTON_WRITE_ERR_INVALID_ARG,
    IBUTTON_WRITE_ERR_TIMEOUT,     /**< Armed write: no blank presented in time. */
} ibutton_write_result_t;

/** Lifecycle of an armed ("wait for the blank") write. */
typedef enum {
    IBUTTON_JOB_IDLE = 0,
    IBUTTON_JOB_WAITING, /**< Armed: the poll task is watching for a blank.       */
    IBUTTON_JOB_WRITING, /**< A blank was seen; programming in progress (~1 s).    */
    IBUTTON_JOB_DONE,    /**< Finished; `result` is valid until the next arm/cancel.*/
} ibutton_job_state_t;

typedef struct {
    ibutton_job_state_t state;
    uint32_t id;                     /**< Incremented on every arm, lets a client tell jobs apart. */
    ibutton_key_t key;
    ibutton_write_variant_t variant;
    int64_t deadline_us;             /**< esp_timer time after which WAITING turns into a timeout. */
    ibutton_write_result_t result;   /**< Valid in IBUTTON_JOB_DONE.                              */
} ibutton_write_job_t;

typedef struct {
    gpio_num_t pin;
    uint32_t poll_interval_ms; /**< 0 disables background polling. */
} ibutton_config_t;

/** Callback fired from the poll task when a key is attached or removed. */
typedef void (*ibutton_event_cb_t)(const ibutton_reader_state_t *state, void *ctx);

/** Callback fired from the poll task when an armed write finishes (any result). */
typedef void (*ibutton_job_cb_t)(const ibutton_write_job_t *job, void *ctx);

esp_err_t ibutton_init(const ibutton_config_t *cfg);
void      ibutton_set_event_callback(ibutton_event_cb_t cb, void *ctx);
void      ibutton_set_job_callback(ibutton_job_cb_t cb, void *ctx);

/** @brief Copy the current reader snapshot. Thread-safe. */
void ibutton_get_state(ibutton_reader_state_t *out);

/**
 * @brief Read the ROM of the attached key immediately (bypasses poll interval).
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no device, ESP_ERR_INVALID_CRC on CRC error,
 *         ESP_ERR_INVALID_STATE if the bus is shorted, ESP_ERR_TIMEOUT if bus is busy.
 */
esp_err_t ibutton_read(ibutton_key_t *out);

/**
 * @brief Program @p key into an attached RW1990 blank and verify by reading it back.
 *
 * Blocks for ~1 s (64 programming pulses of 10 ms each plus overhead).
 */
ibutton_write_result_t ibutton_write(const ibutton_key_t *key, ibutton_write_variant_t variant);

/**
 * @brief Arm a write: the poll task programs @p key into the next blank that
 *        stays on the contacts for two consecutive polls, or gives up after
 *        @p timeout_ms with IBUTTON_WRITE_ERR_TIMEOUT.
 *
 * Returns immediately; track progress with ibutton_write_job_get() or the
 * job callback. Only one job exists at a time.
 * @return ESP_ERR_INVALID_STATE if a job is already waiting or writing,
 *         ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_CRC for a bad request,
 *         ESP_ERR_NOT_SUPPORTED when background polling is disabled.
 */
esp_err_t ibutton_write_arm(const ibutton_key_t *key, ibutton_write_variant_t variant, uint32_t timeout_ms);

/**
 * @brief Drop a waiting job or acknowledge a finished one.
 * @return ESP_ERR_INVALID_STATE while programming is in progress (cannot be interrupted).
 */
esp_err_t ibutton_write_cancel(void);

/** @brief Copy the current job snapshot. Thread-safe. */
void ibutton_write_job_get(ibutton_write_job_t *out);

/**
 * @brief Check that the attached key answers correctly and carries @p expected.
 *
 * Performs several consecutive reads to catch flaky contacts / marginal keys.
 * @param[out] actual    ROM actually read on the last attempt (may be NULL).
 * @param[out] reads_ok  Number of successful matching reads (may be NULL).
 * @param      reads_total Number of read attempts to perform.
 * @return ESP_OK if all reads succeeded and matched, ESP_ERR_INVALID_RESPONSE on mismatch,
 *         or the error codes of ibutton_read().
 */
esp_err_t ibutton_verify(const ibutton_key_t *expected, ibutton_key_t *actual, int *reads_ok, int reads_total);

/* ---- Helpers ---------------------------------------------------------- */

bool ibutton_key_crc_ok(const ibutton_key_t *key);
void ibutton_key_fix_crc(ibutton_key_t *key);
bool ibutton_key_equal(const ibutton_key_t *a, const ibutton_key_t *b);

/** @brief Format as 16 upper-case hex digits, bus order (family first). */
void ibutton_key_to_str(const ibutton_key_t *key, char out[IBUTTON_ROM_STR_LEN]);

/**
 * @brief Parse a ROM code from text. Accepts 16 hex digits with optional
 *        spaces, colons or dashes between bytes.
 */
esp_err_t ibutton_key_from_str(const char *str, ibutton_key_t *out);

const char *ibutton_write_result_str(ibutton_write_result_t r);
const char *ibutton_write_variant_str(ibutton_write_variant_t v);
esp_err_t   ibutton_write_variant_from_str(const char *s, ibutton_write_variant_t *out);

#ifdef __cplusplus
}
#endif
