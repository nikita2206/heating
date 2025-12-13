/*
 * OpenTherm Gateway - Refactored Multi-Thread Architecture
 *
 * Three threads:
 * 1. Thermostat thread - handles communication with thermostat (BLOCKING)
 * 2. Boiler thread - handles communication with boiler (BLOCKING)
 * 3. Main loop (boiler_manager) - coordinates everything (NON-BLOCKING)
 *
 * Inter-thread communication via FreeRTOS queues (size=1).
 */

#include <fcntl.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "opentherm_gateway.h"
#include "ot_queues.h"
#include "ot_thermostat.h"
#include "ot_boiler.h"
#include "boiler_manager.h"
#include "websocket_server.h"
#include "ota_update.h"
#include "mqtt_bridge.h"
#include "sdkconfig.h"

static const char *TAG = "OT_GATEWAY";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static websocket_server_t ws_server;
static boiler_manager_t boiler_mgr;
static mqtt_bridge_state_t mqtt_state;

// Shared queues for inter-thread communication
static ot_queues_t s_queues;

// Thread handles
static ot_thermostat_t *s_thermostat = NULL;
static ot_boiler_t *s_boiler = NULL;

// Getter for boiler manager (for HTTP handlers)
struct boiler_manager* opentherm_gateway_get_boiler_manager(void)
{
    return &boiler_mgr;
}

/* Console initialization for USB Serial JTAG */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
esp_err_t opentherm_gateway_console_init(void)
{
    esp_err_t ret = ESP_OK;
    setvbuf(stdin, NULL, _IONBF, 0);

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ret = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    usb_serial_jtag_vfs_use_driver();
    return ret;
}
#endif

/* WiFi event handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP (attempt %d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize WiFi station */
static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        return ESP_FAIL;
    }
}

/**
 * Message callback - logs all OpenTherm messages to WebSocket
 */
static void opentherm_message_callback(const char *direction, ot_message_source_t source,
                                       uint32_t message, void *user_data)
{
    (void)user_data;

    // Parse message
    uint8_t msg_type = (message >> 28) & 0x7;
    uint8_t data_id = (message >> 16) & 0xFF;
    uint16_t data_value = message & 0xFFFF;

    const char *type_str;
    switch (msg_type) {
        case 0: type_str = "READ_DATA"; break;
        case 1: type_str = "WRITE_DATA"; break;
        case 2: type_str = "INVALID_DATA"; break;
        case 3: type_str = "RESERVED"; break;
        case 4: type_str = "READ_ACK"; break;
        case 5: type_str = "WRITE_ACK"; break;
        case 6: type_str = "DATA_INVALID"; break;
        case 7: type_str = "UNKNOWN_DATAID"; break;
        default: type_str = "UNKNOWN"; break;
    }

    const char *source_str;
    switch (source) {
        case OT_SOURCE_THERMOSTAT_BOILER: source_str = "THERMOSTAT_BOILER"; break;
        case OT_SOURCE_GATEWAY_BOILER: source_str = "GATEWAY_BOILER"; break;
        case OT_SOURCE_THERMOSTAT_GATEWAY: source_str = "THERMOSTAT_GATEWAY"; break;
        default: source_str = "UNKNOWN"; break;
    }

    ESP_LOGI(TAG, "%s | Type: %s | ID: %d | Value: 0x%04X | Source: %s",
             direction, type_str, data_id, data_value, source_str);

    websocket_server_send_opentherm_message(&ws_server, direction, message,
                                            type_str, data_id, data_value, source_str);
}

/**
 * Heartbeat task - sends periodic status updates
 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    ot_stats_t therm_stats, boiler_stats;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        ot_thermostat_get_stats(s_thermostat, &therm_stats);
        ot_boiler_get_stats(s_boiler, &boiler_stats);

        char status_msg[256];
        snprintf(status_msg, sizeof(status_msg),
                 "Gateway status: OK | Uptime: %llu s | Therm RX: %lu TX: %lu | Boiler RX: %lu TX: %lu",
                 (unsigned long long)(esp_timer_get_time() / 1000000),
                 (unsigned long)therm_stats.rx_count, (unsigned long)therm_stats.tx_count,
                 (unsigned long)boiler_stats.rx_count, (unsigned long)boiler_stats.tx_count);

        websocket_server_send_text(&ws_server, status_msg);

        ESP_LOGD(TAG, "Stats: Therm(rx=%lu,tx=%lu,err=%lu,to=%lu) Boiler(rx=%lu,tx=%lu,err=%lu,to=%lu)",
                 (unsigned long)therm_stats.rx_count, (unsigned long)therm_stats.tx_count,
                 (unsigned long)therm_stats.error_count, (unsigned long)therm_stats.timeout_count,
                 (unsigned long)boiler_stats.rx_count, (unsigned long)boiler_stats.tx_count,
                 (unsigned long)boiler_stats.error_count, (unsigned long)boiler_stats.timeout_count);
    }
}

/**
 * Initialize and start the gateway
 */
static void start_gateway(void)
{
    ESP_LOGI(TAG, "Starting OpenTherm gateway (multi-thread architecture)");

    // Create queues (size=1 to avoid buffering stale data)
    s_queues.thermostat_request = xQueueCreate(1, sizeof(ot_msg_t));
    s_queues.thermostat_response = xQueueCreate(1, sizeof(ot_msg_t));
    s_queues.boiler_request = xQueueCreate(1, sizeof(ot_msg_t));
    s_queues.boiler_response = xQueueCreate(1, sizeof(ot_msg_t));

    if (!s_queues.thermostat_request || !s_queues.thermostat_response ||
        !s_queues.boiler_request || !s_queues.boiler_response) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }

    ESP_LOGI(TAG, "Queues created");

    // Start MQTT bridge
    mqtt_bridge_config_t mqtt_cfg;
    mqtt_bridge_load_config(&mqtt_cfg);
    esp_err_t mqtt_ret = mqtt_bridge_start(&mqtt_cfg, &mqtt_state);
    if (mqtt_ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT bridge not started: %s", esp_err_to_name(mqtt_ret));
    }

    // Start WebSocket server
    if (websocket_server_start(&ws_server, &boiler_mgr) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
        return;
    }

    // Register OTA handlers
    httpd_handle_t http_server = websocket_server_get_handle(&ws_server);
    if (http_server) {
        ota_update_register_handlers(http_server);
    }

    ESP_LOGI(TAG, "WebSocket server started");

    // Initialize boiler manager
    uint32_t intercept_rate = 4;
    if (boiler_manager_init(&boiler_mgr, BOILER_MANAGER_MODE_PROXY, NULL, intercept_rate) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize boiler manager");
        return;
    }

    // Set message callback for logging
    boiler_manager_set_message_callback(&boiler_mgr, opentherm_message_callback, NULL);

    // Start thermostat thread
    ot_thermostat_config_t therm_cfg = {
        .rx_pin = OT_MASTER_IN_PIN,
        .tx_pin = OT_MASTER_OUT_PIN,
        .queues = &s_queues,
        .task_stack_size = 4096,
        .task_priority = 6,
    };

    s_thermostat = ot_thermostat_init(&therm_cfg);
    if (!s_thermostat) {
        ESP_LOGE(TAG, "Failed to start thermostat thread");
        return;
    }

    ESP_LOGI(TAG, "Thermostat thread started (RX=GPIO%d, TX=GPIO%d)",
             OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN);

    // Start boiler thread
    ot_boiler_config_t boiler_cfg = {
        .rx_pin = OT_SLAVE_IN_PIN,
        .tx_pin = OT_SLAVE_OUT_PIN,
        .queues = &s_queues,
        .task_stack_size = 4096,
        .task_priority = 6,
    };

    s_boiler = ot_boiler_init(&boiler_cfg);
    if (!s_boiler) {
        ESP_LOGE(TAG, "Failed to start boiler thread");
        return;
    }

    ESP_LOGI(TAG, "Boiler thread started (RX=GPIO%d, TX=GPIO%d)",
             OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);

    // Start boiler manager main loop
    if (boiler_manager_start(&boiler_mgr, &s_queues, 4096, 5) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start boiler manager main loop");
        return;
    }

    ESP_LOGI(TAG, "Main loop started");

    // Start heartbeat task
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "OpenTherm gateway running");
    ESP_LOGI(TAG, "  Thermostat side: RX=GPIO%d, TX=GPIO%d", OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN);
    ESP_LOGI(TAG, "  Boiler side: RX=GPIO%d, TX=GPIO%d", OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);
    ESP_LOGI(TAG, "  Web UI: http://<device-ip>/");
}

void app_main(void)
{
    ESP_LOGI(TAG, "OpenTherm Gateway starting...");
    ESP_LOGI(TAG, "Firmware version: %s", ota_update_get_version());

    // Validate OTA state
    ota_update_validate_app();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    ESP_ERROR_CHECK(opentherm_gateway_console_init());
#endif

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed");
        return;
    }

    // Start the gateway
    start_gateway();

    ESP_LOGI(TAG, "OpenTherm Gateway initialized");
}
