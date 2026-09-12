#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_RW_STATUS_LED_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "led_strip.h"

static const char *TAG = "status_led";

#define TICK_MS     50
#define TASK_STACK  2048
#define TASK_PRIO   3
#define BRIGHT_LOW  8   /* Steady indications are intentionally dim. */
#define BRIGHT_HIGH 40

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/** One flash pattern: colour on for on_ticks, off for off_ticks, repeated `count` times. */
typedef struct {
    rgb_t colour;
    uint8_t on_ticks;
    uint8_t off_ticks;
    uint8_t count;
} flash_pattern_t;

static const flash_pattern_t FLASH_PATTERNS[] = {
    [STATUS_LED_FLASH_KEY_SEEN] = {{0, BRIGHT_HIGH, BRIGHT_HIGH}, 2, 1, 1},
    [STATUS_LED_FLASH_SUCCESS] = {{0, BRIGHT_HIGH, 0}, 3, 2, 2},
    [STATUS_LED_FLASH_ERROR] = {{BRIGHT_HIGH, 0, 0}, 3, 2, 3},
};

static struct {
    led_strip_handle_t strip;
    QueueHandle_t flash_queue;
    volatile status_led_mode_t mode;
} s;

static rgb_t mode_colour(status_led_mode_t mode, uint32_t tick)
{
    switch (mode) {
    case STATUS_LED_MODE_AP:
        /* 1 s period, 20 % duty. */
        return (tick % 20) < 4 ? (rgb_t){0, 0, BRIGHT_HIGH} : (rgb_t){0, 0, 0};
    case STATUS_LED_MODE_CONNECTING:
        /* 300 ms period. */
        return (tick % 6) < 3 ? (rgb_t){BRIGHT_HIGH, BRIGHT_HIGH / 3, 0} : (rgb_t){0, 0, 0};
    case STATUS_LED_MODE_CONNECTED:
        return (rgb_t){0, BRIGHT_LOW, 0};
    case STATUS_LED_MODE_BOOT:
    default:
        return (rgb_t){BRIGHT_LOW, BRIGHT_LOW, BRIGHT_LOW};
    }
}

static void show(rgb_t c)
{
    led_strip_set_pixel(s.strip, 0, c.r, c.g, c.b);
    led_strip_refresh(s.strip);
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    status_led_flash_t kind;

    for (;;) {
        if (xQueueReceive(s.flash_queue, &kind, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            const flash_pattern_t *p = &FLASH_PATTERNS[kind];
            for (int i = 0; i < p->count; i++) {
                show(p->colour);
                vTaskDelay(pdMS_TO_TICKS(TICK_MS * p->on_ticks));
                show((rgb_t){0, 0, 0});
                vTaskDelay(pdMS_TO_TICKS(TICK_MS * p->off_ticks));
            }
        }
        show(mode_colour(s.mode, tick++));
    }
}

esp_err_t status_led_init(void)
{
    ESP_RETURN_ON_FALSE(s.strip == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_RW_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s.strip), TAG, "led_strip init");

    s.flash_queue = xQueueCreate(4, sizeof(status_led_flash_t));
    ESP_RETURN_ON_FALSE(s.flash_queue != NULL, ESP_ERR_NO_MEM, TAG, "queue alloc failed");

    s.mode = STATUS_LED_MODE_BOOT;
    BaseType_t ok = xTaskCreate(led_task, "status_led", TASK_STACK, NULL, TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task alloc failed");

    ESP_LOGI(TAG, "status LED on GPIO%d", CONFIG_RW_STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set_mode(status_led_mode_t mode)
{
    s.mode = mode;
}

void status_led_flash(status_led_flash_t kind)
{
    if (s.flash_queue != NULL) {
        xQueueSend(s.flash_queue, &kind, 0);
    }
}

#else /* CONFIG_RW_STATUS_LED_ENABLE */

esp_err_t status_led_init(void)
{
    return ESP_OK;
}

void status_led_set_mode(status_led_mode_t mode)
{
    (void)mode;
}

void status_led_flash(status_led_flash_t kind)
{
    (void)kind;
}

#endif /* CONFIG_RW_STATUS_LED_ENABLE */
