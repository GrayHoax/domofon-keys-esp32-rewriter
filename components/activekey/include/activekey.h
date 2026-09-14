/**
 * @file activekey.h
 * @brief Reader for "active" intercom keys: Cyfral and Metakom.
 *
 * Unlike 1-Wire (Dallas) keys these do not answer a master; once powered
 * through the pull-up they continuously stream their code by modulating
 * the current drawn from the data line. The resulting voltage swing on the
 * line is captured with the ADC in DMA mode (~83 kS/s for ~25 ms), turned
 * into level/duration runs and fed to pulse-width decoders ported from the
 * Flipper Zero firmware. TM01A blanks finalised as Cyfral/Metakom behave
 * exactly like the originals, so this is how they are read back.
 *
 * Hardware: the ADC-capable "sense" GPIO must see the key line. It may be
 * the 1-Wire data pin itself (when that pin has an ADC1 channel) or a
 * separate pin bridged to it. ADC2 is not usable together with Wi-Fi.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACTIVEKEY_PROTO_NONE = 0,
    ACTIVEKEY_PROTO_CYFRAL,  /**< 16-bit code, 8 one-hot nibbles between 0001 markers. */
    ACTIVEKEY_PROTO_METAKOM, /**< 32-bit code, 4 even-parity bytes between 010 markers.  */
    ACTIVEKEY_PROTO_UNKNOWN, /**< Line is streaming something the decoders reject.        */
} activekey_proto_t;

/** Longest printable code ("XXXXXXXX") plus terminator. */
#define ACTIVEKEY_CODE_STR_LEN 9

typedef struct {
    activekey_proto_t proto; /**< ACTIVEKEY_PROTO_NONE when nothing decoded. */
    uint32_t code;           /**< Bits in the order received, MSB first.     */

    /* Capture diagnostics, filled even when decoding fails. */
    uint16_t adc_min;        /**< Raw 12-bit ADC extremes over the capture.  */
    uint16_t adc_max;
    uint32_t edges;          /**< Level transitions seen after thresholding. */
    uint32_t period_us;      /**< Mean high+low period, 0 if no edges.       */
} activekey_result_t;

/**
 * @brief Set up the ADC on @p sense_gpio. Pass -1 to leave the feature off.
 * @return ESP_ERR_NOT_SUPPORTED if the pin has no ADC1 channel.
 */
esp_err_t activekey_init(int sense_gpio);

bool activekey_available(void);

/**
 * @brief Capture the line for ~25 ms and try to decode a key.
 *
 * Blocks for the capture. The caller must make sure nothing else drives
 * the line meanwhile (hold the 1-Wire bus lock).
 * @return ESP_OK with a protocol set; ESP_ERR_NOT_FOUND when the line is
 *         static or the stream does not decode; ESP_ERR_NOT_SUPPORTED if
 *         the feature is off.
 */
esp_err_t activekey_read(activekey_result_t *out);

const char *activekey_proto_str(activekey_proto_t proto);
void        activekey_code_to_str(activekey_proto_t proto, uint32_t code, char out[ACTIVEKEY_CODE_STR_LEN]);

/**
 * "Dallas container" for a Cyfral code, the convention duplicators use to
 * keep a Cyfral key in an 8-byte 1-Wire ROM: the raw 36-bit frame (start
 * nibble 0001 + eight one-hot nibbles) packed MSB-first into rom[0..4],
 * rom[4] low nibble and rom[5..6] zero. rom[7] is left for the caller to
 * fill with the CRC. Such a ROM can be written to RW1990 for safekeeping
 * and turned into a working Cyfral key on a TM01A blank.
 */
void activekey_cyfral_pack(uint16_t code, uint8_t rom[8]);

/** @return true and the code if @p rom is a well-formed Cyfral container. */
bool activekey_cyfral_unpack(const uint8_t rom[8], uint16_t *code);

/** Raw 36-bit frame for @p code, MSB-first in frame[0..4] (low nibble of frame[4] zero). */
void activekey_cyfral_frame(uint16_t code, uint8_t frame[5]);

#ifdef __cplusplus
}
#endif
