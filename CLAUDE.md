# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
idf.py menuconfig    # Configure WiFi, MQTT, GPIO pins
idf.py build         # Build firmware
idf.py flash         # Flash to device
idf.py monitor       # Monitor serial output
idf.py flash monitor # Build, flash, and monitor in one step
idf.py fullclean     # Clean build artifacts
```

Requires ESP-IDF v5.0+ with `IDF_PATH` environment variable set.

## Project Overview

ESP-IDF OpenTherm Gateway that acts as a MITM proxy between a thermostat and boiler. Intercepts all OpenTherm protocol messages, provides real-time WebSocket monitoring, MQTT integration, and OTA updates.

## Architecture

```
┌─────────────────────┐
│ opentherm_gateway.c │  Main application (app_main, 1ms gateway loop)
└──────────┬──────────┘
           │
    ┌──────┴──────┬────────────┬──────────────┬────────────┐
    │             │            │              │            │
┌───▼─────┐  ┌────▼───────┐  ┌─▼──────────┐  ┌▼────────┐  ┌▼────────┐
│opentherm│  │boiler_mgr  │  │websocket   │  │mqtt     │  │ota      │
│ (API)   │  │            │  │ _server    │  │ _bridge │  │ _update │
└───┬─────┘  └────────────┘  └────────────┘  └─────────┘  └─────────┘
    │
    ├─RMT impl ──► opentherm_rmt (hardware RMT peripheral)
    └─Library impl ► opentherm_library (ISR-based software)
```

### Key Components

- **main/opentherm_gateway.c**: Entry point, WiFi init, main 1ms gateway loop that drives `ot_process()` and `boiler_manager_process()`
- **components/opentherm/**: Generic API abstraction (`opentherm_api.h`) with swappable backends (RMT or software)
- **components/opentherm_rmt/**: Hardware RMT peripheral implementation for Manchester encoding
- **components/opentherm_library/**: Software ISR-based implementation (ported from Arduino library)
- **components/boiler_manager/**: ID=0 interception, diagnostic injection (30+ data types), manual write queuing
- **components/websocket_server/**: HTTP server + WebSocket, serves web UI pages, JSON APIs
- **components/mqtt_bridge/**: MQTT client for external control, config persisted in NVS
- **components/ota_update/**: OTA firmware updates with rollback support
- **components/web_ui/**: Page content with compile-time HTML/CSS minification (`minify.hpp`)

### OpenTherm Backend Selection

Configure via `idf.py menuconfig` → "OpenTherm Implementation":
- **RMT** (default): Uses ESP32 RMT peripheral for hardware-timed Manchester encoding
- **Library**: Pure GPIO/timer ISR implementation, more portable

Both provide identical API through adapter pattern.

## Key Configuration

- **GPIO Pins**: `main/Kconfig.projbuild` (thermostat/boiler RX/TX pins)
- **WiFi/MQTT**: `main/Kconfig.projbuild` (also editable via menuconfig)
- **Flash Partitions**: `partitions.csv` (4MB with OTA rollback)
- **Build Defaults**: `sdkconfig.defaults`

## Development Patterns

### Adding a Web UI Page

1. Add HTML/CSS content to `components/web_ui/web_ui_pages.cpp` using `MINIFY_HTML()` / `MINIFY_CSS()` macros
2. Declare exports in `components/web_ui/web_ui.h`
3. Add HTTP handler in `components/websocket_server/websocket_server.c`

### Adding New Diagnostics

1. Add data ID to diagnostic command list in `components/boiler_manager/boiler_manager.c`
2. Add `boiler_diagnostic_value_t` field to `boiler_diagnostics_t` struct in `boiler_manager.h`
3. Update diagnostics web page to display the new value

### OpenTherm Message Format

32-bit frame: parity (1 bit) + message type (2 bits) + data ID (8 bits) + data value (16 bits)

Common Data IDs: 0=Status, 1=TSet, 17=Modulation, 25=Tboiler, 26=Tdhw, 28=Treturn

## External References

- OpenTherm Protocol: https://www.opentherm.eu/
- ESP-IDF Docs: https://docs.espressif.com/projects/esp-idf/
- OpenTherm Library (original): https://github.com/ihormelnyk/opentherm_library
- To run idf.py commands, export it to the shell first using `source ~/esp/v5.5.1/esp-idf/export.sh`