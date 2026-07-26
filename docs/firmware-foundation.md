# ESP32-S3 N16R8 firmware foundation

The active PlatformIO environment is `esp32-s3-devkitc-1` using ESP-IDF only.
Its project-local board manifest is
`boards/algaguard-esp32-s3-devkitc-1-n16r8.json`; it targets an
ESP32-S3-DevKitC-1 populated with an N16R8 module:

- 16 MB QIO flash at 80 MHz
- 8 MB octal PSRAM at 80 MHz
- native USB Serial/JTAG with GPIO19/20 reserved
- development environment selected at compile time

Runtime diagnostics report chip revision, detected flash and PSRAM sizes,
flash mode, running partition, firmware version, and build environment. A
physical module that does not expose at least 16 MB flash and 8 MB PSRAM enters
the explicit degraded startup state with `BOARD_PROFILE_MISMATCH`.

ESP-IDF intentionally writes a DIO ROM boot header for a QIO selection, then
the bootloader enables quad mode during initialization. Tests therefore assert
the tracked QIO selector and also the expected DIO/80 MHz/16 MB image header.

## Partition layout

| Partition | Offset | Size | Purpose |
| --- | ---: | ---: | --- |
| `nvs` | `0x9000` | 24 KiB | Versioned provisioning and public credential metadata |
| `otadata` | `0xF000` | 8 KiB | ESP-IDF OTA slot selection and rollback state |
| `phy_init` | `0x11000` | 4 KiB | ESP-IDF radio calibration data |
| `ota_0` | `0x20000` | 6 MiB | Primary OTA application slot |
| `ota_1` | `0x620000` | 6 MiB | Secondary OTA application slot |

The remaining flash is deliberately unallocated headroom. No arbitrary data
partition is used for secrets. CI requires both OTA slots, checks overlap and
flash bounds, and requires each slot to fit the built firmware plus 1 MiB.
ESP-IDF bootloader rollback support is enabled; the next integration sprint
will implement pending-image verification and confirmation.

## Configuration and storage boundary

`include/algaguard/config.hpp` defines versioned development, campus
placeholder, and production placeholder profiles. Only development is active.
No private key, Wi-Fi password, shared MQTT password, localhost address, or
fixed cloud IP is compiled into the configuration.

`include/algaguard/storage.hpp` defines the versioned `algaguard-v1` storage
record and a host fake. It stores only a private-key handle, never exportable
private-key material. Hardware-backed key storage is not claimed.

## Isolated PlatformIO cache recovery

Set `PLATFORMIO_CORE_DIR=D:\algaguard-project\.platformio-core`. If only that
isolated cache is corrupt, preserve it and let PlatformIO recreate a clean one:

```powershell
$cache = 'D:\algaguard-project\.platformio-core'
$backup = 'D:\algaguard-project\.platformio-core.corrupt'
Move-Item -LiteralPath $cache -Destination $backup
$env:PLATFORMIO_CORE_DIR = $cache
pio pkg install -e esp32-s3-devkitc-1
```

This does not modify the user-wide PlatformIO cache. Delete the preserved
backup only after the rebuilt isolated environment has been verified.
