/*
 * File    : device_role.h
 * Purpose : Permanent, one-time device-role detection module for ESP32-C3 BLE Mesh node.
 * Does    : Detects whether device is MASTER (DevKitM-1, no Holtek) or NORMAL_NODE
 *           (custom PCB with Holtek co-processor) based on Holtek UART ACK behavior.
 *           Stores detected role in NVS namespace "device_role" key "role".
 */

#ifndef DEVICE_ROLE_H
#define DEVICE_ROLE_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DEVICE_ROLE_UNKNOWN = 0,
    DEVICE_ROLE_MASTER,
    DEVICE_ROLE_NORMAL_NODE
} device_role_t;

/* Initializes and returns device role. Called once at boot.
 * Reads NVS first; if role exists, skips Holtek check.
 * If missing/invalid, performs Holtek ACK detection and saves result to NVS. */
device_role_t device_role_init(void);

/* Returns the current cached device role. */
device_role_t device_role_get(void);

/* Helper functions to check role */
bool device_role_is_master(void);
bool device_role_is_normal_node(void);

/* Erases ONLY the "role" key in "device_role" NVS namespace (optional, manual reset). */
esp_err_t device_role_clear(void);

#endif // DEVICE_ROLE_H
