/*
 * File    : mesh_provisioning.h
 * Purpose : BLE Mesh provisioning configuration interface.
 * Does    : Exposes UUID generation and OOB provisioning structure retrieval APIs.
 * Related : mesh_provisioning.c, mesh_init.c
 */

#ifndef _MESH_PROVISIONING_H_
#define _MESH_PROVISIONING_H_

#include "esp_ble_mesh_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generates a unique RFC 4122 v1 Device UUID from the chip's BT MAC. Called by mesh_init(). */
void mesh_provisioning_init(void);

/* Returns the static provisioning configuration structure (UUID + OOB settings). */
esp_ble_mesh_prov_t *mesh_provisioning_get_prov(void);

#ifdef __cplusplus
}
#endif

#endif /* _MESH_PROVISIONING_H_ */
