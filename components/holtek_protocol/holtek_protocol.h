/*
 * File    : holtek_protocol.h
 * Purpose : UART packet format and codec API for the Holtek BS86D12C co-processor.
 * Does    : Defines packet constants and exposes CRC, encode, and decode functions.
 * Related : holtek_protocol.c, relay_manager.c
 */

#ifndef HOLTEK_PROTOCOL_H
#define HOLTEK_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HOLTEK_HEADER      0xAA   /* Fixed start byte of every packet */
#define HOLTEK_PACKET_LEN  4      /* Header + relay_idx + state + checksum */

/* Prints a readiness banner. Called by app_main() Step 4. */
void holtek_protocol_init(void);

/* Computes modulo-256 checksum over len bytes. Used for both TX and RX validation. */
uint8_t holtek_calculate_crc(const uint8_t *data, size_t len);

/* Fills out_buf with [0xAA, relay_idx, state, crc]. Returns false if buf is NULL or too small. */
bool holtek_encode_packet(uint8_t relay_idx, bool state, uint8_t *out_buf, size_t buf_len);

/* Validates header and CRC, then extracts relay_idx and state. Returns false on any error. */
bool holtek_decode_packet(const uint8_t *in_buf, size_t buf_len, uint8_t *out_relay_idx, bool *out_state);

#endif // HOLTEK_PROTOCOL_H
