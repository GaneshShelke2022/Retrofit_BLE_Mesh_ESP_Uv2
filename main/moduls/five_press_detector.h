/*
 * File    : five_press_detector.h
 * Purpose : Pure-function detector for 5-press sequence on mechanical switch #1.
 * Does    : Implements debounce filtering, gap validation, total window validation,
 *           and cross-switch reset logic with no hardware or RTOS dependencies.
 */

#ifndef FIVE_PRESS_DETECTOR_H
#define FIVE_PRESS_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t count;
    uint32_t first_press_ms;
    uint32_t last_press_ms;
    uint32_t press_gap_ms;
    uint32_t press_window_ms;
    uint32_t debounce_ms;
} five_press_detector_t;

/*
 * Initializes the detector with timing constraints.
 * press_gap_ms    : Maximum allowed gap between consecutive presses (default 1000 ms).
 * press_window_ms : Maximum allowed window for all 5 presses (default 6000 ms).
 * debounce_ms     : Contact debounce filter threshold (default 30-50 ms).
 */
void five_press_detector_init(five_press_detector_t *det,
                             uint32_t press_gap_ms,
                             uint32_t press_window_ms,
                             uint32_t debounce_ms);

/*
 * Resets the detector count and sequence window.
 */
void five_press_detector_reset(five_press_detector_t *det);

typedef enum {
    FIVE_PRESS_DECISION_NONE = 0,
    FIVE_PRESS_DECISION_FIRST_PRESS,
    FIVE_PRESS_DECISION_PROGRESS,
    FIVE_PRESS_DECISION_TRIGGER,
    FIVE_PRESS_DECISION_BOUNCE_REJECTED,
    FIVE_PRESS_DECISION_RESET_OTHER_SWITCH,
    FIVE_PRESS_DECISION_RESET_GAP_TOO_LONG,
    FIVE_PRESS_DECISION_RESET_WINDOW_EXCEEDED
} five_press_decision_t;

typedef struct {
    five_press_decision_t decision;
    uint32_t count_before;
    uint32_t count_after;
    uint32_t gap_ms;
    uint32_t window_ms;
    const char *reason_str;
} five_press_result_t;

/*
 * Feeds a switch event into the detector and returns detailed decision metrics.
 */
bool five_press_detector_feed_verbose(five_press_detector_t *det,
                                     uint8_t switch_idx,
                                     uint32_t timestamp_ms,
                                     five_press_result_t *out_result);

/*
 * Feeds a switch event into the detector.
 * switch_idx   : 0 for switch #1, non-zero for any other switch.
 * timestamp_ms : Monotonic timestamp in milliseconds.
 *
 * Returns true if exactly 5 valid presses are completed within the constraints.
 * On trigger, the count is reset to 0 so subsequent presses require a new sequence.
 */
bool five_press_detector_feed(five_press_detector_t *det,
                             uint8_t switch_idx,
                             uint32_t timestamp_ms);

#ifdef __cplusplus
}
#endif

#endif // FIVE_PRESS_DETECTOR_H
