"""Stage the three allowlisted unsigned development firmware artifacts locally."""

from __future__ import annotations

import argparse
from pathlib import Path

from artifact_security import ArtifactSecurityError, stage_development_artifacts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    arguments = parser.parse_args()
    try:
        staged = stage_development_artifacts(arguments.repo_root)
    except (ArtifactSecurityError, OSError) as error:
        print(f"ARTIFACT_STAGING_REJECTED reason={error}")
        return 1
    print(f"ARTIFACT_STAGING_PASS path={staged.relative_to(arguments.repo_root.resolve()).as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
