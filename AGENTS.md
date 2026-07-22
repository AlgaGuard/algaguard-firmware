# Repository working rules

- Target ESP32-S3-DevKitC-1 with expected N16R8 verified at runtime; use PlatformIO, ESP-IDF, C/C++, and FreeRTOS.
- USB power only. Do not add battery ADC, charger, 3S, power-path, or MicroSD implementation before electrical approval.
- Preserve reserved GPIO19/20, GPIO35/36/37, GPIO38, GPIO43/44, and special/strapping GPIO0/3/45/46.
- Use non-blocking inputs, digital LEDs only, MQTT/TLS, HTTPS OTA, one-time BLE provisioning, contract fixtures, and explicit simulated-data flags.
- Never commit Wi-Fi, MQTT, signing, device private keys, or other credentials. Never weaken production OTA verification.
- Do not claim real-board results without measured hardware evidence.
