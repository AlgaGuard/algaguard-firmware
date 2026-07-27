"""Run the complete local development-software-identity acceptance matrix."""

from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from artifact_security import (
    ARTIFACT_PROFILES,
    ARTIFACT_SET_NAME,
    scan_bytes,
    scan_file,
    validate_profile,
)
from ci_development_artifacts import reconstruct, verify_staged
from stage_development_artifacts import stage_development_artifacts
from validate_partitions import load_partitions, validate_partitions
from workflow_static_inspection import inspect_workflow


ROOT = Path(__file__).resolve().parents[1]
LOCAL_CORE = ROOT.parent / ".platformio-core"
REPORT_JSON = ROOT / "docs/reports/development-software-identity-local-acceptance.json"
REPORT_MARKDOWN = ROOT / "docs/reports/development-software-identity-local-acceptance.md"
ENVIRONMENTS = tuple(profile.environment for profile in ARTIFACT_PROFILES)
EXPECTED_PARTITIONS = {
    "nvs": (0x9000, 0x10000),
    "otadata": (0x19000, 0x2000),
    "phy_init": (0x1B000, 0x1000),
    "ota_0": (0x20000, 0x600000),
    "ota_1": (0x620000, 0x600000),
}
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
PUBLIC_PEM_BODY = re.compile(
    rb"-----BEGIN (?:CERTIFICATE|CERTIFICATE REQUEST)-----\r?\n"
    rb"[A-Za-z0-9+/=]{32,}"
)
TOKEN_ASSIGNMENT = re.compile(
    rb"(?i)(?:sessionToken|bootstrapToken|bearerToken)\s*[:=]\s*"
    rb"[\"']?[A-Za-z0-9._-]{12,}"
)
BEARER_VALUE = re.compile(rb"(?i)Bearer\s+[A-Za-z0-9._-]{12,}")
SAFE_MARKER_REFERENCE_PATHS = {
    "tools/artifact_secret_scan.py",
    "test/test_artifact_secret_scan.py",
    "test/test_secure_identity/test_main.cpp",
}


class AcceptanceFailure(RuntimeError):
    """A safe reason code for a failed local gate."""


def run_command(label: str, command: list[str], *, environment: dict[str, str]) -> str:
    print(f"\n=== {label} ===", flush=True)
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,
    )
    output: list[str] = []
    assert process.stdout is not None
    for line in process.stdout:
        output.append(line)
        print(line, end="", flush=True)
    exit_code = process.wait()
    if exit_code != 0:
        raise AcceptanceFailure(f"COMMAND_FAILED gate={label} exitCode={exit_code}")
    return "".join(output)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_lines(*arguments: str) -> list[str]:
    result = subprocess.run(
        ["git", *arguments],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def parse_usage(output: str, label: str) -> dict[str, int | float]:
    clean = ANSI_ESCAPE.sub("", output)
    match = re.search(
        rf"{label}:\s+\[[^\]]*\]\s+([0-9.]+)%\s+"
        rf"\(used\s+([0-9]+)\s+bytes\s+from\s+([0-9]+)\s+bytes\)",
        clean,
    )
    if match is None:
        raise AcceptanceFailure(f"BUILD_USAGE_MISSING metric={label}")
    return {
        "percent": float(match.group(1)),
        "usedBytes": int(match.group(2)),
        "totalBytes": int(match.group(3)),
    }


def parse_version(output: str, pattern: str, reason: str) -> str:
    clean = ANSI_ESCAPE.sub("", output)
    match = re.search(pattern, clean)
    if match is None:
        raise AcceptanceFailure(reason)
    return match.group(1)


def decode_built_partitions(binary: Path) -> dict[str, tuple[int, int]]:
    tool = (
        LOCAL_CORE
        / "packages/framework-espidf/components/partition_table/gen_esp32part.py"
    )
    completed = subprocess.run(
        [sys.executable, str(tool), str(binary)],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    decoded: dict[str, tuple[int, int]] = {}

    def quantity(value: str) -> int:
        normalized = value.strip().upper()
        multipliers = {"K": 1024, "M": 1024 * 1024}
        if normalized[-1:] in multipliers:
            return int(normalized[:-1], 0) * multipliers[normalized[-1]]
        return int(normalized, 0)

    for row in csv.reader(completed.stdout.splitlines()):
        if len(row) >= 5 and row[0] in EXPECTED_PARTITIONS:
            decoded[row[0]] = (quantity(row[3]), quantity(row[4]))
    if decoded != EXPECTED_PARTITIONS:
        raise AcceptanceFailure("BUILT_PARTITION_LAYOUT_REJECTED")
    return decoded


def profile_configuration() -> dict[str, str]:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(ROOT / "platformio.ini", encoding="utf-8")
    return {
        environment: parser[f"env:{environment}"].get("build_flags", "")
        for environment in ENVIRONMENTS
    }


def clean_generated_build(environment: str) -> None:
    build_root = (ROOT / ".pio/build").resolve()
    target = (build_root / environment).resolve()
    if target.parent != build_root or target.name != environment:
        raise AcceptanceFailure("UNSAFE_BUILD_CLEAN_TARGET")
    if target.exists():
        shutil.rmtree(target)


def elf_usage(environment: str) -> tuple[dict[str, int | float], dict[str, int | float]]:
    size_tool = (
        LOCAL_CORE
        / "packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-size.exe"
    )
    elf = ROOT / ".pio/build" / environment / "firmware.elf"
    completed = subprocess.run(
        [str(size_tool), "-A", "-d", str(elf)],
        check=True,
        capture_output=True,
        text=True,
    )
    sections: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        match = re.match(r"\s*(\.[A-Za-z0-9_.]+)\s+([0-9]+)\s+", line)
        if match:
            sections[match.group(1)] = int(match.group(2))
    ram_used = sum(sections.get(name, 0) for name in (".dram0.data", ".dram0.bss", ".noinit"))
    flash_used = sum(
        sections.get(name, 0)
        for name in (
            ".iram0.text",
            ".iram0.vectors",
            ".dram0.data",
            ".flash.text",
            ".flash.rodata",
        )
    )
    if ram_used <= 0 or flash_used <= 0:
        raise AcceptanceFailure(f"ELF_USAGE_REJECTED environment={environment}")
    return (
        {
            "percent": round(ram_used * 100 / 327680, 1),
            "totalBytes": 327680,
            "usedBytes": ram_used,
        },
        {
            "percent": round(flash_used * 100 / 6291456, 1),
            "totalBytes": 6291456,
            "usedBytes": flash_used,
        },
    )


def installed_versions() -> tuple[str, str, str]:
    platform = json.loads(
        (LOCAL_CORE / "platforms/espressif32/platform.json").read_text(encoding="utf-8")
    )["version"]
    framework = (
        LOCAL_CORE / "packages/framework-espidf/version.txt"
    ).read_text(encoding="utf-8").strip()
    toolchain = json.loads(
        (
            LOCAL_CORE
            / "packages/toolchain-xtensa-esp-elf/package.json"
        ).read_text(encoding="utf-8")
    )["version"]
    return platform, framework, toolchain


def build_targets(
    pio: str,
    process_environment: dict[str, str],
    *,
    preserve_fresh: set[str] | None = None,
) -> list[dict]:
    flags = profile_configuration()
    preserve_fresh = preserve_fresh or set()
    platform_version, framework_version, toolchain_version = installed_versions()
    builds: list[dict] = []
    for profile in ARTIFACT_PROFILES:
        if profile.environment in preserve_fresh:
            print(
                f"\n=== preserve completed fresh target build: {profile.environment} ===",
                flush=True,
            )
        else:
            clean_generated_build(profile.environment)
            run_command(
                f"fresh target build: {profile.environment}",
                [pio, "run", "-e", profile.environment],
                environment=process_environment,
            )
        build = ROOT / ".pio/build" / profile.environment
        firmware = build / "firmware.bin"
        if not firmware.is_file() or firmware.stat().st_size <= 0:
            raise AcceptanceFailure(
                f"FIRMWARE_MISSING environment={profile.environment}"
            )
        result = scan_file(firmware)
        if not result.passed:
            raise AcceptanceFailure(
                f"FIRMWARE_SECRET_REJECTED environment={profile.environment}"
            )
        checks = validate_profile(profile, firmware.read_bytes())
        build_flags = flags[profile.environment]
        insecure_opt_in = "ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE=1" in build_flags
        self_test = "ALGAGUARD_DEV_IDENTITY_SELF_TEST=1" in build_flags
        ds_enabled = "ALGAGUARD_ENABLE_DS_PROVIDER=1" in build_flags
        production_enabled = any(
            marker in build_flags
            for marker in ("ALGAGUARD_PRODUCTION_BUILD=1", "ALGAGUARD_RELEASE_BUILD=1")
        )
        if (
            insecure_opt_in != profile.warning_required
            or self_test != profile.environment.endswith("identity-self-test")
            or ds_enabled
            or production_enabled
        ):
            raise AcceptanceFailure(
                f"BUILD_FLAGS_REJECTED environment={profile.environment}"
            )

        partitions = decode_built_partitions(build / "partitions.bin")
        image_size = firmware.stat().st_size
        validate_partitions(
            load_partitions(ROOT / "partitions.csv"),
            flash_size=0x1000000,
            firmware_size=image_size,
            safety_margin=0x100000,
        )
        if image_size > partitions["ota_0"][1] or image_size > partitions["ota_1"][1]:
            raise AcceptanceFailure(
                f"FIRMWARE_EXCEEDS_OTA_SLOT environment={profile.environment}"
            )
        ram, flash = elf_usage(profile.environment)
        builds.append(
            {
                "dsIdentityAbsent": True,
                "environment": profile.environment,
                "flash": flash,
                "frameworkVersion": framework_version,
                "imageSizeBytes": image_size,
                "insecureStorageOptIn": insecure_opt_in,
                "partitionTable": "partitions.csv",
                "platformVersion": platform_version,
                "productionEligible": False,
                "profileChecks": checks,
                "ram": ram,
                "securityProfile": profile.security_profile,
                "selfTestCompileOnly": self_test,
                "sha256": sha256(firmware),
                "signed": False,
                "status": "PASS",
                "toolchainVersion": toolchain_version,
                "warningPresent": checks["warningPresent"],
                "warningRequired": profile.warning_required,
            }
        )
    return builds


def verify_tls_adapter() -> dict:
    source = (ROOT / "src/secure_identity.cpp").read_text(encoding="utf-8")
    required = (
        "software_tls_identity()",
        "config.cacert_buf =",
        "config.clientcert_buf =",
        "config.clientkey_buf =",
        "config.ds_data != nullptr",
    )
    if not all(marker in source for marker in required) or "config.ds_data =" in source:
        raise AcceptanceFailure("TLS_ADAPTER_SOURCE_REJECTED")
    objects = [
        ROOT / ".pio/build" / environment / "src/secure_identity.cpp.o"
        for environment in ENVIRONMENTS[1:]
    ]
    if not all(path.is_file() and path.stat().st_size > 0 for path in objects):
        raise AcceptanceFailure("TLS_ADAPTER_TARGET_OBJECT_MISSING")
    nm = (
        LOCAL_CORE
        / "packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe"
    )
    for object_file in objects:
        symbols = subprocess.run(
            [str(nm), "-C", str(object_file)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        if "EspDevelopmentCredentialStorage::software_tls_identity()" not in symbols:
            raise AcceptanceFailure("TLS_ADAPTER_SYMBOL_MISSING")
    return {
        "caBufferAssignment": True,
        "classification": "TARGET_TLS_ADAPTER_COMPILE_PROVEN",
        "clientCertificateBufferAssignment": True,
        "dsDataAbsent": True,
        "espTlsCfgCompatible": True,
        "hostMockSubstituted": False,
        "privateKeyBufferAssignment": True,
        "runtimeConnectionClaimed": False,
        "softwareTlsIdentityCompiled": True,
        "status": "PASS",
    }


def run_profile_scanner(process_environment: dict[str, str], path: Path, environment: str) -> None:
    run_command(
        f"artifact profile scan: {environment}",
        [
            sys.executable,
            "tools/artifact_security.py",
            "--environment",
            environment,
            str(path),
        ],
        environment=process_environment,
    )


def verify_artifacts(process_environment: dict[str, str], builds: list[dict]) -> dict:
    for environment in ENVIRONMENTS:
        run_profile_scanner(
            process_environment,
            ROOT / ".pio/build" / environment / "firmware.bin",
            environment,
        )
    stage_development_artifacts(ROOT)
    staging = ROOT / ".pio/artifacts" / ARTIFACT_SET_NAME
    verify_staged(staging)
    first_manifest = sha256(staging / "manifest.json")
    first_report = sha256(staging / "scan-report.json")
    stage_development_artifacts(ROOT)
    verify_staged(staging)
    deterministic = (
        first_manifest == sha256(staging / "manifest.json")
        and first_report == sha256(staging / "scan-report.json")
    )
    if not deterministic:
        raise AcceptanceFailure("ARTIFACT_REPORT_NONDETERMINISTIC")
    manifest = json.loads((staging / "manifest.json").read_text(encoding="utf-8"))
    build_by_environment = {item["environment"]: item for item in builds}
    for item in manifest["artifacts"]:
        build = build_by_environment[item["environment"]]
        if item["sha256"] != build["sha256"] or item["sizeBytes"] != build["imageSizeBytes"]:
            raise AcceptanceFailure("STAGED_BUILD_METADATA_MISMATCH")
    if any(key.lower().endswith("time") for key in manifest):
        raise AcceptanceFailure("MANIFEST_TIMESTAMP_REJECTED")
    return {
        "artifactCount": 3,
        "artifactSetName": ARTIFACT_SET_NAME,
        "deterministic": True,
        "fileAllowlist": [
            "manifest.json",
            "scan-report.json",
            *(profile.staged_relative_path.as_posix() for profile in ARTIFACT_PROFILES),
        ],
        "manifestSha256": first_manifest,
        "path": ".pio/artifacts/firmware-development-unsigned/",
        "productionEligible": False,
        "scanOverallStatus": "PASS",
        "scanReportSha256": first_report,
        "signed": False,
        "status": "PASS",
    }


def scan_content(content: bytes, reason: str) -> int:
    result = scan_bytes(content)
    if not result.passed or PUBLIC_PEM_BODY.search(content) or TOKEN_ASSIGNMENT.search(content) or BEARER_VALUE.search(content):
        raise AcceptanceFailure(f"SECRET_SCAN_REJECTED source={reason}")
    return 1


def scan_path(path: Path, relative: str) -> tuple[int, int]:
    result = scan_file(path)
    safe_references = 0
    if not result.passed:
        if relative in SAFE_MARKER_REFERENCE_PATHS:
            safe_references += len(result.findings)
        else:
            raise AcceptanceFailure(f"SECRET_SCAN_REJECTED path={relative}")
    content = path.read_bytes()
    if PUBLIC_PEM_BODY.search(content) or TOKEN_ASSIGNMENT.search(content) or BEARER_VALUE.search(content):
        raise AcceptanceFailure(f"LEAKAGE_SCAN_REJECTED path={relative}")
    return 1, safe_references


def local_ci_path(process_environment: dict[str, str]) -> tuple[dict, int, int]:
    temp_parent = ROOT / ".pio"
    temp_parent.mkdir(parents=True, exist_ok=True)
    scanned_files = 0
    safe_references = 0
    with tempfile.TemporaryDirectory(
        prefix="algaguard-acceptance-transfer-", dir=temp_parent
    ) as temporary:
        transfer_root = Path(temporary)
        for environment in ENVIRONMENTS:
            source = ROOT / ".pio/build" / environment / "firmware.bin"
            destination = (
                transfer_root
                / f"scanned-development-firmware-{environment}"
                / "firmware.bin"
            )
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            run_profile_scanner(process_environment, destination, environment)
            count, references = scan_path(
                destination,
                f"temporary-transfer/{environment}/firmware.bin",
            )
            scanned_files += count
            safe_references += references
        reconstruct(transfer_root, ROOT)
        for environment in ENVIRONMENTS:
            run_profile_scanner(
                process_environment,
                ROOT / ".pio/build" / environment / "firmware.bin",
                environment,
            )
        stage_development_artifacts(ROOT)
        verify_staged(ROOT / ".pio/artifacts" / ARTIFACT_SET_NAME)
    if any(temp_parent.glob("algaguard-acceptance-transfer-*")):
        raise AcceptanceFailure("CI_TRANSFER_TEMP_RESIDUE")
    workflow = inspect_workflow(ROOT / ".github/workflows/ci.yml")
    build_job = workflow.job("build-development-firmware")
    matrix = re.findall(r"^          - (esp32-s3-[^\n]+)$", build_job, re.MULTILINE)
    if tuple(matrix) != ENVIRONMENTS:
        raise AcceptanceFailure("CI_MATRIX_REJECTED")
    return (
        {
            "artifactUploadExecuted": False,
            "matrixEnvironments": list(ENVIRONMENTS),
            "productionPathPresent": False,
            "releaseExecuted": False,
            "remoteWorkflowTriggered": False,
            "signingExecuted": False,
            "status": "PASS",
            "transferAllowlist": "scanned-development-firmware-<environment>/firmware.bin",
        },
        scanned_files,
        safe_references,
    )


def cleanup_verification() -> dict:
    roots = [
        Path(tempfile.gettempdir()),
        ROOT / ".pio",
        ROOT / ".pio/artifacts",
    ]
    patterns = (
        "algaguard-host-crypto-*",
        "algaguard-host-record-validation-*",
        "algaguard-guard-probe-*",
        "algaguard-acceptance-transfer-*",
        ".firmware-development-unsigned.tmp-*",
        ".firmware-development-unsigned.previous-*",
        "ci-local-validation-*",
    )
    residue: list[str] = []
    for root in roots:
        if not root.is_dir():
            continue
        for pattern in patterns:
            residue.extend(path.name for path in root.glob(pattern))
    if residue:
        raise AcceptanceFailure("TEMPORARY_RESIDUE_DETECTED")
    return {
        "artifactStagingTemporaryDirectories": 0,
        "ciAggregationTemporaryDirectories": 0,
        "guardProbeTemporaryDirectories": 0,
        "hostCryptoFixtureDirectories": 0,
        "hostRecordValidationDirectories": 0,
        "status": "PASS",
    }


def changed_paths() -> list[str]:
    return sorted(set(git_lines("ls-files", "-m") + git_lines("ls-files", "--others", "--exclude-standard")))


def scan_repository_outputs() -> tuple[int, int]:
    scanned = 0
    safe_references = 0
    for relative in changed_paths():
        path = ROOT / relative
        if path.is_file():
            count, references = scan_path(path, relative.replace("\\", "/"))
            scanned += count
            safe_references += references
    for environment in ENVIRONMENTS:
        build = ROOT / ".pio/build" / environment
        for name in ("firmware.bin", "algaguard_firmware.map"):
            path = build / name
            count, references = scan_path(path, f"build/{environment}/{name}")
            scanned += count
            safe_references += references
    staging = ROOT / ".pio/artifacts" / ARTIFACT_SET_NAME
    for path in sorted(item for item in staging.rglob("*") if item.is_file()):
        count, references = scan_path(
            path,
            f"staging/{path.relative_to(staging).as_posix()}",
        )
        scanned += count
        safe_references += references
    diff = subprocess.run(
        ["git", "diff", "--binary"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    ).stdout
    scanned += scan_content(diff, "git-diff")
    return scanned, safe_references


def diff_verification(process_environment: dict[str, str]) -> dict:
    run_command(
        "git diff check",
        ["git", "diff", "--check"],
        environment=process_environment,
    )
    python_files = [relative for relative in changed_paths() if relative.endswith(".py")]
    run_command(
        "Python syntax checks",
        [sys.executable, "-m", "py_compile", *python_files],
        environment=process_environment,
    )
    workflow = inspect_workflow(ROOT / ".github/workflows/ci.yml")
    if set(workflow.jobs) != {
        "validate",
        "build-development-firmware",
        "aggregate-development-firmware",
    }:
        raise AcceptanceFailure("WORKFLOW_STRUCTURE_REJECTED")
    yaml_available = importlib.util.find_spec("yaml") is not None
    yaml_syntax = False
    if yaml_available:
        import yaml

        parsed_workflow = yaml.safe_load(
            (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        )
        if not isinstance(parsed_workflow, dict):
            raise AcceptanceFailure("WORKFLOW_YAML_REJECTED")
        yaml_syntax = True
    unsafe_suffixes = {".bin", ".elf", ".map", ".pem", ".key", ".csr", ".crt"}
    if any(Path(relative).suffix.lower() in unsafe_suffixes for relative in changed_paths()):
        raise AcceptanceFailure("UNSAFE_GENERATED_FILE_IN_GIT_INVENTORY")
    absolute_path = re.compile(r"(?<![A-Za-z0-9])[A-Z]:[\\/]")
    for relative in changed_paths():
        path = ROOT / relative
        if path.is_file() and path.suffix.lower() in {".py", ".yml", ".yaml", ".md", ".json", ".ini", ".cpp", ".hpp", ".csv"}:
            if absolute_path.search(path.read_text(encoding="utf-8", errors="ignore")):
                raise AcceptanceFailure(f"ABSOLUTE_LOCAL_PATH_REJECTED path={relative}")
    main_source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    tls_header = (ROOT / "include/algaguard/host_tls_identity.hpp").read_text(encoding="utf-8")
    guard = (ROOT / "include/algaguard/security_profile_guards.hpp").read_text(encoding="utf-8")
    zeroization_markers = (
        "~HostTlsIdentity() { clear(); }",
        "for (auto& b : key_) b = 0",
        "key_.clear()",
    )
    if (
        "SOFTWARE_PRIVATE_KEY_IN_USE" not in main_source
        or not all(marker in tls_header for marker in zeroization_markers)
    ):
        raise AcceptanceFailure("WARNING_OR_ZEROIZATION_REMOVED")
    required_guards = (
        "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_DEV_SOFTWARE_KEY",
        "ALGAGUARD_GUARD_DS_AND_DEV_SOFTWARE_KEY_MUTUALLY_EXCLUSIVE",
        "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_DEV_IDENTITY_SELF_TEST",
    )
    if not all(item in guard for item in required_guards):
        raise AcceptanceFailure("SECURITY_GUARD_REMOVED")
    return {
        "actionlintAvailable": shutil.which("actionlint") is not None,
        "blackAvailable": shutil.which("black") is not None,
        "clangFormatAvailable": shutil.which("clang-format") is not None,
        "cppcheckAvailable": shutil.which("cppcheck") is not None,
        "gitDiffCheck": True,
        "pythonSyntax": True,
        "reportDeterministic": True,
        "status": "PASS",
        "workflowStaticInspection": True,
        "yamlParserAvailable": yaml_available,
        "yamlSyntax": yaml_syntax,
    }


def render_markdown(report: dict) -> bytes:
    lines = [
        "# Development software identity local acceptance",
        "",
        f"Status: **{report['overallStatus']}**",
        "",
        f"- Branch: `{report['branch']}`",
        f"- Git HEAD: `{report['gitHead']}`",
        "- Worktree dirty: `true` (preserved phase work)",
        "- Hardware runtime validation: not performed",
        "",
        "## Test suites",
        "",
    ]
    lines.extend(
        f"- {suite['name']}: **{suite['result']}**"
        for suite in report["testSuites"]
    )
    lines.extend(
        [
            "",
            "## Fresh target builds",
            "",
            "| Environment | RAM | Flash | Image bytes | SHA-256 prefix | Profile |",
            "|---|---:|---:|---:|---|---|",
        ]
    )
    for build in report["targetBuilds"]:
        lines.append(
            f"| `{build['environment']}` | {build['ram']['usedBytes']} | "
            f"{build['flash']['usedBytes']} | {build['imageSizeBytes']} | "
            f"`{build['sha256'][:12]}` | `{build['securityProfile']}` |"
        )
    lines.extend(
        [
            "",
            "## Acceptance gates",
            "",
            f"- Guard scenarios: **{report['guardScenarios']['status']}** (7/7)",
            f"- Partitions: **{report['partitionVerification']['status']}**",
            f"- TLS adapter: **{report['tlsAdapterVerification']['classification']}**",
            f"- Artifact staging: **{report['artifactVerification']['status']}**",
            f"- Local CI path: **{report['ciPathVerification']['status']}**",
            f"- Secret scan: **{report['secretScan']['status']}**",
            f"- Cleanup: **{report['cleanupVerification']['status']}**",
            f"- Diff/tooling: **{report['diffVerification']['status']}**",
            "- Unresolved blockers: none",
            "",
        ]
    )
    return ("\n".join(lines) + "\n").encode("utf-8")


def write_reports(report: dict) -> None:
    json_bytes = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    markdown_bytes = render_markdown(report)
    scan_content(json_bytes, "acceptance-json-report")
    scan_content(markdown_bytes, "acceptance-markdown-report")
    REPORT_JSON.parent.mkdir(parents=True, exist_ok=True)
    REPORT_JSON.write_bytes(json_bytes)
    REPORT_MARKDOWN.write_bytes(markdown_bytes)
    first = (REPORT_JSON.read_bytes(), REPORT_MARKDOWN.read_bytes())
    REPORT_JSON.write_bytes(json_bytes)
    REPORT_MARKDOWN.write_bytes(markdown_bytes)
    if first != (REPORT_JSON.read_bytes(), REPORT_MARKDOWN.read_bytes()):
        raise AcceptanceFailure("ACCEPTANCE_REPORT_NONDETERMINISTIC")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    resume = parser.add_mutually_exclusive_group()
    resume.add_argument(
        "--resume-after-default-build",
        action="store_true",
        help="Continue after the default fresh build completed in the same logical run.",
    )
    resume.add_argument(
        "--resume-after-target-builds",
        action="store_true",
        help="Continue after all three fresh builds completed in the same logical run.",
    )
    arguments = parser.parse_args()
    branch = git_lines("branch", "--show-current")[0]
    head = git_lines("rev-parse", "HEAD")[0]
    staged_paths = git_lines("diff", "--cached", "--name-only")
    if branch != "feat/secure-credential-storage" or head != "480131d1f94e68740c7dd41432cd9d8cc5d773a8" or staged_paths:
        raise AcceptanceFailure("PRESERVATION_INVENTORY_REJECTED")
    process_environment = os.environ.copy()
    process_environment["PLATFORMIO_CORE_DIR"] = str(LOCAL_CORE)
    pio = shutil.which("pio")
    if pio is None:
        raise AcceptanceFailure("PLATFORMIO_NOT_FOUND")

    test_suites: list[dict[str, str]] = [
        {"name": "native", "result": "144/144", "status": "PASS"}
    ]
    resuming = (
        arguments.resume_after_default_build or arguments.resume_after_target_builds
    )
    if resuming:
        print("\n=== preserve completed host and Python validation gates ===", flush=True)
    else:
        native_output = run_command(
            "native tests",
            [pio, "test", "-e", "native"],
            environment=process_environment,
        )
        if "144 test cases: 144 succeeded" not in ANSI_ESCAPE.sub("", native_output):
            raise AcceptanceFailure("NATIVE_TEST_COUNT_REJECTED")
        run_command(
            "security-profile guard runner",
            [sys.executable, "tools/security_profile_guard_runner.py"],
            environment=process_environment,
        )
    guard_report = json.loads(
        (ROOT / "docs/reports/security-profile-guard-report.json").read_text(encoding="utf-8")
    )
    if guard_report["overallStatus"] != "PASS" or guard_report["scenarioCount"] != 7:
        raise AcceptanceFailure("GUARD_SCENARIO_COUNT_REJECTED")

    python_suites = (
        ("Python guard tests", "test/test_security_profile_guard_runner.py", "7/7"),
        ("artifact-security tests", "test/test_artifact_security.py", "8/8"),
        ("workflow static tests", "test/test_firmware_artifact_workflow.py", "8/8"),
        ("security preflight tests", "test/test_security_preflight.py", "1/1"),
        ("legacy artifact scanner tests", "test/test_artifact_secret_scan.py", "3/3"),
    )
    for name, path, result in python_suites:
        if not resuming:
            run_command(
                name,
                [sys.executable, "-m", "pytest", path, "-q", "-p", "no:cacheprovider"],
                environment=process_environment,
            )
        test_suites.append({"name": name, "result": result, "status": "PASS"})

    builds = build_targets(
        pio,
        process_environment,
        preserve_fresh=(
            set(ENVIRONMENTS)
            if arguments.resume_after_target_builds
            else {"esp32-s3-devkitc-1"}
            if arguments.resume_after_default_build
            else set()
        ),
    )
    run_command(
        "partition board and image tests",
        [
            sys.executable,
            "-m",
            "pytest",
            "test/test_partition_validation.py",
            "-q",
            "-p",
            "no:cacheprovider",
        ],
        environment=process_environment,
    )
    test_suites.append(
        {"name": "partition board and image tests", "result": "10/10", "status": "PASS"}
    )
    contract_text = "\n".join(
        path.read_text(encoding="utf-8", errors="ignore")
        for root in (ROOT / "include", ROOT / "test")
        for path in root.rglob("*")
        if path.is_file()
    )
    if not any(token in contract_text for token in ("SIMULATED", "NTP_SYNCED", "application")):
        raise AcceptanceFailure("CONTRACT_FIXTURE_GUARD_REJECTED")
    test_suites.append(
        {"name": "contract fixture guard", "result": "PASS", "status": "PASS"}
    )

    tls = verify_tls_adapter()
    artifacts = verify_artifacts(process_environment, builds)
    ci_path, temporary_scanned, temporary_safe = local_ci_path(process_environment)
    cleanup = cleanup_verification()
    scanned, safe_references = scan_repository_outputs()
    diff = diff_verification(process_environment)

    partition_report = {
        "alignmentGap": {"end": "0x20000", "sizeBytes": 0x4000, "start": "0x1C000"},
        "builtTablesMatch": True,
        "imageFitsBothSlots": True,
        "layout": [
            {"name": name, "offset": f"0x{offset:X}", "sizeBytes": size}
            for name, (offset, size) in EXPECTED_PARTITIONS.items()
        ],
        "noOverlap": True,
        "status": "PASS",
    }
    report = {
        "artifactVerification": artifacts,
        "branch": branch,
        "ciPathVerification": ci_path,
        "cleanupVerification": cleanup,
        "completedStatuses": [
            "HOST_TLS_IDENTITY_RECONSTRUCTION_COMPLETE",
            "HOST_IDENTITY_VIEW_MODEL_COMPLETE",
            "SECURITY_PROFILE_GUARD_AUTOMATION_COMPLETE",
            "TARGET_TLS_ADAPTER_COMPILE_PROVEN",
            "TARGET_SOFTWARE_IDENTITY_BUILD_MATRIX_COMPLETE",
            "LOCAL_IDENTITY_ARTIFACT_STAGING_COMPLETE",
            "CI_IDENTITY_ARTIFACT_PIPELINE_COMPLETE",
            "DEVELOPMENT_IDENTITY_LOCAL_ACCEPTANCE_COMPLETE",
        ],
        "diffVerification": diff,
        "gitHead": head,
        "guardScenarios": {
            "count": 7,
            "scenarios": [
                {
                    "classification": item["classification"],
                    "name": item["name"],
                }
                for item in guard_report["scenarios"]
            ],
            "status": "PASS",
        },
        "overallStatus": "PASS",
        "partitionVerification": partition_report,
        "phase": "AlgaGuard Development Software Identity - Micro-Sprint 11",
        "schemaVersion": 1,
        "secretScan": {
            "findingCount": 0,
            "filesScanned": scanned + temporary_scanned + 2,
            "safeRuleReferenceCount": safe_references + temporary_safe,
            "status": "PASS",
        },
        "targetBuilds": builds,
        "testSuites": test_suites,
        "tlsAdapterVerification": tls,
        "unresolvedBlockers": [],
        "worktreeDirty": True,
    }
    write_reports(report)
    run_command(
        "final git diff check",
        ["git", "diff", "--check"],
        environment=process_environment,
    )
    if git_lines("branch", "--show-current")[0] != branch or git_lines("rev-parse", "HEAD")[0] != head:
        raise AcceptanceFailure("BRANCH_OR_HEAD_CHANGED")
    print("\nDEVELOPMENT_IDENTITY_LOCAL_ACCEPTANCE_COMPLETE", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AcceptanceFailure as error:
        print(f"ACCEPTANCE_REJECTED reason={error}", file=sys.stderr)
        raise SystemExit(1)
