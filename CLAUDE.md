This file provides guidance to AI agents.

## Build Commands

```bash
idf.py menuconfig    # Configure WiFi, MQTT, GPIO pins
idf.py build         # Build firmware
idf.py flash         # Flash to device
idf.py monitor       # Monitor serial output
idf.py flash monitor # Build, flash, and monitor in one step
idf.py fullclean     # Clean build artifacts
./build.sh           # Build everything (Web UI + Firmware)
./build.sh --clean   # Clean everything
```

Requires ESP-IDF v5.0+ with `IDF_PATH` environment variable set.

## Project Overview

**Gorynych** is an ESP-IDF project (ESP32) that acts as a MITM proxy between a thermostat and boiler. Intercepts all OpenTherm protocol messages, provides real-time WebSocket monitoring, MQTT integration, and OTA updates.

## Architecture

```
┌─────────────────────┐
│    gorynych.cpp     │  Main application (app_main, WiFi init)
└──────────┬──────────┘
           │
    ┌──────┴──────┬────────────┬──────────────┬────────────┐
    │             │            │              │            │
┌───▼─────┐  ┌────▼───────┐  ┌─▼──────────┐  ┌▼────────┐  ┌▼────────┐
│ot_driver│  │boiler_mgr  │  │api_server  │  │mqtt     │  │ota      │
│(RMT)    │  │(Logic)     │  │            │  │ _bridge │  │ _update │
└───┬─────┘  └────────────┘  └────────────┘  └─────────┘  └─────────┘
    │
    └─Hardware RMT Peripheral
```

### Key Components

- **main/gorynych.cpp**: Entry point, WiFi init, startup logic.
- **components/ot/**: `OpenThermDriver` class using ESP32 RMT peripheral for precise timing.
- **components/boiler_manager/**: Core logic. `BoilerManager` class handles message routing, interception, diagnostic injection, and state tracking (`BoilerState`).
- **components/api_server/**: Serves the Web UI (SPA) and provides WebSocket endpoint for real-time logging + JSON APIs.
- **components/mqtt_bridge/**: MQTT client for publishing telemetry and receiving control commands.
- **components/web_ui/**: C wrapper around embedded gzipped Web UI assets (`index.html.gz`, etc.).
- **web-ui/**: React + Vite frontend source code.

## Key Configuration

- **GPIO Pins**: Defined in `main/gorynych.h`.
  - Master (Thermostat): RX=25, TX=26
  - Slave (Boiler): RX=13, TX=14
- **WiFi/MQTT**: Configured via `idf.py menuconfig` (stored in NVS) or `main/Kconfig.projbuild`.
- **Flash Partitions**: `partitions.csv` (4MB with OTA rollback).

## Development Patterns

### Web UI Development

The Web UI is a React Single Page Application (SPA).
1.  Source is in `web-ui/`.
2.  Build with `./build.sh` (which calls `npm run build` in `web-ui/`).
3.  The build artifacts (`dist/`) are gzipped and embedded into the firmware binary via linker scripts.
4.  `api_server.cpp` handles SPA routing (serving `index.html` for client-side routes).

### Adding New Diagnostics

1.  Add new fields to `BoilerState` struct in `components/boiler_manager/include/boiler_manager.hpp`.
2.  Update `BoilerManager::Impl::updateState()` in `components/boiler_manager/boiler_manager.cpp` to parse/store the value.
3.  Update `diagnostics_api_handler` in `components/api_server/api_server.cpp` to include the new field in JSON output.
4.  Update Web UI (`web-ui/src/pages/Diagnostics.js`) to display it.

### OpenTherm Message Format

32-bit frame: parity (1 bit) + message type (2 bits) + data ID (8 bits) + data value (16 bits)

See OPENTHERM_FRAMES.md for a full glossary.

## External References

- OpenTherm Protocol: https://www.opentherm.eu/
- ESP-IDF Docs: https://docs.espressif.com/projects/esp-idf/
- To run idf.py commands, export it to the shell first using `source ~/esp/v5.5.1/esp-idf/export.sh`
