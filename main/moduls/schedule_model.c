/*
 * File    : schedule_model.c
 * Purpose : Schedule data structures, validation rules, and pure matching logic.
 * Does    : Standalone IEEE 802.3 CRC32, range validation, duplicate checks, and IST matching.
 */

#include "schedule_model.h"
#include <string.h>

/* Standard IEEE 802.3 CRC-32 table-less implementation */
static uint32_t compute_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

uint32_t schedule_calc_crc32(const schedule_entry_t *entry)
{
    if (!entry) {
        return 0;
    }
    /* Calculate CRC over all bytes preceding the crc32 field */
    size_t len_to_hash = offsetof(schedule_entry_t, crc32);
    return compute_crc32((const uint8_t *)entry, len_to_hash);
}

sched_err_t schedule_validate(const schedule_entry_t *entry)
{
    if (!entry) {
        return SCHED_ERR_BAD_ID;
    }

    if (entry->id < 1 || entry->id > SCHED_MAX_COUNT) {
        return SCHED_ERR_BAD_ID;
    }

    if (entry->version != SCHED_PROTOCOL_VERSION) {
        return SCHED_ERR_BAD_VERSION;
    }

    if (entry->hour > 23 || entry->minute > 59) {
        return SCHED_ERR_BAD_TIME;
    }

    if (entry->repeat_type > SCHED_REPEAT_CUSTOM) {
        return SCHED_ERR_BAD_REPEAT;
    }

    if (entry->repeat_type == SCHED_REPEAT_ONCE) {
        if (entry->once_month < 1 || entry->once_month > 12 ||
            entry->once_day < 1 || entry->once_day > 31 ||
            entry->once_year < 2025 || entry->once_year > 2099) {
            return SCHED_ERR_BAD_REPEAT;
        }
    } else if (entry->repeat_type == SCHED_REPEAT_CUSTOM) {
        if ((entry->repeat_days & 0x7F) == 0) {
            return SCHED_ERR_BAD_REPEAT;
        }
    }

    if (entry->action_count < 1 || entry->action_count > SCHED_MAX_ACTIONS) {
        return SCHED_ERR_BAD_ACTIONS;
    }

    /* Check for duplicate targets and valid action kinds */
    for (uint8_t i = 0; i < entry->action_count; i++) {
        if (entry->actions[i].kind != SCHED_ACTION_KIND_ONOFF) {
            return SCHED_ERR_BAD_ACTIONS;
        }
        for (uint8_t j = i + 1; j < entry->action_count; j++) {
            if (entry->actions[i].target == entry->actions[j].target) {
                return SCHED_ERR_DUPLICATE_TARGET;
            }
        }
    }

    return SCHED_ERR_OK;
}

bool schedule_is_due(const schedule_entry_t *entry, const struct tm *ist_tm)
{
    if (!entry || !ist_tm) {
        return false;
    }

    if (!entry->enabled) {
        return false;
    }

    if (entry->hour != ist_tm->tm_hour || entry->minute != ist_tm->tm_min) {
        return false;
    }

    switch (entry->repeat_type) {
    case SCHED_REPEAT_ONCE: {
        int current_year = ist_tm->tm_year + 1900;
        int current_month = ist_tm->tm_mon + 1;
        int current_day = ist_tm->tm_mday;
        return (entry->once_year == current_year &&
                entry->once_month == current_month &&
                entry->once_day == current_day);
    }

    case SCHED_REPEAT_DAILY:
        return true;

    case SCHED_REPEAT_WEEKDAYS:
        /* tm_wday: 0 = Sun, 1 = Mon, ..., 5 = Fri, 6 = Sat */
        return (ist_tm->tm_wday >= 1 && ist_tm->tm_wday <= 5);

    case SCHED_REPEAT_WEEKENDS:
        return (ist_tm->tm_wday == 0 || ist_tm->tm_wday == 6);

    case SCHED_REPEAT_CUSTOM:
        /* Bit 0 = Sun, Bit 1 = Mon ... Bit 6 = Sat */
        return ((entry->repeat_days & (1 << ist_tm->tm_wday)) != 0);

    default:
        return false;
    }
}
