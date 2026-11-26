/*
 * File    : uart_driver.c
 * Purpose : Low-level UART hardware driver for ESP32-C3.
 * Does    : Driver install, 9600-8N1 config, TX send, RX byte read with hex logging.
 * Related : uart_driver.h
 */

#include <stdio.h>
#include "uart_driver.h"
#include "driver/uart.h"

/*
 * Purpose : Installs the UART driver and configures UART0 at 9600-8N1 on GPIO 21/20.
 * Called  : app_main() Step 3.
 * Returns : ESP_OK on success, ESP error code on failure.
 */
esp_err_t uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err;

    err = uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0);
    printf("uart_driver_install status: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        printf("====================================\n");
        printf("UART0 INITIALIZATION FAILED\n");
        printf("====================================\n");
        return err;
    }

    err = uart_param_config(UART_PORT, &uart_config);
    printf("uart_param_config status: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    err = uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    printf("uart_set_pin status: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    printf("====================================\n");
    printf("UART0 INITIALIZED\n");
    printf("TX : GPIO%d\n", UART_TX_PIN);
    printf("RX : GPIO%d\n", UART_RX_PIN);
    printf("Baud : %d\n", UART_BAUD);
    printf("====================================\n");

    return ESP_OK;
}

/*
 * Purpose : Queues a binary packet to the UART TX ring buffer and logs it in hex.
 * Called  : relay_manager on every command dispatch or retry.
 * Returns : ESP_OK if bytes were accepted, ESP_FAIL otherwise.
 */
esp_err_t uart_send_packet(const uint8_t *data, size_t size)
{
    int bytes_written = uart_write_bytes(UART_PORT, (const char *)data, size);
    esp_err_t err = (bytes_written >= 0) ? ESP_OK : ESP_FAIL;
    printf("uart_write_bytes status: %s\n", esp_err_to_name(err));
    if (err == ESP_OK) {
        printf("UART0 TX ->");
        for (size_t i = 0; i < size; i++) {
            printf(" %02X", data[i]);
        }
        printf("\n");
    }
    return err;
}

/*
 * Purpose : Reads bytes from the UART RX buffer and logs each complete 4-byte frame.
 * Called  : relay_rx_task (polled every 50 ms).
 * Returns : Number of bytes read, or -1 on error.
 */
int uart_read_packet(uint8_t *buf, size_t max_size, TickType_t timeout)
{
    int len = uart_read_bytes(UART_PORT, buf, max_size, timeout);
    if (len < 0) {
        printf("uart_read_bytes status: %s\n", esp_err_to_name(ESP_FAIL));
    } else if (len > 0) {
        printf("uart_read_bytes status: %s\n", esp_err_to_name(ESP_OK));

        /* Reassemble 4-byte frames starting with 0xAA and print each complete one */
        static uint8_t rx_pkt[4];
        static int rx_cnt = 0;
        for (int i = 0; i < len; i++) {
            uint8_t rx_byte = buf[i];
            if (rx_cnt == 0) {
                if (rx_byte == 0xAA) {
                    rx_pkt[0] = rx_byte;
                    rx_cnt = 1;
                }
            } else {
                rx_pkt[rx_cnt] = rx_byte;
                rx_cnt++;
                if (rx_cnt == 4) {
                    printf("UART0 RX <-");
                    for (int j = 0; j < 4; j++) {
                        printf(" %02X", rx_pkt[j]);
                    }
                    printf("\n");
                    rx_cnt = 0;
                }
            }
        }
    }
    return len;
}