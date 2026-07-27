import re
from pathlib import Path

from tools.workflow_static_inspection import inspect_workflow


WORKFLOW = inspect_workflow(Path(".github/workflows/ci.yml"))
APPROVED_ENVIRONMENTS = [
    "esp32-s3-devkitc-1",
    "esp32-s3-dev-software-key",
    "esp32-s3-dev-software-identity-self-test",
]


def test_workflow_matrix_contains_exactly_three_approved_environments():
    build = WORKFLOW.job("build-development-firmware")
    matrix = re.search(
        r"^        environment:\s*$\n((?:^          - [^\n]+$\n?)+)",
        build,
        re.MULTILINE,
    )
    assert matrix is not None
    environments = re.findall(r"^          - ([^\n]+)$", matrix.group(1), re.MULTILINE)
    assert environments == APPROVED_ENVIRONMENTS
    assert set(WORKFLOW.jobs) == {
        "validate",
        "build-development-firmware",
        "aggregate-development-firmware",
    }


def test_matrix_scans_and_classifies_before_temporary_transfer_upload():
    build = WORKFLOW.job("build-development-firmware")
    scan = build.index("Scan and classify firmware before transfer")
    prepare = build.index("Prepare firmware-only transfer directory")
    upload = build.index("Transfer scanned development firmware")
    assert scan < prepare < upload
    assert "--environment \"${{ matrix.environment }}\"" in build
    assert "continue-on-error" not in build
    assert "always()" not in build


def test_temporary_transfer_upload_contains_only_firmware_bin():
    upload = WORKFLOW.step(
        "build-development-firmware", "Transfer scanned development firmware"
    )
    assert "path: ci-transfer/${{ matrix.environment }}/firmware.bin" in upload
    assert "if-no-files-found: error" in upload
    assert "include-hidden-files: false" in upload
    assert not re.search(r"firmware\.(?:elf|map)|partitions\.bin|bootloader\.bin", upload)


def test_aggregation_depends_on_validation_and_complete_matrix():
    aggregate = WORKFLOW.job("aggregate-development-firmware")
    assert "needs: [validate, build-development-firmware]" in aggregate
    assert aggregate.count("uses: actions/download-artifact@v4") == 3
    for environment in APPROVED_ENVIRONMENTS:
        assert f"name: scanned-development-firmware-{environment}" in aggregate


def test_aggregation_rescans_stages_and_verifies_before_final_upload():
    aggregate = WORKFLOW.job("aggregate-development-firmware")
    reconstruct = aggregate.index("Reject transfer omissions and extras")
    rescan = aggregate.index("Rescan and classify reconstructed firmware")
    stage = aggregate.index("Stage deterministic development artifact set")
    verify = aggregate.index("Verify manifest report and final allowlist")
    upload = aggregate.index("Upload final development artifact set")
    assert reconstruct < rescan < stage < verify < upload
    assert "tools/stage_development_artifacts.py" in aggregate
    assert "continue-on-error" not in aggregate
    assert "always()" not in aggregate


def test_final_artifact_name_is_exact():
    upload = WORKFLOW.step(
        "aggregate-development-firmware", "Upload final development artifact set"
    )
    assert re.search(r"^          name: firmware-development-unsigned$", upload, re.MULTILINE)


def test_final_upload_path_is_exact_staged_directory():
    upload = WORKFLOW.step(
        "aggregate-development-firmware", "Upload final development artifact set"
    )
    paths = re.findall(r"^          path: (.+)$", upload, re.MULTILINE)
    assert paths == [".pio/artifacts/firmware-development-unsigned/"]
    assert "include-hidden-files: true" in upload
    assert "overwrite:" not in upload


def test_release_signing_and_production_upload_paths_are_absent():
    lowered = WORKFLOW.text.lower()
    forbidden = [
        "workflow_dispatch",
        "sign-development",
        "publish-release",
        "github release",
        "secrets.",
        "aws s3",
        "ota_signing",
        "productioneligible: true",
    ]
    assert all(token not in lowered for token in forbidden)
