#pragma once

#include <stdint.h>
#include <functional>
#include <optional>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_types.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "rmt_encoder.h"

namespace ot {

enum class OpenThermMessageType : uint8_t {
    ReadData     = 0b000,
    WriteData    = 0b001,
    InvalidData  = 0b010,
    Reserved     = 0b011,
    ReadAck      = 0b100,
    WriteAck     = 0b101,
    DataInvalid  = 0b110,
    UnknownId    = 0b111
};

inline const char* toString(OpenThermMessageType type) {
    switch (type) {
        case OpenThermMessageType::ReadData:    return "READ_DATA";
        case OpenThermMessageType::WriteData:   return "WRITE_DATA";
        case OpenThermMessageType::InvalidData: return "INVALID_DATA";
        case OpenThermMessageType::Reserved:    return "RESERVED";
        case OpenThermMessageType::ReadAck:     return "READ_ACK";
        case OpenThermMessageType::WriteAck:    return "WRITE_ACK";
        case OpenThermMessageType::DataInvalid: return "DATA_INVALID";
        case OpenThermMessageType::UnknownId:   return "UNKNOWN_ID";
        default:                       return "UNKNOWN";
    }
}

class OpenThermFrame {
public:
    constexpr OpenThermFrame() : raw_(0) {}
    constexpr explicit OpenThermFrame(uint32_t raw) : raw_(raw) {}

    static OpenThermFrame buildRequest(OpenThermMessageType type, uint8_t dataId, uint16_t data) {
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
        return OpenThermFrame(frame);
    }

    static OpenThermFrame buildResponse(OpenThermMessageType type, uint8_t dataId, uint16_t data) {
        return buildRequest(type, dataId, data);
    }

    constexpr uint32_t raw() const { return raw_; }

    constexpr OpenThermMessageType messageType() const {
        return static_cast<OpenThermMessageType>((raw_ >> 28) & 0x7);
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


class OpenThermDriver {
public:
    struct Config {
        gpio_num_t inPin = GPIO_NUM_NC;
        gpio_num_t outPin = GPIO_NUM_NC;
        bool isSlave = false; // Whether this instance is a slave (talks with a thermostat)
        uint32_t minTxIntervalMs = 100; // Minimum time between transmissions or after RX
    };

    OpenThermDriver(const Config& config);
    ~OpenThermDriver();

    void start();
    
    /**
     * Stop the driver.
     *
     * This function will stop the driver and free the resources.
     * It will not wait for the deinit to complete, it's best to implement your own wait in the caller. (wait for ~100ms)
     */
    void stop();

    // Async send. Returns true if queued.
    bool send(OpenThermFrame frame);
    
    // Blocking receive. Waits up to timeoutMs for a valid frame.
    // Returns std::nullopt if timeout or error.
    std::optional<OpenThermFrame> receive(uint32_t timeoutMs);

private:
    static void taskEntry(void* arg);
    void taskLoop();
    
    void initRMT();
    void deinitRMT();
    
    bool transmitFrame(OpenThermFrame frame);
    
    // RMT callbacks
    static bool onRmtRxDone(rmt_channel_handle_t rx_chan, const rmt_rx_done_event_data_t *edata, void *user_ctx);

    Config config_;
    TaskHandle_t taskHandle_ = nullptr;
    QueueHandle_t txQueue_ = nullptr;
    QueueHandle_t rxQueue_ = nullptr;

    // RMT handles
    rmt_channel_handle_t rxChannel_ = nullptr;
    rmt_channel_handle_t txChannel_ = nullptr;
    rmt_encoder_handle_t copyEncoder_ = nullptr;
    
    // Buffers
    rmt_symbol_word_t rxBuffer_[2][128]; // Double buffer
    volatile int activeRxBuffer_ = 0;
    volatile size_t lastRxSize_ = 0;
    
    rmt_symbol_word_t txBuffer_[64]; // For TX

    // State
    volatile bool rxFrameReady_ = false;
    int64_t nextPossibleTrafficTime_ = 0; // Timestamp when next TX is allowed
    
    static const uint32_t NOTIFY_RX_DONE = 1 << 0;
    static const uint32_t NOTIFY_TX_REQUEST = 1 << 1;
    static const uint32_t NOTIFY_STOP = 1 << 2;
};

enum class OpenThermMessageId : uint8_t
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

} // namespace ot
