/*
 * File    : relay_manager.c
 * Purpose : Authoritative relay state machine and co-processor coordinator.
 * Does    : State transitions, UART RX packet assembly, retry/ACK timers,
 *           NVS persistence, and BLE Mesh model updates for 4 relay channels.
 * Related : relay_manager.h, uart_driver.h, holtek_protocol.h, storage.h, mesh_models.h
 *
 * System data flow:
 *   BLE Mesh command received  (relay_set)
 *     ↓  encode & send UART packet
 *   Holtek MCU executes relay
 *     ↓  sends ACK packet back
 *   handle_ack: confirm state, save NVS, publish Mesh status
 *
 * Boot handshake flow:
 *   Holtek powers on → sends BOOT packet (AA FF 00 A9)
 *     ↓  handle_boot loads NVS states
 *   sync_single_relay sends restore commands
 *     ↓  Holtek ACKs each one → state confirmed
 */

#include "relay_manager.h"
#include "uart_driver.h"
#include "holtek_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_networking_api.h"
#include <stdio.h>

#define TAG "RELAY_MGR"

/* Extern NVS and composition helper functions defined in main/ */
extern esp_err_t load_power_up_state(uint8_t index, uint8_t *power_up);
extern esp_err_t load_relay_state(uint8_t index, bool *state);
extern esp_err_t save_relay_state(uint8_t index, bool state);
extern esp_ble_mesh_comp_t *mesh_models_get_composition(void);
extern void wifi_prov_post_switch_event(uint8_t index, bool state);

typedef enum {
    RELAY_STATE_UNKNOWN,
    RELAY_STATE_SYNCING,
    RELAY_STATE_OFF,
    RELAY_STATE_ON,
    RELAY_STATE_ERROR,
    RELAY_STATE_RECOVERING
} relay_state_t;

typedef struct {
    relay_state_t state;
    bool target_state;
    int retry_count;
    TickType_t last_cmd_time;
    bool pending_cmd;
} relay_info_t;

static relay_info_t relays[4] = {
    { .state = RELAY_STATE_UNKNOWN, .target_state = false, .retry_count = 0, .pending_cmd = false },
    { .state = RELAY_STATE_UNKNOWN, .target_state = false, .retry_count = 0, .pending_cmd = false },
    { .state = RELAY_STATE_UNKNOWN, .target_state = false, .retry_count = 0, .pending_cmd = false },
    { .state = RELAY_STATE_UNKNOWN, .target_state = false, .retry_count = 0, .pending_cmd = false }
};

static SemaphoreHandle_t relay_mutex = NULL;

#define MAX_RETRIES       3
#define CMD_TIMEOUT_TICKS pdMS_TO_TICKS(500)

/*
 * Purpose : UART RX loop — assembles 4-byte frames and drives the retry timer for all relays.
 * Called  : xTaskCreate in relay_manager_init(); runs continuously at Priority 4.
 */
static void relay_rx_task(void *pvParameters)
{
    uint8_t rx_byte;
    uint8_t packet[HOLTEK_PACKET_LEN];
    int bytes_in_packet = 0;

    printf("UART RX Task Started\n");
    printf("Waiting for Holtek\n");

    while (1) {
        /* Read one byte at a time with a 50 ms timeout */
        int len = uart_read_packet(&rx_byte, 1, pdMS_TO_TICKS(50));

        if (len > 0) {
            if (bytes_in_packet == 0) {
                if (rx_byte == HOLTEK_HEADER) {
                    packet[0] = rx_byte;
                    bytes_in_packet = 1;
                }
            } else {
                packet[bytes_in_packet] = rx_byte;
                bytes_in_packet++;

                if (bytes_in_packet == HOLTEK_PACKET_LEN) {
                    uint8_t relay_idx;
                    bool state;

                    if (holtek_decode_packet(packet, HOLTEK_PACKET_LEN, &relay_idx, &state)) {
                        if (relay_idx == 0xFF) {
                            handle_boot();
                        } else {
                            uint8_t index = relay_idx - 1;
                            if (index < 4) {
                                /* Non-consuming hook: forward switch transition before relay state filtering */
                                wifi_prov_post_switch_event(index, state);
                                handle_ack(index, state);
                            }
                        }
                    }
                    bytes_in_packet = 0;
                }
            }
        }

        /* Check all relays for ACK timeout and retransmit if needed */
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < 4; i++) {
            xSemaphoreTake(relay_mutex, portMAX_DELAY);
            if (relays[i].pending_cmd && (now - relays[i].last_cmd_time >= CMD_TIMEOUT_TICKS)) {
                if (relays[i].retry_count < MAX_RETRIES) {
                    relays[i].retry_count++;
                    relays[i].last_cmd_time = now;
                    bool target_state = relays[i].target_state;
                    xSemaphoreGive(relay_mutex);

                    uint8_t retry_pkt[HOLTEK_PACKET_LEN];
                    if (holtek_encode_packet(i + 1, target_state, retry_pkt, sizeof(retry_pkt))) {
                        uart_send_packet(retry_pkt, sizeof(retry_pkt));
                    }
                } else {
                    relays[i].state = RELAY_STATE_ERROR;
                    relays[i].pending_cmd = false;
                    xSemaphoreGive(relay_mutex);

                    printf("[ERROR] Relay%d OUT_OF_SYNC (ACK Timeout after %d retries)\n", i + 1, MAX_RETRIES);
                }
            } else {
                xSemaphoreGive(relay_mutex);
            }
        }
    }
}

/*
 * Purpose : Creates the relay mutex, loads NVS power-up states, sends startup frames, and spawns relay_rx_task.
 * Called  : app_main() Step 5.
 */
void relay_manager_init(void)
{
    relay_mutex = xSemaphoreCreateMutex();
    if (relay_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create relay mutex");
        return;
    }

    for (int i = 0; i < 4; i++) {
        uint8_t power_up = 2;
        load_power_up_state(i, &power_up);

        bool init_state = false;
        if (power_up == 0x00) {
            init_state = false;
        } else if (power_up == 0x01) {
            init_state = true;
        } else {
            load_relay_state(i, &init_state);
        }

        relays[i].state = init_state ? RELAY_STATE_ON : RELAY_STATE_OFF;
        relays[i].target_state = init_state;
        relays[i].retry_count = 0;
        relays[i].pending_cmd = false;

        uint8_t packet[HOLTEK_PACKET_LEN];
        if (holtek_encode_packet(i + 1, init_state, packet, sizeof(packet))) {
            uart_send_packet(packet, sizeof(packet));
        }
    }

    xTaskCreate(relay_rx_task, "relay_rx_task", 4096, NULL, 4, NULL);
}

/*
 * Purpose : Updates BLE Mesh model state and publishes status to 0xFFFF (broadcast to all mesh nodes and phones).
 * Called  : relay_set(), handle_ack(), and handle_boot().
 */
static void publish_relay_status(uint8_t index, bool state)
{
    esp_ble_mesh_comp_t *comp = mesh_models_get_composition();
    if (comp != NULL && index < comp->element_count) {
        esp_ble_mesh_elem_t *elem = &comp->elements[index];
        for (int i = 0; i < elem->sig_model_count; i++) {
            esp_ble_mesh_model_t *model = &elem->sig_models[i];
            if (model->model_id == ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV) {
                esp_ble_mesh_gen_onoff_srv_t *srv = (esp_ble_mesh_gen_onoff_srv_t *)model->user_data;
                if (srv != NULL) {
                    srv->state.onoff = state ? 1 : 0;

                    esp_ble_mesh_server_state_value_t val = {0};
                    val.gen_onoff.onoff = state ? 1 : 0;
                    esp_ble_mesh_server_model_update_state(model, ESP_BLE_MESH_GENERIC_ONOFF_STATE, &val);

                    uint8_t onoff = state ? 1 : 0;
                    if (model->pub != NULL) {
                        if (model->pub->publish_addr == ESP_BLE_MESH_ADDR_UNASSIGNED || model->pub->publish_addr == 0x0001) {
                            model->pub->publish_addr = 0xFFFF;
                        }
                        if (model->pub->app_idx == ESP_BLE_MESH_KEY_UNUSED) {
                            model->pub->app_idx = 0;
                        }
                        if (model->pub->ttl == 0) {
                            model->pub->ttl = 7;
                        }
                    }
                    esp_err_t pub_err = esp_ble_mesh_model_publish(model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS,
                                                                   sizeof(onoff), &onoff, ROLE_NODE);
                    if (pub_err != ESP_OK) {
                        ESP_LOGW(TAG, "esp_ble_mesh_model_publish for relay%d returned: 0x%x", index + 1, pub_err);
                    }
                }
                break;
            }
        }
    }
}

/*
 * Purpose : Sends a relay command to Holtek and starts the ACK timeout timer.
 *           Skips and re-publishes immediately if the relay is already in the requested state.
 * Called  : relay_hw_on() / relay_hw_off() from the BLE Mesh generic server callback.
 */
void relay_set(uint8_t index, bool state)
{
    if (index >= 4) {
        return;
    }

    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    relay_state_t cur_state = relays[index].state;
    bool cur_val = (cur_state == RELAY_STATE_ON);
    bool is_confirmed = (cur_state == RELAY_STATE_ON || cur_state == RELAY_STATE_OFF);
    xSemaphoreGive(relay_mutex);
    /* Already in the desired confirmed state — just republish to Mesh */
    if (is_confirmed && cur_val == state) {
        publish_relay_status(index, state);
        return;
    }

    /* New state requested — transition to SYNCING and send UART command */
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    relays[index].state = RELAY_STATE_SYNCING;
    relays[index].target_state = state;
    relays[index].retry_count = 0;
    relays[index].last_cmd_time = xTaskGetTickCount();
    relays[index].pending_cmd = true;
    xSemaphoreGive(relay_mutex);

    uint8_t packet[HOLTEK_PACKET_LEN];
    if (holtek_encode_packet(index + 1, state, packet, sizeof(packet))) {
        ESP_LOGD(TAG, "Relay%d requested %s, UART TX: AA %02X %02X %02X", index + 1, state ? "ON" : "OFF", packet[1], packet[2], packet[3]);
        uart_send_packet(packet, sizeof(packet));
    }
}

/*
 * Purpose : Returns the current confirmed ON/OFF state, or the target state if still syncing.
 * Called  : relay_hw_get_state() and GET message handlers.
 * Returns : true = ON, false = OFF.
 */
bool relay_get(uint8_t index)
{
    if (index >= 4) {
        return false;
    }

    bool state = false;
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (relays[index].state == RELAY_STATE_ON) {
        state = true;
    } else if (relays[index].state == RELAY_STATE_OFF) {
        state = false;
    } else {
        state = relays[index].target_state;
    }
    xSemaphoreGive(relay_mutex);
    return state;
}

/*
 * Purpose : Reads current state and calls relay_set() with the inverse.
 * Called  : relay_hw_toggle() or local switch event handlers.
 */
void relay_toggle(uint8_t index)
{
    if (index >= 4) {
        return;
    }

    bool new_state;
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    if (relays[index].state == RELAY_STATE_ON) {
        new_state = false;
    } else if (relays[index].state == RELAY_STATE_OFF) {
        new_state = true;
    } else {
        new_state = !relays[index].target_state;
    }
    xSemaphoreGive(relay_mutex);

    relay_set(index, new_state);
}

/*
 * Purpose : Handles the BOOT packet (relay_idx == 0xFF) from Holtek.
 *           Loads NVS states and re-syncs all 4 relays after co-processor power-up.
 * Called  : relay_rx_task when relay_idx == 0xFF is decoded.
 */
void handle_boot(void)
{
    printf("BOOT received\n");
    printf("↓\n");
    printf("Restoring NVS\n");
    printf("↓\n");

    for (int i = 0; i < 4; i++) {
        bool stored_state = false;
        load_relay_state(i, &stored_state);

        printf("Relay%d %s\n", i + 1, stored_state ? "ON" : "OFF");
        printf("↓\n");

        sync_single_relay(i, stored_state);
        publish_relay_status(i, stored_state);
    }

    printf("Synchronization Complete\n");
}

/*
 * Purpose : Puts a single relay into RECOVERING state and sends a UART restore command.
 * Called  : handle_boot() for each relay, and sync_all_relays().
 */
void sync_single_relay(uint8_t index, bool state)
{
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    relays[index].state = RELAY_STATE_RECOVERING;
    relays[index].target_state = state;
    relays[index].retry_count = 0;
    relays[index].last_cmd_time = xTaskGetTickCount();
    relays[index].pending_cmd = true;
    xSemaphoreGive(relay_mutex);

    uint8_t packet[HOLTEK_PACKET_LEN];
    if (holtek_encode_packet(index + 1, state, packet, sizeof(packet))) {
        uart_send_packet(packet, sizeof(packet));
    }
}

/*
 * Purpose : Reads NVS for all 4 relays and calls sync_single_relay() on each.
 * Called  : restore_from_nvs() or manual recovery paths.
 */
void sync_all_relays(void)
{
    for (int i = 0; i < 4; i++) {
        bool state = false;
        load_relay_state(i, &state);
        sync_single_relay(i, state);
    }
}

/*
 * Purpose : Prints state machine state and target for a relay index (debug helper).
 * Called  : Diagnostic use only.
 */
void request_state(uint8_t index)
{
    if (index >= 4) {
        return;
    }
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    relay_state_t state = relays[index].state;
    bool target = relays[index].target_state;
    xSemaphoreGive(relay_mutex);

    printf("Relay%d state: %d (target: %d)\n", index + 1, state, target);
}

/*
 * Purpose : Named recovery API — thin wrapper around sync_all_relays().
 * Called  : Startup or handshake recovery paths.
 */
void restore_from_nvs(void)
{
    sync_all_relays();
}

/*
 * Purpose : Processes an ACK or switch-event packet from Holtek.
 *           Confirms the relay state, cancels retries, saves to NVS, and publishes Mesh status.
 * Called  : relay_rx_task on every valid decoded packet with relay_idx != 0xFF.
 */
void handle_ack(uint8_t index, bool state)
{
    xSemaphoreTake(relay_mutex, portMAX_DELAY);
    relay_state_t old_state = relays[index].state;
    bool cur_val = (old_state == RELAY_STATE_ON);
    bool is_confirmed = (old_state == RELAY_STATE_ON || old_state == RELAY_STATE_OFF);

    /* Ignore duplicate ACK for an already-confirmed state */
    if (is_confirmed && cur_val == state) {
        xSemaphoreGive(relay_mutex);
        return;
    }

    relays[index].state = state ? RELAY_STATE_ON : RELAY_STATE_OFF;
    relays[index].pending_cmd = false;
    relays[index].retry_count = 0;
    xSemaphoreGive(relay_mutex);

    save_relay_state(index, state);

    /* Update BLE Mesh model state and publish status to the network */
    publish_relay_status(index, state);

    if (old_state == RELAY_STATE_SYNCING) {
        ESP_LOGD(TAG, "Holtek ACK Relay%d %s", index + 1, state ? "ON" : "OFF");
    } else if (old_state == RELAY_STATE_RECOVERING) {
        /* Boot recovery ACK is handled silently */
    } else {
        ESP_LOGD(TAG, "UART RX Relay%d State %s", index + 1, state ? "ON" : "OFF");
    }
}
