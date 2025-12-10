/*
 * Web UI Pages - Declarations
 * Actual content is in web_ui_pages.cpp with compile-time minification
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Dashboard page
extern const char* WEB_UI_DASHBOARD_STYLES;
extern const char* WEB_UI_DASHBOARD_BODY;

// Logs page
extern const char* WEB_UI_LOGS_STYLES;
extern const char* WEB_UI_LOGS_BODY;

// Diagnostics page
extern const char* WEB_UI_DIAGNOSTICS_STYLES;
extern const char* WEB_UI_DIAGNOSTICS_BODY;

// MQTT page
extern const char* WEB_UI_MQTT_STYLES;
extern const char* WEB_UI_MQTT_BODY;

// Manual Write page
extern const char* WEB_UI_WRITE_STYLES;
extern const char* WEB_UI_WRITE_BODY;

// OTA page
extern const char* WEB_UI_OTA_STYLES;
extern const char* WEB_UI_OTA_BODY;

#ifdef __cplusplus
}
#endif
