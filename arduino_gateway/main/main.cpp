/*
 * OpenTherm Gateway using Arduino OpenTherm Library
 * Based on OpenThermGatewayMonitor_Demo example
 *
 * This gateway sits between thermostat and boiler, monitoring and forwarding
 * all OpenTherm communication.
 */

#include <Arduino.h>
#include <OpenTherm.h>

// GPIO Configuration for ESP32
// Master interface (connects to boiler)
const int mInPin = 21;   // RX from boiler
const int mOutPin = 22;  // TX to boiler

// Slave interface (connects to thermostat)
const int sInPin = 19;   // RX from thermostat
const int sOutPin = 23;  // TX to thermostat

// Create OpenTherm instances
// Master mode for communicating with boiler
OpenTherm mOT(mInPin, mOutPin);
// Slave mode for receiving from thermostat
OpenTherm sOT(sInPin, sOutPin, true);

// Interrupt handlers for Manchester encoding
void IRAM_ATTR mHandleInterrupt()
{
    mOT.handleInterrupt();
}

void IRAM_ATTR sHandleInterrupt()
{
    sOT.handleInterrupt();
}

// Process request from thermostat, forward to boiler, return response
void processRequest(unsigned long request, OpenThermResponseStatus status)
{
    if (status == OpenThermResponseStatus::SUCCESS) {
        // Log the thermostat request
        Serial.print("T");
        Serial.println(request, HEX);

        // Forward request to boiler and wait for response
        unsigned long response = mOT.sendRequest(request);

        if (response) {
            // Log the boiler response
            Serial.print("B");
            Serial.println(response, HEX);

            // Forward response back to thermostat
            sOT.sendResponse(response);
        } else {
            Serial.println("No response from boiler");
        }
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("OpenTherm Gateway starting...");
    Serial.printf("Master: IN=%d, OUT=%d\n", mInPin, mOutPin);
    Serial.printf("Slave:  IN=%d, OUT=%d\n", sInPin, sOutPin);

    // Initialize master interface (to boiler)
    mOT.begin(mHandleInterrupt);
    Serial.println("Master interface initialized");

    // Initialize slave interface (from thermostat) with callback
    sOT.begin(sHandleInterrupt, processRequest);
    Serial.println("Slave interface initialized");

    Serial.println("Gateway ready - monitoring OpenTherm traffic");
    Serial.println("Format: T=Thermostat request, B=Boiler response");
}

void loop()
{
    // Process incoming requests from thermostat
    sOT.process();
}

// Arduino entry point for ESP-IDF
extern "C" void app_main()
{
    initArduino();
    setup();

    while (true) {
        loop();
        vTaskDelay(1);  // Small delay to prevent watchdog issues
    }
}
