/*
OpenTherm.cpp - OpenTherm Communication Library for ESP-IDF
Copyright 2023, Ihor Melnyk
ESP-IDF port
*/

#include "OpenTherm.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>


static void monitorTaskEntry(void* pvParameters) {
    ot::OpenTherm* otInstance = static_cast<ot::OpenTherm*>(pvParameters);
    otInstance->monitorInterrupts();
    vTaskDelete(nullptr);
}

namespace ot {

bool IRAM_ATTR on_rmt_rx_done(rmt_channel_handle_t rx_chan, const rmt_rx_done_event_data_t *edata, void *user_ctx) {
    OpenTherm* instance = static_cast<OpenTherm*>(user_ctx);
    BaseType_t high_task_wakeup = pdFALSE;

    // Record frame size from current buffer (the one RMT just finished writing)
    instance->rmtFrameSize_ = edata->num_symbols;
    instance->rmtFrameReady_ = true;

    // Swap to the other buffer for the next receive (task will restart receive)
    instance->rmtActiveBuffer_ = 1 - instance->rmtActiveBuffer_;

    // Notify the task to process the frame and restart receive
    if (instance->monitorTaskHandle_ != nullptr) {
        vTaskNotifyGiveFromISR(instance->monitorTaskHandle_, &high_task_wakeup);
    }

    return high_task_wakeup == pdTRUE;
}

OpenTherm::OpenTherm(gpio_num_t inPin, gpio_num_t outPin, bool isSlave, bool invertOutput) :
    status(OpenThermStatus::NOT_INITIALIZED),
    inPin(inPin),
    outPin(outPin),
    isSlave(isSlave),
    invertOutput(invertOutput),
    response(0),
    responseStatus(OpenThermResponseStatus::NONE),
    responseTimestamp(0),
    monitorTaskHandle_(nullptr),
    rmtChannel_(nullptr),
    rmtTxChannel_(nullptr),
    rmtCopyEncoder_(nullptr),
    rmtActiveBuffer_(0),
    rmtFrameSize_(0),
    rmtFrameReady_(false)
{
    memset(rmtRxBuffers_, 0, sizeof(rmtRxBuffers_));
    memset(rmtTxBuffer_, 0, sizeof(rmtTxBuffer_));
}

void OpenTherm::begin()
{
    // Set output GPIO to idle state BEFORE RMT takes control of it
    // This ensures the boiler sees a proper idle state during initialization
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << outPin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);
    gpio_set_level(outPin, invertOutput ? 0 : 1);  // Set idle level
    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for boiler to recognize idle state

    // Configure input pin for RMT (no internal pull-up - OpenTherm interface has its own)
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << inPin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    // Initialize RMT for RX and TX
    initRMT();
    initRMTTx();

    // Create monitoring task for RMT data processing
    BaseType_t ret = xTaskCreatePinnedToCore(
        monitorTaskEntry,
        "ot_rmt_monitor",
        8192, // Stack size (bytes) - parseRMTSymbols needs ~3KB for log buffer
        this,
        configMAX_PRIORITIES - 1, // High priority
        &monitorTaskHandle_,
        1 // Core ID (0 or 1 on ESP32, or tskNO_AFFINITY)
    );

    if (ret != pdPASS) {
        ESP_LOGE("OpenTherm", "Failed to create RMT monitor task");
        monitorTaskHandle_ = nullptr;
    }

    // Start RMT reception
    startRMTReceive();

    status = OpenThermStatus::READY;
}

void OpenTherm::initRMT()
{
    ESP_LOGI("OpenTherm", "Initializing RMT for GPIO %d", inPin);

    // Configure RMT RX channel
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = inPin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,  // 1MHz resolution (1μs ticks)
        .mem_block_symbols = 128,  // Memory block size for received symbols
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rmtChannel_));
    ESP_LOGI("OpenTherm", "RMT channel created: %p", rmtChannel_);

    // Register event callbacks - pass this instance as user context
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = on_rmt_rx_done,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rmtChannel_, &cbs, this));
    ESP_LOGI("OpenTherm", "RMT callbacks registered");

    // Enable the channel
    ESP_ERROR_CHECK(rmt_enable(rmtChannel_));
    ESP_LOGI("OpenTherm", "RMT RX channel enabled");
}

void OpenTherm::initRMTTx()
{
    ESP_LOGI("OpenTherm", "Initializing RMT TX for GPIO %d", outPin);

    // Configure RMT TX channel
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = outPin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,  // 1MHz resolution (1μs ticks)
        .mem_block_symbols = 64,   // Memory block size
        .trans_queue_depth = 4,    // Transaction queue depth
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &rmtTxChannel_));
    ESP_LOGI("OpenTherm", "RMT TX channel created: %p", rmtTxChannel_);

    // Create a copy encoder (simply copies pre-encoded symbols)
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &rmtCopyEncoder_));
    ESP_LOGI("OpenTherm", "RMT copy encoder created");

    // Enable the TX channel
    ESP_ERROR_CHECK(rmt_enable(rmtTxChannel_));
    ESP_LOGI("OpenTherm", "RMT TX channel enabled");

    // Set idle level to match idle state (inverted if invertOutput is set)
    // Note: RMT idle level is set per-transmission via transmit config
}

void OpenTherm::startRMTReceive()
{
    if (rmtChannel_) {
        ESP_LOGI("OpenTherm", "Starting RMT reception...");
        // Use receive config appropriate for OpenTherm Manchester encoding
        // Half-bits are ~500μs, full bits ~1000μs, so allow some tolerance
        rmt_receive_config_t receive_config = {
            .signal_range_min_ns = 3000,  // 3μs minimum (filter noise)
            .signal_range_max_ns = 2000000, // 2000μs maximum (allow tolerance)
        };
        // Start with buffer 0
        rmtActiveBuffer_ = 0;
        ESP_ERROR_CHECK(rmt_receive(rmtChannel_, rmtRxBuffers_[0], sizeof(rmtRxBuffers_[0]), &receive_config));
        ESP_LOGI("OpenTherm", "RMT reception started");
    } else {
        ESP_LOGE("OpenTherm", "RMT not initialized: rmtChannel_=%p", rmtChannel_);
    }
}

void OpenTherm::stopRMTReceive()
{
    if (rmtChannel_) {
        ESP_ERROR_CHECK(rmt_disable(rmtChannel_));
    }
}

size_t OpenTherm::encodeFrameToRMT(unsigned long frame, rmt_symbol_word_t* symbols)
{
    // OpenTherm Manchester encoding:
    // Bit '1' = HIGH for 500μs, then LOW for 500μs (falling edge in middle)
    // Bit '0' = LOW for 500μs, then HIGH for 500μs (rising edge in middle)
    // Frame: Start bit (1) + 32 data bits (MSB first) + Stop bit (1) = 34 bits

    constexpr uint32_t HALF_BIT_US = 500;  // 500μs per half-bit

    // Determine actual levels based on invertOutput
    // Normal: active=LOW (0), idle=HIGH (1) for OpenTherm interface
    // With invertOutput: active=HIGH (1), idle=LOW (0)
    const uint32_t activeLevel = invertOutput ? 1 : 0;
    const uint32_t idleLevel = invertOutput ? 0 : 1;

    size_t symbolIdx = 0;

    // Encode 34 bits: start (1) + 32 data bits + stop (1)
    // Build the full 34-bit sequence
    uint64_t fullFrame = 0;
    fullFrame |= (1ULL << 33);                    // Start bit (bit 33)
    fullFrame |= ((uint64_t)frame << 1);          // Data bits (bits 32-1)
    fullFrame |= 1ULL;                             // Stop bit (bit 0)

    for (int i = 33; i >= 0; i--) {
        bool bit = (fullFrame >> i) & 1;

        if (bit) {
            // Bit '1': active->idle (HIGH then LOW normally, but inverted per interface)
            // OpenTherm: '1' = high-to-low transition in middle of bit period
            symbols[symbolIdx].level0 = activeLevel;
            symbols[symbolIdx].duration0 = HALF_BIT_US;
            symbols[symbolIdx].level1 = idleLevel;
            symbols[symbolIdx].duration1 = HALF_BIT_US;
        } else {
            // Bit '0': idle->active (LOW then HIGH normally)
            // OpenTherm: '0' = low-to-high transition in middle of bit period
            symbols[symbolIdx].level0 = idleLevel;
            symbols[symbolIdx].duration0 = HALF_BIT_US;
            symbols[symbolIdx].level1 = activeLevel;
            symbols[symbolIdx].duration1 = HALF_BIT_US;
        }
        symbolIdx++;
    }

    return symbolIdx;  // Should be 34
}

bool OpenTherm::sendFrameRMT(unsigned long frame)
{
    if (!rmtTxChannel_ || !rmtCopyEncoder_) {
        ESP_LOGE("OpenTherm", "RMT TX not initialized");
        return false;
    }

    // Encode the frame to RMT symbols
    size_t numSymbols = encodeFrameToRMT(frame, rmtTxBuffer_);

    // Configure transmission
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,  // No looping
        .flags = {
            .eot_level = static_cast<uint32_t>(invertOutput ? 0 : 1),  // Idle level after transmission
        },
    };

    // Transmit the symbols
    esp_err_t err = rmt_transmit(rmtTxChannel_, rmtCopyEncoder_,
                                  rmtTxBuffer_, numSymbols * sizeof(rmt_symbol_word_t),
                                  &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE("OpenTherm", "RMT transmit failed: %d", err);
        return false;
    }

    // Wait for transmission to complete (34 bits * 1ms = 34ms max)
    err = rmt_tx_wait_all_done(rmtTxChannel_, 50);  // 50ms timeout
    if (err != ESP_OK) {
        ESP_LOGE("OpenTherm", "RMT TX wait failed: %d", err);
        return false;
    }

    return true;
}

bool IRAM_ATTR OpenTherm::isReady()
{
    return status == OpenThermStatus::READY;
}

bool OpenTherm::sendRequestAsync(unsigned long request)
{
    const bool ready = isReady();

    if (!ready)
    {
        return false;
    }

    status = OpenThermStatus::REQUEST_SENDING;
    response = 0;
    responseStatus = OpenThermResponseStatus::NONE;

    // Use RMT for hardware-timed transmission
    if (!sendFrameRMT(request)) {
        status = OpenThermStatus::READY;
        return false;
    }

    responseTimestamp = esp_timer_get_time();
    status = OpenThermStatus::RESPONSE_WAITING;

    return true;
}

unsigned long OpenTherm::sendRequest(unsigned long request)
{
    if (!sendRequestAsync(request))
    {
        return 0;
    }

    while (!isReady())
    {
        process();
        taskYIELD();
    }
    return response;
}

bool OpenTherm::sendResponse(unsigned long request)
{
    // Allow sending response when READY or DELAY (after receiving a request in slave mode)
    const bool canSend = (status == OpenThermStatus::READY || status == OpenThermStatus::DELAY);

    if (!canSend)
    {
        return false;
    }

    status = OpenThermStatus::REQUEST_SENDING;
    response = 0;
    responseStatus = OpenThermResponseStatus::NONE;

    // Use RMT for hardware-timed transmission
    if (!sendFrameRMT(request)) {
        status = OpenThermStatus::READY;
        return false;
    }

    status = OpenThermStatus::READY;
    return true;
}

unsigned long OpenTherm::getLastResponse()
{
    return response;
}

OpenThermResponseStatus OpenTherm::getLastResponseStatus()
{
    return responseStatus;
}


void OpenTherm::monitorInterrupts()
{
    monitorRMT();
}

void OpenTherm::monitorRMT()
{
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 3000,
        .signal_range_max_ns = 2000000,
    };

    while (true) {
        // Wait for notification from RMT callback
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Check if frame is ready (callback sets this)
        if (!rmtFrameReady_) {
            continue;
        }

        // Get frame size and determine which buffer has the completed data
        // The completed buffer is the one NOT currently active (callback swapped it)
        size_t frameSize = rmtFrameSize_;
        uint8_t completedBuffer = 1 - rmtActiveBuffer_;
        rmtFrameReady_ = false;

        // Restart receive ASAP with the new active buffer (before processing)
        esp_err_t err = rmt_receive(rmtChannel_, rmtRxBuffers_[rmtActiveBuffer_],
                                    sizeof(rmtRxBuffers_[0]), &receive_config);
        if (err != ESP_OK) {
            // Channel may need re-enabling
            ESP_LOGW("OpenTherm", "rmt_receive failed (%d), re-enabling channel", err);
            rmt_enable(rmtChannel_);
            rmt_receive(rmtChannel_, rmtRxBuffers_[rmtActiveBuffer_],
                       sizeof(rmtRxBuffers_[0]), &receive_config);
        }

        // Parse the RMT symbols from the completed buffer
        uint32_t parsedFrame = parseRMTSymbols(rmtRxBuffers_[completedBuffer], frameSize);

        if (parsedFrame != 0) {
            response = parsedFrame;
            responseTimestamp = esp_timer_get_time();

            // Validate based on mode: slave expects requests, master expects responses
            bool valid = isSlave ? isValidRequest(parsedFrame, isSlave) : isValidResponse(parsedFrame, isSlave);
            responseStatus = valid ? OpenThermResponseStatus::SUCCESS : OpenThermResponseStatus::INVALID;
            status = OpenThermStatus::RESPONSE_READY;
        } else if (status == OpenThermStatus::RESPONSE_WAITING) {
            // Frame parsing failed while we were expecting a response - mark as invalid
            responseTimestamp = esp_timer_get_time();
            status = OpenThermStatus::RESPONSE_INVALID;
        }
    }
}


unsigned long OpenTherm::process(std::function<void(unsigned long, OpenThermResponseStatus)> callback)
{
    OpenThermStatus st = status;
    unsigned long ts = responseTimestamp;

    if (st == OpenThermStatus::READY)
        return 0;
    unsigned long newTs = esp_timer_get_time();
    if (st != OpenThermStatus::NOT_INITIALIZED && st != OpenThermStatus::DELAY && (newTs - ts) > 1000000)
    {
        status = OpenThermStatus::READY;
        responseStatus = OpenThermResponseStatus::TIMEOUT;
        if (callback) callback(response, responseStatus);
        return response;
    }
    else if (st == OpenThermStatus::RESPONSE_INVALID)
    {
        status = OpenThermStatus::DELAY;
        responseStatus = OpenThermResponseStatus::INVALID;
        if (callback) callback(response, responseStatus);
        return response;
    }
    else if (st == OpenThermStatus::RESPONSE_READY)
    {
        status = OpenThermStatus::DELAY;
        responseStatus = (isSlave ? isValidRequest(response, isSlave) : isValidResponse(response, isSlave)) ? OpenThermResponseStatus::SUCCESS : OpenThermResponseStatus::INVALID;
        if (callback) callback(response, responseStatus);
        return response;
    }
    else if (st == OpenThermStatus::DELAY)
    {
        if ((newTs - ts) > (isSlave ? 20000 : 100000))
        {
            status = OpenThermStatus::READY;
        }
    }
    return 0;
}

bool OpenTherm::parity(unsigned long frame) // odd parity
{
    uint8_t p = 0;
    while (frame > 0)
    {
        if (frame & 1)
            p++;
        frame = frame >> 1;
    }
    return (p & 1);
}

OpenThermMessageType OpenTherm::getMessageType(unsigned long message)
{
    OpenThermMessageType msg_type = static_cast<OpenThermMessageType>((message >> 28) & 7);
    return msg_type;
}

OpenThermMessageID OpenTherm::getDataID(unsigned long frame)
{
    return (OpenThermMessageID)((frame >> 16) & 0xFF);
}

unsigned long OpenTherm::buildRequest(OpenThermMessageType type, OpenThermMessageID id, unsigned int data)
{
    unsigned long request = data;
    if (type == OpenThermMessageType::WRITE_DATA)
    {
        request |= 1ul << 28;
    }
    request |= ((unsigned long)id) << 16;
    if (parity(request))
        request |= (1ul << 31);
    return request;
}

unsigned long OpenTherm::buildResponse(OpenThermMessageType type, OpenThermMessageID id, unsigned int data)
{
    unsigned long response = data;
    response |= ((unsigned long)type) << 28;
    response |= ((unsigned long)id) << 16;
    if (parity(response))
        response |= (1ul << 31);
    return response;
}

bool OpenTherm::isValidResponse(unsigned long response, bool isSlave)
{
    if (parity(response)) {
        ESP_LOGW("OT", "%s Invalid response (parity): %lu", isSlave ? "T" : "B", response);
        return false;
    }
    
    uint8_t msgType = (response << 1) >> 29;
    bool valid = msgType == (uint8_t)OpenThermMessageType::READ_ACK || msgType == (uint8_t)OpenThermMessageType::WRITE_ACK;
    if (!valid) {
        ESP_LOGW("OT", "%s Invalid response (type): %lu", isSlave ? "T" : "B", response);
    }
    return valid;
}

bool OpenTherm::isValidRequest(unsigned long request, bool isSlave)
{
    if (parity(request)) {
        ESP_LOGW("OT", "%s Invalid request (parity): %lu", isSlave ? "T" : "B", request);
        return false;
    }
    uint8_t msgType = (request << 1) >> 29;
    bool valid = msgType == (uint8_t)OpenThermMessageType::READ_DATA || msgType == (uint8_t)OpenThermMessageType::WRITE_DATA;
    if (!valid) {
        ESP_LOGW("OT", "%s Invalid request (type): %lu", isSlave ? "T" : "B", request);
    }
    return valid;
}

void OpenTherm::end()
{
    // Clean up RX channel
    if (rmtChannel_) {
        ESP_ERROR_CHECK(rmt_disable(rmtChannel_));
        ESP_ERROR_CHECK(rmt_del_channel(rmtChannel_));
        rmtChannel_ = nullptr;
    }
    // Clean up TX channel
    if (rmtTxChannel_) {
        ESP_ERROR_CHECK(rmt_disable(rmtTxChannel_));
        ESP_ERROR_CHECK(rmt_del_channel(rmtTxChannel_));
        rmtTxChannel_ = nullptr;
    }
    // Clean up encoder
    if (rmtCopyEncoder_) {
        ESP_ERROR_CHECK(rmt_del_encoder(rmtCopyEncoder_));
        rmtCopyEncoder_ = nullptr;
    }

    if (monitorTaskHandle_ != nullptr) {
        vTaskDelete(monitorTaskHandle_);
        monitorTaskHandle_ = nullptr;
    }
}

OpenTherm::~OpenTherm()
{
    end();
}

const char *OpenTherm::statusToString(OpenThermResponseStatus status)
{
    switch (status)
    {
    case OpenThermResponseStatus::NONE:
        return "NONE";
    case OpenThermResponseStatus::SUCCESS:
        return "SUCCESS";
    case OpenThermResponseStatus::INVALID:
        return "INVALID";
    case OpenThermResponseStatus::TIMEOUT:
        return "TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

const char *OpenTherm::messageTypeToString(OpenThermMessageType message_type)
{
    switch (message_type)
    {
    case OpenThermMessageType::READ_DATA:
        return "READ_DATA";
    case OpenThermMessageType::WRITE_DATA:
        return "WRITE_DATA";
    case OpenThermMessageType::INVALID_DATA:
        return "INVALID_DATA";
    case OpenThermMessageType::RESERVED:
        return "RESERVED";
    case OpenThermMessageType::READ_ACK:
        return "READ_ACK";
    case OpenThermMessageType::WRITE_ACK:
        return "WRITE_ACK";
    case OpenThermMessageType::DATA_INVALID:
        return "DATA_INVALID";
    case OpenThermMessageType::UNKNOWN_DATA_ID:
        return "UNKNOWN_DATA_ID";
    default:
        return "UNKNOWN";
    }
}

// building requests

unsigned long OpenTherm::buildSetBoilerStatusRequest(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2)
{
    unsigned int data = enableCentralHeating | (enableHotWater << 1) | (enableCooling << 2) | (enableOutsideTemperatureCompensation << 3) | (enableCentralHeating2 << 4);
    data <<= 8;
    return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Status, data);
}

unsigned long OpenTherm::buildSetBoilerTemperatureRequest(float temperature)
{
    unsigned int data = temperatureToData(temperature);
    return buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TSet, data);
}

unsigned long OpenTherm::buildGetBoilerTemperatureRequest()
{
    return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tboiler, 0);
}

// parsing responses
bool OpenTherm::isFault(unsigned long response)
{
    return response & 0x1;
}

bool OpenTherm::isCentralHeatingActive(unsigned long response)
{
    return response & 0x2;
}

bool OpenTherm::isHotWaterActive(unsigned long response)
{
    return response & 0x4;
}

bool OpenTherm::isFlameOn(unsigned long response)
{
    return response & 0x8;
}

bool OpenTherm::isCoolingActive(unsigned long response)
{
    return response & 0x10;
}

bool OpenTherm::isDiagnostic(unsigned long response)
{
    return response & 0x40;
}

uint16_t OpenTherm::getUInt(const unsigned long response)
{
    const uint16_t u88 = response & 0xffff;
    return u88;
}

float OpenTherm::getFloat(const unsigned long response)
{
    const uint16_t u88 = getUInt(response);
    const float f = (u88 & 0x8000) ? -(0x10000L - u88) / 256.0f : u88 / 256.0f;
    return f;
}

unsigned int OpenTherm::temperatureToData(float temperature)
{
    if (temperature < 0)
        temperature = 0;
    if (temperature > 100)
        temperature = 100;
    unsigned int data = (unsigned int)(temperature * 256);
    return data;
}

// basic requests

unsigned long OpenTherm::setBoilerStatus(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2)
{
    return sendRequest(buildSetBoilerStatusRequest(enableCentralHeating, enableHotWater, enableCooling, enableOutsideTemperatureCompensation, enableCentralHeating2));
}

bool OpenTherm::setBoilerTemperature(float temperature)
{
    unsigned long response = sendRequest(buildSetBoilerTemperatureRequest(temperature));
    return isValidResponse(response, isSlave);
}

float OpenTherm::getBoilerTemperature()
{
    unsigned long response = sendRequest(buildGetBoilerTemperatureRequest());
    return isValidResponse(response, isSlave) ? getFloat(response) : 0;
}

float OpenTherm::getReturnTemperature()
{
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Tret, 0));
    return isValidResponse(response, isSlave) ? getFloat(response) : 0;
}

bool OpenTherm::setDHWSetpoint(float temperature)
{
    unsigned int data = temperatureToData(temperature);
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TdhwSet, data));
    return isValidResponse(response, isSlave);
}

float OpenTherm::getDHWTemperature()
{
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tdhw, 0));
    return isValidResponse(response, isSlave) ? getFloat(response) : 0;
}

float OpenTherm::getModulation()
{
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::RelModLevel, 0));
    return isValidResponse(response, isSlave) ? getFloat(response) : 0;
}

float OpenTherm::getPressure()
{
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::CHPressure, 0));
    return isValidResponse(response, isSlave) ? getFloat(response) : 0;
}

unsigned char OpenTherm::getFault()
{
    return ((sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::ASFflags, 0)) >> 8) & 0xff);
}

uint32_t OpenTherm::parseRMTSymbols(rmt_symbol_word_t* symbols, size_t num_symbols)
{
    // Log RMT symbols in compact form: "L:dur,H:dur,..." (L=low, H=high)
    // Build a compact string showing level and duration for each symbol part
    // Each entry can be up to ~10 chars (e.g. ",H900000"), 128 symbols × 2 parts max = ~2560 bytes
    char logBuf[3072];
    int logPos = 0;
    for (size_t i = 0; i < num_symbols && logPos < (int)sizeof(logBuf) - 12; i++) {
        rmt_symbol_word_t symbol = symbols[i];
        // Log both parts of each symbol
        for (int part = 0; part < 2 && logPos < (int)sizeof(logBuf) - 12; part++) {
            uint32_t dur = (part == 0) ? symbol.duration0 : symbol.duration1;
            uint32_t level = (part == 0) ? symbol.level0 : symbol.level1;
            int written = snprintf(logBuf + logPos, sizeof(logBuf) - logPos,
                                   "%s%c%lu", (i > 0 || part > 0 ? "," : ""), (level ? 'H' : 'L'), (unsigned long)dur);
            if (written > 0) logPos += written;
        }
    }
    // ESP_LOGI("OT", "%s RMT[%d]: %s", isSlave ? "T" : "B", num_symbols, logBuf);

    // Manchester decoding for OpenTherm using RMT symbols
    // Each RMT symbol represents a pulse: level0 for duration0, then level1 for duration1
    // For Manchester: bit '1' = high->low, bit '0' = low->high

    constexpr int HALF_US_NOM = 500;  // Nominal half-bit duration
    constexpr int ONE_MIN = 300;      // Minimum half-bit duration
    constexpr int ONE_MAX = 700;      // Maximum half-bit duration
    constexpr int TWO_MIN = 700;      // Minimum 2 half-bits
    constexpr int TWO_MAX = 1300;     // Maximum 2 half-bits

    // We expect 68 half-bits for a complete frame (1 start + 32 data + 1 stop bits × 2)
    bool halfLevels[68];
    int halfCount = 0;
    bool skipFirstPart = false;

    // Check if we need to prepend an implicit HIGH half-bit
    // When idle is HIGH and first data symbol is LOW, the start bit's first half is merged with idle
    if (num_symbols > 0) {
        rmt_symbol_word_t first = symbols[0];
        if (first.level0 == 1 && first.duration0 > 0) {
            // First symbol starts HIGH - check if second part is LOW
            if (first.level1 == 0 && first.duration1 > 0) {
                // Idle HIGH merged with start bit - prepend implicit HIGH half
                // and skip the first H part (which is the merged idle)
                halfLevels[halfCount++] = 1;
                skipFirstPart = true;
            }
        }
    }

    // Process each RMT symbol
    for (size_t i = 0; i < num_symbols && halfCount < 68; i++) {
        rmt_symbol_word_t symbol = symbols[i];

        // Each symbol has two parts: duration0/level0 and duration1/level1
        uint32_t durations[2] = {symbol.duration0, symbol.duration1};
        uint32_t levels[2] = {symbol.level0, symbol.level1};

        // Process each part of the symbol
        for (int part = 0; part < 2 && halfCount < 68; part++) {
            // Skip the first part of first symbol if we prepended implicit HIGH
            if (i == 0 && part == 0 && skipFirstPart) {
                continue;
            }

            uint32_t dur = durations[part];
            bool level = levels[part];

            // Skip duration-0 entries (RMT end-of-frame marker or noise)
            if (dur == 0) continue;

            // Skip very short pulses (noise) but not zero
            if (dur < 100) continue;

            int halves = 0;
            if (dur >= ONE_MIN && dur < ONE_MAX) {
                halves = 1;
            } else if (dur >= TWO_MIN && dur <= TWO_MAX) {
                halves = 2;
            } else {
                ESP_LOGW("OT", "RMT Invalid duration: %lu μs for symbol %d FRAME: %s", dur, i, logBuf);
                return 0;  // Invalid frame
            }

            // Add the appropriate number of half-bits
            for (int k = 0; k < halves && halfCount < 68; k++) {
                halfLevels[halfCount++] = level;
            }
        }
    }

    // If we're exactly 1 short and last level was HIGH, infer final LOW half-bit
    // (stop bit second half isn't captured because signal stays LOW with no edge)
    if (halfCount == 67 && halfLevels[66] == 1) {
        halfLevels[halfCount++] = 0;  // Infer final LOW half-bit
    }

    // We need exactly 68 half-bits for a complete frame
    if (halfCount != 68) {
        ESP_LOGW("OT", "RMT Half count mismatch: %d (expected 68) FRAME: %s", halfCount, logBuf);
        return 0;
    }

    // Decode Manchester: pairs of half-bits form one data bit
    uint8_t bits[34];
    for (int b = 0; b < 34; b++) {
        bool a = halfLevels[2 * b];     // First half-bit
        bool c = halfLevels[2 * b + 1]; // Second half-bit

        if (a == c) {
            ESP_LOGW("OT", "RMT Invalid Manchester (no transition): %d FRAME: %s", b, logBuf);
            return 0;
        }

        // Manchester encoding: high->low = '1', low->high = '0'
        if (a == 1 && c == 0) bits[b] = 1;      // Falling edge = '1'
        else if (a == 0 && c == 1) bits[b] = 0; // Rising edge = '0'
        else {
            ESP_LOGW("OT", "RMT Invalid Manchester (wrong transition): %d FRAME: %s", b, logBuf);
            return 0;
        }
    }

    // Start and stop bits must be '1'
    if (bits[0] != 1) {
        ESP_LOGW("OT", "RMT Invalid start bit: %d FRAME: %s", bits[0], logBuf);
        return 0;
    }
    if (bits[33] != 1) {
        ESP_LOGW("OT", "RMT Invalid stop bit: %d FRAME: %s", bits[33], logBuf);
        return 0;
    }

    // Build 32-bit frame from bits[1..32], MSB first
    uint32_t frame = 0;
    for (int b = 1; b <= 32; b++) {
        frame = (frame << 1) | (bits[b] & 1u);
    }

    // Even parity check on the 32 bits
    if (__builtin_popcount(frame) & 1) {
        ESP_LOGW("OT", "RMT Invalid parity: %d ones FRAME: %s", __builtin_popcount(frame), logBuf);
        return 0;
    }

    return frame;
}

} // namespace ot
