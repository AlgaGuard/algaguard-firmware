# BLE/Wi-Fi provisioning host contract

## Development-only physical session installer (14C2A)

`esp32-s3-dev-ble-wifi-physical-test` alone contains the volatile physical
session installer (`VOLATILE_PHYSICAL_SESSION_INSTALLER`). It is rejected by
production, release, and DS-identity guard configurations. A session is RAM
only, has one active instance, expires at its supplied tick, has no read-back
operation, and is cleared after acceptance, terminal rejection, expiry, reset,
shutdown, or destruction.

The COM16 control channel accepts a bounded binary frame only: magic `AGS1`,
version `1`, command, big-endian payload length, payload, then big-endian CRC32
of the preceding frame bytes. The maximum payload is 640 bytes and partial
frames time out after 100 ticks. Commands are install, clear, and safe-state
query. Safe replies are only `SESSION_ARMED`, `SESSION_INSTALL_REJECTED`,
`SESSION_CLEARED`, and `SESSION_INSTALLER_DISABLED`; no frame or field values
are logged or returned.

`tools/install_physical_test_session.py` requires an explicit `--port`, prompts
for the session token with no echo, and never accepts that token as a command
line value or writes it to a file. Its dry run uses only a synthetic fixture.

On BLE `ACCEPTED`, the credential handoff moves once into
`WifiConnectionRuntime`; the session installer and BLE-side handoff clear. The
physical profile initializes station mode with `WIFI_STORAGE_RAM`; connection
execution remains disabled at boot and requires the one-shot operator gate
described below. OLED, LEDs, and logs use only safe state tokens such as
`SESSION_ARMED`, `WIFI_RUNTIME_READY_NOT_CONNECTED`, and `WIFI_HANDOFF_INSTALLED`.

Micro-Sprint 14C2B is the explicit later activation path: install one volatile
runtime session, submit one valid BLE request, enable one controlled station
attempt, verify `ACCEPTED` and `GOT_IP`, then clear all volatile state. No real
session or Wi-Fi credential belongs in this document.

## One-shot physical Wi-Fi execution gate (14C2B1)

The physical profile starts with `WIFI_CONNECT_TEST_DISABLED`. The COM16 binary
protocol adds `ARM_ONE_WIFI_CONNECTION_TEST`, carrying only an 8-byte bounded
lifetime. It can move to `WIFI_CONNECT_TEST_ARMED` once, then is consumed before
`startConnection` is authorized; expiry, clear, reset, and shutdown remove the
authorization. The safe states are disabled, armed, consumed, expired, and
cleared. Production, release, and DS profiles reject physical-test mode.

The operator utility keeps its explicit `--port` requirement and supports
`--command arm-wifi-test` with no secret input. The requested lifetime is in
seconds and is converted to the target's 100 Hz monotonic tick domain; it is
bounded to ten minutes. It does not install a Wi-Fi credential or persistent
setting. A controlled test must explicitly arm this gate before the single
physical connection; there is no auto-connect.

## Development physical-session handoff client flow

`DEVELOPMENT_ONLY_PHYSICAL_SESSION_APPROVAL` is disabled by default and absent
from release navigation. A mobile user enters only a short `userCode`; its
existing RAM-only claim session is sent once in the authenticated approval body
and then moves into BLE provisioning. The PC utility retains the high-entropy
`deviceCode` only in process memory, prints only the user code and expiry, then
polls/redeems over the exact verified-HTTPS development endpoint.
The redeemed session bundle passes directly to the existing installer seam in
memory and then to the explicit COM port. It rejects HTTP, redirects, alternate
hosts, automatic port selection, and token arguments. Synthetic dry-run remains
available without network or serial activity. Live execution requires the
separate `--live-development` opt-in.

This contract defines the request and deterministic state machine plus the
compile-only ESP-IDF GATT boundary. Protocol version `1` uses the
existing field names `sessionId`, `deviceId`, `sessionToken`, `ssid`, and
`password`. A missing `protocolVersion` means version `1`; any supplied version
must equal `1`.

## Fields and limits

| Field | Required | Limit | Validation |
|---|---|---:|---|
| `sessionId` | yes | 64 bytes | non-empty, no embedded NUL |
| `deviceId` | yes | 9 bytes | canonical `AG-` plus six digits |
| `sessionToken` | yes | 512 bytes | non-empty, no embedded NUL |
| `ssid` | yes | 32 bytes | non-empty, no embedded NUL |
| `password` | yes | 63 bytes | non-empty, no embedded NUL |

Open networks and a Wi-Fi security-type field are not enabled because the
current repository contains no established contract for them. Repeating a
field in the request builder is ambiguous and produces `MALFORMED_PAYLOAD`.

## GATT service contract

The service UUID `0000a1a0-0000-1000-8000-00805f9b34fb` already exists in the
firmware configuration and in the mobile QR claim flow. The sibling contracts
and mobile repositories did not define characteristic UUIDs, so this firmware
contract version defines the following values. The mobile client must consume
these values before physical provisioning is enabled.

| Item | UUID | Properties | Maximum |
|---|---|---|---:|
| Provisioning service | `0000a1a0-0000-1000-8000-00805f9b34fb` | primary service | — |
| Provisioning request | `0000a1a1-0000-1000-8000-00805f9b34fb` | write with response only | 1024 bytes |
| Provisioning status | `0000a1a2-0000-1000-8000-00805f9b34fb` | read and notify; never writable | 96 bytes |

The request characteristic carries bytes only. It does not parse, reassemble,
validate, persist, or log a provisioning request in this sprint. Status is
constructed only from allowlisted status/reason enums: `READY`, `RECEIVING`,
`ACCEPTED`, `REJECTED`, `EXPIRED`, `CANCELLED`, and a safe reason code.
It never contains a request field or raw payload.

`BLE_LINK_SECURITY_NOT_YET_ENABLED` is the authoritative policy classification
for this skeleton. NimBLE is configured only as a peripheral GATT server for
compile validation; pairing, bonding, passkeys, authentication, and encryption
are not enabled or claimed.

## Advertising and callback boundary (compile-only)

The safe development advertisement name is `AlgaGuard-Setup`. Advertising
contains only the connectable/discoverable flags and the provisioning service
UUID. It does not contain a session ID or token, network name, password,
organization identifier, device UUID, certificate, or key material. It starts
only from the NimBLE host-sync callback, pauses naturally on connection, and is
requested again after the active connection disconnects. Repeated start calls
are safe; shutdown prevents further advertising. This is a compile proof, not
a claim of physical advertising success.

Only one connection is active. The first connection enters `READY`; a concurrent
connection is safely rejected without exposing either connection identifier.
The GATT write callback copies at most one framed request (267 bytes) to a
bounded temporary buffer, delegates it to the host controller, then clears the
temporary buffer. It never logs the bytes. Completion moves exactly one
assembled payload into a move-only handoff and publishes `COMPLETE`; this
sprint neither parses nor applies that payload.

The status characteristic reads the latest allowlisted status/reason message
and may notify a subscribed client. It retains only the latest safe status when
there is no subscription; it never queues notifications. The existing startup
task invokes non-blocking timeout polling with monotonic FreeRTOS ticks.
Timeout clears framed data and produces `TIMED_OUT/TRANSPORT_TIMEOUT`.
Active disconnect clears framed data, completed handoff, notification state,
and connection state before publishing `READY/DISCONNECTED`; reconnect is then
allowed. GAP identifiers remain internal.

The target transport owns no request payload after its bounded callback returns.
Repeated `init()` and `startGattService()` calls are idempotent. A stop/start
cycle reuses its single registered GATT definition; stack teardown is reserved
for `shutdown()` so NimBLE resource counts are not accumulated.

## Framed request transport (host-only)

Micro-Sprint 13C1 defines a deterministic binary frame before any target
callback consumes a request. The frame is big-endian and contains, in order:
protocol version (1 byte), message ID (4 bytes), fragment index (2 bytes),
fragment count (2 bytes), payload length (2 bytes), then payload bytes. The
header is 11 bytes. A frame is at most 267 bytes, with at most 256 payload
bytes; all fragments together are limited to the existing 1024-byte request
maximum.

Only one message ID is active per connection. The first fragment must be index
zero, then every next fragment must be the exact next index. Duplicate,
out-of-order, changed-ID, changed-count, malformed, unsupported-version, and
overflow frames fail closed. Empty payload fragments are not valid in v1.
Completion occurs only after every index through `fragmentCount - 1` was
accepted. The payload is exposed through a move-only, one-time bounded handoff;
the transport clears its own temporary buffer immediately after handoff.

The host transport uses injected monotonic ticks. The named development
constant `kBleProvisioningDevelopmentTransportTimeoutTicks` is 30 ticks.
Timing starts at the first accepted fragment and is refreshed only by a newly
accepted fragment. It does not represent a hardware timing guarantee. Timeout
clears pending data and emits `TIMED_OUT` with `TRANSPORT_TIMEOUT`.

Disconnect clears pending fragments, assembled data, active/completed message
IDs, and replay tracking so a new BLE connection may start a new request.
Within one connection, a completed message cannot be emitted twice and any
additional frame is rejected as `REPLAY_REJECTED`. Safe status values are
`READY`, `RECEIVING`, `COMPLETE`, `ACCEPTED`, `REJECTED`, `TIMED_OUT`, and
`CANCELLED`; statuses contain only allowlisted status/reason codes and remain
within 96 bytes.

## Canonical payload validation and Wi-Fi handoff

The authoritative payload is the mobile and contracts repository's canonical
UTF-8 JSON object. It has no aliases and no optional fields in v1:

```json
{
  "schema": "urn:algaguard:schema:onboarding:ble-provisioning-request:v1",
  "schemaVersion": "1.0.0",
  "sessionId": "UUID",
  "deviceId": "AG-000001",
  "sessionToken": "32-to-96 URL-safe characters",
  "ssid": "network name",
  "password": "network password"
}
```

The parser accepts at most 1024 bytes. It accepts only this object shape and
exact field names, rejects duplicates, unknown fields (including alternate
secret-looking names), missing fields, unsupported schema/version, malformed
UTF-8 or JSON escapes, embedded NUL, trailing data, and values outside the
existing bounded request limits. The `sessionId` is UUID-shaped; `deviceId`
remains the existing canonical `AG-` plus six digits; the token has the
contract's 32–96 URL-safe-character shape. Parser scratch buffers are cleared
on success and every failure path.

An injected development session supplies the expected session ID, device ID,
secret session token, and bounded remaining lifetime. The COM control boundary
converts that lifetime into an absolute ESP monotonic expiry tick and caps it
at five minutes. These values are held only by the
existing bounded provisioning state machine and will later be populated from
the secure-bootstrap session source. The parser adapts directly into the
existing `BleWifiProvisioningRequest`; it does not create a second validation
model. Validation maps only to safe characteristic values: `ACCEPTED/OK`, or
`REJECTED` with `MALFORMED_PAYLOAD`, `UNSUPPORTED_VERSION`,
`DEVICE_ID_MISMATCH`, `SESSION_MISMATCH`, `SESSION_EXPIRED`,
`REPLAY_REJECTED`, `FIELD_TOO_LONG`, or `INVALID_TRANSITION`.

After `ACCEPTED`, the controller exposes exactly one move-only credential
handoff containing only SSID and password. It never contains the session token,
raw BLE payload, parser scratch, key, or certificate material. Clearing or
destroying that handoff zeroizes its password. This boundary is classified as
`WIFI_CREDENTIAL_HANDOFF_READY_NOT_CONNECTED`: it starts no Wi-Fi connection,
persists nothing, and makes no network request. A replay cannot overwrite the
pending accepted handoff; an explicit reset plus a new active session is
required for another request. Disconnect clears the active validation session
without duplicating an already accepted handoff; reset and shutdown clear all
credential state.

## States and transitions

The nominal path is:

`IDLE -> SESSION_READY -> BLE_CONNECTED -> PAYLOAD_RECEIVING -> PAYLOAD_VALIDATED -> ACCEPTED -> CLEARED`

An invalid payload moves `PAYLOAD_RECEIVING -> REJECTED`. An expired active
session moves to `EXPIRED`; cancellation of any active state moves to
`CANCELLED`. `ACCEPTED`, `REJECTED`, `EXPIRED`, and `CANCELLED` permit terminal
teardown to `CLEARED`. Every other transition fails closed. A request received
after acceptance is rejected as a replay and cannot create a second handoff.

Validation binds the payload to the current canonical device ID and active
session ID/token and rejects expiry at or after the session deadline.

## Host Wi-Fi credential lifecycle and connection state machine

`WIFI_CONNECTION_HOST_STATE_MACHINE_READY` classifies the host-only next
boundary after an accepted provisioning request. It consumes the existing
move-only Wi-Fi credential handoff exactly once and owns only bounded SSID and
password buffers. The handoff has no session ID, session token, or BLE payload.
The password is cleared by the same explicit `clear()` path used by destruction.

The dependency-light `WifiConnectionAdapter` accepts temporary bounded views
only for `beginConnect`, then clears its sensitive driver input. Its host fake
records only a call count, SSID length, password-present flag, and cleanup
counts; it retains no SSID or password text. No ESP-IDF headers or Wi-Fi API
calls are present in this boundary.

The named development-only policy constants are
`kWifiConnectTimeoutTicks = 20`, `kWifiRetryDelayTicks = 5`, and
`kWifiMaxConnectAttempts = 3`. An explicit start moves
`CREDENTIALS_READY -> CONNECTING`. A successful driver event moves to
`CONNECTED`, then clears the manager's credential buffers. Network-not-found,
transient failure, connect-timeout, or disconnect during an attempt may move to
`RETRY_WAIT`; retry starts at the deterministic next tick and is bounded by the
maximum attempt count. Exhaustion reaches a terminal safe state and clears
credentials. Authentication failure is terminal and never retries.

The safe state set is `IDLE`, `CREDENTIALS_READY`, `CONNECTING`, `RETRY_WAIT`,
`CONNECTED`, `AUTH_FAILED`, `NETWORK_NOT_FOUND`, `TIMED_OUT`, `CANCELLED`,
`DISCONNECTED`, and `CLEARED`. Results contain only state, allowlisted reason,
attempt number, retry information, and credential/zeroization booleans.
Reasons include `OK`, `NO_CREDENTIALS`, `INVALID_CREDENTIALS`,
`CONNECT_IN_PROGRESS`, `AUTHENTICATION_FAILED`, `NETWORK_NOT_FOUND`,
`TRANSIENT_FAILURE`, `CONNECT_TIMEOUT`, `RETRY_EXHAUSTED`, `CANCELLED`,
`DISCONNECTED`, `INVALID_TRANSITION`, and `HANDOFF_ALREADY_PRESENT`.

Cancellation, explicit reset, shutdown, and repeated cleanup safely cancel or
disconnect the adapter, clear its temporary input, zeroize credentials, and
leave `CLEARED`. A post-connection disconnect does not reuse cleared
credentials; a newly provisioned handoff is required. These constants are not
production network policy. ESP-IDF station integration, event-loop callbacks,
physical connection, persistence, secure bootstrap, and MQTT remain deferred.

## ESP-IDF station adapter (compile-only)

`WIFI_STATION_ADAPTER_COMPILE_ONLY` describes the ESP-IDF 6.0.1 target boundary.
`EspIdfWifiConnectionAdapter` is station-only and is initialized explicitly after
NVS initialization. It creates the default event loop, default station netif,
and Wi-Fi driver only through its idempotent lifecycle; this repository had no
prior shared network initializer. Repeated `init`, `start`, `stop`, and
`shutdown` calls are safe. The adapter registers its Wi-Fi and IP handlers once,
never enables access-point mode, and uses `WIFI_STORAGE_RAM`, so credential
persistence remains disabled.

The adapter accepts bounded credential views only when the host state machine
explicitly starts an attempt. It copies them into a zero-initialized local
`wifi_config_t`, calls `esp_wifi_set_config`, then overwrites that local
structure before returning. It calls `esp_wifi_connect` only after successful
explicit configuration; initialization and `WIFI_EVENT_STA_START` never connect
automatically. The adapter retains no plaintext credential buffers and does not
log SSID, password, event payloads, or driver strings.

ESP-IDF events map into the host-owned state machine: `WIFI_EVENT_STA_START` is
readiness only; `WIFI_EVENT_STA_CONNECTED` is link association only; and
`IP_EVENT_STA_GOT_IP` is the sole `CONNECTED/OK` success signal, which clears
manager credentials. Disconnect classification maps authentication-related
reasons to `AUTHENTICATION_FAILED`, no-AP variants to `NETWORK_NOT_FOUND`,
manual local departure to `DISCONNECTED`, and all other or unknown reasons to
`TRANSIENT_FAILURE`. Retry, timeout, terminal handling, and credential clearing
remain exclusively in the host state machine; authentication failure is never
retryable.

`WifiConnectionRuntime` is the narrow bridge between the target callbacks and
the host policy. Its periodic `poll` hook runs from the existing startup task
using FreeRTOS monotonic ticks, performs no blocking sleep, and starts retries
only when the host policy permits them. It does not consume BLE credentials in a
callback or make a Wi-Fi API call from BLE code. Physical connection, credential
persistence, secure-bootstrap HTTP, MQTT, captive portals, enterprise Wi-Fi,
and roaming remain deferred.

## Physical-test harness and USB-only preflight

`INSECURE_DEVELOPMENT_PHYSICAL_PROVISIONING_TEST` is a narrowly scoped,
development-only build profile: `esp32-s3-dev-ble-wifi-physical-test`. It
requires explicit development and physical-test opt-in macros, requires RAM-only
Wi-Fi configuration, forbids DS identity, and is rejected at compile time for
production or release builds. The firmware carries the persistent warning
`PHYSICAL TEST MODE`; it is not production eligible.

The physical harness has an explicit RAM-only `VolatilePhysicalTestSession`
boundary for a later operator-provided development session. It accepts no
source-defined, build-flag, or artifact-defined session value, has no log or
display accessor for its fields, and clears all bounded buffers on reset and
destruction. The boundary is disabled outside the physical-test profile. Its
removal/deactivation path is to omit the physical-test environment and its
required opt-in macros; no NVS migration or stored data exists.

Boot preflight requires an ESP32-S3, exactly 16 MiB flash, present 8 MiB PSRAM,
an OTA-data/running-partition layout, and the active physical-test profile.
These failures stop readiness before BLE or Wi-Fi use. OLED initialization is
separately recorded and must succeed for a displayed physical readiness claim.
The physical OLED was measured over the USB-only physical-test harness at
seven-bit I2C address `0x3C` on GPIO8/GPIO9. The previous `0x70` configuration
is superseded. Buttons remain on
GPIO4–GPIO7, and external LEDs on GPIO14/GPIO15/GPIO16; USB, octal-memory,
onboard-RGB, UART-fallback, and strapping pins remain untouched.

The OLED view always includes `PHYSICAL TEST MODE` and a safe state token such
as `BLE READY`, `RECEIVING`, `VALIDATING`, `WIFI CONNECTING`, `WIFI CONNECTED`,
`AUTH FAILED`, `NETWORK NOT FOUND`, `TIMED OUT`, `CANCELLED`, or `RESET
REQUIRED`. It never contains a network name, password, session value, payload,
IP address, or raw driver reason. LED mappings use only the three external LEDs:
blue steady for BLE readiness, blue bounded pulse while receiving/validating,
blue/green bounded pattern while connecting, green steady when connected, red
bounded pattern on failure, and red steady for reset-required. Existing fatal
LED priority remains authoritative; GPIO38 is never used.

Readiness mode starts BLE only. It deliberately does not initialize a Wi-Fi
attempt, bootstrap, MQTT, or OTA path, and emits only redacted profile,
preflight, state, and attempt diagnostics. This is a compile and USB-preflight
harness, not a provisioned network claim. Physical connection, credential
persistence, secure-bootstrap HTTP, MQTT, captive portals, enterprise Wi-Fi,
and roaming remain deferred.

## Safe result and secret lifetime

Results expose only `accepted`, `finalState`, `safeReasonCode`, `retryAllowed`,
and `secretsCleared`. Reason codes are `OK`, `MALFORMED_PAYLOAD`,
`UNSUPPORTED_VERSION`, `DEVICE_ID_MISMATCH`, `SESSION_MISMATCH`,
`SESSION_EXPIRED`, `REPLAY_REJECTED`, `FIELD_TOO_LONG`, `INVALID_TRANSITION`,
and `CANCELLED`.

The owned password and session-token buffers are overwritten by the shared
clear path on rejection, expiry, accepted handoff, cancellation, explicit
teardown, move-from cleanup, and destruction. Diagnostics contain only state
and reason codes. This is memory-lifetime hygiene, not hardware-backed secret
protection; any future BLE transport must also clear its own receive buffers.

## Development QR onboarding profile

`esp32-s3-dev-qr-onboarding-demo` adds the guarded
`ALGAGUARD_ENABLE_QR_ONBOARDING` path without enabling the legacy physical
handoff. The OLED renders a fixed 49-character `ag://q/` invitation encoded as
31 compact binary bytes. It carries only a version, bounded device reference,
128-bit ESP-IDF RNG nonce, monotonic issue/expiry ticks, capability version, and
CRC16. The nonce is RAM-only, rotates at expiry, and is zeroized after a signed
binding grant is accepted.

The QR path uses BLE provisioning request v2. It retains the canonical service
and characteristic UUIDs and adds exactly one backend-signed binding grant to
the existing request fields. Firmware verifies that grant against the pinned
development public key before it arms the one-shot Wi-Fi gate. One accepted
request may therefore authorize one connection; boot, BLE advertising, QR
generation, and Wi-Fi station startup cannot connect automatically.

After `GOT_IP`, the QR profile performs one trusted-HTTPS credential bootstrap.
It generates an EC P-256 key locally, submits only the public CSR, verifies the
returned device binding, and commits the certificate through the existing
development credential store. Failure destroys the staged key and leaves the
device unprovisioned. No private key, nonce, onboarding token, SSID, or password
is displayed or logged. Production, release, and DS profiles reject this
feature at compile time.

## Non-goals

This foundation does not implement pairing, bonding, Wi-Fi scanning or
connection, bootstrap HTTP, MQTT, TLS execution, OTA, OLED hardware behavior,
hardware flashing, eFuse operations, or cloud deployment. Physical BLE
validation remains deferred. Wi-Fi connection and persistence, secure-bootstrap
HTTP, pairing, and physical BLE validation remain explicitly deferred.
