/*
 * OpenTherm Gateway with WiFi and WebSocket Logging
 * 
 * Proxies OpenTherm messages between thermostat and boiler
 * while logging all communication via WebSocket for analysis.
 * 
 * Uses RMT peripheral for precise Manchester encoding/decoding.
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
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "opentherm_gateway.h"
#include "opentherm_rmt.h"
#include "websocket_server.h"
#include "ota_update.h"

static const char *TAG = "OT_GATEWAY";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static websocket_server_t ws_server;
static OpenThermRmt ot;

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
        ESP_LOGI(TAG, "Connect to the AP failed");
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
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
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unexpected WiFi event");
        return ESP_FAIL;
    }
}

/**
 * OpenTherm message callback - invoked for every captured frame
 * 
 * WHY: The gateway state machine calls this callback twice per transaction:
 *   1. When a thermostat REQUEST is captured (from_role = OT_RMT_ROLE_MASTER)
 *   2. When a boiler RESPONSE is captured (from_role = OT_RMT_ROLE_SLAVE)
 * 
 * This provides real-time telemetry of all OpenTherm communication, critical for:
 *   - Debugging heating system issues
 *   - Understanding communication patterns for future smart thermostat implementation
 *   - Monitoring system health (temperatures, modulation levels, error codes)
 * 
 * WHAT: Parses the 32-bit OpenTherm frame and logs it to both:
 *   - Serial console (ESP_LOGI)
 *   - WebSocket clients (for remote monitoring via browser)
 */
static void opentherm_message_callback(OpenThermRmt *ot_instance, OpenThermRmtMessage *message, OpenThermRmtRole from_role)
{
    const char *direction = (from_role == OT_RMT_ROLE_MASTER) ? "REQUEST" : "RESPONSE";
    OpenThermRmtMessageType msg_type = opentherm_rmt_get_message_type(message->data);
    uint8_t data_id = opentherm_rmt_get_data_id(message->data);
    uint16_t data_value = opentherm_rmt_get_uint16(message->data);
    
    // Log to serial console for debugging
    ESP_LOGI(TAG, "%s | Type: %s | ID: %d | Value: 0x%04X | Raw: 0x%08lX",
             direction,
             opentherm_rmt_message_type_to_string(msg_type),
             data_id,
             data_value,
             (unsigned long)message->data);
    
    // Send to WebSocket clients for remote monitoring
    websocket_server_send_opentherm_message(&ws_server,
                                            direction,
                                            message->data,
                                            opentherm_rmt_message_type_to_string(msg_type),
                                            data_id,
                                            data_value);
}

/**
 * OpenTherm gateway task - Main application task for MITM proxying
 * 
 * WHY: This task orchestrates the entire gateway operation:
 *   1. Ensures network connectivity for remote monitoring
 *   2. Drives the gateway state machine at high frequency (1ms loop)
 *   3. Provides telemetry/diagnostics via WebSocket for debugging
 * 
 * The 1ms loop frequency is critical - it allows the state machine to respond
 * quickly to frame completions and maintain OpenTherm timing requirements.
 */
static void opentherm_gateway_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting OpenTherm gateway task (RMT implementation)");
    
    // Wait for WiFi connection before starting (needed for WebSocket logging)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi not connected, cannot start gateway");
        vTaskDelete(NULL);
        return;
    }
    
    // Start WebSocket server for real-time message monitoring
    if (websocket_server_start(&ws_server) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
        vTaskDelete(NULL);
        return;
    }
    
    // Register OTA update handlers with the HTTP server
    httpd_handle_t http_server = websocket_server_get_handle(&ws_server);
    if (http_server) {
        ota_update_register_handlers(http_server);
    }
    
    ESP_LOGI(TAG, "WebSocket server started. Connect to http://<device-ip>/ to view messages");
    ESP_LOGI(TAG, "OTA update available at POST http://<device-ip>/ota");
    
    // Initialize OpenTherm gateway with dual interfaces using RMT
    // Master side connects to thermostat, slave side connects to boiler
    esp_err_t ret = opentherm_rmt_init_gateway(&ot,
                                                OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN,  // Thermostat side
                                                OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);   // Boiler side
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize OpenTherm RMT gateway: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    // Register callback for logging each captured frame (both directions)
    opentherm_rmt_set_message_callback(&ot, opentherm_message_callback, NULL);
    
    // Start the RMT channels
    ret = opentherm_rmt_start(&ot);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OpenTherm RMT: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    opentherm_rmt_gateway_reset(&ot);  // Start in clean IDLE state
    
    ESP_LOGI(TAG, "OpenTherm RMT gateway initialized and ready");
    ESP_LOGI(TAG, "Thermostat side - RX: GPIO%d, TX: GPIO%d", OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN);
    ESP_LOGI(TAG, "Boiler side - RX: GPIO%d, TX: GPIO%d", OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);
    
    OpenThermRmtMessage request, response;
    bool timeout_reported = false;
    const TickType_t loop_delay = pdMS_TO_TICKS(1);  // 1ms - fast enough to handle OpenTherm timing
    
    // Heartbeat counter for periodic status messages (even when idle)
    // WHY: Confirms WebSocket is alive and working during idle periods
    uint32_t heartbeat_counter = 0;
    const uint32_t HEARTBEAT_INTERVAL = 5000;  // Send status every 5 seconds (5000 * 1ms)
    
    // Debug counter for hardware diagnostics
    // WHY: Helps diagnose issues by showing RMT statistics
    uint32_t debug_counter = 0;
    const uint32_t DEBUG_INTERVAL = 10000;  // Log debug info every 10 seconds (10000 * 1ms)
    uint32_t last_tx_count = 0;
    uint32_t last_rx_count = 0;
    uint32_t last_error_count = 0;
    
    // Main gateway loop - drives the state machine continuously
    // WHY 1ms loop: OpenTherm frames take ~34ms to transmit. A 1ms loop ensures
    // we check for frame completion and state transitions frequently enough to
    // meet the 800ms response timeout while keeping CPU usage reasonable.
    while (1) {
        // Process one state machine iteration
        bool proxied = opentherm_rmt_gateway_process(&ot, &request, &response);

        if (proxied) {
            // Complete request->response transaction proxied
            // Validate that response data ID matches request (OpenTherm requirement)
            if (!opentherm_rmt_is_valid_response(request.data, response.data)) {
                ESP_LOGW(TAG, "Gateway forwarded request 0x%08lX but response ID mismatch (0x%08lX)",
                         (unsigned long)request.data, (unsigned long)response.data);
                websocket_server_send_text(&ws_server,
                                           "OT Gateway warning: response data ID mismatch");
            } else {
                ESP_LOGI(TAG, "Gateway proxied request 0x%08lX -> response 0x%08lX",
                         (unsigned long)request.data, (unsigned long)response.data);
            }
        }

        // Monitor and report timeout conditions to remote clients
        // WHY: Timeouts indicate boiler communication issues that need investigation
        if (ot.gateway_timeout_flag && !timeout_reported) {
            timeout_reported = true;
            ESP_LOGW(TAG, "Gateway timeout reported to websocket clients");
            websocket_server_send_text(&ws_server,
                                       "OT Gateway warning: boiler response timeout");
        } else if (!ot.gateway_timeout_flag && timeout_reported) {
            timeout_reported = false;
        }

        // Hardware diagnostics: log RMT statistics periodically
        // WHY: Helps diagnose communication issues by showing TX/RX/error counts
        debug_counter++;
        if (debug_counter >= DEBUG_INTERVAL) {
            debug_counter = 0;
            uint32_t tx_delta = ot.tx_count - last_tx_count;
            uint32_t rx_delta = ot.rx_count - last_rx_count;
            uint32_t error_delta = ot.error_count - last_error_count;
            last_tx_count = ot.tx_count;
            last_rx_count = ot.rx_count;
            last_error_count = ot.error_count;
            
            ESP_LOGI(TAG, "DEBUG: status=%s, state=%d",
                     opentherm_rmt_status_to_string(ot.status), ot.gateway_state);
            ESP_LOGI(TAG, "DEBUG: RMT stats (last 10s): TX=%lu (+%lu), RX=%lu (+%lu), errors=%lu (+%lu), timeouts=%lu",
                     (unsigned long)ot.tx_count, (unsigned long)tx_delta,
                     (unsigned long)ot.rx_count, (unsigned long)rx_delta,
                     (unsigned long)ot.error_count, (unsigned long)error_delta,
                     (unsigned long)ot.timeout_count);
            
            // If no activity detected at all, alert about possible hardware issue
            if (ot.tx_count == 0 && ot.rx_count == 0) {
                ESP_LOGW(TAG, "DEBUG: NO RMT ACTIVITY DETECTED - Check hardware connections!");
                websocket_server_send_text(&ws_server,
                    "WARNING: No RMT activity detected on either interface. Check hardware!");
            } else if (tx_delta == 0 && rx_delta == 0) {
                ESP_LOGW(TAG, "DEBUG: No new TX/RX in last 10s - possible idle period or connection issue");
            }
        }
        
        // Send periodic heartbeat/status messages to confirm WebSocket is alive
        // WHY: When no OpenTherm traffic is happening (e.g., thermostat idle), clients
        // need confirmation that the gateway and WebSocket connection are still working
        heartbeat_counter++;
        if (heartbeat_counter >= HEARTBEAT_INTERVAL) {
            heartbeat_counter = 0;
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg),
                     "Gateway status: %s | Uptime: %llu seconds | TX: %lu | RX: %lu",
                     ot.gateway_timeout_flag ? "TIMEOUT" : "OK",
                     (unsigned long long)(esp_timer_get_time() / 1000000),
                     (unsigned long)ot.tx_count,
                     (unsigned long)ot.rx_count);
            websocket_server_send_text(&ws_server, status_msg);
        }

        vTaskDelay(loop_delay);
    }
    
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "OpenTherm Gateway starting (RMT implementation)...");
    ESP_LOGI(TAG, "Firmware version: %s", ota_update_get_version());
    
    // Validate OTA state early - handles rollback verification
    ota_update_validate_app();
    
    // Initialize NVS for WiFi
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
    
    // Create OpenTherm gateway task
    xTaskCreate(opentherm_gateway_task, 
                "ot_gateway", 
                OT_GATEWAY_TASK_STACK_SIZE, 
                NULL, 
                OT_GATEWAY_TASK_PRIORITY, 
                NULL);
    
    ESP_LOGI(TAG, "OpenTherm Gateway initialized");
}
