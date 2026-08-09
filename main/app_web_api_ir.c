#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_web_internal.h"
#include "app_ir.h"
#include "cJSON.h"

/* Shared IR command cores, invoked by the WebSocket RPC dispatcher
 * (app_web_rpc.c). The REST API has been removed — all control now
 * happens over the /api/ws command channel. */

static esp_err_t play_by_seq(uint32_t seq, uint32_t freq)
{
    uint32_t n = ir_history_count();
    for (uint32_t i = 0; i < n; i++) {
        ir_frame_t fr;
        if (ir_history_get(i, &fr) == ESP_OK && fr.seq == seq) {
            return ir_play_history(i, freq);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Command core for playback. Body: {"type","freq?",value/data/seq}. */
esp_err_t web_ir_play_exec(cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *freq_item = cJSON_GetObjectItem(root, "freq");
    uint32_t freq = 0;
    if (cJSON_IsNumber(freq_item)) {
        double fv = freq_item->valuedouble;
        /* 0 = use global carrier; otherwise must be in the valid range */
        if (fv != 0.0 && (fv < IR_CARRIER_FREQ_MIN || fv > IR_CARRIER_FREQ_MAX)) {
            return ESP_ERR_INVALID_ARG;
        }
        freq = (uint32_t)fv;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(type) && strcmp(type->valuestring, "hxd") == 0) {
        cJSON *v = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(v)) {
            ret = ir_play_hxd(v->valuestring, freq);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "raw") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsArray(data)) {
            int n = cJSON_GetArraySize(data);
            if (n <= 0) {
                ret = ESP_ERR_INVALID_ARG;
            } else {
                uint32_t *durs = malloc((size_t)n * sizeof(uint32_t));
                if (durs) {
                    bool good = true;
                    for (int i = 0; i < n; i++) {
                        cJSON *it = cJSON_GetArrayItem(data, i);
                        double dv = cJSON_IsNumber(it) ? it->valuedouble : -1.0;
                        if (dv <= 0.0 || dv > 65000.0) {
                            good = false;
                            break;
                        }
                        durs[i] = (uint32_t)dv;
                    }
                    if (good) {
                        ret = ir_play_raw(durs, (uint32_t)n, freq);
                    } else {
                        ret = ESP_ERR_INVALID_ARG;
                    }
                    free(durs);
                } else {
                    ret = ESP_ERR_NO_MEM;
                }
            }
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "frame") == 0) {
        cJSON *s = cJSON_GetObjectItem(root, "seq");
        if (cJSON_IsNumber(s)) {
            ret = play_by_seq((uint32_t)s->valuedouble, freq);
        }
    }
    return ret;
}

/* Command core for carrier. Body: {"freq":38000}. */
esp_err_t web_ir_carrier_exec(cJSON *root, uint32_t *freq_out)
{
    cJSON *freq_item = cJSON_GetObjectItem(root, "freq");
    if (!cJSON_IsNumber(freq_item)) {
        return ESP_ERR_NOT_FOUND;
    }
    double fv = freq_item->valuedouble;
    if (fv < IR_CARRIER_FREQ_MIN || fv > IR_CARRIER_FREQ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t freq = (uint32_t)fv;
    esp_err_t ret = ir_set_carrier_freq(freq);
    if (ret == ESP_OK && freq_out) {
        *freq_out = ir_get_carrier_freq();
    }
    return ret;
}

/* Command core for RX pause. Body: {"enabled":true|false}. */
esp_err_t web_ir_rxpause_exec(cJSON *root, bool *enabled_out)
{
    cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(en)) {
        return ESP_ERR_NOT_FOUND;
    }
    ir_set_rx_pause_enabled(cJSON_IsTrue(en));
    if (enabled_out) {
        *enabled_out = ir_get_rx_pause_enabled();
    }
    return ESP_OK;
}
