/*
OpenTherm.cpp - OpenTherm Communication Library for ESP-IDF
Copyright 2023, Ihor Melnyk
ESP-IDF port
*/

#include "OpenTherm.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Helper macro for bit reading
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)

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

    //responseTimestamp = esp_timer_get_time();
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

void IRAM_ATTR OpenTherm::newInterruptHandler()
{
    interruptCount++;

    auto previousReception = lastReceptionTimestamp;
    auto currentReception = lastReceptionTimestamp = esp_timer_get_time();
    auto elapsed = currentReception - previousReception;

    // Been more than 3sm since last reception, meaning that we must be at the start of a frame right now
    if (elapsed > 3000000) { // 3ms
        // First edge must be rising from idle, then go down to indicate bit 1 (start bit)
        if (readState() == 1) {
            status = OpenThermStatus::RESPONSE_START_BIT;
            responseStartsAt = currentReception;
            midBit = true;
        } else {
            status = OpenThermStatus::RESPONSE_INVALID;
        }
        return;
    }

    if (status == OpenThermStatus::RESPONSE_START_BIT) {
        if (readState() == 0) {
            if (elapsed > 750) {
                status = OpenThermStatus::RESPONSE_INVALID;
                return;
            }

            status = OpenThermStatus::RESPONSE_RECEIVING;
            response = 0;
            responseBitIndex = 0;
            midBit = false;
            return;
        } else {
            status = OpenThermStatus::RESPONSE_INVALID;
        }
    }

    if (status == OpenThermStatus::RESPONSE_RECEIVING) {
        if (midBit) {
            // Skip this transition, we already accounted for this bit in the else branch below
            midBit = false;
        } else {
            if (elapsed > 750 && elapsed < 1500) {
                // Not in mid-bit, and raising edge, means we are in the middle of a 0 bit after a 1 bit
                if (readState() == 1) {
                    response = (response << 1) | 0;
                    responseBitIndex++;
                } else {
                    response = (response << 1) | 1;
                    responseBitIndex++;
                }
            } else if (elapsed >= 1500) {
                status = OpenThermStatus::RESPONSE_INVALID;
                return;
            } else {
                // Line transition between bits (if it transitions down, then it will go up in 500us, meaning it is going to be a 0 bit, or otherwise 1 bit)
                midBit = true;
                if (readState() == 0) {
                    response = (response << 1) | 0;
                    responseBitIndex++;
                } else {
                    response = (response << 1) | 1;
                    responseBitIndex++;
                }
            }
        }

        if (responseBitIndex == 32) {
            status = OpenThermStatus::RESPONSE_READY;
            return;
        }
    }
}

void IRAM_ATTR OpenTherm::handleInterrupt()
{
    interruptCount++;

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
    static_cast<OpenTherm*>(ptr)->newInterruptHandler();
}

unsigned long OpenTherm::process(std::function<void(unsigned long, OpenThermResponseStatus)> callback)
{
    portDISABLE_INTERRUPTS();
    OpenThermStatus st = status;
    unsigned long ts = lastReceptionTimestamp;
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

} // namespace ot
