#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "app_ir.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "ir"

#define IR_RESOLUTION_HZ    500000U    /* 1 RMT tick = 2 us */
#define IR_RX_GPIO          CONFIG_IR_TOOL_IR_RX_GPIO
#define IR_TX_GPIO          CONFIG_IR_TOOL_IR_TX_GPIO
#define IR_RX_BUF_SYMBOLS   96         /* 2 mem blocks of 48; NEC frame fits */
#define IR_TX_BUF_SYMBOLS   96
#define IR_RX_MIN_PULSE_NS  1000U      /* glitches < 1 us are ignored */
#define IR_RX_TIMEOUT_NS    50000000U  /* idle gap > 50 ms ends the frame */

#define NEC_LEAD_PULSE_US   9000U
#define NEC_LEAD_SPACE_US   4500U
#define NEC_REPEAT_SPACE_US 2250U
#define NEC_BIT_PULSE_US    560U
#define NEC_BIT0_SPACE_US   560U
#define NEC_BIT1_SPACE_US   1690U

#define IR_CARRIER_DUTY     CONFIG_IR_TOOL_CARRIER_DUTY
#define IR_PLAY_QUEUE_LEN   4

#define IR_NVS_NS           "ir_tool"
#define IR_NVS_KEY_FREQ     "carrier_hz"

typedef struct {
    uint32_t dur;   /* duration in us */
    uint8_t level;
} ir_seg_t;

/* Playback request passed to the playback task via queue */
typedef struct {
    uint32_t freq_hz;       /* 0 = use global carrier */
    uint32_t symbol_count;
    rmt_symbol_word_t *symbols; /* malloc'ed, freed by the playback task */
} ir_play_req_t;

/* RX variables */
static rmt_channel_handle_t s_rx_ch = NULL;
static rmt_receive_config_t s_rx_cfg;
static rmt_symbol_word_t s_rx_buf[IR_RX_BUF_SYMBOLS];
static SemaphoreHandle_t s_mutex;
static ir_frame_t s_frame;
static bool s_frame_new;
static uint32_t s_seq;
static ir_seg_t s_segs[IR_RAW_MAX_SEGS];

/* TX variables */
static rmt_channel_handle_t s_tx_ch = NULL;
static rmt_encoder_handle_t s_copy_enc = NULL;
static rmt_transmit_config_t s_tx_cfg;
static QueueHandle_t s_play_queue;
static volatile bool s_playing = false;

/* global carrier frequency (Hz) */
static uint32_t s_carrier_freq = CONFIG_IR_TOOL_CARRIER_FREQ_HZ;

/* history ring */
static ir_frame_t s_history[IR_HISTORY_DEPTH];
static uint32_t s_history_head = 0;
static uint32_t s_history_count = 0;

static void ir_playback_task(void *arg);

static inline bool dur_in_range(uint32_t v, uint32_t nom, uint32_t margin)
{
    return (v >= nom - margin) && (v <= nom + margin);
}

/* Simple flag + copy instead of a FreeRTOS queue in ISR (avoids ISR compatibility issues) */
static volatile bool s_rx_done = false;
static rmt_rx_done_event_data_t s_rx_data;

static bool ir_rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata)
{
    (void)ch;
    (void)udata;
    s_rx_data = *edata;
    s_rx_done = true;
    return true; /* request context switch so ir_task wakes immediately */
}

/* Analyze raw RMT symbols: features + NEC decode. Writes polarity-normalized durs. */
static void ir_analyze(const rmt_symbol_word_t *sym, size_t num, ir_frame_t *f)
{
    /* flatten RMT symbol pairs into an alternating level/duration list */
    int n = 0;
    for (size_t i = 0; i < num && n < IR_RAW_MAX_SEGS; i++) {
        if (sym[i].duration0 > 0) {
            s_segs[n].level = sym[i].level0;
            s_segs[n].dur = (uint32_t)sym[i].duration0 * 2;
            n++;
        }
        if (n < IR_RAW_MAX_SEGS && sym[i].duration1 > 0) {
            s_segs[n].level = sym[i].level1;
            s_segs[n].dur = (uint32_t)sym[i].duration1 * 2;
            n++;
        }
    }

    /* trim leading idle and the trailing idle-to-timeout tail */
    int start = 0;
    int end = n;
    if (end - start >= 2 && s_segs[start].dur > 15000) {
        start++;
    }
    if (end - start >= 2 && s_segs[end - 1].dur > 10000) {
        end--;
    }

    /* raw signal features */
    f->seg_count = (uint32_t)(end - start);
    uint64_t total = 0;
    for (int i = start; i < end; i++) {
        total += s_segs[i].dur;
    }
    f->total_us = (uint32_t)total;

    f->leader_pulse_us = 0;
    f->leader_space_us = 0;
    f->last_gap_us = 0;
    if (end - start >= 1) {
        f->leader_pulse_us = s_segs[start].dur;
    }
    if (end - start >= 2) {
        f->leader_space_us = s_segs[start + 1].dur;
    }
    if (end - start >= 1) {
        f->last_gap_us = s_segs[end - 1].dur;
    }

    f->pulse_count = 0;
    f->min_pulse_us = 0;
    f->max_pulse_us = 0;
    for (int i = start; i < end; i += 2) {
        f->pulse_count++;
        if (i == start) {
            continue; /* exclude the leader from min/max */
        }
        if (f->min_pulse_us == 0 || s_segs[i].dur < f->min_pulse_us) {
            f->min_pulse_us = s_segs[i].dur;
        }
        if (s_segs[i].dur > f->max_pulse_us) {
            f->max_pulse_us = s_segs[i].dur;
        }
    }

    /* store raw durations, normalized so first segment = carrier-on (level 1).
     * VS1838B outputs active-low baseband (bursts are level 0), so flip levels:
     * durations are unchanged. */
    f->raw_count = 0;
    for (int i = start; i < end && f->raw_count < IR_RAW_MAX_SEGS; i++) {
        f->raw_durs[f->raw_count++] = s_segs[i].dur;
    }

    /* NEC decode: scan for the 9 ms leader (polarity agnostic) */
    f->nec_ok = false;
    f->nec_repeat = false;
    f->nec_chksum_ok = false;
    f->nec_ext_addr = false;
    f->nec_bits = 0;
    f->nec_addr = 0;
    f->nec_cmd = 0;
    f->nec_raw = 0;

    for (int i = start; i + 1 < end; i++) {
        if (s_segs[i + 1].level == s_segs[i].level) {
            continue;
        }
        if (!dur_in_range(s_segs[i].dur, NEC_LEAD_PULSE_US, 1200)) {
            continue;
        }

        /* repeat code: 9ms + 2.25ms + 560us */
        if (dur_in_range(s_segs[i + 1].dur, NEC_REPEAT_SPACE_US, 500)) {
            if (i + 2 < end && dur_in_range(s_segs[i + 2].dur, NEC_BIT_PULSE_US, 300)) {
                f->nec_ok = true;
                f->nec_repeat = true;
            }
            return;
        }
        if (!dur_in_range(s_segs[i + 1].dur, NEC_LEAD_SPACE_US, 700)) {
            continue;
        }

        /* 32 data bits, LSB first: (pulse, space) pairs */
        uint32_t raw = 0;
        bool good = true;
        for (int b = 0; b < 32; b++) {
            int p = i + 2 + b * 2;
            if (p + 1 >= end || !dur_in_range(s_segs[p].dur, NEC_BIT_PULSE_US, 300)) {
                good = false;
                break;
            }
            if (dur_in_range(s_segs[p + 1].dur, NEC_BIT0_SPACE_US, 350)) {
                /* logic 0 */
            } else if (dur_in_range(s_segs[p + 1].dur, NEC_BIT1_SPACE_US, 450)) {
                raw |= (1UL << b);
            } else {
                good = false;
                break;
            }
        }
        if (good) {
            uint8_t a = raw & 0xFF;
            uint8_t ai = (raw >> 8) & 0xFF;
            uint8_t c = (raw >> 16) & 0xFF;
            uint8_t ci = (raw >> 24) & 0xFF;
            f->nec_ok = true;
            f->nec_bits = 32;
            f->nec_raw = raw;
            if (ai == (uint8_t)~a && ci == (uint8_t)~c) {
                f->nec_chksum_ok = true;
                f->nec_addr = a;
                f->nec_cmd = c;
            } else if (ci == (uint8_t)~c) {
                /* extended NEC with 16-bit address */
                f->nec_ext_addr = true;
                f->nec_addr = raw & 0xFFFF;
                f->nec_cmd = c;
            } else {
                f->nec_addr = a;
                f->nec_cmd = c;
            }
        }
        return;
    }
}

static void ir_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_rx_done) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        /* read symbol data BEFORE clearing the flag to avoid racing the ISR */
        rmt_rx_done_event_data_t ev = s_rx_data;
        s_rx_done = false;

        ir_frame_t fr;
        memset(&fr, 0, sizeof(fr));
        size_t num = ev.num_symbols;
        if (num > IR_RX_BUF_SYMBOLS) {
            num = IR_RX_BUF_SYMBOLS;
        }
        ir_analyze(ev.received_symbols, num, &fr);
        fr.seq = ++s_seq;
        fr.valid = true;
        fr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_frame = fr;
            s_frame_new = true;
            /* push into history ring */
            s_history[s_history_head] = fr;
            s_history_head = (s_history_head + 1) % IR_HISTORY_DEPTH;
            if (s_history_count < IR_HISTORY_DEPTH) {
                s_history_count++;
            }
            xSemaphoreGive(s_mutex);
        }

        rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg);
    }
}

static esp_err_t ir_load_carrier_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(IR_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t freq = 0;
    esp_err_t err = nvs_get_u32(h, IR_NVS_KEY_FREQ, &freq);
    nvs_close(h);
    if (err == ESP_OK && freq >= IR_CARRIER_FREQ_MIN && freq <= IR_CARRIER_FREQ_MAX) {
        s_carrier_freq = freq;
        ESP_LOGI(TAG, "Carrier loaded from NVS: %lu Hz", (unsigned long)freq);
    }
    return ESP_OK;
}

esp_err_t ir_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_play_queue = xQueueCreate(IR_PLAY_QUEUE_LEN, sizeof(ir_play_req_t *));
    if (!s_play_queue) {
        return ESP_ERR_NO_MEM;
    }

    ir_load_carrier_from_nvs();

    /* Configure RMT RX channel (ESP32-C3: no DMA, multiple mem blocks, ping-pong) */
    rmt_rx_channel_config_t rx_ch_cfg = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_BUF_SYMBOLS,
        .intr_priority = 0,
        .flags = {
            .invert_in = false,
            .with_dma = false,
            .allow_pd = false,
        },
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_ch_cfg, &s_rx_ch), TAG, "create RMT RX channel");

    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = ir_rx_done_cb,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_ch, &rx_cbs, NULL),
                        TAG, "register RMT RX callback");

    s_rx_cfg.signal_range_min_ns = IR_RX_MIN_PULSE_NS;
    s_rx_cfg.signal_range_max_ns = IR_RX_TIMEOUT_NS;
    s_rx_cfg.flags.en_partial_rx = 0;

    /* Configure RMT TX channel (no DMA; copy encoder segments long payloads) */
    rmt_tx_channel_config_t tx_ch_cfg = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_TX_BUF_SYMBOLS,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = false,
            .allow_pd = false,
        },
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_ch_cfg, &s_tx_ch), TAG, "create RMT TX channel");

    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = IR_CARRIER_DUTY / 100.0f,
        .frequency_hz = s_carrier_freq,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx_ch, &carrier_cfg), TAG, "apply carrier");

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_cfg, &s_copy_enc), TAG, "create copy encoder");

    s_tx_cfg.loop_count = 0;

    ESP_RETURN_ON_ERROR(rmt_enable(s_tx_ch), TAG, "enable RMT TX");

    /* create IR receive task */
    if (xTaskCreate(ir_task, "ir_task", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* create playback task */
    if (xTaskCreate(ir_playback_task, "ir_play", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* start receiving */
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_ch), TAG, "enable RMT RX");
    ESP_RETURN_ON_ERROR(rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg),
                        TAG, "start RMT receive");

    ESP_LOGI(TAG, "IR ready (RX: GPIO%d, TX: GPIO%d, carrier %lu Hz)",
             IR_RX_GPIO, IR_TX_GPIO, (unsigned long)s_carrier_freq);
    return ESP_OK;
}

bool ir_get_frame(ir_frame_t *out)
{
    bool got = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_frame_new) {
            *out = s_frame;
            s_frame_new = false;
            got = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return got;
}

uint32_t ir_history_count(void)
{
    uint32_t c;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return 0;
    }
    c = s_history_count;
    xSemaphoreGive(s_mutex);
    return c;
}

esp_err_t ir_history_get(uint32_t index, ir_frame_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (index >= s_history_count) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    /* ring: oldest = head - count (mod depth) */
    uint32_t pos = (s_history_head + IR_HISTORY_DEPTH - s_history_count + index) % IR_HISTORY_DEPTH;
    *out = s_history[pos];
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ir_history_get_latest(ir_frame_t *out)
{
    uint32_t c = ir_history_count();
    if (c == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return ir_history_get(c - 1, out);
}

uint32_t ir_get_latest_seq(void)
{
    ir_frame_t fr;
    if (ir_history_get_latest(&fr) == ESP_OK) {
        return fr.seq;
    }
    return 0;
}

uint32_t ir_get_carrier_freq(void)
{
    return s_carrier_freq;
}

esp_err_t ir_set_carrier_freq(uint32_t freq_hz)
{
    if (freq_hz < IR_CARRIER_FREQ_MIN || freq_hz > IR_CARRIER_FREQ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = IR_CARRIER_DUTY / 100.0f,
        .frequency_hz = freq_hz,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_tx_ch, &carrier_cfg), TAG, "apply carrier");
    s_carrier_freq = freq_hz;

    nvs_handle_t h;
    if (nvs_open(IR_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, IR_NVS_KEY_FREQ, freq_hz);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Carrier set to %lu Hz", (unsigned long)freq_hz);
    return ESP_OK;
}

/* ---- playback ---- */

/* Build RMT symbols from polarity-normalized durations (first segment carrier-on).
 * Odd segment count: last symbol gets duration1 = 0 (level flips instantly, then stop). */
static esp_err_t ir_build_symbols(const uint32_t *durs, uint32_t count,
                                  rmt_symbol_word_t **out_syms, uint32_t *out_count)
{
    if (count == 0 || count > IR_MAX_TX_SYMBOLS * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t n_syms = (count + 1) / 2;
    /* RMT duration field is 15-bit: at 500kHz (2us/tick) the cap is 65534us */
    for (uint32_t i = 0; i < count; i++) {
        if (durs[i] == 0 || durs[i] > 65000U) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    rmt_symbol_word_t *syms = calloc(n_syms, sizeof(rmt_symbol_word_t));
    if (!syms) {
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t level = (i & 1) ? 0 : 1; /* carrier-on for even segments */
        uint32_t ticks = (durs[i] + 1) / 2; /* us -> RMT ticks (2us), round up */
        if (i & 1) {
            syms[i / 2].duration1 = ticks;
            syms[i / 2].level1 = level;
        } else {
            syms[i / 2].duration0 = ticks;
            syms[i / 2].level0 = level;
        }
    }
    *out_syms = syms;
    *out_count = n_syms;
    return ESP_OK;
}

/* Enqueue a playback request; task applies carrier, transmits, frees the payload. */
static esp_err_t ir_play_durs(const uint32_t *durs, uint32_t count, uint32_t freq_hz)
{
    if (count == 0 || count > IR_MAX_TX_SYMBOLS * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    rmt_symbol_word_t *syms = NULL;
    uint32_t n_syms = 0;
    esp_err_t ret = ir_build_symbols(durs, count, &syms, &n_syms);
    if (ret != ESP_OK) {
        return ret;
    }
    ir_play_req_t *req = malloc(sizeof(ir_play_req_t));
    if (!req) {
        free(syms);
        return ESP_ERR_NO_MEM;
    }
    req->freq_hz = freq_hz;
    req->symbol_count = n_syms;
    req->symbols = syms;

    if (xQueueSend(s_play_queue, &req, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(syms);
        free(req);
        ESP_LOGW(TAG, "Playback queue full");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Playback queued: %lu segments -> %lu symbols", (unsigned long)count, (unsigned long)n_syms);
    return ESP_OK;
}

static void ir_playback_task(void *arg)
{
    (void)arg;
    for (;;) {
        ir_play_req_t *req = NULL;
        if (xQueueReceive(s_play_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_playing = true;

        uint32_t freq = req->freq_hz ? req->freq_hz : s_carrier_freq;
        rmt_carrier_config_t cc = {
            .duty_cycle = IR_CARRIER_DUTY / 100.0f,
            .frequency_hz = freq,
        };
        if (rmt_apply_carrier(s_tx_ch, &cc) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to apply carrier %lu Hz", (unsigned long)freq);
        }

        esp_err_t ret = rmt_transmit(s_tx_ch, s_copy_enc, req->symbols,
                                     req->symbol_count * sizeof(rmt_symbol_word_t), &s_tx_cfg);
        if (ret == ESP_OK) {
            ret = rmt_tx_wait_all_done(s_tx_ch, pdMS_TO_TICKS(5000));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Transmission timeout");
            }
        } else {
            ESP_LOGE(TAG, "Transmit failed: %s", esp_err_to_name(ret));
        }

        free(req->symbols);
        free(req);
        s_playing = false;
    }
}

bool ir_is_playing(void)
{
    return s_playing;
}

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

    ESP_LOGI(TAG, "Play hxd %08lX (%lu Hz)", (unsigned long)raw, (unsigned long)(freq_hz ? freq_hz : s_carrier_freq));
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
