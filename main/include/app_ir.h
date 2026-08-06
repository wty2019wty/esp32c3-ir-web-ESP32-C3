#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define IR_RAW_MAX_SEGS 192              /* RX buffer 96 symbols -> up to 192 alternating segments */
#define IR_MAX_TX_SYMBOLS CONFIG_IR_TOOL_MAX_RAW_SYMBOLS  /* cap for one transmission */
#define IR_HISTORY_DEPTH CONFIG_IR_TOOL_HISTORY_DEPTH

/* Alternating level segments, normalized so the FIRST segment is carrier-on (level 1).
 * Stored as durations only (level is implicit: 1,0,1,0,...). */
typedef struct {
    bool valid;
    uint32_t seq;            /* frame sequence number */
    uint32_t timestamp_ms;   /* uptime in ms */

    /* raw signal features */
    uint32_t seg_count;      /* number of alternating segments */
    uint32_t total_us;       /* frame duration (trailing idle excluded) */
    uint32_t leader_pulse_us;
    uint32_t leader_space_us;
    uint32_t pulse_count;
    uint32_t min_pulse_us;   /* smallest data pulse (leader excluded) */
    uint32_t max_pulse_us;   /* largest data pulse (leader excluded) */
    uint32_t last_gap_us;

    /* NEC decode result */
    bool nec_ok;
    bool nec_repeat;         /* NEC repeat code */
    bool nec_chksum_ok;      /* address/command inverse bytes match */
    bool nec_ext_addr;       /* 16-bit (extended) NEC address */
    uint8_t nec_bits;        /* 32 for data frame, 0 for repeat */
    uint16_t nec_addr;
    uint16_t nec_cmd;
    uint32_t nec_raw;        /* full 32-bit NEC frame, LSB first -> the "hxd" value */

    /* raw waveform as durations (us), alternating levels starting with carrier-on */
    uint32_t raw_count;
    uint32_t raw_durs[IR_RAW_MAX_SEGS];
} ir_frame_t;

/**
 * Initialize RMT RX/TX, load carrier from NVS, create tasks and start receiving.
 * Must be called after nvs_flash_init().
 */
esp_err_t ir_init(void);

/**
 * Get the latest captured frame. Returns true once per new frame.
 */
bool ir_get_frame(ir_frame_t *out);

/**
 * Callback invoked from the IR task right after a new frame has been captured
 * and stored in the history ring. Keep it short (no blocking).
 */
typedef void (*ir_frame_cb_t)(const ir_frame_t *frame, void *arg);
void ir_set_frame_cb(ir_frame_cb_t cb, void *arg);

/**
 * Callback invoked right after the playback state changes
 * (true = playback started, false = playback finished).
 */
typedef void (*ir_play_cb_t)(bool playing, void *arg);
void ir_set_play_cb(ir_play_cb_t cb, void *arg);

/* ---- in-memory history ring (no filesystem) ---- */
uint32_t ir_history_count(void);
esp_err_t ir_history_get(uint32_t index, ir_frame_t *out);  /* 0 = oldest */
esp_err_t ir_history_get_latest(ir_frame_t *out);
uint32_t ir_get_latest_seq(void);

/* ---- carrier ---- */
#define IR_CARRIER_FREQ_MIN 1000U
#define IR_CARRIER_FREQ_MAX 1000000U
uint32_t ir_get_carrier_freq(void);
esp_err_t ir_set_carrier_freq(uint32_t freq_hz);            /* applies + persists to NVS */

/* ---- RX pause on playback (default on; persists to NVS) ---- */
bool ir_get_rx_pause_enabled(void);
esp_err_t ir_set_rx_pause_enabled(bool enabled);

/* ---- playback (freq_hz == 0 -> use current global carrier) ---- */
esp_err_t ir_play_hxd(const char *hex, uint32_t freq_hz);   /* e.g. "ED127F80", LSB order */
esp_err_t ir_play_raw(const uint32_t *durs, uint32_t count, uint32_t freq_hz);
esp_err_t ir_play_history(uint32_t index, uint32_t freq_hz);
bool ir_is_playing(void);
