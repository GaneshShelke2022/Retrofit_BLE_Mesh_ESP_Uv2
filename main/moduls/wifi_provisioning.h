/*
 * File    : wifi_provisioning.h
 * Purpose : Wi-Fi provisioning and connection manager for ESP32-C3 Master node.
 * Does    : Lazy Wi-Fi initialization, 5-press detector integration, SoftAP web server
 *           provisioning, atomic NVS blob persistence with CRC32 verification ("lumflux_wifi"),
 *           exponential backoff STA reconnection, and Phase 2 status/event APIs.
 */

#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connection and provisioning state machine */
typedef enum {
    WIFI_PROV_STATE_IDLE = 0,        /* Wi-Fi off or idle in STA mode */
    WIFI_PROV_STATE_PROVISIONING,    /* SoftAP and HTTP provisioning server active */
    WIFI_PROV_STATE_CONNECTING,      /* Connection attempt in progress */
    WIFI_PROV_STATE_CONNECTED,       /* Connected to router with valid IP */
    WIFI_PROV_STATE_FAILED           /* Connection attempt failed */
} wifi_prov_state_t;

/* Failure reasons mapped for /status response */
typedef enum {
    WIFI_PROV_FAIL_NONE = 0,
    WIFI_PROV_FAIL_WRONG_PASSWORD,
    WIFI_PROV_FAIL_NETWORK_NOT_FOUND,
    WIFI_PROV_FAIL_TIMEOUT,
    WIFI_PROV_FAIL_OTHER
} wifi_prov_fail_reason_t;

/* Event base and events for Phase 2 integration */
ESP_EVENT_DECLARE_BASE(WIFI_PROV_EVENT);

enum {
    WIFI_PROV_EVENT_GOT_IP,
    WIFI_PROV_EVENT_LOST_IP,
    WIFI_PROV_EVENT_PROV_STARTED,
    WIFI_PROV_EVENT_PROV_STOPPED,
};

/* Primary entry point: called at boot by app_main() */
void wifi_init(void);

/* Hook called by relay_manager on every mechanical switch transition */
void wifi_prov_post_switch_event(uint8_t switch_idx, bool state);

/* Provisioning control APIs */
void wifi_prov_start_provisioning(void);
void wifi_prov_stop_provisioning(void);

/* Status query APIs for Phase 2 */
bool wifi_prov_is_connected(void);
wifi_prov_state_t wifi_prov_get_state(void);
wifi_prov_fail_reason_t wifi_prov_get_fail_reason(void);

/* Credential & NVS management */
bool wifi_credentials_exist(void);
void clear_wifi_credentials(void);

/* Legacy aliases for backward compatibility */
void wifi_init_sta(void);
void wifi_init_provisioning(void);
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISIONING_H
