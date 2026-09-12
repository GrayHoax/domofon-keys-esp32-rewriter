#include "onewire.h"

#include "esp_rom_sys.h"
#include "esp_check.h"

static const char *TAG = "onewire";

/* Standard-speed 1-Wire timing (microseconds), Maxim AN126 "recommended" values. */
#define T_RESET_LOW     480
#define T_PRESENCE_WAIT 70
#define T_RESET_TAIL    410
#define T_WRITE1_LOW    6
#define T_WRITE1_REC    64
#define T_WRITE0_LOW    60
#define T_WRITE0_REC    10
#define T_READ_LOW      6
#define T_READ_SAMPLE   9
#define T_READ_REC      55

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

esp_err_t onewire_init(onewire_bus_t *bus, gpio_num_t pin)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(pin), ESP_ERR_INVALID_ARG, TAG, "invalid GPIO %d", pin);

    bus->pin = pin;
    portMUX_INITIALIZE(&bus->lock);

    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config failed");
    bus_release(bus);

    ESP_LOGI(TAG, "1-Wire bus on GPIO%d", pin);
    return ESP_OK;
}

bool onewire_reset(onewire_bus_t *bus)
{
    bool presence;

    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    esp_rom_delay_us(T_RESET_LOW);
    bus_release(bus);
    esp_rom_delay_us(T_PRESENCE_WAIT);
    presence = (bus_level(bus) == 0);
    portEXIT_CRITICAL(&bus->lock);

    /* Let the presence pulse finish; nothing time-critical here. */
    esp_rom_delay_us(T_RESET_TAIL);

    /* A bus that is still low after the reset sequence is shorted, not a device. */
    if (presence && bus_level(bus) == 0) {
        return false;
    }
    return presence;
}

void onewire_write_bit(onewire_bus_t *bus, bool bit)
{
    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    if (bit) {
        esp_rom_delay_us(T_WRITE1_LOW);
        bus_release(bus);
        esp_rom_delay_us(T_WRITE1_REC);
    } else {
        esp_rom_delay_us(T_WRITE0_LOW);
        bus_release(bus);
        esp_rom_delay_us(T_WRITE0_REC);
    }
    portEXIT_CRITICAL(&bus->lock);
}

bool onewire_read_bit(onewire_bus_t *bus)
{
    bool bit;

    portENTER_CRITICAL(&bus->lock);
    bus_drive_low(bus);
    esp_rom_delay_us(T_READ_LOW);
    bus_release(bus);
    esp_rom_delay_us(T_READ_SAMPLE);
    bit = (bus_level(bus) != 0);
    esp_rom_delay_us(T_READ_REC);
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
