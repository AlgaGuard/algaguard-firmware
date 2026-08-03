import subprocess
import sys
from pathlib import Path

SCRIPT = Path('tools/artifact_secret_scan.py')

def run(tmp_path, payload: bytes):
    target = tmp_path / 'artifact.bin'
    target.write_bytes(payload)
    return subprocess.run([sys.executable, str(SCRIPT), str(target)], text=True, capture_output=True)

def test_safe_warning_passes(tmp_path):
    result = run(tmp_path, b'INSECURE DEV KEY')
    assert result.returncode == 0 and 'SECRET_SCAN_PASS' in result.stdout

def test_private_key_marker_is_redacted(tmp_path):
    result = run(tmp_path, b'-----BEGIN PRIVATE KEY-----')
    assert result.returncode == 1 and 'PRIVATE_KEY_PEM' in result.stdout
    assert 'BEGIN PRIVATE KEY' not in result.stdout

def test_token_marker_is_redacted(tmp_path):
    result = run(tmp_path, b'sessionToken=synthetic')
    assert result.returncode == 1 and 'SESSION_TOKEN_MARKER' in result.stdout
