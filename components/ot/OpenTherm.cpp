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

// Helper macro for bit reading
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

static void monitorTaskEntry(void* pvParameters) {
    ot::OpenTherm* otInstance = static_cast<ot::OpenTherm*>(pvParameters);
    otInstance->monitorInterrupts();
    vTaskDelete(nullptr);
}

namespace ot {

OpenTherm::OpenTherm(gpio_num_t inPin, gpio_num_t outPin, bool isSlave, bool invertOutput, bool invertInput) :
    status(OpenThermStatus::NOT_INITIALIZED),
    inPin(inPin),
    outPin(outPin),
    isSlave(isSlave),
    invertOutput(invertOutput),
    invertInput(invertInput),
    response(0),
    responseStatus(OpenThermResponseStatus::NONE),
    responseTimestamp(0)
{
}

void OpenTherm::begin()
{
    // Configure input pin (no internal pull-up - OpenTherm interface has its own)
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << inPin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&in_conf);

    // Configure output pin
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << outPin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    // Install ISR service if not already installed
    gpio_install_isr_service(0);

    gpio_isr_handler_add(inPin, OpenTherm::handleInterruptHelper, this);
    activateBoiler();
    status = OpenThermStatus::READY;

    // Create monitoring task
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
}

bool IRAM_ATTR OpenTherm::isReady()
{
    return status == OpenThermStatus::READY;
}

int IRAM_ATTR OpenTherm::readState()
{
    int level = gpio_get_level(inPin);
    return invertInput ? !level : level;
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
    portDISABLE_INTERRUPTS();
    const bool ready = isReady();

    if (!ready)
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
    interruptCount++;

    // Precise bit tracking for debugging
    {
        uint64_t currentTimestamp = esp_timer_get_time();
        auto delta = static_cast<uint32_t>(currentTimestamp - lastReceptionTimestamp);

        // Edge ISR: current pin level is the *new* level,
        // duration belongs to the *previous* level
        const bool prevLevel = !readState();
        uint32_t saveInterrupt = (prevLevel ? STATE_BIT : 0) | (delta & DUR_MASK);

        interruptTimestamps[interruptsUpToIndex] = saveInterrupt;
        interruptsUpToIndex = (interruptsUpToIndex + 1) % 68;

        lastReceptionTimestamp = currentTimestamp;

        // signal to the monitoring thread
        if (monitorTaskHandle_ != nullptr) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            vTaskNotifyGiveFromISR(monitorTaskHandle_, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        }
    }

    if (isReady())
    {
        if (isSlave && readState() == 1)
        {
            status = OpenThermStatus::RESPONSE_WAITING;
        }
        else
        {
            return;
        }
    }

    unsigned long newTs = esp_timer_get_time();
    if (status == OpenThermStatus::RESPONSE_WAITING)
    {
        // For inverted hardware (idle=0 on GPIO), mid-bit of start bit is falling edge (state=0)
        // For standard hardware (idle=1 on GPIO), mid-bit of start bit is rising edge (state=1)
        // Since we've established hardware is inverted (gpio=0 at idle), trigger on state=0
        if (readState() == 0)
        {
            status = OpenThermStatus::RESPONSE_START_BIT;
            responseTimestamp = newTs;
        }
        // Ignore rising edges (start of start bit with inverted hardware)
    }
    else if (status == OpenThermStatus::RESPONSE_START_BIT)
    {
        // After start bit rising edge (at T=500us into start bit), wait for mid-bit edge of bit 31
        // The mid-bit edge should come at ~1000us elapsed (T=1500us into frame)
        // For parity=1: mid-bit is rising edge (state=1)
        // For parity=0: mid-bit is falling edge (state=0)
        // Note: for parity=1, there's a boundary falling edge at ~500us elapsed - ignore it
        unsigned long elapsed = newTs - responseTimestamp;

        if (elapsed < 600)
        {
            // Edge too soon - this is a boundary edge (parity=1 case), not mid-bit
            // Ignore and keep waiting for the mid-bit edge
        }
        else if (elapsed < 1500)
        {
            // Mid-bit edge for bit 31 (parity bit)
            int state = readState();
            // Debug: capture timing and state for bit 31
            debugBit31Elapsed = elapsed;
            debugBit31State = state;
            status = OpenThermStatus::RESPONSE_RECEIVING;
            responseTimestamp = newTs;
            responseBitIndex = 1;  // Bit 31 sampled, next is bit 30
            response = state ? 0 : 1;  // Inverted: state=0 means bit=1
        }
        else // elapsed >= 1500
        {
            status = OpenThermStatus::RESPONSE_INVALID;
            responseTimestamp = newTs;
        }
    }
    else if (status == OpenThermStatus::RESPONSE_RECEIVING)
    {
        // Threshold lowered from 750 to 600 to handle timing jitter
        // Mid-bit edges are ~1000us apart, boundary edges (if any) are at ~500us
        // 600us safely distinguishes between them while allowing some jitter
        if ((newTs - responseTimestamp) > 600)
        {
            if (responseBitIndex < 32)
            {
                int state = readState();
                // Inverted hardware: state=0 means bit=1
                int bit = !state;
                response = (response << 1) | bit;
                responseTimestamp = newTs;
                responseBitIndex = responseBitIndex + 1;
            }
            else
            { // stop bit
                status = OpenThermStatus::RESPONSE_READY;
                responseTimestamp = newTs;
                // Debug: store last raw frame for logging
                lastRawFrame = response;
            }
        }
    }
}

void IRAM_ATTR OpenTherm::handleInterruptHelper(void* ptr)
{
    static_cast<OpenTherm*>(ptr)->handleInterrupt();
}

void OpenTherm::monitorInterrupts()
{
    uint32_t interruptTimestampsCopy[68];

    // Wait for ESP notifications to this thread
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI("OT", "Scanning interrupts");

        // Fast copy of interrupt timestamps, starting from interruptsUpToIndex
        for (int i = 0; i < 68; i++) {
            interruptTimestampsCopy[i] = interruptTimestamps[(i + interruptsUpToIndex) % 68];
        }

        auto parsedFrame = parseInterrupts(interruptTimestampsCopy, 68);
        if (parsedFrame != 0) {
            // Print as binary string (e.g. 01010101)
            logU32Bin(parsedFrame);
        }
    }
}

unsigned long OpenTherm::process(std::function<void(unsigned long, OpenThermResponseStatus)> callback)
{
    portDISABLE_INTERRUPTS();
    OpenThermStatus st = status;
    unsigned long ts = responseTimestamp;
    auto currentInterrupIdx = interruptsUpToIndex;
    portENABLE_INTERRUPTS();

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
        responseStatus = (isSlave ? isValidRequest(response) : isValidResponse(response)) ? OpenThermResponseStatus::SUCCESS : OpenThermResponseStatus::INVALID;
        // commenting to break it
        //if (callback) callback(response, responseStatus);
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

bool OpenTherm::isValidResponse(unsigned long response)
{
    if (parity(response))
        return false;
    uint8_t msgType = (response << 1) >> 29;
    return msgType == (uint8_t)OpenThermMessageType::READ_ACK || msgType == (uint8_t)OpenThermMessageType::WRITE_ACK;
}

bool OpenTherm::isValidRequest(unsigned long request)
{
    if (parity(request))
        return false;
    uint8_t msgType = (request << 1) >> 29;
    return msgType == (uint8_t)OpenThermMessageType::READ_DATA || msgType == (uint8_t)OpenThermMessageType::WRITE_DATA;
}

void OpenTherm::end()
{
    gpio_isr_handler_remove(inPin);
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
    return isValidResponse(response);
}

float OpenTherm::getBoilerTemperature()
{
    unsigned long response = sendRequest(buildGetBoilerTemperatureRequest());
    return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getReturnTemperature()
{
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Tret, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

bool OpenTherm::setDHWSetpoint(float temperature)
{
    unsigned int data = temperatureToData(temperature);
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::TdhwSet, data));
    return isValidResponse(response);
}

float OpenTherm::getDHWTemperature()
{
    unsigned long response = sendRequest(buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::Tdhw, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getModulation()
{
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::RelModLevel, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getPressure()
{
    unsigned long response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::CHPressure, 0));
    return isValidResponse(response) ? getFloat(response) : 0;
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

uint32_t OpenTherm::parseInterrupts(uint32_t interrupts[68], int upToIndex)
{
    // Need at least: idle + a bunch of segments
    if (upToIndex < 4) return 0;

    // Expect first entry to be the long idle period before the start bit
    // We'll just skip index 0 unconditionally.
    int i = 1;

    // Manchester half-bit is ~500us (bit is ~1000us).
    constexpr int HALF_US_NOM = 500;

    // Loose tolerances: classify segment as 1 half-bit (~500us) or 2 half-bits (~1000us).
    constexpr int ONE_MIN = 300;
    constexpr int ONE_MAX = 700;
    constexpr int TWO_MIN = 700;
    constexpr int TWO_MAX = 1300;

    // We want start(1) + 32 frame + stop(1) = 34 bits => 69 half-bits.
    bool halfLevels[69];
    int halfCount = 0;

    while (i < upToIndex && halfCount < 69) {
        const bool level = seg_level(interrupts[i]);
        const uint32_t dur = seg_dur_us(interrupts[i]);

        int halves = 0;
        if (dur >= ONE_MIN && dur < ONE_MAX) halves = 1;
        else if (dur >= TWO_MIN && dur <= TWO_MAX) halves = 2;
        else {
            ESP_LOGI("OT", "Invalid duration: %d", dur);
            // Allow a final trailing idle to be long; stop if we already have enough
            // or treat as invalid if it appears mid-frame.
            return 0;
        }

        for (int k = 0; k < halves && halfCount < 69; ++k) {
            halfLevels[halfCount++] = level;
        }
        ++i;
    }

    if (halfCount != 68) {
        ESP_LOGI("OT", "Half count mismatch: %d", halfCount);
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
            ESP_LOGI("OT", "Invalid Manchester (two same bits): %d", b);
            return 0;
        }

        if (a == 1 && c == 0) bits[b] = 1;
        else if (a == 0 && c == 1) bits[b] = 0;
        else {
            ESP_LOGI("OT", "Invalid Manchester (no mid-bit transition): %d", b);
            return 0;
        }
    }

    // Start/stop bits must be '1' (outside the 32-bit frame). :contentReference[oaicite:3]{index=3}
    if (bits[0] != 1) {
        ESP_LOGI("OT", "Invalid start bit: %d", bits[0]);
        return 0;
    }
    if (bits[33] != 1) {
        ESP_LOGI("OT", "Invalid stop bit: %d", bits[33]);
        return 0;
    }

    // Build the 32-bit frame from bits[1..32], MSB first.
    uint32_t frame = 0;
    for (int b = 1; b <= 32; ++b) {
        frame = (frame << 1) | (bits[b] & 1u);
    }

    // Parity: total number of '1' bits in entire 32 bits must be even. :contentReference[oaicite:4]{index=4}
    if ((popcount32(frame) & 1) != 0) {
        ESP_LOGI("OT", "Invalid parity: %d", popcount32(frame));
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
    ESP_LOGI("OT", "Incoming frame from %s: %s", isSlave ? "T" : "B", buf);
}

} // namespace ot
