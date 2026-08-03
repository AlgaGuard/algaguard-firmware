# ESP32-S3 eFuse allocation and manual provisioning runbook

| Key block | Purpose | Protection |
| --- | --- | --- |
| KEY0 | HMAC downstream for RSA DS | Read-protected; dedicated to DS |
| KEY1 | HMAC upstream for NVS encryption | Read-protected; dedicated to NVS |
| KEY2 | Flash Encryption XTS-AES key | Read/write protected after activation |
| KEY3 | Secure Boot v2 public-key digest | Write-protected after verification |
| KEY4-5 | Reserved for approved recovery/rotation | Do not program without a new ADR |

Preflight must confirm chip/revision, key purposes, free blocks, secure boot, flash encryption, download/JTAG state, and NVS HMAC state. It is read-only and must be recorded before any action below.

1. Verify board identity, stable USB power, approved production profile, and preflight report.
2. Obtain owner approval for each irreversible transition.
3. Provision DS and NVS HMAC keys into separate allocated blocks; verify purpose and read protection.
4. Provision Secure Boot v2 digest, then Flash Encryption according to the approved release-image process.
5. Restrict download/JTAG only after first encrypted, signed boot succeeds.
6. Verify DS signing, encrypted NVS, certificate loading, and OTA rollback.

Every key-burn, secure-boot, flash-encryption, or debug-disable command is **MANUAL — IRREVERSIBLE — DO NOT RUN WITHOUT OWNER APPROVAL**. This repository has no automatic eFuse write. Lost eFuse keys cannot be recovered.
