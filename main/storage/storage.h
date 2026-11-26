/*
 * File    : storage.h
 * Purpose : Asynchronous NVS (Non-Volatile Storage) subsystem interface.
 * Does    : Exposes APIs for reading and asynchronously saving relay states and OnPowerUp behaviors.
 * Related : storage.c, main.c, relay_manager.c, mesh_callbacks.c
 */

#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes NVS flash, creates the write queue (depth 16), and starts the worker task. */
esp_err_t storage_init(void);

/* Enqueues a relay ON/OFF state write to NVS (non-blocking). Called by handle_ack(). */
esp_err_t save_relay_state(uint8_t index, bool state);

/* Reads relay state from NVS synchronously. Defaults to false (OFF) if key not found. */
esp_err_t load_relay_state(uint8_t index, bool *state);

/* Enqueues a GenOnPowerUp behavior write to NVS (non-blocking). Called by generic server callback. */
esp_err_t save_power_up_state(uint8_t index, uint8_t power_up);

/* Reads GenOnPowerUp behavior from NVS synchronously. Defaults to 2 (Restore) if key not found. */
esp_err_t load_power_up_state(uint8_t index, uint8_t *power_up);

/* Erases the relay_storage NVS namespace without rebooting (targeted clean-up). */
esp_err_t factory_reset_storage(void);

/* Erases the entire NVS partition (including BLE Mesh credentials) and restarts the chip. */
void storage_factory_reset_and_restart(void);

/* One-time erase of the stale BLE Mesh "mesh_core" namespace after the persistence fix. Call before esp_ble_mesh_init(). */
void storage_mesh_settings_migrate_once(void);

/* Diagnostics: true if the BLE Mesh stack has stored the given settings record (e.g. "mesh/net") in NVS. */
bool storage_mesh_key_present(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* _STORAGE_H_ */
