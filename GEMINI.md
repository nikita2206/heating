# Gorynych (OpenTherm Gateway) Project Context

## Project Overview
**Gorynych** is an ESP32-based OpenTherm gateway that acts as a transparent proxy (MITM) between a thermostat and a boiler. It allows for real-time monitoring, logging, and control of heating systems via a modern Web UI and MQTT integration without disrupting normal thermostat operation.

### Key Features
*   **Transparent Proxy:** Intercepts and forwards OpenTherm frames.
*   **Real-time Dashboard:** React/Vite-based Web UI for status monitoring.
*   **Live Logging:** WebSocket stream of raw OpenTherm traffic.
*   **Home Automation:** MQTT integration for Home Assistant (telemetry & control).
*   **Diagnostics:** Injectable queries for extended boiler data (fault codes, pressure, etc.).

## Technology Stack

### Firmware (ESP32)
*   **Framework:** ESP-IDF v5.0+ (C++17 standard).
*   **OS:** FreeRTOS.
*   **Hardware Abstraction:** ESP32 RMT peripheral used for precise Manchester encoding/decoding of OpenTherm signals.
*   **Build System:** CMake + `idf.py`.

### Frontend (Web UI)
*   **Framework:** React (SPA).
*   **Bundler:** Vite.
*   **Language:** JavaScript (ES Modules).
*   **Deployment:** Pre-built, gzipped, and embedded directly into the ESP32 firmware binary.

## Project Structure

```text
/
├── main/
│   ├── gorynych.cpp         # Application entry point, WiFi/NVS init
│   └── gorynych.h           # Global pin definitions & constants
├── components/
│   ├── ot/                  # Low-level OpenTherm driver (RMT peripheral)
│   ├── boiler_manager/      # Core business logic, state machine, diagnostics
│   ├── api_server/          # HTTP/WebSocket server, Web UI serving
│   ├── mqtt_bridge/         # MQTT client implementation
│   └── web_ui/              # C wrapper to embed gzipped Web UI assets
├── web-ui/                  # Frontend source code (React/Vite)
├── build.sh                 # Unified build script (UI + Firmware)
├── CLAUDE.md                # Quick reference for AI assistants
└── partitions.csv           # Flash partition table (OTA enabled)
```

## Building and Running

### Prerequisites
*   **ESP-IDF v5.0+** (with `IDF_PATH` set).
*   **Node.js** (for building the Web UI).

### Key Commands

| Command | Description |
| :--- | :--- |
| `./build.sh` | **Recommended.** Builds both Web UI and ESP Firmware. |
| `idf.py build` | Builds only the firmware (requires existing Web UI artifacts). |
| `idf.py flash monitor` | Flashes the firmware and opens the serial monitor. |
| `idf.py menuconfig` | Opens configuration menu (WiFi, MQTT, Pins). |
| `idf.py fullclean` | Cleans all build artifacts. |

### Web UI Build Process
The Web UI is built separately and then embedded.
1.  **Source:** `web-ui/`
2.  **Build:** `npm run build` (inside `web-ui/`) -> generates `dist/`.
3.  **Embed:** Scripts (`scripts/gzip.js`, `add-hash.js`) compress assets, which are then linked into the firmware via `components/web_ui`.

## Configuration
Configuration is handled via `idf.py menuconfig` (Kconfig) and stored in NVS.
*   **WiFi:** SSID/Password.
*   **MQTT:** Broker URL, Topic Prefix.
*   **Pins:** Defined in `main/gorynych.h` (default: Master RX=25/TX=26, Slave RX=13/TX=14).

## Development Conventions
*   **C++ Style:** Modern C++17. Use `std::unique_ptr` for resource management.
*   **OpenTherm:** See `OPENTHERM_FRAMES.md` and `components/ot/` for protocol details.
*   **Boiler State:** Defined in `components/boiler_manager/include/boiler_manager.hpp`. When adding new diagnostics, update this struct and the JSON serializer in `api_server`.
*   **Thread Safety:** The `BoilerManager` runs in its own task. Access to state should be thread-safe or strictly managed.

## Architecture Notes
The system runs a central `BoilerManager` task that:
1.  Receives frames from the **Thermostat** (via `ot` driver).
2.  Decides whether to forward, modify, or reply (MITM logic).
3.  Periodically injects its own queries to the **Boiler** (Diagnostic phase) when the bus is idle.
4.  Updates the internal `BoilerState`.
5.  Notifies `MqttBridge` and `ApiServer` of state changes.
