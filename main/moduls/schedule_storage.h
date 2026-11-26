/*
 * File    : schedule_storage.h
 * Purpose : Persistent NVS storage for schedules on ESP32-C3 Master node.
 * Does    : CRUD operations for schedules under dedicated namespace "lumflux_sched",
 *           maintains schedule mask, and ensures atomic commit with CRC32 verification.
 */

#ifndef SCHEDULE_STORAGE_H
#define SCHEDULE_STORAGE_H

#include "schedule_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_NVS_NAMESPACE "lumflux_sched"

/* Initializes schedule storage namespace */
esp_err_t sched_storage_init(void);

/* Atomically saves schedule entry to NVS with CRC32 verification and updates index */
esp_err_t sched_storage_save(const schedule_entry_t *entry);

/* Loads schedule entry by ID (1..16) from NVS */
esp_err_t sched_storage_load(uint8_t id, schedule_entry_t *out_entry);

/* Deletes schedule entry by ID from NVS and updates index */
esp_err_t sched_storage_delete(uint8_t id);

/* Erases all schedules in "lumflux_sched" namespace and resets index */
esp_err_t sched_storage_clear_all(void);

/* Returns 16-bit bitmask of active schedule IDs (bit 0 = ID 1, ..., bit 15 = ID 16) */
uint16_t sched_storage_get_active_mask(void);

/* Returns count of currently stored active schedules */
uint8_t sched_storage_get_count(void);

/* Finds active schedule by its CRC32 for idempotent commit retries */
esp_err_t sched_storage_find_by_crc(uint32_t crc32, uint8_t *out_id);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULE_STORAGE_H
