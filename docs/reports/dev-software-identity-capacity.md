# Development software identity capacity

This report is reproducible with `python tools/development_identity_capacity.py`.

- Shared `nvs` partition: 64 KiB (`0x9000`–`0x19000`).
- `otadata`: `0x19000`–`0x1B000`; `phy_init`: `0x1B000`–`0x1C000`.
- Alignment gap: `0x1C000`–`0x20000` (16 KiB).
- `ota_0`: `0x20000`, 6 MiB; `ota_1`: `0x620000`, 6 MiB. Neither slot was reduced.
- Pinned ESP-IDF 6.0.1 PSA maximum export size for an RSA-3072 key pair: 1,787 bytes.
- One slot: 18,683 bytes; two slots: 37,366 bytes.
- With 25% NVS page/entry overhead: 46,708 bytes; headroom: 18,828 bytes (about 28.7%).

`TARGET_RUNTIME_MEASUREMENT_DEFERRED`: actual `psa_export_key` output length
requires an authorized ESP32-S3 runtime or QEMU execution. The PSA maximum is
the authoritative storage safety bound.
