import hashlib
import json
from pathlib import Path

import pytest

from tools.artifact_security import (
    ARTIFACT_PROFILES,
    ARTIFACT_SET_NAME,
    ArtifactSecurityError,
    scan_bytes,
    stage_development_artifacts,
    validate_profile,
)


def _write_source_set(root: Path, payloads: tuple[bytes, bytes, bytes]) -> None:
    for profile, payload in zip(ARTIFACT_PROFILES, payloads, strict=True):
        path = root / profile.source_relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)


def test_safe_synthetic_firmware_passes():
    result = scan_bytes(b"ordinary firmware\0DEV_SOFTWARE_KEY\0INSECURE DEV KEY")
    assert result.passed


def test_private_key_pem_marker_is_rejected():
    result = scan_bytes(b"prefix-----BEGIN " + b"PRIVATE KEY-----suffix")
    assert result.findings[0].rule_id == "PRIVATE_KEY_PEM"
    assert result.findings[0].byte_offset == 6


def test_jwt_like_token_is_rejected():
    token = (
        b"eyJhbGciOiJSUzI1NiJ9"
        + b".eyJzdWIiOiJkZXZpY2UifQ"
        + b".signature123"
    )
    result = scan_bytes(b"prefix " + token)
    assert {finding.rule_id for finding in result.findings} == {"JWT_BEARER_TOKEN"}


def test_aws_access_key_pattern_is_rejected():
    result = scan_bytes(b"prefix " + b"AKIA" + b"1234567890ABCDEF suffix")
    assert {finding.rule_id for finding in result.findings} == {"AWS_ACCESS_KEY_ID"}


def test_default_profile_rejects_insecure_warning_presence():
    with pytest.raises(ArtifactSecurityError, match="PROFILE_CHECK_REJECTED"):
        validate_profile(
            ARTIFACT_PROFILES[0],
            b"DEV_SOFTWARE_KEY\0SOFTWARE_PRIVATE_KEY_IN_USE",
        )


def test_insecure_development_profile_requires_warning_presence():
    with pytest.raises(ArtifactSecurityError, match="PROFILE_CHECK_REJECTED"):
        validate_profile(ARTIFACT_PROFILES[1], b"DEV_SOFTWARE_KEY")


def test_staging_failure_leaves_no_partial_final_artifact_set(tmp_path):
    _write_source_set(
        tmp_path,
        (
            b"DEV_SOFTWARE_KEY\0SOFTWARE_PRIVATE_KEY_IN_USE",
            b"DEV_SOFTWARE_KEY\0SOFTWARE_PRIVATE_KEY_IN_USE",
            b"DEV_SOFTWARE_KEY\0SOFTWARE_PRIVATE_KEY_IN_USE",
        ),
    )
    output_root = tmp_path / "output"
    with pytest.raises(ArtifactSecurityError):
        stage_development_artifacts(
            tmp_path,
            output_root=output_root,
            git_head="a" * 40,
            worktree_dirty=True,
        )
    assert not (output_root / ARTIFACT_SET_NAME).exists()
    assert not list(output_root.glob(".*.tmp-*")) if output_root.exists() else True


def test_successful_staging_is_complete_hashed_and_deterministic(tmp_path):
    payloads = (
        b"default-development",
        b"DEV_SOFTWARE_KEY\0SOFTWARE_PRIVATE_KEY_IN_USE\0identity",
        b"DEV_SOFTWARE_KEY\0SOFTWARE_PRIVATE_KEY_IN_USE\0self-test",
    )
    _write_source_set(tmp_path, payloads)
    output_root = tmp_path / "output"
    final = stage_development_artifacts(
        tmp_path,
        output_root=output_root,
        git_head="b" * 40,
        worktree_dirty=True,
    )
    first_manifest = (final / "manifest.json").read_bytes()
    first_report = (final / "scan-report.json").read_bytes()
    final = stage_development_artifacts(
        tmp_path,
        output_root=output_root,
        git_head="b" * 40,
        worktree_dirty=True,
    )

    files = sorted(
        path.relative_to(final).as_posix() for path in final.rglob("*") if path.is_file()
    )
    assert files == sorted(
        [
            "manifest.json",
            "scan-report.json",
            *(f"{profile.environment}/firmware.bin" for profile in ARTIFACT_PROFILES),
        ]
    )
    manifest = json.loads((final / "manifest.json").read_text(encoding="utf-8"))
    assert [item["environment"] for item in manifest["artifacts"]] == [
        profile.environment for profile in ARTIFACT_PROFILES
    ]
    assert [
        item["sha256"] for item in manifest["artifacts"]
    ] == [hashlib.sha256(payload).hexdigest() for payload in payloads]
    assert first_manifest == (final / "manifest.json").read_bytes()
    assert first_report == (final / "scan-report.json").read_bytes()
