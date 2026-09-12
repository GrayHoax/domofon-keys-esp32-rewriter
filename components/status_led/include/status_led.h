/**
 * @file status_led.h
 * @brief Single addressable RGB LED (WS2812, on-board on ESP32-C6 dev kits) used as a status indicator.
 *
 * A persistent "mode" colour/pattern reflects the network state; short
 * "flash" notifications overlay it for key events and then fall back.
 * All functions are safe to call from any task; the component is a no-op
 * when CONFIG_RW_STATUS_LED_ENABLE is off.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_MODE_BOOT = 0,      /**< Dim white.                    */
    STATUS_LED_MODE_AP,            /**< Slow blue blink: own network. */
    STATUS_LED_MODE_CONNECTING,    /**< Fast amber blink.             */
    STATUS_LED_MODE_CONNECTED,     /**< Steady dim green.             */
} status_led_mode_t;

typedef enum {
    STATUS_LED_FLASH_KEY_SEEN = 0, /**< Short cyan: key detected.        */
    STATUS_LED_FLASH_SUCCESS,      /**< Green x2: write/verify ok.        */
    STATUS_LED_FLASH_ERROR,        /**< Red x3: operation failed.         */
} status_led_flash_t;

esp_err_t status_led_init(void);
void      status_led_set_mode(status_led_mode_t mode);
void      status_led_flash(status_led_flash_t kind);

#ifdef __cplusplus
}
#endif
