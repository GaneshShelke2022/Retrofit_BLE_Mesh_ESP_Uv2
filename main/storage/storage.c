/*
 * File    : storage.c
 * Purpose : Asynchronous NVS (Non-Volatile Storage) implementation.
 * Does    : Queue-based async NVS writes and direct NVS reads for relay and power-up states.
 * Related : storage.h, main.c, relay_manager.c
 */

#include "storage.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_ble_mesh_networking_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>

#define TAG           "STORAGE"
#define NVS_NAMESPACE "relay_storage"

typedef enum {
    STORAGE_TYPE_RELAY,
    STORAGE_TYPE_POWER_UP
} storage_item_type_t;

typedef struct {
    storage_item_type_t type;
    uint8_t index;
    uint8_t value;
} storage_queue_item_t;

static QueueHandle_t storage_queue = NULL;
static TaskHandle_t storage_task_handle = NULL;

static esp_err_t save_relay_state_direct(uint8_t index, bool state);
static esp_err_t save_power_up_state_direct(uint8_t index, uint8_t power_up);

/*
 * Purpose : Processes queued NVS writes with a read-before-write check to skip redundant flash updates.
 * Called  : Spawned by storage_init(); runs continuously in the background at Priority 5.
 */
static void storage_worker_task(void *pvParameters)
{
    storage_queue_item_t item;
    ESP_LOGI(TAG, "Storage worker task started");

    while (1) {
        if (xQueueReceive(storage_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.type == STORAGE_TYPE_RELAY) {
                bool current_state = false;
                esp_err_t err = load_relay_state(item.index, &current_state);
                bool new_state = (item.value != 0);

                if (err != ESP_OK || current_state != new_state) {
                    save_relay_state_direct(item.index, new_state);
                } else {
                    ESP_LOGD(TAG, "Relay %d state is already %s, skipping duplicate write",
                             item.index + 1, new_state ? "ON" : "OFF");
                }
            } else if (item.type == STORAGE_TYPE_POWER_UP) {
                uint8_t current_power_up = 2;
                esp_err_t err = load_power_up_state(item.index, &current_power_up);

                if (err != ESP_OK || current_power_up != item.value) {
                    save_power_up_state_direct(item.index, item.value);
                } else {
                    ESP_LOGD(TAG, "Relay %d power-up state is already %d, skipping duplicate write",
                             item.index + 1, item.value);
                }
            }
        }
    }
}

/*
 * Purpose : Initializes the NVS flash partition, creates the write queue, and spawns the worker task.
 * Called  : app_main() Step 1.
 * Returns : ESP_OK on success, ESP error code on failure.
 */
esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
        return err;
    }

    storage_queue = xQueueCreate(16, sizeof(storage_queue_item_t));
    if (storage_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create storage queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t xReturned = xTaskCreate(storage_worker_task, "storage_worker", 3072, NULL, 5, &storage_task_handle);
    if (xReturned != pdPASS) {
        ESP_LOGE(TAG, "Failed to create storage worker task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*
 * Purpose : Enqueues a relay state write (non-blocking) to the storage worker task.
 * Called  : handle_ack() when a state change is confirmed by Holtek ACK.
 * Returns : ESP_OK if enqueued, ESP_FAIL if queue is full.
 */
esp_err_t save_relay_state(uint8_t index, bool state)
{
    if (storage_queue == NULL) {
        ESP_LOGE(TAG, "Storage queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    storage_queue_item_t item = {
        .type = STORAGE_TYPE_RELAY,
        .index = index,
        .value = state ? 1 : 0
    };

    if (xQueueSend(storage_queue, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Storage queue full, state change for relay %d dropped!", index + 1);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*
 * Purpose : Opens NVS, writes key "relay_state_N", commits and closes (blocking).
 * Called  : storage_worker_task after dequeue.
 */
static esp_err_t save_relay_state_direct(uint8_t index, bool state)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    char key[16];
    snprintf(key, sizeof(key), "relay_state_%u", index);

    uint8_t value = state ? 1 : 0;
    err = nvs_set_u8(my_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save relay %d state to NVS: %s", index + 1, esp_err_to_name(err));
    } else {
        err = nvs_commit(my_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit relay %d state to NVS: %s", index + 1, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Saved relay %d state to NVS: %s", index + 1, state ? "ON" : "OFF");
        }
    }

    nvs_close(my_handle);
    return err;
}

/*
 * Purpose : Opens NVS, reads key "relay_state_N", and closes (blocking).
 *           Defaults *state to false if key is not found.
 * Called  : relay_manager_init(), handle_boot(), and storage_worker_task (dedup check).
 * Returns : ESP_OK, ESP_ERR_NVS_NOT_FOUND, or other NVS error.
 */
esp_err_t load_relay_state(uint8_t index, bool *state)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            *state = false;
            return ESP_ERR_NVS_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    char key[16];
    snprintf(key, sizeof(key), "relay_state_%u", index);

    uint8_t value = 0;
    err = nvs_get_u8(my_handle, key, &value);
    if (err == ESP_OK) {
        *state = (value != 0);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *state = false;
    } else {
        ESP_LOGE(TAG, "Failed to get relay %d state from NVS: %s", index + 1, esp_err_to_name(err));
    }

    nvs_close(my_handle);
    return err;
}

/*
 * Purpose : Enqueues a GenOnPowerUp behavior write (non-blocking) to the storage worker task.
 * Called  : generic server callback on GEN_ONPOWERUP_SET.
 * Returns : ESP_OK if enqueued, ESP_FAIL if queue is full.
 */
esp_err_t save_power_up_state(uint8_t index, uint8_t power_up)
{
    if (storage_queue == NULL) {
        ESP_LOGE(TAG, "Storage queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    storage_queue_item_t item = {
        .type = STORAGE_TYPE_POWER_UP,
        .index = index,
        .value = power_up
    };

    if (xQueueSend(storage_queue, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Storage queue full, power-up change for relay %d dropped!", index + 1);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*
 * Purpose : Opens NVS, writes key "power_up_N", commits and closes (blocking).
 * Called  : storage_worker_task after dequeue.
 */
static esp_err_t save_power_up_state_direct(uint8_t index, uint8_t power_up)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    char key[16];
    snprintf(key, sizeof(key), "power_up_%u", index);

    err = nvs_set_u8(my_handle, key, power_up);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save power-up %d state to NVS: %s", index + 1, esp_err_to_name(err));
    } else {
        err = nvs_commit(my_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit power-up %d state to NVS: %s", index + 1, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Saved power-up %d state to NVS: %d", index + 1, power_up);
        }
    }

    nvs_close(my_handle);
    return err;
}

/*
 * Purpose : Opens NVS, reads key "power_up_N", and closes (blocking).
 *           Defaults *power_up to 2 (Restore) if key is not found.
 * Called  : relay_manager_init() and mesh_models_init_state() during startup.
 * Returns : ESP_OK, ESP_ERR_NVS_NOT_FOUND, or other NVS error.
 */
esp_err_t load_power_up_state(uint8_t index, uint8_t *power_up)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            *power_up = 2; /* Default: Restore last state */
            return ESP_ERR_NVS_NOT_FOUND;
        }
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    char key[16];
    snprintf(key, sizeof(key), "power_up_%u", index);

    err = nvs_get_u8(my_handle, key, power_up);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Loaded power-up %d state from NVS: %d", index + 1, *power_up);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *power_up = 2; /* Default: Restore last state */
    } else {
        ESP_LOGE(TAG, "Failed to get power-up %d state from NVS: %s", index + 1, esp_err_to_name(err));
    }

    nvs_close(my_handle);
    return err;
}

/*
 * Purpose : Erases a specific NVS namespace by opening it, deleting all keys, committing, and closing.
 */
static void erase_nvs_namespace(const char *ns_name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns_name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            nvs_commit(handle);
            ESP_LOGI(TAG, "[MESH] NVS namespace '%s' erased successfully", ns_name);
        } else {
            ESP_LOGE(TAG, "[MESH] Failed to erase keys in NVS namespace '%s': %s", ns_name, esp_err_to_name(err));
        }
        nvs_close(handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "[MESH] NVS namespace '%s' not found (clean)", ns_name);
    } else {
        ESP_LOGE(TAG, "[MESH] Error (%s) opening NVS namespace '%s'", esp_err_to_name(err), ns_name);
    }
}

/*
 * Purpose : Erases all keys in the relay_storage NVS namespace without rebooting.
 * Called  : Manual recovery / factory reset without full restart.
 * Returns : ESP_OK on success.
 */
esp_err_t factory_reset_storage(void)
{
    erase_nvs_namespace(NVS_NAMESPACE);
    return ESP_OK;
}

/*
 * Purpose : Deferred task to perform complete stack-level and app-level factory reset:
 *           1. Calls esp_ble_mesh_node_local_reset() to reset BLE Mesh stack in-memory state
 *              and erase stack-managed keys (NetKeys, AppKeys, RPL, Seq, Role).
 *           2. Explicitly erases BLE Mesh stack NVS namespaces ("mesh_core", "mesh_user_id")
 *              to wipe residual keys like mesh/iv that stack reset omits.
 *           3. Explicitly erases app namespace "relay_storage" and BLE host namespaces.
 *           4. Deinits, erases, and re-inits the entire NVS partition for 100% clean state.
 *           5. Restarts system after short delay to boot into clean unprovisioned state.
 */
static void deferred_factory_reset_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[MESH] Executing local BLE Mesh stack reset...");
    esp_err_t err = esp_ble_mesh_node_local_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] esp_ble_mesh_node_local_reset failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[MESH] BLE Mesh local node reset completed");
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "[MESH] Clearing application and BLE stack NVS namespaces...");
    erase_nvs_namespace(NVS_NAMESPACE);     /* "relay_storage" */
    erase_nvs_namespace("mesh_core");        /* ESP-BLE-MESH core settings */
    erase_nvs_namespace("mesh_user_id");     /* ESP-BLE-MESH user ID settings */
    erase_nvs_namespace("nimble_val");       /* NimBLE host storage */
    erase_nvs_namespace("nimble_ble");       /* NimBLE host storage */
    erase_nvs_namespace("bt_config.conf");   /* Bluedroid host storage */

    ESP_LOGI(TAG, "[MESH] Performing complete NVS partition format...");
    nvs_flash_deinit();
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] nvs_flash_erase failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[MESH] NVS partition erased cleanly");
    }

    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] nvs_flash_init after erase failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[MESH] NVS partition re-initialized successfully");
    }

    ESP_LOGI(TAG, "[MESH] Factory reset complete. Restarting system in unprovisioned state...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/*
 * Purpose : Initiates complete factory reset and system restart.
 * Called  : Provisioning callback on ESP_BLE_MESH_NODE_PROV_RESET_EVT.
 */
static bool s_reset_in_progress = false;

void storage_factory_reset_and_restart(void)
{
    if (s_reset_in_progress) {
        ESP_LOGW(TAG, "[MESH] Factory reset already in progress, ignoring duplicate request");
        return;
    }
    s_reset_in_progress = true;

    ESP_LOGI(TAG, "[MESH] Provisioning reset requested. Scheduling complete factory reset...");
    BaseType_t ret = xTaskCreate(deferred_factory_reset_task, "reset_task", 3072, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "[MESH] Failed to create reset task, performing immediate fallback reset");
        esp_ble_mesh_node_local_reset();
        erase_nvs_namespace(NVS_NAMESPACE);
        erase_nvs_namespace("mesh_core");
        erase_nvs_namespace("mesh_user_id");
        nvs_flash_deinit();
        nvs_flash_erase();
        nvs_flash_init();
        esp_restart();
    }
}

#define MESH_CORE_NAMESPACE   "mesh_core"    /* ESP-BLE-MESH stack settings namespace */
#define MESH_FIX_NAMESPACE    "lumflux_sys"
#define MESH_FIX_KEY          "mesh_pfix"
#define MESH_FIX_VERSION      1

/*
 * Purpose : One-time clean-up for the reboot-persistence fix (CONFIG_BT_ALARM_MAX_NUM).
 *           Earlier firmware could not open the "mesh_core" namespace, so anything in it
 *           was written by an older build and holds an outdated identity (unicast address,
 *           keys). It is erased once so the stack never restores a stale address; nothing
 *           else in NVS is touched. Later boots skip this (marker in "lumflux_sys").
 * Called  : mesh_init(), before esp_ble_mesh_init().
 */
void storage_mesh_settings_migrate_once(void)
{
    nvs_handle_t fix_h;
    esp_err_t err = nvs_open(MESH_FIX_NAMESPACE, NVS_READWRITE, &fix_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] Persistence migration: cannot open '%s': %s", MESH_FIX_NAMESPACE, esp_err_to_name(err));
        return;
    }

    uint8_t version = 0;
    if (nvs_get_u8(fix_h, MESH_FIX_KEY, &version) == ESP_OK && version >= MESH_FIX_VERSION) {
        nvs_close(fix_h);
        return;
    }

    ESP_LOGW(TAG, "[MESH] Persistence migration v%d: clearing stale '%s' once - provision this board one more time",
             MESH_FIX_VERSION, MESH_CORE_NAMESPACE);

    nvs_handle_t mesh_h;
    err = nvs_open(MESH_CORE_NAMESPACE, NVS_READWRITE, &mesh_h);
    if (err == ESP_OK) {
        err = nvs_erase_all(mesh_h);
        if (err == ESP_OK) {
            err = nvs_commit(mesh_h);
        }
        nvs_close(mesh_h);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(fix_h, MESH_FIX_KEY, MESH_FIX_VERSION);
        if (err == ESP_OK) {
            err = nvs_commit(fix_h);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] Persistence migration failed (%s); it will be retried next boot", esp_err_to_name(err));
    }
    nvs_close(fix_h);
}

/*
 * Purpose : Read-only check that the BLE Mesh stack has written a settings record
 *           (e.g. "mesh/role", "mesh/net", "mesh/netkey") to NVS. Diagnostics only.
 * Returns : true if the record exists.
 */
bool storage_mesh_key_present(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(MESH_CORE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    nvs_close(h);
    return (err == ESP_OK && len > 0);
}

