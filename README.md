# AlgaGuard USB-powered firmware foundation

Minimum ESP32-S3-DevKitC-1 PlatformIO/ESP-IDF foundation for the platform-first phase. It defines runtime N16R8 diagnostics, OLED/menu/QR state, non-blocking buttons, digital LEDs, BLE/Wi-Fi provisioning boundaries, MQTT/TLS batching and ACK state, deterministic simulated telemetry, queue abstraction, profile cache, and verified HTTPS OTA state transitions.

The reproducible N16R8 build, dual-slot OTA layout, typed environment
configuration, secure-storage boundary, and explicit startup state machine are
documented in [docs/firmware-foundation.md](docs/firmware-foundation.md).
BLE, Wi-Fi, certificate bootstrap, MQTT mTLS, command execution, durable queue
persistence, and OTA execution intentionally return explicit unavailable or
not-implemented results until their integration sprint.

The credential foundation defines a local key-provider boundary with no key-export API, public CSR submission, bounded certificate-chain storage, bootstrap and MQTT TLS loading interfaces, exact CN/SAN UUID binding, a two-certificate rotation/recovery state machine, revocation/expiry behavior, time-synchronization policy, and PEM log redaction. Production implementations must keep key operations inside encrypted NVS/flash-backed storage where those ESP32 protections are enabled; this sprint does not claim a secure element or physical validation.

MQTT batch/queue/replay, OTA artifact/manifest, certificate/chain/CSR, and unsynchronized-time holdover bounds are compile-time configurable PlatformIO flags with safe defaults.

The prototype is USB powered only. Battery ADC, charging, 3S power-path logic, and MicroSD access are intentionally absent. The SD queue adapter remains disabled until electrical validation.

```sh
pio test -e native
pio run -e esp32-s3-devkitc-1
pio run -e esp32-s3-dev-local-oled-demo
```

`esp32-s3-dev-local-oled-demo` is a development-only, USB-powered presentation
profile. It generates bounded local sample values for the OLED once per second,
labels them `LOCAL SIMULATION`, keeps Wi-Fi and cloud access off, and preserves
BLE setup advertising. It is disabled by default and rejected by production,
release, and DS-identity profiles. These local readings are not cloud telemetry
and are not scientifically validated cultivation ranges.

No real board has been flashed or electrically validated by this repository.
