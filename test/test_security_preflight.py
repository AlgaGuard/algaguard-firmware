import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def test_preflight_is_read_only_and_reports_plan():
    result = subprocess.run([sys.executable, str(ROOT / "tools" / "security_preflight.py"), str(ROOT / "test" / "fixtures" / "efuse-preflight-valid.json")], check=True, capture_output=True, text=True)
    report = json.loads(result.stdout)
    assert report["chip"] == "ESP32-S3"
    assert report["planMatches"] is True
    assert report["flashEncryption"] is False
