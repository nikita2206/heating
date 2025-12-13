/*
 * OpenTherm Thermostat Task Implementation (C++)
 */

#include "ot_thermostat.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "OT_THERM";

namespace ot {

ThermostatTask::ThermostatTask(const ThermostatConfig& config)
    : port_(config.rxPin, config.txPin, true)  // slave mode to receive from thermostat
    , queues_(config.queues)
{
    if (!queues_ || !queues_->isValid()) {
        ESP_LOGE(TAG, "Invalid queues");
        return;
    }

    if (port_.begin() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize OpenTherm port");
        return;
    }

    running_ = true;

    BaseType_t ret = xTaskCreate(
        &ThermostatTask::taskEntry,
        "ot_therm",
        config.taskStackSize > 0 ? config.taskStackSize : 4096,
        this,
        config.taskPriority > 0 ? config.taskPriority : 5,
        &taskHandle_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        running_ = false;
        return;
    }

    ESP_LOGI(TAG, "Initialized: RX=GPIO%d, TX=GPIO%d",
             static_cast<int>(config.rxPin), static_cast<int>(config.txPin));
}

ThermostatTask::~ThermostatTask() {
    running_ = false;

    // Give task time to exit
    if (taskHandle_) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    port_.end();
    ESP_LOGI(TAG, "Destroyed");
}

void ThermostatTask::taskEntry(void* arg) {
    auto* self = static_cast<ThermostatTask*>(arg);
    self->taskFunction();
    vTaskDelete(nullptr);
}

void ThermostatTask::taskFunction() {
    ESP_LOGI(TAG, "Task started");

    while (running_.load()) {
        // Process state machine
        port_.process();

        // Check if we received a request
        auto status = port_.lastResponseStatus();
        if (status == ResponseStatus::Success) {
            Frame request = port_.lastResponse();

            // Validate request
            if (!request.isValidRequest()) {
                ESP_LOGW(TAG, "Invalid request: 0x%08lX", static_cast<unsigned long>(request.raw()));
                {
                    std::lock_guard<Mutex> lock(statsMutex_);
                    stats_.errorCount++;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            ESP_LOGI(TAG, "RX: 0x%08lX", static_cast<unsigned long>(request.raw()));
            {
                std::lock_guard<Mutex> lock(statsMutex_);
                stats_.rxCount++;
            }

            // Put request on queue for main loop
            queues_->thermostatRequest.overwrite(request);

            // Wait for response from main loop (up to 750ms)
            Frame response;
            if (queues_->thermostatResponse.receive(response, pdMS_TO_TICKS(750))) {
                ESP_LOGI(TAG, "TX: 0x%08lX", static_cast<unsigned long>(response.raw()));

                // Send response to thermostat
                if (port_.sendResponse(response)) {
                    std::lock_guard<Mutex> lock(statsMutex_);
                    stats_.txCount++;
                } else {
                    ESP_LOGW(TAG, "Failed to send response");
                    std::lock_guard<Mutex> lock(statsMutex_);
                    stats_.errorCount++;
                }
            } else {
                ESP_LOGD(TAG, "No response from main loop");
                std::lock_guard<Mutex> lock(statsMutex_);
                stats_.timeoutCount++;
            }
        }
        else if (status == ResponseStatus::Timeout) {
            std::lock_guard<Mutex> lock(statsMutex_);
            stats_.timeoutCount++;
        }
        else if (status == ResponseStatus::Invalid) {
            std::lock_guard<Mutex> lock(statsMutex_);
            stats_.errorCount++;
        }

        // Small delay to avoid busy-waiting
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Task stopped");
}

Stats ThermostatTask::stats() const {
    std::lock_guard<Mutex> lock(statsMutex_);
    return stats_;
}

void ThermostatTask::sendResponse(Queues& queues, Frame response) {
    queues.thermostatResponse.overwrite(response);
}

bool ThermostatTask::getRequest(Queues& queues, Frame& request) {
    return queues.thermostatRequest.receive(request, 0);
}

} // namespace ot
