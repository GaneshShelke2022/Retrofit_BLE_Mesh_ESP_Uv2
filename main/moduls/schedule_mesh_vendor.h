/*
 * File    : schedule_mesh_vendor.h
 * Purpose : Bluetooth Mesh Vendor Server Model for Schedule Transfer and Status.
 * Does    : Implements 3-byte vendor opcodes, chunked atomic upload staging,
 *           rate-limiting, decoupled FreeRTOS queue processing, and status replies.
 */

#ifndef SCHEDULE_MESH_VENDOR_H
#define SCHEDULE_MESH_VENDOR_H

#include "schedule_model.h"
#include "esp_ble_mesh_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CID_ESP 0x02E5

#ifndef CONFIG_SCHED_VENDOR_COMPANY_ID
#define CONFIG_SCHED_VENDOR_COMPANY_ID CID_ESP
#endif

#ifndef CONFIG_SCHED_VENDOR_MODEL_ID
#define CONFIG_SCHED_VENDOR_MODEL_ID 0x0001
#endif

/* 1-byte Opcode definitions */
#define SCHED_OP_ID_BEGIN       0x01
#define SCHED_OP_ID_ACTIONS     0x02
#define SCHED_OP_ID_COMMIT      0x03
#define SCHED_OP_ID_DELETE      0x04
#define SCHED_OP_ID_ENABLE      0x05
#define SCHED_OP_ID_CLEAR       0x06
#define SCHED_OP_ID_GET         0x07
#define SCHED_OP_ID_LIST        0x08
#define SCHED_OP_ID_RUN_NOW     0x09
#define SCHED_OP_ID_TIME_GET    0x0A

#define SCHED_OP_ID_STATUS      0x0B
#define SCHED_OP_ID_LIST_STATUS 0x0C
#define SCHED_OP_ID_TIME_STATUS 0x0D

/* 3-byte Vendor Opcodes for ESP-BLE-MESH */
#define SCHED_VND_OP_BEGIN       ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_BEGIN, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_ACTIONS     ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_ACTIONS, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_COMMIT      ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_COMMIT, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_DELETE      ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_DELETE, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_ENABLE      ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_ENABLE, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_CLEAR       ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_CLEAR, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_GET         ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_GET, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_LIST        ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_LIST, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_RUN_NOW     ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_RUN_NOW, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_TIME_GET    ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_TIME_GET, CONFIG_SCHED_VENDOR_COMPANY_ID)

#define SCHED_VND_OP_STATUS      ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_STATUS, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_LIST_STATUS ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_LIST_STATUS, CONFIG_SCHED_VENDOR_COMPANY_ID)
#define SCHED_VND_OP_TIME_STATUS ESP_BLE_MESH_MODEL_OP_3(SCHED_OP_ID_TIME_STATUS, CONFIG_SCHED_VENDOR_COMPANY_ID)

/* Status error codes */
typedef enum {
    SCHED_STATUS_OK                   = 0x00,
    SCHED_STATUS_ERR_BAD_VERSION      = 0x01,
    SCHED_STATUS_ERR_BAD_RANGE        = 0x02,
    SCHED_STATUS_ERR_TOO_MANY_ACTIONS = 0x03,
    SCHED_STATUS_ERR_STORAGE_FULL     = 0x04,
    SCHED_STATUS_ERR_CRC_MISMATCH     = 0x05,
    SCHED_STATUS_ERR_BUSY             = 0x06,
    SCHED_STATUS_ERR_UNKNOWN_ID       = 0x07,
    SCHED_STATUS_WARN_TIME_NOT_SYNCED = 0x08,
    SCHED_STATUS_ERR_DUPLICATE_TARGET = 0x09,
    SCHED_STATUS_ERR_STAGING_TIMEOUT  = 0x0A,
    SCHED_STATUS_ERR_INVALID_PARAM    = 0x0B
} sched_status_code_t;

/* Opcode array for Vendor Model definition */
extern esp_ble_mesh_model_op_t sched_vnd_opcodes[];

/* Returns the opcode list for the Vendor Model definition */
esp_ble_mesh_model_op_t *schedule_mesh_vendor_get_opcodes(void);

/* Initializes the Vendor Model message worker task and staging buffers */
esp_err_t schedule_mesh_vendor_init(void);

/* Callback function registered via esp_ble_mesh_register_custom_model_callback */
void schedule_mesh_vendor_cb(esp_ble_mesh_model_cb_event_t event,
                             esp_ble_mesh_model_cb_param_t *param);

/* Wire protocol encode / decode helper functions for tests and serialization */
size_t sched_encode_begin(uint8_t *out_buf, size_t buf_len, uint8_t tid, const schedule_entry_t *s);
bool sched_decode_begin(const uint8_t *in_buf, size_t len, uint8_t *out_tid, schedule_entry_t *out_s);

size_t sched_encode_actions(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint8_t seq, const sched_action_t *actions, uint8_t count);
bool sched_decode_actions(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint8_t *out_seq, sched_action_t *out_actions, uint8_t *out_count);

size_t sched_encode_commit(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint32_t crc32);
bool sched_decode_commit(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint32_t *out_crc32);

size_t sched_encode_status(uint8_t *out_buf, size_t buf_len, uint8_t status, uint8_t tid, uint8_t id);
bool sched_decode_status(const uint8_t *in_buf, size_t len, uint8_t *out_status, uint8_t *out_tid, uint8_t *out_id);

size_t sched_encode_list_status(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint8_t count, uint16_t mask);
bool sched_decode_list_status(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint8_t *out_count, uint16_t *out_mask);

size_t sched_encode_time_status(uint8_t *out_buf, size_t buf_len, uint8_t tid, uint8_t time_valid, uint32_t last_epoch, int16_t tz_offset, uint8_t count, uint8_t free_slots, uint8_t version);
bool sched_decode_time_status(const uint8_t *in_buf, size_t len, uint8_t *out_tid, uint8_t *out_time_valid, uint32_t *out_last_epoch, int16_t *out_tz_offset, uint8_t *out_count, uint8_t *out_free_slots, uint8_t *out_version);

#ifdef __cplusplus
}
#endif

#endif // SCHEDULE_MESH_VENDOR_H
