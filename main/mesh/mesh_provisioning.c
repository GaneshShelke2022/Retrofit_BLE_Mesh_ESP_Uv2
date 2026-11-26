
/*
 * File    : mesh_provisioning.c
 * Purpose : Device UUID generation and BLE Mesh provisioning configuration.
 * Does    : Builds a unique RFC 4122 v1 UUID from the BT MAC and provides provisioning parameters.
 * Related : mesh_provisioning.h
 */

#include "mesh_provisioning.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>

#define TAG "MESH_PROV"

static uint8_t dev_uuid[16] = {0};

/* OOB authentication disabled for compatibility with standard provisioning apps */
static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
    .output_size = 0,
    .output_actions = 0,
};

/*
 * Purpose : Reads the BT MAC address and builds a unique RFC 4122 v1 Device UUID.
 * Called  : mesh_init() before registering callbacks.
 */
void mesh_provisioning_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BT MAC address, using Wi-Fi MAC");
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }

    memset(dev_uuid, 0, sizeof(dev_uuid));
    dev_uuid[6] = 0x10; /* Version 1 */
    dev_uuid[8] = 0x80; /* Variant */
    memcpy(&dev_uuid[10], mac, 6);

    ESP_LOGI(TAG, "Generated unique Device UUID: %02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             dev_uuid[0], dev_uuid[1], dev_uuid[2], dev_uuid[3],
             dev_uuid[4], dev_uuid[5],
             dev_uuid[6], dev_uuid[7],
             dev_uuid[8], dev_uuid[9],
             dev_uuid[10], dev_uuid[11], dev_uuid[12], dev_uuid[13], dev_uuid[14], dev_uuid[15]);
}

/*
 * Purpose : Returns the statically configured provisioning structure reference.
 * Called  : mesh_init() during stack setup.
 * Returns : Pointer to the esp_ble_mesh_prov_t structure.
 */
esp_ble_mesh_prov_t *mesh_provisioning_get_prov(void)
{
    return &provision;
}
