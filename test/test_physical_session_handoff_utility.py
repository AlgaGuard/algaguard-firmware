import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).parents[1]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

handoff = load("physical_session_handoff", ROOT / "tools" / "physical_session_handoff.py")

class FakeTransport:
    def __init__(self, replies): self.replies = iter(replies); self.code = None
    def start(self, _device):
        return handoff.HandoffStart(bytearray(b"d" * 43), "AB2CDE", "2099-01-01T00:00:00Z", 1)
    def redeem(self, code): self.code = bytes(code); return next(self.replies)

def bundle():
    return handoff.RedeemedSession(bytearray(b"50000000-0000-4000-8000-000000000001"), bytearray(b"AG-000001"), bytearray(b"x" * 32), 10)

def run(replies, *, cancelled=lambda: False):
    out, sleeps, installs = [], [], []
    runner = handoff.SyntheticHandoffRunner(FakeTransport(replies), lambda value: installs.append(value) or "SESSION_ARMED", sleeps.append, out.append)
    return runner.run("AG-000001", cancelled=cancelled), out, sleeps, installs

def test_1_start_prints_only_user_code_and_expiry():
    result, out, _, _ = run([("EXPIRED", 0)])
    assert result == "EXPIRED" and out == ["USER_CODE AB2CDE", "EXPIRES_AT 2099-01-01T00:00:00Z"]

def test_2_pending_then_success_respects_interval():
    result, _, sleeps, installs = run([("PENDING", 1), ("REDEEMED", bundle())])
    assert result == "SESSION_ARMED" and sleeps == [1] and len(installs) == 1

def test_3_slow_down_expiry_rejection_and_cancel_clear():
    assert run([("SLOW_DOWN", 2), ("REJECTED", 0)])[0] == "REJECTED"
    assert run([("PENDING", 1)], cancelled=lambda: True)[0] == "CANCELLED"

def test_4_bundle_crosses_only_the_installer_seam():
    value = bundle(); result, _, _, installs = run([("REDEEMED", value)])
    assert result == "SESSION_ARMED" and installs == [value] and not value.session_token

def test_5_duplicate_success_is_terminal_without_second_install():
    result, _, _, installs = run([("REDEEMED", bundle()), ("REDEEMED", bundle())])
    assert result == "SESSION_ARMED" and len(installs) == 1

def test_6_live_transport_is_unavailable(): assert handoff.live_handoff_is_available() is False

def test_7_no_serial_or_network_implementation_exists():
    text = (ROOT / "tools" / "physical_session_handoff.py").read_text()
    assert "import requests" not in text and "import serial" not in text and "import socket" not in text

def test_8_safe_outputs_and_representations_do_not_contain_secret_values():
    value = bundle(); assert "x" * 32 not in repr(value)
    assert all("d" * 43 not in item for item in run([("EXPIRED", 0)])[1])
