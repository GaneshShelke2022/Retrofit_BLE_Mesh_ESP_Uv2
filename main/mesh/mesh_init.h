/*
 * File    : mesh_init.h
 * Purpose : Public interface for BLE Host and BLE Mesh stack initialization.
 * Does    : Exposes the single init function called from app_main().
 * Related : mesh_init.c, main.c
 */

#ifndef _MESH_INIT_H_
#define _MESH_INIT_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes Bluetooth, registers callbacks, inits mesh stack, and starts advertising. */
esp_err_t mesh_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _MESH_INIT_H_ */
