/*
 * File    : relay_hw.c
 * Purpose : Hardware relay abstraction layer — delegates all calls to relay_manager.
 * Does    : Thin wrappers for ON, OFF, toggle, and state query.
 * Related : relay_hw.h, relay_manager.h
 */

#include "relay_hw.h"
#include "relay_manager.h"

/* Stub — relay_manager_init() handles actual initialization. */
void relay_hw_init(void)
{
}

/* Delegates relay ON to the state machine coordinator. */
void relay_hw_on(uint8_t index)
{
    relay_set(index, true);
}

/* Delegates relay OFF to the state machine coordinator. */
void relay_hw_off(uint8_t index)
{
    relay_set(index, false);
}

/* Delegates relay toggle to the state machine coordinator. */
void relay_hw_toggle(uint8_t index)
{
    relay_toggle(index);
}

/* Returns the current confirmed state from the relay manager. */
bool relay_hw_get_state(uint8_t index)
{
    return relay_get(index);
}




