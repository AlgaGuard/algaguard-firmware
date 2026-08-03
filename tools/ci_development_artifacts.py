"""Fail-closed cross-job reconstruction and staged-set verification."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

from artifact_security import (
    ARTIFACT_PROFILES,
    ARTIFACT_SET_NAME,
    ArtifactSecurityError,
    scan_file,
    validate_profile,
)


TRANSFER_PREFIX = "scanned-development-firmware-"


def _files_below(root: Path) -> set[Path]:
    if not root.is_dir():
        raise ArtifactSecurityError("TRANSFER_ROOT_MISSING")
    return {path.relative_to(root) for path in root.rglob("*") if path.is_file()}


def reconstruct(transfer_root: Path, repo_root: Path) -> None:
    expected = {
        Path(f"{TRANSFER_PREFIX}{profile.environment}") / "firmware.bin"
        for profile in ARTIFACT_PROFILES
    }
    if _files_below(transfer_root) != expected:
        raise ArtifactSecurityError("TRANSFER_ALLOWLIST_MISMATCH")

    for profile in ARTIFACT_PROFILES:
        source = (
            transfer_root
            / f"{TRANSFER_PREFIX}{profile.environment}"
            / "firmware.bin"
        )
        result = scan_file(source)
        if not result.passed:
            raise ArtifactSecurityError(
                f"TRANSFER_SECRET_REJECTED environment={profile.environment}"
            )
        validate_profile(profile, source.read_bytes())
        destination = repo_root / profile.source_relative_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_staged(staging_directory: Path) -> None:
    expected_files = {
        Path("manifest.json"),
        Path("scan-report.json"),
        *(profile.staged_relative_path for profile in ARTIFACT_PROFILES),
    }
    if _files_below(staging_directory) != expected_files:
        raise ArtifactSecurityError("FINAL_ALLOWLIST_MISMATCH")

    manifest = json.loads(
        (staging_directory / "manifest.json").read_text(encoding="utf-8")
    )
    report = json.loads(
        (staging_directory / "scan-report.json").read_text(encoding="utf-8")
    )
    if (
        manifest.get("artifactSetName") != ARTIFACT_SET_NAME
        or manifest.get("artifactCount") != 3
        or manifest.get("signed") is not False
        or manifest.get("productionEligible") is not False
        or report.get("artifactCount") != 3
        or report.get("overallStatus") != "PASS"
    ):
        raise ArtifactSecurityError("FINAL_METADATA_REJECTED")

    expected_environments = [profile.environment for profile in ARTIFACT_PROFILES]
    if [item.get("environment") for item in manifest.get("artifacts", [])] != expected_environments:
        raise ArtifactSecurityError("MANIFEST_ORDER_REJECTED")
    if [item.get("environment") for item in report.get("artifacts", [])] != expected_environments:
        raise ArtifactSecurityError("SCAN_REPORT_ORDER_REJECTED")

    for profile, entry, report_entry in zip(
        ARTIFACT_PROFILES,
        manifest["artifacts"],
        report["artifacts"],
        strict=True,
    ):
        artifact = staging_directory / profile.staged_relative_path
        if (
            not artifact.is_file()
            or artifact.stat().st_size <= 0
            or entry.get("relativePath") != profile.staged_relative_path.as_posix()
            or entry.get("securityProfile") != profile.security_profile
            or entry.get("warningRequired") != profile.warning_required
            or entry.get("sha256") != _sha256(artifact)
            or entry.get("sizeBytes") != artifact.stat().st_size
            or entry.get("signed") is not False
            or entry.get("productionEligible") is not False
            or report_entry.get("relativePath")
            != profile.staged_relative_path.as_posix()
            or report_entry.get("result") != "PASS"
            or report_entry.get("matchedRuleIds") != []
            or report_entry.get("profileChecks", {}).get("passed") is not True
        ):
            raise ArtifactSecurityError(
                f"FINAL_ARTIFACT_REJECTED environment={profile.environment}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    reconstruct_parser = subparsers.add_parser("reconstruct")
    reconstruct_parser.add_argument("--transfer-root", type=Path, required=True)
    reconstruct_parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    verify_parser = subparsers.add_parser("verify-staged")
    verify_parser.add_argument("--staging-directory", type=Path, required=True)
    arguments = parser.parse_args()

    try:
        if arguments.command == "reconstruct":
            reconstruct(arguments.transfer_root.resolve(), arguments.repo_root.resolve())
            print("TRANSFER_RECONSTRUCTION_PASS")
        else:
            verify_staged(arguments.staging_directory.resolve())
            print("FINAL_ARTIFACT_SET_PASS")
    except (ArtifactSecurityError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"CI_ARTIFACT_REJECTED reason={error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
