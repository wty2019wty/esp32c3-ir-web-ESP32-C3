#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "app_ir.h"
#include "app_ir_internal.h"
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
#define IR_RX_BUF_SYMBOLS   IR_RAW_MAX_SEGS  /* user-side RX buffer; a symbol holds 2 segments, so this covers
                                              * IR_RAW_MAX_SEGS segments with headroom (4KB SRAM at 1024) */
#define IR_RX_MEM_SYMBOLS   96         /* max HW memory a single RX channel can own on ESP32-C3: the group has
                                        * 4 blocks x 48 symbols, RX candidates are blocks 2-3 only, so 2 blocks
                                        * max. The driver ping-pongs 48-symbol halves and copies chunks into the
                                        * large user buffer (IR_RX_BUF_SYMBOLS), which is what lifts the limit. */
#define IR_TX_BUF_SYMBOLS   96
#define IR_RX_MIN_PULSE_NS  1000U      /* glitches < 1 us are ignored */
#define IR_RX_TIMEOUT_NS    50000000U  /* idle gap > 50 ms ends the frame */

#define IR_CARRIER_DUTY     CONFIG_IR_TOOL_CARRIER_DUTY
#define IR_PLAY_QUEUE_LEN   4

#define IR_NVS_NS           "ir_tool"
#define IR_NVS_KEY_FREQ     "carrier_hz"
#define IR_NVS_KEY_RX_PAUSE "rx_pause"

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
static ir_seg_t s_segs[IR_RAW_MAX_SEGS];   /* analysis scratch (RMT ticks -> us) */

/* TX variables */
static rmt_channel_handle_t s_tx_ch = NULL;
static rmt_encoder_handle_t s_copy_enc = NULL;
static rmt_transmit_config_t s_tx_cfg;
static QueueHandle_t s_play_queue;
static volatile bool s_playing = false;
static volatile bool s_rx_paused = false;      /* RX paused while transmitting */
static bool s_rx_pause_enabled = true;         /* user switch, default on */

/* global carrier frequency (Hz) */
static uint32_t s_carrier_freq = CONFIG_IR_TOOL_CARRIER_FREQ_HZ;

static void ir_playback_task(void *arg);

/* Simple flag + copy instead of a FreeRTOS queue in ISR (avoids ISR compatibility issues) */
static volatile bool s_rx_done = false;
static volatile bool s_rx_overflow = false; /* a partial (non-last) chunk was reported -> final event is only a tail */
static rmt_rx_done_event_data_t s_rx_data;

static bool ir_rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *udata)
{
    (void)ch;
    (void)udata;
    /* With en_partial_rx the driver notifies mid-frame chunks once the user
     * buffer fills, then reuses the buffer from offset 0. The final is_last
     * event then holds only the tail chunk of an over-limit frame. Remember
     * the partial event so ir_task drops that tail instead of decoding it. */
    if (!edata->flags.is_last) {
        s_rx_overflow = true;
        return false;
    }
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

    /* Trim leading idle and the trailing idle-to-timeout tail. With the
     * active-low VS1838B output, carrier bursts read as level 0 and idle (no
     * carrier) as level 1, so a level-1 head segment is leading idle regardless
     * of its duration. The duration-only check (>15000us) would miss a short
     * 10-15ms leading idle (fast re-press / RMT tail), leaving a space as the
     * first stored segment and flipping the implied playback polarity.
     * For active-high receivers the polarity is inverted: level-1 segments are
     * real carrier, so only a long leading idle is trimmed (see
     * IR_TOOL_RX_ACTIVE_LOW). */
    int start = 0;
    int end = n;
#if CONFIG_IR_TOOL_RX_ACTIVE_LOW
    if (end - start >= 2 && (s_segs[start].dur > 15000 || s_segs[start].level == 1)) {
        start++;
    }
#else
    if (end - start >= 2 && s_segs[start].dur > 15000) {
        start++;
    }
#endif
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

    /* NEC decode (separate module) */
    ir_nec_decode(s_segs, start, end, f);
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
        bool overflow = s_rx_overflow;
        s_rx_done = false;
        s_rx_overflow = false;

        ir_frame_t fr;
        memset(&fr, 0, sizeof(fr));
        size_t num = ev.num_symbols;
        /* Two ways a frame can exceed IR_RAW_MAX_SEGS:
         * - the driver fired a partial event (user buffer filled), so this
         *   final event only holds the tail chunk of an over-limit frame;
         * - the symbol count alone already exceeds what IR_RAW_MAX_SEGS
         *   alternating segments can produce (2 segments per symbol), so the
         *   frame would be silently truncated by ir_analyze.
         * Either way decoding would produce garbage/a truncated frame, so
         * drop it and re-arm the receiver. */
        if (overflow || num > (IR_RAW_MAX_SEGS + 1) / 2) {
            ESP_LOGW(TAG, "Frame too long for RX buffer (%u symbols), dropped", (unsigned)num);
            if (!s_rx_paused) {
                rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg);
            }
            continue;
        }
        ir_analyze(ev.received_symbols, num, &fr);
        fr.valid = true;
        fr.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        fr.capture_freq_hz = ir_get_carrier_freq();

        /* store into latest + history ring and notify listeners (e.g.
         * WebSocket push) about the new frame (assigns seq) */
        ir_store_push(&fr);

        /* While the TX task is transmitting, RX stays paused (avoids
         * self-loop frames). The playback task resumes the receive loop. */
        if (!s_rx_paused) {
            rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg);
        }
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
    if (ir_store_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    s_play_queue = xQueueCreate(IR_PLAY_QUEUE_LEN, sizeof(ir_play_req_t *));
    if (!s_play_queue) {
        return ESP_ERR_NO_MEM;
    }

    ir_load_carrier_from_nvs();

    nvs_handle_t nh;
    if (nvs_open(IR_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
        uint8_t v = 1;
        if (nvs_get_u8(nh, IR_NVS_KEY_RX_PAUSE, &v) == ESP_OK) {
            s_rx_pause_enabled = v != 0;
        }
        nvs_close(nh);
    }

    /* Configure RMT RX channel (ESP32-C3: no DMA, multiple mem blocks, ping-pong) */
    rmt_rx_channel_config_t rx_ch_cfg = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_MEM_SYMBOLS,
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
    /* long-frame capture: the driver ping-pongs HW memory chunks into the
     * large user buffer (IR_RX_BUF_SYMBOLS) in the ISR and reports the whole
     * frame on idle timeout. en_partial_rx only changes overflow behavior:
     * when the user buffer fills, it fires a partial event and reuses the
     * buffer from offset 0, so the final event then holds only the tail chunk
     * (dropped in ir_task). In-range frames (<= IR_RAW_MAX_SEGS) never fill
     * the buffer, so the flag has no effect for them. */
    s_rx_cfg.flags.en_partial_rx = 1;

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

    /* create IR receive task: stack must fit one ir_frame_t (raw_durs scales
     * with IR_RAW_MAX_SEGS, up to ~8.2KB at 2048) plus the analysis/callback
     * chain, so derive it from the frame size instead of hardcoding it */
    if (xTaskCreate(ir_task, "ir_task", sizeof(ir_frame_t) + 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* create playback task */
    if (xTaskCreate(ir_playback_task, "ir_play", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* start receiving: clear any stale receive flags first (static init is
     * false, but re-run / reordered init must not carry a leftover state) */
    s_rx_done = false;
    s_rx_overflow = false;
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_ch), TAG, "enable RMT RX");
    ESP_RETURN_ON_ERROR(rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg),
                        TAG, "start RMT receive");

    ESP_LOGI(TAG, "IR ready (RX: GPIO%d, TX: GPIO%d, carrier %lu Hz)",
             IR_RX_GPIO, IR_TX_GPIO, (unsigned long)s_carrier_freq);
    return ESP_OK;
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

/* Enqueue a playback request; task applies carrier, transmits, frees the payload.
 * Shared with the playback API layer (app_ir_play.c). */
esp_err_t ir_play_durs(const uint32_t *durs, uint32_t count, uint32_t freq_hz)
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
        ir_store_notify_play(true);

        /* pause IR reception while transmitting to avoid self-loop frames */
        if (!s_rx_paused && s_rx_pause_enabled) {
            if (rmt_disable(s_rx_ch) == ESP_OK) {
                s_rx_paused = true;
            } else {
                ESP_LOGW(TAG, "Failed to pause RX before playback");
            }
        }

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
        ir_store_notify_play(false);

        /* resume RX once the queue drains */
        if (s_rx_paused && uxQueueMessagesWaiting(s_play_queue) == 0) {
            /* a receive aborted by rmt_disable() above can leave the partial
             * flag set; don't let it leak into the next frame */
            s_rx_overflow = false;
            if (rmt_enable(s_rx_ch) == ESP_OK &&
                rmt_receive(s_rx_ch, s_rx_buf, sizeof(s_rx_buf), &s_rx_cfg) == ESP_OK) {
                s_rx_paused = false;
            } else {
                ESP_LOGW(TAG, "Failed to resume RX after playback");
            }
        }
    }
}

bool ir_get_rx_pause_enabled(void)
{
    return s_rx_pause_enabled;
}

esp_err_t ir_set_rx_pause_enabled(bool enabled)
{
    s_rx_pause_enabled = enabled;
    nvs_handle_t nh;
    if (nvs_open(IR_NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, IR_NVS_KEY_RX_PAUSE, enabled ? 1 : 0);
        nvs_commit(nh);
        nvs_close(nh);
    }
    ESP_LOGI(TAG, "RX pause on playback: %s", enabled ? "on" : "off");
    return ESP_OK;
}

bool ir_is_playing(void)
{
    return s_playing;
}
