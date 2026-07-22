# AlgaGuard USB-powered firmware foundation

Minimum ESP32-S3-DevKitC-1 PlatformIO/ESP-IDF foundation for the platform-first phase. It defines runtime N16R8 diagnostics, OLED/menu/QR state, non-blocking buttons, digital LEDs, BLE/Wi-Fi provisioning boundaries, MQTT/TLS batching and ACK state, deterministic simulated telemetry, queue abstraction, profile cache, and verified HTTPS OTA state transitions.

The prototype is USB powered only. Battery ADC, charging, 3S power-path logic, and MicroSD access are intentionally absent. The SD queue adapter remains disabled until electrical validation.

```sh
pio test -e native
pio run -e esp32-s3-devkitc-1
```

No real board has been flashed or electrically validated by this repository.
