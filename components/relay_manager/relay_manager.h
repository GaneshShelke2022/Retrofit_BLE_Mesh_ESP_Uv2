/*
 * File    : relay_manager.h
 * Purpose : Authoritative relay state machine and co-processor coordinator interface.
 * Does    : Exposes init, set, get, toggle, sync, boot handler, and ACK handler APIs.
 * Related : relay_manager.c, main.c, relay_hw.c, holtek_protocol.h, storage.h, mesh_models.h
 */

#ifndef RELAY_MANAGER_H
#define RELAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/* Loads NVS states, sends startup frames to Holtek, and spawns the UART RX task. */
void relay_manager_init(void);

/* Sends a relay ON/OFF command to Holtek and starts the ACK timeout timer. */
void relay_set(uint8_t index, bool state);

/* Returns the current confirmed state (or optimistic target if still syncing). */
bool relay_get(uint8_t index);

/* Reads current state and calls relay_set() with the opposite value. */
void relay_toggle(uint8_t index);

/* Reads NVS and force-syncs all 4 relays to Holtek in RECOVERING state. */
void sync_all_relays(void);

/* Force-syncs a single relay to Holtek in RECOVERING state. */
void sync_single_relay(uint8_t index, bool state);

/* Prints the state machine state and target for the given relay index (debug). */
void request_state(uint8_t index);

/* Thin wrapper that calls sync_all_relays(). Explicit recovery API. */
void restore_from_nvs(void);

/* Handles the 0xFF BOOT packet from Holtek: loads NVS and re-syncs all relays. */
void handle_boot(void);

/* Handles ACK/switch-event packet: confirms state, saves NVS, and publishes to Mesh. */
void handle_ack(uint8_t index, bool state);

#endif // RELAY_MANAGER_H
