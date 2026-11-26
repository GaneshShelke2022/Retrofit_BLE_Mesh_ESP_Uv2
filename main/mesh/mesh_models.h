/*
 * File    : mesh_models.h
 * Purpose : Public interface for BLE Mesh composition data and server model configuration.
 * Does    : Exposes composition retrieval, model state init, and element index lookup APIs.
 * Related : mesh_models.c, mesh_init.c, relay_manager.c
 */

#ifndef _MESH_MODELS_H_
#define _MESH_MODELS_H_

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CID_ESP 0x02E5

/* Returns the static composition structure for passing to esp_ble_mesh_init(). */
esp_ble_mesh_comp_t *mesh_models_get_composition(void);

/* Reads power-up and last relay states from NVS and sets initial BLE Mesh model states. */
void mesh_models_init_state(void);

/* Returns the 0-based element index of the given model pointer, or -1 if not found. */
int mesh_models_get_element_index(esp_ble_mesh_model_t *model);

#ifdef __cplusplus
}
#endif

#endif /* _MESH_MODELS_H_ */
