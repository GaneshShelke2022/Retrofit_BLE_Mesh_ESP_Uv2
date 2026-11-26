/*
 * File    : five_press_detector.c
 * Purpose : Pure-function detector implementation for 5-press trigger.
 * Does    : Evaluates timestamps against gap and window rules; pure logic.
 */

#include "five_press_detector.h"

void five_press_detector_init(five_press_detector_t *det,
                             uint32_t press_gap_ms,
                             uint32_t press_window_ms,
                             uint32_t debounce_ms)
{
    if (!det) {
        return;
    }
    det->count = 0;
    det->first_press_ms = 0;
    det->last_press_ms = 0;
    det->press_gap_ms = press_gap_ms;
    det->press_window_ms = press_window_ms;
    det->debounce_ms = debounce_ms;
}

void five_press_detector_reset(five_press_detector_t *det)
{
    if (!det) {
        return;
    }
    det->count = 0;
    det->first_press_ms = 0;
    det->last_press_ms = 0;
}

bool five_press_detector_feed_verbose(five_press_detector_t *det,
                                     uint8_t switch_idx,
                                     uint32_t timestamp_ms,
                                     five_press_result_t *out_result)
{
    if (!det) {
        return false;
    }

    five_press_result_t res = {
        .decision = FIVE_PRESS_DECISION_NONE,
        .count_before = det->count,
        .count_after = det->count,
        .gap_ms = 0,
        .window_ms = 0,
        .reason_str = "idle",
    };

    /* A press of any switch other than switch #1 resets the detector count */
    if (switch_idx != 0) {
        det->count = 0;
        det->first_press_ms = 0;
        det->last_press_ms = timestamp_ms;
        res.decision = FIVE_PRESS_DECISION_RESET_OTHER_SWITCH;
        res.count_after = 0;
        res.reason_str = "reset: other switch pressed";
        if (out_result) *out_result = res;
        return false;
    }

    /* Contact debounce filter: reject rapid edges occurring within debounce_ms */
    if (det->last_press_ms != 0 && (timestamp_ms - det->last_press_ms) < det->debounce_ms) {
        res.decision = FIVE_PRESS_DECISION_BOUNCE_REJECTED;
        res.gap_ms = timestamp_ms - det->last_press_ms;
        res.count_after = det->count;
        res.reason_str = "debounce: contact bounce rejected";
        if (out_result) *out_result = res;
        return false;
    }

    if (det->count > 0) {
        uint32_t gap = timestamp_ms - det->last_press_ms;
        uint32_t total_window = timestamp_ms - det->first_press_ms;
        res.gap_ms = gap;
        res.window_ms = total_window;

        /* If gap exceeds maximum allowed gap, restart count with this press as #1 */
        if (gap > det->press_gap_ms) {
            det->count = 1;
            det->first_press_ms = timestamp_ms;
            det->last_press_ms = timestamp_ms;
            res.decision = FIVE_PRESS_DECISION_RESET_GAP_TOO_LONG;
            res.count_after = 1;
            res.reason_str = "reset: gap too long (> max gap)";
            if (out_result) *out_result = res;
            return false;
        }

        /* If total sequence time exceeds window, restart count with this press as #1 */
        if (total_window > det->press_window_ms) {
            det->count = 1;
            det->first_press_ms = timestamp_ms;
            det->last_press_ms = timestamp_ms;
            res.decision = FIVE_PRESS_DECISION_RESET_WINDOW_EXCEEDED;
            res.count_after = 1;
            res.reason_str = "reset: total window exceeded (> max window)";
            if (out_result) *out_result = res;
            return false;
        }

        /* Valid subsequent press */
        det->count++;
        det->last_press_ms = timestamp_ms;
        res.count_after = det->count;

        if (det->count == 5) {
            /* Trigger reached! Reset sequence state for next trigger cycle */
            det->count = 0;
            det->first_press_ms = 0;
            res.decision = FIVE_PRESS_DECISION_TRIGGER;
            res.reason_str = "TRIGGER: 5 valid presses completed";
            if (out_result) *out_result = res;
            return true;
        }

        res.decision = FIVE_PRESS_DECISION_PROGRESS;
        res.reason_str = "press counted";
        if (out_result) *out_result = res;
        return false;
    } else {
        /* First press of a new sequence */
        det->count = 1;
        det->first_press_ms = timestamp_ms;
        det->last_press_ms = timestamp_ms;
        res.decision = FIVE_PRESS_DECISION_FIRST_PRESS;
        res.count_after = 1;
        res.reason_str = "sequence started (press #1)";
        if (out_result) *out_result = res;
        return false;
    }
}

bool five_press_detector_feed(five_press_detector_t *det,
                             uint8_t switch_idx,
                             uint32_t timestamp_ms)
{
    return five_press_detector_feed_verbose(det, switch_idx, timestamp_ms, NULL);
}
