/*
 * File    : ntp_time.c
 * Purpose : SNTP synchronization and POSIX timezone management implementation.
 * Does    : Fixed IST-5:30 POSIX timezone, SNTP polling, sync notification tracking.
 */

#include "ntp_time.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_provisioning.h"

#define TAG "NTP_TIME"

#define NTP_NVS_NAMESPACE      "ntp_time"
#define NTP_NVS_KEY_LAST_EPOCH "last_epoch"

#ifndef CONFIG_SCHED_TIMEZONE
#define CONFIG_SCHED_TIMEZONE DEFAULT_POSIX_TZ
#endif

#ifndef CONFIG_SCHED_NTP_SERVER
#define CONFIG_SCHED_NTP_SERVER "pool.ntp.org"
#endif

static bool s_time_synced = false;
static time_t s_last_sync_epoch = 0;
static time_t s_persisted_last_epoch = 0;
static bool s_sntp_running = false;
static bool s_network_connected = false;
static esp_timer_handle_t s_sntp_delay_timer = NULL;

static void persist_epoch_to_nvs(time_t epoch)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NTP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        uint64_t val = (uint64_t)epoch;
        nvs_set_u64(h, NTP_NVS_KEY_LAST_EPOCH, val);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGD(TAG, "Persisted timestamp to NVS: %llu", (unsigned long long)val);
    }
}

static time_t load_persisted_epoch_from_nvs(void)
{
    nvs_handle_t h;
    uint64_t val = 0;
    esp_err_t err = nvs_open(NTP_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_get_u64(h, NTP_NVS_KEY_LAST_EPOCH, &val);
        nvs_close(h);
    }
    return (time_t)val;
}

void ntp_time_persist_timestamp(time_t epoch)
{
    if (epoch > 1735689600) { /* Only persist valid post-2025 epoch */
        persist_epoch_to_nvs(epoch);
    }
}

static void time_sync_notification_cb(struct timeval *tv)
{
    s_time_synced = true;
    s_last_sync_epoch = tv->tv_sec;

    struct tm timeinfo;
    localtime_r(&s_last_sync_epoch, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "NTP Time synchronized successfully! Current IST: %s (epoch %lld)",
             strftime_buf, (long long)s_last_sync_epoch);

    /* Persist to NVS for power-cycle recovery diagnostics */
    persist_epoch_to_nvs(s_last_sync_epoch);
}

static void sntp_delay_timer_cb(void *arg)
{
    (void)arg;
#if defined(CONFIG_SNTP_ENABLE) && !CONFIG_SNTP_ENABLE
    ESP_LOGI(TAG, "CONFIG_SNTP_ENABLE is disabled; skipping SNTP initialization.");
#else
    ESP_LOGI(TAG, "[NTP] 2000 ms stabilization delay elapsed after GOT_IP. Starting/restarting SNTP client...");
    ntp_time_start_sntp();
#endif
}

static void on_wifi_prov_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT) {
        if (id == WIFI_PROV_EVENT_GOT_IP) {
            s_network_connected = true;
            ESP_LOGI(TAG, "Wi-Fi Got IP event received. Network online. Scheduling SNTP sync in 2000 ms...");
            if (!s_sntp_delay_timer) {
                esp_timer_create_args_t tmr_args = {
                    .callback = &sntp_delay_timer_cb,
                    .name = "sntp_delay_tmr"
                };
                esp_timer_create(&tmr_args, &s_sntp_delay_timer);
            }
            if (s_sntp_delay_timer) {
                esp_timer_stop(s_sntp_delay_timer);
                esp_timer_start_once(s_sntp_delay_timer, 2000000ULL); /* 2 seconds delay */
            }
        } else if (id == WIFI_PROV_EVENT_LOST_IP) {
            s_network_connected = false;
            ESP_LOGW(TAG, "Wi-Fi Lost IP event received (network disconnected).");
            ESP_LOGI(TAG, "Offline Scheduling: Internal RTC maintaining accurate timekeeping. System time remains valid.");
            if (s_sntp_delay_timer) {
                esp_timer_stop(s_sntp_delay_timer);
            }
            /* Crucial: Do NOT reset s_time_synced or system clock! Internal RTC continues ticking. */
        }
    }
}

esp_err_t ntp_time_init(void)
{
    /* 1. Configure fixed POSIX timezone for India (IST, UTC+5:30) */
    setenv("TZ", CONFIG_SCHED_TIMEZONE, 1);
    tzset();
    ESP_LOGI(TAG, "Configured POSIX timezone: %s (UTC+5:30, no DST)", CONFIG_SCHED_TIMEZONE);

    /* 2. Configure SNTP operating mode, smooth sync mode, and notification callback */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_SCHED_NTP_SERVER);
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    /* 3. Load persisted last-known timestamp from NVS for power-cycle diagnostics */
    s_persisted_last_epoch = load_persisted_epoch_from_nvs();
    if (s_persisted_last_epoch > 0) {
        struct tm tm_persisted;
        localtime_r(&s_persisted_last_epoch, &tm_persisted);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_persisted);
        ESP_LOGI(TAG, "Loaded last-known persisted time from NVS: %s IST (epoch: %lld)",
                 buf, (long long)s_persisted_last_epoch);
        ESP_LOGI(TAG, "Power-Cycle Policy: Internal RTC has no battery backup. Clock marked uncalibrated until fresh NTP sync.");
    }

    /* 4. Register for Wi-Fi IP events to start/restart SNTP automatically when connected */
    esp_event_handler_instance_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, on_wifi_prov_event, NULL, NULL);

    /* If Wi-Fi is already connected with IP at boot, schedule SNTP start with 2s delay */
    if (wifi_prov_is_connected()) {
        s_network_connected = true;
        if (!s_sntp_delay_timer) {
            esp_timer_create_args_t tmr_args = {
                .callback = &sntp_delay_timer_cb,
                .name = "sntp_delay_tmr"
            };
            esp_timer_create(&tmr_args, &s_sntp_delay_timer);
        }
        if (s_sntp_delay_timer) {
            esp_timer_start_once(s_sntp_delay_timer, 2000000ULL);
        }
    }

    return ESP_OK;
}

void ntp_time_start_sntp(void)
{
#if defined(CONFIG_SNTP_ENABLE) && !CONFIG_SNTP_ENABLE
    ESP_LOGI(TAG, "CONFIG_SNTP_ENABLE is disabled; SNTP client will not start.");
    return;
#else
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP client already active; restarting for immediate resynchronization with server: %s", CONFIG_SCHED_NTP_SERVER);
        esp_sntp_restart();
    } else {
        ESP_LOGI(TAG, "Initializing SNTP client (server: %s)...", CONFIG_SCHED_NTP_SERVER);
        esp_sntp_init();
        s_sntp_running = true;
    }
#endif
}

void ntp_time_stop_sntp(void)
{
    if (s_sntp_running) {
        esp_sntp_stop();
        s_sntp_running = false;
        ESP_LOGI(TAG, "SNTP client stopped.");
    }
}

bool ntp_time_is_synced(void)
{
    return s_time_synced;
}

bool ntp_time_is_valid(void)
{
    return s_time_synced;
}

bool ntp_time_is_network_connected(void)
{
    return s_network_connected;
}

time_t ntp_time_get_persisted_epoch(void)
{
    return s_persisted_last_epoch;
}

time_t ntp_time_get_last_sync_epoch(void)
{
    return s_last_sync_epoch;
}

time_t ntp_time_get_current_epoch(void)
{
    if (!s_time_synced) {
        return 0;
    }
    time_t now;
    time(&now);
    return now;
}

bool ntp_time_get_ist(struct tm *out_tm)
{
    if (!out_tm) {
        return false;
    }

    time_t now;
    time(&now);

    /* Epoch before year 2026 implies system uncalibrated without NTP */
    if (!s_time_synced && now < 1735689600) { /* 2025-01-01 00:00:00 */
        return false;
    }

    localtime_r(&now, out_tm);
    return true;
}

void ntp_time_get_status(bool *out_synced,
                         time_t *out_last_epoch,
                         int16_t *out_tz_offset_min,
                         const char **out_tz_name)
{
    if (out_synced) {
        *out_synced = s_time_synced;
    }
    if (out_last_epoch) {
        *out_last_epoch = s_last_sync_epoch;
    }
    if (out_tz_offset_min) {
        *out_tz_offset_min = IST_UTC_OFFSET_MINUTES;
    }
    if (out_tz_name) {
        *out_tz_name = IST_TZ_NAME;
    }
}
