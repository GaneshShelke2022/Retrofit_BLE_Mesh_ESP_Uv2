/*
 * File    : ntp_time.h
 * Purpose : SNTP synchronization and POSIX timezone management for ESP32-C3 Master node.
 * Does    : Configures fixed IST (UTC+5:30) timezone, handles SNTP client lifecycle,
 *           tracks synchronization status, and provides local IST time queries.
 */

#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IST_UTC_OFFSET_MINUTES 330
#define IST_TZ_NAME            "IST"
#define DEFAULT_POSIX_TZ       "IST-5:30"

/* Initializes POSIX timezone to IST-5:30 and registers Wi-Fi IP event hook for SNTP */
esp_err_t ntp_time_init(void);

/* Starts SNTP client explicitly (called when Wi-Fi STA connects and gets IP) */
void ntp_time_start_sntp(void);

/* Stops SNTP client */
void ntp_time_stop_sntp(void);

/* Returns true if time has been synchronized with NTP at least once since boot */
bool ntp_time_is_synced(void);

/* Returns the epoch timestamp of the last successful NTP sync (0 if never synced) */
time_t ntp_time_get_last_sync_epoch(void);

/* Returns current epoch time (or 0 if not synced) */
time_t ntp_time_get_current_epoch(void);

/* Returns true if system time is valid (calibrated and maintained by internal RTC) */
bool ntp_time_is_valid(void);

/* Returns true if Wi-Fi network interface currently has an active IP connection */
bool ntp_time_is_network_connected(void);

/* Returns last-known persisted epoch loaded from NVS before reboot/power loss */
time_t ntp_time_get_persisted_epoch(void);

/* Safely persists the given timestamp to NVS for power-cycle recovery diagnostics */
void ntp_time_persist_timestamp(time_t epoch);

/*
 * Populates out_tm with the current local IST broken-down time.
 * Returns true if time is valid (synced), false if uncalibrated.
 */
bool ntp_time_get_ist(struct tm *out_tm);

/*
 * Queries comprehensive time status for the TIME_STATUS vendor mesh message.
 */
void ntp_time_get_status(bool *out_synced,
                         time_t *out_last_epoch,
                         int16_t *out_tz_offset_min,
                         const char **out_tz_name);

#ifdef __cplusplus
}
#endif

#endif // NTP_TIME_H
