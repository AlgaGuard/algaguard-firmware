# ADR: Development software identity and deferred production DS identity

## Decision

The active university/demo architecture is `DEV_SOFTWARE_KEY`. It generates RSA-3072 with ESP-IDF 6.0.1 PSA Crypto, writes a CSR through `mbedtls_pk_wrap_psa` and `mbedtls_x509write_csr_*`, then stores the PKCS#1 DER private key, client certificate and CA chain in the ordinary `algaguard_dev_identity` NVS namespace.

**DEV_SOFTWARE_KEY stores the device private key in ordinary ESP32-S3 flash-backed NVS. It is intended only for university development and demonstration. It does not protect the key against physical flash extraction and must never be enabled for a production deployment.**

This profile requires both `ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY` and `ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE=1`. It is blocked for `ALGAGUARD_PRODUCTION_BUILD`, never configures `esp_tls_cfg_t.ds_data`, and must be revoked and erased before moving to production.

The deferred production architecture is `FACTORY_PREPARED_DS_IDENTITY`: an approved external factory process creates the RSA key and opaque DS context, then owner-approved eFuse provisioning occurs. Firmware only loads that opaque context.

The Device Service accepts `RSA_3072` and issues client certificates with `digitalSignature` and `clientAuth`. ESP-IDF 6.0.1 provides `psa_generate_key`, `psa_export_key`, `psa_import_key`, `psa_destroy_key`, `mbedtls_pk_wrap_psa`, `mbedtls_x509write_csr_pem`, `nvs_set_blob`, and `nvs_commit` for the development implementation. Temporary buffers are zeroized with `mbedtls_platform_zeroize`.

## Alternatives

| Option | Result |
| --- | --- |
| Development software key in ordinary NVS | Selected only for the university prototype. Explicitly insecure against physical extraction. |
| Factory-prepared RSA DS + HMAC-encrypted NVS | Deferred production option. Private parameters are sealed outside ordinary firmware. |
| External secure element | Deferred hardware option. |

Target-side DS sealing was rejected: ESP-IDF's `esp_ds_encrypt_params` requires complete RSA internals plus the raw HMAC key, while the supported operational path expects external/factory preparation and manual eFuse provisioning. No normal application path burns eFuses.

## Profiles

- `HOST_TEST`: virtual deterministic identity; no eFuse access.
- `SECURITY_CI`: DS/NVS compile and virtual-state validation only; no retained CI keys.
- `DEV_SOFTWARE_KEY`: opt-in development identity; reports `INSECURE_DEVELOPMENT_IDENTITY` and must visibly warn `INSECURE DEV KEY`.
- `FACTORY_PREPARED_DS_IDENTITY`: deferred production design: Secure Boot v2, release Flash Encryption, HMAC NVS, DS, restricted download/JTAG, and signed OTA after manual approval.

Migration: revoke the development certificate, erase the development identity, provision the factory-prepared DS identity or a secure element, then issue a new production certificate. Development credentials must never be reused in production.
