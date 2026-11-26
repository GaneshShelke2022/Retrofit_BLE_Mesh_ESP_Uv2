/*
 * File    : relay_hw.h
 * Purpose : Hardware relay abstraction layer interface.
 * Does    : Thin wrapper functions that delegate relay control to relay_manager.
 * Related : relay_hw.c, relay_manager.h, mesh_callbacks.c
 */

#ifndef _RELAY_HW_H_
#define _RELAY_HW_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_COUNT 4

/* Stub — actual initialization is in relay_manager_init(). Retained for API compatibility. */
void relay_hw_init(void);

/* Calls relay_set(index, true). Invoked by generic server callback on ON command. */
void relay_hw_on(uint8_t index);

/* Calls relay_set(index, false). Invoked by generic server callback on OFF command or node reset. */
void relay_hw_off(uint8_t index);

/* Calls relay_toggle(index). Retained for future extension. */
void relay_hw_toggle(uint8_t index);

/* Calls relay_get(index) and returns the current logical state. */
bool relay_hw_get_state(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* _RELAY_HW_H_ */
