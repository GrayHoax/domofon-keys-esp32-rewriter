/**
 * @file main.c
 * @brief RW1990 / DS1990A key programmer for ESP32-C6.
 *
 * Boot sequence:
 *   1. NVS (Wi-Fi credentials) and the key database partition
 *   2. Status LED
 *   3. 1-Wire reader service (every clean read is stored in the database)
 *   4. Wi-Fi manager (station with AP fallback) + captive DNS glue
 *   5. HTTP server with the web UI
 */
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_netif_ip_addr.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "ibutton.h"
#include "activekey.h"
#include "keydb.h"
#include "wifi_manager.h"
#include "captive_dns.h"
#include "web_server.h"
#include "status_led.h"

static const char *TAG = "main";

/* ------------------------------------------------------------------------- */
/* Glue between modules                                                       */
/* ------------------------------------------------------------------------- */

/** The LED mirrors the network state machine, not individual events. */
static status_led_mode_t led_mode_for(const wifi_mgr_status_t *st)
{
    switch (st->state) {
    case WIFI_MGR_STATE_CONNECTED:
        return STATUS_LED_MODE_CONNECTED;
    case WIFI_MGR_STATE_AP_ONLY:
        return STATUS_LED_MODE_AP;
    case WIFI_MGR_STATE_AP_FALLBACK:
        return STATUS_LED_MODE_AP_FALLBACK;
    case WIFI_MGR_STATE_CONNECTING:
        return STATUS_LED_MODE_CONNECTING;
    case WIFI_MGR_STATE_IDLE:
    default:
        return STATUS_LED_MODE_BOOT;
    }
}

static void on_wifi_mgr_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch ((wifi_mgr_event_id_t)id) {
    case WIFI_MGR_EVENT_AP_STARTED:
        captive_dns_start(*(const esp_ip4_addr_t *)data);
        break;
    case WIFI_MGR_EVENT_AP_STOPPED:
        captive_dns_stop();
        break;
    default:
        break;
    }

    wifi_mgr_status_t st;
    wifi_manager_get_status(&st);
    status_led_set_mode(led_mode_for(&st));
}

/** Every key that reads cleanly is recorded; the database rejects duplicates itself. */
static void on_key_event(const ibutton_reader_state_t *state, void *ctx)
{
    (void)ctx;
    if (!state->present || !state->crc_ok) {
        return;
    }
    status_led_flash(STATUS_LED_FLASH_KEY_SEEN);

    /* The database holds 1-Wire ROMs only; Cyfral/Metakom codes are just shown. */
    if (state->active_proto != ACTIVEKEY_PROTO_NONE) {
        return;
    }

    /* While a write is armed the key on the pad is a blank about to be
     * overwritten; its factory ID would only litter the database. */
    ibutton_write_job_t job;
    ibutton_write_job_get(&job);
    if (job.state == IBUTTON_JOB_WAITING || job.state == IBUTTON_JOB_WRITING) {
        return;
    }

    bool added = false;
    esp_err_t err = keydb_add(&state->key, "", &added);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "key not saved: %s", esp_err_to_name(err));
    }
}

/** Outcome of an armed write, mirrored on the LED like a direct one. */
static void on_write_job_done(const ibutton_write_job_t *job, void *ctx)
{
    (void)ctx;
    if (job->result == IBUTTON_WRITE_OK) {
        status_led_flash(STATUS_LED_FLASH_SUCCESS);
        /* The read-back happened while the job was still "writing" and was
         * therefore skipped by on_key_event(); record the new key now. */
        bool added = false;
        esp_err_t err = keydb_add(&job->key, "", &added);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "written key not saved: %s", esp_err_to_name(err));
        }
    } else if (job->result != IBUTTON_WRITE_ERR_TIMEOUT) {
        status_led_flash(STATUS_LED_FLASH_ERROR);
    }
}

/* ------------------------------------------------------------------------- */
/* Initialisation                                                             */
/* ------------------------------------------------------------------------- */

/**
 * One look at the idle key line right after the drivers are up. With
 * nothing attached the pull-up must read high on the digital input and
 * near full scale on the ADC; anything else is wiring or a pin conflict,
 * and the log line says which.
 */
static void line_self_test(void)
{
    int level = gpio_get_level(CONFIG_RW_ONEWIRE_GPIO);
    activekey_result_t adc = {0};
    bool have_adc = activekey_available() && ibutton_active_probe(&adc) != ESP_ERR_TIMEOUT;

    if (have_adc) {
        ESP_LOGI(TAG, "line self-test: GPIO%d reads %s, ADC idle %u..%u of 4095", CONFIG_RW_ONEWIRE_GPIO,
                 level ? "HIGH" : "LOW", adc.adc_min, adc.adc_max);
    } else {
        ESP_LOGI(TAG, "line self-test: GPIO%d reads %s", CONFIG_RW_ONEWIRE_GPIO, level ? "HIGH" : "LOW");
    }
    if (level == 0 && have_adc && adc.adc_max > 3000) {
        ESP_LOGE(TAG, "line self-test: ADC sees the line high but the digital input reads low - "
                      "GPIO%d input path is blocked by the analog configuration", CONFIG_RW_ONEWIRE_GPIO);
    } else if (level == 0) {
        ESP_LOGW(TAG, "line self-test: data line is low with no key - check the pull-up to 3V3 and the wiring");
    }
}

static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition is stale, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "RW1990 programmer starting");

    ESP_ERROR_CHECK(nvs_init());
    ESP_ERROR_CHECK(keydb_init());
    ESP_ERROR_CHECK(status_led_init());

    /* The ADC driver floats its pin; init it before the 1-Wire driver so a
     * shared data/sense pin ends up configured as open-drain. Optional. */
    esp_err_t sense_err = activekey_init(CONFIG_RW_ANALOG_SENSE_GPIO);
    if (sense_err != ESP_OK) {
        ESP_LOGW(TAG, "Cyfral/Metakom sensing unavailable: %s", esp_err_to_name(sense_err));
    }

    const ibutton_config_t reader_cfg = {
        .pin = CONFIG_RW_ONEWIRE_GPIO,
        .poll_interval_ms = CONFIG_RW_KEY_POLL_INTERVAL_MS,
    };
    ESP_ERROR_CHECK(ibutton_init(&reader_cfg));
    ibutton_set_event_callback(on_key_event, NULL);
    ibutton_set_job_callback(on_write_job_done, NULL);
    line_self_test();

    /* The default event loop is created inside wifi_manager_init(); register
     * our handler first so the initial AP_STARTED event is not missed. */
    esp_err_t err = esp_event_loop_create_default();
    ESP_ERROR_CHECK(err == ESP_ERR_INVALID_STATE ? ESP_OK : err);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_MGR_EVENT, ESP_EVENT_ANY_ID, on_wifi_mgr_event, NULL));

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "ready; free heap %" PRIu32 " bytes", esp_get_free_heap_size());
}
