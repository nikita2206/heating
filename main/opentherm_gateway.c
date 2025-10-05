/*
 * OpenTherm Gateway with WiFi and WebSocket Logging
 * 
 * Proxies OpenTherm messages between thermostat and boiler
 * while logging all communication via WebSocket for analysis.
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
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "opentherm_gateway.h"
#include "opentherm.h"
#include "websocket_server.h"

static const char *TAG = "OT_GATEWAY";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static websocket_server_t ws_server;
static OpenTherm ot;

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

/* OpenTherm message callback for logging */
static void opentherm_message_callback(OpenTherm *ot_instance, OpenThermMessage *message, OpenThermRole from_role)
{
    const char *direction = (from_role == OT_ROLE_MASTER) ? "REQUEST" : "RESPONSE";
    OpenThermMessageType msg_type = opentherm_get_message_type(message->data);
    uint8_t data_id = opentherm_get_data_id(message->data);
    uint16_t data_value = opentherm_get_uint16(message->data);
    
    // Log to console
    ESP_LOGI(TAG, "%s | Type: %s | ID: %d | Value: 0x%04X | Raw: 0x%08X",
             direction,
             opentherm_message_type_to_string(msg_type),
             data_id,
             data_value,
             message->data);
    
    // Send to WebSocket clients
    websocket_server_send_opentherm_message(&ws_server,
                                            direction,
                                            message->data,
                                            opentherm_message_type_to_string(msg_type),
                                            data_id,
                                            data_value);
}

/* OpenTherm gateway task - proxies messages between thermostat and boiler */
static void opentherm_gateway_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting OpenTherm gateway task");
    
    // Wait for WiFi connection
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
    
    // Start WebSocket server
    if (websocket_server_start(&ws_server) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "WebSocket server started. Connect to http://<device-ip>/ to view messages");
    
    // Initialize OpenTherm gateway
    opentherm_init_gateway(&ot,
                          OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN,  // Thermostat side
                          OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);    // Boiler side
    
    opentherm_set_message_callback(&ot, opentherm_message_callback, NULL);
    opentherm_start(&ot);
    
    ESP_LOGI(TAG, "OpenTherm gateway initialized and ready");
    ESP_LOGI(TAG, "Thermostat side - IN: GPIO%d, OUT: GPIO%d", OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN);
    ESP_LOGI(TAG, "Boiler side - IN: GPIO%d, OUT: GPIO%d", OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);
    
    OpenThermMessage request, response;
    
    // Main gateway loop
    while (1) {
        // Wait for request from thermostat (master)
        if (opentherm_get_status(&ot) == OT_STATUS_READY) {
            // In a real gateway implementation, we would:
            // 1. Listen for incoming message from thermostat on master interface
            // 2. Forward it to boiler on slave interface
            // 3. Wait for response from boiler
            // 4. Forward response back to thermostat
            // 5. Log both request and response
            
            // For now, this is a simplified placeholder
            // The actual implementation would need proper bi-directional proxying
            // which requires more complex state machine handling
            
            ESP_LOGI(TAG, "Gateway ready - waiting for thermostat messages...");
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "OpenTherm Gateway starting...");
    
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

