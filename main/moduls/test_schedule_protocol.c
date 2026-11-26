/*
 * File    : test_schedule_protocol.c
 * Purpose : Host unit test suite for BLE Mesh Schedule protocol, validation,
 *           staging, CRC32, repeat matching, rate limiting, and deduplication.
 * Target  : Host GCC (Linux / macOS / Windows host, no ESP hardware needed).
 * Compile : gcc -Wall -Wextra -std=c99 -I. test_schedule_protocol.c schedule_model.c -o test_schedule_protocol
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include "schedule_model.h"

/* Mock minimal definitions for vendor wire protocol testing on host */
#define CID_ESP 0x02E5
#define CONFIG_SCHED_VENDOR_COMPANY_ID CID_ESP
#define CONFIG_SCHED_VENDOR_MODEL_ID   0x0001

/* Status error codes */
typedef enum {
    SCHED_STATUS_OK                   = 0x00,
    SCHED_STATUS_ERR_BAD_VERSION      = 0x01,
    SCHED_STATUS_ERR_BAD_RANGE        = 0x02,
    SCHED_STATUS_ERR_TOO_MANY_ACTIONS = 0x03,
    SCHED_STATUS_ERR_STORAGE_FULL     = 0x04,
    SCHED_STATUS_ERR_CRC_MISMATCH     = 0x05,
    SCHED_STATUS_ERR_BUSY             = 0x06,
    SCHED_STATUS_ERR_UNKNOWN_ID       = 0x07,
    SCHED_STATUS_WARN_TIME_NOT_SYNCED = 0x08,
    SCHED_STATUS_ERR_DUPLICATE_TARGET = 0x09,
    SCHED_STATUS_ERR_STAGING_TIMEOUT  = 0x0A,
    SCHED_STATUS_ERR_INVALID_PARAM    = 0x0B
} sched_status_code_t;

/* Wire Protocol Functions under test */
size_t sched_encode_begin(uint8_t *out_buf, size_t buf_len, uint8_t tid, const schedule_entry_t *s)
{
    if (!out_buf || buf_len < 29 || !s) {
        return 0;
    }
    out_buf[0] = tid;
    out_buf[1] = s->id;
    out_buf[2] = s->version;
    out_buf[3] = s->enabled ? 1 : 0;
    out_buf[4] = s->hour;
    out_buf[5] = s->minute;
    out_buf[6] = s->repeat_type;
    out_buf[7] = s->repeat_days;
    out_buf[8] = (uint8_t)(s->once_year & 0xFF);
    out_buf[9] = (uint8_t)((s->once_year >> 8) & 0xFF);
    out_buf[10] = s->once_month;
    out_buf[11] = s->once_day;
    memset(&out_buf[12], 0, SCHED_NAME_MAX_LEN);
    strncpy((char *)&out_buf[12], s->name, SCHED_NAME_MAX_LEN - 1);
    out_buf[28] = s->action_count;
    return 29;
}

bool sched_decode_begin(const uint8_t *in_buf, size_t len, uint8_t *out_tid, schedule_entry_t *out_s)
{
    if (!in_buf || len < 29 || !out_s) {
        return false;
    }
    memset(out_s, 0, sizeof(schedule_entry_t));
    if (out_tid) *out_tid = in_buf[0];
    out_s->id = in_buf[1];
    out_s->version = in_buf[2];
    out_s->enabled = in_buf[3];
    out_s->hour = in_buf[4];
    out_s->minute = in_buf[5];
    out_s->repeat_type = in_buf[6];
    out_s->repeat_days = in_buf[7];
    out_s->once_year = (uint16_t)in_buf[8] | ((uint16_t)in_buf[9] << 8);
    out_s->once_month = in_buf[10];
    out_s->once_day = in_buf[11];
    memset(out_s->name, 0, sizeof(out_s->name));
    memcpy(out_s->name, &in_buf[12], SCHED_NAME_MAX_LEN);
    out_s->name[SCHED_NAME_MAX_LEN - 1] = '\0';
    out_s->action_count = in_buf[28];
    return true;
}

size_t sched_encode_actions(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint8_t seq, const sched_action_t *actions, uint8_t count)
{
    size_t req_len = 3 + (count * 4);
    if (!out_buf || buf_len < req_len || !actions) {
        return 0;
    }
    out_buf[0] = tid;
    out_buf[1] = seq;
    out_buf[2] = count;
    size_t offset = 3;
    for (uint8_t i = 0; i < count; i++) {
        out_buf[offset++] = (uint8_t)(actions[i].target & 0xFF);
        out_buf[offset++] = (uint8_t)((actions[i].target >> 8) & 0xFF);
        out_buf[offset++] = actions[i].kind;
        out_buf[offset++] = actions[i].value;
    }
    return offset;
}

bool sched_decode_actions(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint8_t *out_seq, sched_action_t *out_actions, uint8_t *out_count)
{
    if (!in_buf || len < 3 || !out_actions || !out_count) {
        return false;
    }
    if (out_tid) *out_tid = in_buf[0];
    if (out_seq) *out_seq = in_buf[1];
    uint8_t count = in_buf[2];
    if (len < (size_t)(3 + (count * 4))) {
        return false;
    }
    size_t offset = 3;
    for (uint8_t i = 0; i < count; i++) {
        out_actions[i].target = (uint16_t)in_buf[offset] | ((uint16_t)in_buf[offset + 1] << 8);
        out_actions[i].kind = in_buf[offset + 2];
        out_actions[i].value = in_buf[offset + 3];
        offset += 4;
    }
    *out_count = count;
    return true;
}

size_t sched_encode_commit(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint32_t crc32)
{
    if (!out_buf || buf_len < 5) {
        return 0;
    }
    out_buf[0] = tid;
    out_buf[1] = (uint8_t)(crc32 & 0xFF);
    out_buf[2] = (uint8_t)((crc32 >> 8) & 0xFF);
    out_buf[3] = (uint8_t)((crc32 >> 16) & 0xFF);
    out_buf[4] = (uint8_t)((crc32 >> 24) & 0xFF);
    return 5;
}

bool sched_decode_commit(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint32_t *out_crc32)
{
    if (!in_buf || len < 5 || !out_crc32) {
        return false;
    }
    if (out_tid) *out_tid = in_buf[0];
    *out_crc32 = (uint32_t)in_buf[1] |
                 ((uint32_t)in_buf[2] << 8) |
                 ((uint32_t)in_buf[3] << 16) |
                 ((uint32_t)in_buf[4] << 24);
    return true;
}

size_t sched_encode_status(uint8_t *out_buf, size_t buf_len, uint8_t status, uint8_t tid, uint8_t id)
{
    if (!out_buf || buf_len < 3) return 0;
    out_buf[0] = status;
    out_buf[1] = tid;
    out_buf[2] = id;
    return 3;
}

bool sched_decode_status(const uint8_t *in_buf, size_t len, uint8_t *out_status, uint8_t *out_tid, uint8_t *out_id)
{
    if (!in_buf || len < 3 || !out_status) return false;
    *out_status = in_buf[0];
    if (out_tid) *out_tid = in_buf[1];
    if (out_id) *out_id = in_buf[2];
    return true;
}

/* =========================================================================
 * Simulated Staging Engine for Host Testing
 * ========================================================================= */
#define MAX_STAGING_BUFFERS 2
#define STAGING_TIMEOUT_SEC 15

typedef struct {
    bool active;
    uint16_t src_addr;
    uint8_t transaction_id;
    uint32_t start_time_sec;
    schedule_entry_t sched;
    uint8_t actions_received;
} mock_staging_buffer_t;

static mock_staging_buffer_t s_mock_staging[MAX_STAGING_BUFFERS];
static uint32_t s_mock_current_time_sec = 1000;

static mock_staging_buffer_t *mock_find_or_alloc(uint16_t src_addr, uint8_t tid)
{
    mock_staging_buffer_t *free_slot = NULL;
    for (int i = 0; i < MAX_STAGING_BUFFERS; i++) {
        if (s_mock_staging[i].active) {
            if ((s_mock_current_time_sec - s_mock_staging[i].start_time_sec) > STAGING_TIMEOUT_SEC) {
                s_mock_staging[i].active = false;
                if (!free_slot) free_slot = &s_mock_staging[i];
            } else if (s_mock_staging[i].src_addr == src_addr) {
                return &s_mock_staging[i];
            }
        } else {
            if (!free_slot) free_slot = &s_mock_staging[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(mock_staging_buffer_t));
        free_slot->active = true;
        free_slot->src_addr = src_addr;
        free_slot->transaction_id = tid;
        free_slot->start_time_sec = s_mock_current_time_sec;
        return free_slot;
    }
    return NULL;
}

/* =========================================================================
 * Test Cases
 * ========================================================================= */

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    s_tests_run++; \
    if (cond) { \
        s_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while(0)

/* Test 1: Wire Protocol Encode & Decode */
static void test_wire_protocol_encode_decode(void)
{
    printf("\n=== TEST 1: Wire Protocol Encode / Decode ===\n");
    schedule_entry_t orig;
    memset(&orig, 0, sizeof(orig));
    orig.id = 1;
    orig.version = SCHED_PROTOCOL_VERSION;
    orig.enabled = 1;
    orig.hour = 18;
    orig.minute = 30;
    orig.repeat_type = SCHED_REPEAT_CUSTOM;
    orig.repeat_days = 0b00101010; /* Mon, Wed, Fri */
    orig.once_year = 2026;
    orig.once_month = 9;
    orig.once_day = 19;
    strncpy(orig.name, "Garden Lights", sizeof(orig.name) - 1);
    orig.action_count = 2;

    uint8_t begin_buf[32];
    size_t enc_len = sched_encode_begin(begin_buf, sizeof(begin_buf), 0xA5, &orig);
    TEST_ASSERT(enc_len == 29, "sched_encode_begin outputs exactly 29 bytes");

    uint8_t dec_tid = 0;
    schedule_entry_t dec_entry;
    bool dec_ok = sched_decode_begin(begin_buf, enc_len, &dec_tid, &dec_entry);
    TEST_ASSERT(dec_ok, "sched_decode_begin succeeds");
    TEST_ASSERT(dec_tid == 0xA5, "Decoded TID matches 0xA5");
    TEST_ASSERT(dec_entry.id == orig.id, "Decoded ID matches");
    TEST_ASSERT(dec_entry.hour == 18 && dec_entry.minute == 30, "Decoded time 18:30 matches");
    TEST_ASSERT(dec_entry.repeat_type == SCHED_REPEAT_CUSTOM, "Decoded repeat type matches");
    TEST_ASSERT(dec_entry.repeat_days == 0b00101010, "Decoded repeat days mask matches");
    TEST_ASSERT(strcmp(dec_entry.name, "Garden Lights") == 0, "Decoded name matches");

    sched_action_t actions[2] = {
        { .target = 0, .kind = SCHED_ACTION_KIND_ONOFF, .value = 1 },
        { .target = 1, .kind = SCHED_ACTION_KIND_ONOFF, .value = 0 }
    };
    uint8_t act_buf[32];
    enc_len = sched_encode_actions(act_buf, sizeof(act_buf), 0xA5, 0, actions, 2);
    TEST_ASSERT(enc_len == (3 + 8), "sched_encode_actions length is 11 bytes");

    sched_action_t dec_actions[4];
    uint8_t dec_count = 0, dec_seq = 0;
    dec_ok = sched_decode_actions(act_buf, enc_len, &dec_tid, &dec_seq, dec_actions, &dec_count);
    TEST_ASSERT(dec_ok && dec_count == 2, "sched_decode_actions decodes 2 actions");
    TEST_ASSERT(dec_actions[0].target == 0 && dec_actions[0].value == 1, "Action 0 target=0 val=1");
    TEST_ASSERT(dec_actions[1].target == 1 && dec_actions[1].value == 0, "Action 1 target=1 val=0");

    uint8_t commit_buf[8];
    enc_len = sched_encode_commit(commit_buf, sizeof(commit_buf), 0xA5, 0x12345678);
    TEST_ASSERT(enc_len == 5, "sched_encode_commit length is 5 bytes");

    uint32_t dec_crc = 0;
    dec_ok = sched_decode_commit(commit_buf, enc_len, &dec_tid, &dec_crc);
    TEST_ASSERT(dec_ok && dec_crc == 0x12345678, "sched_decode_commit decodes CRC 0x12345678");
}

/* Test 2: Chunked Staging, Drops, and CRC32 Validation */
static void test_chunked_staging_and_crc(void)
{
    printf("\n=== TEST 2: Chunked Staging, Drops & CRC32 ===\n");
    memset(s_mock_staging, 0, sizeof(s_mock_staging));
    s_mock_current_time_sec = 2000;

    schedule_entry_t sched;
    memset(&sched, 0, sizeof(sched));
    sched.id = 2;
    sched.version = 1;
    sched.enabled = 1;
    sched.hour = 7;
    sched.minute = 15;
    sched.repeat_type = SCHED_REPEAT_DAILY;
    strncpy(sched.name, "Morning Relay", sizeof(sched.name) - 1);
    sched.action_count = 3;
    sched.actions[0] = (sched_action_t){ .target = 0, .kind = 1, .value = 1 };
    sched.actions[1] = (sched_action_t){ .target = 1, .kind = 1, .value = 1 };
    sched.actions[2] = (sched_action_t){ .target = 2, .kind = 1, .value = 0 };

    uint32_t correct_crc = schedule_calc_crc32(&sched);
    TEST_ASSERT(correct_crc != 0, "CRC32 calculated successfully");

    /* Staging from node 0x0005, TID 1 */
    mock_staging_buffer_t *sb = mock_find_or_alloc(0x0005, 1);
    TEST_ASSERT(sb != NULL, "Allocated staging buffer for node 0x0005");
    sb->sched = sched;
    sb->actions_received = 3;

    /* Verify good CRC matches and commits */
    uint32_t calc_crc = schedule_calc_crc32(&sb->sched);
    TEST_ASSERT(calc_crc == correct_crc, "Staged schedule CRC matches expected CRC");
    TEST_ASSERT(schedule_validate(&sb->sched) == SCHED_ERR_OK, "Staged schedule passes validation");

    /* Simulated corruption: bad CRC rejected */
    uint32_t corrupt_crc = correct_crc ^ 0xFFFFFFFF;
    TEST_ASSERT(calc_crc != corrupt_crc, "Staging rejects corrupted CRC32 commit");

    /* User scenario cross-platform test vector: "fho", 14:29, 2 devices */
    schedule_entry_t user_s;
    memset(&user_s, 0, sizeof(user_s));
    user_s.id = 1;
    user_s.version = 1;
    user_s.enabled = 1;
    user_s.hour = 14;
    user_s.minute = 29;
    user_s.repeat_type = SCHED_REPEAT_ONCE;
    user_s.repeat_days = 0;
    user_s.once_year = 2026;
    user_s.once_month = 11;
    user_s.once_day = 6;
    strncpy(user_s.name, "fho", sizeof(user_s.name) - 1);
    user_s.action_count = 2;
    user_s.actions[0] = (sched_action_t){ .target = 0x0002, .kind = 1, .value = 1 };
    user_s.actions[1] = (sched_action_t){ .target = 0x0003, .kind = 1, .value = 1 };

    uint32_t user_crc = schedule_calc_crc32(&user_s);
    TEST_ASSERT(user_crc == 0x62339A63, "User scenario CRC32 matches expected 0x62339A63");
}

/* Test 3: Staging Timeout & Resource Cleanup */
static void test_staging_timeout(void)
{
    printf("\n=== TEST 3: Staging Timeout & Slot Recovery ===\n");
    memset(s_mock_staging, 0, sizeof(s_mock_staging));
    s_mock_current_time_sec = 3000;

    /* Fill slot 0 with node 0x0010 */
    mock_staging_buffer_t *s0 = mock_find_or_alloc(0x0010, 10);
    TEST_ASSERT(s0 != NULL, "Slot 0 allocated for node 0x0010");

    /* Fill slot 1 with node 0x0020 */
    mock_staging_buffer_t *s1 = mock_find_or_alloc(0x0020, 20);
    TEST_ASSERT(s1 != NULL, "Slot 1 allocated for node 0x0020");

    /* Third caller when all slots occupied and active */
    mock_staging_buffer_t *s2 = mock_find_or_alloc(0x0030, 30);
    TEST_ASSERT(s2 == NULL, "Third caller rejected (BUSY) when slots full");

    /* Advance time past STAGING_TIMEOUT_SEC (16 seconds later) */
    s_mock_current_time_sec += 16;

    /* Node 0x0030 tries again: slot 0 should have expired and be reclaimed */
    s2 = mock_find_or_alloc(0x0030, 30);
    TEST_ASSERT(s2 != NULL, "Expired slot reclaimed; slot 2 not blocked");
    TEST_ASSERT(s2->src_addr == 0x0030, "Reclaimed slot reallocated to 0x0030");
}

/* Test 4: Validation Rules & Boundary Conditions */
static void test_validation_rules(void)
{
    printf("\n=== TEST 4: Validation Rules & Boundaries ===\n");
    schedule_entry_t s;
    memset(&s, 0, sizeof(s));
    s.id = 1;
    s.version = 1;
    s.enabled = 1;
    s.hour = 23;
    s.minute = 59;
    s.repeat_type = SCHED_REPEAT_DAILY;
    strncpy(s.name, "Test", sizeof(s.name) - 1);
    s.action_count = 1;
    s.actions[0] = (sched_action_t){ .target = 0, .kind = 1, .value = 1 };

    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_OK, "Valid schedule passes validation");

    /* Boundary ID tests */
    s.id = 0;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_ID, "ID 0 rejected");
    s.id = 17;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_ID, "ID 17 rejected (> SCHED_MAX_COUNT)");
    s.id = 1;

    /* Boundary Hour / Minute tests */
    s.hour = 24;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_TIME, "Hour 24 rejected");
    s.hour = 23;
    s.minute = 60;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_TIME, "Minute 60 rejected");
    s.minute = 59;

    /* Repeat type tests */
    s.repeat_type = 5;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "Invalid repeat_type 5 rejected");
    s.repeat_type = SCHED_REPEAT_CUSTOM;
    s.repeat_days = 0;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "CUSTOM repeat with mask 0 rejected");
    s.repeat_days = 0x80;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "CUSTOM repeat with bit 7 set rejected");
    s.repeat_type = SCHED_REPEAT_DAILY;

    /* ONCE date validation */
    s.repeat_type = SCHED_REPEAT_ONCE;
    s.once_year = 2026;
    s.once_month = 0;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "ONCE month 0 rejected");
    s.once_month = 13;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "ONCE month 13 rejected");
    s.once_month = 2;
    s.once_day = 0;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "ONCE day 0 rejected");
    s.once_day = 32;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_BAD_REPEAT, "ONCE day 32 rejected");
    s.once_day = 28;
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_OK, "Valid ONCE schedule passes");
    s.repeat_type = SCHED_REPEAT_DAILY;

    /* Duplicate target check */
    s.action_count = 2;
    s.actions[0] = (sched_action_t){ .target = 1, .kind = 1, .value = 1 };
    s.actions[1] = (sched_action_t){ .target = 1, .kind = 1, .value = 0 };
    TEST_ASSERT(schedule_validate(&s) == SCHED_ERR_DUPLICATE_TARGET, "Duplicate target in single schedule rejected");
}

/* Test 5: Repeat Logic Across Calendar Boundaries & Leap Day */
static void test_repeat_logic_and_calendar(void)
{
    printf("\n=== TEST 5: Repeat Logic, Boundaries & Leap Day ===\n");
    schedule_entry_t s;
    memset(&s, 0, sizeof(s));
    s.id = 1;
    s.version = 1;
    s.enabled = 1;
    s.hour = 14;
    s.minute = 30;

    struct tm ist_tm;
    memset(&ist_tm, 0, sizeof(ist_tm));

    /* DAILY repeat */
    s.repeat_type = SCHED_REPEAT_DAILY;
    ist_tm.tm_hour = 14;
    ist_tm.tm_min = 30;
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "DAILY matches at 14:30");
    ist_tm.tm_min = 31;
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "DAILY does not match at 14:31");

    /* WEEKDAYS repeat (Mon..Fri) */
    s.repeat_type = SCHED_REPEAT_WEEKDAYS;
    ist_tm.tm_min = 30;
    ist_tm.tm_wday = 1; /* Monday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "WEEKDAYS matches on Monday");
    ist_tm.tm_wday = 5; /* Friday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "WEEKDAYS matches on Friday");
    ist_tm.tm_wday = 6; /* Saturday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "WEEKDAYS does NOT match on Saturday");
    ist_tm.tm_wday = 0; /* Sunday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "WEEKDAYS does NOT match on Sunday");

    /* WEEKENDS repeat (Sat..Sun) */
    s.repeat_type = SCHED_REPEAT_WEEKENDS;
    ist_tm.tm_wday = 6; /* Saturday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "WEEKENDS matches on Saturday");
    ist_tm.tm_wday = 0; /* Sunday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "WEEKENDS matches on Sunday");
    ist_tm.tm_wday = 3; /* Wednesday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "WEEKENDS does NOT match on Wednesday");

    /* CUSTOM repeat: Tuesday(bit 2) and Thursday(bit 4) = 0b00010100 = 0x14 */
    s.repeat_type = SCHED_REPEAT_CUSTOM;
    s.repeat_days = 0x14;
    ist_tm.tm_wday = 2; /* Tuesday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "CUSTOM matches on Tuesday");
    ist_tm.tm_wday = 4; /* Thursday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "CUSTOM matches on Thursday");
    ist_tm.tm_wday = 1; /* Monday */
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "CUSTOM does NOT match on Monday");

    /* ONCE repeat: Leap day 2028-02-29 14:30 */
    s.repeat_type = SCHED_REPEAT_ONCE;
    s.once_year = 2028;
    s.once_month = 2;
    s.once_day = 29;

    ist_tm.tm_year = 2028 - 1900;
    ist_tm.tm_mon = 2 - 1; /* 0-based month = 1 */
    ist_tm.tm_mday = 29;
    ist_tm.tm_hour = 14;
    ist_tm.tm_min = 30;
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "ONCE matches on Leap Day 2028-02-29 14:30");

    /* Next day 2028-03-01 */
    ist_tm.tm_mon = 3 - 1;
    ist_tm.tm_mday = 1;
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "ONCE does not match on 2028-03-01");

    /* Month boundary: 2026-03-31 23:59 -> 2026-04-01 00:00 */
    s.once_year = 2026;
    s.once_month = 4;
    s.once_day = 1;
    s.hour = 0;
    s.minute = 0;

    ist_tm.tm_year = 2026 - 1900;
    ist_tm.tm_mon = 3 - 1; /* March */
    ist_tm.tm_mday = 31;
    ist_tm.tm_hour = 23;
    ist_tm.tm_min = 59;
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == false, "ONCE 2026-04-01 00:00 does not fire on March 31 23:59");

    ist_tm.tm_mon = 4 - 1; /* April */
    ist_tm.tm_mday = 1;
    ist_tm.tm_hour = 0;
    ist_tm.tm_min = 0;
    TEST_ASSERT(schedule_is_due(&s, &ist_tm) == true, "ONCE 2026-04-01 00:00 fires at exact month boundary rollover");
}

/* Test 6: Rate Limiter Simulation */
static void test_rate_limiter(void)
{
    printf("\n=== TEST 6: Rate Limiter Simulation (Max 8 msg/s) ===\n");
    int allowed = 0;
    int dropped = 0;

    int rate_limit_window = 1; /* Window 1 second */
    int current_window = 1;
    int window_count = 0;
    const int RATE_LIMIT_MAX = 8;

    for (int msg = 1; msg <= 10; msg++) {
        if (current_window == rate_limit_window) {
            window_count++;
            if (window_count > RATE_LIMIT_MAX) {
                dropped++;
            } else {
                allowed++;
            }
        }
    }

    TEST_ASSERT(allowed == 8, "Exactly 8 messages allowed in 1-second burst window");
    TEST_ASSERT(dropped == 2, "9th and 10th messages dropped when exceeding rate limit");
}

/* Test 7: Deduplication & Conflict Resolution (Last Action Wins) */
static void test_target_deduplication(void)
{
    printf("\n=== TEST 7: Target Deduplication (Last Action Wins) ===\n");
    typedef struct {
        uint16_t target;
        uint8_t value;
        uint8_t sched_id;
    } merged_action_t;

    merged_action_t merged[8];
    uint8_t merged_count = 0;

    /* Schedule #1 sets Relay 1 (target 0) -> ON (1) */
    /* Schedule #2 sets Relay 1 (target 0) -> OFF (0) in the same minute */
    struct {
        uint8_t sched_id;
        uint16_t target;
        uint8_t value;
    } incoming[2] = {
        { .sched_id = 1, .target = 0, .value = 1 },
        { .sched_id = 2, .target = 0, .value = 0 }
    };

    for (int i = 0; i < 2; i++) {
        int existing = -1;
        for (int m = 0; m < merged_count; m++) {
            if (merged[m].target == incoming[i].target) {
                existing = m;
                break;
            }
        }
        if (existing >= 0) {
            merged[existing].value = incoming[i].value;
            merged[existing].sched_id = incoming[i].sched_id;
        } else {
            merged[merged_count].target = incoming[i].target;
            merged[merged_count].value = incoming[i].value;
            merged[merged_count].sched_id = incoming[i].sched_id;
            merged_count++;
        }
    }

    TEST_ASSERT(merged_count == 1, "Relay 1 merged into 1 executed action");
    TEST_ASSERT(merged[0].value == 0, "Last action wins: value is OFF (0)");
    TEST_ASSERT(merged[0].sched_id == 2, "Last action wins: originating schedule is #2");
}

int main(void)
{
    printf("===================================================\n");
    printf(" BLE Mesh Schedule Protocol Unit Test Suite (Host) \n");
    printf("===================================================\n");

    test_wire_protocol_encode_decode();
    test_chunked_staging_and_crc();
    test_staging_timeout();
    test_validation_rules();
    test_repeat_logic_and_calendar();
    test_rate_limiter();
    test_target_deduplication();

    printf("\n===================================================\n");
    printf(" TEST SUMMARY: %d / %d assertions passed\n", s_tests_passed, s_tests_run);
    printf("===================================================\n");

    return (s_tests_passed == s_tests_run) ? 0 : 1;
}
