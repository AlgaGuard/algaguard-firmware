# Development-only local OLED demo

The `esp32-s3-dev-local-oled-demo` PlatformIO environment is the only profile
that defines both `ALGAGUARD_LOCAL_DEMO_MODE=1` and
`ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS=1`. Security-profile guards reject this
combination in production, release, DS-identity, and non-demo builds.

The generator updates once per second using deterministic, smooth, bounded
drift. It provides presentation values for temperature, pH, light intensity,
nitrate, phosphate, and potassium. The values are local simulation data only;
they are not uploaded, do not represent scientific recommendations, and must
not be described as readings from physical sensors.

The 128x64 OLED uses I2C address `0x3C`, SDA GPIO8, and SCL GPIO9. The six pages
are Home, Temperature/pH, Light, Nutrients, Device status, and About. Buttons on
GPIO4/GPIO5/GPIO6/GPIO7 provide previous, next, details, and home navigation
with the existing non-blocking debounce boundary. Status text explicitly says
`DEMO MODE`, `WIFI NOT CONFIG`, `CLOUD OFFLINE`, and `LOCAL SIMULATION`.

BLE setup advertising remains available. Wi-Fi initialization and connection,
MQTT, cloud credentials, NVS sensor persistence, bootstrap, and OTA are not
started by this profile. Green GPIO15 never claims network or cloud success;
blue GPIO16 may indicate BLE advertising, while red GPIO14 remains reserved for
a real fault. GPIO38 remains unused.

Build and upload only after the USB-only board checks pass:

```sh
pio run -e esp32-s3-dev-local-oled-demo
pio run -e esp32-s3-dev-local-oled-demo -t upload --upload-port COM16
```

Do not use a full-chip erase or connect a battery, 3S supply, or MicroSD card.

The separate `esp32-s3-dev-qr-onboarding-demo` profile preserves these local
simulation pages and BLE advertising while adding full-screen `Scan to add`, QR
ready, and QR-expired pages. A QR invitation is public, short-lived, and
one-time; it contains no Wi-Fi, session, account, key, certificate, or cloud
secret. This combined profile may initialize the Wi-Fi runtime but cannot start
a connection until a signed QR-bound BLE request consumes the one-shot gate.
