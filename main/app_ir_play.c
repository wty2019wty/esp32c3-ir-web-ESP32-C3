#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "app_ir.h"
#include "app_ir_internal.h"
#include "esp_log.h"
#include "esp_check.h"

#define TAG "ir"

/* Parse 1..8 hex digits (0x prefix / spaces allowed) into a 32-bit value. */
static esp_err_t ir_parse_hxd(const char *hex, uint32_t *out_raw)
{
    if (!hex) {
        return ESP_ERR_INVALID_ARG;
    }
    while (isspace((unsigned char)*hex)) {
        hex++;
    }
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }
    size_t len = 0;
    const char *p = hex;
    while (isxdigit((unsigned char)*p)) {
        p++;
        len++;
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (len == 0 || len > 8 || *p != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[16];
    memcpy(buf, hex, len);
    buf[len] = '\0';
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 16);
    if (end == buf || *end != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *out_raw = (uint32_t)v;
    return ESP_OK;
}

esp_err_t ir_play_hxd(const char *hex, uint32_t freq_hz)
{
    uint32_t raw = 0;
    ESP_RETURN_ON_ERROR(ir_parse_hxd(hex, &raw), TAG, "bad hex value");

    /* standard NEC frame, LSB first */
    uint32_t durs[2 + 32 * 2 + 1];
    uint32_t n = 0;
    durs[n++] = NEC_LEAD_PULSE_US;
    durs[n++] = NEC_LEAD_SPACE_US;
    for (int b = 0; b < 32; b++) {
        durs[n++] = NEC_BIT_PULSE_US;
        durs[n++] = (raw & (1UL << b)) ? NEC_BIT1_SPACE_US : NEC_BIT0_SPACE_US;
    }
    durs[n++] = NEC_BIT_PULSE_US; /* trailing stop pulse */

    ESP_LOGI(TAG, "Play hxd %08lX (%lu Hz)", (unsigned long)raw,
             (unsigned long)(freq_hz ? freq_hz : ir_get_carrier_freq()));
    return ir_play_durs(durs, n, freq_hz);
}

esp_err_t ir_play_raw(const uint32_t *durs, uint32_t count, uint32_t freq_hz)
{
    if (!durs) {
        return ESP_ERR_INVALID_ARG;
    }
    return ir_play_durs(durs, count, freq_hz);
}

esp_err_t ir_play_history(uint32_t index, uint32_t freq_hz)
{
    ir_frame_t fr;
    esp_err_t ret = ir_history_get(index, &fr);
    if (ret != ESP_OK) {
        return ret;
    }
    if (fr.raw_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return ir_play_durs(fr.raw_durs, fr.raw_count, freq_hz);
}
