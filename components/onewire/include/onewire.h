/**
 * @file onewire.h
 * @brief Minimal bit-banged 1-Wire master.
 *
 * The bus is driven through an open-drain GPIO. Timing-critical slots are
 * executed with interrupts disabled so that Wi-Fi/RTOS activity cannot
 * stretch them. Besides the standard slots, the driver exposes a raw
 * "pulse" primitive which RW1990-style writable keys need for programming.
 *
 * The driver is not thread-safe by itself; the caller (see ibutton
 * component) serialises access to the bus.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 1-Wire ROM commands used in this project. */
#define ONEWIRE_CMD_READ_ROM   0x33
#define ONEWIRE_CMD_SKIP_ROM   0xCC
#define ONEWIRE_CMD_MATCH_ROM  0x55
#define ONEWIRE_CMD_SEARCH_ROM 0xF0

/**
 * Slot timing set, microseconds. Standard 1-Wire devices follow Maxim's
 * recommended values; some rewritable clones (TM01A/TM01C) answer the reset
 * later and release read slots earlier than DS1990A, so they get their own
 * set. Presence is detected by polling the line during `presence_window_us`
 * rather than by a single sample, which covers both families.
 */
typedef struct {
    uint16_t reset_low_us;       /**< Master reset pulse.                                 */
    uint16_t presence_window_us; /**< After release: keep looking for a presence pulse.   */
    uint16_t reset_tail_us;      /**< Minimum bus-high time after release (recovery).     */
    uint8_t  write1_low_us;
    uint8_t  write1_rec_us;
    uint8_t  write0_low_us;
    uint8_t  write0_rec_us;
    uint8_t  read_low_us;
    uint8_t  read_sample_us;     /**< Sample point measured from the falling edge.        */
    uint8_t  read_rec_us;
} onewire_timings_t;

extern const onewire_timings_t ONEWIRE_TIMINGS_STANDARD; /**< DS1990A, RW1990, TM2004 ... */
extern const onewire_timings_t ONEWIRE_TIMINGS_TM01;     /**< TM01A / TM01C blanks.       */

typedef struct {
    gpio_num_t pin;
    portMUX_TYPE lock;
    const onewire_timings_t *timings;
} onewire_bus_t;

/**
 * @brief Configure the GPIO as open-drain with pull-up and initialise the bus object.
 *
 * An external 2.2k..4.7k pull-up to 3.3 V is still required: the internal
 * pull-up alone is too weak for reliable operation with iButton contacts.
 */
esp_err_t onewire_init(onewire_bus_t *bus, gpio_num_t pin);

/** @brief Select the slot timing set (ONEWIRE_TIMINGS_STANDARD after init). */
void onewire_set_timings(onewire_bus_t *bus, const onewire_timings_t *timings);
const onewire_timings_t *onewire_get_timings(const onewire_bus_t *bus);

/**
 * @brief Issue a reset pulse and sample the presence pulse.
 * @return true if at least one device answered with a presence pulse.
 */
bool onewire_reset(onewire_bus_t *bus);

void    onewire_write_bit(onewire_bus_t *bus, bool bit);
bool    onewire_read_bit(onewire_bus_t *bus);
void    onewire_write_byte(onewire_bus_t *bus, uint8_t value);
uint8_t onewire_read_byte(onewire_bus_t *bus);
void    onewire_write_bytes(onewire_bus_t *bus, const uint8_t *data, size_t len);
void    onewire_read_bytes(onewire_bus_t *bus, uint8_t *data, size_t len);

/**
 * @brief Drive the bus low for @p low_us microseconds, then release it.
 *
 * Used for programming pulses of RW1990 keys. Executed atomically.
 */
void onewire_pulse_low(onewire_bus_t *bus, uint32_t low_us);

/** @brief Check whether the bus is idle (pulled high) - detects shorted contacts. */
bool onewire_is_idle(onewire_bus_t *bus);

/** Pad probe results, each the level read in that configuration. */
typedef struct {
    int input_floating;  /**< Input, no pulls: what the external circuit holds.    */
    int input_pullup;    /**< Input + internal ~45k pull-up.                      */
    int driven_high;     /**< Push-pull high for 1 ms: 0 here = hard short/dead pad. */
    int od_released;     /**< Normal open-drain idle, as used for 1-Wire.         */
} onewire_pad_probe_t;

/**
 * @brief Exercise the pad in several modes to tell wiring faults apart
 *        (bring-up aid). Restores the open-drain configuration afterwards.
 */
void onewire_pad_probe(onewire_bus_t *bus, onewire_pad_probe_t *out);

/** @brief Dallas/Maxim CRC-8 (polynomial 0x31, reflected), as used in ROM codes. */
uint8_t onewire_crc8(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
