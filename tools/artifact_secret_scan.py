"""Fail closed on accidentally packaged credential material without echoing it."""
from __future__ import annotations
import argparse
from pathlib import Path

MARKERS = {
    b"-----BEGIN PRIVATE KEY-----": "PRIVATE_KEY_PEM",
    b"-----BEGIN RSA PRIVATE KEY-----": "RSA_PRIVATE_KEY_PEM",
    b"sessionToken": "SESSION_TOKEN_MARKER",
    b"bootstrapToken": "BOOTSTRAP_TOKEN_MARKER",
    b"Bearer ": "BEARER_TOKEN_MARKER",
}

def scan(path: Path) -> list[str]:
    findings: list[str] = []
    for file in ([path] if path.is_file() else (item for item in path.rglob('*') if item.is_file())):
        if any(part in {'.git', '.pytest_cache', 'test'} for part in file.parts) or file.name == 'artifact_secret_scan.py':
            continue
        try:
            content = file.read_bytes()
        except OSError:
            continue
        for marker, classification in MARKERS.items():
            if marker in content:
                findings.append(f"SECRET_SCAN_FAIL file={file} classification={classification}")
    return findings

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    results = [finding for path in args.paths for finding in scan(path)]
    print('\n'.join(results) if results else 'SECRET_SCAN_PASS')
    raise SystemExit(1 if results else 0)
