"""One-time development physical-session handoff with memory-only secrets."""
from __future__ import annotations

import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Callable, Protocol


DEVICE = re.compile(r"^AG-[0-9]{6}$")
DEVICE_CODE = re.compile(r"^[A-Za-z0-9_-]{43,128}$")
USER_CODE = re.compile(r"^[A-HJ-NP-Z2-9]{6,16}$")
UUID = re.compile(r"^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$")
TOKEN = re.compile(r"^[A-Za-z0-9_-]{32,96}$")
APPROVED_LIVE_HOST = "api.algaguard.bosilu.dev"
PHYSICAL_TICKS_PER_SECOND = 100
MAX_SESSION_LIFETIME_SECONDS = 5 * 60
MAX_RESPONSE_BYTES = 4096


@dataclass(repr=False)
class RedeemedSession:
    session_id: bytearray
    device_id: bytearray
    session_token: bytearray
    lifetime_ticks: int

    def clear(self) -> None:
        for value in (self.session_id, self.device_id, self.session_token):
            value[:] = b"\0" * len(value)
            value.clear()
        self.lifetime_ticks = 0


@dataclass(repr=False)
class HandoffStart:
    device_code: bytearray
    user_code: str
    expires_at: str
    poll_interval_seconds: int

    def clear(self) -> None:
        self.device_code[:] = b"\0" * len(self.device_code)
        self.device_code.clear()
        self.user_code = ""
        self.expires_at = ""
        self.poll_interval_seconds = 0


class HandoffTransport(Protocol):
    def start(self, device_id: str) -> HandoffStart: ...
    def redeem(self, device_code: memoryview) -> tuple[str, int | RedeemedSession]: ...


class _RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        raise urllib.error.HTTPError(
            request.full_url, code, "redirect rejected", headers, file_pointer
        )


class LiveHttpsHandoffTransport:
    """Verified-HTTPS transport for the single approved development endpoint."""

    def __init__(self, backend_url: str, *, timeout_seconds: int = 10) -> None:
        parsed = urllib.parse.urlsplit(backend_url)
        if (
            parsed.scheme != "https"
            or parsed.hostname != APPROVED_LIVE_HOST
            or parsed.port not in (None, 443)
            or parsed.username
            or parsed.password
            or parsed.query
            or parsed.fragment
            or parsed.path not in ("", "/")
        ):
            raise ValueError("live handoff requires the approved HTTPS backend")
        self._base_url = f"https://{APPROVED_LIVE_HOST}"
        self._timeout_seconds = timeout_seconds
        self._opener = urllib.request.build_opener(
            _RejectRedirects(),
            urllib.request.HTTPSHandler(context=ssl.create_default_context()),
        )
        self._poll_interval_seconds = 5

    def _post(self, path: str, body: bytearray, expected_status: int) -> dict[str, object]:
        try:
            request = urllib.request.Request(
                self._base_url + path,
                data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with self._opener.open(request, timeout=self._timeout_seconds) as response:
                if response.status != expected_status:
                    raise RuntimeError("HANDOFF_SERVICE_UNAVAILABLE")
                raw = bytearray(response.read(MAX_RESPONSE_BYTES + 1))
                if len(raw) > MAX_RESPONSE_BYTES:
                    raise RuntimeError("HANDOFF_RESPONSE_INVALID")
                try:
                    value = json.loads(raw.decode("utf-8"))
                finally:
                    raw[:] = b"\0" * len(raw)
                    raw.clear()
            if not isinstance(value, dict):
                raise RuntimeError("HANDOFF_RESPONSE_INVALID")
            return value
        except urllib.error.HTTPError as error:
            if error.code == 410:
                raise RuntimeError("HANDOFF_EXPIRED") from None
            if error.code == 429:
                raise RuntimeError("HANDOFF_SLOW_DOWN") from None
            raise RuntimeError("HANDOFF_SERVICE_UNAVAILABLE") from None
        except (urllib.error.URLError, TimeoutError, ssl.SSLError):
            raise RuntimeError("HANDOFF_SERVICE_UNAVAILABLE") from None
        finally:
            body[:] = b"\0" * len(body)
            body.clear()

    def start(self, device_id: str) -> HandoffStart:
        if not DEVICE.fullmatch(device_id):
            raise ValueError("invalid canonical device binding")
        body = bytearray(
            json.dumps(
                {"protocolVersion": 1, "deviceId": device_id},
                separators=(",", ":"),
            ).encode("ascii")
        )
        value = self._post(
            "/v1/development/physical-session-handoffs/start", body, 201
        )
        if set(value) != {
            "handoffId",
            "deviceCode",
            "userCode",
            "expiresAt",
            "pollIntervalSeconds",
        }:
            raise RuntimeError("HANDOFF_RESPONSE_INVALID")
        device_code = value.get("deviceCode")
        user_code = value.get("userCode")
        expires_at = value.get("expiresAt")
        interval = value.get("pollIntervalSeconds")
        if (
            not isinstance(device_code, str)
            or not DEVICE_CODE.fullmatch(device_code)
            or not isinstance(user_code, str)
            or not USER_CODE.fullmatch(user_code)
            or not isinstance(expires_at, str)
            or _remaining_seconds(expires_at) <= 0
            or not isinstance(interval, int)
            or interval < 1
            or interval > 30
        ):
            raise RuntimeError("HANDOFF_RESPONSE_INVALID")
        self._poll_interval_seconds = interval
        return HandoffStart(
            bytearray(device_code.encode("ascii")),
            user_code,
            expires_at,
            interval,
        )

    def redeem(self, device_code: memoryview) -> tuple[str, int | RedeemedSession]:
        code = bytes(device_code)
        if not DEVICE_CODE.fullmatch(code.decode("ascii", errors="ignore")):
            return "REJECTED", 0
        body = bytearray(b'{"protocolVersion":1,"deviceCode":"')
        body.extend(code)
        body.extend(b'"}')
        try:
            value = self._post(
                "/v1/development/physical-session-handoffs/redeem", body, 200
            )
        except RuntimeError as error:
            if str(error) == "HANDOFF_EXPIRED":
                return "EXPIRED", 0
            if str(error) == "HANDOFF_SLOW_DOWN":
                self._poll_interval_seconds = min(30, self._poll_interval_seconds + 5)
                return "SLOW_DOWN", self._poll_interval_seconds
            raise
        status = value.get("status")
        if status in {"PENDING", "SLOW_DOWN"}:
            interval = value.get("pollIntervalSeconds", self._poll_interval_seconds)
            if not isinstance(interval, int) or interval < 1 or interval > 30:
                return "REJECTED", 0
            self._poll_interval_seconds = interval
            return str(status), interval
        if status == "EXPIRED":
            return "EXPIRED", 0
        if status != "REDEEMED" or set(value) != {
            "status",
            "sessionId",
            "deviceId",
            "sessionToken",
            "expiresAt",
        }:
            return "REJECTED", 0
        session_id = value.get("sessionId")
        device_id = value.get("deviceId")
        token = value.get("sessionToken")
        expires_at = value.get("expiresAt")
        remaining = _remaining_seconds(expires_at) if isinstance(expires_at, str) else 0
        if (
            not isinstance(session_id, str)
            or not UUID.fullmatch(session_id)
            or not isinstance(device_id, str)
            or not DEVICE.fullmatch(device_id)
            or not isinstance(token, str)
            or not TOKEN.fullmatch(token)
            or remaining <= 0
        ):
            return "REJECTED", 0
        lifetime = min(remaining, MAX_SESSION_LIFETIME_SECONDS)
        return "REDEEMED", RedeemedSession(
            bytearray(session_id.encode("ascii")),
            bytearray(device_id.encode("ascii")),
            bytearray(token.encode("ascii")),
            lifetime * PHYSICAL_TICKS_PER_SECOND,
        )


def _remaining_seconds(value: str) -> int:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return 0
    if parsed.tzinfo is None:
        return 0
    return max(0, int((parsed.astimezone(timezone.utc) - datetime.now(timezone.utc)).total_seconds()))


Installer = Callable[[RedeemedSession], str]
Output = Callable[[str], None]


@dataclass
class HandoffRunner:
    transport: HandoffTransport
    installer: Installer
    sleep: Callable[[int], None]
    output: Output
    _installed: bool = field(default=False, init=False)

    def run(self, device_id: str, *, cancelled: Callable[[], bool] | None = None) -> str:
        start = self.transport.start(device_id)
        try:
            self.output(f"USER_CODE {start.user_code}")
            self.output(f"EXPIRES_AT {start.expires_at}")
            interval = start.poll_interval_seconds
            while _remaining_seconds(start.expires_at) > 0 and not (
                cancelled and cancelled()
            ):
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
                if (
                    status != "REDEEMED"
                    or not isinstance(value, RedeemedSession)
                    or self._installed
                ):
                    return "REJECTED"
                self._installed = True
                try:
                    return self.installer(value)
                finally:
                    value.clear()
            return "CANCELLED" if cancelled and cancelled() else "EXPIRED"
        finally:
            start.clear()


# Compatibility name retained for existing synthetic unit tests.
SyntheticHandoffRunner = HandoffRunner


def live_handoff_is_available() -> bool:
    return True
