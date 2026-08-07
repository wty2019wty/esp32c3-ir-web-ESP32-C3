#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_ir.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Alternating level segment (duration in us + level).
 * Stored normalized so the first segment is carrier-on (level 1). */
typedef struct {
    uint32_t dur;
    uint8_t level;
} ir_seg_t;

/* NEC protocol timing (us), shared by the decoder (app_ir_nec.c) and the
 * hxd encoder (app_ir_play.c). */
#define NEC_LEAD_PULSE_US   9000U
#define NEC_LEAD_SPACE_US   4500U
#define NEC_REPEAT_SPACE_US 2250U
#define NEC_BIT_PULSE_US    560U
#define NEC_BIT0_SPACE_US   560U
#define NEC_BIT1_SPACE_US   1690U

/* ---- app_ir_store.c (frame store / history / callbacks) ---- */
esp_err_t ir_store_init(void);
void ir_store_push(ir_frame_t *fr);        /* assigns seq, stores, notifies frame cb */
void ir_store_notify_play(bool playing);

/* ---- app_ir.c (RMT driver core / playback engine) ---- */
esp_err_t ir_play_durs(const uint32_t *durs, uint32_t count, uint32_t freq_hz);

/* ---- app_ir_nec.c (NEC protocol decode) ---- */
void ir_nec_decode(const ir_seg_t *segs, int start, int end, ir_frame_t *f);

#ifdef __cplusplus
}
#endif
