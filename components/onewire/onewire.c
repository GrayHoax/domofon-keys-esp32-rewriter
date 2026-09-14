#include "onewire.h"

#include "esp_rom_sys.h"
#include "esp_check.h"

static const char *TAG = "onewire";

/* Maxim AN126 "recommended" standard-speed values. The read sample sits a
 * little before the 15 us limit: a genuine slave holds a zero for >= 15 us,
 * and sampling early tolerates clones that let go sooner. */
const onewire_timings_t ONEWIRE_TIMINGS_STANDARD = {
    .reset_low_us = 480,
    .presence_window_us = 250,
    .reset_tail_us = 480,
    .write1_low_us = 6,
    .write1_rec_us = 64,
    .write0_low_us = 60,
    .write0_rec_us = 10,
    .read_low_us = 6,
    .read_sample_us = 13,
    .read_rec_us = 52,
};

/* TM01A/TM01C: longer reset, presence arrives ~100-150 us after release,
 * read slots sampled at 10 us. Values follow the Flipper Zero tm01x set. */
const onewire_timings_t ONEWIRE_TIMINGS_TM01 = {
    .reset_low_us = 740,
    .presence_window_us = 250,
    .reset_tail_us = 480,
    .write1_low_us = 5,
    .write1_rec_us = 80,
    .write0_low_us = 70,
    .write0_rec_us = 10,
    .read_low_us = 5,
    .read_sample_us = 10,
    .read_rec_us = 70,
};

#define PRESENCE_POLL_US     2
#define PRESENCE_END_MAX_US  600 /* Presence pulse still low after this: shorted. */

static inline void bus_drive_low(const onewire_bus_t *bus)
{
    gpio_set_level(bus->pin, 0);
}

static inline void bus_release(const onewire_bus_t *bus)
{
    gpio_set_level(bus->pin, 1);
}

static inline int bus_level(const onewire_bus_t *bus)
{
    return gpio_get_level(bus->pin);
}

static esp_err_t pad_config(gpio_num_t pin, gpio_mode_t mode, bool pullup)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t onewire_init(onewire_bus_t *bus, gpio_num_t pin)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(pin), ESP_ERR_INVALID_ARG, TAG, "invalid GPIO %d", pin);

    bus->pin = pin;
    bus->timings = &ONEWIRE_TIMINGS_STANDARD;
    portMUX_INITIALIZE(&bus->lock);

    ESP_RETURN_ON_ERROR(pad_config(pin, GPIO_MODE_INPUT_OUTPUT_OD, true), TAG, "gpio_config failed");
    bus_release(bus);

    ESP_LOGI(TAG, "1-Wire bus on GPIO%d", pin);
    return ESP_OK;
}

void onewire_pad_probe(onewire_bus_t *bus, onewire_pad_probe_t *out)
{
    const gpio_num_t pin = bus->pin;

    pad_config(pin, GPIO_MODE_INPUT, false);
    esp_rom_delay_us(1000);
    out->input_floating = bus_level(bus);

    pad_config(pin, GPIO_MODE_INPUT, true);
    esp_rom_delay_us(1000);
    out->input_pullup = bus_level(bus);

    pad_config(pin, GPIO_MODE_INPUT_OUTPUT, false);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(1000);
    out->driven_high = bus_level(bus);

    pad_config(pin, GPIO_MODE_INPUT_OUTPUT_OD, true);
    bus_release(bus);
    esp_rom_delay_us(1000);
    out->od_released = bus_level(bus);
}

void onewire_set_timings(onewire_bus_t *bus, const onewire_timings_t *timings)
{
    bus->timings = timings ? timings : &ONEWIRE_TIMINGS_STANDARD;
}

const onewire_timings_t *onewire_get_timings(const onewire_bus_t *bus)
{
    return bus->timings;
}

bool onewire_reset(onewire_bus_t *bus)
{
    const onewire_timings_t *t = bus->timings;
    bool released = false; /* Line seen high after our pulse: rules out a short. */
    bool presence = false;
    uint32_t elapsed = 0;  /* Microseconds since release.                        */

    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    esp_rom_delay_us(t->reset_low_us);
    bus_release(bus);

    /* A slave answers 15..60 us after release with a 60..240 us low pulse;
     * TM01-type blanks answer noticeably later. Polling the whole window
     * catches any of them without knowing which one is attached. */
    for (; elapsed < t->presence_window_us; elapsed += PRESENCE_POLL_US) {
        int level = bus_level(bus);
        if (!released) {
            released = (level != 0);
        } else if (level == 0) {
            presence = true;
            break;
        }
        esp_rom_delay_us(PRESENCE_POLL_US);
    }
    portEXIT_CRITICAL(&bus->lock);

    /* Let the presence pulse finish: it may last up to 240 us and, for a
     * late answer, end 300+ us after release. Only a line that is still
     * low well beyond that is shorted rather than a device. */
    if (presence) {
        while (bus_level(bus) == 0 && elapsed < PRESENCE_END_MAX_US) {
            esp_rom_delay_us(PRESENCE_POLL_US * 5);
            elapsed += PRESENCE_POLL_US * 5;
        }
        if (bus_level(bus) == 0) {
            return false;
        }
    }

    /* Recovery: the slave needs the bus high for reset_tail_us in total. */
    if (elapsed < t->reset_tail_us) {
        esp_rom_delay_us(t->reset_tail_us - elapsed);
    }
    return presence;
}

void onewire_write_bit(onewire_bus_t *bus, bool bit)
{
    const onewire_timings_t *t = bus->timings;

    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    if (bit) {
        esp_rom_delay_us(t->write1_low_us);
        bus_release(bus);
        esp_rom_delay_us(t->write1_rec_us);
    } else {
        esp_rom_delay_us(t->write0_low_us);
        bus_release(bus);
        esp_rom_delay_us(t->write0_rec_us);
    }
    portEXIT_CRITICAL(&bus->lock);
}

bool onewire_read_bit(onewire_bus_t *bus)
{
    const onewire_timings_t *t = bus->timings;
    bool bit;

    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    esp_rom_delay_us(t->read_low_us);
    bus_release(bus);
    esp_rom_delay_us(t->read_sample_us - t->read_low_us);
    bit = (bus_level(bus) != 0);
    esp_rom_delay_us(t->read_rec_us);
    portEXIT_CRITICAL(&bus->lock);

    return bit;
}

void onewire_write_byte(onewire_bus_t *bus, uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        onewire_write_bit(bus, value & 0x01);
        value >>= 1;
    }
}

uint8_t onewire_read_byte(onewire_bus_t *bus)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        value >>= 1;
        if (onewire_read_bit(bus)) {
            value |= 0x80;
        }
    }
    return value;
}

void onewire_write_bytes(onewire_bus_t *bus, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        onewire_write_byte(bus, data[i]);
    }
}

void onewire_read_bytes(onewire_bus_t *bus, uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        data[i] = onewire_read_byte(bus);
    }
}

void onewire_pulse_low(onewire_bus_t *bus, uint32_t low_us)
{
    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    esp_rom_delay_us(low_us);
    bus_release(bus);
    portEXIT_CRITICAL(&bus->lock);
}

bool onewire_is_idle(onewire_bus_t *bus)
{
    return bus_level(bus) != 0;
}

bool onewire_line_toggles(onewire_bus_t *bus, uint32_t window_us)
{
    bool seen_low = false, seen_high = false;
    for (uint32_t elapsed = 0; elapsed < window_us; elapsed += 5) {
        if (bus_level(bus)) {
            seen_high = true;
        } else {
            seen_low = true;
        }
        if (seen_low && seen_high) {
            return true;
        }
        esp_rom_delay_us(5);
    }
    return false;
}

uint8_t onewire_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    while (len--) {
        uint8_t inbyte = *data++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            inbyte >>= 1;
        }
    }
    return crc;
}
