/*
 * File    : mesh_callbacks.h
 * Purpose : Public interface for BLE Mesh stack callback functions.
 * Does    : Declares provisioning, configuration, health, and generic server event handlers.
 * Related : mesh_callbacks.c, mesh_init.c
 */

#ifndef _MESH_CALLBACKS_H_
#define _MESH_CALLBACKS_H_

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "relay_manager.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Handles provisioning lifecycle events: link open/close, provision complete, node reset. */
void mesh_callbacks_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                     esp_ble_mesh_prov_cb_param_t *param);

/* Handles Configuration Server events: app key binding and model subscription logging. */
void mesh_callbacks_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                      esp_ble_mesh_cfg_server_cb_param_t *param);

/* Handles Health Server events: attention timer and fault test logging. */
void mesh_callbacks_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                      esp_ble_mesh_health_server_cb_param_t *param);

/* Handles Generic OnOff Set and OnPowerUp Set commands; dispatches to relay hardware. */
void mesh_callbacks_generic_server_cb(esp_ble_mesh_generic_server_cb_event_t event,
                                       esp_ble_mesh_generic_server_cb_param_t *param);

#ifdef __cplusplus
}
#endif

#endif /* _MESH_CALLBACKS_H_ */
