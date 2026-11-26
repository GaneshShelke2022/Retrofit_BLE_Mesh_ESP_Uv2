/*
 * File    : device_role.c
 * Purpose : Implementation of permanent, one-time device-role detection.
 * Does    : NVS persistence check, Holtek UART ACK probe, role assignment.
 */

#include "device_role.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_driver.h"
#include "holtek_protocol.h"
#include <stdio.h>

#define TAG "DEVICE_ROLE"
#define NVS_ROLE_NAMESPACE "device_role"
#define NVS_ROLE_KEY       "role"
#define ROLE_DETECTION_RETRIES 3
#define ROLE_ACK_TIMEOUT_MS    500

static device_role_t s_current_role = DEVICE_ROLE_UNKNOWN;

static const char *role_to_string(device_role_t role)
{
    switch (role) {
    case DEVICE_ROLE_MASTER:
        return "MASTER";
    case DEVICE_ROLE_NORMAL_NODE:
        return "NORMAL_NODE";
    default:
        return "UNKNOWN";
    }
}

device_role_t device_role_init(void)
{
#if defined(CONFIG_DEVICE_ROLE_FORCE_MASTER) && CONFIG_DEVICE_ROLE_FORCE_MASTER
    s_current_role = DEVICE_ROLE_MASTER;
    ESP_LOGI("wifi_prov", "[BOOT] CONFIG_DEVICE_ROLE_FORCE_MASTER is set: Device role = MASTER (is_master=1)");
    ESP_LOGI(TAG, "Forced MASTER role active. Bypassing Holtek UART probe.");
    return s_current_role;
#endif

    nvs_handle_t nvs_h;
    uint8_t stored_val = 0;
    esp_err_t err = nvs_open(NVS_ROLE_NAMESPACE, NVS_READWRITE, &nvs_h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open error: %s", esp_err_to_name(err));
    } else {
        err = nvs_get_u8(nvs_h, NVS_ROLE_KEY, &stored_val);
        nvs_close(nvs_h);
    }

    if (err == ESP_OK) {
        if (stored_val == DEVICE_ROLE_MASTER || stored_val == DEVICE_ROLE_NORMAL_NODE) {
            s_current_role = (device_role_t)stored_val;
            ESP_LOGI(TAG, "Stored role found: %s. Skipping Holtek detection.", role_to_string(s_current_role));
            ESP_LOGI("wifi_prov", "[BOOT] Device role result: %s (is_master=%d)", role_to_string(s_current_role), (s_current_role == DEVICE_ROLE_MASTER));
            return s_current_role;
        } else {
            ESP_LOGW(TAG, "Stored role invalid (%u). Treating as UNKNOWN and re-detecting.", stored_val);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored role found. Starting first-boot detection.");
    } else {
        ESP_LOGE(TAG, "NVS read error: %s", esp_err_to_name(err));
    }

    /* 3a. Short startup delay for Holtek readiness */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 3b. Existing Holtek init + ACK-check routine with bounded retries */
    bool ack_received = false;
    uint8_t tx_buf[HOLTEK_PACKET_LEN];

    if (holtek_encode_packet(1, false, tx_buf, sizeof(tx_buf))) {
        for (int retry = 0; retry < ROLE_DETECTION_RETRIES; retry++) {
            uart_send_packet(tx_buf, sizeof(tx_buf));

            uint8_t frame[HOLTEK_PACKET_LEN];
            int bytes_in_frame = 0;
            TickType_t start_tick = xTaskGetTickCount();
            TickType_t timeout_ticks = pdMS_TO_TICKS(ROLE_ACK_TIMEOUT_MS);

            while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
                uint8_t rx_byte;
                int len = uart_read_packet(&rx_byte, 1, pdMS_TO_TICKS(50));
                if (len > 0) {
                    if (bytes_in_frame == 0) {
                        if (rx_byte == HOLTEK_HEADER) {
                            frame[0] = rx_byte;
                            bytes_in_frame = 1;
                        }
                    } else {
                        frame[bytes_in_frame++] = rx_byte;
                        if (bytes_in_frame == HOLTEK_PACKET_LEN) {
                            uint8_t relay_idx;
                            bool state;
                            if (holtek_decode_packet(frame, HOLTEK_PACKET_LEN, &relay_idx, &state)) {
                                ack_received = true;
                                break;
                            }
                            bytes_in_frame = 0;
                        }
                    }
                }
            }
            if (ack_received) {
                break;
            }
        }
    }

    /* 3c. Valid ACK -> DEVICE_ROLE_NORMAL_NODE, No valid ACK after retries -> DEVICE_ROLE_MASTER */
    if (ack_received) {
        s_current_role = DEVICE_ROLE_NORMAL_NODE;
    } else {
        s_current_role = DEVICE_ROLE_MASTER;
    }

    /* 3d. Write role to NVS and verify write */
    err = nvs_open(NVS_ROLE_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_h, NVS_ROLE_KEY, (uint8_t)s_current_role);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
        }
        nvs_close(nvs_h);
    }

    if (err == ESP_OK) {
        if (s_current_role == DEVICE_ROLE_NORMAL_NODE) {
            ESP_LOGI(TAG, "Holtek ACK received. Device detected as NORMAL_NODE. Saved to NVS.");
        } else {
            ESP_LOGI(TAG, "Holtek ACK not received after retries. Detected as MASTER. Saved to NVS.");
        }
    } else {
        ESP_LOGE(TAG, "Failed to save detected role (%s) to NVS: %s", role_to_string(s_current_role), esp_err_to_name(err));
    }

    /* 3e. Return detected role */
    ESP_LOGI("wifi_prov", "[BOOT] Device role result: %s (is_master=%d)", role_to_string(s_current_role), (s_current_role == DEVICE_ROLE_MASTER));
    return s_current_role;
}

device_role_t device_role_get(void)
{
    return s_current_role;
}

bool device_role_is_master(void)
{
    return (s_current_role == DEVICE_ROLE_MASTER);
}

bool device_role_is_normal_node(void)
{
    return (s_current_role == DEVICE_ROLE_NORMAL_NODE);
}

esp_err_t device_role_clear(void)
{
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_ROLE_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs_h, NVS_ROLE_KEY);
        if (err == ESP_OK) {
            nvs_commit(nvs_h);
            s_current_role = DEVICE_ROLE_UNKNOWN;
            ESP_LOGI(TAG, "Device role cleared from NVS.");
        }
        nvs_close(nvs_h);
    }
    return err;
}
