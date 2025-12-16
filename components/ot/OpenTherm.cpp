/*
OpenTherm.cpp - OpenTherm Communication Library for ESP-IDF
Copyright 2023, Ihor Melnyk
ESP-IDF port
*/

#include "OpenTherm.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

// Helper macro for bit reading
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

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
    useRMT_(false),
    rmtActiveBuffer_(0),
    rmtFrameSize_(0),
    rmtFrameReady_(false)
{
    memset(rmtRxBuffers_, 0, sizeof(rmtRxBuffers_));
}

void OpenTherm::begin(bool useRMT)
{
    useRMT_ = useRMT;

    // Configure output pin (common for both modes)
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << outPin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    if (useRMT) {
        // Configure input pin for RMT (no internal pull-up - OpenTherm interface has its own)
        gpio_config_t in_conf = {
            .pin_bit_mask = (1ULL << inPin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,  // No GPIO interrupts when using RMT
        };
        gpio_config(&in_conf);

        // Initialize RMT
        initRMT();

        // Create monitoring task (for RMT data processing) - though modern RMT uses callbacks
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
    } else {
        // Configure input pin for GPIO interrupts (no internal pull-up - OpenTherm interface has its own)
        gpio_config_t in_conf = {
            .pin_bit_mask = (1ULL << inPin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&in_conf);

        // Create monitoring task before enabling interrupts
        BaseType_t ret = xTaskCreatePinnedToCore(
            monitorTaskEntry,
            "ot_monitor",
            4096, // Stack size (bytes) - adjust as needed
            this,
            configMAX_PRIORITIES - 1, // High priority
            &monitorTaskHandle_,
            0 // Core ID (0 or 1 on ESP32, or tskNO_AFFINITY)
        );

        if (ret != pdPASS) {
            ESP_LOGE("OpenTherm", "Failed to create monitor task");
            monitorTaskHandle_ = nullptr;
        }

        // Install ISR service if not already installed
        gpio_install_isr_service(0);

        gpio_isr_handler_add(inPin, OpenTherm::handleInterruptHelper, this);
    }

    activateBoiler();
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
    ESP_LOGI("OpenTherm", "RMT channel enabled");
}

void OpenTherm::startRMTReceive()
{
    if (useRMT_ && rmtChannel_) {
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
        ESP_LOGE("OpenTherm", "RMT not initialized: useRMT_=%d, rmtChannel_=%p", useRMT_, rmtChannel_);
    }
}

void OpenTherm::stopRMTReceive()
{
    if (useRMT_ && rmtChannel_) {
        ESP_ERROR_CHECK(rmt_disable(rmtChannel_));
    }
}

bool IRAM_ATTR OpenTherm::isReady()
{
    return status == OpenThermStatus::READY;
}

int IRAM_ATTR OpenTherm::readState()
{
    return gpio_get_level(inPin);
}

void OpenTherm::setActiveState()
{
    gpio_set_level(outPin, invertOutput ? 1 : 0);
}

void OpenTherm::setIdleState()
{
    gpio_set_level(outPin, invertOutput ? 0 : 1);
}

void OpenTherm::activateBoiler()
{
    setIdleState();
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void OpenTherm::sendBit(bool high)
{
    if (high)
        setActiveState();
    else
        setIdleState();
    esp_rom_delay_us(500);
    if (high)
        setIdleState();
    else
        setActiveState();
    esp_rom_delay_us(500);
}

bool OpenTherm::sendRequestAsync(unsigned long request)
{
    //portDISABLE_INTERRUPTS();
    const bool ready = isReady();

    if (!ready)
    {
        //portENABLE_INTERRUPTS();
        return false;
    }

    status = OpenThermStatus::REQUEST_SENDING;
    response = 0;
    responseStatus = OpenThermResponseStatus::NONE;

    BaseType_t schedulerState = xTaskGetSchedulerState();
    if (schedulerState == taskSCHEDULER_RUNNING)
    {
        vTaskSuspendAll();
    }

    //portENABLE_INTERRUPTS();

    sendBit(true); // start bit
    for (int i = 31; i >= 0; i--)
    {
        sendBit(bitRead(request, i));
    }
    sendBit(true); // stop bit
    setIdleState();

    responseTimestamp = esp_timer_get_time();
    status = OpenThermStatus::RESPONSE_WAITING;

    if (schedulerState == taskSCHEDULER_RUNNING) {
        xTaskResumeAll();
    }

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
    portDISABLE_INTERRUPTS();
    // Allow sending response when READY or DELAY (after receiving a request in slave mode)
    const bool canSend = (status == OpenThermStatus::READY || status == OpenThermStatus::DELAY);

    if (!canSend)
    {
        portENABLE_INTERRUPTS();
        return false;
    }

    status = OpenThermStatus::REQUEST_SENDING;
    response = 0;
    responseStatus = OpenThermResponseStatus::NONE;

    BaseType_t schedulerState = xTaskGetSchedulerState();
    if (schedulerState == taskSCHEDULER_RUNNING)
    {
        vTaskSuspendAll();
    }

    portENABLE_INTERRUPTS();

    sendBit(true); // start bit
    for (int i = 31; i >= 0; i--)
    {
        sendBit(bitRead(request, i));
    }
    sendBit(true); // stop bit
    setIdleState();
    status = OpenThermStatus::READY;

    if (schedulerState == taskSCHEDULER_RUNNING) {
        xTaskResumeAll();
    }

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

constexpr uint32_t STATE_BIT = 1 << 31;
constexpr uint32_t DUR_MASK  = ~STATE_BIT;

void IRAM_ATTR OpenTherm::handleInterrupt()
{
    // Edge ISR: current pin level is the *new* level,
    // duration belongs to the *previous* level
    const bool prevLevel = !readState();
    
    interruptCount++;

    uint64_t currentTimestamp = esp_timer_get_time();
    auto delta = static_cast<uint32_t>(currentTimestamp - lastReceptionTimestamp);

    uint32_t saveInterrupt = (prevLevel ? STATE_BIT : 0) | (delta & DUR_MASK);

    // Reset to the start of the frame on long gap (inter-frame idle)
    // A complete frame is ~34ms, so any gap > 50ms is definitely between frames
    if (delta > 45000) {
        interruptsUpToIndex = 0;
    }

    interruptTimestamps[interruptsUpToIndex] = saveInterrupt;
    interruptsUpToIndex = (interruptsUpToIndex + 1) % 128;

    lastReceptionTimestamp = currentTimestamp;

    // In slave mode, transition to RESPONSE_WAITING when we see activity during READY state
    if (isReady() && isSlave && prevLevel == 0) {
        status = OpenThermStatus::RESPONSE_WAITING;
        responseTimestamp = currentTimestamp;
    }

    // Signal to the monitoring task (frame parsing happens there)
    if (monitorTaskHandle_ != nullptr) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(monitorTaskHandle_, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

void IRAM_ATTR OpenTherm::handleInterruptHelper(void* ptr)
{
    static_cast<OpenTherm*>(ptr)->handleInterrupt();
}

void OpenTherm::monitorInterrupts()
{
    if (useRMT_) {
        monitorRMT();
    } else {
        monitorGPIOInterrupts();
    }
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

void OpenTherm::monitorGPIOInterrupts()
{
    // Frame parsing task - waits for interrupts, then parses complete frames
    while (true) {
        // Wait for first interrupt (start of potential frame)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Wait for frame to complete - keep consuming notifications until
        // no more arrive for 5ms (indicates end of frame, since bits are ~1ms each)
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5)) > 0) {
            // Keep consuming notifications until timeout
        }

        int currentIndex = interruptsUpToIndex;
        if (currentIndex < 35) {
            // Not enough interrupts for a complete frame, skip parsing
            // (minimum is ~35 for alternating bit patterns, typical is 50-65)
            continue;
        }

        auto parsedFrame = parseInterrupts(interruptTimestamps, currentIndex);

        // Log outside critical section (ESP_LOGI requires locks)
        if (parsedFrame != 0) {
            //logU32Bin(parsedFrame);
        }

        // Update response and status (with interrupt protection for thread safety)
        //portDISABLE_INTERRUPTS();
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
        //portENABLE_INTERRUPTS();
    }
}

unsigned long OpenTherm::process(std::function<void(unsigned long, OpenThermResponseStatus)> callback)
{
    //portDISABLE_INTERRUPTS();
    OpenThermStatus st = status;
    unsigned long ts = responseTimestamp;
    auto currentInterrupIdx = interruptsUpToIndex;
    //portENABLE_INTERRUPTS();

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
    if (useRMT_) {
        if (rmtChannel_) {
            ESP_ERROR_CHECK(rmt_disable(rmtChannel_));
            ESP_ERROR_CHECK(rmt_del_channel(rmtChannel_));
            rmtChannel_ = nullptr;
        }
    } else {
        gpio_isr_handler_remove(inPin);
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


static inline bool seg_level(uint32_t packed) {
    return (packed & STATE_BIT) != 0;               // MSB is level
}
static inline uint32_t seg_dur_us(uint32_t packed) {
    return packed & DUR_MASK;
}

static inline int popcount32(uint32_t v) {
#if defined(__GNUC__)
    return __builtin_popcount(v);
#else
    int c = 0;
    while (v) { c += (v & 1u); v >>= 1; }
    return c;
#endif
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

uint32_t OpenTherm::parseInterrupts(uint32_t interrupts[128], int upToIndex)
{
    // Log interrupts in compact form: "L:dur,H:dur,..." (L=low, H=high)
    // Build a compact string showing level and duration for each segment
    // Each entry can be up to ~10 chars (e.g. ",H900000"), 128 entries max = ~1280 bytes
    char logBuf[1536];
    int logPos = 0;
    for (int i = 0; i < upToIndex && logPos < (int)sizeof(logBuf) - 12; i++) {
        const bool level = seg_level(interrupts[i]);
        const uint32_t dur = seg_dur_us(interrupts[i]);
        int written = snprintf(logBuf + logPos, sizeof(logBuf) - logPos,
                               "%s%c%lu", (i > 0 ? "," : ""), (level ? 'H' : 'L'), (unsigned long)dur);
        if (written > 0) logPos += written;
    }
    // ESP_LOGI("OT", "%s IRQ[%d]: %s", isSlave ? "T" : "B", upToIndex, logBuf);

    // Manchester half-bit is ~500us (bit is ~1000us).
    constexpr int HALF_US_NOM = 500;

    // Loose tolerances: classify segment as 1 half-bit (~500us) or 2 half-bits (~1000us).
    constexpr int ONE_MIN = 300;
    constexpr int ONE_MAX = 700;
    constexpr int TWO_MIN = 700;
    constexpr int TWO_MAX = 1300;

    // We want start(1) + 32 frame + stop(1) = 34 bits => 68 half-bits.
    bool halfLevels[68];
    int halfCount = 0;

    // Glitch filter threshold - pulses shorter than this are noise
    constexpr int GLITCH_MAX_US = 200;

    // Check if start bit first half merged with idle.
    // When idle is HIGH and start bit first half is also HIGH, they merge.
    // We detect this when: idle (index 0) is HIGH, and first data entry is LOW.
    // In this case, prepend an implicit HIGH half-bit for the start bit first half.
    if (upToIndex > 1 && seg_level(interrupts[0]) && !seg_level(interrupts[1])) {
        halfLevels[halfCount++] = 1;  // Implicit start bit first half (HIGH)
    }

    // Expect first entry to be the long idle period before the start bit
    // We'll just skip index 0 unconditionally.
    for (int i = 1; i < upToIndex && halfCount < 68; i++) {
        bool level = seg_level(interrupts[i]);
        uint32_t dur = seg_dur_us(interrupts[i]);

        // Handle very short glitch pulses (e.g., H76 between L938 and L516)
        // These can be noise, or squished transitions that need restoration
        if (dur < GLITCH_MAX_US) {
            // Check if restoring this glitch would prevent a Manchester error
            // If prev half and next segment are same level, and this glitch is opposite,
            // restore it as 1 half-bit to maintain valid Manchester encoding
            if (halfCount > 0 && i + 1 < upToIndex) {
                bool prevHalfLevel = halfLevels[halfCount - 1];
                bool nextLevel = seg_level(interrupts[i + 1]);
                uint32_t nextDur = seg_dur_us(interrupts[i + 1]);

                if (prevHalfLevel == nextLevel && nextDur >= ONE_MIN && level != prevHalfLevel) {
                    // Check if previous segment was "stretched" (borderline 2-half that should be 1-half)
                    // This happens when a glitch squishes: the time gets redistributed to neighbors
                    // Pattern: L938,H76,L516 -> should be 3 half-bits (L,H,L), not 4 (L,L,H,L)
                    if (i > 1) {
                        uint32_t prevSegDur = seg_dur_us(interrupts[i - 1]);
                        // If prev segment was stretched (e.g., L938 counted as 2 halves), remove one half
                        if (prevSegDur >= 700 && prevSegDur <= 1100 && halfCount >= 2 && halfLevels[halfCount - 2] == prevHalfLevel) {
                            halfCount--;  // Remove one of the stretched segment's halves
                        }
                    }
                    halfLevels[halfCount++] = level;  // Restore glitch as 1 half-bit
                }
            }
            continue;
        }

        // Merge consecutive entries with the same level (caused by ISR timing glitches).
        // E.g., L506,L5,H502 -> L511,H502 (the L5 is a spurious edge from noise)
        // BUT: check for pattern where a glitch separates same-level segments that shouldn't merge.
        // E.g., L987,L514,H4,L482 -> the L514 was misread and should be H514 (flip and merge with H4)
        while (i + 1 < upToIndex && seg_level(interrupts[i + 1]) == level) {
            uint32_t nextDur = seg_dur_us(interrupts[i + 1]);

            // Check for flip pattern: next segment is same level, followed by glitch of opposite level,
            // followed by segment of same level as current. This suggests the next segment was misread.
            // Pattern: L987(current), L514(i+1), H4(i+2), L482(i+3)
            if (nextDur >= ONE_MIN && nextDur <= ONE_MAX &&   // i+1 is ~1 half-bit duration
                i + 2 < upToIndex &&
                seg_dur_us(interrupts[i + 2]) < GLITCH_MAX_US &&  // i+2 is a glitch
                seg_level(interrupts[i + 2]) != level &&          // glitch has opposite level
                i + 3 < upToIndex &&
                seg_level(interrupts[i + 3]) == level) {          // i+3 back to original level
                // Don't merge - stop here and let the flip logic handle it after
                break;
            }

            i++;
            dur += seg_dur_us(interrupts[i]);
        }

        // After processing current segment, check if next should be flipped.
        // Pattern: we just processed L987, now check if L514,H4 at i+1,i+2 should become H518
        if (i + 2 < upToIndex &&
            seg_level(interrupts[i + 1]) == level &&              // i+1 same level as current
            seg_dur_us(interrupts[i + 1]) >= ONE_MIN &&
            seg_dur_us(interrupts[i + 1]) <= ONE_MAX &&           // i+1 is ~1 half-bit
            seg_dur_us(interrupts[i + 2]) < GLITCH_MAX_US &&      // i+2 is glitch
            seg_level(interrupts[i + 2]) != level) {              // glitch is opposite level

            bool glitchLevel = seg_level(interrupts[i + 2]);
            uint32_t flippedDur = seg_dur_us(interrupts[i + 1]) + seg_dur_us(interrupts[i + 2]);

            // First, add halves for current segment
            int halves = 0;
            if (dur >= ONE_MIN && dur < ONE_MAX) halves = 1;
            else if (dur >= TWO_MIN && dur <= TWO_MAX) halves = 2;
            else {
                ESP_LOGW("OT", "%s Invalid duration: %lu for index %d FRAME: %s", 
                    isSlave ? "T" : "B", (unsigned long)dur, i, logBuf);
                return 0;
            }
            for (int k = 0; k < halves && halfCount < 68; ++k) {
                halfLevels[halfCount++] = level;
            }

            // Now add the flipped segment
            if (flippedDur >= ONE_MIN && flippedDur < ONE_MAX) {
                halfLevels[halfCount++] = glitchLevel;
            } else if (flippedDur >= TWO_MIN && flippedDur <= TWO_MAX) {
                halfLevels[halfCount++] = glitchLevel;
                if (halfCount < 68) halfLevels[halfCount++] = glitchLevel;
            }
            // Skip both the misread segment and the glitch
            i += 2;
            continue;
        }

        int halves = 0;
        if (dur >= ONE_MIN && dur < ONE_MAX) halves = 1;
        else if (dur >= TWO_MIN && dur <= TWO_MAX) halves = 2;
        else {
            // Allow a final trailing idle to be long; stop if we already have enough
            // or treat as invalid if it appears mid-frame.
            ESP_LOGW("OT", "%s Invalid duration: %lu for index %d FRAME: %s",
                 isSlave ? "T" : "B", (unsigned long)dur, i, logBuf);
            return 0;
        }

        for (int k = 0; k < halves && halfCount < 68; ++k) {
            halfLevels[halfCount++] = level;
        }
    }

    // The last half-bit (stop bit second half, LOW) may not be captured because
    // the line stays at idle (LOW) with no edge to trigger an interrupt.
    // If we're exactly 1 short and the last recorded level was HIGH, infer the final LOW half.
    if (halfCount == 67 && halfLevels[66] == 1) {
        halfLevels[halfCount++] = 0;  // Infer final LOW half-bit
    }

    if (halfCount != 68) {
        ESP_LOGW("OT", "%s Half count mismatch: %d FRAME: %s", isSlave ? "T" : "B", halfCount, logBuf);
        return 0;
    }

    // Decode 34 Manchester bits from pairs of half-levels.
    // With your described start: idle low -> high then low indicates first bit=1,
    // we assume: active=HIGH, idle=LOW, and:
    //   bit '1' = active-to-idle = HIGH->LOW
    //   bit '0' = idle-to-active = LOW->HIGH :contentReference[oaicite:2]{index=2}
    uint8_t bits[34];
    for (int b = 0; b < 34; ++b) {
        const bool a = halfLevels[2 * b];
        const bool c = halfLevels[2 * b + 1];
        if (a == c) {
            ESP_LOGW("OT", "%s Invalid Manchester (two same bits): %d FRAME: %s", isSlave ? "T" : "B", b, logBuf);
            return 0;
        }

        if (a == 1 && c == 0) bits[b] = 1;
        else if (a == 0 && c == 1) bits[b] = 0;
        else {
            ESP_LOGW("OT", "%s Invalid Manchester (no mid-bit transition): %d FRAME: %s", isSlave ? "T" : "B", b, logBuf);
            return 0;
        }
    }

    // Start/stop bits must be '1' (outside the 32-bit frame). :contentReference[oaicite:3]{index=3}
    if (bits[0] != 1) {
        ESP_LOGW("OT", "%s Invalid start bit: %d FRAME: %s", isSlave ? "T" : "B", bits[0], logBuf);
        return 0;
    }
    if (bits[33] != 1) {
        ESP_LOGW("OT", "%s Invalid stop bit: %d FRAME: %s", isSlave ? "T" : "B", bits[33], logBuf);
        return 0;
    }

    // Build the 32-bit frame from bits[1..32], MSB first.
    uint32_t frame = 0;
    for (int b = 1; b <= 32; ++b) {
        frame = (frame << 1) | (bits[b] & 1u);
    }

    // Parity: total number of '1' bits in entire 32 bits must be even. :contentReference[oaicite:4]{index=4}
    if ((popcount32(frame) & 1) != 0) {
        ESP_LOGW("OT", "%s Invalid parity: %d FRAME: %s", isSlave ? "T" : "B", popcount32(frame), logBuf);
        return 0;
    }

    return frame;
}

void OpenTherm::logU32Bin(uint32_t v)
{
    char buf[33];
    for (int i = 31; i >= 0; --i) {
        buf[31 - i] = (v & (1u << i)) ? '1' : '0';
    }
    buf[32] = '\0';
    //ESP_LOGI("OT", "Incoming frame from %s: %s", isSlave ? "T" : "B", buf);
}

} // namespace ot
