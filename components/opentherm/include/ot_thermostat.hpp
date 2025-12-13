/*
 * OpenTherm Thermostat Task (C++)
 *
 * RAII wrapper for FreeRTOS task that handles thermostat communication.
 * Runs in its own task and communicates with main loop via queues.
 */

#pragma once

#include "ot_queues.hpp"
#include "opentherm.hpp"
#include <atomic>
#include <cstdint>

namespace ot {

/**
 * Configuration for thermostat task
 */
struct ThermostatConfig {
    gpio_num_t rxPin;           // GPIO to receive from thermostat
    gpio_num_t txPin;           // GPIO to transmit to thermostat
    Queues* queues;             // Shared queue handles (non-owning)
    uint32_t taskStackSize = 4096;
    UBaseType_t taskPriority = 5;
};

/**
 * RAII wrapper for thermostat communication task
 *
 * Creates a FreeRTOS task that:
 * 1. BLOCKS waiting for request from thermostat
 * 2. Puts request on thermostat_request queue
 * 3. BLOCKS waiting for response from main loop
 * 4. Sends response back to thermostat
 */
class ThermostatTask {
public:
    explicit ThermostatTask(const ThermostatConfig& config);
    ~ThermostatTask();

    // Non-copyable
    ThermostatTask(const ThermostatTask&) = delete;
    ThermostatTask& operator=(const ThermostatTask&) = delete;

    // Query state
    [[nodiscard]] bool isRunning() const { return running_.load(); }
    [[nodiscard]] Stats stats() const;

    // Static helpers for main loop interaction (non-blocking)
    static void sendResponse(Queues& queues, Frame response);
    static bool getRequest(Queues& queues, Frame& request);

private:
    void taskFunction();
    static void taskEntry(void* arg);

    Port port_;
    Queues* queues_;
    TaskHandle_t taskHandle_ = nullptr;
    std::atomic<bool> running_{false};
    Stats stats_;
    mutable Mutex statsMutex_;
};

} // namespace ot
