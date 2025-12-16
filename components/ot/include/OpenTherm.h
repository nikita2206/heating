/*
OpenTherm.h - OpenTherm Library for ESP-IDF
https://github.com/ihormelnyk/OpenTherm
Licensed under MIT license
Copyright 2023, Ihor Melnyk
ESP-IDF port

Frame Structure:
P MGS-TYPE SPARE DATA-ID  DATA-VALUE
0 000      0000  00000000 00000000 00000000
*/

#ifndef OpenTherm_h
#define OpenTherm_h

#include <stdint.h>
#include <functional>
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ot {

enum class OpenThermResponseStatus : uint8_t
{
    NONE,
    SUCCESS,
    INVALID,
    TIMEOUT
};

enum class OpenThermMessageType : uint8_t
{
    /*  Master to Slave */
    READ_DATA = 0b000,
    READ = READ_DATA, // for backward compatibility
    WRITE_DATA = 0b001,
    WRITE = WRITE_DATA, // for backward compatibility
    INVALID_DATA = 0b010,
    RESERVED = 0b011,
    /* Slave to Master */
    READ_ACK = 0b100,
    WRITE_ACK = 0b101,
    DATA_INVALID = 0b110,
    UNKNOWN_DATA_ID = 0b111
};

typedef OpenThermMessageType OpenThermRequestType; // for backward compatibility

enum class OpenThermMessageID : uint8_t
{
    Status                                       = 0, // flag8/flag8  Master and Slave Status flags.
    TSet                                         = 1, // f8.8    Control Setpoint i.e.CH water temperature Setpoint(°C)
    MConfigMMemberIDcode                         = 2, // flag8/u8  Master Configuration Flags / Master MemberID Code
    SConfigSMemberIDcode                         = 3, // flag8/u8  Slave Configuration Flags / Slave MemberID Code
    RemoteRequest                                = 4, // u8/u8     Remote Request
    ASFflags                                     = 5, // flag8/u8  Application - specific fault flags and OEM fault code
    RBPflags                                     = 6, // flag8/flag8   Remote boiler parameter transfer - enable & read / write flags
    CoolingControl                               = 7, // f8.8    Cooling control signal(%)
    TsetCH2                                      = 8, // f8.8    Control Setpoint for 2e CH circuit(°C)
    TrOverride                                   = 9, // f8.8    Remote override room Setpoint
    TSP                                         = 10, // u8/u8     Number of Transparent - Slave - Parameters supported by slave
    TSPindexTSPvalue                            = 11, // u8/u8     Index number / Value of referred - to transparent slave parameter.
    FHBsize                                     = 12, // u8/u8     Size of Fault - History - Buffer supported by slave
    FHBindexFHBvalue                            = 13, // u8/u8     Index number / Value of referred - to fault - history buffer entry.
    MaxRelModLevelSetting                       = 14, // f8.8    Maximum relative modulation level setting(%)
    MaxCapacityMinModLevel                      = 15, // u8/u8     Maximum boiler capacity(kW) / Minimum boiler modulation level(%)
    TrSet                                       = 16, // f8.8    Room Setpoint(°C)
    RelModLevel                                 = 17, // f8.8    Relative Modulation Level(%)
    CHPressure                                  = 18, // f8.8    Water pressure in CH circuit(bar)
    DHWFlowRate                                 = 19, // f8.8    Water flow rate in DHW circuit. (litres / minute)
    DayTime                                     = 20, // special/u8    Day of Week and Time of Day
    Date                                        = 21, // u8/u8     Calendar date
    Year                                        = 22, // u16     Calendar year
    TrSetCH2                                    = 23, // f8.8    Room Setpoint for 2nd CH circuit(°C)
    Tr                                          = 24, // f8.8    Room temperature(°C)
    Tboiler                                     = 25, // f8.8    Boiler flow water temperature(°C)
    Tdhw                                        = 26, // f8.8    DHW temperature(°C)
    Toutside                                    = 27, // f8.8    Outside temperature(°C)
    Tret                                        = 28, // f8.8    Return water temperature(°C)
    Tstorage                                    = 29, // f8.8    Solar storage temperature(°C)
    Tcollector                                  = 30, // f8.8    Solar collector temperature(°C)
    TflowCH2                                    = 31, // f8.8    Flow water temperature CH2 circuit(°C)
    Tdhw2                                       = 32, // f8.8    Domestic hot water temperature 2 (°C)
    Texhaust                                    = 33, // s16     Boiler exhaust temperature(°C)
    TboilerHeatExchanger                        = 34, // f8.8    Boiler heat exchanger temperature(°C)
    BoilerFanSpeedSetpointAndActual             = 35, // u8/u8     Boiler fan speed Setpoint and actual value
    FlameCurrent                                = 36, // f8.8    Electrical current through burner flame[μA]
    TrCH2                                       = 37, // f8.8    Room temperature for 2nd CH circuit(°C)
    RelativeHumidity                            = 38, // f8.8    Actual relative humidity as a percentage
    TrOverride2                                 = 39, // f8.8    Remote Override Room Setpoint 2
    TdhwSetUBTdhwSetLB                          = 48, // s8/s8     DHW Setpoint upper & lower bounds for adjustment(°C)
    MaxTSetUBMaxTSetLB                          = 49, // s8/s8     Max CH water Setpoint upper & lower bounds for adjustment(°C)
    TdhwSet                                     = 56, // f8.8    DHW Setpoint(°C) (Remote parameter 1)
    MaxTSet                                     = 57, // f8.8    Max CH water Setpoint(°C) (Remote parameters 2)
    StatusVentilationHeatRecovery               = 70, // flag8/flag8   Master and Slave Status flags ventilation / heat - recovery
    Vset                                        = 71, // -/u8  Relative ventilation position (0-100%).
    ASFflagsOEMfaultCodeVentilationHeatRecovery = 72, // flag8/u8  Application-specific fault flags and OEM fault code ventilation / heat-recovery
    OEMDiagnosticCodeVentilationHeatRecovery    = 73, // u16     An OEM-specific diagnostic/service code for ventilation / heat-recovery system
    SConfigSMemberIDCodeVentilationHeatRecovery = 74, // flag8/u8  Slave Configuration Flags / Slave MemberID Code ventilation / heat-recovery
    OpenThermVersionVentilationHeatRecovery     = 75, // f8.8    The implemented version of the OpenTherm Protocol Specification in the ventilation / heat-recovery system.
    VentilationHeatRecoveryVersion              = 76, // u8/u8     Ventilation / heat-recovery product version number and type
    RelVentLevel                                = 77, // -/u8  Relative ventilation (0-100%)
    RHexhaust                                   = 78, // -/u8  Relative humidity exhaust air (0-100%)
    CO2exhaust                                  = 79, // u16     CO2 level exhaust air (0-2000 ppm)
    Tsi                                         = 80, // f8.8    Supply inlet temperature (°C)
    Tso                                         = 81, // f8.8    Supply outlet temperature (°C)
    Tei                                         = 82, // f8.8    Exhaust inlet temperature (°C)
    Teo                                         = 83, // f8.8    Exhaust outlet temperature (°C)
    RPMexhaust                                  = 84, // u16     Exhaust fan speed in rpm
    RPMsupply                                   = 85, // u16     Supply fan speed in rpm
    RBPflagsVentilationHeatRecovery             = 86, // flag8/flag8   Remote ventilation / heat-recovery parameter transfer-enable & read/write flags
    NominalVentilationValue                     = 87, // u8/-  Nominal relative value for ventilation (0-100 %)
    TSPventilationHeatRecovery                  = 88, // u8/u8     Number of Transparent-Slave-Parameters supported by TSP's ventilation / heat-recovery
    TSPindexTSPvalueVentilationHeatRecovery     = 89, // u8/u8     Index number / Value of referred-to transparent TSP's ventilation / heat-recovery parameter.
    FHBsizeVentilationHeatRecovery              = 90, // u8/u8     Size of Fault-History-Buffer supported by ventilation / heat-recovery
    FHBindexFHBvalueVentilationHeatRecovery     = 91, // u8/u8     Index number / Value of referred-to fault-history buffer entry ventilation / heat-recovery
    Brand                                       = 93, // u8/u8     Index number of the character in the text string ASCII character referenced by the above index number
    BrandVersion                                = 94, // u8/u8     Index number of the character in the text string ASCII character referenced by the above index number
    BrandSerialNumber                           = 95, // u8/u8     Index number of the character in the text string ASCII character referenced by the above index number
    CoolingOperationHours                       = 96, // u16     Number of hours that the slave is in Cooling Mode.
    PowerCycles                                 = 97, // u16     Number of Power Cycles of a slave (wake-up after Reset)
    RFsensorStatusInformation                   = 98, // special/special   For a specific RF sensor the RF strength and battery level is written
    RemoteOverrideOperatingModeHeatingDHW       = 99, // special/special   Operating Mode HC1, HC2/ Operating Mode DHW
    RemoteOverrideFunction                     = 100, // flag8/-   Function of manual and program changes in master and remote room Setpoint
    StatusSolarStorage                         = 101, // flag8/flag8   Master and Slave Status flags Solar Storage
    ASFflagsOEMfaultCodeSolarStorage           = 102, // flag8/u8  Application-specific fault flags and OEM fault code Solar Storage
    SConfigSMemberIDcodeSolarStorage           = 103, // flag8/u8  Slave Configuration Flags / Slave MemberID Code Solar Storage
    SolarStorageVersion                        = 104, // u8/u8     Solar Storage product version number and type
    TSPSolarStorage                            = 105, // u8/u8     Number of Transparent - Slave - Parameters supported by TSP's Solar Storage
    TSPindexTSPvalueSolarStorage               = 106, // u8/u8     Index number / Value of referred - to transparent TSP's Solar Storage parameter.
    FHBsizeSolarStorage                        = 107, // u8/u8     Size of Fault - History - Buffer supported by Solar Storage
    FHBindexFHBvalueSolarStorage               = 108, // u8/u8     Index number / Value of referred - to fault - history buffer entry Solar Storage
    ElectricityProducerStarts                  = 109, // U16     Number of start of the electricity producer.
    ElectricityProducerHours                   = 110, // U16     Number of hours the electricity produces is in operation
    ElectricityProduction                      = 111, // U16     Current electricity production in Watt.
    CumulativElectricityProduction             = 112, // U16     Cumulative electricity production in KWh.
    UnsuccessfulBurnerStarts                   = 113, // u16     Number of un - successful burner starts
    FlameSignalTooLowNumber                    = 114, // u16     Number of times flame signal was too low
    OEMDiagnosticCode                          = 115, // u16     OEM - specific diagnostic / service code
    SuccessfulBurnerStarts                     = 116, // u16     Number of succesful starts burner
    CHPumpStarts                               = 117, // u16     Number of starts CH pump
    DHWPumpValveStarts                         = 118, // u16     Number of starts DHW pump / valve
    DHWBurnerStarts                            = 119, // u16     Number of starts burner during DHW mode
    BurnerOperationHours                       = 120, // u16     Number of hours that burner is in operation(i.e.flame on)
    CHPumpOperationHours                       = 121, // u16     Number of hours that CH pump has been running
    DHWPumpValveOperationHours                 = 122, // u16     Number of hours that DHW pump has been running or DHW valve has been opened
    DHWBurnerOperationHours                    = 123, // u16     Number of hours that burner is in operation during DHW mode
    OpenThermVersionMaster                     = 124, // f8.8    The implemented version of the OpenTherm Protocol Specification in the master.
    OpenThermVersionSlave                      = 125, // f8.8    The implemented version of the OpenTherm Protocol Specification in the slave.
    MasterVersion                              = 126, // u8/u8     Master product version number and type
    SlaveVersion                               = 127, // u8/u8     Slave product version number and type
};

enum class OpenThermStatus : uint8_t
{
    NOT_INITIALIZED,
    READY,
    DELAY,
    REQUEST_SENDING,
    RESPONSE_WAITING,
    RESPONSE_START_BIT,
    RESPONSE_RECEIVING,
    RESPONSE_READY,
    RESPONSE_INVALID
};

class OpenTherm
{
public:
    friend void monitorTaskEntry(void* pvParameters);
    OpenTherm(gpio_num_t inPin = GPIO_NUM_4, gpio_num_t outPin = GPIO_NUM_5, bool isSlave = false, bool invertOutput = false, bool invertInput = false);
    ~OpenTherm();
    volatile OpenThermStatus status;
    volatile int64_t lastReceptionTimestamp;
    volatile int64_t responseStartsAt;
    volatile bool midBit;
    uint32_t interruptTimestamps[128];
    int interruptsUpToIndex = 0;
    void begin(bool useRMT = false);
    bool isReady();
    unsigned long sendRequest(unsigned long request);
    bool sendResponse(unsigned long request);
    bool sendRequestAsync(unsigned long request);
    [[deprecated("Use OpenTherm::sendRequestAsync(unsigned long) instead")]]
    bool sendRequestAync(unsigned long request) {
        return sendRequestAsync(request);
    }
    static unsigned long buildRequest(OpenThermMessageType type, OpenThermMessageID id, unsigned int data);
    static unsigned long buildResponse(OpenThermMessageType type, OpenThermMessageID id, unsigned int data);
    unsigned long getLastResponse();
    OpenThermResponseStatus getLastResponseStatus();
    static const char *statusToString(OpenThermResponseStatus status);
    void handleInterrupt();
    void newInterruptHandler();
    static void handleInterruptHelper(void* ptr);
    unsigned long process(std::function<void(unsigned long, OpenThermResponseStatus)> callback = nullptr);
    void end();

    static bool parity(unsigned long frame);
    static OpenThermMessageType getMessageType(unsigned long message);
    static OpenThermMessageID getDataID(unsigned long frame);
    static const char *messageTypeToString(OpenThermMessageType message_type);
    static bool isValidRequest(unsigned long request, bool isSlave);
    static bool isValidResponse(unsigned long response, bool isSlave);

    // requests
    static unsigned long buildSetBoilerStatusRequest(bool enableCentralHeating, bool enableHotWater = false, bool enableCooling = false, bool enableOutsideTemperatureCompensation = false, bool enableCentralHeating2 = false);
    static unsigned long buildSetBoilerTemperatureRequest(float temperature);
    static unsigned long buildGetBoilerTemperatureRequest();

    // responses
    static bool isFault(unsigned long response);
    static bool isCentralHeatingActive(unsigned long response);
    static bool isHotWaterActive(unsigned long response);
    static bool isFlameOn(unsigned long response);
    static bool isCoolingActive(unsigned long response);
    static bool isDiagnostic(unsigned long response);
    static uint16_t getUInt(const unsigned long response);
    static float getFloat(const unsigned long response);
    static unsigned int temperatureToData(float temperature);

    // basic requests
    unsigned long setBoilerStatus(bool enableCentralHeating, bool enableHotWater = false, bool enableCooling = false, bool enableOutsideTemperatureCompensation = false, bool enableCentralHeating2 = false);
    bool setBoilerTemperature(float temperature);
    float getBoilerTemperature();
    float getReturnTemperature();
    bool setDHWSetpoint(float temperature);
    float getDHWTemperature();
    float getModulation();
    float getPressure();
    unsigned char getFault();


    void monitorInterrupts();
    bool handleRMTFrame(const rmt_rx_done_event_data_t *edata);

private:
    const gpio_num_t inPin;
    const gpio_num_t outPin;
    const bool isSlave;
    const bool invertOutput;
    const bool invertInput;

    volatile unsigned long response;
    volatile OpenThermResponseStatus responseStatus;
    volatile unsigned long responseTimestamp;
    volatile uint8_t responseBitIndex;

public:
    volatile uint32_t interruptCount = 0;  // Debug: count interrupts
    volatile unsigned long lastRawFrame = 0;  // Debug: last successfully received frame
    volatile unsigned long debugBit31Elapsed = 0;  // Debug: elapsed time when sampling bit 31
    volatile int debugBit31State = 0;  // Debug: state when sampling bit 31
private:

    int readState();
    void setActiveState();
    void setIdleState();
    void activateBoiler();
    uint32_t parseInterrupts(uint32_t interrupts[128], int upToIndex);
    void logU32Bin(uint32_t v);

    TaskHandle_t monitorTaskHandle_;

    // RMT-related members
    rmt_channel_handle_t rmtChannel_;
    bool useRMT_;  // Flag to switch between interrupt and RMT mode

    void sendBit(bool high);

    // RMT methods
    void initRMT();
    void startRMTReceive();
    void stopRMTReceive();
    void monitorGPIOInterrupts();
    uint32_t parseRMTSymbols(rmt_symbol_word_t* symbols, size_t num_symbols);
};

enum class MessageType : uint8_t {
    ReadData     = 0b000,
    WriteData    = 0b001,
    InvalidData  = 0b010,
    Reserved     = 0b011,
    ReadAck      = 0b100,
    WriteAck     = 0b101,
    DataInvalid  = 0b110,
    UnknownId    = 0b111
};

class Frame {
public:
    constexpr Frame() : raw_(0) {}
    constexpr explicit Frame(uint32_t raw) : raw_(raw) {}

    static Frame buildRequest(MessageType type, uint8_t dataId, uint16_t data) {
        uint32_t frame = (static_cast<uint32_t>(type) << 28) |
                         (static_cast<uint32_t>(dataId) << 16) |
                         data;
        // Add parity bit if needed (odd parity)
        uint8_t p = 0;
        uint32_t temp = frame;
        while (temp > 0) {
            if (temp & 1) p++;
            temp >>= 1;
        }
        if (p & 1) frame |= (1UL << 31);
        return Frame(frame);
    }

    static Frame buildResponse(MessageType type, uint8_t dataId, uint16_t data) {
        return buildRequest(type, dataId, data);
    }

    constexpr uint32_t raw() const { return raw_; }

    constexpr MessageType messageType() const {
        return static_cast<MessageType>((raw_ >> 28) & 0x7);
    }

    constexpr uint8_t dataId() const {
        return static_cast<uint8_t>((raw_ >> 16) & 0xFF);
    }

    constexpr uint16_t dataValue() const {
        return static_cast<uint16_t>(raw_ & 0xFFFF);
    }

    constexpr uint8_t highByte() const {
        return static_cast<uint8_t>((raw_ >> 8) & 0xFF);
    }

    constexpr uint8_t lowByte() const {
        return static_cast<uint8_t>(raw_ & 0xFF);
    }

    float asFloat() const {
        uint16_t u88 = dataValue();
        return (u88 & 0x8000) ? -(0x10000L - u88) / 256.0f : u88 / 256.0f;
    }

    constexpr explicit operator bool() const { return raw_ != 0; }

private:
    uint32_t raw_;
};

inline const char* toString(MessageType type) {
    switch (type) {
        case MessageType::ReadData:    return "READ_DATA";
        case MessageType::WriteData:   return "WRITE_DATA";
        case MessageType::InvalidData: return "INVALID_DATA";
        case MessageType::Reserved:    return "RESERVED";
        case MessageType::ReadAck:     return "READ_ACK";
        case MessageType::WriteAck:    return "WRITE_ACK";
        case MessageType::DataInvalid: return "DATA_INVALID";
        case MessageType::UnknownId:   return "UNKNOWN_ID";
        default:                       return "UNKNOWN";
    }
}

} // namespace ot

#endif // OpenTherm_h
