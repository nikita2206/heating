# OpenTherm Gateway with WiFi and WebSocket Logging

ESP-IDF project implementing an OpenTherm gateway that sits between a thermostat and boiler, proxying OpenTherm communication while providing real-time monitoring, diagnostics, and MQTT integration.

## Features

- **OpenTherm Gateway**: Transparent proxy between thermostat (master) and boiler (slave)
- **Swappable Backends**: Choose between RMT hardware-based or software-based OpenTherm implementations
- **WiFi Connectivity**: WPA2/WPA3 support with automatic reconnection
- **WebSocket Server**: Real-time message streaming to connected clients
- **MQTT Integration**: Publish OpenTherm data to MQTT broker for home automation
- **Web Interface**: Built-in monitoring page with live OpenTherm traffic
- **Boiler Manager**: Advanced diagnostics and control features
  - Manual write injection for testing
  - Direct diagnostic polling independent of thermostat
  - Response injection for gateway modifications
- **OTA Updates**: Over-the-air firmware updates via HTTP

## Architecture

The project uses a modular component-based architecture:

```
┌─────────────────────┐
│ opentherm_gateway.c │  Main application
└──────────┬──────────┘
           │
    ┌──────┴──────┬────────────┬──────────────┬────────────┐
    │             │            │              │            │
┌───▼─────┐  ┌────▼───────┐  ┌─▼──────────┐  ┌▼────────┐  ┌▼────────┐
│opentherm│  │boiler_mgr  │  │websocket   │  │mqtt     │  │ota      │
│ (API)   │  │            │  │ _server    │  │ _bridge │  │ _update │
└───┬─────┘  └────────────┘  └────────────┘  └─────────┘  └─────────┘
    │
    ├─RMT impl ──────► opentherm_rmt (hardware-based)
    │
    └─Library impl ──► opentherm_library (ISR based)
```

### Components

- **opentherm**: Generic API abstraction layer for OpenTherm communication
  - `opentherm_rmt`: Hardware RMT peripheral implementation (precise timing)
  - `opentherm_library`: Pure ESP-IDF software implementation
- **boiler_manager**: Diagnostic injection, state management, and boiler control
- **websocket_server**: Real-time logging and monitoring over WebSocket
- **mqtt_bridge**: Publish OpenTherm telemetry to MQTT topics
- **ota_update**: Over-the-air firmware update functionality

## Hardware Requirements

1. **ESP32 Development Board** (ESP32, ESP32-S2, ESP32-C3, etc.)
2. **OpenTherm Interface Circuits** (x2) - One for thermostat side, one for boiler side
   - Hardware schematic: https://github.com/ihormelnyk/opentherm_library/tree/master/hardware
   - Handles 7-15V OpenTherm ↔ 3.3V ESP32 level shifting
3. **Default GPIO Pins**:
   - Thermostat side: GPIO4 (RX), GPIO5 (TX)
   - Boiler side: GPIO18 (RX), GPIO19 (TX)

### Wiring

```
Thermostat ←→ OpenTherm Interface 1 ←→ ESP32 (GPIO4/5) ←→ OpenTherm Interface 2 ←→ Boiler
```

## Software Setup

### Prerequisites

- ESP-IDF v5.0 or later
- Python 3.8+

### Configuration

Run `idf.py menuconfig` to configure:

1. **OpenTherm Implementation** (`OpenTherm Implementation` menu):
   - RMT (Hardware-based) - default, uses ESP32 RMT peripheral
   - Library - pure ESP-IDF software implementation

2. **WiFi Settings** (`OpenTherm Gateway Configuration` menu):
   - WiFi SSID
   - WiFi Password

3. **MQTT Settings** (`OpenTherm Gateway Configuration` menu):
   - MQTT Broker URI (e.g., `mqtt://192.168.1.100:1883`)
   - MQTT Username (optional)
   - MQTT Password (optional)

4. **GPIO Pins** (optional, in code):
   Edit `main/opentherm_gateway.c` if default pins don't match your hardware.

### Building and Flashing

```bash
# Configure
idf.py menuconfig

# Build
idf.py build

# Flash and monitor
idf.py flash monitor
```

## Usage

### Initial Setup

1. Flash firmware to ESP32
2. Connect hardware:
   - Thermostat to GPIO4/5 via OpenTherm interface
   - Boiler to GPIO18/19 via OpenTherm interface
3. Power on and watch serial console for WiFi connection
4. Note the device IP address

### Web Interface

Navigate to `http://<device-ip>/` to access the monitoring interface:
- Real-time OpenTherm message display
- Request/Response pairs with timestamps
- Message type, Data ID, and parsed values
- Raw hex frame data

### MQTT Integration

OpenTherm data is published to MQTT topics (if MQTT is configured):

```
opentherm/status             - Connection and gateway status
opentherm/messages           - All OpenTherm messages (JSON)
opentherm/boiler/temperature - Boiler water temperature
opentherm/boiler/pressure    - System pressure
opentherm/thermostat/setpoint - Temperature setpoint
opentherm/room/temperature   - Room temperature
```

### Boiler Manager Features

The boiler manager component provides advanced functionality:

**Manual Write Injection**: Send custom OpenTherm write commands to the boiler for testing purposes.

**Direct Diagnostic Polling**: Query boiler diagnostics independently of the thermostat's normal communication cycle.

**Response Injection**: Modify gateway behavior by injecting custom responses (e.g., virtual sensors, override values).

These features enable:
- Testing boiler capabilities
- Implementing custom control logic
- Multi-room temperature averaging
- Advanced diagnostics

## OpenTherm Message Format

### Message Structure

```
[Timestamp] [Direction] Type=TYPE DataID=XX Value=YY (0xZZZZZZZZ)
```

- **Timestamp**: Milliseconds since boot
- **Direction**: `→ REQUEST` or `← RESPONSE`
- **Type**: Message type (READ_DATA, READ_ACK, WRITE_DATA, etc.)
- **DataID**: OpenTherm data identifier (0-127)
- **Value**: Parsed value with units
- **Raw**: 32-bit hex frame

### Common Data IDs

| ID | Name | Description | Type |
|----|------|-------------|------|
| 0 | Status | Master/Slave status flags | READ |
| 1 | TSet | Control setpoint (°C) | WRITE |
| 17 | RelModLevel | Relative modulation level (%) | READ |
| 24 | TRoom | Room temperature (°C) | WRITE |
| 25 | Tboiler | Boiler water temperature (°C) | READ |
| 26 | Tdhw | DHW temperature (°C) | READ |
| 27 | Toutside | Outside temperature (°C) | WRITE |
| 28 | Tret | Return water temperature (°C) | READ |

Full specification: https://www.opentherm.eu/

## Project Structure

```
project/
├── components/
│   ├── opentherm/              # Generic OpenTherm API
│   │   ├── include/
│   │   │   ├── opentherm_api.h
│   │   │   └── opentherm_types.h
│   │   ├── opentherm_rmt_impl.c
│   │   ├── opentherm_lib_impl.cpp
│   │   ├── Kconfig
│   │   └── CMakeLists.txt
│   ├── opentherm_rmt/          # RMT hardware implementation
│   │   ├── include/
│   │   │   ├── opentherm_rmt.h
│   │   │   └── opentherm.h
│   │   ├── opentherm_rmt.c
│   │   ├── opentherm.c
│   │   └── CMakeLists.txt
│   ├── opentherm_library/      # Pure ESP-IDF implementation
│   │   ├── src/
│   │   │   ├── OpenTherm.h
│   │   │   └── OpenTherm.cpp
│   │   ├── idf_component.yml
│   │   └── CMakeLists.txt
│   ├── boiler_manager/         # Diagnostics and control
│   ├── websocket_server/       # WebSocket logging
│   ├── mqtt_bridge/            # MQTT integration
│   └── ota_update/             # OTA firmware updates
├── main/
│   ├── opentherm_gateway.c     # Main application
│   ├── opentherm_gateway.h
│   ├── Kconfig.projbuild
│   └── CMakeLists.txt
├── CMakeLists.txt
├── sdkconfig.defaults
└── README.md
```

## Switching OpenTherm Implementations

The project supports two OpenTherm backends:

### RMT Implementation (Default)
- Uses ESP32 RMT peripheral for hardware-timed signal generation
- Precise timing, minimal CPU overhead
- Best performance and reliability

### Library Implementation
- Pure ESP-IDF implementation using GPIO interrupts and timers
- No special hardware requirements
- Portable to any GPIO-capable ESP32 variant

To switch between implementations:

```bash
idf.py menuconfig
# Navigate to: OpenTherm Implementation → Select desired backend
idf.py build flash
```

Both implementations provide identical API, so the rest of the application is unaffected.

## Troubleshooting

### WiFi Issues
- Verify SSID and password in menuconfig
- Check 2.4GHz WiFi is enabled (ESP32 doesn't support 5GHz)
- Monitor serial output: `idf.py monitor`

### No OpenTherm Messages
- Verify OpenTherm interfaces are properly wired
- Check GPIO pins match your configuration
- Ensure both thermostat and boiler are powered
- Test voltage levels on OpenTherm lines (should be 7-15V idle)

### WebSocket Connection Fails
- Verify device IP address (check serial monitor)
- Ensure HTTP server started successfully
- Check firewall settings on client device
- Try different browser or clear cache

### MQTT Not Connecting
- Verify MQTT broker URI is correct
- Check username/password if broker requires authentication
- Ensure MQTT broker is reachable from ESP32 network
- Monitor logs for connection errors

## OTA Updates

To perform over-the-air firmware updates:

1. Build new firmware: `idf.py build`
2. Host the binary on HTTP server
3. Navigate to `http://<device-ip>/ota`
4. Enter firmware URL and click "Update"
5. Device will download, flash, and reboot

Alternatively, trigger OTA via MQTT:
```bash
mosquitto_pub -h <broker> -t opentherm/ota/update -m "http://server/firmware.bin"
```

## Development

### Adding Custom Data ID Handlers

Edit `components/boiler_manager/boiler_manager.c` to add custom handling for specific OpenTherm Data IDs.

### Extending MQTT Topics

Modify `components/mqtt_bridge/mqtt_bridge.c` to publish additional telemetry to custom topics.

### Custom Web Interface

Replace the built-in HTML in `components/websocket_server/websocket_server.c` with your own interface.

## References

- OpenTherm Protocol Specification: https://www.opentherm.eu/
- ESP-IDF Documentation: https://docs.espressif.com/projects/esp-idf/
- Original OpenTherm Library: https://github.com/ihormelnyk/opentherm_library

## License

MIT License. Based on ESP-IDF and the OpenTherm library by Ihor Melnyk.
