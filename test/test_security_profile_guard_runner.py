import importlib.util
import json
import sys
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "security_profile_guard_runner.py"
SPEC = importlib.util.spec_from_file_location("guard_runner", MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)


def test_all_seven_scenarios_are_defined():
    assert len(runner.SCENARIOS) == 7


def test_expected_pass_cases_compile():
    report = runner.run_all(runner.locate_compiler())
    assert all(item["classification"] == "GUARD_PASS" for item in report["scenarios"] if item["expected"] == "PASS")


def test_expected_rejections_match_intended_guards():
    report = runner.run_all(runner.locate_compiler())
    rejected = [item for item in report["scenarios"] if item["expected"] == "FAIL"]
    assert all(item["classification"] == "EXPECTED_GUARD_REJECTION" and item["matchedGuardIdentifier"] for item in rejected)


def test_unrelated_failure_is_not_expected_rejection():
    classification, _ = runner.classify("FAIL", 1, "unrelated compiler error", "EXPECTED_GUARD")
    assert classification == "UNEXPECTED_COMPILE_FAILURE"


def test_unexpected_acceptance_is_detected():
    classification, _ = runner.classify("FAIL", 0, "", "EXPECTED_GUARD")
    assert classification == "UNEXPECTED_GUARD_ACCEPTANCE"


def test_report_schema_and_order_are_deterministic():
    report = runner.run_all(runner.locate_compiler())
    assert report["schemaVersion"] == 1 and report["scenarioCount"] == 7
    assert [item["name"] for item in report["scenarios"]] == [scenario.name for scenario in runner.SCENARIOS]
    json.dumps(report)


def test_temporary_build_artifacts_are_cleaned(tmp_path):
    runner.run_all(runner.locate_compiler(), temp_parent=tmp_path)
    assert list(tmp_path.iterdir()) == []
