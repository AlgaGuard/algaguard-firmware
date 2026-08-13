# Repository working rules

- Target ESP32-S3-DevKitC-1 with expected N16R8 verified at runtime; use PlatformIO, ESP-IDF, C/C++, and FreeRTOS.
- USB power only. Do not add battery ADC, charger, 3S, or power-path implementation before electrical approval. MicroSD (SPI, GPIO10/11/12/13) and sensor-ADC channels (GPIO1/2, ADC1, for TDS/pH -- distinct from the still-prohibited battery/power-path ADC) are approved for development/physical-test builds as of 2026-08-14 by explicit project-owner request; see ALGAGUARD_ENABLE_REAL_SENSORS.
- Preserve reserved GPIO19/20, GPIO26-37 (shared flash+octal-PSRAM SPI bus -- wider than GPIO35/36/37 alone, confirmed via sdkconfig CONFIG_SPIRAM_CLK_IO=30/CONFIG_SPIRAM_CS_IO=26), GPIO38, GPIO43/44, and special/strapping GPIO0/3/45/46.
- Use non-blocking inputs, digital LEDs only, MQTT/TLS, HTTPS OTA, one-time BLE provisioning, contract fixtures, and explicit simulated-data flags.
- Never commit Wi-Fi, MQTT, signing, device private keys, or other credentials. Never weaken production OTA verification.
- Do not claim real-board results without measured hardware evidence.
