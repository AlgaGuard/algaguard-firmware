"""Conservative structural inspection for the repository's GitHub workflow."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


TOP_LEVEL_PATTERN = re.compile(r"^([A-Za-z][A-Za-z0-9_-]*):(?:\s.*)?$", re.MULTILINE)
JOB_PATTERN = re.compile(r"^  ([A-Za-z][A-Za-z0-9_-]*):\s*$", re.MULTILINE)
STEP_PATTERN = re.compile(r"^      - (?:name: ([^\n]+)|uses: ([^\n]+)|run:.*)$", re.MULTILINE)


@dataclass(frozen=True)
class WorkflowDocument:
    text: str
    jobs: dict[str, str]

    def job(self, name: str) -> str:
        if name not in self.jobs:
            raise AssertionError(f"workflow job missing: {name}")
        return self.jobs[name]

    def step(self, job_name: str, step_name: str) -> str:
        job = self.job(job_name)
        starts = list(STEP_PATTERN.finditer(job))
        for index, match in enumerate(starts):
            if match.group(1) == step_name:
                end = starts[index + 1].start() if index + 1 < len(starts) else len(job)
                return job[match.start() : end]
        raise AssertionError(f"workflow step missing: {job_name}/{step_name}")


def inspect_workflow(path: Path) -> WorkflowDocument:
    text = path.read_text(encoding="utf-8")
    if not text.endswith("\n") or "\t" in text:
        raise AssertionError("workflow must use newline-terminated space indentation")
    if text.count("${{") != text.count("}}"):
        raise AssertionError("unbalanced GitHub expression delimiters")

    top_level = TOP_LEVEL_PATTERN.findall(text)
    if top_level != ["name", "on", "permissions", "env", "jobs"]:
        raise AssertionError(f"unexpected top-level workflow structure: {top_level}")

    jobs_offset = re.search(r"^jobs:\s*$", text, re.MULTILINE)
    if jobs_offset is None:
        raise AssertionError("jobs mapping missing")
    jobs_text = text[jobs_offset.end() :]
    matches = list(JOB_PATTERN.finditer(jobs_text))
    names = [match.group(1) for match in matches]
    if len(names) != len(set(names)):
        raise AssertionError("duplicate job identifier")
    jobs: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(jobs_text)
        jobs[match.group(1)] = jobs_text[match.start() : end]
    return WorkflowDocument(text, jobs)
