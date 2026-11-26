/* =============================================================================
 * PROJECT      : 4-Switch BLE Mesh Node
 * FILE         : main.c
 * PURPOSE      : Application entry point. Orchestrates the boot sequence of
 *                all subsystems (NVS, GPIO, UART, Holtek protocol, relay
 *                manager, and BLE Mesh stack) in the correct order.
 * AUTHOR       : Ganesh Shelke
 * DATE         : 14 July 2026(Version 0.1)
 * Reviewed By     : Bodas Sir
 * TARGET MCU   : ESP32-C3/ESP8685
 * COMPILER     : Xtensa GCC (ESP-IDF v6.0.1 toolchain)
 * DEPENDENCIES : esp_log, board.h, relay_hw.h, mesh_init.h, storage.h,
 *                uart_driver.h, holtek_protocol.h, relay_manager.h,
 *                FreeRTOS
 *
 * DESCRIPTION:
 *   This file contains app_main(), the top-level application entry point
 *   called by the ESP-IDF framework after the system, scheduler, and heap
 *   are initialized. It performs a strictly ordered, staggered initialization
 *   sequence to ensure no power brownout or radio surge occurs between BLE
 *   Mesh and Wi-Fi subsystems.
 *
 *   STAGGERED BOOT ORDER:
 *     1. Early shutdown handler (captures caller PC & backtrace on any restart)
 *     2. NVS Storage (storage_init)
 *     3. Netif & Default Event Loop (initialized ONCE globally)
 *     4. GPIO / LEDs (board_init)
 *     5. UART Driver (uart_init)
 *     6. Holtek Protocol (holtek_protocol_init)
 *     7. Device Role Detection (device_role_init)
 *     8. Relay Manager (relay_manager_init)
 *     9. BLE Mesh Stack (mesh_init - stabilizes without Wi-Fi contention)
 *    10. Master Subsystems (if device_role_is_master):
 *          - Vendor Model Worker (schedule_mesh_vendor_init)
 *          - Scheduler Engine Task (sched_engine_init)
 *          - NTP Time Subsystem (ntp_time_init)
 *          - Wi-Fi Provisioning (wifi_init - 5-press detector active,
 *            Wi-Fi STA autoconnect staggered by CONFIG_WIFI_BOOT_DELAY_MS)
 *
 * =============================================================================
 */

#include "board.h"
#include "device_role.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_rom_sys.h"
#include "esp_debug_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "holtek_protocol.h"
#include "mesh_init.h"
#include "relay_hw.h"
#include "relay_manager.h"
#include "storage.h"
#include "uart_driver.h"
#include "wifi_provisioning.h"
#include "ntp_time.h"
#include "schedule_engine.h"
#include "schedule_mesh_vendor.h"

#define TAG "MAIN"

#if defined(CONFIG_APP_BOOT_DEBUG)
/*
 * Purpose : Registered early in boot to capture caller PC and backtrace
 *           if any component calls esp_restart(). Prints via ROM printf.
 */
static void app_shutdown_handler(void)
{
    esp_rom_printf("\n======================================================\n");
    esp_rom_printf("*** [SHUTDOWN HANDLER TRIGGERED] System restart detected! ***\n");
    esp_rom_printf("*** Call Stack Backtrace (depth 20): ***\n");
    esp_backtrace_print(20);
    esp_rom_printf("*** End of shutdown backtrace ***\n");
    esp_rom_printf("======================================================\n\n");
}
#endif

/*
 * Purpose : Converts esp_reset_reason_t enum into a human-readable string.
 */
static const char *reset_reason_to_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SW_CPU_RESET (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC/EXCEPTION";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT_RESET";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "RESERVED";
    }
}

#define LOG_BOOT_STAGE(stage_name) \
    ESP_LOGI(TAG, "[BOOT_STAGE] %-32s | Free Heap: %7lu B | Min Free: %7lu B", \
             stage_name, \
             (unsigned long)esp_get_free_heap_size(), \
             (unsigned long)esp_get_minimum_free_heap_size())

/*
 * Purpose : Starts all subsystems in strict staggered dependency order.
 * Called  : Automatically by the ESP-IDF framework at startup.
 */
void app_main(void)
{
    esp_err_t err;

#if defined(CONFIG_APP_BOOT_DEBUG)
    esp_register_shutdown_handler(app_shutdown_handler);
#endif

    esp_reset_reason_t rst_reason = esp_reset_reason();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "[MESH] === BLE MESH NODE BOOT DIAGNOSTICS ===");
    ESP_LOGI(TAG, "[MESH] ESP-IDF Version : %s", esp_get_idf_version());
    ESP_LOGI(TAG, "[MESH] Firmware Build  : %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "[MESH] Reset Reason    : %d (%s)", (int)rst_reason, reset_reason_to_name(rst_reason));
    ESP_LOGI(TAG, "[MESH] Boot Baseline   : Free Heap %lu B | Min Free %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "================================================");

    /* 1. Initialize NVS Storage */
    LOG_BOOT_STAGE("1. Before Storage Init");
    err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] NVS initialization failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[MESH] NVS initialization succeeded");
    }
    LOG_BOOT_STAGE("1. After Storage Init");

    /* 2. Initialize Netif & Default Event Loop ONCE early */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
    }
    LOG_BOOT_STAGE("2. After Netif/EventLoop Init");

    /* 3. Initialize GPIO / LEDs */
    board_init();
    LOG_BOOT_STAGE("3. After Board GPIO Init");

    /* 4. Initialize UART Driver */
    err = uart_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART initialization failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "UART Initialized");
    }
    LOG_BOOT_STAGE("4. After UART Init");

    /* 5. Initialize Holtek Protocol */
    holtek_protocol_init();
    LOG_BOOT_STAGE("5. After Holtek Protocol Init");

    /* 6. Device Role Detection */
    device_role_init();
    LOG_BOOT_STAGE("6. After Device Role Detection");

    /* 7. Initialize Relay Manager */
    relay_manager_init();
    LOG_BOOT_STAGE("7. After Relay Manager Init");

    /* Brief delay so RX task logs flush before BLE Mesh stack logs begin */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 8. Initialize BLE Mesh Stack (must run first, without Wi-Fi radio active) */
    LOG_BOOT_STAGE("8. Before BLE Mesh Init");
    err = mesh_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MESH] Bluetooth mesh initialization failed: %s (err %d)",
                 esp_err_to_name(err), err);
    } else {
        ESP_LOGI(TAG, "[MESH] Bluetooth Mesh Stack Ready");
    }
    LOG_BOOT_STAGE("8. After BLE Mesh Init");

    /* 9. Master Node Subsystems (Vendor Model, Scheduler, NTP, Wi-Fi Provisioning) */
    if (device_role_is_master()) {
        ESP_LOGI(TAG, "[MASTER] Initializing Master Node Subsystems...");

#if !defined(CONFIG_VENDOR_MODEL_ENABLE) || CONFIG_VENDOR_MODEL_ENABLE
        schedule_mesh_vendor_init();
        LOG_BOOT_STAGE("9a. After Vendor Model Init");
#else
        ESP_LOGI(TAG, "[MASTER] Vendor Model disabled via CONFIG_VENDOR_MODEL_ENABLE=n");
#endif

#if !defined(CONFIG_SCHED_ENABLE) || CONFIG_SCHED_ENABLE
        sched_engine_init();
        LOG_BOOT_STAGE("9b. After Sched Engine Init");
#else
        ESP_LOGI(TAG, "[MASTER] Scheduler Engine disabled via CONFIG_SCHED_ENABLE=n");
#endif

        ntp_time_init();
        LOG_BOOT_STAGE("9c. After NTP Time Init");

        /* wifi_init() registers 5-press detector and staggers Wi-Fi STA autoconnect by CONFIG_WIFI_BOOT_DELAY_MS */
        wifi_init();
        LOG_BOOT_STAGE("9d. After Wi-Fi Provisioning Init");
    } else {
        ESP_LOGI(TAG, "[DEVICE_ROLE] Normal node operational. Wi-Fi / Master subsystems inactive.");
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "[MESH] Boot sequence completed successfully.");
    LOG_BOOT_STAGE("Final Boot Sequence Ready");
    ESP_LOGI(TAG, "================================================");
}
