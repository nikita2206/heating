## OpenTherm frame glossary

```
ID 0 (R)  Status (special exchange)
  DATA.HB = MasterStatus(flag8):
    b0 CH_enable
    b1 DHW_enable
    b2 Cooling_enable
    b3 OTC_active
    b4 CH2_enable
    b5 Summer/Winter (0=winter,1=summer)
    b6 DHW_blocking
    b7 reserved
  DATA.LB = SlaveStatus(flag8):
    b0 Fault_indication
    b1 CH_mode_active
    b2 DHW_mode_active
    b3 Flame_on
    b4 Cooling_mode_active
    b5 CH2_mode_active
    b6 Diagnostic/Service_event
    b7 Electricity_production_on
  Note: master should use READ-DATA(id=0, MasterStatus,00); slave replies READ-ACK(id=0, MasterStatus, SlaveStatus).

ID 1 (-W) Control setpoint TsetCH (f8.8, °C, default assumed 0..100)
ID 2 (-W) Master config (HB=flag8, LB=Master MemberID u8)
  HB flag8: b0 SmartPower_implemented; b1..7 reserved
ID 3 (R)  Slave config (HB=flag8, LB=Slave MemberID u8)
  HB flag8:
    b0 DHW_present
    b1 Control_type (0=modulating,1=on/off)
    b2 Cooling_supported
    b3 DHW_config (0=instant/not-specified,1=storage_tank)
    b4 Master_low-off&pump_control_allowed (0=allowed,1=not allowed)
    b5 CH2_present
    b6 Remote_water_filling (0=available/unknown,1=not available)
    b7 Heat/Cool_mode_control (0=master can switch,1=slave switches)

ID 4 (-W) Remote request (HB=RequestCode u8, LB=ReqResponseCode u8)
  RequestCode:
    0 normal_mode
    1 boiler_lockout_reset (BLOR)
    2 CH_water_filling (CHWF)
    3 service max power (chimney sweep)
    4 service min power
    5 spark test (no gas)
    6 fan max speed (no flame)
    7 fan min speed (no flame)
    8 3-way valve to CH (no pump, no flame)
    9 3-way valve to DHW (no pump, no flame)
    10 reset service-request flag
    11 service test 1 (OEM-specific)
    12 automatic hydronic air purge
    13..255 reserved
  ReqResponseCode: 0..127 refused, 128..255 accepted

ID 5 (R)  Fault info (HB=ASF flag8, LB=OEM fault code u8)
  HB ASF flag8:
    b0 Service_request
    b1 Lockout_reset_enabled
    b2 Low_water_pressure_fault
    b3 Gas/Flame_fault
    b4 Air_pressure_fault
    b5 Water_overtemp_fault
    b6..7 reserved

ID 6 (R)  Remote boiler parameter flags (HB=transfer_enable flag8, LB=R/W flag8)
  bit0: DHW setpoint (ID56) enabled / RO-vs-RW
  bit1: Max CH setpoint (ID57) enabled / RO-vs-RW
  bit2..7 reserved

ID 7 (-W) Cooling control signal (f8.8, %, 0..100)
ID 8 (-W) Control setpoint 2 TsetCH2 (f8.8, °C)
ID 9 (R)  Remote override room setpoint (f8.8, 0=no override, 1..30)

ID 10 (R) Number of TSPs (HB=u8 count, LB reserved)
ID 11 (RW) Transparent Slave Parameter (HB=TSP index u8, LB=TSP value u8)

ID 12 (R) Fault history buffer size (HB=u8 size, LB reserved)
ID 13 (R) Fault history entry (HB=index u8, LB=value u8)

ID 14 (-W) Max relative modulation level setting (f8.8, %, 0..100)
ID 15 (R) Max boiler capacity & min modulation (HB=u8 kW, LB=u8 %)

ID 16 (-W) Room setpoint (f8.8, °C)
ID 17 (R) Relative modulation level (f8.8, %, 0..100)
ID 18 (R) CH water pressure (f8.8, bar, 0..5)
ID 19 (R) DHW flow rate (f8.8, l/min, 0..16)

ID 20 (RW) Day of week & time (special)
  HB: bits7..5 day(1=Mon..7=Sun, 0=no info); bits4..0 hours(0..23)
  LB: minutes(0..59)
ID 21 (RW) Date (HB=month u8 1..12, LB=day u8 1..31)
ID 22 (RW) Year (u16)

ID 23 (-W) Room setpoint CH2 (f8.8, °C)
ID 24 (-W) Room temperature (f8.8, °C)
ID 25 (R) Boiler flow water temp (f8.8, °C)
ID 26 (R) DHW temperature (f8.8, °C)
ID 27 (RW) Outside temperature (f8.8, °C)
ID 28 (R) Return water temperature (f8.8, °C)
ID 29 (R) Solar storage temperature (f8.8, °C)
ID 30 (R) Solar collector temperature (s16, °C)
ID 31 (R) Flow temperature CH2 (f8.8, °C)
ID 32 (R) DHW2 temperature (f8.8, °C)
ID 33 (R) Exhaust temperature (s16, °C)
ID 34 (R) Boiler heat exchanger temp (f8.8, °C)
ID 35 (R) Boiler fan speed setpoint/actual (HB=u8 Hz, LB=u8 Hz)
ID 36 (R) Flame current (f8.8, µA)
ID 37 (-W) Room temperature CH2 (f8.8, °C)
ID 38 (RW) Relative humidity (f8.8, %, 0..100)
ID 39 (R) Remote override room setpoint 2 (f8.8, 0..30; 0=no override)

ID 48 (R) DHW setpoint bounds (HB=s8 upper °C, LB=s8 lower °C)
ID 49 (R) Max CH setpoint bounds (HB=s8 upper °C, LB=s8 lower °C)
ID 56 (RW) DHW setpoint (f8.8, °C)
ID 57 (RW) Max CH water setpoint (f8.8, °C)

Ventilation / heat-recovery block:
ID 70 (R) Vent status (HB=Master flag8, LB=Slave flag8)
  HB:
    b0 Vent_enable
    b1 Bypass_position (manual mode only; 0=close,1=open)
    b2 Bypass_mode (0=manual,1=auto)
    b3 Free_ventilation_mode
    b4..7 reserved
  LB:
    b0 Fault_indication
    b1 Vent_mode_active
    b2 Bypass_status (0=closed,1=open)
    b3 Bypass_auto_status (0=manual,1=auto)
    b4 Free_ventilation_status
    b5 reserved
    b6 Diagnostic_indication
    b7 reserved
ID 71 (-W) Vent control setpoint (LB=u8 0..100% ; HB unused)
ID 72 (R) Vent ASF + OEM code (HB=flag8, LB=u8)
  HB: b0 service_request, b1 exhaust_fan_fault, b2 inlet_fan_fault, b3 frost_protection, b4..7 reserved
ID 73 (R) Vent OEM diagnostic code (u16)
ID 74 (R) Vent config (HB=flag8, LB=MemberID u8)
  HB: b0 system_type(0=central exhaust,1=heat-recovery), b1 bypass_present, b2 speed_control(0=3-speed,1=variable), b3..7 reserved
ID 75 (R) Vent OpenTherm version (f8.8)
ID 76 (R) Vent product version/type (HB=u8 type, LB=u8 version)
ID 77 (R) Relative ventilation (LB=u8 0..100%; HB unused)
ID 78 (RW) RH exhaust (LB=u8 0..100%; HB unused)
ID 79 (RW) CO2 exhaust (u16 ppm 0..2000)
ID 80 (R) Supply inlet temp (f8.8, °C)
ID 81 (R) Supply outlet temp (f8.8, °C)
ID 82 (R) Exhaust inlet temp (f8.8, °C)
ID 83 (R) Exhaust outlet temp (f8.8, °C)
ID 84 (R) Actual exhaust fan speed (u16 rpm 0..6000)
ID 85 (R) Actual inlet fan speed (u16 rpm 0..6000)
ID 86 (R) Vent remote-param flags (HB=transfer_enable flag8, LB=R/W flag8)
  bit0 Nominal ventilation value (ID87)
ID 87 (RW) Nominal ventilation value (HB=u8 0..100; LB unused)
ID 88 (R) Vent TSP count (HB=u8, LB reserved)
ID 89 (RW) Vent TSP (HB=index u8, LB=value u8)
ID 90 (R) Vent FHB size (HB=u8, LB reserved)
ID 91 (R) Vent FHB entry (HB=index u8, LB=value u8)

Brand strings (slave):
ID 93 (R) Brand string char (HB=index u8, LB=ASCII u8). Slave returns max index in HB on READ-ACK; out-of-range => DATA-INVALID.
ID 94 (R) Brand version string char (HB=index u8, LB=ASCII u8)
ID 95 (R) Brand serial string char (HB=index u8, LB=ASCII u8)

Counters / stats:
ID 96 (RW) Cooling operation hours (u16)
ID 97 (RW) Power cycles (u16)

RF sensor meta (master writes):
ID 98 (-W) RF sensor status info (special)
  HB: bits3..0 sensor_index; bits7..4 sensor_type:
      0000 room temp controllers
      0001 room temp sensors
      0010 outside temp sensors
      1111 not-defined
      others reserved
  LB: bits1..0 battery:
      00 no indication; 01 low; 10 nearly low; 11 ok
      bits4..2 RF strength: 000 none, 001..101 strength 1..5
      bits7..5 reserved

Remote override operating modes:
ID 99 (RW) Operating mode override (special)
  LB: bits3..0 HC1 mode; bits7..4 HC2 mode
  HB: bits3..0 DHW mode; bits7..4 DHW process bits:
      bit4 Manual_DHW_push2; bit5..7 reserved
  Mode enum (0..15):
    0 no override
    1 auto
    2 comfort
    3 precomfort
    4 reduced
    5 protection (frost)
    6 off
    7..15 reserved

ID 100 (R) Remote override room setpoint function (LB=flag8; HB reserved=0)
  b0 manual_change_priority (allow manual to overrule remote setpoint)
  b1 program_change_priority (allow program to overrule remote setpoint)
  b2..7 reserved

Solar storage block:
ID 101 (R) Solar storage status (HB=flag8 master, LB=flag8 slave)
  HB: bits2..0 SolarMode:
      000 off, 001 DHW eco, 010 DHW comfort,
      011 single boost, 100 continuous boost, others reserved
  LB:
    bit0 fault
    bits3..1 SolarMode (same coding)
    bits5..4 SolarStatus:
      00 standby
      01 loading by sun
      10 loading by boiler
      11 anti-legionella active
ID 102 (R) Solar ASF + OEM code (HB reserved flag8, LB=u8 OEM code)
ID 103 (R) Solar config (HB=flag8, LB=MemberID u8)
  LB bit0 system_type: 0=DHW preheat, 1=DHW parallel
ID 104 (R) Solar product version/type (HB=u8 type, LB=u8 version)
ID 105 (R) Solar TSP count (HB=u8, LB reserved)
ID 106 (RW) Solar TSP (HB=index u8, LB=value u8)
ID 107 (R) Solar FHB size (HB=u8, LB reserved)
ID 108 (R) Solar FHB entry (HB=index u8, LB=value u8)

Electricity production block:
ID 109 (RW) Electricity producer starts (u16; reset by writing 0 optional for slave)
ID 110 (RW) Electricity producer hours (u16; reset by writing 0 optional)
ID 111 (R)  Current electricity production (u16 W)
ID 112 (RW) Cumulative electricity production (u16 kWh; reset by 0 optional)

Burner/pump counters:
ID 113 (RW) Unsuccessful burner starts (u16)
ID 114 (RW) Flame signal too low count (u16)
ID 115 (R)  OEM diagnostic/service code (u16)
ID 116 (RW) Successful burner starts (u16; reset by 0 optional)
ID 117 (RW) CH pump starts (u16; reset by 0 optional)
ID 118 (RW) DHW pump/valve starts (u16; reset by 0 optional)
ID 119 (RW) DHW burner starts (u16; reset by 0 optional)
ID 120 (RW) Burner operation hours (u16; reset by 0 optional)
ID 121 (RW) CH pump operation hours (u16; reset by 0 optional)
ID 122 (RW) DHW pump/valve operation hours (u16; reset by 0 optional)
ID 123 (RW) DHW burner operation hours (u16; reset by 0 optional)

Protocol/product identity:
ID 124 (-W) OpenTherm version master (f8.8)
ID 125 (R)  OpenTherm version slave (f8.8)
ID 126 (-W) Master product version/type (HB=u8 type, LB=u8 version)
ID 127 (R)  Slave product version/type (HB=u8 type, LB=u8 version)
```
