#include <string.h>
#include "app_ir.h"
#include "app_ir_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define TAG "ir_store"

/* Multiple independent listeners are supported (e.g. WebSocket push + MQTT
 * publisher). Registration happens during init, before the callbacks fire. */
#define IR_CB_MAX 4

typedef struct {
    ir_frame_cb_t cb;
    void *arg;
} ir_frame_listener_t;

typedef struct {
    ir_play_cb_t cb;
    void *arg;
} ir_play_listener_t;

/* latest captured frame + history ring (shared with the RX task) */
static SemaphoreHandle_t s_mutex = NULL;
static ir_frame_t s_frame;
static bool s_frame_new;
static uint32_t s_seq;
static ir_frame_listener_t s_frame_listeners[IR_CB_MAX];
static ir_play_listener_t s_play_listeners[IR_CB_MAX];

static ir_frame_t s_history[IR_HISTORY_DEPTH];
static uint32_t s_history_head = 0;
static uint32_t s_history_count = 0;

esp_err_t ir_store_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Store a new frame: assign seq, update latest + history ring, then notify
 * the frame callback (called from the IR receive task). */
void ir_store_push(ir_frame_t *fr)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        fr->seq = ++s_seq;
        s_frame = *fr;
        s_frame_new = true;
        /* push into history ring */
        s_history[s_history_head] = *fr;
        s_history_head = (s_history_head + 1) % IR_HISTORY_DEPTH;
        if (s_history_count < IR_HISTORY_DEPTH) {
            s_history_count++;
        }
        xSemaphoreGive(s_mutex);
    }

    /* notify listeners (e.g. WebSocket push, MQTT publisher) about the new frame */
    for (int i = 0; i < IR_CB_MAX; i++) {
        if (s_frame_listeners[i].cb) {
            s_frame_listeners[i].cb(fr, s_frame_listeners[i].arg);
        }
    }
}

void ir_store_notify_play(bool playing)
{
    for (int i = 0; i < IR_CB_MAX; i++) {
        if (s_play_listeners[i].cb) {
            s_play_listeners[i].cb(playing, s_play_listeners[i].arg);
        }
    }
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

void ir_set_frame_cb(ir_frame_cb_t cb, void *arg)
{
    if (!cb) {
        for (int i = 0; i < IR_CB_MAX; i++) {
            s_frame_listeners[i].cb = NULL;
            s_frame_listeners[i].arg = NULL;
        }
        return;
    }
    for (int i = 0; i < IR_CB_MAX; i++) {
        if (s_frame_listeners[i].cb == cb) {
            s_frame_listeners[i].arg = arg;
            return;
        }
        if (s_frame_listeners[i].cb == NULL) {
            s_frame_listeners[i].cb = cb;
            s_frame_listeners[i].arg = arg;
            return;
        }
    }
    ESP_LOGW(TAG, "frame listener slots full, ignoring new callback");
}

void ir_set_play_cb(ir_play_cb_t cb, void *arg)
{
    if (!cb) {
        for (int i = 0; i < IR_CB_MAX; i++) {
            s_play_listeners[i].cb = NULL;
            s_play_listeners[i].arg = NULL;
        }
        return;
    }
    for (int i = 0; i < IR_CB_MAX; i++) {
        if (s_play_listeners[i].cb == cb) {
            s_play_listeners[i].arg = arg;
            return;
        }
        if (s_play_listeners[i].cb == NULL) {
            s_play_listeners[i].cb = cb;
            s_play_listeners[i].arg = arg;
            return;
        }
    }
    ESP_LOGW(TAG, "play listener slots full, ignoring new callback");
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
