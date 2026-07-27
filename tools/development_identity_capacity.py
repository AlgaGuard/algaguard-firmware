"""Deterministic development-identity capacity report; contains no credentials."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVS_BYTES = 0x10000
RSA3072_PKCS1_DER_MAX = 1787  # PSA_EXPORT_KEY_OUTPUT_SIZE(..., 3072) in pinned IDF 6.0.1
CERT_MAX = 4096
CHAIN_MAX = 12288
METADATA = 512
OVERHEAD_PERCENT = 25

one_slot = RSA3072_PKCS1_DER_MAX + CERT_MAX + CHAIN_MAX + METADATA
two_slot_raw = one_slot * 2
required = (two_slot_raw * (100 + OVERHEAD_PERCENT) + 99) // 100
report = {"nvsBytes": NVS_BYTES, "rsa3072Pkcs1DerMax": RSA3072_PKCS1_DER_MAX,
          "certificateMax": CERT_MAX, "caChainMax": CHAIN_MAX, "metadataPerSlot": METADATA,
          "nvsOverheadPercent": OVERHEAD_PERCENT, "oneSlotRaw": one_slot,
          "twoSlotRaw": two_slot_raw, "requiredWithOverhead": required,
          "headroom": NVS_BYTES - required, "headroomPercent": round((NVS_BYTES - required) * 100 / NVS_BYTES, 2),
          "otaSlotsUnchanged": [0x600000, 0x600000]}
print(json.dumps(report, indent=2))
raise SystemExit(0 if report["headroom"] > 0 else 1)
