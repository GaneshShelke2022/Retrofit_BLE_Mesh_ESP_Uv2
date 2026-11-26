/*
 * File    : holtek_protocol.c
 * Purpose : UART packet encoder, decoder, and CRC for the Holtek BS86D12C co-processor.
 * Does    : Stateless checksum computation, 4-byte packet formatting, and parser validation.
 * Related : holtek_protocol.h
 */

#include "holtek_protocol.h"
#include <stdio.h>

/*
 * Purpose : Confirms component is linked by printing a readiness banner.
 * Called  : app_main() Step 4.
 */
void holtek_protocol_init(void)
{
    printf("Holtek Protocol Ready\n");
}

/*
 * Purpose : Computes the 8-bit modulo-256 sum of data[0..len-1].
 * Called  : holtek_encode_packet() and holtek_decode_packet().
 * Returns : Sum of all bytes masked to 8 bits.
 */
uint8_t holtek_calculate_crc(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(sum & 0xFF);
}

/*
 * Purpose : Builds a 4-byte command packet: [0xAA, relay_idx, state, crc].
 * Called  : relay_manager before every UART command or retry.
 * Returns : true on success, false if out_buf is NULL or too small.
 */
bool holtek_encode_packet(uint8_t relay_idx, bool state, uint8_t *out_buf, size_t buf_len)
{
    if (out_buf == NULL || buf_len < HOLTEK_PACKET_LEN) {
        return false;
    }

    out_buf[0] = HOLTEK_HEADER;
    out_buf[1] = relay_idx;
    out_buf[2] = state ? 0x01 : 0x00;
    out_buf[3] = holtek_calculate_crc(out_buf, HOLTEK_PACKET_LEN - 1);

    return true;
}

/*
 * Purpose : Validates header and checksum, then extracts relay_idx and state.
 * Called  : relay_rx_task after a complete 4-byte frame is assembled.
 * Returns : true if packet is valid, false on header mismatch or CRC error.
 */
bool holtek_decode_packet(const uint8_t *in_buf, size_t buf_len, uint8_t *out_relay_idx, bool *out_state)
{
    if (in_buf == NULL || buf_len < HOLTEK_PACKET_LEN) {
        return false;
    }

    if (in_buf[0] != HOLTEK_HEADER) {
        return false;
    }

    uint8_t calculated_crc = holtek_calculate_crc(in_buf, HOLTEK_PACKET_LEN - 1);
    if (in_buf[3] != calculated_crc) {
        return false;
    }

    if (out_relay_idx != NULL) {
        *out_relay_idx = in_buf[1];
    }
    if (out_state != NULL) {
        *out_state = (in_buf[2] == 0x01);
    }

    return true;
}
