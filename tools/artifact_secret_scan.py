"""Fail closed on accidentally packaged credential material without echoing it."""
from __future__ import annotations
import argparse
import re
from pathlib import Path

PATTERNS = {
    re.compile(rb"-----BEGIN PRIVATE KEY-----\s+[A-Za-z0-9+/=\r\n]{32,}"):
        "PRIVATE_KEY_PEM",
    re.compile(rb"-----BEGIN RSA PRIVATE KEY-----\s+[A-Za-z0-9+/=\r\n]{32,}"):
        "RSA_PRIVATE_KEY_PEM",
    re.compile(rb"sessionToken[\"']?\s*[:=]\s*[\"']?[A-Za-z0-9_-]{16,}"):
        "SESSION_TOKEN_VALUE",
    re.compile(rb"bootstrapToken[\"']?\s*[:=]\s*[\"']?[A-Za-z0-9_-]{16,}"):
        "BOOTSTRAP_TOKEN_VALUE",
    re.compile(rb"Bearer [A-Za-z0-9._~+/=-]{16,}"):
        "BEARER_TOKEN_VALUE",
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
        for pattern, classification in PATTERNS.items():
            if pattern.search(content):
                findings.append(f"SECRET_SCAN_FAIL file={file} classification={classification}")
    return findings

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    results = [finding for path in args.paths for finding in scan(path)]
    print('\n'.join(results) if results else 'SECRET_SCAN_PASS')
    raise SystemExit(1 if results else 0)
