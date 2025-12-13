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
        if (readState() == 1)
        {
            status = OpenThermStatus::RESPONSE_START_BIT;
            responseTimestamp = newTs;
        }
        else
        {
            status = OpenThermStatus::RESPONSE_INVALID;
            responseTimestamp = newTs;
        }
    }
    else if (status == OpenThermStatus::RESPONSE_START_BIT)
    {
        // After start bit rising edge, wait for falling edge to begin receiving
        // If parity bit = 1: falling edge comes at ~500us (end of start bit)
        // If parity bit = 0: falling edge comes at ~1500us (middle of parity bit)
        unsigned long elapsed = newTs - responseTimestamp;
        if (elapsed < 750 && readState() == 0)
        {
            // Falling edge within 750us = end of start bit, parity bit = 1
            // Don't sample yet - wait for the middle of bit 31
            status = OpenThermStatus::RESPONSE_RECEIVING;
            responseTimestamp = newTs;
            responseBitIndex = 0;
            response = 0;
        }
        else if (elapsed >= 750 && elapsed < 1750 && readState() == 0)
        {
            // Falling edge after 750us = middle of bit 31 (parity bit = 0)
            // Sample this bit immediately since we're at its middle
            // Falling edge in Manchester = bit value 0
            status = OpenThermStatus::RESPONSE_RECEIVING;
            responseTimestamp = newTs;
            responseBitIndex = 1;
            response = 0;  // Bit 31 = 0 (falling edge = 0 in Manchester)
        }
        else if (elapsed >= 1750)
        {
            status = OpenThermStatus::RESPONSE_INVALID;
            responseTimestamp = newTs;
        }
        // else: rising edge within window, ignore and wait for falling edge
    }
    else if (status == OpenThermStatus::RESPONSE_RECEIVING)
    {
        if ((newTs - responseTimestamp) > 750)
        {
            if (responseBitIndex < 32)
            {
                int state = readState();
                int bit = !state;  // Original logic assumes hardware inverts signal
                response = (response << 1) | bit;
                responseTimestamp = newTs;
                responseBitIndex = responseBitIndex + 1;
            }
            else
            { // stop bit
                status = OpenThermStatus::RESPONSE_READY;
                responseTimestamp = newTs;
            }
        }
    }
}

void IRAM_ATTR OpenTherm::handleInterruptHelper(void* ptr)
{
    static_cast<OpenTherm*>(ptr)->handleInterrupt();
}

unsigned long OpenTherm::process(std::function<void(unsigned long, OpenThermResponseStatus)> callback)
{
    portDISABLE_INTERRUPTS();
    OpenThermStatus st = status;
    unsigned long ts = responseTimestamp;
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
