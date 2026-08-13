# USB-only hardware scope

| Function | Pin and rule |
| --- | --- |
| OLED | 128x64 I2C, address `0x3C`, SDA GPIO8, SCL GPIO9 |
| BH1750 (light) | I2C, address `0x23`, shares the OLED's SDA GPIO8 / SCL GPIO9 bus |
| DS3231 (RTC) | I2C, address `0x68`, shares the same bus |
| DS18B20 (temperature) | OneWire, data GPIO17, external 4.7kOhm pull-up to 3.3V required |
| TDS sensor | Analog, GPIO1 (ADC1_CH0) |
| pH sensor (PH4502C) | Analog, GPIO2 (ADC1_CH1); power at 3.3V, not 5V, to keep the output within the ADC's safe input range |
| SD card (offline buffer/replay) | SPI, CS GPIO10, MOSI GPIO11, SCK GPIO12, MISO GPIO13 |
| Buttons | Up GPIO4, Down GPIO5, Select GPIO6, Back GPIO7; active-low, software debounce |
| LEDs | Red GPIO14, Green GPIO15, Blue GPIO16; digital only with external current-limiting resistors |
| Reserved | GPIO19/20 native USB; GPIO26-37 shared flash+octal-PSRAM SPI bus; GPIO38 onboard RGB; GPIO43/44 UART0; GPIO0/3/45/46 special |
| Power | USB only; no battery ADC, charger, 3S, or power-path code |

LED semantics are green for provisioned/cloud-connected, blue for setup/provisioning/OTA, and red for a fault requiring attention. Destructive reset actions require the menu confirmation state.

DS18B20/BH1750/DS3231/TDS/pH readings feed a single derived Nutrient Strength Index (`nutrientPercent`, 0-100%) computed on-device from TDS + pH + temperature -- see `include/algaguard/nutrient_index.hpp`. This replaces the separate nitrate/phosphate/potassium fields the platform previously reported; there is only a TDS probe, not three separate ion-selective electrodes. The TDS/pH conversion formulas and the NSI's own reference constants are calibration starting points, not a validated instrument -- see the comments in `analog_conversion.hpp`/`nutrient_index.hpp`.

The SD card is an offline buffer, not persistent storage: samples are queued there only while the device can't reach the cloud (no Wi-Fi, or Wi-Fi but no MQTT session), then replayed once connectivity returns -- see `include/algaguard/sd_queue.hpp`.

The development-only local OLED demo uses the same pins and `0x3C` address. It
shows `WIFI NOT CONFIG`, `CLOUD OFFLINE`, and `LOCAL SIMULATION`; green must not
indicate connectivity in this mode. The simulated OLED values remain entirely
local and are separate from the development cloud telemetry simulator.
