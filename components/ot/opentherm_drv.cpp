#include "opentherm_drv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>

static const char* TAG = "OT_DRV";

namespace ot {

OpenThermDriver::OpenThermDriver(const Config& config) 
    : config_(config) {
    txQueue_ = xQueueCreate(10, sizeof(OpenThermFrame));
    rxQueue_ = xQueueCreate(10, sizeof(OpenThermFrame));
}

OpenThermDriver::~OpenThermDriver() {
    stop();
    if (txQueue_) {
        vQueueDelete(txQueue_);
    }
    if (rxQueue_) {
        vQueueDelete(rxQueue_);
    }
}

void OpenThermDriver::start() {
    if (taskHandle_) return;

    // Initialize RMT
    initRMT();

    xTaskCreatePinnedToCore(
        taskEntry,
        "ot_drv_task",
        4096,
        this,
        configMAX_PRIORITIES - 1,
        &taskHandle_,
        1
    );
}

void OpenThermDriver::stop() {
    if (!taskHandle_) return;

    xTaskNotify(taskHandle_, NOTIFY_STOP, eSetBits);
}

bool OpenThermDriver::send(OpenThermFrame frame) {
    if (!txQueue_) return false;
    if (xQueueSend(txQueue_, &frame, 0) == pdTRUE) {
        if (taskHandle_) {
            xTaskNotify(taskHandle_, NOTIFY_TX_REQUEST, eSetBits);
        }
        return true;
    }
    return false;
}

std::optional<OpenThermFrame> OpenThermDriver::receive(uint32_t timeoutMs) {
    if (!rxQueue_) return std::nullopt;
    
    OpenThermFrame rxFrame;
    if (xQueueReceive(rxQueue_, &rxFrame, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
        return rxFrame;
    }
    return std::nullopt;
}

void OpenThermDriver::initRMT() {
    ESP_LOGI(TAG, "Initializing RMT: In=%d, Out=%d", config_.inPin, config_.outPin);

    // TX Config
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = config_.outPin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &txChannel_));

    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copyEncoder_));
    ESP_ERROR_CHECK(rmt_enable(txChannel_));
    
    // Set idle level (important for OpenTherm)
    gpio_set_level(config_.outPin, 1);

    // RX Config
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = config_.inPin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 128,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rxChannel_));

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = onRmtRxDone,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rxChannel_, &cbs, this));
    ESP_ERROR_CHECK(rmt_enable(rxChannel_));

    // Start receiving
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 3000, // 3us
        .signal_range_max_ns = 2000000, // 2ms
    };
    activeRxBuffer_ = 0;
    ESP_ERROR_CHECK(rmt_receive(rxChannel_, rxBuffer_[activeRxBuffer_], sizeof(rxBuffer_[0]), &receive_config));
}

void OpenThermDriver::deinitRMT() {
    if (rxChannel_) {
        rmt_disable(rxChannel_);
        rmt_del_channel(rxChannel_);
        rxChannel_ = nullptr;
    }
    if (txChannel_) {
        rmt_disable(txChannel_);
        rmt_del_channel(txChannel_);
        txChannel_ = nullptr;
    }
    if (copyEncoder_) {
        rmt_del_encoder(copyEncoder_);
        copyEncoder_ = nullptr;
    }
}

bool OpenThermDriver::onRmtRxDone(rmt_channel_handle_t rx_chan, const rmt_rx_done_event_data_t *edata, void *user_ctx) {
    OpenThermDriver* drv = static_cast<OpenThermDriver*>(user_ctx);
    BaseType_t high_task_wakeup = pdFALSE;

    drv->lastRxSize_ = edata->num_symbols;
    drv->rxFrameReady_ = true;
    
    // Notify task
    if (drv->taskHandle_) {
        xTaskNotifyFromISR(drv->taskHandle_, NOTIFY_RX_DONE, eSetBits, &high_task_wakeup);
    }
    
    // Don't restart receive here; task will do it after processing to ensure buffer swap is clean
    // Actually, RMT might need immediate restart if we want to catch back-to-back?
    // OpenTherm is half-duplex Request-Response, so we shouldn't receive while processing unless it's noise.
    // Safe to let task restart it.
    
    return high_task_wakeup == pdTRUE;
}

void OpenThermDriver::taskEntry(void* arg) {
    OpenThermDriver* drv = static_cast<OpenThermDriver*>(arg);
    drv->taskLoop();
    vTaskDelete(nullptr);
}

void OpenThermDriver::taskLoop() {
    // Initial 1 second delay with pin at HIGH (gpio_set_level in initRMT)
    nextPossibleTrafficTime_ = esp_timer_get_time() + 1000000;
    
    while (true) {
        uint32_t notificationValue = 0;
        uint32_t waitTimeTicks = portMAX_DELAY;
        
        // Calculate next allowed TX time if we have pending TX
        int64_t now = esp_timer_get_time();
        int64_t minIntervalUs = config_.minTxIntervalMs * 1000;
        
        // If we have something in queue (peek), we might need to wake up sooner
        OpenThermFrame pendingTxFrame;
        bool hasPendingTx = (xQueuePeek(txQueue_, &pendingTxFrame, 0) == pdTRUE);
        
        if (hasPendingTx) {
            if (now < nextPossibleTrafficTime_) {
                // We need to wait
                int64_t waitUs = nextPossibleTrafficTime_ - now;
                if (waitUs > 0) {
                    waitTimeTicks = pdMS_TO_TICKS(waitUs / 1000) + 1;
                } else {
                    waitTimeTicks = 0;
                }
            } else {
                waitTimeTicks = 0; // Ready to send immediately
            }
        }

        // Wait for notification or timeout
        xTaskNotifyWait(0, ULONG_MAX, &notificationValue, waitTimeTicks);

        if (notificationValue & NOTIFY_STOP) {
            break;
        }

        // Handle RX
        if ((notificationValue & NOTIFY_RX_DONE) || rxFrameReady_) {
            // Process received frame
            size_t size = lastRxSize_;
            rmt_symbol_word_t* buf = rxBuffer_[activeRxBuffer_];
            
            // Swap buffer for next receive
            activeRxBuffer_ = 1 - activeRxBuffer_;
            rxFrameReady_ = false;
            
            // Restart receive immediately
            rmt_receive_config_t receive_config = {
                .signal_range_min_ns = 3000,
                .signal_range_max_ns = 2000000,
            };
            rmt_receive(rxChannel_, rxBuffer_[activeRxBuffer_], sizeof(rxBuffer_[0]), &receive_config);
            
            // Decode
            uint32_t frame = decodeRmtAsOpenTherm(buf, size, config_.isSlave);
            bool isValid = (frame != 0); // Minimal validation provided by decode
            
            if (isValid && rxQueue_) {
                // NOTE: If queue is full, we drop the frame (or could block with timeout 0).
                xQueueSend(rxQueue_, &frame, 0);
            }
            
            int64_t nextTime = esp_timer_get_time() + minIntervalUs;
            if (nextTime > nextPossibleTrafficTime_) {
                nextPossibleTrafficTime_ = nextTime;
            }
        }

        // Handle TX
        // We check queue again
        now = esp_timer_get_time();
        
        if (xQueuePeek(txQueue_, &pendingTxFrame, 0) == pdTRUE) {
            if (now >= nextPossibleTrafficTime_) {
                // Actually pop the item
                xQueueReceive(txQueue_, &pendingTxFrame, 0);
                
                transmitFrame(pendingTxFrame);
                
                int64_t nextTime = esp_timer_get_time() + minIntervalUs;
                if (nextTime > nextPossibleTrafficTime_) {
                    nextPossibleTrafficTime_ = nextTime;
                }
            }
            // Else: we will loop around and calculate waitTimeTicks
        }
    }
    
    deinitRMT();
    taskHandle_ = nullptr;
}

/**
 * Blocks for up to 100ms for the frame to be transmitted.
 */
bool OpenThermDriver::transmitFrame(OpenThermFrame frame) {
    size_t numSymbols = encodeOpenThermAsRmt(frame.raw(), txBuffer_);
    
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags = { .eot_level = 1 } 
    };
    
    if (rmt_transmit(txChannel_, copyEncoder_, txBuffer_, numSymbols * sizeof(rmt_symbol_word_t), &tx_config) != ESP_OK) {
        return false;
    }
    
    return rmt_tx_wait_all_done(txChannel_, 100) == ESP_OK;
}

} // namespace ot
