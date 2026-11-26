/*
 * File    : mesh_callbacks.c
 * Purpose : BLE Mesh stack event callback implementations.
 * Does    : Handles provisioning, configuration, health, and generic server model events.
 * Related : mesh_callbacks.h, board.h, relay_hw.h, storage.h, mesh_models.h
 */

#include "mesh_callbacks.h"
#include "esp_log.h"
#include "board.h"
#include "relay_hw.h"
#include "storage.h"
#include "mesh_models.h"
#include "esp_ble_mesh_networking_api.h"
#include <inttypes.h>

#define TAG "MESH_CALLBACKS"

/*
 * Purpose : Logs assigned unicast address and turns off the Green LED on successful provisioning.
 * Called  : mesh_callbacks_provisioning_cb() on ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT.
 */
static void prov_complete(uint16_t net_idx, uint16_t addr, uint8_t flags, uint32_t iv_index)
{
    ESP_LOGI(TAG, "net_idx: 0x%04x, addr: 0x%04x", net_idx, addr);
    ESP_LOGI(TAG, "flags: 0x%02x, iv_index: 0x%08" PRIx32, flags, iv_index);
    board_led_operation(LED_G, LED_OFF);

    /* The stack writes "mesh/net" (unicast + DevKey) before this event, also when restoring at boot */
    if (storage_mesh_key_present("mesh/net")) {
        ESP_LOGI(TAG, "[MESH] Provisioning data persisted in NVS (survives power cycles)");
    } else {
        ESP_LOGE(TAG, "[MESH] Provisioning data NOT in NVS - node will be unprovisioned after reboot "
                      "(check CONFIG_BT_ALARM_MAX_NUM / BLE_MESH settings errors)");
    }
}

/*
 * Purpose : Handles provisioning lifecycle events: link open/close, provision complete, node reset.
 * Called  : BLE Mesh stack during node enrollment, de-enrollment, and link management.
 */
void mesh_callbacks_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                    esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "[MESH] ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "[MESH] ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT, err_code %d", param->node_prov_enable_comp.err_code);
        /* Enabling provisioning stores the node role first; if it is missing, mesh NVS settings are not working */
        if (param->node_prov_enable_comp.err_code == 0 && !storage_mesh_key_present("mesh/role")) {
            ESP_LOGE(TAG, "[MESH] BLE Mesh settings are not being written to NVS - provisioning will not survive reboot");
        }
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "[MESH] ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT, bearer %s",
             param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "[MESH] ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT, bearer %s",
             param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "[MESH] ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT");
        prov_complete(param->node_prov_complete.net_idx, param->node_prov_complete.addr,
            param->node_prov_complete.flags, param->node_prov_complete.iv_index);
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        /* Node reset from network: turn all relays OFF and clear NVS storage */
        ESP_LOGI(TAG, "[MESH] Provisioning reset requested");
        ESP_LOGI(TAG, "[MESH] Clearing persistent mesh state and scheduling restart...");
        for (int i = 0; i < RELAY_COUNT; i++) {
            relay_hw_off(i);
        }
        storage_factory_reset_and_restart();
        break;
    case ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT:
        ESP_LOGI(TAG, "[MESH] ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT, err_code %d", param->node_set_unprov_dev_name_comp.err_code);
        break;
    default:
        break;
    }
}

/*
 * Purpose : Logs app key bindings and model subscription commands sent by the Provisioner.
 * Called  : BLE Mesh stack when Configuration Client commands are received.
 */
void mesh_callbacks_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                     esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        switch (param->ctx.recv_op) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD");
            ESP_LOGI(TAG, "net_idx 0x%04x, app_idx 0x%04x",
                param->value.state_change.appkey_add.net_idx,
                param->value.state_change.appkey_add.app_idx);
            ESP_LOG_BUFFER_HEX("AppKey", param->value.state_change.appkey_add.app_key, 16);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND");
            ESP_LOGI(TAG, "elem_addr 0x%04x, app_idx 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_app_bind.element_addr,
                param->value.state_change.mod_app_bind.app_idx,
                param->value.state_change.mod_app_bind.company_id,
                param->value.state_change.mod_app_bind.model_id);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD");
            ESP_LOGI(TAG, "elem_addr 0x%04x, sub_addr 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_sub_add.element_addr,
                param->value.state_change.mod_sub_add.sub_addr,
                param->value.state_change.mod_sub_add.company_id,
                param->value.state_change.mod_sub_add.model_id);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_SUB_DELETE");
            ESP_LOGI(TAG, "elem_addr 0x%04x, sub_addr 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_sub_delete.element_addr,
                param->value.state_change.mod_sub_delete.sub_addr,
                param->value.state_change.mod_sub_delete.company_id,
                param->value.state_change.mod_sub_delete.model_id);
            break;
        default:
            break;
        }
    }
}

/*
 * Purpose : Logs Health Server events: attention timer, fault tests, and fault clears.
 * Called  : BLE Mesh stack on health server updates.
 */
void mesh_callbacks_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                     esp_ble_mesh_health_server_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_HEALTH_SERVER_FAULT_UPDATE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_HEALTH_SERVER_FAULT_UPDATE_COMP_EVT, err_code %d",
                 param->fault_update_comp.error_code);
        break;
    case ESP_BLE_MESH_HEALTH_SERVER_FAULT_CLEAR_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_HEALTH_SERVER_FAULT_CLEAR_EVT, company_id 0x%04x",
                 param->fault_clear.company_id);
        break;
    case ESP_BLE_MESH_HEALTH_SERVER_FAULT_TEST_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_HEALTH_SERVER_FAULT_TEST_EVT, test_id 0x%02x, company_id 0x%04x",
                 param->fault_test.test_id, param->fault_test.company_id);
        break;
    case ESP_BLE_MESH_HEALTH_SERVER_ATTENTION_ON_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_HEALTH_SERVER_ATTENTION_ON_EVT, time %d",
                 param->attention_on.time);
        break;
    case ESP_BLE_MESH_HEALTH_SERVER_ATTENTION_OFF_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_HEALTH_SERVER_ATTENTION_OFF_EVT");
        break;
    default:
        break;
    }
}

/*
 * Purpose : Entry point for incoming relay control messages from the BLE Mesh network.
 *           Dispatches GenOnOff Set to relay hardware and saves OnPowerUp changes to NVS.
 * Called  : BLE Mesh stack when Generic Client messages are received.
 */
void mesh_callbacks_generic_server_cb(esp_ble_mesh_generic_server_cb_event_t event,
                                      esp_ble_mesh_generic_server_cb_param_t *param)
{
    int index = mesh_models_get_element_index(param->model);
    if (index < 0) {
        ESP_LOGE(TAG, "Failed to find element index for model");
        return;
    }

    ESP_LOGD(TAG, "Generic Server event 0x%02x, element index %d, src 0x%04x, dst 0x%04x",
             event, index, param->ctx.addr, param->ctx.recv_dst);

    switch (event) {
    case ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT:
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
            uint8_t onoff = param->value.state_change.onoff_set.onoff;

            if (onoff) {
                relay_hw_on(index);
            } else {
                relay_hw_off(index);
            }

            /* Additive publish: broadcast GenericOnOffStatus to 0xFFFF on state change */
            if (param->model->pub != NULL) {
                if (param->model->pub->publish_addr == ESP_BLE_MESH_ADDR_UNASSIGNED || param->model->pub->publish_addr == 0x0001) {
                    param->model->pub->publish_addr = 0xFFFF;
                }
                if (param->model->pub->app_idx == ESP_BLE_MESH_KEY_UNUSED) {
                    param->model->pub->app_idx = 0;
                }
                if (param->model->pub->ttl == 0) {
                    param->model->pub->ttl = 7;
                }
            }
            esp_err_t pub_err = esp_ble_mesh_model_publish(param->model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS,
                                                           sizeof(onoff), &onoff, ROLE_NODE);
            if (pub_err != ESP_OK) {
                ESP_LOGW(TAG, "esp_ble_mesh_model_publish for Generic OnOff element %d returned: 0x%x", index + 1, pub_err);
            }
        } else if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONPOWERUP_SET ||
                   param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONPOWERUP_SET_UNACK) {
            uint8_t onpowerup = param->value.state_change.onpowerup_set.onpowerup;
            ESP_LOGI(TAG, "Generic OnPowerUp on element %d set to %d", index, onpowerup);

            save_power_up_state(index, onpowerup);

            esp_ble_mesh_model_publish(param->model, ESP_BLE_MESH_MODEL_OP_GEN_ONPOWERUP_STATUS,
                                       sizeof(onpowerup), &onpowerup, ROLE_NODE);
        }
        break;

    case ESP_BLE_MESH_GENERIC_SERVER_RECV_GET_MSG_EVT:
        if (param->model->user_data != NULL) {
            esp_ble_mesh_gen_onoff_srv_t *srv = (esp_ble_mesh_gen_onoff_srv_t *)param->model->user_data;
            bool current_state = relay_hw_get_state(index);
            srv->state.onoff = current_state ? 1 : 0;
            esp_ble_mesh_server_state_value_t val = {0};
            val.gen_onoff.onoff = current_state ? 1 : 0;
            esp_ble_mesh_server_model_update_state(param->model, ESP_BLE_MESH_GENERIC_ONOFF_STATE, &val);
            ESP_LOGI(TAG, "Generic Server GET for element %d: state %d", index, current_state ? 1 : 0);
        }
        break;

    case ESP_BLE_MESH_GENERIC_SERVER_RECV_SET_MSG_EVT:
        break;

    default:
        ESP_LOGW(TAG, "Unhandled Generic Server event 0x%02x", event);
        break;
    }
}
