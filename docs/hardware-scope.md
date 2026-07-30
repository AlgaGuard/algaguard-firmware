# USB-only hardware scope

| Function | Pin and rule |
| --- | --- |
| OLED | 128x64 I2C, address `0x3C`, SDA GPIO8, SCL GPIO9 |
| Buttons | Up GPIO4, Down GPIO5, Select GPIO6, Back GPIO7; active-low, software debounce |
| LEDs | Red GPIO14, Green GPIO15, Blue GPIO16; digital only with external current-limiting resistors |
| Reserved | GPIO19/20 native USB; GPIO35/36/37 octal memory; GPIO38 onboard RGB; GPIO43/44 UART0; GPIO0/3/45/46 special |
| Power | USB only; no battery ADC, charger, 3S, or power-path code |

LED semantics are green for provisioned/cloud-connected, blue for setup/provisioning/OTA, and red for a fault requiring attention. Destructive reset actions require the menu confirmation state. MicroSD is deferred and its adapter remains disabled.

The development-only local OLED demo uses the same pins and `0x3C` address. It
shows `WIFI NOT CONFIG`, `CLOUD OFFLINE`, and `LOCAL SIMULATION`; green must not
indicate connectivity in this mode. The simulated OLED values remain entirely
local and are separate from the development cloud telemetry simulator.
