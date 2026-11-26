/*
 * File    : schedule_storage.c
 * Purpose : Persistent NVS storage implementation for schedules.
 * Does    : Atomic NVS blob commit with CRC32 verification and bitmask index tracking.
 */

#include "schedule_storage.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "SCHED_STORAGE"
#define NVS_KEY_MASK "mask"

static uint16_t s_active_mask = 0;
static SemaphoreHandle_t s_storage_mutex = NULL;

static void format_key(uint8_t id, char *out_buf, size_t buf_len)
{
    snprintf(out_buf, buf_len, "sc_%02x", id);
}

esp_err_t sched_storage_init(void)
{
    if (!s_storage_mutex) {
        s_storage_mutex = xSemaphoreCreateMutex();
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", SCHED_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    uint16_t mask = 0;
    err = nvs_get_u16(handle, NVS_KEY_MASK, &mask);
    if (err == ESP_OK) {
        s_active_mask = mask;
        ESP_LOGI(TAG, "Loaded schedule active mask: 0x%04X (count: %u)", s_active_mask, sched_storage_get_count());
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_active_mask = 0;
        nvs_set_u16(handle, NVS_KEY_MASK, 0);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Initialized new schedule index mask in NVS.");
    } else {
        ESP_LOGE(TAG, "Failed to read schedule mask: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t sched_storage_save(const schedule_entry_t *entry)
{
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }

    sched_err_t val_err = schedule_validate(entry);
    if (val_err != SCHED_ERR_OK) {
        ESP_LOGE(TAG, "Schedule validation failed with error code %d", val_err);
        return ESP_ERR_INVALID_ARG;
    }

    /* Verify CRC32 matches calculated */
    uint32_t expected_crc = schedule_calc_crc32(entry);
    if (entry->crc32 != expected_crc) {
        ESP_LOGE(TAG, "Schedule CRC32 mismatch: got 0x%08lX, expected 0x%08lX",
                 (unsigned long)entry->crc32, (unsigned long)expected_crc);
        return ESP_ERR_INVALID_CRC;
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        return err;
    }

    char key[16];
    format_key(entry->id, key, sizeof(key));

    /* Write blob */
    err = nvs_set_blob(handle, key, entry, sizeof(schedule_entry_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    /* Read back and verify write */
    if (err == ESP_OK) {
        schedule_entry_t verify_buf;
        size_t read_len = sizeof(verify_buf);
        err = nvs_get_blob(handle, key, &verify_buf, &read_len);
        if (err == ESP_OK && read_len == sizeof(schedule_entry_t) &&
            schedule_calc_crc32(&verify_buf) == expected_crc) {
            /* Update bitmask in RAM and NVS */
            s_active_mask |= (1 << (entry->id - 1));
            nvs_set_u16(handle, NVS_KEY_MASK, s_active_mask);
            nvs_commit(handle);
            ESP_LOGI(TAG, "Schedule #%u ('%s') saved and verified in NVS successfully.", entry->id, entry->name);
        } else {
            ESP_LOGE(TAG, "Schedule #%u readback verification failed!", entry->id);
            err = ESP_ERR_INVALID_CRC;
        }
    }

    nvs_close(handle);
    xSemaphoreGive(s_storage_mutex);
    return err;
}

esp_err_t sched_storage_load(uint8_t id, schedule_entry_t *out_entry)
{
    if (id < 1 || id > SCHED_MAX_COUNT || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((s_active_mask & (1 << (id - 1))) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        return err;
    }

    char key[16];
    format_key(id, key, sizeof(key));

    size_t len = sizeof(schedule_entry_t);
    err = nvs_get_blob(handle, key, out_entry, &len);
    nvs_close(handle);
    xSemaphoreGive(s_storage_mutex);

    if (err == ESP_OK) {
        if (len != sizeof(schedule_entry_t) || schedule_calc_crc32(out_entry) != out_entry->crc32) {
            ESP_LOGE(TAG, "Loaded schedule #%u has corrupted data or CRC mismatch!", id);
            return ESP_ERR_INVALID_CRC;
        }
    }

    return err;
}

esp_err_t sched_storage_delete(uint8_t id)
{
    if (id < 1 || id > SCHED_MAX_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        return err;
    }

    char key[16];
    format_key(id, key, sizeof(key));

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        s_active_mask &= ~(1 << (id - 1));
        nvs_set_u16(handle, NVS_KEY_MASK, s_active_mask);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Schedule #%u deleted from NVS.", id);
        err = ESP_OK;
    }

    nvs_close(handle);
    xSemaphoreGive(s_storage_mutex);
    return err;
}

esp_err_t sched_storage_clear_all(void)
{
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCHED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_storage_mutex);
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        s_active_mask = 0;
        nvs_set_u16(handle, NVS_KEY_MASK, 0);
        nvs_commit(handle);
        ESP_LOGI(TAG, "All schedules cleared from NVS.");
    }

    nvs_close(handle);
    xSemaphoreGive(s_storage_mutex);
    return err;
}

uint16_t sched_storage_get_active_mask(void)
{
    return s_active_mask;
}

uint8_t sched_storage_get_count(void)
{
    uint8_t count = 0;
    for (int i = 0; i < SCHED_MAX_COUNT; i++) {
        if (s_active_mask & (1 << i)) {
            count++;
        }
    }
    return count;
}

esp_err_t sched_storage_find_by_crc(uint32_t crc32, uint8_t *out_id)
{
    if (!out_id) return ESP_ERR_INVALID_ARG;
    uint16_t mask = s_active_mask;
    for (uint8_t id = 1; id <= SCHED_MAX_COUNT; id++) {
        if (mask & (1 << (id - 1))) {
            schedule_entry_t entry;
            if (sched_storage_load(id, &entry) == ESP_OK) {
                if (entry.crc32 == crc32) {
                    *out_id = id;
                    return ESP_OK;
                }
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}
