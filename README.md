# OpenTherm Gateway with WiFi and WebSocket Logging

This ESP-IDF project implements an OpenTherm gateway that sits between a thermostat and boiler, proxying all OpenTherm communication while logging messages via WebSocket for analysis.

## Overview

This is the first phase of a multi-room thermostat project. Currently, it focuses on:
- Intercepting and logging OpenTherm communication between thermostat and boiler
- Providing real-time monitoring via WebSocket connection
- Collecting telemetry data for future Zigbee-based multi-room implementation

## Features

- **OpenTherm Gateway**: Transparent proxy between thermostat (master) and boiler (slave)
- **WiFi Connectivity**: Connects to your local network
- **WebSocket Server**: Real-time message streaming to connected clients
- **Web Interface**: Built-in HTML page for monitoring OpenTherm traffic
- **Message Logging**: Detailed logging of all OpenTherm requests and responses

## Hardware Requirements

1. **ESP32 Development Board** (ESP32-C6, ESP32-H2, or similar)
2. **OpenTherm Adapter** (x2) - One for thermostat side, one for boiler side
   - See schematic inspiration: https://github.com/ihormelnyk/opentherm_library
3. **Wiring**:
   - Thermostat side: GPIO4 (IN), GPIO5 (OUT)
   - Boiler side: GPIO18 (IN), GPIO19 (OUT)

### OpenTherm Adapter Connection

```
Thermostat → OpenTherm Adapter 1 → ESP32 (GPIO 4/5) → OpenTherm Adapter 2 → Boiler
```

**Important**: You need OpenTherm-compatible adapters to handle voltage level conversion (7-15V OpenTherm ↔ 3.3V ESP32).

## Software Setup

### Prerequisites

- ESP-IDF v5.0 or later
- Python 3.8+

### Configuration

1. **WiFi Credentials**:
   ```bash
   idf.py menuconfig
   ```
   Navigate to: `OpenTherm Gateway Configuration` → Set your WiFi SSID and password

2. **GPIO Pins** (optional):
   Modify in `main/opentherm_gateway.h` if needed:
   ```c
   #define OT_MASTER_IN_PIN    GPIO_NUM_4
   #define OT_MASTER_OUT_PIN   GPIO_NUM_5
   #define OT_SLAVE_IN_PIN     GPIO_NUM_18
   #define OT_SLAVE_OUT_PIN    GPIO_NUM_19
   ```

### Building and Flashing

```bash
# Configure the project
idf.py menuconfig

# Build
idf.py build

# Flash to device
idf.py flash

# Monitor output
idf.py monitor
```

## Usage

1. **Connect Hardware**:
   - Connect thermostat to "master side" pins (GPIO 4/5 via OpenTherm adapter)
   - Connect boiler to "slave side" pins (GPIO 18/19 via OpenTherm adapter)
   - Power up the ESP32

2. **Monitor Device**:
   - Check serial console for WiFi connection status
   - Note the IP address assigned to the device

3. **Access Web Interface**:
   - Open browser and navigate to: `http://<device-ip>/`
   - WebSocket connection will establish automatically
   - All OpenTherm messages will appear in real-time

4. **View Logs**:
   - Serial console: `idf.py monitor`
   - Web interface: Real-time message display with timestamps
   - Messages show: Direction (REQUEST/RESPONSE), Type, Data ID, Value, Raw hex

## OpenTherm Message Format

Each logged message includes:
- **Timestamp**: Time of message capture
- **Direction**: REQUEST (from thermostat) or RESPONSE (from boiler)
- **Message Type**: READ_DATA, WRITE_DATA, READ_ACK, WRITE_ACK, etc.
- **Data ID**: OpenTherm data identifier (0-127)
- **Value**: Parsed data value
- **Raw**: Complete 32-bit message in hexadecimal

### Common Data IDs

| ID | Description | Type |
|----|-------------|------|
| 0 | Status flags | READ |
| 1 | Control setpoint (°C) | WRITE |
| 17 | Relative modulation level (%) | READ |
| 24 | Room temperature (°C) | WRITE |
| 25 | Boiler water temperature (°C) | READ |
| 26 | DHW temperature (°C) | READ |
| 27 | Outside temperature (°C) | WRITE |

Full OpenTherm specification: https://www.opentherm.eu/

## Project Structure

```
.
├── components/
│   ├── opentherm/          # OpenTherm protocol library
│   │   ├── opentherm.h
│   │   ├── opentherm.c
│   │   └── CMakeLists.txt
│   └── websocket_server/   # WebSocket server for logging
│       ├── websocket_server.h
│       ├── websocket_server.c
│       └── CMakeLists.txt
├── main/
│   ├── opentherm_gateway.c # Main application
│   ├── opentherm_gateway.h # Configuration
│   ├── Kconfig.projbuild   # menuconfig options
│   └── CMakeLists.txt
├── CMakeLists.txt
├── sdkconfig.defaults      # Default configuration
└── README.md
```

## Troubleshooting

### WiFi won't connect
- Verify SSID and password in menuconfig
- Check 2.4GHz WiFi is enabled on your router
- Monitor serial output for connection errors

### No OpenTherm messages
- Verify OpenTherm adapters are properly connected
- Check GPIO pin assignments match your hardware
- Ensure thermostat and boiler are both powered
- Verify OpenTherm adapters have correct voltage levels

### WebSocket not connecting
- Ensure device obtained IP address (check serial monitor)
- Try accessing `http://<device-ip>/` directly
- Check firewall settings
- Verify HTTP server started (look for "WebSocket server started" in logs)

## Next Steps

This is Phase 1 of the multi-room thermostat project. Future phases will:
1. **Analyze collected telemetry** to understand communication patterns
2. **Implement Zigbee coordinator** functionality
3. **Add multi-room temperature sensors** via Zigbee
4. **Smart heating control** based on room occupancy and temperature
5. **Replace simple proxy** with intelligent OpenTherm request generation

## References

- OpenTherm Library: https://github.com/ihormelnyk/opentherm_library
- OpenTherm Protocol: https://www.opentherm.eu/
- ESP-IDF Documentation: https://docs.espressif.com/projects/esp-idf/

## License

This project is based on ESP-IDF examples and the OpenTherm library by Ihor Melnyk.
Licensed under MIT License.
