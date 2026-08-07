#include "app_ir.h"
#include "app_ir_internal.h"

static inline bool dur_in_range(uint32_t v, uint32_t nom, uint32_t margin)
{
    return (v >= nom - margin) && (v <= nom + margin);
}

/* NEC decode over polarity-normalized alternating segments [start, end):
 * scan for the 9 ms leader (polarity agnostic). Writes f->nec_* fields. */
void ir_nec_decode(const ir_seg_t *segs, int start, int end, ir_frame_t *f)
{
    f->nec_ok = false;
    f->nec_repeat = false;
    f->nec_chksum_ok = false;
    f->nec_ext_addr = false;
    f->nec_bits = 0;
    f->nec_addr = 0;
    f->nec_cmd = 0;
    f->nec_raw = 0;

    for (int i = start; i + 1 < end; i++) {
        if (segs[i + 1].level == segs[i].level) {
            continue;
        }
        if (!dur_in_range(segs[i].dur, NEC_LEAD_PULSE_US, 1200)) {
            continue;
        }

        /* repeat code: 9ms + 2.25ms + 560us */
        if (dur_in_range(segs[i + 1].dur, NEC_REPEAT_SPACE_US, 500)) {
            if (i + 2 < end && dur_in_range(segs[i + 2].dur, NEC_BIT_PULSE_US, 300)) {
                f->nec_ok = true;
                f->nec_repeat = true;
            }
            return;
        }
        if (!dur_in_range(segs[i + 1].dur, NEC_LEAD_SPACE_US, 700)) {
            continue;
        }

        /* 32 data bits, LSB first: (pulse, space) pairs */
        uint32_t raw = 0;
        bool good = true;
        for (int b = 0; b < 32; b++) {
            int p = i + 2 + b * 2;
            if (p + 1 >= end || !dur_in_range(segs[p].dur, NEC_BIT_PULSE_US, 300)) {
                good = false;
                break;
            }
            if (dur_in_range(segs[p + 1].dur, NEC_BIT0_SPACE_US, 350)) {
                /* logic 0 */
            } else if (dur_in_range(segs[p + 1].dur, NEC_BIT1_SPACE_US, 450)) {
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
