/*
 * OpenTherm Queue Definitions (C++)
 *
 * RAII wrappers for FreeRTOS queues and shared queue structures
 * for inter-thread communication.
 */

#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include "opentherm.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

namespace ot {

// Statistics structure
struct Stats {
    uint32_t txCount = 0;
    uint32_t rxCount = 0;
    uint32_t errorCount = 0;
    uint32_t timeoutCount = 0;
};

/**
 * RAII wrapper for FreeRTOS queue
 *
 * Thread-safe queue for passing messages between tasks.
 */
template<typename T>
class Queue {
public:
    explicit Queue(size_t size = 1)
        : handle_(xQueueCreate(size, sizeof(T)))
    {
    }

    ~Queue() {
        if (handle_) {
            vQueueDelete(handle_);
        }
    }

    // Non-copyable
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    // Movable
    Queue(Queue&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Queue& operator=(Queue&& other) noexcept {
        if (this != &other) {
            if (handle_) vQueueDelete(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // Send item to queue
    [[nodiscard]] bool send(const T& item, TickType_t timeout = 0) {
        return handle_ && xQueueSend(handle_, &item, timeout) == pdTRUE;
    }

    // Receive item from queue
    [[nodiscard]] bool receive(T& item, TickType_t timeout = 0) {
        return handle_ && xQueueReceive(handle_, &item, timeout) == pdTRUE;
    }

    // Overwrite item in queue (replaces any existing item)
    [[nodiscard]] bool overwrite(const T& item) {
        return handle_ && xQueueOverwrite(handle_, &item) == pdTRUE;
    }

    // Peek at front item without removing
    [[nodiscard]] bool peek(T& item, TickType_t timeout = 0) const {
        return handle_ && xQueuePeek(handle_, &item, timeout) == pdTRUE;
    }

    // Check if queue is empty
    [[nodiscard]] bool empty() const {
        return !handle_ || uxQueueMessagesWaiting(handle_) == 0;
    }

    // Get underlying handle (for compatibility)
    [[nodiscard]] QueueHandle_t handle() const { return handle_; }

    // Check if valid
    [[nodiscard]] bool isValid() const { return handle_ != nullptr; }

private:
    QueueHandle_t handle_;
};

/**
 * Shared queues for gateway communication
 *
 * All queues have size=1 to ensure single-message processing
 * and avoid buffering stale data.
 */
struct Queues {
    Queue<Frame> thermostatRequest{1};   // thermostat thread -> main loop
    Queue<Frame> thermostatResponse{1};  // main loop -> thermostat thread
    Queue<Frame> boilerRequest{1};       // main loop -> boiler thread
    Queue<Frame> boilerResponse{1};      // boiler thread -> main loop

    [[nodiscard]] bool isValid() const {
        return thermostatRequest.isValid() &&
               thermostatResponse.isValid() &&
               boilerRequest.isValid() &&
               boilerResponse.isValid();
    }
};

/**
 * RAII wrapper for FreeRTOS mutex
 *
 * Compatible with std::lock_guard and std::unique_lock.
 */
class Mutex {
public:
    Mutex() : handle_(xSemaphoreCreateMutex()) {}

    ~Mutex() {
        if (handle_) {
            vSemaphoreDelete(handle_);
        }
    }

    // Non-copyable
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        if (handle_) {
            xSemaphoreTake(handle_, portMAX_DELAY);
        }
    }

    void unlock() {
        if (handle_) {
            xSemaphoreGive(handle_);
        }
    }

    [[nodiscard]] bool try_lock() {
        return handle_ && xSemaphoreTake(handle_, 0) == pdTRUE;
    }

    [[nodiscard]] bool try_lock_for(TickType_t timeout) {
        return handle_ && xSemaphoreTake(handle_, timeout) == pdTRUE;
    }

private:
    SemaphoreHandle_t handle_;
};

/**
 * RAII wrapper for FreeRTOS binary semaphore
 */
class BinarySemaphore {
public:
    BinarySemaphore() : handle_(xSemaphoreCreateBinary()) {}

    ~BinarySemaphore() {
        if (handle_) {
            vSemaphoreDelete(handle_);
        }
    }

    // Non-copyable
    BinarySemaphore(const BinarySemaphore&) = delete;
    BinarySemaphore& operator=(const BinarySemaphore&) = delete;

    void give() {
        if (handle_) {
            xSemaphoreGive(handle_);
        }
    }

    [[nodiscard]] bool take(TickType_t timeout = portMAX_DELAY) {
        return handle_ && xSemaphoreTake(handle_, timeout) == pdTRUE;
    }

private:
    SemaphoreHandle_t handle_;
};

// Pin configuration for gateway mode
struct PinConfig {
    gpio_num_t thermostatInPin;
    gpio_num_t thermostatOutPin;
    gpio_num_t boilerInPin;
    gpio_num_t boilerOutPin;
};

} // namespace ot
