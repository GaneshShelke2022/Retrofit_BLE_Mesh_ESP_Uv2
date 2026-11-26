/*
 * File    : mesh_models.c
 * Purpose : BLE Mesh composition data, elements, and server model configuration.
 * Does    : Defines 4 elements (one per relay) with Config, Health, Generic OnOff,
 *           and Generic Power OnOff servers; initializes model states from NVS.
 * Related : mesh_models.h, storage.c, mesh_init.c
 */

#include "mesh_models.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "storage.h"
#include "schedule_mesh_vendor.h"
#include "device_role.h"

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};

static uint8_t test_ids[] = {
    0x00,
};

static esp_ble_mesh_health_srv_t health_server = {
    .health_test = {
        .id_count = 1,
        .test_ids = test_ids,
        .company_id = CID_ESP,
    },
};

ESP_BLE_MESH_HEALTH_PUB_DEFINE(health_pub, 4, ROLE_NODE);

/* Publication contexts for 4 Generic OnOff Servers */
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_0, 2 + 3, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_1, 2 + 3, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_2, 2 + 3, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_3, 2 + 3, ROLE_NODE);

/* Publication contexts for 4 Generic Power OnOff Servers */
ESP_BLE_MESH_MODEL_PUB_DEFINE(power_onoff_pub_0, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(power_onoff_pub_1, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(power_onoff_pub_2, 2 + 1, ROLE_NODE);
ESP_BLE_MESH_MODEL_PUB_DEFINE(power_onoff_pub_3, 2 + 1, ROLE_NODE);

/* Generic OnOff server instances — auto-respond to GET and SET */
static esp_ble_mesh_gen_onoff_srv_t onoff_srv_0 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
};

static esp_ble_mesh_gen_onoff_srv_t onoff_srv_1 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
};

static esp_ble_mesh_gen_onoff_srv_t onoff_srv_2 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
};

static esp_ble_mesh_gen_onoff_srv_t onoff_srv_3 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
};

/* Persisted power-up startup state for each relay */
static esp_ble_mesh_gen_onpowerup_state_t onpowerup_state_0;
static esp_ble_mesh_gen_onpowerup_state_t onpowerup_state_1;
static esp_ble_mesh_gen_onpowerup_state_t onpowerup_state_2;
static esp_ble_mesh_gen_onpowerup_state_t onpowerup_state_3;

/* Generic Power OnOff server instances */
static esp_ble_mesh_gen_power_onoff_srv_t power_onoff_srv_0 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_0,
};

static esp_ble_mesh_gen_power_onoff_srv_t power_onoff_srv_1 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_1,
};

static esp_ble_mesh_gen_power_onoff_srv_t power_onoff_srv_2 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_2,
};

static esp_ble_mesh_gen_power_onoff_srv_t power_onoff_srv_3 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_3,
};

/* Generic Power OnOff Setup server instances */
static esp_ble_mesh_gen_power_onoff_setup_srv_t power_onoff_setup_srv_0 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_0,
};

static esp_ble_mesh_gen_power_onoff_setup_srv_t power_onoff_setup_srv_1 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_1,
};

static esp_ble_mesh_gen_power_onoff_setup_srv_t power_onoff_setup_srv_2 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_2,
};

static esp_ble_mesh_gen_power_onoff_setup_srv_t power_onoff_setup_srv_3 = {
    .rsp_ctrl = {
        .get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
        .set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    },
    .state = &onpowerup_state_3,
};

/* Element 0: Primary element — Config, Health, OnOff, Power OnOff, Power OnOff Setup  */
static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_HEALTH_SRV(&health_server, &health_pub),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_0, &onoff_srv_0),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SRV(&power_onoff_pub_0, &power_onoff_srv_0),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SETUP_SRV(NULL, &power_onoff_setup_srv_0),
};

/* Elements 1–3: Secondary elements — OnOff, Power OnOff, Power OnOff Setup */
static esp_ble_mesh_model_t extend_model_0[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_1, &onoff_srv_1),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SRV(&power_onoff_pub_1, &power_onoff_srv_1),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SETUP_SRV(NULL, &power_onoff_setup_srv_1),
};

static esp_ble_mesh_model_t extend_model_1[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_2, &onoff_srv_2),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SRV(&power_onoff_pub_2, &power_onoff_srv_2),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SETUP_SRV(NULL, &power_onoff_setup_srv_2),
};

static esp_ble_mesh_model_t extend_model_2[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_3, &onoff_srv_3),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SRV(&power_onoff_pub_3, &power_onoff_srv_3),
    ESP_BLE_MESH_MODEL_GEN_POWER_ONOFF_SETUP_SRV(NULL, &power_onoff_setup_srv_3),
};

#if !defined(CONFIG_VENDOR_MODEL_ENABLE) || CONFIG_VENDOR_MODEL_ENABLE
/* Publication context for Vendor Model on Element 0 (Schedule Management) */
ESP_BLE_MESH_MODEL_PUB_DEFINE(sched_vnd_pub, 3 + 64, ROLE_NODE);

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CONFIG_SCHED_VENDOR_COMPANY_ID, CONFIG_SCHED_VENDOR_MODEL_ID,
                              sched_vnd_opcodes, &sched_vnd_pub, NULL),
};
#endif

/* Node composition: 4 elements, each mapped to one relay channel */
static esp_ble_mesh_elem_t elements[] = {
#if !defined(CONFIG_VENDOR_MODEL_ENABLE) || CONFIG_VENDOR_MODEL_ENABLE
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
#else
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
#endif
    ESP_BLE_MESH_ELEMENT(0, extend_model_0, ESP_BLE_MESH_MODEL_NONE),
    ESP_BLE_MESH_ELEMENT(0, extend_model_1, ESP_BLE_MESH_MODEL_NONE),
    ESP_BLE_MESH_ELEMENT(0, extend_model_2, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

/*
 * Purpose : Returns the static composition structure pointer.
 * Called  : mesh_init() at startup and relay_manager.c when publishing status updates.
 * Returns : Pointer to the node's esp_ble_mesh_comp_t.
 */
esp_ble_mesh_comp_t *mesh_models_get_composition(void)
{
    return &composition;
}

/*
 * Purpose : Reads NVS power-up and relay states and applies them to the BLE Mesh server model instances.
 * Called  : mesh_init() before registering callbacks.
 */
void mesh_models_init_state(void)
{
    for (int i = 0; i < 4; i++) {
        uint8_t power_up = 2; /* Default: Restore last state */
        load_power_up_state(i, &power_up);

        switch (i) {
        case 0: onpowerup_state_0.onpowerup = power_up; break;
        case 1: onpowerup_state_1.onpowerup = power_up; break;
        case 2: onpowerup_state_2.onpowerup = power_up; break;
        case 3: onpowerup_state_3.onpowerup = power_up; break;
        }

        bool relay_state = false;
        if (power_up == 0x00) {
            relay_state = false;
        } else if (power_up == 0x01) {
            relay_state = true;
        } else {
            load_relay_state(i, &relay_state);
        }

        switch (i) {
        case 0: onoff_srv_0.state.onoff = relay_state ? 1 : 0; break;
        case 1: onoff_srv_1.state.onoff = relay_state ? 1 : 0; break;
        case 2: onoff_srv_2.state.onoff = relay_state ? 1 : 0; break;
        case 3: onoff_srv_3.state.onoff = relay_state ? 1 : 0; break;
        }
    }
}

/*
 * Purpose : Searches all composition elements to find the one containing the given model.
 * Called  : mesh_callbacks_generic_server_cb() to map a model event to a relay index.
 * Returns : 0-based element index, or -1 if not found.
 */
int mesh_models_get_element_index(esp_ble_mesh_model_t *model)
{
    for (int i = 0; i < composition.element_count; i++) {
        if (model->element == &composition.elements[i]) {
            return i;
        }
    }
    return -1;
}
