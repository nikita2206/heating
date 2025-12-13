/*
 * OpenTherm Boiler Task (C++)
 *
 * RAII wrapper for FreeRTOS task that handles boiler communication.
 * Runs in its own task and communicates with main loop via queues.
 */

#pragma once

#include "ot_queues.hpp"
#include "opentherm.hpp"
#include <atomic>
#include <cstdint>

namespace ot {

/**
 * Configuration for boiler task
 */
struct BoilerConfig {
    gpio_num_t rxPin;           // GPIO to receive from boiler
    gpio_num_t txPin;           // GPIO to transmit to boiler
    Queues* queues;             // Shared queue handles (non-owning)
    uint32_t taskStackSize = 4096;
    UBaseType_t taskPriority = 5;
};

/**
 * RAII wrapper for boiler communication task
 *
 * Creates a FreeRTOS task that:
 * 1. BLOCKS waiting for request from main loop
 * 2. Sends request to boiler
 * 3. BLOCKS waiting for response from boiler
 * 4. Puts response on queue for main loop
 */
class BoilerTask {
public:
    explicit BoilerTask(const BoilerConfig& config);
    ~BoilerTask();

    // Non-copyable
    BoilerTask(const BoilerTask&) = delete;
    BoilerTask& operator=(const BoilerTask&) = delete;

    // Query state
    [[nodiscard]] bool isRunning() const { return running_.load(); }
    [[nodiscard]] Stats stats() const;

    // Static helpers for main loop interaction (non-blocking)
    static void sendRequest(Queues& queues, Frame request);
    static bool getResponse(Queues& queues, Frame& response);

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
