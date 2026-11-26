/*
 * File    : wifi_provisioning.c
 * Purpose : Wi-Fi provisioning and connection manager for ESP32-C3 Master node.
 * Does    : Implements lazy Wi-Fi start, 5-press detector queue worker, self-contained
 *           embedded HTML/REST provisioning web server, atomic NVS credentials blob with CRC32,
 *           graceful 5s success handover to STA-only mode with modem sleep, and exponential backoff.
 */

#include "wifi_provisioning.h"
#include "five_press_detector.h"
#include "device_role.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define TAG "wifi_prov"

/* Event base definition for Phase 2 integration */
ESP_EVENT_DEFINE_BASE(WIFI_PROV_EVENT);

/* Kconfig defaults fallback if not defined */
#ifndef CONFIG_WIFI_PROV_PRESS_GAP_MS
#define CONFIG_WIFI_PROV_PRESS_GAP_MS 1000
#endif

#ifndef CONFIG_WIFI_PROV_PRESS_WINDOW_MS
#define CONFIG_WIFI_PROV_PRESS_WINDOW_MS 6000
#endif

#ifndef CONFIG_WIFI_PROV_DEBOUNCE_MS
#define CONFIG_WIFI_PROV_DEBOUNCE_MS 40
#endif

#ifndef CONFIG_WIFI_PROV_AP_SSID
#define CONFIG_WIFI_PROV_AP_SSID "lumflux_device"
#endif

#ifndef CONFIG_WIFI_PROV_AP_PASSWORD
#define CONFIG_WIFI_PROV_AP_PASSWORD ""
#endif

#ifndef CONFIG_WIFI_PROV_AP_CHANNEL
#define CONFIG_WIFI_PROV_AP_CHANNEL 1
#endif

#ifndef CONFIG_WIFI_PROV_AP_MAX_CONN
#define CONFIG_WIFI_PROV_AP_MAX_CONN 4
#endif

#ifndef CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC
#define CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC 300
#endif

#ifndef CONFIG_WIFI_PROV_CONNECT_TIMEOUT_SEC
#define CONFIG_WIFI_PROV_CONNECT_TIMEOUT_SEC 20
#endif

#ifndef CONFIG_WIFI_BOOT_DELAY_MS
#define CONFIG_WIFI_BOOT_DELAY_MS 8000
#endif

/* NVS storage constants */
#define LUMFLUX_WIFI_NVS_NAMESPACE "lumflux_wifi"
#define LUMFLUX_WIFI_NVS_KEY       "credentials"
#define LUMFLUX_WIFI_BLOB_VERSION  1

typedef struct __attribute__((packed)) {
    uint8_t version;
    char ssid[33];
    char password[65];
    uint32_t crc32;
} wifi_prov_blob_t;

/* Queue item for switch events */
typedef struct {
    uint8_t switch_idx;
    bool raw_level;
    uint32_t timestamp_ms;
} switch_event_item_t;

/* AP scan cache item */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} scanned_ap_info_t;

#define MAX_SCANNED_APS 24
#define SCAN_CACHE_LIFETIME_US 9000000ULL /* 9 seconds cache */

/* State variables */
static wifi_prov_state_t s_prov_state = WIFI_PROV_STATE_IDLE;
static wifi_prov_fail_reason_t s_fail_reason = WIFI_PROV_FAIL_NONE;
static char s_connected_ip[16] = {0};

static httpd_handle_t s_httpd_server = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static QueueHandle_t s_switch_queue = NULL;
static TaskHandle_t s_worker_task_handle = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;

static five_press_detector_t s_detector;
static esp_timer_handle_t s_idle_timer = NULL;
static esp_timer_handle_t s_connect_timer = NULL;
static esp_timer_handle_t s_backoff_timer = NULL;
static esp_timer_handle_t s_handover_timer = NULL;
static esp_timer_handle_t s_boot_autoconnect_timer = NULL;

static bool s_wifi_inited = false;
static bool s_is_provisioning_mode = false;
static uint32_t s_reconnect_retry_count = 0;

/* Candidate credentials being tested */
static char s_candidate_ssid[33] = {0};
static char s_candidate_pass[65] = {0};

/* Rollback credentials cached in RAM */
static char s_prev_ssid[33] = {0};
static char s_prev_pass[65] = {0};
static bool s_prev_creds_valid = false;

/* Scan cache */
static scanned_ap_info_t s_scan_cache[MAX_SCANNED_APS];
static uint16_t s_scan_cache_count = 0;
static int64_t s_scan_cache_time_us = 0;

/* Forward declarations */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void start_connect_attempt(void);
static void idle_timer_cb(void *arg);
static void connect_timeout_cb(void *arg);
static void backoff_timer_cb(void *arg);
static void handover_timer_cb(void *arg);
static void boot_autoconnect_timer_cb(void *arg);
static esp_err_t ensure_wifi_stack_inited(void);
static esp_err_t start_http_server(void);
static void stop_http_server(void);
static void log_heap_diagnostics(const char *phase_label);

/*
 * Self-contained, responsive mobile-friendly HTML configuration page.
 * Strictly under 6 KB, zero external dependencies, complete UI flow with exact messages.
 */
static const char s_config_html[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Wi-Fi Setup</title><style>"
"*{box-sizing:border-box;margin:0;padding:0}body{font-family:sans-serif;background:#121212;color:#eee;padding:16px}"
".c{max-width:360px;margin:20px auto;background:#1e1e1e;border-radius:10px;padding:18px}"
"h1{font-size:18px;margin-bottom:12px;text-align:center}"
".s{padding:10px;border-radius:6px;font-size:13px;line-height:1.4;margin-bottom:12px;background:#262626;color:#aaa;text-align:center;min-height:36px;display:flex;align-items:center;justify-content:center}"
".err{background:#3d1a1a;color:#ff8585}.ok{background:#16381e;color:#69db7c}"
".h{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;font-size:13px;color:#999}"
".rs{background:#2a2a2a;border:1px solid #444;color:#ddd;padding:3px 8px;border-radius:4px;cursor:pointer;font-size:12px}"
".l{max-height:200px;overflow-y:auto;border:1px solid #333;border-radius:6px;margin-bottom:12px;background:#161616}"
".i{display:flex;justify-content:space-between;align-items:center;padding:10px;border-bottom:1px solid #222;cursor:pointer;font-size:14px}"
".i.sel{background:#183820;border-left:3px solid #00e676}"
".r{display:flex;align-items:center;gap:6px}"
".b{display:flex;align-items:flex-end;gap:2px;height:12px}"
".b span{width:3px;background:#444;border-radius:1px}"
".b .o{background:#00e676}"
".p{margin-bottom:12px;display:none}"
".p label{display:block;font-size:12px;color:#aaa;margin-bottom:4px}"
".w{position:relative}"
".w input{width:100%;padding:9px 40px 9px 9px;background:#262626;border:1px solid #444;border-radius:6px;color:#fff;font-size:14px}"
".w button{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:none;color:#888;cursor:pointer;font-size:12px}"
".btn{width:100%;padding:11px;background:#00e676;color:#000;border:none;border-radius:6px;font-size:15px;font-weight:600;cursor:pointer}"
".btn:disabled{background:#333;color:#666;cursor:not-allowed}"
"</style></head><body><div class='c'><h1>Wi-Fi Setup</h1><div id='st' class='s'>Select your Wi-Fi network.</div>"
"<div class='h'><span>Networks</span><button class='rs' onclick='doScan()'>Refresh</button></div>"
"<div id='nl' class='l'></div>"
"<div id='pwb' class='p'><label>Password</label><div class='w'>"
"<input type='password' id='pw' placeholder='Password' oninput='checkValid()'>"
"<button type='button' id='tb' onclick='togglePw()'>Show</button>"
"</div></div>"
"<button id='btn' class='btn' disabled onclick='doConnect()'>Configure</button></div>"
"<script>"
"var nets=[],sel=null,pollT=null;"
"function $(id){return document.getElementById(id);}"
"function setSt(m,t){var e=$('st');e.textContent=m;e.className='s '+(t||'');}"
"function doScan(){"
"setSt('Scanning for Wi-Fi networks...');$('nl').innerHTML='';"
"fetch('/scan').then(function(r){return r.json();}).then(function(d){nets=d||[];render();})"
".catch(function(){setSt('No Wi-Fi networks found. Tap Refresh to scan again.','err');});"
"}"
"function render(){"
"var el=$('nl');if(!nets.length){el.innerHTML='';setSt('No Wi-Fi networks found. Tap Refresh to scan again.');return;}"
"el.innerHTML=nets.map(function(n,idx){"
"var l=n.auth>0?'&#x1F512;':'';"
"var s=n.rssi>=-55?4:(n.rssi>=-70?3:(n.rssi>=-85?2:1));"
"var b='<div class=\"b\"><span style=\"height:3px\" class=\"'+(s>=1?'o':'')+'\"></span><span style=\"height:6px\" class=\"'+(s>=2?'o':'')+'\"></span><span style=\"height:9px\" class=\"'+(s>=3?'o':'')+'\"></span><span style=\"height:12px\" class=\"'+(s>=4?'o':'')+'\"></span></div>';"
"return '<div class=\"i\" id=\"n'+idx+'\" onclick=\"selNet('+idx+')\"><span>'+n.ssid+'</span><div class=\"r\"><small>'+l+'</small>'+b+'</div></div>';"
"}).join('');"
"if(!sel)setSt('Select your Wi-Fi network.');"
"}"
"function selNet(idx){"
"sel=nets[idx];for(var j=0;j<nets.length;j++){var e=$('n'+j);if(e)e.className='i '+(j===idx?'sel':'');}"
"var pb=$('pwb');if(sel.auth===0){pb.style.display='none';$('pw').value='';}else{pb.style.display='block';}"
"checkValid();"
"}"
"function togglePw(){var p=$('pw'),b=$('tb');if(p.type==='password'){p.type='text';b.textContent='Hide';}else{p.type='password';b.textContent='Show';}}"
"function checkValid(){"
"var b=$('btn');if(!sel){b.disabled=true;return;}"
"if(sel.auth===0){b.disabled=false;return;}"
"var v=$('pw').value;b.disabled=!(v.length>=8&&v.length<=63||v.length===64);"
"}"
"function doConnect(){"
"if(!sel)return;"
"var p=$('pw').value;"
"if(sel.auth!==0&&(p.length<8||p.length>64)){setSt('Password must be 8 to 63 characters.','err');return;}"
"setSt('Connecting to '+sel.ssid+'... Please wait.');$('btn').disabled=true;"
"fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:sel.ssid,password:p})})"
".then(function(r){"
"if(r.status===202||r.status===200||r.status===409){startPoll();}"
"else{setSt('Could not connect. Please try again.','err');$('btn').disabled=false;}"
"}).catch(function(){setSt('Could not connect. Please try again.','err');$('btn').disabled=false;});"
"}"
"function startPoll(){"
"if(pollT)clearInterval(pollT);var f=0;"
"pollT=setInterval(function(){"
"fetch('/status').then(function(r){return r.json();}).then(function(s){"
"f=0;"
"if(s.state==='connected'){"
"clearInterval(pollT);"
"setSt('Connected successfully! Wi-Fi settings are saved. You can now close this page and disconnect from lumflux_device.','ok');"
"}else if(s.state==='failed'){"
"clearInterval(pollT);$('btn').disabled=false;"
"if(s.reason==='wrong_password')setSt('Wrong password. Please check it and try again.','err');"
"else if(s.reason==='network_not_found')setSt('Network not found. Move the device closer to the router and try again.','err');"
"else setSt('Could not connect. Please try again.','err');"
"}"
"}).catch(function(){"
"f++;if(f>=6){clearInterval(pollT);setSt('Setup timed out. Press the first switch 5 times to start again.','err');$('btn').disabled=false;}"
"});"
"},1000);"
"}"
"doScan();"
"</script></body></html>";

/* Helper: Log heap statistics */
static void log_heap_diagnostics(const char *phase_label)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "[HEAP] %s: Free Heap = %lu bytes, Min Free Heap = %lu bytes",
             phase_label, (unsigned long)free_heap, (unsigned long)min_heap);
}

/* Helper: Check and load saved credentials from NVS */
static bool load_nvs_credentials(char *out_ssid, size_t max_ssid, char *out_pass, size_t max_pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LUMFLUX_WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    wifi_prov_blob_t blob;
    size_t required_size = sizeof(blob);
    err = nvs_get_blob(handle, LUMFLUX_WIFI_NVS_KEY, &blob, &required_size);
    nvs_close(handle);

    if (err != ESP_OK || required_size != sizeof(blob)) {
        return false;
    }

    if (blob.version != LUMFLUX_WIFI_BLOB_VERSION) {
        ESP_LOGW(TAG, "Unsupported credentials blob version (%u)", blob.version);
        return false;
    }

    uint32_t computed_crc = esp_rom_crc32_le(0, (const uint8_t *)&blob, sizeof(blob) - sizeof(uint32_t));
    if (computed_crc != blob.crc32) {
        ESP_LOGE(TAG, "Credentials blob CRC mismatch (computed 0x%08lx != stored 0x%08lx)",
                 (unsigned long)computed_crc, (unsigned long)blob.crc32);
        return false;
    }

    if (strlen(blob.ssid) == 0) {
        return false;
    }

    if (out_ssid && max_ssid > 0) {
        strncpy(out_ssid, blob.ssid, max_ssid - 1);
        out_ssid[max_ssid - 1] = '\0';
    }
    if (out_pass && max_pass > 0) {
        strncpy(out_pass, blob.password, max_pass - 1);
        out_pass[max_pass - 1] = '\0';
    }
    return true;
}

/* Helper: Save credentials to NVS atomically in one blob with CRC32 and verify readback */
static bool save_and_verify_credentials(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0) {
        return false;
    }

    wifi_prov_blob_t blob = {0};
    blob.version = LUMFLUX_WIFI_BLOB_VERSION;
    strncpy(blob.ssid, ssid, sizeof(blob.ssid) - 1);
    if (pass) {
        strncpy(blob.password, pass, sizeof(blob.password) - 1);
    }
    blob.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&blob, sizeof(blob) - sizeof(uint32_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LUMFLUX_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", LUMFLUX_WIFI_NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(handle, LUMFLUX_WIFI_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write credentials blob: %s", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }

    /* Read back and verify */
    wifi_prov_blob_t verify_blob = {0};
    size_t size = sizeof(verify_blob);
    err = nvs_get_blob(handle, LUMFLUX_WIFI_NVS_KEY, &verify_blob, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(verify_blob) || memcmp(&blob, &verify_blob, sizeof(blob)) != 0) {
        ESP_LOGE(TAG, "Verification of saved credentials blob failed!");
        return false;
    }

    ESP_LOGI(TAG, "Credentials saved and verified in NVS namespace '%s' for SSID: %s",
             LUMFLUX_WIFI_NVS_NAMESPACE, ssid);
    return true;
}

/* Helper: Parse JSON string field */
static bool parse_json_string(const char *json, const char *key, char *out, size_t out_max)
{
    if (!json || !key || !out || out_max == 0) {
        return false;
    }

    char search_key[48];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *p = strstr(json, search_key);
    if (!p) {
        return false;
    }
    p += strlen(search_key);

    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) {
        p++;
    }

    if (*p != '\"') {
        return false;
    }
    p++; /* skip opening quote */

    size_t idx = 0;
    while (*p && *p != '\"' && idx < out_max - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
        }
        out[idx++] = *p++;
    }
    out[idx] = '\0';
    return (*p == '\"');
}

/* Helper: Escape string for JSON */
static void json_escape(const char *src, char *dst, size_t dst_max)
{
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d < dst_max - 2; s++) {
        if (src[s] == '\"' || src[s] == '\\') {
            if (d < dst_max - 3) {
                dst[d++] = '\\';
                dst[d++] = src[s];
            }
        } else if ((unsigned char)src[s] >= 32 && (unsigned char)src[s] <= 126) {
            dst[d++] = src[s];
        }
    }
    dst[d] = '\0';
}

/* HTTP Handlers */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_config_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int compare_ap_records(const void *a, const void *b)
{
    const scanned_ap_info_t *ap_a = (const scanned_ap_info_t *)a;
    const scanned_ap_info_t *ap_b = (const scanned_ap_info_t *)b;
    return (ap_b->rssi - ap_a->rssi); /* descending order: strongest first */
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    int64_t now_us = esp_timer_get_time();
    bool use_cache = (s_scan_cache_time_us > 0 && (now_us - s_scan_cache_time_us) < SCAN_CACHE_LIFETIME_US);

    if (!use_cache) {
        wifi_scan_config_t scan_cfg = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false
        };

        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err == ESP_OK) {
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);

            if (ap_count > 0) {
                wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
                if (records) {
                    if (esp_wifi_scan_get_ap_records(&ap_count, records) == ESP_OK) {
                        s_scan_cache_count = 0;
                        for (uint16_t i = 0; i < ap_count; i++) {
                            if (records[i].ssid[0] == '\0') {
                                continue; /* Skip empty SSIDs */
                            }
                            /* Check for duplicate in cache */
                            int dup_idx = -1;
                            for (uint16_t j = 0; j < s_scan_cache_count; j++) {
                                if (strncmp(s_scan_cache[j].ssid, (const char *)records[i].ssid, 32) == 0) {
                                    dup_idx = j;
                                    break;
                                }
                            }
                            if (dup_idx >= 0) {
                                if (records[i].rssi > s_scan_cache[dup_idx].rssi) {
                                    s_scan_cache[dup_idx].rssi = records[i].rssi;
                                    s_scan_cache[dup_idx].authmode = records[i].authmode;
                                }
                            } else if (s_scan_cache_count < MAX_SCANNED_APS) {
                                strncpy(s_scan_cache[s_scan_cache_count].ssid, (const char *)records[i].ssid, 32);
                                s_scan_cache[s_scan_cache_count].ssid[32] = '\0';
                                s_scan_cache[s_scan_cache_count].rssi = records[i].rssi;
                                s_scan_cache[s_scan_cache_count].authmode = records[i].authmode;
                                s_scan_cache_count++;
                            }
                        }
                        /* Sort by RSSI descending */
                        qsort(s_scan_cache, s_scan_cache_count, sizeof(scanned_ap_info_t), compare_ap_records);
                        s_scan_cache_time_us = now_us;
                        ESP_LOGI(TAG, "Scan completed: %u unique networks discovered", s_scan_cache_count);
                    }
                    free(records);
                }
            }
        } else {
            ESP_LOGW(TAG, "esp_wifi_scan_start returned error: %s", esp_err_to_name(err));
        }
    }

    /* Build JSON array: [{"ssid":"...","rssi":-40,"auth":3},...] */
    size_t json_size = 64 + (s_scan_cache_count * 80);
    char *json_buf = (char *)malloc(json_size);
    if (!json_buf) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    int pos = snprintf(json_buf, json_size, "[");
    for (uint16_t i = 0; i < s_scan_cache_count && pos < (int)json_size - 60; i++) {
        char esc_ssid[65];
        json_escape(s_scan_cache[i].ssid, esc_ssid, sizeof(esc_ssid));
        pos += snprintf(json_buf + pos, json_size - pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        (i == 0) ? "" : ",",
                        esc_ssid,
                        s_scan_cache[i].rssi,
                        s_scan_cache[i].authmode);
    }
    snprintf(json_buf + pos, json_size - pos, "]");

    httpd_resp_sendstr(req, json_buf);
    free(json_buf);
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (s_prov_state == WIFI_PROV_STATE_CONNECTING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"error\":\"Connection attempt in progress\"}");
        return ESP_OK;
    }

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 256) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Invalid body length\"}");
        return ESP_OK;
    }

    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strstr(content_type, "application/json") == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Content-Type must be application/json\"}");
        return ESP_OK;
    }

    char body[257] = {0};
    int ret = httpd_req_recv(req, body, total_len);
    if (ret <= 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"Failed to receive body\"}");
        return ESP_OK;
    }
    body[ret] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!parse_json_string(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"Missing or invalid ssid field\"}");
        return ESP_OK;
    }
    parse_json_string(body, "password", pass, sizeof(pass));

    /* Validate SSID: 1-32 bytes */
    size_t ssid_len = strlen(ssid);
    if (ssid_len < 1 || ssid_len > 32) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"SSID length must be 1 to 32 characters\"}");
        return ESP_OK;
    }

    /* Validate SSID is present in last scan result */
    int found_idx = -1;
    for (uint16_t i = 0; i < s_scan_cache_count; i++) {
        if (strcmp(s_scan_cache[i].ssid, ssid) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":\"SSID not found in last scan results\"}");
        return ESP_OK;
    }

    /* Validate Password: empty only for open networks, otherwise 8-63 chars or 64 hex */
    size_t pass_len = strlen(pass);
    if (s_scan_cache[found_idx].authmode == WIFI_AUTH_OPEN) {
        pass[0] = '\0';
    } else {
        if (pass_len < 8 || pass_len > 64) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\":\"Password must be 8 to 63 characters\"}");
            return ESP_OK;
        }
    }

    /* Accept connection attempt */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(s_candidate_ssid, ssid, sizeof(s_candidate_ssid) - 1);
    strncpy(s_candidate_pass, pass, sizeof(s_candidate_pass) - 1);
    s_prov_state = WIFI_PROV_STATE_CONNECTING;
    s_fail_reason = WIFI_PROV_FAIL_NONE;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "Connection attempt initiated for SSID: %s", ssid);

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "{\"status\":\"connecting\"}");

    /* Trigger background connection attempt */
    start_connect_attempt();
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    const char *state_str = "idle";
    const char *reason_str = "other";

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    switch (s_prov_state) {
    case WIFI_PROV_STATE_IDLE:
        state_str = "idle";
        break;
    case WIFI_PROV_STATE_PROVISIONING:
        state_str = "idle";
        break;
    case WIFI_PROV_STATE_CONNECTING:
        state_str = "connecting";
        break;
    case WIFI_PROV_STATE_CONNECTED:
        state_str = "connected";
        break;
    case WIFI_PROV_STATE_FAILED:
        state_str = "failed";
        switch (s_fail_reason) {
        case WIFI_PROV_FAIL_WRONG_PASSWORD:
            reason_str = "wrong_password";
            break;
        case WIFI_PROV_FAIL_NETWORK_NOT_FOUND:
            reason_str = "network_not_found";
            break;
        case WIFI_PROV_FAIL_TIMEOUT:
            reason_str = "timeout";
            break;
        default:
            reason_str = "other";
            break;
        }
        break;
    }

    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"reason\":\"%s\",\"ip\":\"%s\"}",
             state_str, reason_str, s_connected_ip);
    xSemaphoreGive(s_state_mutex);

    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* HTTP Web Server Init & Teardown */
static esp_err_t start_http_server(void)
{
    if (s_httpd_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    config.server_port = 80;

    esp_err_t ret = httpd_start(&s_httpd_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd_server, &root_uri);

    httpd_uri_t scan_uri = {
        .uri      = "/scan",
        .method   = HTTP_GET,
        .handler  = scan_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd_server, &scan_uri);

    httpd_uri_t connect_uri = {
        .uri      = "/connect",
        .method   = HTTP_POST,
        .handler  = connect_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd_server, &connect_uri);

    httpd_uri_t status_uri = {
        .uri      = "/status",
        .method   = HTTP_GET,
        .handler  = status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd_server, &status_uri);

    ESP_LOGI(TAG, "HTTP web server active on port %d", config.server_port);
    return ESP_OK;
}

static void stop_http_server(void)
{
    if (s_httpd_server) {
        httpd_stop(s_httpd_server);
        s_httpd_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

/* Timer Callbacks */
static void idle_timer_cb(void *arg)
{
    ESP_LOGW(TAG, "Provisioning idle timeout (%d s) reached", CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC);
    wifi_prov_stop_provisioning();
}

static void connect_timeout_cb(void *arg)
{
    ESP_LOGW(TAG, "Connection timeout (%d s) reached", CONFIG_WIFI_PROV_CONNECT_TIMEOUT_SEC);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_prov_state == WIFI_PROV_STATE_CONNECTING) {
        s_prov_state = WIFI_PROV_STATE_FAILED;
        s_fail_reason = WIFI_PROV_FAIL_TIMEOUT;
    }
    xSemaphoreGive(s_state_mutex);

    esp_wifi_disconnect();

    /* Restart idle timer to keep AP active for user's next try */
    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
        esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC * 1000000ULL);
    }
}

static void backoff_timer_cb(void *arg)
{
    if (s_prov_state != WIFI_PROV_STATE_CONNECTED && !s_is_provisioning_mode) {
        if (wifi_credentials_exist()) {
            ESP_LOGI(TAG, "Attempting Wi-Fi reconnection (retry #%lu)...", (unsigned long)s_reconnect_retry_count);
            esp_wifi_connect();
        }
    }
}

static void handover_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "5-second handover delay elapsed. Stopping SoftAP and HTTP server...");
    stop_http_server();

    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
    }

    /* Switch Wi-Fi mode to STA only */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_is_provisioning_mode = false;

    ESP_LOGI(TAG, "Switched to STA-only mode with MIN_MODEM power save");
    log_heap_diagnostics("After STA-only handover");

    esp_event_post(WIFI_PROV_EVENT, WIFI_PROV_EVENT_PROV_STOPPED, NULL, 0, portMAX_DELAY);
}

static void boot_autoconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_is_provisioning_mode) {
        ESP_LOGI(TAG, "[AUTOCONN] SoftAP provisioning active; skipping delayed autoconnect.");
        return;
    }
    if (s_prov_state == WIFI_PROV_STATE_CONNECTED || s_prov_state == WIFI_PROV_STATE_CONNECTING) {
        ESP_LOGI(TAG, "[AUTOCONN] Already connected/connecting; skipping.");
        return;
    }
    if (!s_prev_creds_valid) {
        return;
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "[AUTOCONN] Boot delay elapsed (%d ms). Initializing Wi-Fi STA connection to '%s'...",
             CONFIG_WIFI_BOOT_DELAY_MS, s_prev_ssid);
    log_heap_diagnostics("Before delayed Wi-Fi autoconnect start");

    ensure_wifi_stack_inited();

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, s_prev_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_prev_pass, sizeof(sta_cfg.sta.password) - 1);
    if (strlen(s_prev_pass) > 0) {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_set_max_tx_power(44);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_err_t start_err = esp_wifi_start();
    ESP_LOGI(TAG, "[AUTOCONN] esp_wifi_start() returned %s", esp_err_to_name(start_err));
    log_heap_diagnostics("After delayed Wi-Fi autoconnect start");
    ESP_LOGI(TAG, "================================================");
}

/* Wi-Fi & IP Event Handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA started");
            if (!s_is_provisioning_mode && wifi_credentials_exist()) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            uint8_t reason = disc->reason;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            bool was_connecting = (s_prov_state == WIFI_PROV_STATE_CONNECTING);
            bool was_connected = (s_prov_state == WIFI_PROV_STATE_CONNECTED);

            if (was_connecting) {
                if (s_connect_timer) {
                    esp_timer_stop(s_connect_timer);
                }
                s_prov_state = WIFI_PROV_STATE_FAILED;

                /* Map disconnect reason */
                if (reason == WIFI_REASON_AUTH_FAIL ||
                    reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                    reason == WIFI_REASON_MIC_FAILURE) {
                    s_fail_reason = WIFI_PROV_FAIL_WRONG_PASSWORD;
                    ESP_LOGW(TAG, "Connection failed: wrong password (reason %u)", reason);
                } else if (reason == WIFI_REASON_NO_AP_FOUND) {
                    s_fail_reason = WIFI_PROV_FAIL_NETWORK_NOT_FOUND;
                    ESP_LOGW(TAG, "Connection failed: network not found (reason %u)", reason);
                } else {
                    s_fail_reason = WIFI_PROV_FAIL_OTHER;
                    ESP_LOGW(TAG, "Connection failed: other reason (%u)", reason);
                }

                /* Rollback candidate, keep old credentials */
                if (s_prev_creds_valid) {
                    wifi_config_t prev_cfg = {0};
                    strncpy((char *)prev_cfg.sta.ssid, s_prev_ssid, sizeof(prev_cfg.sta.ssid) - 1);
                    strncpy((char *)prev_cfg.sta.password, s_prev_pass, sizeof(prev_cfg.sta.password) - 1);
                    esp_wifi_set_config(WIFI_IF_STA, &prev_cfg);
                }

                /* Restart idle timer so AP stays open for another try */
                if (s_idle_timer) {
                    esp_timer_stop(s_idle_timer);
                    esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC * 1000000ULL);
                }
            } else if (was_connected) {
                s_prov_state = WIFI_PROV_STATE_IDLE;
                s_connected_ip[0] = '\0';
                esp_event_post(WIFI_PROV_EVENT, WIFI_PROV_EVENT_LOST_IP, NULL, 0, 0);
            }
            xSemaphoreGive(s_state_mutex);

            /* In normal STA mode (not in provisioning connection), apply exponential backoff */
            if (!s_is_provisioning_mode && wifi_credentials_exist()) {
                uint32_t delay_ms = 1000 * (1 << (s_reconnect_retry_count < 6 ? s_reconnect_retry_count : 6));
                if (delay_ms > 60000) {
                    delay_ms = 60000;
                }
                s_reconnect_retry_count++;
                ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u). Reconnecting in %lu ms...", reason, (unsigned long)delay_ms);
                if (s_backoff_timer) {
                    esp_timer_stop(s_backoff_timer);
                    esp_timer_start_once(s_backoff_timer, (uint64_t)delay_ms * 1000ULL);
                }
            }
            break;
        }

        case WIFI_EVENT_AP_START: {
            wifi_country_t cur_country = {0};
            esp_wifi_get_country(&cur_country);
            uint8_t primary_ch = 0;
            wifi_second_chan_t second_ch = 0;
            esp_wifi_get_channel(&primary_ch, &second_ch);

            esp_netif_dhcp_status_t dhcp_st = 0;
            if (s_ap_netif) {
                esp_netif_dhcps_get_status(s_ap_netif, &dhcp_st);
            }

            ESP_LOGI(TAG, ">>> [WIFI_EVENT: AP_START] SoftAP '%s' active! Country: '%.2s', Channel: %u, DHCP status: %d <<<",
                     CONFIG_WIFI_PROV_AP_SSID, cur_country.cc, primary_ch, (int)dhcp_st);
            ESP_LOGI(TAG, "[AP_START] Heap free: %lu, Min free: %lu",
                     (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ap_conn = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "[WIFI_EVENT: AP_STACONNECTED] Client joined SoftAP (AID: %d, MAC: %02x:%02x:%02x:%02x:%02x:%02x) | Heap free: %lu, Min free: %lu",
                     ap_conn->aid, ap_conn->mac[0], ap_conn->mac[1], ap_conn->mac[2],
                     ap_conn->mac[3], ap_conn->mac[4], ap_conn->mac[5],
                     (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
            /* Reset idle timer on client connection activity */
            if (s_idle_timer) {
                esp_timer_stop(s_idle_timer);
                esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC * 1000000ULL);
            }
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ap_disconn = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "[WIFI_EVENT: AP_STADISCONNECTED] Client left SoftAP (AID: %d, MAC: %02x:%02x:%02x:%02x:%02x:%02x, Reason: %d) | Heap free: %lu, Min free: %lu",
                     ap_disconn->aid, ap_disconn->mac[0], ap_disconn->mac[1], ap_disconn->mac[2],
                     ap_disconn->mac[3], ap_disconn->mac[4], ap_disconn->mac[5], ap_disconn->reason,
                     (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
            break;
        }

        default:
            ESP_LOGI(TAG, "[WIFI_EVENT] Received event_id: %ld", (long)event_id);
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            s_reconnect_retry_count = 0;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            snprintf(s_connected_ip, sizeof(s_connected_ip), IPSTR, IP2STR(&event->ip_info.ip));
            s_prov_state = WIFI_PROV_STATE_CONNECTED;
            s_fail_reason = WIFI_PROV_FAIL_NONE;

            if (s_connect_timer) {
                esp_timer_stop(s_connect_timer);
            }

            ESP_LOGI(TAG, "Got IP: %s", s_connected_ip);

            /* If in provisioning mode, save candidate credentials atomically to NVS */
            if (s_is_provisioning_mode && strlen(s_candidate_ssid) > 0) {
                if (save_and_verify_credentials(s_candidate_ssid, s_candidate_pass)) {
                    /* Update cached previous credentials in RAM */
                    strncpy(s_prev_ssid, s_candidate_ssid, sizeof(s_prev_ssid) - 1);
                    strncpy(s_prev_pass, s_candidate_pass, sizeof(s_prev_pass) - 1);
                    s_prev_creds_valid = true;

                    /* Schedule 5-second graceful handover */
                    if (s_handover_timer) {
                        esp_timer_stop(s_handover_timer);
                        esp_timer_start_once(s_handover_timer, 5000000ULL);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to commit and verify credentials in NVS!");
                }
            }
            xSemaphoreGive(s_state_mutex);

            esp_event_post(WIFI_PROV_EVENT, WIFI_PROV_EVENT_GOT_IP, &event->ip_info, sizeof(esp_netif_ip_info_t), 0);
        }
    }
}

/* Connect Attempt Trigger */
static void start_connect_attempt(void)
{
    if (s_connect_timer) {
        esp_timer_stop(s_connect_timer);
        esp_timer_start_once(s_connect_timer, (uint64_t)CONFIG_WIFI_PROV_CONNECT_TIMEOUT_SEC * 1000000ULL);
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, s_candidate_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, s_candidate_pass, sizeof(sta_cfg.sta.password) - 1);
    if (strlen(s_candidate_pass) > 0) {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();
}

/* Macro for logging each initialization step with return code and heap before/after */
#define LOG_PROV_STEP(name, cmd) do { \
    size_t _hb = esp_get_free_heap_size(), _mb = esp_get_minimum_free_heap_size(); \
    esp_err_t _err = (cmd); \
    size_t _ha = esp_get_free_heap_size(), _ma = esp_get_minimum_free_heap_size(); \
    ESP_LOGI(TAG, "[STEP: %s] result: %s (0x%x) | Heap free: %lu -> %lu (min: %lu -> %lu)", \
             name, esp_err_to_name(_err), _err, \
             (unsigned long)_hb, (unsigned long)_ha, (unsigned long)_mb, (unsigned long)_ma); \
    if (_err != ESP_OK && _err != ESP_ERR_INVALID_STATE) { \
        ESP_LOGE(TAG, "[STEP: %s] Failed: %s", name, esp_err_to_name(_err)); \
    } \
} while(0)

/* Lazy Wi-Fi Initializer (called only when needed) */
static esp_err_t ensure_wifi_stack_inited(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "[INIT_STACK] Initializing netif, event loop, and Wi-Fi driver...");
    log_heap_diagnostics("Before Wi-Fi stack init");

    LOG_PROV_STEP("esp_netif_init", esp_netif_init());
    LOG_PROV_STEP("esp_event_loop_create_default", esp_event_loop_create_default());

    /* CRITICAL FIX: Both STA and AP default netifs MUST be created BEFORE esp_wifi_init(&cfg) */
    if (!s_sta_netif) {
        size_t hb = esp_get_free_heap_size(), mb = esp_get_minimum_free_heap_size();
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_LOGI(TAG, "[STEP: create_default_wifi_sta] ptr: %p | Heap free: %lu -> %lu (min: %lu -> %lu)",
                 s_sta_netif, (unsigned long)hb, (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)mb, (unsigned long)esp_get_minimum_free_heap_size());
    }

    if (!s_ap_netif) {
        size_t hb = esp_get_free_heap_size(), mb = esp_get_minimum_free_heap_size();
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_LOGI(TAG, "[STEP: create_default_wifi_ap] ptr: %p | Heap free: %lu -> %lu (min: %lu -> %lu)",
                 s_ap_netif, (unsigned long)hb, (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)mb, (unsigned long)esp_get_minimum_free_heap_size());
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    LOG_PROV_STEP("esp_wifi_init", esp_wifi_init(&cfg));
    LOG_PROV_STEP("esp_wifi_set_max_tx_power(44)", esp_wifi_set_max_tx_power(44));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    s_wifi_inited = true;
    return ESP_OK;
}

/* Background Worker Task: reads switch event queue and feeds detector */
static void wifi_prov_worker_task(void *pvParameters)
{
    (void)pvParameters;
    switch_event_item_t item;
    ESP_LOGI(TAG, "Wi-Fi provisioning worker task active (Priority 3, stack 4096)");

    while (1) {
        if (xQueueReceive(s_switch_queue, &item, portMAX_DELAY) == pdTRUE) {
            five_press_result_t res = {0};
            bool triggered = five_press_detector_feed_verbose(&s_detector, item.switch_idx, item.timestamp_ms, &res);

            ESP_LOGI(TAG, "[SWITCH_EVT] Sw#%u | Level: %d (%s) | Time: %lu ms | Count: %u -> %u | Decision: %s | Gap: %lu ms | Window: %lu ms",
                     item.switch_idx + 1,
                     item.raw_level ? 1 : 0,
                     item.raw_level ? "HIGH/ON" : "LOW/OFF",
                     (unsigned long)item.timestamp_ms,
                     res.count_before,
                     res.count_after,
                     res.reason_str,
                     (unsigned long)res.gap_ms,
                     (unsigned long)res.window_ms);

            if (triggered) {
                ESP_LOGI(TAG, ">>> [DETECTOR] TRIGGER! 5 presses confirmed on switch #1! <<<");

                if (s_is_provisioning_mode) {
                    ESP_LOGI(TAG, "Already in provisioning mode; restarting idle timer (%d s)", CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC);
                    if (s_idle_timer) {
                        esp_timer_stop(s_idle_timer);
                        esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC * 1000000ULL);
                    }
                } else {
                    wifi_prov_start_provisioning();
                }
            }
        }
    }
}

#if defined(CONFIG_WIFI_PROV_SWITCH_GPIO) && (CONFIG_WIFI_PROV_SWITCH_GPIO >= 0)
static void IRAM_ATTR gpio_switch_isr_handler(void *arg)
{
    int gpio_num = (int)(intptr_t)arg;
    int level = gpio_get_level(gpio_num);
    wifi_prov_post_switch_event(0, level != 0);
}
#endif

/* Hook called from relay_manager or GPIO ISR */
void wifi_prov_post_switch_event(uint8_t switch_idx, bool state)
{
    if (!device_role_is_master()) {
        return;
    }

    if (!s_switch_queue) {
        return;
    }

    switch_event_item_t item = {
        .switch_idx = switch_idx,
        .raw_level = state,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };

    if (xPortInIsrContext()) {
        BaseType_t high_task_wakeup = pdFALSE;
        xQueueSendFromISR(s_switch_queue, &item, &high_task_wakeup);
        if (high_task_wakeup) {
            portYIELD_FROM_ISR();
        }
    } else {
        xQueueSend(s_switch_queue, &item, 0);
    }
}

/* Start SoftAP Provisioning Mode */
void wifi_prov_start_provisioning(void)
{
    if (!device_role_is_master()) {
        ESP_LOGI(TAG, "Device is NORMAL_NODE. Provisioning disabled.");
        return;
    }

    ESP_LOGI(TAG, "================ [WIFI_PROV] STARTING PROVISIONING MODE ================");
    ESP_LOGI(TAG, "[PROV_START] Baseline Free heap: %lu, Min free: %lu",
             (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());

    if (s_boot_autoconnect_timer) {
        esp_timer_stop(s_boot_autoconnect_timer);
    }

    if (ensure_wifi_stack_inited() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi stack for provisioning!");
        return;
    }

    LOG_PROV_STEP("esp_wifi_set_mode(APSTA)", esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    LOG_PROV_STEP("esp_wifi_set_country('01', ch 1-13)", esp_wifi_set_country(&country));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = CONFIG_WIFI_PROV_AP_SSID,
            .ssid_len = strlen(CONFIG_WIFI_PROV_AP_SSID),
            .channel = CONFIG_WIFI_PROV_AP_CHANNEL,
            .max_connection = CONFIG_WIFI_PROV_AP_MAX_CONN,
            .ssid_hidden = 0,
            .beacon_interval = 100,
        },
    };

    const char *ap_pass = CONFIG_WIFI_PROV_AP_PASSWORD;
    if (strlen(ap_pass) > 0) {
        strncpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.password[0] = '\0';
    }

    ESP_LOGI(TAG, "[AP_CONFIG] SSID: '%s' (len: %u), Channel: %d, Authmode: %s (%d), MaxConn: %d",
             (char *)ap_cfg.ap.ssid, (unsigned int)ap_cfg.ap.ssid_len, ap_cfg.ap.channel,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2_PSK",
             ap_cfg.ap.authmode, ap_cfg.ap.max_connection);

    LOG_PROV_STEP("esp_wifi_set_config(AP)", esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    LOG_PROV_STEP("esp_wifi_set_max_tx_power(44)", esp_wifi_set_max_tx_power(44));
    LOG_PROV_STEP("esp_wifi_start()", esp_wifi_start());

    s_is_provisioning_mode = true;
    s_prov_state = WIFI_PROV_STATE_PROVISIONING;
    s_fail_reason = WIFI_PROV_FAIL_NONE;

    LOG_PROV_STEP("start_http_server()", start_http_server());

    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
        esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_WIFI_PROV_IDLE_TIMEOUT_SEC * 1000000ULL);
    }

    ESP_LOGI(TAG, "SoftAP provisioning started: SSID='%s' Channel=%d Auth=%d",
             CONFIG_WIFI_PROV_AP_SSID, CONFIG_WIFI_PROV_AP_CHANNEL, ap_cfg.ap.authmode);
    log_heap_diagnostics("During provisioning (SoftAP + HTTP active)");

    esp_event_post(WIFI_PROV_EVENT, WIFI_PROV_EVENT_PROV_STARTED, NULL, 0, portMAX_DELAY);
}

/* Stop Provisioning Mode & Revert */
void wifi_prov_stop_provisioning(void)
{
    if (!s_is_provisioning_mode) {
        return;
    }

    stop_http_server();

    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
    }
    if (s_connect_timer) {
        esp_timer_stop(s_connect_timer);
    }

    s_is_provisioning_mode = false;

    if (wifi_credentials_exist()) {
        ESP_LOGI(TAG, "Returning to saved STA mode");
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_max_tx_power(44);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_reconnect_retry_count = 0;
        esp_wifi_connect();
        log_heap_diagnostics("After revert to STA mode");
    } else {
        ESP_LOGI(TAG, "No credentials saved. Turning Wi-Fi stack off completely.");
        esp_wifi_stop();
        s_prov_state = WIFI_PROV_STATE_IDLE;
        log_heap_diagnostics("After Wi-Fi fully off");
    }

    esp_event_post(WIFI_PROV_EVENT, WIFI_PROV_EVENT_PROV_STOPPED, NULL, 0, portMAX_DELAY);
}

/* Primary entry point called at boot by app_main() */
void wifi_init(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "[BOOT] wifi_init() executing on MASTER node");
    ESP_LOGI(TAG, "[BOOT] Device role: %s (is_master=%d)",
             device_role_is_master() ? "MASTER" : "NORMAL_NODE", device_role_is_master());
    ESP_LOGI(TAG, "[BOOT] Reset reason: %d", esp_reset_reason());
    ESP_LOGI(TAG, "[BOOT] Initial Wi-Fi state: %d (IDLE)", s_prov_state);

    if (!device_role_is_master()) {
        ESP_LOGI(TAG, "Device role is NORMAL_NODE. Wi-Fi stack disabled.");
        return;
    }

    s_state_mutex = xSemaphoreCreateMutex();

    /* Initialize pure five-press detector */
    five_press_detector_init(&s_detector,
                             CONFIG_WIFI_PROV_PRESS_GAP_MS,
                             CONFIG_WIFI_PROV_PRESS_WINDOW_MS,
                             CONFIG_WIFI_PROV_DEBOUNCE_MS);

    /* Create switch events queue */
    s_switch_queue = xQueueCreate(16, sizeof(switch_event_item_t));

    /* Create provisioning worker task at priority 3 (stack 4096) */
    xTaskCreate(wifi_prov_worker_task, "wifi_prov_task", 4096, NULL, 3, &s_worker_task_handle);

#if defined(CONFIG_WIFI_PROV_SWITCH_GPIO) && (CONFIG_WIFI_PROV_SWITCH_GPIO >= 0)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_WIFI_PROV_SWITCH_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t gpio_err = gpio_config(&io_conf);
    if (gpio_err == ESP_OK) {
        gpio_install_isr_service(0);
        gpio_isr_handler_add(CONFIG_WIFI_PROV_SWITCH_GPIO, gpio_switch_isr_handler, (void *)(intptr_t)CONFIG_WIFI_PROV_SWITCH_GPIO);
        ESP_LOGI(TAG, "[BOOT] Direct switch #1 GPIO %d configured (ANYEDGE, pull-up)", CONFIG_WIFI_PROV_SWITCH_GPIO);
    } else {
        ESP_LOGW(TAG, "[BOOT] Failed to configure direct switch GPIO %d: %s", CONFIG_WIFI_PROV_SWITCH_GPIO, esp_err_to_name(gpio_err));
    }
#endif

    /* Create timers */
    esp_timer_create_args_t idle_args = { .callback = &idle_timer_cb, .name = "wifi_prov_idle" };
    esp_timer_create(&idle_args, &s_idle_timer);

    esp_timer_create_args_t conn_args = { .callback = &connect_timeout_cb, .name = "wifi_prov_conn" };
    esp_timer_create(&conn_args, &s_connect_timer);

    esp_timer_create_args_t backoff_args = { .callback = &backoff_timer_cb, .name = "wifi_prov_backoff" };
    esp_timer_create(&backoff_args, &s_backoff_timer);

    esp_timer_create_args_t handover_args = { .callback = &handover_timer_cb, .name = "wifi_prov_handover" };
    esp_timer_create(&handover_args, &s_handover_timer);

    /* Cache existing NVS credentials in RAM */
    s_prev_creds_valid = load_nvs_credentials(s_prev_ssid, sizeof(s_prev_ssid), s_prev_pass, sizeof(s_prev_pass));
    ESP_LOGI(TAG, "[BOOT] Saved credentials found in NVS: %s", s_prev_creds_valid ? "YES" : "NO");

    log_heap_diagnostics("Boot baseline (BLE Mesh running, Wi-Fi off)");
    ESP_LOGI(TAG, "================================================");

#if defined(CONFIG_WIFI_PROV_FORCE_START) && CONFIG_WIFI_PROV_FORCE_START
    ESP_LOGW(TAG, ">>> CONFIG_WIFI_PROV_FORCE_START IS ENABLED! Starting SoftAP provisioning mode at boot (split test)! <<<");
    wifi_prov_start_provisioning();
#else
    /* Staggered Wi-Fi autoconnect: Wi-Fi stack remains OFF until boot delay elapses */
    if (s_prev_creds_valid) {
#if defined(CONFIG_WIFI_AUTOSTART) && !CONFIG_WIFI_AUTOSTART
        ESP_LOGI(TAG, "[BOOT] Saved credentials exist, but CONFIG_WIFI_AUTOSTART is disabled. Wi-Fi stack remains OFF.");
#else
        ESP_LOGI(TAG, "[BOOT] Saved credentials found in NVS ('%s'). Staggering Wi-Fi autoconnect by %d ms to let BLE Mesh stabilize...",
                 s_prev_ssid, CONFIG_WIFI_BOOT_DELAY_MS);
        if (!s_boot_autoconnect_timer) {
            esp_timer_create_args_t autoconnect_args = {
                .callback = &boot_autoconnect_timer_cb,
                .name = "wifi_autoconn_delay"
            };
            esp_timer_create(&autoconnect_args, &s_boot_autoconnect_timer);
        }
        if (s_boot_autoconnect_timer) {
            esp_timer_stop(s_boot_autoconnect_timer);
            esp_timer_start_once(s_boot_autoconnect_timer, (uint64_t)CONFIG_WIFI_BOOT_DELAY_MS * 1000ULL);
        }
#endif
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials found. Wi-Fi stack remains OFF (awaiting 5-press trigger).");
    }
#endif
}

/* Status query APIs */
bool wifi_prov_is_connected(void)
{
    return (s_prov_state == WIFI_PROV_STATE_CONNECTED);
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    return s_prov_state;
}

wifi_prov_fail_reason_t wifi_prov_get_fail_reason(void)
{
    return s_fail_reason;
}

bool wifi_credentials_exist(void)
{
    return load_nvs_credentials(NULL, 0, NULL, 0);
}

void clear_wifi_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LUMFLUX_WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, LUMFLUX_WIFI_NVS_KEY);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Credentials erased from NVS namespace '%s'", LUMFLUX_WIFI_NVS_NAMESPACE);
    }
    s_prev_creds_valid = false;
    s_prev_ssid[0] = '\0';
    s_prev_pass[0] = '\0';
}

/* Legacy API compatibility */
void wifi_init_sta(void)
{
    ensure_wifi_stack_inited();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

void wifi_init_provisioning(void)
{
    wifi_prov_start_provisioning();
}

bool wifi_is_connected(void)
{
    return wifi_prov_is_connected();
}
