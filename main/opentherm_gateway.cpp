/*
 * OpenTherm Gateway - C++ Main Application
 *
 * Multi-thread architecture:
 * 1. Thermostat task - handles communication with thermostat (BLOCKING)
 * 2. Boiler task - handles communication with boiler (BLOCKING)
 * 3. Main loop (BoilerManager) - coordinates everything (NON-BLOCKING)
 */

#include <fcntl.h>
#include <cstring>
#include <memory>

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
#include "OpenTherm.h"
#include "boiler_manager.hpp"
#include "mqtt_bridge.hpp"

// WebSocket server (now C++)
#include "websocket_server.h"

// C header for OTA (still in C)
extern "C" {
#include "ota_update.h"
}

#include "sdkconfig.h"

static const char* TAG = "OT_GATEWAY";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static websocket_server_t ws_server;

// C++ smart pointers for RAII components
static std::unique_ptr<ot::BoilerManager> s_manager;
static std::unique_ptr<ot::MqttBridge> s_mqtt;


#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
extern "C" esp_err_t opentherm_gateway_console_init(void) {
    setvbuf(stdin, nullptr, _IONBF, 0);

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t ret = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    usb_serial_jtag_vfs_use_driver();
    return ret;
}
#endif

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
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
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi station
static esp_err_t wifi_init_sta() {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, nullptr, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, nullptr, &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(wifi_config.sta.password), WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

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

// Message callback - logs all OpenTherm messages to WebSocket
static void opentherm_message_callback(std::string_view direction, ot::MessageSource source,
                                       ot::Frame message) {
    uint8_t data_id = message.dataId();
    uint16_t data_value = message.dataValue();

    const char* type_str = ot::toString(message.messageType());
    const char* source_str = ot::toString(source);

    ESP_LOGI(TAG, "%.*s | Type: %s | ID: %d | Value: 0x%04X | Source: %s",
             static_cast<int>(direction.size()), direction.data(),
             type_str, data_id, data_value, source_str);

    websocket_server_send_opentherm_message(&ws_server,
                                            std::string(direction).c_str(),
                                            message.raw(),
                                            type_str, data_id, data_value, source_str);
}

// Heartbeat task - sends periodic status updates
static void heartbeat_task(void* arg) {
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));


        // if (s_thermostat && s_boiler) {
        //     auto therm_stats = s_thermostat->stats();
        //     auto boiler_stats = s_boiler->stats();

        //     char status_msg[256];
        //     snprintf(status_msg, sizeof(status_msg),
        //              "Gateway status: OK | Uptime: %llu s | Therm RX: %lu TX: %lu | Boiler RX: %lu TX: %lu",
        //              static_cast<unsigned long long>(esp_timer_get_time() / 1000000),
        //              static_cast<unsigned long>(therm_stats.rxCount),
        //              static_cast<unsigned long>(therm_stats.txCount),
        //              static_cast<unsigned long>(boiler_stats.rxCount),
        //              static_cast<unsigned long>(boiler_stats.txCount));

        //     websocket_server_send_text(&ws_server, status_msg);

        //     ESP_LOGD(TAG, "Stats: Therm(rx=%lu,tx=%lu,err=%lu,to=%lu) Boiler(rx=%lu,tx=%lu,err=%lu,to=%lu)",
        //              static_cast<unsigned long>(therm_stats.rxCount),
        //              static_cast<unsigned long>(therm_stats.txCount),
        //              static_cast<unsigned long>(therm_stats.errorCount),
        //              static_cast<unsigned long>(therm_stats.timeoutCount),
        //              static_cast<unsigned long>(boiler_stats.rxCount),
        //              static_cast<unsigned long>(boiler_stats.txCount),
        //              static_cast<unsigned long>(boiler_stats.errorCount),
        //              static_cast<unsigned long>(boiler_stats.timeoutCount));
        // }
    }
}

// Initialize and start the gateway
static void start_gateway() {
    ESP_LOGI(TAG, "Starting OpenTherm gateway (C++ implementation)");

    // Start MQTT bridge
    ot::MqttConfig mqtt_cfg;
    (void)ot::MqttBridge::loadConfig(mqtt_cfg);
    s_mqtt = std::make_unique<ot::MqttBridge>(mqtt_cfg);
    esp_err_t mqtt_ret = s_mqtt->start();
    if (mqtt_ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT bridge not started: %s", esp_err_to_name(mqtt_ret));
    }

    // Initialize boiler manager (before WebSocket so it's available for API calls)
    ot::ManagerConfig mgr_cfg;
    mgr_cfg.mode = ot::ManagerMode::Proxy;
    mgr_cfg.interceptRate = 4;
    mgr_cfg.taskStackSize = 4096;
    mgr_cfg.taskPriority = 5;
    mgr_cfg.thermostatInPin = OT_MASTER_IN_PIN;
    mgr_cfg.thermostatOutPin = OT_MASTER_OUT_PIN;
    mgr_cfg.boilerInPin = OT_SLAVE_IN_PIN;
    mgr_cfg.boilerOutPin = OT_SLAVE_OUT_PIN;

    s_manager = std::make_unique<ot::BoilerManager>(mgr_cfg);

    // Set message callback
    s_manager->setMessageCallback(opentherm_message_callback);

    // Set MQTT bridge for diagnostics publishing
    s_manager->setMqttBridge(s_mqtt.get());

    // Start WebSocket server (pass C++ pointers directly)
    websocket_server_set_mqtt(s_mqtt.get());
    if (websocket_server_start(&ws_server, s_manager.get()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
        return;
    }

    // Register OTA handlers
    httpd_handle_t http_server = websocket_server_get_handle(&ws_server);
    if (http_server) {
        ota_update_register_handlers(http_server);
    }
    ESP_LOGI(TAG, "WebSocket server started");

    // Start boiler manager main loop
    if (s_manager->start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start boiler manager main loop");
        return;
    }
    ESP_LOGI(TAG, "Main loop started");

    // Start heartbeat task
    //xTaskCreate(heartbeat_task, "heartbeat", 2048, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "OpenTherm gateway running");
    ESP_LOGI(TAG, "  Thermostat side: RX=GPIO%d, TX=GPIO%d", OT_MASTER_IN_PIN, OT_MASTER_OUT_PIN);
    ESP_LOGI(TAG, "  Boiler side: RX=GPIO%d, TX=GPIO%d", OT_SLAVE_IN_PIN, OT_SLAVE_OUT_PIN);
    ESP_LOGI(TAG, "  Web UI: http://<device-ip>/");
}

extern "C" void app_main() {
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
