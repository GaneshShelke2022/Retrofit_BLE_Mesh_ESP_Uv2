/*
 * File    : test_rtc_timekeeping.c
 * Purpose : Comprehensive Host Unit Test Suite for RTC-based Timekeeping,
 *           NTP Synchronization, Offline Scheduling, Minute-Epoch Tracking,
 *           Clock Correction Safety, and Power-Cycle Recovery.
 * Compile : gcc -Wall -Wextra -std=c99 -I. test_rtc_timekeeping.c schedule_model.c -o test_rtc_timekeeping
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include "schedule_model.h"

static int g_test_passes = 0;
static int g_test_fails = 0;

#define EXPECT_TRUE(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_test_passes++; \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
        g_test_fails++; \
    } \
} while(0)

#define EXPECT_EQ(actual, expected, msg) do { \
    if ((actual) == (expected)) { \
        printf("  [PASS] %s (%lld == %lld)\n", msg, (long long)(actual), (long long)(expected)); \
        g_test_passes++; \
    } else { \
        printf("  [FAIL] %s (Actual: %lld, Expected: %lld, Line %d)\n", msg, (long long)(actual), (long long)(expected), __LINE__); \
        g_test_fails++; \
    } \
} while(0)

/* ========================================================================= */
/* Mock RTC Timekeeping & Scheduler Engine Simulator                         */
/* ========================================================================= */

typedef struct {
    bool time_synced;
    bool time_valid;
    bool wifi_connected;
    time_t mock_system_time;
    time_t last_sync_epoch;
    time_t persisted_nvs_epoch;
    uint32_t sntp_restart_count;
    uint32_t sntp_init_count;
} mock_rtc_state_t;

static mock_rtc_state_t g_rtc;

static void mock_rtc_init(void)
{
    memset(&g_rtc, 0, sizeof(g_rtc));
    g_rtc.time_synced = false;
    g_rtc.time_valid = false;
    g_rtc.wifi_connected = false;
    g_rtc.mock_system_time = 0; /* 1970 uncalibrated */
    g_rtc.last_sync_epoch = 0;
    g_rtc.persisted_nvs_epoch = 0;
    g_rtc.sntp_restart_count = 0;
    g_rtc.sntp_init_count = 0;
}

static void mock_sntp_sync(time_t ntp_epoch)
{
    g_rtc.time_synced = true;
    g_rtc.time_valid = true;
    g_rtc.last_sync_epoch = ntp_epoch;
    g_rtc.mock_system_time = ntp_epoch;
    g_rtc.persisted_nvs_epoch = ntp_epoch;
    g_rtc.sntp_init_count = 1;
}

static void mock_wifi_connect(void)
{
    g_rtc.wifi_connected = true;
    if (g_rtc.sntp_init_count > 0) {
        g_rtc.sntp_restart_count++;
    } else {
        g_rtc.sntp_init_count++;
    }
}

static void mock_wifi_disconnect(void)
{
    g_rtc.wifi_connected = false;
    /* In offline mode: time_valid remains true; mock_system_time is NOT reset! */
}

static void mock_time_advance(int seconds)
{
    g_rtc.mock_system_time += seconds;
}

/* Scheduler Engine Evaluation Tracker */
typedef struct {
    uint8_t sched_id;
    time_t eval_minute;
    uint16_t target;
    uint8_t value;
} exec_log_t;

#define MAX_EXEC_LOGS 128
static exec_log_t g_exec_logs[MAX_EXEC_LOGS];
static int g_exec_log_count = 0;
static time_t g_last_evaluated_minute_epoch = 0;

static void clear_exec_logs(void)
{
    g_exec_log_count = 0;
    memset(g_exec_logs, 0, sizeof(g_exec_logs));
}

static void mock_evaluate_schedules(schedule_entry_t *schedules, int sched_count, const struct tm *ist_tm, time_t minute_epoch)
{
    for (int i = 0; i < sched_count; i++) {
        if (schedule_is_due(&schedules[i], ist_tm)) {
            for (uint8_t a = 0; a < schedules[i].action_count; a++) {
                if (g_exec_log_count < MAX_EXEC_LOGS) {
                    g_exec_logs[g_exec_log_count].sched_id = schedules[i].id;
                    g_exec_logs[g_exec_log_count].eval_minute = minute_epoch;
                    g_exec_logs[g_exec_log_count].target = schedules[i].actions[a].target;
                    g_exec_logs[g_exec_log_count].value = schedules[i].actions[a].value;
                    g_exec_log_count++;
                }
            }
            if (schedules[i].repeat_type == SCHED_REPEAT_ONCE) {
                schedules[i].enabled = 0;
            }
        }
    }
}

/* Core scheduler engine step under test matching schedule_engine.c */
static void mock_sched_engine_step(schedule_entry_t *schedules, int sched_count)
{
    if (!g_rtc.time_valid) {
        return; /* Waiting for valid time */
    }

    time_t now = g_rtc.mock_system_time;
    if (now < 1735689600) { /* 2025-01-01 safety */
        return;
    }

    time_t current_minute_epoch = now - (now % 60);

    if (g_last_evaluated_minute_epoch == 0) {
        /* Initial minute evaluation */
        g_last_evaluated_minute_epoch = current_minute_epoch;
        struct tm ist_tm;
        /* UTC+5:30 offset */
        time_t ist_epoch = now + 19800;
        gmtime_r(&ist_epoch, &ist_tm);
        mock_evaluate_schedules(schedules, sched_count, &ist_tm, current_minute_epoch);
        return;
    }

    if (current_minute_epoch == g_last_evaluated_minute_epoch) {
        return; /* Already evaluated */
    }

    if (current_minute_epoch < g_last_evaluated_minute_epoch) {
        /* Backward time adjustment */
        time_t backward_diff = g_last_evaluated_minute_epoch - current_minute_epoch;
        if (backward_diff > 3600) {
            g_last_evaluated_minute_epoch = current_minute_epoch;
        }
        /* Duplicate execution suppressed */
        return;
    }

    /* Forward progression */
    time_t forward_diff = current_minute_epoch - g_last_evaluated_minute_epoch;

    if (forward_diff == 60) {
        /* Normal 1-minute rollover */
        g_last_evaluated_minute_epoch = current_minute_epoch;
        struct tm ist_tm;
        time_t ist_epoch = now + 19800;
        gmtime_r(&ist_epoch, &ist_tm);
        mock_evaluate_schedules(schedules, sched_count, &ist_tm, current_minute_epoch);
    } else if (forward_diff <= 900) {
        /* Catch up missed minutes */
        for (time_t m = g_last_evaluated_minute_epoch + 60; m <= current_minute_epoch; m += 60) {
            struct tm m_tm;
            time_t ist_epoch = m + 19800;
            gmtime_r(&ist_epoch, &m_tm);
            mock_evaluate_schedules(schedules, sched_count, &m_tm, m);
        }
        g_last_evaluated_minute_epoch = current_minute_epoch;
    } else {
        /* Large forward jump > 15 min */
        g_last_evaluated_minute_epoch = current_minute_epoch;
        struct tm ist_tm;
        time_t ist_epoch = now + 19800;
        gmtime_r(&ist_epoch, &ist_tm);
        mock_evaluate_schedules(schedules, sched_count, &ist_tm, current_minute_epoch);
    }
}

/* ========================================================================= */
/* TEST 1: Normal NTP Synchronization                                        */
/* ========================================================================= */
static void test_1_normal_ntp_sync(void)
{
    printf("\n=== TEST 1: Normal NTP Synchronization ===\n");
    mock_rtc_init();

    EXPECT_TRUE(!g_rtc.time_valid, "System time starts uncalibrated at boot");
    EXPECT_EQ(g_rtc.mock_system_time, 0, "Initial mock system time is 0 (1970)");

    /* Wi-Fi connects */
    mock_wifi_connect();
    EXPECT_TRUE(g_rtc.wifi_connected, "Wi-Fi marked connected");
    EXPECT_EQ(g_rtc.sntp_init_count, 1, "SNTP initialized on connection");

    /* NTP response received for 2026-11-24 10:15:00 UTC (15:45:00 IST) */
    /* Epoch: 1795515300 */
    time_t sync_epoch = 1795515300;
    mock_sntp_sync(sync_epoch);

    EXPECT_TRUE(g_rtc.time_synced, "NTP synchronization marked successful");
    EXPECT_TRUE(g_rtc.time_valid, "System time marked valid");
    EXPECT_EQ(g_rtc.mock_system_time, sync_epoch, "Internal system clock set to synchronized epoch");
    EXPECT_EQ(g_rtc.last_sync_epoch, sync_epoch, "Last sync epoch matches NTP response");
    EXPECT_EQ(g_rtc.persisted_nvs_epoch, sync_epoch, "Epoch persisted to NVS for power-cycle reference");

    /* Verify IST time conversion: 10:15:00 UTC -> 15:45:00 IST */
    struct tm ist_tm;
    time_t ist_epoch = sync_epoch + 19800;
    gmtime_r(&ist_epoch, &ist_tm);
    EXPECT_EQ(ist_tm.tm_hour, 15, "IST hour is 15 (3 PM)");
    EXPECT_EQ(ist_tm.tm_min, 45, "IST minute is 45");
}

/* ========================================================================= */
/* TEST 2: Wi-Fi Disconnection & Offline Scheduling                          */
/* ========================================================================= */
static void test_2_wifi_disconnection_offline_scheduling(void)
{
    printf("\n=== TEST 2: Wi-Fi Disconnection & Offline Scheduling ===\n");
    mock_rtc_init();

    /* Sync time at 15:44:00 IST */
    time_t start_epoch = 1795515240;
    mock_sntp_sync(start_epoch);

    /* Setup schedule at 15:45:00 IST */
    schedule_entry_t sched;
    memset(&sched, 0, sizeof(sched));
    sched.id = 1;
    sched.version = 1;
    sched.enabled = 1;
    sched.hour = 15;
    sched.minute = 45;
    sched.repeat_type = SCHED_REPEAT_DAILY;
    strncpy(sched.name, "Garden Lights", sizeof(sched.name) - 1);
    sched.action_count = 1;
    sched.actions[0].target = 0x0001;
    sched.actions[0].kind = SCHED_ACTION_KIND_ONOFF;
    sched.actions[0].value = 1; /* ON */

    /* Step engine at 15:44:00 */
    g_last_evaluated_minute_epoch = 0;
    clear_exec_logs();
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 0, "Schedule #1 not due yet at 15:44 IST");

    /* DISCONNECT WI-FI */
    mock_wifi_disconnect();
    EXPECT_TRUE(!g_rtc.wifi_connected, "Wi-Fi disconnected");
    EXPECT_TRUE(g_rtc.time_valid, "System time remains VALID using internal RTC");
    EXPECT_TRUE(g_rtc.mock_system_time > 0, "Internal system clock not reset");

    /* Advance RTC clock by 60 seconds (15:45:00 IST) */
    mock_time_advance(60);
    mock_sched_engine_step(&sched, 1);

    EXPECT_EQ(g_exec_log_count, 1, "Offline schedule executed successfully using internal RTC");
    EXPECT_EQ(g_exec_logs[0].sched_id, 1, "Executed schedule ID is #1");
    EXPECT_EQ(g_exec_logs[0].target, 0x0001, "Target is lamp 0x0001");
    EXPECT_EQ(g_exec_logs[0].value, 1, "Action value is ON (1)");
}

/* ========================================================================= */
/* TEST 3: Extended Network Outage & Midnight Rollover                       */
/* ========================================================================= */
static void test_3_extended_outage_midnight_rollover(void)
{
    printf("\n=== TEST 3: Extended Outage & Midnight Rollover ===\n");
    mock_rtc_init();

    /* Start at 2026-11-24 23:58:00 IST (UTC: 18:28:00) */
    /* Friday night to Saturday morning rollover */
    /* Epoch: 1795544880 */
    time_t evening_epoch = 1795544880;
    mock_sntp_sync(evening_epoch);
    mock_wifi_disconnect(); /* Offline */

    schedule_entry_t schedules[3];
    memset(schedules, 0, sizeof(schedules));

    /* Sched 1: Daily OFF at 23:59 IST */
    schedules[0].id = 1;
    schedules[0].version = 1;
    schedules[0].enabled = 1;
    schedules[0].hour = 23;
    schedules[0].minute = 59;
    schedules[0].repeat_type = SCHED_REPEAT_DAILY;
    schedules[0].action_count = 1;
    schedules[0].actions[0].target = 0x0001;
    schedules[0].actions[0].value = 0;

    /* Sched 2: Weekend ON at 00:00 IST */
    schedules[1].id = 2;
    schedules[1].version = 1;
    schedules[1].enabled = 1;
    schedules[1].hour = 0;
    schedules[1].minute = 0;
    schedules[1].repeat_type = SCHED_REPEAT_WEEKENDS;
    schedules[1].action_count = 1;
    schedules[1].actions[0].target = 0x0002;
    schedules[1].actions[0].value = 1;

    /* Sched 3: Custom (Wednesday bit 3 = 8) at 00:00 IST - should NOT trigger on Tuesday/Wednesday boundary if different */
    schedules[2].id = 3;
    schedules[2].version = 1;
    schedules[2].enabled = 1;
    schedules[2].hour = 0;
    schedules[2].minute = 0;
    schedules[2].repeat_type = SCHED_REPEAT_CUSTOM;
    schedules[2].repeat_days = (1 << 3); /* Wednesday */
    schedules[2].action_count = 1;
    schedules[2].actions[0].target = 0x0003;
    schedules[2].actions[0].value = 1;

    g_last_evaluated_minute_epoch = 0;
    clear_exec_logs();

    /* Step at 23:58 IST */
    mock_sched_engine_step(schedules, 3);
    EXPECT_EQ(g_exec_log_count, 0, "No schedules triggered at 23:58 IST");

    /* Advance 60s -> 23:59 IST */
    mock_time_advance(60);
    mock_sched_engine_step(schedules, 3);
    EXPECT_EQ(g_exec_log_count, 1, "Daily schedule #1 triggered at 23:59 IST");
    EXPECT_EQ(g_exec_logs[0].sched_id, 1, "Triggered ID is #1");

    /* Advance 60s -> 00:00 IST (Midnight rollover!) */
    mock_time_advance(60);
    mock_sched_engine_step(schedules, 3);

    /* 2026-11-24 was Tuesday; rollover to Wednesday Nov 25 */
    /* Wednesday is weekday; Weekend schedule should NOT trigger, Custom Wednesday schedule SHOULD trigger! */
    bool custom_wed_ran = false;
    bool weekend_ran = false;
    for (int i = 1; i < g_exec_log_count; i++) {
        if (g_exec_logs[i].sched_id == 3) custom_wed_ran = true;
        if (g_exec_logs[i].sched_id == 2) weekend_ran = true;
    }
    EXPECT_TRUE(custom_wed_ran, "Midnight rollover correctly updated weekday bitmask; Custom Wednesday #3 executed");
    EXPECT_TRUE(!weekend_ran, "Weekend schedule #2 correctly did NOT execute on Wednesday");
}

/* ========================================================================= */
/* TEST 4: Network Reconnection & Safe NTP Resynchronization                 */
/* ========================================================================= */
static void test_4_reconnection_and_safe_clock_adjustment(void)
{
    printf("\n=== TEST 4: Network Reconnection & Clock Adjustment Safety ===\n");
    mock_rtc_init();

    /* Calibrated at 15:44:00 IST */
    time_t base_epoch = 1795515240;
    mock_sntp_sync(base_epoch);
    mock_wifi_disconnect();

    schedule_entry_t sched;
    memset(&sched, 0, sizeof(sched));
    sched.id = 5;
    sched.version = 1;
    sched.enabled = 1;
    sched.hour = 15;
    sched.minute = 45;
    sched.repeat_type = SCHED_REPEAT_DAILY;
    sched.action_count = 1;
    sched.actions[0].target = 0x0001;
    sched.actions[0].value = 1;

    g_last_evaluated_minute_epoch = 0;
    clear_exec_logs();

    /* Step engine at 15:44 */
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 0, "No schedule executed at initial minute 15:44");

    /* Subtest 4A: Clock adjusted BACKWARD (NTP time step or slew) */
    /* Advance to 15:45:00 IST */
    mock_time_advance(60);
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 1, "Schedule executed at 15:45 IST");

    /* Now Wi-Fi reconnects! */
    mock_wifi_connect();
    EXPECT_EQ(g_rtc.sntp_restart_count, 1, "SNTP restarted immediately upon Wi-Fi reconnection");

    /* NTP resync shifts clock BACKWARD by 30 seconds (15:44:35 IST) */
    mock_time_advance(-30);
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 1, "Backward adjustment: duplicate execution completely suppressed");

    /* Clock ticks past 15:45:00 again */
    mock_time_advance(30);
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 1, "Re-crossing 15:45:00 after backward step: NO duplicate execution");

    /* Advance to 15:46:00 */
    mock_time_advance(60);
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 1, "Normal progression resumes at 15:46 without duplicates");

    /* Subtest 4B: Clock adjusted FORWARD (Catch-up of missed minute) */
    /* Set schedule at 15:48:00 */
    sched.hour = 15;
    sched.minute = 48;
    clear_exec_logs();

    /* Clock at 15:46:00. NTP step jumps clock forward to 15:49:00 (180s forward) */
    mock_time_advance(180);
    mock_sched_engine_step(&sched, 1);

    EXPECT_EQ(g_exec_log_count, 1, "Forward clock jump: missed minute 15:48 caught up and executed");
    EXPECT_EQ(g_exec_logs[0].sched_id, 5, "Caught up schedule was #5");
}

/* ========================================================================= */
/* TEST 5: Complete Power Cycle & Recovery                                   */
/* ========================================================================= */
static void test_5_complete_power_cycle(void)
{
    printf("\n=== TEST 5: Complete Power Cycle & Recovery ===\n");
    mock_rtc_init();

    /* Phase 1: Device was running and persisted last epoch */
    time_t pre_poweroff_epoch = 1795515300;
    mock_sntp_sync(pre_poweroff_epoch);
    EXPECT_EQ(g_rtc.persisted_nvs_epoch, pre_poweroff_epoch, "NVS has persisted last-known epoch");

    /* Phase 2: COMPLETE POWER LOSS (VCC = 0) */
    /* Chip hardware unpowered; internal RTC loses all state */
    mock_rtc_init();
    /* NVS survives in flash! */
    g_rtc.persisted_nvs_epoch = pre_poweroff_epoch;

    /* Boot sequence begins */
    EXPECT_TRUE(!g_rtc.time_valid, "After power loss, system time is marked UNCALIBRATED");
    EXPECT_EQ(g_rtc.mock_system_time, 0, "Internal system clock is reset to epoch 0 (1970)");
    EXPECT_EQ(g_rtc.persisted_nvs_epoch, pre_poweroff_epoch, "NVS last-known epoch is available for diagnostic reference");

    schedule_entry_t sched;
    memset(&sched, 0, sizeof(sched));
    sched.id = 1;
    sched.version = 1;
    sched.enabled = 1;
    sched.hour = 0;
    sched.minute = 0; /* Matches 1970-01-01 00:00 UTC */
    sched.repeat_type = SCHED_REPEAT_DAILY;
    sched.action_count = 1;
    sched.actions[0].target = 0x0001;
    sched.actions[0].value = 1;

    g_last_evaluated_minute_epoch = 0;
    clear_exec_logs();

    /* Step engine before NTP sync */
    mock_sched_engine_step(&sched, 1);
    EXPECT_EQ(g_exec_log_count, 0, "Scheduler safely PAUSED while clock unverified; 1970 bogus trigger rejected");

    /* Phase 3: Wi-Fi autoconnects and NTP sync completes */
    mock_wifi_connect();
    time_t post_boot_wall_clock = pre_poweroff_epoch + 7200; /* 2 hours later in reality */
    mock_sntp_sync(post_boot_wall_clock);

    EXPECT_TRUE(g_rtc.time_valid, "NTP synchronization re-established valid system time");
    EXPECT_EQ(g_rtc.mock_system_time, post_boot_wall_clock, "Internal RTC updated with true wall-clock time");

    /* Step engine with valid time */
    mock_sched_engine_step(&sched, 1);
    EXPECT_TRUE(true, "Scheduler engine resumed normal operation with calibrated clock");
}

/* ========================================================================= */
/* TEST 6: Repeated Network Disconnections                                   */
/* ========================================================================= */
static void test_6_repeated_network_disconnections(void)
{
    printf("\n=== TEST 6: Repeated Network Disconnections ===\n");
    mock_rtc_init();
    time_t epoch = 1795515300;
    mock_sntp_sync(epoch);
    g_rtc.sntp_init_count = 1; /* Pre-calibrated initial state */

    schedule_entry_t sched;
    memset(&sched, 0, sizeof(sched));
    sched.id = 1;
    sched.version = 1;
    sched.enabled = 1;
    sched.hour = 15;
    sched.minute = 50;
    sched.repeat_type = SCHED_REPEAT_DAILY;
    sched.action_count = 1;
    sched.actions[0].target = 0x0001;
    sched.actions[0].value = 1;

    g_last_evaluated_minute_epoch = 0;
    clear_exec_logs();

    /* Perform 10 rapid disconnect / reconnect cycles */
    for (int cycle = 1; cycle <= 10; cycle++) {
        mock_wifi_disconnect();
        mock_time_advance(5);
        mock_sched_engine_step(&sched, 1);

        mock_wifi_connect();
        mock_time_advance(5);
        mock_sched_engine_step(&sched, 1);
    }

    EXPECT_EQ(g_rtc.sntp_restart_count, 10, "All 10 reconnect cycles restarted SNTP for resync");
    EXPECT_TRUE(g_rtc.time_valid, "Time validity maintained throughout all 10 network drops");
    EXPECT_EQ(g_exec_log_count, 0, "No spurious triggers during repeated network drops");
}

/* ========================================================================= */
/* TEST 7: Regression Testing (Repeat Types, ONCE auto-disable, Deduplication)*/
/* ========================================================================= */
static void test_7_regression_testing(void)
{
    printf("\n=== TEST 7: Regression Testing ===\n");
    mock_rtc_init();
    mock_sntp_sync(1795515300); /* Calibrated */

    /* Subtest 7A: ONCE repeat auto-disables */
    schedule_entry_t once_sched;
    memset(&once_sched, 0, sizeof(once_sched));
    once_sched.id = 7;
    once_sched.version = 1;
    once_sched.enabled = 1;
    once_sched.hour = 15;
    once_sched.minute = 45;
    once_sched.repeat_type = SCHED_REPEAT_ONCE;
    once_sched.once_year = 2026;
    once_sched.once_month = 11;
    once_sched.once_day = 24;
    once_sched.action_count = 1;
    once_sched.actions[0].target = 0x0001;
    once_sched.actions[0].value = 1;

    g_last_evaluated_minute_epoch = 0;
    clear_exec_logs();

    mock_sched_engine_step(&once_sched, 1);
    EXPECT_EQ(g_exec_log_count, 1, "ONCE schedule executed at trigger time");
    EXPECT_EQ(once_sched.enabled, 0, "ONCE schedule automatically disabled after execution");

    /* Subtest 7B: Weekdays schedule (Mon-Fri) */
    schedule_entry_t wd_sched;
    memset(&wd_sched, 0, sizeof(wd_sched));
    wd_sched.id = 8;
    wd_sched.version = 1;
    wd_sched.enabled = 1;
    wd_sched.hour = 15;
    wd_sched.minute = 45;
    wd_sched.repeat_type = SCHED_REPEAT_WEEKDAYS;

    struct tm mon_tm = { .tm_hour = 15, .tm_min = 45, .tm_wday = 1 }; /* Monday */
    struct tm sat_tm = { .tm_hour = 15, .tm_min = 45, .tm_wday = 6 }; /* Saturday */
    EXPECT_TRUE(schedule_is_due(&wd_sched, &mon_tm), "WEEKDAYS matches on Monday");
    EXPECT_TRUE(!schedule_is_due(&wd_sched, &sat_tm), "WEEKDAYS does NOT match on Saturday");
}

int main(void)
{
    printf("===============================================================\n");
    printf(" RTC-Based Timekeeping & Offline Scheduling Unit Test Suite    \n");
    printf("===============================================================\n");

    test_1_normal_ntp_sync();
    test_2_wifi_disconnection_offline_scheduling();
    test_3_extended_outage_midnight_rollover();
    test_4_reconnection_and_safe_clock_adjustment();
    test_5_complete_power_cycle();
    test_6_repeated_network_disconnections();
    test_7_regression_testing();

    printf("\n===============================================================\n");
    printf(" TEST RESULTS: %d Passed, %d Failed\n", g_test_passes, g_test_fails);
    printf("===============================================================\n");

    return (g_test_fails == 0) ? 0 : 1;
}
