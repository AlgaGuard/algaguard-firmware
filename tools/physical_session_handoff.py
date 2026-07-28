"""Synthetic-only client flow for the development physical session handoff.

This module intentionally has no HTTP or serial implementation.  A future,
separately approved physical validation phase may provide authenticated
transports; this boundary keeps all secret-bearing values in memory meanwhile.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Protocol


@dataclass(repr=False)
class RedeemedSession:
    session_id: bytearray
    device_id: bytearray
    session_token: bytearray
    expiry_tick: int

    def clear(self) -> None:
        for value in (self.session_id, self.device_id, self.session_token):
            value[:] = b"\0" * len(value)
        self.session_id.clear()
        self.device_id.clear()
        self.session_token.clear()


@dataclass(repr=False)
class HandoffStart:
    device_code: bytearray
    user_code: str
    expires_at: str
    poll_interval_seconds: int

    def clear(self) -> None:
        self.device_code[:] = b"\0" * len(self.device_code)
        self.device_code.clear()


class SyntheticHandoffTransport(Protocol):
    def start(self, device_id: str) -> HandoffStart: ...
    def redeem(self, device_code: memoryview) -> tuple[str, int | RedeemedSession]: ...


Installer = Callable[[RedeemedSession], str]
Output = Callable[[str], None]


@dataclass
class SyntheticHandoffRunner:
    transport: SyntheticHandoffTransport
    installer: Installer
    sleep: Callable[[int], None]
    output: Output
    _installed: bool = field(default=False, init=False)

    def run(self, device_id: str, *, cancelled: Callable[[], bool] | None = None) -> str:
        start = self.transport.start(device_id)
        try:
            # userCode and expiry are the only values intentionally shown.
            self.output(f"USER_CODE {start.user_code}")
            self.output(f"EXPIRES_AT {start.expires_at}")
            interval = start.poll_interval_seconds
            while not (cancelled and cancelled()):
                status, value = self.transport.redeem(memoryview(start.device_code))
                if status == "PENDING":
                    self.sleep(interval)
                    continue
                if status == "SLOW_DOWN":
                    interval = int(value)
                    self.sleep(interval)
                    continue
                if status in {"EXPIRED", "REJECTED"}:
                    return status
                if status != "REDEEMED" or not isinstance(value, RedeemedSession) or self._installed:
                    return "REJECTED"
                self._installed = True
                try:
                    return self.installer(value)
                finally:
                    value.clear()
            return "CANCELLED"
        finally:
            start.clear()


def live_handoff_is_available() -> bool:
    """Live HTTP and serial execution are intentionally not implemented."""
    return False
