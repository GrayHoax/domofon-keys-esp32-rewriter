#include "activekey.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "esp_check.h"
#include "soc/soc_caps.h"

static const char *TAG = "activekey";

/* Capture geometry. One Cyfral/Metakom frame is 36..40 bits of ~125 us,
 * i.e. about 5 ms; 25 ms holds several frames whatever the alignment. */
#define SAMPLE_RATE_HZ   SOC_ADC_SAMPLE_FREQ_THRES_HIGH
#define SAMPLE_US        (1000000 / SAMPLE_RATE_HZ)
#define CAPTURE_SAMPLES  2048
#define FRAME_SAMPLES    256
#define FRAME_BYTES      (FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define READ_TIMEOUT_MS  60

/* A static line (no key, or a Dallas key) shows only noise: skip decoding
 * below this peak-to-peak swing, ~0.12 V at 12 dB attenuation. */
#define MIN_SWING        200

/* Protocol constants, from the Flipper Zero decoders. */
#define CYFRAL_MAX_PERIOD_US    230
#define METAKOM_PERIOD_SAMPLES  10

typedef struct {
    bool level;
    uint16_t us;
} run_t;

static struct {
    adc_continuous_handle_t adc;
    uint8_t *raw;          /* One DMA frame.                */
    adc_continuous_data_t *parsed;
    uint16_t *samples;     /* CAPTURE_SAMPLES raw values.   */
    run_t *runs;           /* Up to CAPTURE_SAMPLES runs.   */
    int gpio;
} s;

/* ------------------------------------------------------------------------- */
/* Cyfral decoder                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    enum { CY_WAIT_START, CY_READ_NIBBLE, CY_READ_STOP } state;
    bool wait_front_low;
    uint32_t period;
    uint8_t nibble;
    uint8_t index;
    uint8_t bit_index;
    bool data_valid;
    uint16_t data;
} cyfral_dec_t;

static void cy_start(cyfral_dec_t *d)
{
    memset(d, 0, sizeof(*d));
    d->state = CY_WAIT_START;
    d->wait_front_low = true;
    d->data_valid = true;
}

/* A bit is a low run followed by a high run; the low share tells 0 from 1. */
static bool cy_process_bit(cyfral_dec_t *d, bool polarity, uint32_t len, bool *ready, bool *value)
{
    *ready = false;
    if (d->wait_front_low) {
        if (!polarity) {
            return false;
        }
        d->period += len;
        *ready = true;
        if (d->period > CYFRAL_MAX_PERIOD_US) {
            return false;
        }
        *value = !((d->period / 2) > len);
        d->wait_front_low = false;
    } else {
        if (polarity) {
            return false;
        }
        d->period = len;
        d->wait_front_low = true;
    }
    return true;
}

static bool cy_feed(cyfral_dec_t *d, bool level, uint32_t len)
{
    bool ready, value;

    if (!cy_process_bit(d, level, len, &ready, &value)) {
        cy_start(d);
        return false;
    }
    if (!ready) {
        return false;
    }

    switch (d->state) {
    case CY_WAIT_START:
        d->nibble = ((d->nibble << 1) | value) & 0x0F;
        if (d->nibble == 0x1) {
            d->nibble = 0;
            d->state = CY_READ_NIBBLE;
        }
        break;

    case CY_READ_NIBBLE:
        d->nibble = (d->nibble << 1) | value;
        if (++d->bit_index == 4) {
            switch (d->nibble) {
            case 0xE: d->data = (d->data << 2) | 0x3; break;
            case 0xD: d->data = (d->data << 2) | 0x2; break;
            case 0xB: d->data = (d->data << 2) | 0x1; break;
            case 0x7: d->data = (d->data << 2) | 0x0; break;
            default:  d->data_valid = false;          break;
            }
            d->nibble = 0;
            d->bit_index = 0;
            if (++d->index == 8) {
                d->state = CY_READ_STOP;
            }
        }
        break;

    case CY_READ_STOP:
        d->nibble = ((d->nibble << 1) | value) & 0x0F;
        if (++d->bit_index == 4) {
            if (d->nibble == 0x1 && d->data_valid) {
                return true;
            }
            cy_start(d);
        }
        break;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Metakom decoder                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    enum { MK_SYNC, MK_WAIT_START_BIT, MK_WAIT_START_WORD, MK_READ_WORD, MK_READ_STOP } state;
    bool wait_front_low;
    uint32_t period;
    uint32_t low_store;
    uint32_t period_samples[METAKOM_PERIOD_SAMPLES];
    uint8_t period_index;
    uint8_t tmp_data;
    uint8_t tmp_counter;
    uint8_t byte_index;
    uint32_t data;
} metakom_dec_t;

static void mk_start(metakom_dec_t *d)
{
    memset(d, 0, sizeof(*d));
    d->state = MK_SYNC;
    d->wait_front_low = true;
}

static bool mk_parity_even(uint8_t v)
{
    int ones = 0;
    for (int i = 0; i < 8; i++) {
        ones += (v >> i) & 1;
    }
    return (ones % 2) == 0;
}

/* Yields one (high, low) pair per falling edge. */
static bool mk_process_bit(metakom_dec_t *d, bool polarity, uint32_t len, uint32_t *high, uint32_t *low)
{
    if (d->wait_front_low) {
        if (!polarity) {
            *low = d->low_store;
            *high = len;
            d->wait_front_low = false;
            return true;
        }
    } else if (polarity) {
        d->low_store = len;
        d->wait_front_low = true;
    }
    return false;
}

static bool mk_feed(metakom_dec_t *d, bool level, uint32_t len)
{
    uint32_t high = 0, low = 0;

    if (!mk_process_bit(d, level, len, &high, &low)) {
        return false;
    }

    switch (d->state) {
    case MK_SYNC:
        d->period_samples[d->period_index++] = high + low;
        if (d->period_index == METAKOM_PERIOD_SAMPLES) {
            for (int i = 0; i < METAKOM_PERIOD_SAMPLES; i++) {
                d->period += d->period_samples[i];
            }
            d->period /= METAKOM_PERIOD_SAMPLES;
            d->state = MK_WAIT_START_BIT;
        }
        break;

    case MK_WAIT_START_BIT:
        d->tmp_counter++;
        if (high > d->period) {
            d->tmp_counter = 0;
            d->state = MK_WAIT_START_WORD;
        } else if (d->tmp_counter > 40) {
            mk_start(d);
        }
        break;

    case MK_WAIT_START_WORD:
        d->tmp_data = (d->tmp_data << 1) | (low < d->period / 2 ? 0 : 1);
        if (++d->tmp_counter == 3) {
            if (d->tmp_data == 0x2) {
                d->tmp_counter = 0;
                d->tmp_data = 0;
                d->state = MK_READ_WORD;
            } else {
                mk_start(d);
            }
        }
        break;

    case MK_READ_WORD:
        d->tmp_data = (d->tmp_data << 1) | (low < d->period / 2 ? 0 : 1);
        if (++d->tmp_counter == 8) {
            if (!mk_parity_even(d->tmp_data)) {
                mk_start(d);
                break;
            }
            d->data = (d->data << 8) | d->tmp_data;
            d->tmp_data = 0;
            d->tmp_counter = 0;
            if (++d->byte_index == 4) {
                if (high > d->period) {
                    d->state = MK_READ_STOP;
                } else {
                    mk_start(d);
                }
            }
        }
        break;

    case MK_READ_STOP:
        d->tmp_data = (d->tmp_data << 1) | (low < d->period / 2 ? 0 : 1);
        if (++d->tmp_counter == 3) {
            if (d->tmp_data == 0x2) {
                return true;
            }
            mk_start(d);
        }
        break;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Capture and run extraction                                                 */
/* ------------------------------------------------------------------------- */

static esp_err_t capture(size_t *count)
{
    size_t n = 0;
    esp_err_t err;

    adc_continuous_flush_pool(s.adc);
    ESP_RETURN_ON_ERROR(adc_continuous_start(s.adc), TAG, "adc start");

    /* Bounded so a misbehaving driver can never wedge the poll task. */
    for (int frames = 0; n < CAPTURE_SAMPLES && frames < CAPTURE_SAMPLES / FRAME_SAMPLES * 2; frames++) {
        uint32_t got = 0;
        err = adc_continuous_read(s.adc, s.raw, FRAME_BYTES, &got, READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            break;
        }
        uint32_t parsed_n = 0;
        if (adc_continuous_parse_data(s.adc, s.raw, got, s.parsed, &parsed_n) != ESP_OK) {
            continue;
        }
        for (uint32_t i = 0; i < parsed_n && n < CAPTURE_SAMPLES; i++) {
            if (s.parsed[i].valid) {
                s.samples[n++] = (uint16_t)s.parsed[i].raw_data;
            }
        }
    }
    adc_continuous_stop(s.adc);

    *count = n;
    return (n >= CAPTURE_SAMPLES / 2) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/**
 * Slices the capture into level runs using a hysteresis band around the
 * midpoint between the observed extremes. Returns the number of runs.
 */
static size_t extract_runs(size_t count, activekey_result_t *res)
{
    uint16_t lo = 4095, hi = 0;
    for (size_t i = 0; i < count; i++) {
        if (s.samples[i] < lo) {
            lo = s.samples[i];
        }
        if (s.samples[i] > hi) {
            hi = s.samples[i];
        }
    }
    res->adc_min = lo;
    res->adc_max = hi;
    if (hi - lo < MIN_SWING) {
        return 0;
    }

    const uint16_t mid = (uint16_t)((lo + hi) / 2);
    const uint16_t band = (uint16_t)((hi - lo) / 6);
    bool level = s.samples[0] > mid;
    size_t run_len = 0, runs = 0;

    for (size_t i = 0; i < count; i++) {
        bool next = level;
        if (level && s.samples[i] < mid - band) {
            next = false;
        } else if (!level && s.samples[i] > mid + band) {
            next = true;
        }
        if (next != level) {
            uint32_t us = run_len * SAMPLE_US;
            s.runs[runs++] = (run_t){.level = level, .us = us > UINT16_MAX ? UINT16_MAX : (uint16_t)us};
            level = next;
            run_len = 0;
        }
        run_len++;
    }
    res->edges = runs;
    return runs;
}

/**
 * Feeds the runs to both decoders. The decoders take Flipper-style events:
 * the level *after* an edge and the length of the run that just ended.
 * Comparator polarity differs between front-ends, so both are tried; the
 * framing rules reject the wrong one.
 */
static bool decode(size_t runs, bool invert, activekey_result_t *res)
{
    cyfral_dec_t cy;
    metakom_dec_t mk;
    cy_start(&cy);
    mk_start(&mk);

    for (size_t i = 0; i < runs; i++) {
        bool new_level = !s.runs[i].level;
        if (invert) {
            new_level = !new_level;
        }
        uint32_t len = s.runs[i].us;
        if (cy_feed(&cy, new_level, len)) {
            res->proto = ACTIVEKEY_PROTO_CYFRAL;
            res->code = cy.data;
            return true;
        }
        if (mk_feed(&mk, new_level, len)) {
            res->proto = ACTIVEKEY_PROTO_METAKOM;
            res->code = mk.data;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

esp_err_t activekey_init(int sense_gpio)
{
    ESP_RETURN_ON_FALSE(s.adc == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    if (sense_gpio < 0) {
        ESP_LOGI(TAG, "Cyfral/Metakom sensing disabled");
        return ESP_OK;
    }

    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_continuous_io_to_channel(sense_gpio, &unit, &channel);
    ESP_RETURN_ON_FALSE(err == ESP_OK && unit == ADC_UNIT_1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "GPIO%d has no ADC1 channel; pick an ADC1 pin for the key line", sense_gpio);

    s.raw = malloc(FRAME_BYTES);
    s.parsed = malloc(FRAME_SAMPLES * sizeof(*s.parsed));
    s.samples = malloc(CAPTURE_SAMPLES * sizeof(*s.samples));
    s.runs = malloc(CAPTURE_SAMPLES * sizeof(*s.runs));
    ESP_RETURN_ON_FALSE(s.raw && s.parsed && s.samples && s.runs, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");

    const adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = FRAME_BYTES * 4,
        .conv_frame_size = FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &s.adc), TAG, "adc handle");

    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = channel,
        .unit = unit,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    const adc_continuous_config_t cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };
    /* Note: this floats the pin; a shared 1-Wire pin must be configured
     * (again) as open-drain by the bus driver after this call. */
    ESP_RETURN_ON_ERROR(adc_continuous_config(s.adc, &cfg), TAG, "adc config");

    s.gpio = sense_gpio;
    ESP_LOGI(TAG, "Cyfral/Metakom sensing on GPIO%d (ADC1 ch%d, %d kS/s)", sense_gpio, channel,
             SAMPLE_RATE_HZ / 1000);
    return ESP_OK;
}

bool activekey_available(void)
{
    return s.adc != NULL;
}

esp_err_t activekey_read(activekey_result_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));
    if (s.adc == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t count = 0;
    ESP_RETURN_ON_ERROR(capture(&count), TAG, "capture");

    size_t runs = extract_runs(count, out);
    if (runs < 8) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t total_us = 0;
    for (size_t i = 0; i < runs; i++) {
        total_us += s.runs[i].us;
    }
    out->period_us = total_us * 2 / runs;

    if (decode(runs, false, out) || decode(runs, true, out)) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

const char *activekey_proto_str(activekey_proto_t proto)
{
    switch (proto) {
    case ACTIVEKEY_PROTO_CYFRAL:
        return "cyfral";
    case ACTIVEKEY_PROTO_METAKOM:
        return "metakom";
    case ACTIVEKEY_PROTO_UNKNOWN:
        return "unknown";
    default:
        return "none";
    }
}

void activekey_code_to_str(activekey_proto_t proto, uint32_t code, char out[ACTIVEKEY_CODE_STR_LEN])
{
    if (proto == ACTIVEKEY_PROTO_CYFRAL) {
        snprintf(out, ACTIVEKEY_CODE_STR_LEN, "%04X", (unsigned)(code & 0xFFFF));
    } else if (proto == ACTIVEKEY_PROTO_METAKOM) {
        snprintf(out, ACTIVEKEY_CODE_STR_LEN, "%08X", (unsigned)code);
    } else {
        out[0] = '\0';
    }
}
