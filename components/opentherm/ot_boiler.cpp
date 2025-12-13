/*
 * OpenTherm Boiler Task Implementation (C++)
 */

#include "ot_boiler.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "OT_BOILER";

namespace ot {

BoilerTask::BoilerTask(const BoilerConfig& config)
    : port_(config.rxPin, config.txPin, false)  // master mode to send to boiler
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
        &BoilerTask::taskEntry,
        "ot_boiler",
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

BoilerTask::~BoilerTask() {
    running_ = false;

    // Give task time to exit
    if (taskHandle_) {
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    port_.end();
    ESP_LOGI(TAG, "Destroyed");
}

void BoilerTask::taskEntry(void* arg) {
    auto* self = static_cast<BoilerTask*>(arg);
    self->taskFunction();
    vTaskDelete(nullptr);
}

void BoilerTask::taskFunction() {
    ESP_LOGI(TAG, "Task started");

    while (running_.load()) {
        // BLOCKING: Wait for request from main loop (100ms timeout)
        Frame request;
        if (!queues_->boilerRequest.receive(request, pdMS_TO_TICKS(100))) {
            continue;
        }

        ESP_LOGI(TAG, "Sending request to boiler: 0x%08lX",
                 static_cast<unsigned long>(request.raw()));

        // Send request to boiler and wait for response (blocks ~1 second max)
        Frame response = port_.sendRequest(request);

        auto status = port_.lastResponseStatus();
        ESP_LOGI(TAG, "Response status: %s, response: 0x%08lX",
                 toString(status), static_cast<unsigned long>(response.raw()));

        if (!response || status != ResponseStatus::Success) {
            // Timeout or error
            ESP_LOGW(TAG, "Boiler response timeout or error");
            {
                std::lock_guard<Mutex> lock(statsMutex_);
                stats_.timeoutCount++;
            }

            // Send error response to main loop (frame = 0 indicates error)
            queues_->boilerResponse.overwrite(Frame{});
            continue;
        }

        // Validate response
        if (!response.isValidResponse()) {
            ESP_LOGW(TAG, "Invalid response from boiler: 0x%08lX",
                     static_cast<unsigned long>(response.raw()));
            {
                std::lock_guard<Mutex> lock(statsMutex_);
                stats_.errorCount++;
            }

            queues_->boilerResponse.overwrite(Frame{});
            continue;
        }

        ESP_LOGD(TAG, "Received response from boiler: 0x%08lX",
                 static_cast<unsigned long>(response.raw()));

        {
            std::lock_guard<Mutex> lock(statsMutex_);
            stats_.rxCount++;
            stats_.txCount++;
        }

        // Put response on queue for main loop
        queues_->boilerResponse.overwrite(response);
    }

    ESP_LOGI(TAG, "Task stopped");
}

Stats BoilerTask::stats() const {
    std::lock_guard<Mutex> lock(statsMutex_);
    return stats_;
}

void BoilerTask::sendRequest(Queues& queues, Frame request) {
    queues.boilerRequest.overwrite(request);
}

bool BoilerTask::getResponse(Queues& queues, Frame& response) {
    return queues.boilerResponse.receive(response, 0);
}

} // namespace ot
