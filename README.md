# Gorynych (OpenTherm Gateway)

Gorynych is a WiFi-enabled OpenTherm gateway based on ESP32 that sits between your thermostat and boiler. It acts as a transparent proxy, allowing you to monitor and control your heating system via a modern Web UI and MQTT without disrupting normal operation.

## Features

- **Transparent Proxy**: Seamlessly intercepts OpenTherm communication.
- **Real-time Dashboard**: Web UI (React/Vite) showing boiler status, temperatures, pressure, and modulation.
- **Live Frame Logging**: Watch raw OpenTherm traffic via WebSocket in real-time.
- **MQTT Integration**: Publish telemetry to Home Assistant and control setpoints remotely.
- **Boiler Control**:
  - **Manual Write**: Inject custom OpenTherm commands via API.
  - **Diagnostics**: Periodically queries boiler for extended data (e.g., fault codes, OEM info).
  - **Override**: Optional MQTT control mode to take over thermostat functions.
- **OTA Updates**: Wireless firmware updates via Web UI.

## Hardware

- **MCU**: ESP32
- **OpenTherm Adapter**: Requires two OpenTherm interfaces (one for Master/Thermostat, one for Slave/Boiler).
  - Typical design uses optocouplers for isolation.
  - [Hardware Schematic Reference](https://github.com/ihormelnyk/opentherm_library/tree/master/hardware)

### Pinout (Default)

| Function | ESP32 Pin    | Description |
|----------|--------------|-------------|
| **Master RX** | GPIO 25 | Input from Thermostat |
| **Master TX** | GPIO 26 | Output to Thermostat |
| **Slave RX** | GPIO 13 | Input from Boiler |
| **Slave TX** | GPIO 14 | Output to Boiler |

*Note: Pins can be changed in `main/gorynych.h`.*

## Project Structure

```
├── main/
│   ├── gorynych.cpp         # Main application entry
│   └── gorynych.h           # Pin definitions & config
├── components/
│   ├── ot/                  # OpenTherm RMT driver (Hardware-timed)
│   ├── boiler_manager/      # State machine, diagnostics, proxy logic
│   ├── websocket_server/    # Web server, JSON API, WebSocket
│   ├── mqtt_bridge/         # MQTT client implementation
│   └── web_ui/              # Embedded assets wrapper
├── web-ui/                  # React + Vite Frontend
└── build.sh                 # Unified build script
```

## Getting Started

### Prerequisites
- **ESP-IDF v5.0+**
- **Node.js** (for building Web UI)

### Build & Flash

1.  **Clone the repository**:
    ```bash
    git clone <repo-url>
    cd idf-heating/project
    ```

2.  **Configure**:
    ```bash
    idf.py menuconfig
    ```
    - Set WiFi SSID/Password under `OpenTherm Gateway Configuration`.
    - Set MQTT Broker URL.

3.  **Build Everything**:
    The included script builds both the React UI and the ESP firmware.
    ```bash
    ./build.sh
    ```

4.  **Flash**:
    ```bash
    idf.py flash monitor
    ```

## Web Interface

Once running, navigate to `http://<device-ip>/`.
- **Dashboard**: Overview of current temperatures and status.
- **Logs**: Real-time stream of OpenTherm messages.
- **Diagnostics**: Detailed boiler parameters and flags.
- **Write**: Manually send OpenTherm commands (for testing).

## MQTT Topics

The gateway publishes to `opentherm/#` (configurable):

- `opentherm/status`: JSON (connection status, uptime)
- `opentherm/messages`: JSON (stream of all frames)
- `opentherm/boiler/temperature`: Boiler water temp
- `opentherm/boiler/pressure`: System pressure
- `opentherm/thermostat/setpoint`: Current control setpoint
- `opentherm/room/temperature`: Room temperature
- ...and many more.

## Architecture Details

**Gorynych** runs a dedicated FreeRTOS task (`BoilerManager`) that orchestrates traffic.
- **Proxy Mode**: Frames from Thermostat are read, parsed, and forwarded to Boiler. Responses from Boiler are forwarded back.
- **Interception**: The gateway can modify frames on-the-fly or inject its own queries (e.g., "Give me your fault code") during idle slots.
- **Driver**: Uses the ESP32's **RMT (Remote Control)** peripheral to generate and decode Manchester-encoded OpenTherm signals with high precision, unloading the CPU.

## License

MIT License.
