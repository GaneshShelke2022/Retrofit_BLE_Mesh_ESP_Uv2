/*
 * File    : mesh_init.c
 * Purpose : BLE Host and BLE Mesh stack initialization.
 * Does    : Bluetooth setup, UUID generation, callback registration, and node activation.
 * Related : mesh_init.h, mesh_provisioning.h, mesh_models.h, mesh_callbacks.h, ble_mesh_example_init.h
 */

#include "mesh_init.h"
#include "mesh_provisioning.h"
#include "mesh_models.h"
#include "mesh_callbacks.h"
#include "ble_mesh_example_init.h"
#include "board.h"
#include "esp_log.h"
#include "schedule_mesh_vendor.h"
#include "storage.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"

#define TAG "MESH_INIT"

/*
 * Bluedroid and ESP-BLE-MESH share CONFIG_BT_ALARM_MAX_NUM alarms. This composition
 * (10 model publications, 4 OnOff transitions, 4 proxy clients, 10+10 SAR contexts, ...)
 * takes ~49 before the mesh settings module is initialised last in esp_ble_mesh_init().
 * With 50 the settings timer is never created and nothing is saved to or restored from
 * NVS, so every reboot comes up unprovisioned. Do not lower this below 80.
 */
#if defined(CONFIG_BLE_MESH_SETTINGS) && defined(CONFIG_BT_ALARM_MAX_NUM) && (CONFIG_BT_ALARM_MAX_NUM < 80)
#error "CONFIG_BT_ALARM_MAX_NUM is too small for this BLE Mesh composition: provisioning would not persist across reboots (set >= 80)"
#endif

/*
 * Purpose : Prints detailed breakdown of node composition data (elements and models).
 * Called  : mesh_init() before esp_ble_mesh_init().
 */
void mesh_print_composition_data(void)
{
    esp_ble_mesh_comp_t *comp = mesh_models_get_composition();
    if (!comp) {
        ESP_LOGW(TAG, "[COMPOSITION] No composition data pointer available.");
        return;
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "[COMPOSITION] === BLE MESH COMPOSITION DATA ===");
    ESP_LOGI(TAG, "[COMPOSITION] Company ID (CID)   : 0x%04X", comp->cid);
    ESP_LOGI(TAG, "[COMPOSITION] Element Count      : %d", comp->element_count);

    for (int i = 0; i < comp->element_count; i++) {
        esp_ble_mesh_elem_t *elem = &comp->elements[i];
        ESP_LOGI(TAG, "[COMPOSITION] Element #%d (Location 0x%04X):", i, elem->location);
        ESP_LOGI(TAG, "[COMPOSITION]   - SIG Models (%d):", elem->sig_model_count);
        for (int m = 0; m < elem->sig_model_count; m++) {
            ESP_LOGI(TAG, "[COMPOSITION]       [%d] SIG Model ID: 0x%04X", m, elem->sig_models[m].model_id);
        }
        ESP_LOGI(TAG, "[COMPOSITION]   - Vendor Models (%d):", elem->vnd_model_count);
        for (int v = 0; v < elem->vnd_model_count; v++) {
            ESP_LOGI(TAG, "[COMPOSITION]       [%d] Company: 0x%04X, Model ID: 0x%04X",
                     v, elem->vnd_models[v].vnd.company_id, elem->vnd_models[v].vnd.model_id);
        }
    }
    ESP_LOGI(TAG, "================================================");
}

/*
 * Purpose : Initializes BLE host, generates Device UUID, registers all callbacks,
 *           calls esp_ble_mesh_init(), enables PB-ADV/PB-GATT, and lights the green LED.
 * Called  : app_main() Step 8.
 * Returns : ESP_OK on success, or the first error code encountered.
 */
esp_err_t mesh_init(void)
{
    esp_err_t err = bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] BLE Mesh initialization failed - bluetooth_init error: %d", err);
        return err;
    }

    mesh_provisioning_init();
    mesh_models_init_state();

    /* Register callback functions for stack events and model handlers */
    esp_ble_mesh_register_prov_callback(mesh_callbacks_provisioning_cb);
    esp_ble_mesh_register_config_server_callback(mesh_callbacks_config_server_cb);
    esp_ble_mesh_register_health_server_callback(mesh_callbacks_health_server_cb);
    esp_ble_mesh_register_generic_server_callback(mesh_callbacks_generic_server_cb);
#if !defined(CONFIG_VENDOR_MODEL_ENABLE) || CONFIG_VENDOR_MODEL_ENABLE
    esp_ble_mesh_register_custom_model_callback(schedule_mesh_vendor_cb);
#endif

    mesh_print_composition_data();

    esp_ble_mesh_prov_t *prov = mesh_provisioning_get_prov();
    esp_ble_mesh_comp_t *comp = mesh_models_get_composition();

    /* Must run before esp_ble_mesh_init(), which loads "mesh_core" */
    storage_mesh_settings_migrate_once();

    err = esp_ble_mesh_init(prov, comp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] BLE Mesh initialization failed (err %d)", err);
        return err;
    }   
    ESP_LOGI(TAG, "[MESH] esp_ble_mesh_init succeeded (err %d)", err);

    bool provisioned = esp_ble_mesh_node_is_provisioned();
    ESP_LOGI(TAG, "[MESH] Provisioning Status Check: %s", provisioned ? "PROVISIONED" : "UNPROVISIONED");
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    ESP_LOGI(TAG, "[MESH] GATT Proxy Server feature enabled");
#endif

    if (provisioned) {
        ESP_LOGI(TAG, "[MESH] Device is PROVISIONED - restored from NVS, primary unicast 0x%04x",
                 esp_ble_mesh_get_primary_element_address());
        board_led_operation(LED_G, LED_OFF);
    } else {
        if (storage_mesh_key_present("mesh/net")) {
            ESP_LOGE(TAG, "[MESH] Provisioning data is in NVS but was NOT restored - check BLE_MESH logs above");
        }
        ESP_LOGI(TAG, "[MESH] Device is UNPROVISIONED");
        ESP_LOGI(TAG, "[MESH] Enabling PB-ADV and PB-GATT unprovisioned advertising bearers...");

        /* Enable PB-ADV and PB-GATT unprovisioned advertising */
        err = esp_ble_mesh_node_prov_enable((esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[MESH] Failed to enable mesh node prov advertising: %s (err %d)", esp_err_to_name(err), err);
            return err;
        }
        ESP_LOGI(TAG, "[MESH] Unprovisioned advertising started successfully");

        /* Green LED ON = node is advertising as unprovisioned */
        board_led_operation(LED_G, LED_ON);
    }

    ESP_LOGI(TAG, "[MESH] BLE Mesh Node initialized successfully");
    return ESP_OK;
}

