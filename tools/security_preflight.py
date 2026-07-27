#!/usr/bin/env python3
"""Read-only parser for a manually captured ESP-IDF/eFuse inventory."""
from __future__ import annotations
import argparse
import json
from pathlib import Path

REQUIRED = {"KEY0": "HMAC_DOWN_DS", "KEY1": "HMAC_UP", "KEY2": "XTS_AES_128_KEY", "KEY3": "SECURE_BOOT_DIGEST"}

def assess(report: dict) -> dict:
    keys = report.get("keyPurposes", {})
    return {"chip": report.get("chip", "unknown"), "secureBoot": bool(report.get("secureBoot")), "flashEncryption": bool(report.get("flashEncryption")), "downloadDisabled": bool(report.get("downloadDisabled")), "jtagDisabled": bool(report.get("jtagDisabled")), "keyPurposes": keys, "planMatches": all(keys.get(block) == purpose for block, purpose in REQUIRED.items())}

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="JSON created from a manually captured read-only inventory")
    result = assess(json.loads(parser.parse_args().report.read_text(encoding="utf-8")))
    print(json.dumps(result, sort_keys=True))
    return 0

if __name__ == "__main__": raise SystemExit(main())
