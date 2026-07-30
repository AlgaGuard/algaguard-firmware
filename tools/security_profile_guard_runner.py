#!/usr/bin/env python3
"""Compile the authoritative security-profile guard probe on the host."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUARD = Path("include/algaguard/security_profile_guards.hpp")
PROBE = Path("test/guard_probe/security_profile_guard_probe.cpp")
DEFAULT_REPORT = Path("docs/reports/security-profile-guard-report.json")


@dataclass(frozen=True)
class Scenario:
    name: str
    expected: str
    defines: tuple[str, ...] = ()
    guard: str = ""


SCENARIOS = (
    Scenario("default-development", "PASS"),
    Scenario("dev-software-key-without-opt-in", "FAIL", ("ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY=1",), "ALGAGUARD_GUARD_DEV_SOFTWARE_KEY_REQUIRES_INSECURE_STORAGE_OPT_IN"),
    Scenario("dev-software-key-with-opt-in", "PASS", ("ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY=1", "ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE=1")),
    Scenario("production-with-dev-software-key", "FAIL", ("ALGAGUARD_PRODUCTION_BUILD=1", "ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY=1", "ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE=1"), "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_DEV_SOFTWARE_KEY"),
    Scenario("ds-with-dev-software-key", "FAIL", ("ALGAGUARD_ENABLE_DS_PROVIDER=1", "ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY=1", "ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE=1"), "ALGAGUARD_GUARD_DS_AND_DEV_SOFTWARE_KEY_MUTUALLY_EXCLUSIVE"),
    Scenario("production-with-dev-identity-self-test", "FAIL", ("ALGAGUARD_PRODUCTION_BUILD=1", "ALGAGUARD_DEV_IDENTITY_SELF_TEST=1"), "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_DEV_IDENTITY_SELF_TEST"),
    Scenario("self-test-with-dev-software-key-and-opt-in", "PASS", ("ALGAGUARD_DEV_IDENTITY_SELF_TEST=1", "ALGAGUARD_SECURITY_PROFILE_DEV_SOFTWARE_KEY=1", "ALGAGUARD_ALLOW_INSECURE_KEY_STORAGE=1")),
    Scenario("physical-test-development", "PASS", ("ALGAGUARD_PHYSICAL_TEST_MODE=1", "ALGAGUARD_DEVELOPMENT_BUILD=1", "ALGAGUARD_DEVELOPMENT_PHYSICAL_TEST_OPT_IN=1", "ALGAGUARD_WIFI_CREDENTIAL_PERSISTENCE_DISABLED=1")),
    Scenario("production-with-physical-test", "FAIL", ("ALGAGUARD_PHYSICAL_TEST_MODE=1", "ALGAGUARD_DEVELOPMENT_BUILD=1", "ALGAGUARD_DEVELOPMENT_PHYSICAL_TEST_OPT_IN=1", "ALGAGUARD_WIFI_CREDENTIAL_PERSISTENCE_DISABLED=1", "ALGAGUARD_PRODUCTION_BUILD=1"), "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_PHYSICAL_TEST"),
    Scenario("local-oled-demo-development", "PASS", ("ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS=1", "ALGAGUARD_LOCAL_DEMO_MODE=1", "ALGAGUARD_DEVELOPMENT_BUILD=1")),
    Scenario("local-mock-without-demo-opt-in", "FAIL", ("ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS=1", "ALGAGUARD_DEVELOPMENT_BUILD=1"), "ALGAGUARD_GUARD_LOCAL_MOCK_SENSORS_REQUIRE_DEVELOPMENT_DEMO"),
    Scenario("production-with-local-mock", "FAIL", ("ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS=1", "ALGAGUARD_LOCAL_DEMO_MODE=1", "ALGAGUARD_DEVELOPMENT_BUILD=1", "ALGAGUARD_PRODUCTION_BUILD=1"), "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_LOCAL_MOCK_SENSORS"),
    Scenario("release-with-local-mock", "FAIL", ("ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS=1", "ALGAGUARD_LOCAL_DEMO_MODE=1", "ALGAGUARD_DEVELOPMENT_BUILD=1", "ALGAGUARD_RELEASE_BUILD=1"), "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_LOCAL_MOCK_SENSORS"),
    Scenario("ds-with-local-mock", "FAIL", ("ALGAGUARD_ENABLE_LOCAL_MOCK_SENSORS=1", "ALGAGUARD_LOCAL_DEMO_MODE=1", "ALGAGUARD_DEVELOPMENT_BUILD=1", "ALGAGUARD_ENABLE_DS_PROVIDER=1"), "ALGAGUARD_GUARD_PRODUCTION_FORBIDS_LOCAL_MOCK_SENSORS"),
)


def classify(expected: str, exit_code: int, diagnostics: str, guard: str) -> tuple[str, str]:
    matched = guard if guard and guard in diagnostics else ""
    if expected == "PASS":
        return ("GUARD_PASS" if exit_code == 0 else "UNEXPECTED_COMPILE_FAILURE", matched)
    if exit_code == 0:
        return "UNEXPECTED_GUARD_ACCEPTANCE", matched
    if matched:
        return "EXPECTED_GUARD_REJECTION", matched
    return "UNEXPECTED_COMPILE_FAILURE", matched


def locate_compiler() -> str:
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise RuntimeError("HOST_CXX_NOT_FOUND")
    return compiler


def run_all(compiler: str, root: Path = ROOT, temp_parent: Path | None = None) -> dict:
    identity = subprocess.run([compiler, "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    results = []
    with tempfile.TemporaryDirectory(prefix="algaguard-guard-probe-", dir=temp_parent) as work:
        for scenario in SCENARIOS:
            command = [compiler, "-std=c++17", f"-I{root / 'include'}", "-fsyntax-only"]
            command.extend(f"-D{item}" for item in scenario.defines)
            command.append(str(root / PROBE))
            completed = subprocess.run(command, cwd=work, capture_output=True, text=True)
            classification, matched = classify(scenario.expected, completed.returncode, completed.stderr, scenario.guard)
            results.append({
                "name": scenario.name,
                "expected": scenario.expected,
                "actual": "PASS" if completed.returncode == 0 else "FAIL",
                "classification": classification,
                "exitCode": completed.returncode,
                "expectedGuardIdentifier": scenario.guard,
                "matchedGuardIdentifier": matched,
            })
    accepted = {"GUARD_PASS", "EXPECTED_GUARD_REJECTION"}
    return {
        "schemaVersion": 1,
        "authoritativeGuardHeader": GUARD.as_posix(),
        "probeSource": PROBE.as_posix(),
        "compilerIdentity": identity,
        "scenarioCount": len(results),
        "scenarios": results,
        "overallStatus": "PASS" if all(item["classification"] in accepted for item in results) else "FAIL",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, default=ROOT / DEFAULT_REPORT)
    args = parser.parse_args()
    report = run_all(locate_compiler())
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for item in report["scenarios"]:
        print(f"{item['name']}: {item['classification']}")
    print(f"overall: {report['overallStatus']}")
    return 0 if report["overallStatus"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
