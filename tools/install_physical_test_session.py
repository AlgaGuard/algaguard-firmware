"""Install a development-only volatile physical-test session over an explicit COM port.

This utility never accepts a token as an argument, never writes it to disk, and is
intentionally unusable outside the physical-test firmware profile.
"""
from __future__ import annotations

import argparse
import getpass
import re
import struct
import sys
import time
import zlib

from physical_session_handoff import (
    HandoffStart,
    HandoffRunner,
    LiveHttpsHandoffTransport,
    RedeemedSession,
)

MAGIC = b"AGS1"
VERSION = 1
INSTALL = 1
CLEAR = 2
QUERY = 3
ARM_WIFI_TEST = 4
QUERY_OLED_ADDRESS = 5
MAX_PAYLOAD = 640
UUID = re.compile(r"^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$")
DEVICE = re.compile(r"^AG-[0-9]{6}$")
TOKEN = re.compile(r"^[A-Za-z0-9_-]{32,96}$")


def _frame(command: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload exceeds physical-test bound")
    header = MAGIC + bytes((VERSION, command)) + struct.pack(">H", len(payload))
    return header + payload + struct.pack(">I", zlib.crc32(header + payload) & 0xFFFFFFFF)


def _install_payload(session_id: str, device_id: str, token: str, lifetime_ticks: int) -> bytearray:
    if not UUID.fullmatch(session_id) or not DEVICE.fullmatch(device_id) or not TOKEN.fullmatch(token):
        raise ValueError("session fields do not meet the development control-contract shape")
    if lifetime_ticks <= 0 or lifetime_ticks > 30000:
        raise ValueError("session lifetime must be within the physical-test bound")
    encoded_session = session_id.encode("ascii")
    encoded_device = device_id.encode("ascii")
    encoded_token = token.encode("ascii")
    return bytearray(
        bytes((len(encoded_session), len(encoded_device)))
        + struct.pack(">H", len(encoded_token))
        + struct.pack(">Q", lifetime_ticks)
        + encoded_session
        + encoded_device
        + encoded_token
    )


def _send(port: str, frame: bytes) -> str:
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as error:
        raise RuntimeError("pyserial is required; install it separately in the operator environment") from error
    try:
        with serial.Serial(port=port, baudrate=115200, timeout=2, write_timeout=2) as connection:
            connection.write(frame)
            connection.flush()
            acknowledgements = {
                "SESSION_ARMED", "SESSION_INSTALL_REJECTED", "SESSION_CLEARED",
                "SESSION_INSTALLER_DISABLED", "WIFI_CONNECT_TEST_ARMED",
                "WIFI_CONNECT_TEST_REJECTED", "WIFI_CONNECT_TEST_ALREADY_ACTIVE",
                "WIFI_CONNECT_TEST_CONSUMED", "WIFI_CONNECT_TEST_EXPIRED",
                "WIFI_CONNECT_TEST_CLEARED", "WIFI_CONNECT_TEST_DISABLED",
            }
            deadline = time.monotonic() + 2
            reply = ""
            safe_state = ""
            expect_safe_state = len(frame) > 5 and frame[5] == QUERY
            while time.monotonic() < deadline:
                candidate = connection.readline(320).decode("ascii", errors="ignore").strip()
                if candidate in acknowledgements:
                    reply = candidate
                elif (candidate == "OLED_ADDRESS_NONE" or
                        re.fullmatch(r"OLED_ADDRESS_0x[0-7][0-9A-F]", candidate)):
                    reply = candidate
                elif re.fullmatch(
                    r"SAFE_SESSION_STATE gate=WIFI_CONNECT_TEST_(?:DISABLED|ARMED|CONSUMED|EXPIRED|CLEARED) "
                    r"activeSessionPresent=(?:true|false) handoffPresent=(?:true|false) "
                    r"wifiRuntimeReady=true connectAttemptActive=(?:true|false) "
                    r"credentialsPresent=(?:true|false) secretsCleared=(?:true|false) persistence=false",
                    candidate,
                ):
                    safe_state = candidate
                if reply and (not expect_safe_state or safe_state):
                    break
    except Exception as error:  # Never include any session field in an operator error.
        raise RuntimeError(f"serial control failed for the explicit port: {type(error).__name__}") from error
    if safe_state:
        return f"{reply}\n{safe_state}"
    return reply if reply else "SESSION_INSTALL_REJECTED"


def _install_redeemed_in_memory(port: str, bundle: RedeemedSession, send=_send) -> str:
    """Use the existing binary installer protocol without CLI/file serialization."""
    session_id = bytes(bundle.session_id).decode("ascii")
    device_id = bytes(bundle.device_id).decode("ascii")
    token = bytes(bundle.session_token).decode("ascii")
    payload = _install_payload(session_id, device_id, token, bundle.lifetime_ticks)
    try:
        return send(port, _frame(INSTALL, payload))
    finally:
        payload[:] = b"\0" * len(payload)
        bundle.clear()


class _SyntheticTransport:
    """A deterministic, non-network test fixture; it is not a live backend."""
    def __init__(self) -> None:
        self._polls = 0

    def start(self, device_id: str) -> HandoffStart:
        return HandoffStart(bytearray(b"d" * 43), "AB2CDE", "2099-01-01T00:00:00Z", 1)

    def redeem(self, _device_code: memoryview) -> tuple[str, int | RedeemedSession]:
        self._polls += 1
        if self._polls == 1:
            return "PENDING", 1
        return "REDEEMED", RedeemedSession(
            bytearray(b"50000000-0000-4000-8000-000000000001"),
            bytearray(b"AG-000001"),
            bytearray(b"x" * 32),
            100,
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Explicit COM port; automatic selection is prohibited.")
    parser.add_argument("--backend-url", help="Explicit verified-HTTPS handoff backend.")
    parser.add_argument("--command", choices=("install", "clear", "query", "query-oled-address", "arm-wifi-test", "handoff-install"), default="install")
    parser.add_argument("--session-id")
    parser.add_argument("--device-id")
    parser.add_argument("--expiry-tick", type=int)
    parser.add_argument("--lifetime-seconds", type=int)
    parser.add_argument("--live-development", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not args.port and not (args.command == "handoff-install" and args.dry_run):
        parser.error("--port is required for serial execution")
    if args.command == "handoff-install":
        if args.dry_run == args.live_development:
            parser.error("choose exactly one of --dry-run or --live-development")
        if args.dry_run:
            if args.backend_url != "synthetic://handoff":
                parser.error("synthetic dry-run requires --backend-url synthetic://handoff")
            transport = _SyntheticTransport()
            installer = lambda bundle: _install_redeemed_in_memory(
                "SYNTHETIC", bundle, lambda _, __: "SESSION_ARMED"
            )
            sleeper = lambda _: None
        else:
            if args.backend_url != "https://api.algaguard.bosilu.dev":
                parser.error("live execution requires the approved HTTPS backend")
            if not args.device_id:
                parser.error("live execution requires an explicit canonical --device-id")
            transport = LiveHttpsHandoffTransport(args.backend_url)
            installer = lambda bundle: _install_redeemed_in_memory(args.port, bundle)
            sleeper = time.sleep
        runner = HandoffRunner(transport, installer, sleeper, print)
        result = runner.run(args.device_id or "AG-000001")
        print(result)
        return 0 if result == "SESSION_ARMED" else 1
    if args.command == "install":
        if not (args.session_id and args.device_id and args.expiry_tick):
            parser.error("install requires --session-id, --device-id, and --expiry-tick")
        token = "synthetic-development-token-000000" if args.dry_run else getpass.getpass("Session token: ")
        payload = _install_payload(args.session_id, args.device_id, token, args.expiry_tick)
        frame = _frame(INSTALL, payload)
        for index in range(len(payload)):
            payload[index] = 0
        token = ""
    elif args.command == "arm-wifi-test":
        lifetime_seconds = args.lifetime_seconds if args.lifetime_seconds is not None else 300
        if lifetime_seconds <= 0 or lifetime_seconds > 600:
            parser.error("arm-wifi-test requires a lifetime from 1 to 600 seconds")
        frame = _frame(ARM_WIFI_TEST, struct.pack(">Q", lifetime_seconds * 100))
    else:
        frame = _frame(CLEAR if args.command == "clear" else
                       QUERY_OLED_ADDRESS if args.command == "query-oled-address" else QUERY)
    if args.dry_run:
        print("DRY_RUN_FRAME_READY")
        return 0
    print(_send(args.port, frame))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
