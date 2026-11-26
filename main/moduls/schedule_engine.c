/*
 * File    : schedule_engine.c
 * Purpose : Scheduler execution engine implementation.
 * Does    : Periodic minute rollover evaluation, target action deduplication (last action wins),
 *           local relay control via relay_hw, and ONCE repeat auto-disable.
 */

#include "schedule_engine.h"
#include "schedule_storage.h"
#include "ntp_time.h"
#include "relay_hw.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_defs.h"
#include "mesh_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "SCHED_ENGINE"

/* Spacing between scheduled commands. Manual control reaches the relays one message at a
 * time; a schedule issues them together, and back-to-back 4-byte UART frames can overrun the
 * Holtek receiver while it is sending the ACK for the previous frame. */
#define SCHED_ACTION_GAP_MS      50
/* Lamps on other nodes: Generic OnOff Set Unacknowledged, sent 3 times with the same TID
 * (the receiving server drops repeats with the same TID, so a lamp switches only once). */
#define SCHED_REMOTE_SEND_COUNT  3
#define SCHED_REMOTE_MSG_GAP_MS  20
#define SCHED_REMOTE_ROUND_MS    200

typedef struct {
    uint16_t target;
    uint8_t value;
    uint8_t sched_id;
} merged_action_t;

#define MAX_MERGED_ACTIONS 32

static sched_target_result_t s_history[SCHED_MAX_HISTORY];
static uint8_t s_history_count = 0;
static SemaphoreHandle_t s_engine_mutex = NULL;
static TaskHandle_t s_engine_task_handle = NULL;

static void record_history(uint16_t target, uint8_t value, uint8_t sched_id, bool success)
{
    if (xSemaphoreTake(s_engine_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_history_count < SCHED_MAX_HISTORY) {
            s_history_count++;
        }
        /* Shift older entries */
        for (int i = s_history_count - 1; i > 0; i--) {
            s_history[i] = s_history[i - 1];
        }
        s_history[0].target = target;
        s_history[0].value = value;
        s_history[0].sched_id = sched_id;
        s_history[0].timestamp = ntp_time_get_current_epoch();
        s_history[0].success = success;
        xSemaphoreGive(s_engine_mutex);
    }
}

/*
 * The app stores each selected lamp as its element unicast address (primary + element index),
 * the same address manual control uses. A provisioned Master owns exactly the 4 addresses
 * primary..primary+3 (element i -> relay i, as in mesh_callbacks_generic_server_cb). Raw relay
 * channels 0..3 are accepted only while unprovisioned: once provisioned, 0x0001..0x0003 are
 * real unicast addresses and must never be read as channel numbers.
 */
static bool resolve_target_to_relay(uint16_t target, uint8_t *out_relay_idx)
{
    if (!out_relay_idx) {
        return false;
    }

    if (esp_ble_mesh_node_is_provisioned()) {
        uint16_t primary_addr = esp_ble_mesh_get_primary_element_address();
        if (target >= primary_addr && target < (primary_addr + RELAY_COUNT)) {
            *out_relay_idx = (uint8_t)(target - primary_addr);
            return true;
        }
        return false;
    }

    if (target < RELAY_COUNT) {
        *out_relay_idx = (uint8_t)target;
        return true;
    }
    return false;
}

/* Master element-0 Generic OnOff Server (already bound to the AppKey by the app) used as the sender */
static esp_ble_mesh_model_t *get_onoff_sender(uint16_t *out_app_idx)
{
    esp_ble_mesh_comp_t *comp = mesh_models_get_composition();
    if (comp == NULL || comp->element_count == 0) {
        return NULL;
    }
    esp_ble_mesh_elem_t *elem = &comp->elements[0];
    for (int i = 0; i < elem->sig_model_count; i++) {
        esp_ble_mesh_model_t *model = &elem->sig_models[i];
        if (model->model_id != ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV) {
            continue;
        }
        for (int k = 0; k < CONFIG_BLE_MESH_MODEL_KEY_COUNT; k++) {
            if (model->keys[k] != ESP_BLE_MESH_KEY_UNUSED) {
                *out_app_idx = model->keys[k];
                return model;
            }
        }
        return NULL;
    }
    return NULL;
}

static esp_err_t send_remote_onoff(esp_ble_mesh_model_t *model, uint16_t app_idx,
                                   uint16_t dst, uint8_t onoff, uint8_t tid)
{
    esp_ble_mesh_msg_ctx_t ctx = {0};
    ctx.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    ctx.app_idx = app_idx;
    ctx.addr = dst;
    ctx.send_ttl = ESP_BLE_MESH_TTL_DEFAULT;

    uint8_t msg[2] = { onoff ? 1 : 0, tid };
    return esp_ble_mesh_server_model_send_msg(model, &ctx, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK,
                                              sizeof(msg), msg);
}

static void execute_merged_actions(merged_action_t *actions, uint8_t count)
{
    static uint8_t s_onoff_tid = 0;
    uint8_t remote_idx[MAX_MERGED_ACTIONS];
    uint8_t remote_tid[MAX_MERGED_ACTIONS];
    uint8_t remote_count = 0;
    bool local_sent = false;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t relay_idx = 0;
        if (resolve_target_to_relay(actions[i].target, &relay_idx)) {
            if (local_sent) {
                vTaskDelay(pdMS_TO_TICKS(SCHED_ACTION_GAP_MS));
            }
            ESP_LOGI(TAG, "[SCHED_EXEC] Schedule #%u: Setting Relay #%u (0x%04X) -> %s",
                     actions[i].sched_id, relay_idx + 1, actions[i].target, actions[i].value ? "ON" : "OFF");
            if (actions[i].value) {
                relay_hw_on(relay_idx);
            } else {
                relay_hw_off(relay_idx);
            }
            local_sent = true;
            record_history(actions[i].target, actions[i].value, actions[i].sched_id, true);
        } else if (esp_ble_mesh_node_is_provisioned() && ESP_BLE_MESH_ADDR_IS_UNICAST(actions[i].target) &&
                   remote_count < MAX_MERGED_ACTIONS) {
            remote_idx[remote_count] = i;
            remote_tid[remote_count] = ++s_onoff_tid;
            remote_count++;
        } else {
            ESP_LOGW(TAG, "[SCHED_EXEC] Target 0x%04X is not a valid lamp address; skipped.", actions[i].target);
            record_history(actions[i].target, actions[i].value, actions[i].sched_id, false);
        }
    }

    if (remote_count == 0) {
        return;
    }

    uint16_t app_idx = ESP_BLE_MESH_KEY_UNUSED;
    esp_ble_mesh_model_t *sender = get_onoff_sender(&app_idx);
    if (sender == NULL) {
        ESP_LOGE(TAG, "[SCHED_EXEC] No AppKey bound to the Master's Generic OnOff Server; %u remote action(s) not sent",
                 remote_count);
        for (uint8_t r = 0; r < remote_count; r++) {
            merged_action_t *a = &actions[remote_idx[r]];
            record_history(a->target, a->value, a->sched_id, false);
        }
        return;
    }

    bool sent_ok[MAX_MERGED_ACTIONS] = {0};
    for (int round = 0; round < SCHED_REMOTE_SEND_COUNT; round++) {
        if (round > 0) {
            vTaskDelay(pdMS_TO_TICKS(SCHED_REMOTE_ROUND_MS));
        }
        for (uint8_t r = 0; r < remote_count; r++) {
            merged_action_t *a = &actions[remote_idx[r]];
            if (r > 0) {
                vTaskDelay(pdMS_TO_TICKS(SCHED_REMOTE_MSG_GAP_MS));
            }
            esp_err_t err = send_remote_onoff(sender, app_idx, a->target, a->value, remote_tid[r]);
            if (err == ESP_OK) {
                sent_ok[r] = true;
            }
            if (round == 0) {
                ESP_LOGI(TAG, "[SCHED_EXEC] Schedule #%u: Remote lamp 0x%04X -> %s (TID %u)%s",
                         a->sched_id, a->target, a->value ? "ON" : "OFF", remote_tid[r],
                         err == ESP_OK ? "" : " - send failed");
            }
        }
    }
    for (uint8_t r = 0; r < remote_count; r++) {
        merged_action_t *a = &actions[remote_idx[r]];
        record_history(a->target, a->value, a->sched_id, sent_ok[r]);
    }
}

static void evaluate_and_run_schedules(const struct tm *ist_tm)
{
    uint16_t mask = sched_storage_get_active_mask();
    if (mask == 0) {
        return;
    }

    merged_action_t merged[MAX_MERGED_ACTIONS];
    uint8_t merged_count = 0;
    uint8_t once_schedules_to_disable[SCHED_MAX_COUNT];
    uint8_t once_count = 0;

    /* Iterate through active schedules in ascending order of ID */
    for (uint8_t id = 1; id <= SCHED_MAX_COUNT; id++) {
        if ((mask & (1 << (id - 1))) == 0) {
            continue;
        }

        schedule_entry_t entry;
        if (sched_storage_load(id, &entry) != ESP_OK) {
            continue;
        }

        if (schedule_is_due(&entry, ist_tm)) {
            ESP_LOGI(TAG, "[TRIGGER] Schedule #%u ('%s') is due at %02d:%02d IST",
                     entry.id, entry.name, entry.hour, entry.minute);

            /* Merge actions: if target already in list, LAST action wins */
            for (uint8_t a = 0; a < entry.action_count; a++) {
                uint16_t target = entry.actions[a].target;
                uint8_t val = entry.actions[a].value;

                int existing_idx = -1;
                for (uint8_t m = 0; m < merged_count; m++) {
                    if (merged[m].target == target) {
                        existing_idx = m;
                        break;
                    }
                }

                if (existing_idx >= 0) {
                    ESP_LOGI(TAG, "Duplicate target 0x%04X in minute: schedule #%u overrides previous action with %u (LAST action wins)",
                             target, entry.id, val);
                    merged[existing_idx].value = val;
                    merged[existing_idx].sched_id = entry.id;
                } else if (merged_count < MAX_MERGED_ACTIONS) {
                    merged[merged_count].target = target;
                    merged[merged_count].value = val;
                    merged[merged_count].sched_id = entry.id;
                    merged_count++;
                }
            }

            if (entry.repeat_type == SCHED_REPEAT_ONCE) {
                once_schedules_to_disable[once_count++] = entry.id;
            }
        }
    }

    if (merged_count > 0) {
        ESP_LOGI(TAG, "Executing %u merged actions for minute %02d:%02d IST",
                 merged_count, ist_tm->tm_hour, ist_tm->tm_min);
        execute_merged_actions(merged, merged_count);
    }

    /* Auto-disable ONCE schedules that have executed */
    for (uint8_t i = 0; i < once_count; i++) {
        schedule_entry_t entry;
        if (sched_storage_load(once_schedules_to_disable[i], &entry) == ESP_OK) {
            entry.enabled = 0;
            entry.crc32 = schedule_calc_crc32(&entry);
            sched_storage_save(&entry);
            ESP_LOGI(TAG, "Schedule #%u (ONCE) completed and disabled.", entry.id);
        }
    }
}

static void sched_engine_task(void *pvParameters)
{
    (void)pvParameters;
    time_t last_evaluated_minute_epoch = 0;
    TickType_t last_wait_log = 0;
    TickType_t last_nvs_sync = 0;

    ESP_LOGI(TAG, "Scheduler Engine Task running (Priority 3, RTC-based timekeeping)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* Poll once per second */

        if (!ntp_time_is_synced()) {
            TickType_t now_ticks = xTaskGetTickCount();
            if ((now_ticks - last_wait_log) >= pdMS_TO_TICKS(60000)) {
                last_wait_log = now_ticks;
                ESP_LOGI(TAG, "Waiting for initial NTP time synchronization; scheduler safely paused.");
            }
            continue;
        }

        time_t now = ntp_time_get_current_epoch();
        if (now < 1735689600) { /* 2025-01-01 00:00:00 UTC safety threshold */
            continue;
        }

        /* Periodic timestamp persistence to NVS every 15 minutes (900 seconds) */
        TickType_t current_ticks = xTaskGetTickCount();
        if ((current_ticks - last_nvs_sync) >= pdMS_TO_TICKS(900000)) {
            last_nvs_sync = current_ticks;
            ntp_time_persist_timestamp(now);
        }

        time_t current_minute_epoch = now - (now % 60);

        if (last_evaluated_minute_epoch == 0) {
            /* Initial minute evaluation after calibration / boot */
            last_evaluated_minute_epoch = current_minute_epoch;
            struct tm ist_tm;
            if (ntp_time_get_ist(&ist_tm)) {
                ESP_LOGI(TAG, "Initial minute evaluated: %04d-%02d-%02d %02d:%02d IST",
                         ist_tm.tm_year + 1900, ist_tm.tm_mon + 1, ist_tm.tm_mday,
                         ist_tm.tm_hour, ist_tm.tm_min);
                evaluate_and_run_schedules(&ist_tm);
            }
            continue;
        }

        if (current_minute_epoch == last_evaluated_minute_epoch) {
            /* Already evaluated this minute */
            continue;
        }

        if (current_minute_epoch < last_evaluated_minute_epoch) {
            /* Clock adjusted backward (e.g. NTP time correction). Suppress duplicate execution. */
            time_t backward_diff = last_evaluated_minute_epoch - current_minute_epoch;
            if (backward_diff > 3600) {
                ESP_LOGW(TAG, "Extreme backward clock adjustment (%lld s). Re-aligning scheduler.", (long long)backward_diff);
                last_evaluated_minute_epoch = current_minute_epoch;
            } else {
                ESP_LOGW(TAG, "Clock adjusted backward by %lld s. Suppressing duplicate execution for minute %lld.",
                         (long long)backward_diff, (long long)current_minute_epoch);
            }
            continue;
        }

        /* current_minute_epoch > last_evaluated_minute_epoch */
        time_t forward_diff = current_minute_epoch - last_evaluated_minute_epoch;

        if (forward_diff == 60) {
            /* Normal 1-minute rollover (including midnight rollover) */
            last_evaluated_minute_epoch = current_minute_epoch;
            struct tm ist_tm;
            if (ntp_time_get_ist(&ist_tm)) {
                evaluate_and_run_schedules(&ist_tm);
            }
        } else if (forward_diff <= 900) {
            /* Forward jump up to 15 minutes (e.g. NTP resync forward step).
             * Catch up each missed minute sequentially so no scheduled event is permanently missed. */
            ESP_LOGI(TAG, "Clock stepped forward by %lld s. Catching up %lld missed minute(s)...",
                     (long long)forward_diff, (long long)(forward_diff / 60));
            for (time_t m = last_evaluated_minute_epoch + 60; m <= current_minute_epoch; m += 60) {
                struct tm m_tm;
                localtime_r(&m, &m_tm);
                ESP_LOGI(TAG, "Catch-up evaluate minute: %04d-%02d-%02d %02d:%02d IST",
                         m_tm.tm_year + 1900, m_tm.tm_mon + 1, m_tm.tm_mday,
                         m_tm.tm_hour, m_tm.tm_min);
                evaluate_and_run_schedules(&m_tm);
            }
            last_evaluated_minute_epoch = current_minute_epoch;
        } else {
            /* Large forward jump (> 15 minutes). Re-align without blasting stale events. */
            ESP_LOGW(TAG, "Large clock jump forward (%lld s). Aligning scheduler to current minute.", (long long)forward_diff);
            last_evaluated_minute_epoch = current_minute_epoch;
            struct tm ist_tm;
            if (ntp_time_get_ist(&ist_tm)) {
                evaluate_and_run_schedules(&ist_tm);
            }
        }
    }
}

esp_err_t sched_engine_init(void)
{
    s_engine_mutex = xSemaphoreCreateMutex();
    memset(s_history, 0, sizeof(s_history));

    sched_storage_init();

#if defined(CONFIG_SCHED_ENABLE) && !CONFIG_SCHED_ENABLE
    ESP_LOGI(TAG, "CONFIG_SCHED_ENABLE is disabled; Scheduler Engine task not created.");
#else
    xTaskCreate(sched_engine_task, "sched_task", 4096, NULL, 3, &s_engine_task_handle);
    ESP_LOGI(TAG, "Scheduler Engine initialized with %u active schedules.", sched_storage_get_count());
#endif
    return ESP_OK;
}

esp_err_t sched_engine_run_now(uint8_t id)
{
    schedule_entry_t entry;
    esp_err_t err = sched_storage_load(id, &entry);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sched_engine_run_now: Schedule #%u not found (%s)", id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "[RUN_NOW] Testing Schedule #%u ('%s') with %u actions", entry.id, entry.name, entry.action_count);
    merged_action_t actions[SCHED_MAX_ACTIONS];
    for (uint8_t i = 0; i < entry.action_count; i++) {
        actions[i].target = entry.actions[i].target;
        actions[i].value = entry.actions[i].value;
        actions[i].sched_id = entry.id;
    }

    execute_merged_actions(actions, entry.action_count);
    return ESP_OK;
}

void sched_engine_print_status(void)
{
    bool synced = false;
    time_t last_sync = 0;
    int16_t tz_offset = 0;
    const char *tz_name = NULL;
    ntp_time_get_status(&synced, &last_sync, &tz_offset, &tz_name);

    struct tm ist_tm;
    bool time_valid = ntp_time_get_ist(&ist_tm);

    printf("\n=== SCHEDULER ENGINE STATUS ===\n");
    printf("Timekeeping    : %s\n", time_valid ? "RTC Active (Valid IST)" : "UNCALIBRATED (Awaiting NTP)");
    printf("NTP Calibrated : %s\n", synced ? "YES" : "NO");
    printf("Network State  : %s\n", ntp_time_is_network_connected() ? "CONNECTED (Wi-Fi Online)" : "OFFLINE (Autonomous RTC Scheduling)");
    printf("Timezone       : %s (UTC%+d:%02d)\n", tz_name, tz_offset / 60, tz_offset % 60);
    if (time_valid) {
        printf("Current IST    : %04d-%02d-%02d %02d:%02d:%02d\n",
               ist_tm.tm_year + 1900, ist_tm.tm_mon + 1, ist_tm.tm_mday,
               ist_tm.tm_hour, ist_tm.tm_min, ist_tm.tm_sec);
    } else {
        printf("Current IST    : UNCALIBRATED (awaiting NTP)\n");
    }
    printf("Stored Schedules: %u / %d\n", sched_storage_get_count(), SCHED_MAX_COUNT);
    printf("Active Mask    : 0x%04X\n", sched_storage_get_active_mask());

    uint16_t mask = sched_storage_get_active_mask();
    for (uint8_t id = 1; id <= SCHED_MAX_COUNT; id++) {
        if (mask & (1 << (id - 1))) {
            schedule_entry_t s;
            if (sched_storage_load(id, &s) == ESP_OK) {
                const char *rep_str = (s.repeat_type == 0) ? "ONCE" :
                                      (s.repeat_type == 1) ? "DAILY" :
                                      (s.repeat_type == 2) ? "WEEKDAYS" :
                                      (s.repeat_type == 3) ? "WEEKENDS" : "CUSTOM";
                printf("  [ID %2u] %s | %02d:%02d | %-8s | %s | %u actions\n",
                       s.id, s.name, s.hour, s.minute, rep_str, s.enabled ? "ENABLED" : "DISABLED", s.action_count);
            }
        }
    }

    printf("\nRecent Execution History (latest first):\n");
    if (xSemaphoreTake(s_engine_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_history_count == 0) {
            printf("  (No execution history)\n");
        } else {
            for (uint8_t i = 0; i < s_history_count; i++) {
                struct tm t_tm;
                localtime_r(&s_history[i].timestamp, &t_tm);
                printf("  [%02d:%02d:%02d] Sched #%u -> Target %u := %s (%s)\n",
                       t_tm.tm_hour, t_tm.tm_min, t_tm.tm_sec,
                       s_history[i].sched_id, s_history[i].target,
                       s_history[i].value ? "ON" : "OFF",
                       s_history[i].success ? "SUCCESS" : "FAIL");
            }
        }
        xSemaphoreGive(s_engine_mutex);
    }
    printf("================================\n\n");
}

uint8_t sched_engine_get_history(sched_target_result_t *out_history, uint8_t max_count)
{
    if (!out_history || max_count == 0) {
        return 0;
    }
    uint8_t copied = 0;
    if (xSemaphoreTake(s_engine_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        copied = (s_history_count < max_count) ? s_history_count : max_count;
        memcpy(out_history, s_history, copied * sizeof(sched_target_result_t));
        xSemaphoreGive(s_engine_mutex);
    }
    return copied;
}
