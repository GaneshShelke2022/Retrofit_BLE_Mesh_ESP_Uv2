/*
 * File    : schedule_mesh_vendor.c
 * Purpose : Bluetooth Mesh Vendor Server Model implementation for schedules.
 * Does    : Opcode dispatching, chunked staging buffer, NVS commit, non-blocking queue.
 */

#include "schedule_mesh_vendor.h"
#include "schedule_storage.h"
#include "schedule_engine.h"
#include "ntp_time.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ble_mesh_networking_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define TAG "SCHED_VENDOR"

#define MAX_STAGING_BUFFERS 2
#define STAGING_TIMEOUT_US  15000000ULL /* 15 seconds */
#define VENDOR_QUEUE_SIZE   16
#define RATE_LIMIT_MAX_MSG  8           /* Max 8 requests per second */

typedef struct {
    bool active;
    uint16_t src_addr;
    uint8_t transaction_id;
    int64_t start_time_us;
    schedule_entry_t sched;
    uint8_t actions_received;
    int8_t last_seq;
} staging_buffer_t;

typedef struct {
    uint32_t opcode;
    uint16_t length;
    uint8_t msg[64];
    esp_ble_mesh_msg_ctx_t ctx;
    esp_ble_mesh_model_t *model;
} vendor_msg_item_t;

esp_ble_mesh_model_op_t sched_vnd_opcodes[] = {
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_BEGIN, 29),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_ACTIONS, 3),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_COMMIT, 5),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_DELETE, 2),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_ENABLE, 3),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_CLEAR, 1),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_GET, 2),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_LIST, 1),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_RUN_NOW, 2),
    ESP_BLE_MESH_MODEL_OP(SCHED_VND_OP_TIME_GET, 1),
    ESP_BLE_MESH_MODEL_OP_END,
};

static staging_buffer_t s_staging[MAX_STAGING_BUFFERS];
static QueueHandle_t s_vendor_rx_queue = NULL;
static TaskHandle_t s_vendor_worker_task = NULL;
static SemaphoreHandle_t s_vendor_mutex = NULL;

/* Rate limiter tracking */
static int64_t s_rate_limit_window_start = 0;
static uint32_t s_rate_limit_count = 0;

esp_ble_mesh_model_op_t *schedule_mesh_vendor_get_opcodes(void)
{
    return sched_vnd_opcodes;
}

/* =========================================================================
 * Wire Protocol Encode / Decode Helpers
 * ========================================================================= */

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
    if (!out_buf || buf_len < 3) {
        return 0;
    }
    out_buf[0] = status;
    out_buf[1] = tid;
    out_buf[2] = id;
    return 3;
}

bool sched_decode_status(const uint8_t *in_buf, size_t len, uint8_t *out_status, uint8_t *out_tid, uint8_t *out_id)
{
    if (!in_buf || len < 3 || !out_status) {
        return false;
    }
    *out_status = in_buf[0];
    if (out_tid) *out_tid = in_buf[1];
    if (out_id) *out_id = in_buf[2];
    return true;
}

size_t sched_encode_list_status(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint8_t count, uint16_t mask)
{
    if (!out_buf || buf_len < 4) {
        return 0;
    }
    out_buf[0] = tid;
    out_buf[1] = count;
    out_buf[2] = (uint8_t)(mask & 0xFF);
    out_buf[3] = (uint8_t)((mask >> 8) & 0xFF);
    return 4;
}

bool sched_decode_list_status(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint8_t *out_count, uint16_t *out_mask)
{
    if (!in_buf || len < 4 || !out_count || !out_mask) {
        return false;
    }
    if (out_tid) *out_tid = in_buf[0];
    *out_count = in_buf[1];
    *out_mask = (uint16_t)in_buf[2] | ((uint16_t)in_buf[3] << 8);
    return true;
}

size_t sched_encode_time_status(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint8_t time_valid, uint32_t last_epoch, int16_t tz_offset, uint8_t count, uint8_t free_slots, uint8_t version)
{
    if (!out_buf || buf_len < 11) {
        return 0;
    }
    out_buf[0] = tid;
    out_buf[1] = time_valid;
    out_buf[2] = (uint8_t)(last_epoch & 0xFF);
    out_buf[3] = (uint8_t)((last_epoch >> 8) & 0xFF);
    out_buf[4] = (uint8_t)((last_epoch >> 16) & 0xFF);
    out_buf[5] = (uint8_t)((last_epoch >> 24) & 0xFF);
    out_buf[6] = (uint8_t)(tz_offset & 0xFF);
    out_buf[7] = (uint8_t)((tz_offset >> 8) & 0xFF);
    out_buf[8] = count;
    out_buf[9] = free_slots;
    out_buf[10] = version;
    return 11;
}

bool sched_decode_time_status(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint8_t *out_time_valid, uint32_t *out_last_epoch, int16_t *out_tz_offset, uint8_t *out_count, uint8_t *out_free_slots, uint8_t *out_version)
{
    if (!in_buf || len < 11 || !out_time_valid || !out_last_epoch || !out_tz_offset || !out_count || !out_free_slots || !out_version) {
        return false;
    }
    if (out_tid) *out_tid = in_buf[0];
    *out_time_valid = in_buf[1];
    *out_last_epoch = (uint32_t)in_buf[2] | ((uint32_t)in_buf[3] << 8) | ((uint32_t)in_buf[4] << 16) | ((uint32_t)in_buf[5] << 24);
    *out_tz_offset = (int16_t)((uint16_t)in_buf[6] | ((uint16_t)in_buf[7] << 8));
    *out_count = in_buf[8];
    *out_free_slots = in_buf[9];
    *out_version = in_buf[10];
    return true;
}

/* =========================================================================
 * Staging Buffer Management
 * ========================================================================= */

static staging_buffer_t *find_or_alloc_staging(uint16_t src_addr, uint8_t tid)
{
    int64_t now_us = esp_timer_get_time();
    staging_buffer_t *free_slot = NULL;

    for (int i = 0; i < MAX_STAGING_BUFFERS; i++) {
        if (s_staging[i].active) {
            /* Check expiry */
            if ((now_us - s_staging[i].start_time_us) > STAGING_TIMEOUT_US) {
                ESP_LOGW(TAG, "Staging buffer for src 0x%04X expired; discarding.", s_staging[i].src_addr);
                memset(&s_staging[i], 0, sizeof(staging_buffer_t));
                if (!free_slot) free_slot = &s_staging[i];
            } else if (s_staging[i].src_addr == src_addr) {
                /* Reset existing slot for new transaction */
                memset(&s_staging[i], 0, sizeof(staging_buffer_t));
                s_staging[i].active = true;
                s_staging[i].src_addr = src_addr;
                s_staging[i].transaction_id = tid;
                s_staging[i].start_time_us = now_us;
                s_staging[i].last_seq = -1;
                return &s_staging[i];
            }
        } else {
            if (!free_slot) free_slot = &s_staging[i];
        }
    }

    if (free_slot) {
        memset(free_slot, 0, sizeof(staging_buffer_t));
        free_slot->active = true;
        free_slot->src_addr = src_addr;
        free_slot->transaction_id = tid;
        free_slot->start_time_us = now_us;
        free_slot->last_seq = -1;
        return free_slot;
    }

    return NULL;
}

static staging_buffer_t *get_active_staging(uint16_t src_addr)
{
    int64_t now_us = esp_timer_get_time();
    for (int i = 0; i < MAX_STAGING_BUFFERS; i++) {
        if (s_staging[i].active && s_staging[i].src_addr == src_addr) {
            if ((now_us - s_staging[i].start_time_us) > STAGING_TIMEOUT_US) {
                ESP_LOGW(TAG, "Staging buffer for src 0x%04X expired; discarding.", src_addr);
                memset(&s_staging[i], 0, sizeof(staging_buffer_t));
                return NULL;
            }
            return &s_staging[i];
        }
    }
    return NULL;
}

/* =========================================================================
 * Worker Task & Message Handlers
 * ========================================================================= */

static void send_status_reply(esp_ble_mesh_model_t *model, esp_ble_mesh_msg_ctx_t *ctx, uint8_t status_code, uint8_t tid, uint8_t id)
{
    uint8_t payload[4];
    size_t len = sched_encode_status(payload, sizeof(payload), status_code, tid, id);
    esp_err_t err = esp_ble_mesh_server_model_send_msg(model, ctx, SCHED_VND_OP_STATUS, len, payload);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send SCHED_STATUS reply: %s", esp_err_to_name(err));
    }
}

static void process_vendor_message(const vendor_msg_item_t *item)
{
    uint32_t op = item->opcode;
    const uint8_t *data = item->msg;
    size_t len = item->length;
    uint16_t src = item->ctx.addr;

    xSemaphoreTake(s_vendor_mutex, portMAX_DELAY);

    if (op == SCHED_VND_OP_BEGIN) {
        uint8_t tid = 0;
        schedule_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        if (!sched_decode_begin(data, len, &tid, &entry)) {
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_INVALID_PARAM, tid, 0);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        if (entry.id < 1 || entry.id > SCHED_MAX_COUNT) {
            ESP_LOGW(TAG, "SCHED_BEGIN: Invalid schedule ID %u (must be 1..%u) from 0x%04X (TID %u)",
                     entry.id, SCHED_MAX_COUNT, src, tid);
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_BAD_RANGE, tid, entry.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        staging_buffer_t *sb = find_or_alloc_staging(src, tid);
        if (!sb) {
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_BUSY, tid, entry.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        sb->transaction_id = tid;
        memset(&sb->sched, 0, sizeof(schedule_entry_t));
        sb->sched = entry;
        sb->actions_received = 0;
        sb->last_seq = -1;
        sb->start_time_us = esp_timer_get_time();

        ESP_LOGI(TAG, "SCHED_BEGIN: Sched #%u ('%s') staged from 0x%04X (TID %u)", entry.id, entry.name, src, tid);
        send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_OK, tid, entry.id);

    } else if (op == SCHED_VND_OP_ACTIONS) {
        uint8_t tid = 0, seq = 0, count = 0;
        sched_action_t actions[SCHED_MAX_ACTIONS];
        memset(actions, 0, sizeof(actions));
        if (!sched_decode_actions(data, len, &tid, &seq, actions, &count)) {
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_INVALID_PARAM, tid, 0);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        staging_buffer_t *sb = get_active_staging(src);
        if (!sb || sb->transaction_id != tid) {
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_STAGING_TIMEOUT, tid, 0);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        /* Deduplicate chunk retransmission from mesh client */
        if (seq == sb->last_seq && sb->actions_received >= count) {
            ESP_LOGW(TAG, "SCHED_ACTIONS: Duplicate chunk seq %u received for Sched #%u; replying OK", seq, sb->sched.id);
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_OK, tid, sb->sched.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        if (seq == 0) {
            sb->actions_received = 0;
            memset(sb->sched.actions, 0, sizeof(sb->sched.actions));
        }

        if ((sb->actions_received + count) > SCHED_MAX_ACTIONS) {
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_TOO_MANY_ACTIONS, tid, sb->sched.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        for (uint8_t i = 0; i < count; i++) {
            sb->sched.actions[sb->actions_received++] = actions[i];
        }
        sb->last_seq = seq;

        ESP_LOGI(TAG, "SCHED_ACTIONS: Added %u actions (total %u / %u, seq %u) for Sched #%u",
                 count, sb->actions_received, sb->sched.action_count, seq, sb->sched.id);
        send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_OK, tid, sb->sched.id);

    } else if (op == SCHED_VND_OP_COMMIT) {
        uint8_t tid = 0;
        uint32_t expected_crc = 0;
        if (!sched_decode_commit(data, len, &tid, &expected_crc)) {
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_INVALID_PARAM, tid, 0);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        staging_buffer_t *sb = get_active_staging(src);
        if (!sb || sb->transaction_id != tid) {
            /* Check if already committed in NVS (idempotency check) */
            uint8_t found_id = 0;
            if (sched_storage_find_by_crc(expected_crc, &found_id) == ESP_OK) {
                ESP_LOGI(TAG, "SCHED_COMMIT: Retransmission detected; Sched #%u already committed (CRC 0x%08lX)",
                         found_id, (unsigned long)expected_crc);
                send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_OK, tid, found_id);
            } else {
                send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_STAGING_TIMEOUT, tid, 0);
            }
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        sb->sched.action_count = sb->actions_received;
        /* Zero any remaining unused action slots up to SCHED_MAX_ACTIONS */
        for (int i = sb->actions_received; i < SCHED_MAX_ACTIONS; i++) {
            memset(&sb->sched.actions[i], 0, sizeof(sched_action_t));
        }

        uint32_t actual_crc = schedule_calc_crc32(&sb->sched);
        if (actual_crc != expected_crc) {
            ESP_LOGE(TAG, "SCHED_COMMIT: CRC mismatch (calc 0x%08lX != expected 0x%08lX, actions %u); discarding staging.",
                     (unsigned long)actual_crc, (unsigned long)expected_crc, sb->actions_received);
            memset(sb, 0, sizeof(staging_buffer_t));
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_CRC_MISMATCH, tid, sb->sched.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        sb->sched.crc32 = actual_crc;
        sched_err_t val_res = schedule_validate(&sb->sched);
        if (val_res != SCHED_ERR_OK) {
            ESP_LOGE(TAG, "SCHED_COMMIT: Validation failed with error %d", val_res);
            memset(sb, 0, sizeof(staging_buffer_t));
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_BAD_RANGE, tid, sb->sched.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        esp_err_t save_err = sched_storage_save(&sb->sched);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "SCHED_COMMIT: NVS save failed with error %s", esp_err_to_name(save_err));
            memset(sb, 0, sizeof(staging_buffer_t));
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_STORAGE_FULL, tid, sb->sched.id);
            xSemaphoreGive(s_vendor_mutex);
            return;
        }

        uint8_t sched_id = sb->sched.id;
        memset(sb, 0, sizeof(staging_buffer_t)); /* Clear staging buffer cleanly */

        uint8_t status_reply_code = SCHED_STATUS_OK;
        if (!ntp_time_is_synced()) {
            status_reply_code = SCHED_STATUS_WARN_TIME_NOT_SYNCED;
        }

        ESP_LOGI(TAG, "SCHED_COMMIT: Schedule #%u saved to NVS successfully (CRC 0x%08lX)", sched_id, (unsigned long)actual_crc);
        send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, status_reply_code, tid, sched_id);

    } else if (op == SCHED_VND_OP_DELETE) {
        if (len >= 2) {
            uint8_t tid = data[0];
            uint8_t id = data[1];
            esp_err_t del_err = sched_storage_delete(id);
            uint8_t st = (del_err == ESP_OK) ? SCHED_STATUS_OK : SCHED_STATUS_ERR_UNKNOWN_ID;
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, st, tid, id);
        }
    } else if (op == SCHED_VND_OP_ENABLE) {
        if (len >= 3) {
            uint8_t tid = data[0];
            uint8_t id = data[1];
            uint8_t enable_val = data[2];
            schedule_entry_t entry;
            if (sched_storage_load(id, &entry) == ESP_OK) {
                entry.enabled = enable_val ? 1 : 0;
                entry.crc32 = schedule_calc_crc32(&entry);
                sched_storage_save(&entry);
                send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_OK, tid, id);
            } else {
                send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_UNKNOWN_ID, tid, id);
            }
        }
    } else if (op == SCHED_VND_OP_CLEAR) {
        if (len >= 1) {
            uint8_t tid = data[0];
            sched_storage_clear_all();
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_OK, tid, 0);
        }
    } else if (op == SCHED_VND_OP_GET) {
        if (len >= 2) {
            uint8_t tid = data[0];
            uint8_t id = data[1];
            schedule_entry_t entry;
            if (sched_storage_load(id, &entry) == ESP_OK) {
                /* Reply with SCHED_BEGIN */
                uint8_t out[32];
                size_t enc_len = sched_encode_begin(out, sizeof(out), tid, &entry);
                esp_ble_mesh_server_model_send_msg(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_VND_OP_BEGIN, enc_len, out);

                /* Reply with SCHED_ACTIONS */
                if (entry.action_count > 0) {
                    enc_len = sched_encode_actions(out, sizeof(out), tid, 0, entry.actions, entry.action_count);
                    esp_ble_mesh_server_model_send_msg(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_VND_OP_ACTIONS, enc_len, out);
                }
            } else {
                send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_STATUS_ERR_UNKNOWN_ID, tid, id);
            }
        }
    } else if (op == SCHED_VND_OP_LIST) {
        uint8_t tid = (len >= 1) ? data[0] : 0;
        uint8_t count = sched_storage_get_count();
        uint16_t mask = sched_storage_get_active_mask();
        uint8_t out[8];
        size_t enc_len = sched_encode_list_status(out, sizeof(out), tid, count, mask);
        esp_ble_mesh_server_model_send_msg(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_VND_OP_LIST_STATUS, enc_len, out);

    } else if (op == SCHED_VND_OP_RUN_NOW) {
        if (len >= 2) {
            uint8_t tid = data[0];
            uint8_t id = data[1];
            esp_err_t run_err = sched_engine_run_now(id);
            uint8_t st = (run_err == ESP_OK) ? SCHED_STATUS_OK : SCHED_STATUS_ERR_UNKNOWN_ID;
            send_status_reply(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, st, tid, id);
        }
    } else if (op == SCHED_VND_OP_TIME_GET) {
        uint8_t tid = (len >= 1) ? data[0] : 0;
        bool synced = false;
        time_t last_epoch = 0;
        int16_t tz_offset = 0;
        const char *tz_name = NULL;
        ntp_time_get_status(&synced, &last_epoch, &tz_offset, &tz_name);

        uint8_t count = sched_storage_get_count();
        uint8_t free_slots = (count < SCHED_MAX_COUNT) ? (SCHED_MAX_COUNT - count) : 0;

        uint8_t out[16];
        size_t enc_len = sched_encode_time_status(out, sizeof(out), tid, synced ? 1 : 0,
                                                  (uint32_t)last_epoch, tz_offset, count,
                                                  free_slots, SCHED_PROTOCOL_VERSION);
        esp_ble_mesh_server_model_send_msg(item->model, (esp_ble_mesh_msg_ctx_t *)&item->ctx, SCHED_VND_OP_TIME_STATUS, enc_len, out);
    }

    xSemaphoreGive(s_vendor_mutex);
}

static void schedule_vendor_worker_task(void *pvParameters)
{
    (void)pvParameters;
    vendor_msg_item_t item;
    ESP_LOGI(TAG, "Schedule Vendor Model worker task active (Priority 3)");

    while (1) {
        if (xQueueReceive(s_vendor_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            process_vendor_message(&item);
        }
    }
}

/* =========================================================================
 * Public Entry Points & Callback
 * ========================================================================= */

esp_err_t schedule_mesh_vendor_init(void)
{
    s_vendor_mutex = xSemaphoreCreateMutex();
    memset(s_staging, 0, sizeof(s_staging));

    s_vendor_rx_queue = xQueueCreate(VENDOR_QUEUE_SIZE, sizeof(vendor_msg_item_t));
    if (!s_vendor_rx_queue) {
        ESP_LOGE(TAG, "Failed to create vendor RX queue!");
        return ESP_ERR_NO_MEM;
    }

    xTaskCreate(schedule_vendor_worker_task, "sched_vnd_task", 4096, NULL, 3, &s_vendor_worker_task);
    ESP_LOGI(TAG, "Schedule Vendor Model initialized (Company ID 0x%04X, Model ID 0x%04X)",
             CONFIG_SCHED_VENDOR_COMPANY_ID, CONFIG_SCHED_VENDOR_MODEL_ID);
    return ESP_OK;
}

void schedule_mesh_vendor_cb(esp_ble_mesh_model_cb_event_t event,
                             esp_ble_mesh_model_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_MODEL_OPERATION_EVT) {
        if (!param || !param->model_operation.model || !param->model_operation.ctx) {
            return;
        }

        /* Check rate limit (max 8 messages/sec) */
        int64_t now_us = esp_timer_get_time();
        if ((now_us - s_rate_limit_window_start) >= 1000000ULL) {
            s_rate_limit_window_start = now_us;
            s_rate_limit_count = 1;
        } else {
            s_rate_limit_count++;
            if (s_rate_limit_count > RATE_LIMIT_MAX_MSG) {
                ESP_LOGW(TAG, "Incoming vendor message rate limit exceeded (> %d/s); dropping.", RATE_LIMIT_MAX_MSG);
                return;
            }
        }

        uint16_t msg_len = param->model_operation.length;
        if (msg_len > sizeof(((vendor_msg_item_t *)0)->msg)) {
            ESP_LOGW(TAG, "Oversized vendor message (%u bytes); dropping.", msg_len);
            return;
        }

        vendor_msg_item_t item;
        item.opcode = param->model_operation.opcode;
        item.length = msg_len;
        item.model = param->model_operation.model;
        item.ctx = *param->model_operation.ctx;
        if (msg_len > 0 && param->model_operation.msg) {
            memcpy(item.msg, param->model_operation.msg, msg_len);
        }

        /* Forward to worker queue without blocking mesh callback */
        if (s_vendor_rx_queue) {
            if (xQueueSend(s_vendor_rx_queue, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Vendor RX queue full; dropping message 0x%06lX", (unsigned long)item.opcode);
            }
        }
    } else if (event == ESP_BLE_MESH_MODEL_SEND_COMP_EVT) {
        ESP_LOGD(TAG, "Vendor model send complete, err_code: %d", param->model_send_comp.err_code);
    }
}
