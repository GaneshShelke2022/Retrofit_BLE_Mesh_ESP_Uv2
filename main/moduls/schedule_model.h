
/*
 * File    : schedule_model.h
 * Purpose : Schedule data structures, validation rules, and pure matching logic.
 * Does    : Defines schedule payload formats, action descriptors, repeat types,
 *           CRC32 verification, and minute-match evaluation without hardware dependencies.
 */

#ifndef SCHEDULE_MODEL_H
#define SCHEDULE_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_MAX_COUNT        16
#define SCHED_MAX_ACTIONS      8
#define SCHED_NAME_MAX_LEN     16
#define SCHED_PROTOCOL_VERSION 1

/* Repeat type enumeration */
typedef enum {
    SCHED_REPEAT_ONCE     = 0,
    SCHED_REPEAT_DAILY    = 1,
    SCHED_REPEAT_WEEKDAYS = 2, /* Monday to Friday */
    SCHED_REPEAT_WEEKENDS = 3, /* Saturday and Sunday */
    SCHED_REPEAT_CUSTOM   = 4  /* Bitmask: bit 0 = Sun, bit 1 = Mon, ..., bit 6 = Sat */
} sched_repeat_type_t;

/* Action kind enumeration */
typedef enum {
    SCHED_ACTION_KIND_NONE  = 0,
    SCHED_ACTION_KIND_ONOFF = 1
} sched_action_kind_t;

/* Individual action on a target node / relay */
typedef struct __attribute__((packed)) {
    uint16_t target; /* 0..3 for local relay channel, or element unicast address */
    uint8_t kind;    /* sched_action_kind_t */
    uint8_t value;   /* 0 = OFF, 1 = ON */
} sched_action_t;

/* Schedule entry structure */
typedef struct __attribute__((packed)) {
    uint8_t id;                        /* 1..SCHED_MAX_COUNT */
    uint8_t version;                   /* SCHED_PROTOCOL_VERSION */
    uint8_t enabled;                   /* 1 = Enabled, 0 = Disabled */
    uint8_t hour;                      /* 0..23 (IST) */
    uint8_t minute;                    /* 0..59 (IST) */
    uint8_t repeat_type;               /* sched_repeat_type_t */
    uint8_t repeat_days;               /* Bitmask for CUSTOM repeat: bits 0..6 */
    uint16_t once_year;                /* e.g. 2026 for ONCE */
    uint8_t once_month;                /* 1..12 for ONCE */
    uint8_t once_day;                  /* 1..31 for ONCE */
    char name[SCHED_NAME_MAX_LEN];     /* Null-terminated name */
    uint8_t action_count;              /* 1..SCHED_MAX_ACTIONS */
    sched_action_t actions[SCHED_MAX_ACTIONS];
    uint32_t crc32;                    /* CRC32 calculated over bytes preceding this field */
} schedule_entry_t;

/* Validation status codes */
typedef enum {
    SCHED_ERR_OK = 0,
    SCHED_ERR_BAD_ID,
    SCHED_ERR_BAD_VERSION,
    SCHED_ERR_BAD_TIME,
    SCHED_ERR_BAD_REPEAT,
    SCHED_ERR_BAD_ACTIONS,
    SCHED_ERR_DUPLICATE_TARGET,
    SCHED_ERR_CRC_MISMATCH,
    SCHED_ERR_STORAGE_FULL,
    SCHED_ERR_BUSY,
    SCHED_ERR_UNKNOWN_ID,
    SCHED_ERR_TIME_NOT_SYNCED
} sched_err_t;

/* Calculates CRC32 over schedule fields (excluding crc32 field itself) */
uint32_t schedule_calc_crc32(const schedule_entry_t *entry);

/* Validates schedule structure, field ranges, and target uniqueness */
sched_err_t schedule_validate(const schedule_entry_t *entry);

/* Evaluates whether schedule is due to execute at the given IST time */
bool schedule_is_due(const schedule_entry_t *entry, const struct tm *ist_tm);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULE_MODEL_H
