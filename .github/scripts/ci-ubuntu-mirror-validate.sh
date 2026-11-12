#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail

repo="${1:?usage: ci-ubuntu-mirror-validate.sh REPO EXPECTED_SHA EVIDENCE_DIR}"
expected_sha="${2:?usage: ci-ubuntu-mirror-validate.sh REPO EXPECTED_SHA EVIDENCE_DIR}"
evidence_dir="${3:?usage: ci-ubuntu-mirror-validate.sh REPO EXPECTED_SHA EVIDENCE_DIR}"

actual_sha="$(git -C "$repo" rev-parse HEAD)"
test "$actual_sha" = "$expected_sha"

mkdir -p "$evidence_dir/shell"
exec > >(tee "$evidence_dir/validate.log") 2>&1

printf 'repository: %s\ncommit: %s\n' "$repo" "$actual_sha"

python3 - "$repo/.github/workflows/ci.yml" "$evidence_dir/shell" <<'PY'
import pathlib
import sys

import yaml

workflow_path = pathlib.Path(sys.argv[1])
shell_dir = pathlib.Path(sys.argv[2])
workflow = yaml.safe_load(workflow_path.read_text())

install_steps = []
for job_name, job in workflow["jobs"].items():
    for step in job.get("steps", []):
        run = step.get("run")
        if isinstance(run, str) and "apt-mirrors.txt" in run:
            install_steps.append((job_name, step.get("name"), run))

expected_jobs = {"clippy", "test-linux", "rustdoc", "msrv"}
actual_jobs = {job for job, _, _ in install_steps}
assert len(install_steps) == 4, install_steps
assert actual_jobs == expected_jobs, actual_jobs

for job, step_name, run in install_steps:
    assert step_name == "Install dependencies", (job, step_name)
    assert "https://mirrors.edge.kernel.org/ubuntu/" in run, job
    assert "http://azure.archive.ubuntu.com/ubuntu/" in run, job
    assert "https://archive.ubuntu.com/ubuntu/" in run, job
    assert "https://security.ubuntu.com/ubuntu/" in run, job
    assert "priority:1" in run, job
    assert "priority:4" in run, job
    assert "sudo apt-get update" in run, job
    assert "sudo apt-get install -y protobuf-compiler" in run, job
    (shell_dir / f"{job}.sh").write_text(run)

print(f"parsed jobs: {', '.join(sorted(actual_jobs))}")
PY

for script in "$evidence_dir"/shell/*.sh; do
  bash -n "$script"
  printf 'bash -n passed: %s\n' "$(basename "$script")"
done
