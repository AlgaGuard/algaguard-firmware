"""Fail-closed scanning and deterministic staging for development firmware."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCHEMA_VERSION = 1
SCANNER_VERSION = "1.0.0"
ARTIFACT_SET_NAME = "firmware-development-unsigned"
STAGING_RELATIVE_PATH = Path(".pio/artifacts") / ARTIFACT_SET_NAME
FIRMWARE_NAME = "firmware.bin"


@dataclass(frozen=True)
class ArtifactProfile:
    environment: str
    security_profile: str
    warning_required: bool
    dev_profile_required: bool

    @property
    def source_relative_path(self) -> Path:
        return Path(".pio/build") / self.environment / FIRMWARE_NAME

    @property
    def staged_relative_path(self) -> Path:
        return Path(self.environment) / FIRMWARE_NAME


ARTIFACT_PROFILES = (
    ArtifactProfile(
        "esp32-s3-devkitc-1",
        "DEFAULT_DEVELOPMENT",
        warning_required=False,
        dev_profile_required=False,
    ),
    ArtifactProfile(
        "esp32-s3-dev-software-key",
        "INSECURE_DEVELOPMENT_IDENTITY",
        warning_required=True,
        dev_profile_required=True,
    ),
    ArtifactProfile(
        "esp32-s3-dev-software-identity-self-test",
        "INSECURE_DEVELOPMENT_SELF_TEST",
        warning_required=True,
        dev_profile_required=True,
    ),
)

PRIVATE_KEY_MARKERS = (
    (b"-----BEGIN " + b"PRIVATE KEY-----", "PRIVATE_KEY_PEM"),
    (b"-----BEGIN RSA " + b"PRIVATE KEY-----", "RSA_PRIVATE_KEY_PEM"),
    (b"-----BEGIN EC " + b"PRIVATE KEY-----", "EC_PRIVATE_KEY_PEM"),
    (b"-----BEGIN OPENSSH " + b"PRIVATE KEY-----", "OPENSSH_PRIVATE_KEY_PEM"),
)
GENERATED_FIXTURE_MARKERS = (
    (b"ALGAGUARD_GENERATED_" + b"PRIVATE_KEY_FIXTURE", "GENERATED_KEY_FIXTURE"),
    (b"NVS_" + b"PRIVATE_KEY_BLOB_EXPORT", "NVS_PRIVATE_KEY_EXPORT"),
)
JWT_PATTERN = re.compile(
    rb"(?<![A-Za-z0-9_-])eyJ[A-Za-z0-9_-]{8,}\."
    rb"[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}(?![A-Za-z0-9_-])"
)
AWS_ACCESS_KEY_PATTERN = re.compile(rb"(?<![A-Z0-9])(?:AKIA|ASIA)[A-Z0-9]{16}(?![A-Z0-9])")
PASSWORD_ASSIGNMENT_PATTERN = re.compile(
    rb"(?i)(?<![A-Za-z0-9_])(?:password|passwd|pwd)\s*[:=]\s*"
    rb"[\"']?([A-Za-z0-9!@#$%^&*()_+\-./]{8,})"
)
PRINTABLE_STRING_PATTERN = re.compile(rb"[\x20-\x7e]{4,}")
SAFE_PASSWORD_VALUES = {
    b"placeholder",
    b"changeme",
    b"example",
    b"synthetic",
    b"not-a-secret",
    b"redacted",
}
SAFE_CLASSIFICATION_STRINGS = (
    b"DEV_SOFTWARE_KEY",
    b"SOFTWARE_PRIVATE_KEY_IN_USE",
    b"INSECURE DEV KEY",
)
DEV_PROFILE_MARKER = b"DEV_SOFTWARE_KEY"
INSECURE_WARNING_MARKER = b"SOFTWARE_PRIVATE_KEY_IN_USE"


@dataclass(frozen=True)
class ScanFinding:
    rule_id: str
    byte_offset: int


@dataclass(frozen=True)
class ScanResult:
    findings: tuple[ScanFinding, ...]
    safe_finding_count: int

    @property
    def passed(self) -> bool:
        return not self.findings


class ArtifactSecurityError(RuntimeError):
    """A safe, non-secret-bearing artifact validation failure."""


def scan_bytes(content: bytes) -> ScanResult:
    """Inspect raw bytes and embedded printable strings without executing content."""
    findings: list[ScanFinding] = []

    for marker, rule_id in (*PRIVATE_KEY_MARKERS, *GENERATED_FIXTURE_MARKERS):
        offset = content.find(marker)
        if offset >= 0:
            findings.append(ScanFinding(rule_id, offset))

    for printable in PRINTABLE_STRING_PATTERN.finditer(content):
        decoded = printable.group(0)
        for rule_id, pattern in (
            ("JWT_BEARER_TOKEN", JWT_PATTERN),
            ("AWS_ACCESS_KEY_ID", AWS_ACCESS_KEY_PATTERN),
        ):
            findings.extend(
                ScanFinding(rule_id, printable.start() + match.start())
                for match in pattern.finditer(decoded)
            )

        for match in PASSWORD_ASSIGNMENT_PATTERN.finditer(decoded):
            candidate = match.group(1).lower()
            if candidate not in SAFE_PASSWORD_VALUES:
                findings.append(
                    ScanFinding(
                        "PASSWORD_ASSIGNMENT", printable.start() + match.start()
                    )
                )

    findings.sort(key=lambda finding: (finding.byte_offset, finding.rule_id))
    safe_count = sum(content.count(marker) for marker in SAFE_CLASSIFICATION_STRINGS)
    return ScanResult(tuple(findings), safe_count)


def scan_file(path: Path) -> ScanResult:
    if not path.is_file():
        raise ArtifactSecurityError(f"ARTIFACT_NOT_FILE path={path.name}")
    content = path.read_bytes()
    if not content:
        raise ArtifactSecurityError(f"ARTIFACT_EMPTY path={path.name}")
    if "nvs" in path.name.lower() and "key" in path.name.lower():
        return ScanResult((ScanFinding("NVS_PRIVATE_KEY_EXPORT", 0),), 0)
    return scan_bytes(content)


def profile_checks(profile: ArtifactProfile, content: bytes) -> dict[str, bool]:
    warning_present = INSECURE_WARNING_MARKER in content
    dev_profile_present = DEV_PROFILE_MARKER in content
    warning_matches = warning_present == profile.warning_required
    dev_profile_matches = dev_profile_present == profile.dev_profile_required
    return {
        "devSoftwareKeyExpected": profile.dev_profile_required,
        "devSoftwareKeyPresent": dev_profile_present,
        "devSoftwareKeyValid": dev_profile_matches,
        "passed": warning_matches and dev_profile_matches,
        "warningExpected": profile.warning_required,
        "warningPresent": warning_present,
        "warningValid": warning_matches,
    }


def validate_profile(profile: ArtifactProfile, content: bytes) -> dict[str, bool]:
    checks = profile_checks(profile, content)
    if not checks["passed"]:
        raise ArtifactSecurityError(
            f"PROFILE_CHECK_REJECTED environment={profile.environment}"
        )
    return checks


def _json_bytes(payload: object) -> bytes:
    return (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as artifact:
        for block in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _git_metadata(repo_root: Path) -> tuple[str, bool]:
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return head, bool(status)


def _expected_staged_files() -> set[Path]:
    return {
        Path("manifest.json"),
        Path("scan-report.json"),
        *(profile.staged_relative_path for profile in ARTIFACT_PROFILES),
    }


def _verify_staged_layout(staging_directory: Path) -> None:
    actual = {
        path.relative_to(staging_directory)
        for path in staging_directory.rglob("*")
        if path.is_file()
    }
    if actual != _expected_staged_files():
        raise ArtifactSecurityError("STAGED_ALLOWLIST_MISMATCH")


def _replace_directory_transactionally(temporary: Path, final: Path) -> None:
    """Publish with rename operations and restore the prior valid set on failure."""
    backup: Path | None = None
    if final.exists():
        backup = final.parent / f".{final.name}.previous-{os.getpid()}"
        if backup.exists():
            shutil.rmtree(backup)
        os.replace(final, backup)

    try:
        os.replace(temporary, final)
    except BaseException:
        if backup is not None and backup.exists() and not final.exists():
            os.replace(backup, final)
        raise
    else:
        if backup is not None:
            shutil.rmtree(backup)


def stage_development_artifacts(
    repo_root: Path,
    *,
    output_root: Path | None = None,
    git_head: str | None = None,
    worktree_dirty: bool | None = None,
) -> Path:
    repo_root = repo_root.resolve()
    output_root = (
        output_root.resolve()
        if output_root is not None
        else (repo_root / STAGING_RELATIVE_PATH.parent).resolve()
    )
    final_directory = output_root / ARTIFACT_SET_NAME

    if git_head is None or worktree_dirty is None:
        discovered_head, discovered_dirty = _git_metadata(repo_root)
        git_head = discovered_head if git_head is None else git_head
        worktree_dirty = discovered_dirty if worktree_dirty is None else worktree_dirty

    validated: list[dict[str, object]] = []
    for profile in ARTIFACT_PROFILES:
        source = repo_root / profile.source_relative_path
        result = scan_file(source)
        if not result.passed:
            first = result.findings[0]
            raise ArtifactSecurityError(
                f"SECRET_REJECTED environment={profile.environment} "
                f"rule={first.rule_id} offset={first.byte_offset}"
            )
        content = source.read_bytes()
        checks = validate_profile(profile, content)
        validated.append(
            {
                "profile": profile,
                "source": source,
                "sha256": _sha256(source),
                "sizeBytes": source.stat().st_size,
                "scan": result,
                "profileChecks": checks,
            }
        )

    manifest = {
        "artifactCount": len(validated),
        "artifactSetName": ARTIFACT_SET_NAME,
        "artifacts": [
            {
                "environment": item["profile"].environment,
                "productionEligible": False,
                "relativePath": item["profile"].staged_relative_path.as_posix(),
                "securityProfile": item["profile"].security_profile,
                "sha256": item["sha256"],
                "signed": False,
                "sizeBytes": item["sizeBytes"],
                "warningRequired": item["profile"].warning_required,
            }
            for item in validated
        ],
        "gitHead": git_head,
        "productionEligible": False,
        "schemaVersion": SCHEMA_VERSION,
        "signed": False,
        "worktreeDirty": worktree_dirty,
    }
    scan_report = {
        "artifactCount": len(validated),
        "artifacts": [
            {
                "environment": item["profile"].environment,
                "matchedRuleIds": [],
                "profileChecks": item["profileChecks"],
                "relativePath": item["profile"].staged_relative_path.as_posix(),
                "result": "PASS",
                "safeFindingCount": item["scan"].safe_finding_count,
            }
            for item in validated
        ],
        "overallStatus": "PASS",
        "scannerVersion": SCANNER_VERSION,
        "schemaVersion": SCHEMA_VERSION,
    }

    output_root.mkdir(parents=True, exist_ok=True)
    temporary = Path(
        tempfile.mkdtemp(prefix=f".{ARTIFACT_SET_NAME}.tmp-", dir=output_root)
    )
    try:
        for item in validated:
            profile = item["profile"]
            destination = temporary / profile.staged_relative_path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item["source"], destination)
        (temporary / "manifest.json").write_bytes(_json_bytes(manifest))
        (temporary / "scan-report.json").write_bytes(_json_bytes(scan_report))
        _verify_staged_layout(temporary)
        _replace_directory_transactionally(temporary, final_directory)
        _verify_staged_layout(final_directory)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)

    return final_directory


def scanner_main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--environment",
        choices=[profile.environment for profile in ARTIFACT_PROFILES],
        help="Also enforce the selected environment's profile markers.",
    )
    parser.add_argument("artifacts", nargs="+", type=Path)
    arguments = parser.parse_args(argv)
    if arguments.environment and len(arguments.artifacts) != 1:
        parser.error("--environment requires exactly one artifact")
    selected_profile = next(
        (
            profile
            for profile in ARTIFACT_PROFILES
            if profile.environment == arguments.environment
        ),
        None,
    )
    rejected = False
    for artifact in arguments.artifacts:
        try:
            result = scan_file(artifact)
        except (ArtifactSecurityError, OSError):
            print(f"REJECTED path={artifact.as_posix()} rule=ARTIFACT_READ_ERROR")
            rejected = True
            continue
        if result.passed:
            if selected_profile is not None:
                try:
                    validate_profile(selected_profile, artifact.read_bytes())
                except (ArtifactSecurityError, OSError):
                    print(
                        f"REJECTED path={artifact.as_posix()} "
                        f"rule=PROFILE_CHECK_REJECTED "
                        f"environment={selected_profile.environment}"
                    )
                    rejected = True
                    continue
            classification = (
                f" classification={selected_profile.security_profile}"
                if selected_profile is not None
                else ""
            )
            print(f"PASS path={artifact.as_posix()}{classification}")
            continue
        rejected = True
        for finding in result.findings:
            print(
                f"REJECTED path={artifact.as_posix()} rule={finding.rule_id} "
                f"offset={finding.byte_offset}"
            )
    return 1 if rejected else 0


if __name__ == "__main__":
    raise SystemExit(scanner_main())
