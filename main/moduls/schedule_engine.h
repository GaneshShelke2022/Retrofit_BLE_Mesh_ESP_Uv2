/*
 * File    : schedule_engine.h
 * Purpose : Scheduler execution engine for ESP32-C3 Master node.
 * Does    : Periodic minute rollover evaluation, target action deduplication (last action wins),
 *           local relay execution, ONCE auto-disable, and execution history recording.
 */

#ifndef SCHEDULE_ENGINE_H
#define SCHEDULE_ENGINE_H

#include "schedule_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t target;
    uint8_t value;
    time_t timestamp;
    uint8_t sched_id;
    bool success;
} sched_target_result_t;

#define SCHED_MAX_HISTORY 16

/* Initializes the scheduler engine and spawns the background worker task */
esp_err_t sched_engine_init(void);

/* Immediately executes the actions of schedule 'id' for testing (SCHED_RUN_NOW) */
esp_err_t sched_engine_run_now(uint8_t id);

/* Prints current scheduler status, next due schedules, and recent execution history */
void sched_engine_print_status(void);

/* Retrieves recent execution results */
uint8_t sched_engine_get_history(sched_target_result_t *out_history, uint8_t max_count);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULE_ENGINE_H
