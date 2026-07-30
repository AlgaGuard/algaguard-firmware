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

def test_private_key_value_is_redacted(tmp_path):
    result = run(
        tmp_path,
        b'-----BEGIN PRIVATE KEY-----\n' + b'A' * 64 + b'\n-----END PRIVATE KEY-----',
    )
    assert result.returncode == 1 and 'PRIVATE_KEY_PEM' in result.stdout
    assert 'BEGIN PRIVATE KEY' not in result.stdout

def test_token_value_is_redacted(tmp_path):
    result = run(tmp_path, b'sessionToken=synthetic-token-value')
    assert result.returncode == 1 and 'SESSION_TOKEN_VALUE' in result.stdout

def test_contract_and_pem_format_markers_without_values_pass(tmp_path):
    result = run(
        tmp_path,
        b'sessionToken\x00bootstrapToken\x00Bearer \x00-----BEGIN PRIVATE KEY-----\x00',
    )
    assert result.returncode == 0 and 'SECRET_SCAN_PASS' in result.stdout
