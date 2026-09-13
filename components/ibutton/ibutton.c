#include "ibutton.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "onewire.h"

static const char *TAG = "ibutton";

/* RW1990 programming constants. */
#define RW1990_CMD_WRITE_ROM   0xD5
#define RW1990_V1_CMD_UNLOCK   0xD1
#define RW1990_V2_CMD_UNLOCK   0x1D
#define RW1990_PROG_PULSE_MS   10 /* Hold-off after each programmed bit.       */
#define RW1990_UNLOCK_PULSE_US 60 /* Long (logic 0) pulse for the unlock byte. */
#define RW1990_LOCK_PULSE_US   10 /* Short (logic 1) pulse for the lock byte.  */

#define BUS_MUTEX_TIMEOUT_MS 3000
#define POLL_TASK_STACK      4096 /* Event callback may write to NVS. */
#define POLL_TASK_PRIO       5

static struct {
    onewire_bus_t bus;
    SemaphoreHandle_t bus_mutex;
    SemaphoreHandle_t state_mutex;
    ibutton_reader_state_t state;
    ibutton_event_cb_t cb;
    void *cb_ctx;
    uint32_t poll_interval_ms;
    TaskHandle_t poll_task;
} s_ib;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

bool ibutton_key_crc_ok(const ibutton_key_t *key)
{
    return onewire_crc8(key->rom, IBUTTON_ROM_LEN - 1) == key->rom[IBUTTON_ROM_LEN - 1];
}

void ibutton_key_fix_crc(ibutton_key_t *key)
{
    key->rom[IBUTTON_ROM_LEN - 1] = onewire_crc8(key->rom, IBUTTON_ROM_LEN - 1);
}

bool ibutton_key_equal(const ibutton_key_t *a, const ibutton_key_t *b)
{
    return memcmp(a->rom, b->rom, IBUTTON_ROM_LEN) == 0;
}

void ibutton_key_to_str(const ibutton_key_t *key, char out[IBUTTON_ROM_STR_LEN])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < IBUTTON_ROM_LEN; i++) {
        out[i * 2] = hex[key->rom[i] >> 4];
        out[i * 2 + 1] = hex[key->rom[i] & 0x0F];
    }
    out[IBUTTON_ROM_LEN * 2] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

esp_err_t ibutton_key_from_str(const char *str, ibutton_key_t *out)
{
    if (str == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rom[IBUTTON_ROM_LEN];
    int nibbles = 0;

    for (const char *p = str; *p != '\0'; p++) {
        if (*p == ' ' || *p == ':' || *p == '-' || *p == '\t') {
            continue;
        }
        int v = hex_nibble(*p);
        if (v < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (nibbles >= IBUTTON_ROM_LEN * 2) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (nibbles % 2 == 0) {
            rom[nibbles / 2] = (uint8_t)(v << 4);
        } else {
            rom[nibbles / 2] |= (uint8_t)v;
        }
        nibbles++;
    }

    if (nibbles != IBUTTON_ROM_LEN * 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out->rom, rom, IBUTTON_ROM_LEN);
    return ESP_OK;
}

const char *ibutton_write_result_str(ibutton_write_result_t r)
{
    switch (r) {
    case IBUTTON_WRITE_OK:
        return "ok";
    case IBUTTON_WRITE_ERR_NO_DEVICE:
        return "no_device";
    case IBUTTON_WRITE_ERR_BUS_SHORTED:
        return "bus_shorted";
    case IBUTTON_WRITE_ERR_VERIFY:
        return "verify_failed";
    case IBUTTON_WRITE_ERR_BAD_CRC:
        return "bad_crc";
    case IBUTTON_WRITE_ERR_BUSY:
        return "busy";
    case IBUTTON_WRITE_ERR_INVALID_ARG:
        return "invalid_arg";
    default:
        return "unknown";
    }
}

const char *ibutton_write_variant_str(ibutton_write_variant_t v)
{
    switch (v) {
    case IBUTTON_WRITE_RW1990_V1:
        return "rw1990v1";
    case IBUTTON_WRITE_RW1990_V2:
        return "rw1990v2";
    default:
        return "unknown";
    }
}

esp_err_t ibutton_write_variant_from_str(const char *s, ibutton_write_variant_t *out)
{
    if (s == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int v = 0; v < IBUTTON_WRITE_VARIANT_MAX; v++) {
        if (strcasecmp(s, ibutton_write_variant_str((ibutton_write_variant_t)v)) == 0) {
            *out = (ibutton_write_variant_t)v;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------- */
/* Bus-level operations (caller holds bus_mutex)                              */
/* ------------------------------------------------------------------------- */

static esp_err_t bus_read_rom(ibutton_key_t *out)
{
    if (!onewire_is_idle(&s_ib.bus)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!onewire_reset(&s_ib.bus)) {
        return ESP_ERR_NOT_FOUND;
    }
    onewire_write_byte(&s_ib.bus, ONEWIRE_CMD_READ_ROM);
    onewire_read_bytes(&s_ib.bus, out->rom, IBUTTON_ROM_LEN);

    /* All 0xFF means the key was pulled off mid-transaction; all zero is a glitch. */
    bool all_ff = true, all_00 = true;
    for (int i = 0; i < IBUTTON_ROM_LEN; i++) {
        all_ff &= (out->rom[i] == 0xFF);
        all_00 &= (out->rom[i] == 0x00);
    }
    if (all_ff || all_00) {
        return ESP_ERR_NOT_FOUND;
    }
    return ibutton_key_crc_ok(out) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/**
 * Programs one byte, LSB first, with a 10 ms hold after each bit.
 * RW1990.1 expects inverted bits on the wire, RW1990.2 direct ones.
 */
static void rw1990_write_byte(uint8_t value, bool invert)
{
    for (int i = 0; i < 8; i++) {
        bool bit = value & 0x01;
        onewire_write_bit(&s_ib.bus, invert ? !bit : bit);
        vTaskDelay(pdMS_TO_TICKS(RW1990_PROG_PULSE_MS));
        value >>= 1;
    }
}

static ibutton_write_result_t rw1990_program(const ibutton_key_t *key, ibutton_write_variant_t variant)
{
    const bool v1 = (variant == IBUTTON_WRITE_RW1990_V1);
    const uint8_t unlock_cmd = v1 ? RW1990_V1_CMD_UNLOCK : RW1990_V2_CMD_UNLOCK;

    /* Step 1: enable write mode. V1 wants a "0" pulse, V2 wants a "1" pulse. */
    if (!onewire_reset(&s_ib.bus)) {
        return IBUTTON_WRITE_ERR_NO_DEVICE;
    }
    onewire_write_byte(&s_ib.bus, unlock_cmd);
    onewire_pulse_low(&s_ib.bus, v1 ? RW1990_UNLOCK_PULSE_US : RW1990_LOCK_PULSE_US);
    vTaskDelay(pdMS_TO_TICKS(RW1990_PROG_PULSE_MS));

    /* Step 2: write the ROM, one bit per programming cycle. */
    if (!onewire_reset(&s_ib.bus)) {
        return IBUTTON_WRITE_ERR_NO_DEVICE;
    }
    onewire_write_byte(&s_ib.bus, RW1990_CMD_WRITE_ROM);
    for (int i = 0; i < IBUTTON_ROM_LEN; i++) {
        rw1990_write_byte(key->rom[i], v1);
    }

    /* Step 3: lock the key again (opposite pulse polarity to step 1). */
    if (!onewire_reset(&s_ib.bus)) {
        return IBUTTON_WRITE_ERR_NO_DEVICE;
    }
    onewire_write_byte(&s_ib.bus, unlock_cmd);
    onewire_pulse_low(&s_ib.bus, v1 ? RW1990_LOCK_PULSE_US : RW1990_UNLOCK_PULSE_US);
    vTaskDelay(pdMS_TO_TICKS(RW1990_PROG_PULSE_MS));

    return IBUTTON_WRITE_OK;
}

/* ------------------------------------------------------------------------- */
/* State handling                                                             */
/* ------------------------------------------------------------------------- */

static void state_update(bool present, bool crc_ok, bool shorted, const ibutton_key_t *key)
{
    bool changed = false;
    ibutton_reader_state_t snapshot;

    xSemaphoreTake(s_ib.state_mutex, portMAX_DELAY);
    ibutton_reader_state_t *st = &s_ib.state;

    if (st->present != present || st->crc_ok != crc_ok || st->bus_shorted != shorted ||
        (present && key != NULL && !ibutton_key_equal(&st->key, key))) {
        st->present = present;
        st->crc_ok = crc_ok;
        st->bus_shorted = shorted;
        if (present && key != NULL) {
            st->key = *key;
        }
        st->updated_us = esp_timer_get_time();
        changed = true;
    }
    snapshot = *st;
    xSemaphoreGive(s_ib.state_mutex);

    if (changed) {
        if (present) {
            char str[IBUTTON_ROM_STR_LEN];
            ibutton_key_to_str(&snapshot.key, str);
            ESP_LOGI(TAG, "key attached: %s (crc %s)", str, crc_ok ? "ok" : "BAD");
        } else if (shorted) {
            ESP_LOGW(TAG, "bus shorted");
        } else {
            ESP_LOGI(TAG, "key removed");
        }
        if (s_ib.cb) {
            s_ib.cb(&snapshot, s_ib.cb_ctx);
        }
    }
}

static void apply_read_result(esp_err_t err, const ibutton_key_t *key)
{
    switch (err) {
    case ESP_OK:
        state_update(true, true, false, key);
        break;
    case ESP_ERR_INVALID_CRC:
        state_update(true, false, false, key);
        break;
    case ESP_ERR_INVALID_STATE:
        state_update(false, false, true, NULL);
        break;
    default:
        state_update(false, false, false, NULL);
        break;
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    ibutton_key_t key;

    for (;;) {
        if (xSemaphoreTake(s_ib.bus_mutex, pdMS_TO_TICKS(s_ib.poll_interval_ms)) == pdTRUE) {
            esp_err_t err = bus_read_rom(&key);
            xSemaphoreGive(s_ib.bus_mutex);
            apply_read_result(err, &key);
        }
        vTaskDelay(pdMS_TO_TICKS(s_ib.poll_interval_ms));
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t ibutton_init(const ibutton_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(s_ib.bus_mutex == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    ESP_RETURN_ON_ERROR(onewire_init(&s_ib.bus, cfg->pin), TAG, "bus init failed");

    s_ib.bus_mutex = xSemaphoreCreateMutex();
    s_ib.state_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ib.bus_mutex && s_ib.state_mutex, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    s_ib.poll_interval_ms = cfg->poll_interval_ms;
    if (s_ib.poll_interval_ms > 0) {
        BaseType_t ok = xTaskCreate(poll_task, "ibutton_poll", POLL_TASK_STACK, NULL, POLL_TASK_PRIO,
                                    &s_ib.poll_task);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "poll task alloc failed");
    }
    return ESP_OK;
}

void ibutton_set_event_callback(ibutton_event_cb_t cb, void *ctx)
{
    s_ib.cb = cb;
    s_ib.cb_ctx = ctx;
}

void ibutton_get_state(ibutton_reader_state_t *out)
{
    xSemaphoreTake(s_ib.state_mutex, portMAX_DELAY);
    *out = s_ib.state;
    xSemaphoreGive(s_ib.state_mutex);
}

esp_err_t ibutton_read(ibutton_key_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    if (xSemaphoreTake(s_ib.bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = bus_read_rom(out);
    xSemaphoreGive(s_ib.bus_mutex);

    apply_read_result(err, out);
    return err;
}

ibutton_write_result_t ibutton_write(const ibutton_key_t *key, ibutton_write_variant_t variant)
{
    if (key == NULL || variant >= IBUTTON_WRITE_VARIANT_MAX) {
        return IBUTTON_WRITE_ERR_INVALID_ARG;
    }
    if (!ibutton_key_crc_ok(key)) {
        return IBUTTON_WRITE_ERR_BAD_CRC;
    }

    char str[IBUTTON_ROM_STR_LEN];
    ibutton_key_to_str(key, str);
    ESP_LOGI(TAG, "writing %s as %s", str, ibutton_write_variant_str(variant));

    if (xSemaphoreTake(s_ib.bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return IBUTTON_WRITE_ERR_BUSY;
    }

    ibutton_write_result_t result;
    ibutton_key_t readback = {0};
    esp_err_t err;

    if (!onewire_is_idle(&s_ib.bus)) {
        result = IBUTTON_WRITE_ERR_BUS_SHORTED;
        goto out;
    }
    if (!onewire_reset(&s_ib.bus)) {
        result = IBUTTON_WRITE_ERR_NO_DEVICE;
        goto out;
    }

    result = rw1990_program(key, variant);
    if (result != IBUTTON_WRITE_OK) {
        goto out;
    }

    /* Give the key a moment to settle, then read back. */
    vTaskDelay(pdMS_TO_TICKS(50));
    err = bus_read_rom(&readback);
    if (err == ESP_ERR_NOT_FOUND) {
        result = IBUTTON_WRITE_ERR_NO_DEVICE;
    } else if (err == ESP_ERR_INVALID_STATE) {
        result = IBUTTON_WRITE_ERR_BUS_SHORTED;
    } else if (err != ESP_OK || !ibutton_key_equal(&readback, key)) {
        char got[IBUTTON_ROM_STR_LEN];
        ibutton_key_to_str(&readback, got);
        ESP_LOGE(TAG, "verify failed: wrote %s, read %s", str, got);
        result = IBUTTON_WRITE_ERR_VERIFY;
    }

out:
    xSemaphoreGive(s_ib.bus_mutex);

    if (result == IBUTTON_WRITE_OK) {
        ESP_LOGI(TAG, "write ok");
        apply_read_result(ESP_OK, &readback);
    } else {
        ESP_LOGW(TAG, "write failed: %s", ibutton_write_result_str(result));
    }
    return result;
}

esp_err_t ibutton_verify(const ibutton_key_t *expected, ibutton_key_t *actual, int *reads_ok, int reads_total)
{
    ESP_RETURN_ON_FALSE(expected != NULL && reads_total > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");

    if (xSemaphoreTake(s_ib.bus_mutex, pdMS_TO_TICKS(BUS_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t last_err = ESP_OK;
    int ok_count = 0;
    ibutton_key_t key = {0};

    for (int i = 0; i < reads_total; i++) {
        esp_err_t err = bus_read_rom(&key);
        if (err == ESP_OK && ibutton_key_equal(&key, expected)) {
            ok_count++;
        } else {
            last_err = (err == ESP_OK) ? ESP_ERR_INVALID_RESPONSE : err;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreGive(s_ib.bus_mutex);

    if (actual) {
        *actual = key;
    }
    if (reads_ok) {
        *reads_ok = ok_count;
    }
    return (ok_count == reads_total) ? ESP_OK : last_err;
}
