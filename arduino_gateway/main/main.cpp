/*
 * OpenTherm Gateway using Arduino OpenTherm Library
 * Based on OpenThermGatewayMonitor_Demo example
 *
 * This gateway sits between thermostat and boiler, monitoring and forwarding
 * all OpenTherm communication.
 */

#include <Arduino.h>
#include <OpenTherm.h>

// GPIO Configuration for ESP32 (matching opentherm_gateway.c)
// Master interface - gateway acts as master to boiler (sends requests, receives responses)
const int mInPin = 13;   // OT_SLAVE_IN_PIN - RX from boiler
const int mOutPin = 14;  // OT_SLAVE_OUT_PIN - TX to boiler

// Slave interface - gateway acts as slave to thermostat (receives requests, sends responses)
const int sInPin = 25;   // OT_MASTER_IN_PIN - RX from thermostat
const int sOutPin = 26;  // OT_MASTER_OUT_PIN - TX to thermostat

// Create OpenTherm instances
// Master mode: we send requests to boiler and receive its responses
OpenTherm mOT(mInPin, mOutPin);
// Slave mode: we receive requests from thermostat and send responses back
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

    // Explicitly configure pins before OpenTherm init
    // Output pins: start HIGH (idle state for OpenTherm)
    pinMode(mOutPin, OUTPUT);
    digitalWrite(mOutPin, HIGH);
    pinMode(sOutPin, OUTPUT);
    digitalWrite(sOutPin, HIGH);

    // Input pins: enable pull-up (OpenTherm idle is HIGH)
    pinMode(mInPin, INPUT_PULLUP);
    pinMode(sInPin, INPUT_PULLUP);

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

