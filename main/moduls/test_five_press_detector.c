/*
 * File    : test_five_press_detector.c
 * Purpose : Host unit test suite for 5-press detector pure function.
 * Does    : Verifies exact 5, 4, 6, too slow (gap & window), bouncy contacts,
 *           restart after a reset, and long idle.
 */

#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include "five_press_detector.h"

static int s_tests_passed = 0;
static int s_tests_run = 0;

#define TEST_ASSERT(cond, msg) do { \
    s_tests_run++; \
    if (!(cond)) { \
        printf("[FAIL] Line %d: %s\n", __LINE__, msg); \
        assert(cond); \
    } else { \
        s_tests_passed++; \
    } \
} while(0)

/* Test 1: Exact 5 presses within gap and window */
static void test_exact_5(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1 should not trigger");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1500), "Press 2 should not trigger");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2000), "Press 3 should not trigger");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2500), "Press 4 should not trigger");
    TEST_ASSERT(five_press_detector_feed(&det, 0, 3000), "Press 5 should trigger");
}

/* Test 2: 4 presses only, then idle (should never trigger) */
static void test_4_presses(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1 should not trigger");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1400), "Press 2 should not trigger");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1800), "Press 3 should not trigger");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2200), "Press 4 should not trigger");
    TEST_ASSERT(det.count == 4, "Count should be 4");
}

/* Test 3: 6 presses (triggers on 5th, 6th starts new sequence) */
static void test_6_presses(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1500), "Press 2");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2000), "Press 3");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2500), "Press 4");
    TEST_ASSERT(five_press_detector_feed(&det, 0, 3000), "Press 5 triggers");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3500), "Press 6 should start fresh sequence, not trigger");
    TEST_ASSERT(det.count == 1, "Count should be 1 after press 6");
}

/* Test 4: Too slow gap (> 1000 ms gap restarts count) */
static void test_too_slow_gap(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1500), "Press 2");
    /* Gap is 1200 ms > 1000 ms, restarts count to 1 */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2700), "Press 3 (gap 1200) restarts to count 1");
    TEST_ASSERT(det.count == 1, "Count should restart to 1");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3200), "Press 4 -> count 2");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3700), "Press 5 -> count 3");
    TEST_ASSERT(det.count == 3, "Count should be 3");
}

/* Test 5: Too slow window (> 6000 ms total window restarts count) */
static void test_too_slow_window(void)
{
    five_press_detector_t det;
    /* Use gap 1000, window 3000 to test window constraint */
    five_press_detector_init(&det, 1000, 3000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1 (t=1000)");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1800), "Press 2 (t=1800)");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2600), "Press 3 (t=2600)");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3400), "Press 4 (t=3400)");
    /* Total window from press 1 is 4200 - 1000 = 3200 > 3000 ms window! */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 4200), "Press 5 (window 3200) restarts count to 1");
    TEST_ASSERT(det.count == 1, "Count should be 1 after window timeout");
}

/* Test 6: Bouncy contacts (< 40 ms ignored) */
static void test_bouncy_contacts(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1");
    /* Contact bounce at +10 ms and +25 ms */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1010), "Bounce 1 (10ms) ignored");
    TEST_ASSERT(det.count == 1, "Count should still be 1");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1025), "Bounce 2 (25ms) ignored");
    TEST_ASSERT(det.count == 1, "Count should still be 1");

    /* Genuine press 2 */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1500), "Press 2");
    TEST_ASSERT(det.count == 2, "Count should be 2");

    /* Bounce on press 2 */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1515), "Bounce on press 2 ignored");
    TEST_ASSERT(det.count == 2, "Count should still be 2");

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2000), "Press 3");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2500), "Press 4");
    TEST_ASSERT(five_press_detector_feed(&det, 0, 3000), "Press 5 triggers");

    /* Bounce on press 5 (trigger) */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3020), "Bounce on press 5 ignored");
    TEST_ASSERT(det.count == 0, "Count should still be 0");
}

/* Test 7: Restart after a reset (press of another switch resets count) */
static void test_restart_after_reset(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1500), "Press 2");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2000), "Press 3");
    TEST_ASSERT(det.count == 3, "Count is 3");

    /* User accidentally flips switch #2 (index 1) */
    TEST_ASSERT(!five_press_detector_feed(&det, 1, 2300), "Switch 2 pressed -> resets count");
    TEST_ASSERT(det.count == 0, "Count should be reset to 0");

    /* Now user does 5 presses on switch #1 */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 2600), "Press 1 of new sequence");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3100), "Press 2");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 3600), "Press 3");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 4100), "Press 4");
    TEST_ASSERT(five_press_detector_feed(&det, 0, 4600), "Press 5 triggers!");
}

/* Test 8: Long idle */
static void test_long_idle(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1000, 6000, 40);

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1000), "Press 1");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 1500), "Press 2");
    TEST_ASSERT(det.count == 2, "Count is 2");

    /* Idle for 60 seconds (60,000 ms) */
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 61500), "Press after 60s idle restarts to count 1");
    TEST_ASSERT(det.count == 1, "Count is 1");

    TEST_ASSERT(!five_press_detector_feed(&det, 0, 62000), "Press 2");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 62500), "Press 3");
    TEST_ASSERT(!five_press_detector_feed(&det, 0, 63000), "Press 4");
    TEST_ASSERT(five_press_detector_feed(&det, 0, 63500), "Press 5 triggers!");
}

/* Test 9: Verbose decision feedback */
static void test_verbose_decisions(void)
{
    five_press_detector_t det;
    five_press_detector_init(&det, 1500, 10000, 40);
    five_press_result_t res;

    bool trig = five_press_detector_feed_verbose(&det, 0, 1000, &res);
    TEST_ASSERT(!trig && res.decision == FIVE_PRESS_DECISION_FIRST_PRESS, "Press 1 should be FIRST_PRESS");
    TEST_ASSERT(res.count_before == 0 && res.count_after == 1, "Count 0 -> 1");

    /* Contact bounce at +10 ms */
    trig = five_press_detector_feed_verbose(&det, 0, 1010, &res);
    TEST_ASSERT(!trig && res.decision == FIVE_PRESS_DECISION_BOUNCE_REJECTED, "Bounce should be rejected");

    /* Press 2 */
    trig = five_press_detector_feed_verbose(&det, 0, 1400, &res);
    TEST_ASSERT(!trig && res.decision == FIVE_PRESS_DECISION_PROGRESS, "Press 2 progress");
    TEST_ASSERT(res.gap_ms == 400, "Gap 400 ms");

    /* Other switch reset */
    trig = five_press_detector_feed_verbose(&det, 1, 1800, &res);
    TEST_ASSERT(!trig && res.decision == FIVE_PRESS_DECISION_RESET_OTHER_SWITCH, "Other switch reset");
    TEST_ASSERT(res.count_after == 0, "Count reset to 0");
}

int main(void)
{
    printf("=== Running 5-Press Detector Host Unit Tests ===\n");
    test_exact_5();
    test_4_presses();
    test_6_presses();
    test_too_slow_gap();
    test_too_slow_window();
    test_bouncy_contacts();
    test_restart_after_reset();
    test_long_idle();
    test_verbose_decisions();
    printf("=== All %d checks PASSED! ===\n", s_tests_passed);
    return 0;
}
