/*
 * File    : uart_driver.h
 * Purpose : Public interface for the low-level UART hardware driver.
 * Does    : Defines port, pin, baud constants and exposes init/send/read APIs.
 * Related : uart_driver.c, relay_manager.c
 */

#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* UART0 config for communicating with the Holtek co-processor */
#define UART_PORT      UART_NUM_1
#define UART_TX_PIN    21
#define UART_RX_PIN    20
#define UART_BAUD      9600
#define UART_BUF_SIZE  2048

/* Initializes UART0 at 9600-8N1 on GPIO 21 (TX) / 20 (RX). Called by app_main(). */
esp_err_t uart_init(void);

/* Writes a binary packet to the UART TX ring buffer. Called by relay_manager. */
esp_err_t uart_send_packet(const uint8_t *data, size_t size);

/* Reads bytes from the UART RX ring buffer with a timeout. Called by relay_rx_task. */
int uart_read_packet(uint8_t *buf, size_t max_size, TickType_t timeout);

#endif // UART_DRIVER_H
