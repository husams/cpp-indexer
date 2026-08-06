#!/usr/bin/env python3
"""End-to-end regression for the cpp-indexer Backlog Done gate."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


class Harness:
    def __init__(self, backlog: Path, source_root: Path, root: Path) -> None:
        self.backlog = backlog
        self.source_root = source_root
        self.root = root
        ctest = shutil.which("ctest")
        if ctest is None:
            raise RuntimeError("ctest executable not found")
        self.ctest = Path(ctest).resolve()
        self.environment = os.environ.copy()
        self.environment["BACKLOG_DB"] = "sqlite"
        self.environment["BACK_LOG_URL"] = (
            f"sqlite:///{root.parent / 'backlog-store.db'}"
        )
        self.environment["BACKLOG_PROJECT"] = "cpp-indexer"
        self.environment.pop("BACKLOG_SCHEMA", None)
        self.environment.pop("BACKLOG_PROJECT_ROOT", None)
        self.environment.pop("BACKLOG_DELIVERY_REVISION", None)
        self.environment.pop("BACKLOG_DELIVERY_PR_URL", None)
        self.environment.pop("CIDX_BACKLOG_DELIVERY_VALIDATION_TASK", None)

    def run(
        self,
        *arguments: str,
        expected: int = 0,
        extra_environment: dict[str, str] | None = None,
    ) -> str:
        environment = self.environment.copy()
        if extra_environment:
            environment.update(extra_environment)
        completed = subprocess.run(
            [str(self.backlog), *arguments],
            cwd=self.root,
            env=environment,
            capture_output=True,
            text=True,
            timeout=120,
        )
        output = completed.stdout + completed.stderr
        if completed.returncode != expected:
            raise AssertionError(
                f"backlog {' '.join(arguments)} returned {completed.returncode}, "
                f"expected {expected}:\n{output}"
            )
        return output

    def git(self, *arguments: str) -> str:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=self.root,
            check=True,
            capture_output=True,
            text=True,
        )
        return completed.stdout.strip()

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-m", message)
        return self.git("rev-parse", "HEAD")

    def write_policy(self, commands: list[str]) -> None:
        command_lines = "\n".join(f"  - {command}" for command in commands)
        (self.root / ".backlog" / "execution.yaml").write_text(
            "shell_enabled: true\n"
            "allowed_commands:\n"
            f"{command_lines}\n"
            "allowed_working_directories:\n"
            "  - .\n"
            "max_timeout_seconds: 30\n"
            "max_output_bytes: 1048576\n"
            "max_batch_seconds: 120\n",
            encoding="utf-8",
        )

    def write_behavior(self, first: int, second: int, third: int) -> None:
        (self.root / "behavior.txt").write_text(
            f"{first} {second} {third}\n", encoding="utf-8"
        )

    def create_story(
        self,
        title: str,
        commands: list[tuple[str, int]] | None,
    ) -> tuple[str, list[int]]:
        if commands:
            command, timeout = commands[0]
            output = self.run(
                "story",
                "add",
                "--title",
                title,
                "--actor",
                "planner",
                "--ac",
                "required delivery check 1",
                "--shell",
                command,
                "--timeout",
                str(timeout),
            )
        else:
            output = self.run(
                "story",
                "add",
                "--title",
                title,
                "--actor",
                "planner",
                "--ac",
                "plain acceptance evidence",
            )
        match = re.search(r"\bS-\d+\b", output)
        if match is None:
            raise AssertionError(f"story key missing from output:\n{output}")
        key = match.group(0)

        for index, (command, timeout) in enumerate(commands[1:] if commands else [], 2):
            self.run(
                "item",
                "add",
                key,
                "--kind",
                "acceptance_criteria",
                "--content",
                f"required delivery check {index}",
                "--shell",
                command,
                "--timeout",
                str(timeout),
            )

        self.run("assign", key, "--to", "developer", "--reviewer", "reviewer")
        self.run("action", key, "refinement.accepted", "--actor", "reviewer")
        self.run("action", key, "work.started", "--actor", "developer")
        self.run(
            "pr",
            "set",
            key,
            "--url",
            f"https://example.invalid/pulls/{key}",
            "--state",
            "open",
            "--actor",
            "developer",
        )

        item_output = self.run("item", "list", key, "--kind", "acceptance_criteria")
        item_ids = [int(value) for value in re.findall(r"#(\d+)", item_output)]
        if commands:
            self.run(
                "validation",
                "run-all",
                key,
                "--project-root",
                str(self.root),
                "--actor",
                "developer",
            )
        # Every acceptance criterion is subject to the
        # acceptance_criteria_verified gate, whether or not it declares an
        # executable check, and criteria are proven by an independent
        # reviewer's verdict rather than by a tick. Running the validations
        # above is the evidence; recording the verdict is what the gate reads.
        evidence = (
            "validation run-all passed" if commands else "reviewed by hand: no "
            "executable check declared"
        )
        for item_id in item_ids:
            self.run(
                "criteria",
                "verify",
                str(item_id),
                "--met",
                "--evidence",
                evidence,
                "--actor",
                "reviewer",
            )
        self.run(
            "pr",
            "set",
            key,
            "--review-state",
            "approved",
            "--actor",
            "reviewer",
        )
        self.assert_status(key, "Accepted")
        return key, item_ids

    def merge(self, key: str, revision: str, *, expected: int = 0) -> str:
        pull_request_url = f"https://example.invalid/pulls/{key}"
        return self.run(
            "pr",
            "set",
            key,
            "--state",
            "merged",
            "--actor",
            "github-actions",
            expected=expected,
            extra_environment={
                "BACKLOG_PROJECT_ROOT": str(self.root),
                "BACKLOG_DELIVERY_REVISION": revision,
                "BACKLOG_DELIVERY_PR_URL": pull_request_url,
                # A caller-controlled legacy marker must not suppress check.*
                # transitions; the hook now uses a process-local marker.
                "CIDX_BACKLOG_DELIVERY_VALIDATION_TASK": key,
            },
        )

    def assert_status(self, key: str, expected: str) -> str:
        output = self.run("show", key)
        if f"status     : {expected}" not in output:
            raise AssertionError(f"{key} was not {expected}:\n{output}")
        return output

    def task_data(self, key: str) -> dict[str, object]:
        return json.loads(self.run("show", key, "--json"))


def prepare(harness: Harness) -> str:
    root = harness.root
    root.mkdir(parents=True)
    harness.git("init")
    harness.git("config", "user.name", "Backlog E2E")
    harness.git("config", "user.email", "backlog-e2e@example.invalid")
    harness.run("init", ".")

    source_backlog = harness.source_root / ".backlog"
    tracked = subprocess.run(
        [
            "git",
            "-C",
            str(harness.source_root),
            "ls-files",
            "--error-unmatch",
            ".backlog/execution.yaml",
            ".backlog/hooks/__init__.py",
            ".backlog/hooks/delivery_validation.py",
        ],
        capture_output=True,
        text=True,
    )
    if tracked.returncode != 0:
        raise AssertionError(
            "Done-validation policy/hooks must be tracked by git:\n" + tracked.stderr
        )
    (root / ".backlog").mkdir(exist_ok=True)
    shutil.copy2(source_backlog / "workflow.yaml", root / ".backlog" / "workflow.yaml")
    shutil.copy2(source_backlog / ".gitignore", root / ".backlog" / ".gitignore")
    shutil.copytree(
        source_backlog / "hooks", root / ".backlog" / "hooks", dirs_exist_ok=True
    )
    validation_command = root / "validation-command"
    harness.write_policy(["ctest", str(harness.ctest), str(validation_command)])
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\nproject(cpp_indexer_gate NONE)\n",
        encoding="utf-8",
    )
    (root / "check_behavior.py").write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "values = [int(value) for value in Path('behavior.txt').read_text().split()]\n"
        "index = int(sys.argv[1])\n"
        "raise SystemExit(values[index])\n",
        encoding="utf-8",
    )
    (root / "check_slow.py").write_text(
        "from pathlib import Path\n"
        "import time\n"
        "time.sleep(float(Path('slow-seconds.txt').read_text()))\n",
        encoding="utf-8",
    )
    (root / "slow-seconds.txt").write_text("0\n", encoding="utf-8")
    validation_command.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    validation_command.chmod(0o755)
    (root / "CTestTestfile.cmake").write_text(
        'add_test(first "python3" "check_behavior.py" "0")\n'
        'add_test(second "python3" "check_behavior.py" "1")\n'
        'add_test(third "python3" "check_behavior.py" "2")\n'
        'add_test(slow "python3" "check_slow.py")\n',
        encoding="utf-8",
    )
    (root / ".gitignore").write_text("Testing/\n", encoding="utf-8")
    harness.write_behavior(0, 0, 0)
    return harness.commit("seed exact delivery checkout")


def exercise(harness: Harness, initial_revision: str) -> None:
    no_checks, _ = harness.create_story("No executable criteria", None)
    harness.run(
        "pr",
        "set",
        no_checks,
        "--state",
        "merged",
        "--actor",
        "github-actions",
    )
    harness.assert_status(no_checks, "Done")

    all_pass, _ = harness.create_story(
        "All required checks pass",
        [(f"{harness.ctest} -R first --output-on-failure", 10)],
    )
    harness.merge(all_pass, initial_revision)
    harness.assert_status(all_pass, "Done")

    missing_root, _ = harness.create_story(
        "Missing project root",
        [(f"{harness.ctest} -R first --output-on-failure", 10)],
    )
    missing_output = harness.run(
        "pr",
        "set",
        missing_root,
        "--state",
        "merged",
        "--actor",
        "github-actions",
        expected=1,
    )
    if "validation_environment_unavailable" not in missing_output:
        raise AssertionError(missing_output)
    harness.assert_status(missing_root, "Accepted")

    mismatch, _ = harness.create_story(
        "Mismatched revision",
        [(f"{harness.ctest} -R first --output-on-failure", 10)],
    )
    (harness.root / "revision-marker.txt").write_text("new head\n", encoding="utf-8")
    harness.commit("advance beyond the asserted delivery revision")
    mismatch_output = harness.merge(mismatch, initial_revision, expected=1)
    if "validation_environment_unavailable" not in mismatch_output:
        raise AssertionError(mismatch_output)
    harness.assert_status(mismatch, "Accepted")

    mismatched_pr, _ = harness.create_story(
        "Mismatched pull request",
        [(f"{harness.ctest} -R first --output-on-failure", 10)],
    )
    mismatched_pr_output = harness.run(
        "pr",
        "set",
        mismatched_pr,
        "--state",
        "merged",
        "--actor",
        "github-actions",
        expected=1,
        extra_environment={
            "BACKLOG_PROJECT_ROOT": str(harness.root),
            "BACKLOG_DELIVERY_REVISION": harness.git("rev-parse", "HEAD"),
            "BACKLOG_DELIVERY_PR_URL": "https://example.invalid/pulls/wrong",
        },
    )
    if "does not match the task PR" not in mismatched_pr_output:
        raise AssertionError(mismatched_pr_output)
    harness.assert_status(mismatched_pr, "Accepted")

    missing_policy, _ = harness.create_story(
        "Missing policy",
        [(f"{harness.ctest} -R first --output-on-failure", 10)],
    )
    no_checks_without_policy, _ = harness.create_story(
        "No checks do not require a policy", None
    )
    policy = harness.root / ".backlog" / "execution.yaml"
    saved_policy = policy.read_text(encoding="utf-8")
    policy.unlink()
    no_policy_revision = harness.commit("remove trusted execution policy")
    harness.merge(no_checks_without_policy, no_policy_revision)
    harness.assert_status(no_checks_without_policy, "Done")
    policy_output = harness.merge(missing_policy, no_policy_revision, expected=1)
    if "validation_policy_denied" not in policy_output:
        raise AssertionError(policy_output)
    harness.assert_status(missing_policy, "Accepted")
    policy.write_text(saved_policy, encoding="utf-8")
    harness.commit("restore trusted execution policy")

    python = Path(sys.executable).resolve()
    harness.write_policy(
        [
            "ctest",
            str(harness.ctest),
            str(harness.root / "validation-command"),
            str(python),
        ]
    )
    harness.commit("temporarily permit the denied-executor fixture")
    denied, _ = harness.create_story(
        "Denied executor",
        [(f"{python} check_behavior.py 0", 10)],
    )
    harness.write_policy(
        ["ctest", str(harness.ctest), str(harness.root / "validation-command")]
    )
    denied_revision = harness.commit("deny the fixture executor")
    denied_output = harness.merge(denied, denied_revision, expected=1)
    if "validation_policy_denied" not in denied_output:
        raise AssertionError(denied_output)
    harness.assert_status(denied, "Accepted")

    timeout_story, _ = harness.create_story(
        "Timed out required check",
        [(f"{harness.ctest} -R slow --output-on-failure", 1)],
    )
    (harness.root / "slow-seconds.txt").write_text("5\n", encoding="utf-8")
    timeout_revision = harness.commit("make the accepted check time out")
    timeout_output = harness.merge(timeout_story, timeout_revision, expected=1)
    if "check_timeout" not in timeout_output:
        raise AssertionError(timeout_output)
    harness.assert_status(timeout_story, "Accepted")
    (harness.root / "slow-seconds.txt").write_text("0\n", encoding="utf-8")
    harness.commit("restore fast validation behavior")

    unavailable_command = harness.root / "validation-command"
    error_story, _ = harness.create_story(
        "Errored required check", [(str(unavailable_command), 10)]
    )
    unavailable_command.unlink()
    error_revision = harness.commit("remove the accepted validation command")
    error_output = harness.merge(error_story, error_revision, expected=1)
    if "check_error" not in error_output:
        raise AssertionError(error_output)
    harness.assert_status(error_story, "Accepted")
    unavailable_command.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    unavailable_command.chmod(0o755)
    harness.commit("restore the validation command")

    multiple, item_ids = harness.create_story(
        "Aggregate every required failure",
        [
            (f"{harness.ctest} -R first --output-on-failure", 10),
            (f"{harness.ctest} -R second --output-on-failure", 10),
            (f"{harness.ctest} -R third --output-on-failure", 10),
        ],
    )
    accepted_closed_at = harness.task_data(multiple)["closed_at"]
    harness.write_behavior(1, 0, 1)
    failing_revision = harness.commit("revert two validated behaviors")
    failure_output = harness.merge(multiple, failing_revision, expected=1)
    if failure_output.count("check_failed") < 2:
        raise AssertionError(failure_output)
    harness.assert_status(multiple, "Accepted")
    if len(item_ids) != 3:
        raise AssertionError(f"expected three item ids, got {item_ids}")
    first_position = failure_output.find(f'"item_id":{item_ids[0]}')
    third_position = failure_output.find(f'"item_id":{item_ids[2]}')
    if first_position < 0 or third_position <= first_position:
        raise AssertionError(f"failure order was not stable:\n{failure_output}")
    refused_task = harness.task_data(multiple)
    if refused_task["closed_at"] != accepted_closed_at:
        raise AssertionError(
            "refusal changed closed_at: "
            f"before={accepted_closed_at} after={refused_task['closed_at']}"
        )
    refusal_notes = [
        item
        for item in refused_task["items"]
        if item["kind"] == "note"
        and str(item["content"]).startswith("validation.refused ")
    ]
    if len(refusal_notes) != 1 or refusal_notes[0]["created_by"] != "github-actions":
        raise AssertionError(f"unexpected refusal audit records: {refusal_notes}")
    refusal_history = harness.run("history", multiple)
    if re.search(r"\bstatus\b.*\baccepted\b.*\bdone\b", refusal_history):
        raise AssertionError(
            f"a refused Done status event was retained:\n{refusal_history}"
        )
    if re.search(r"\bstatus\b.*\bneeds_work\b", refusal_history):
        raise AssertionError(
            f"a state-changing check action was retained:\n{refusal_history}"
        )
    later_history = harness.run(
        "validation",
        "history",
        str(item_ids[2]),
        "--limit",
        "1",
        "--project-root",
        str(harness.root),
    )
    if "fail" not in later_history.lower():
        raise AssertionError(later_history)
    if "github-actions" not in later_history:
        raise AssertionError(f"failure result actor was not retained:\n{later_history}")

    harness.write_behavior(0, 0, 0)
    repaired_revision = harness.commit("restore validated behavior")
    harness.run(
        "action",
        multiple,
        "pr.merged",
        "--actor",
        "github-actions",
        "--parameter",
        f"project_root={harness.root}",
        "--parameter",
        f"delivery_revision={repaired_revision}",
        "--parameter",
        f"pull_request_url=https://example.invalid/pulls/{multiple}",
    )
    harness.assert_status(multiple, "Done")
    completion_history = harness.run("history", multiple)
    if (
        "check.passed" not in completion_history
        or "github-actions" not in completion_history
    ):
        raise AssertionError(completion_history)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--backlog", type=Path)
    arguments = parser.parse_args()
    backlog = arguments.backlog
    if backlog is None:
        configured = os.environ.get("BACKLOG_BIN")
        found = configured or shutil.which("backlog")
        if not found:
            print("backlog executable not found; skipping integration test")
            return 77
        backlog = Path(found)
    backlog = backlog.resolve()

    with tempfile.TemporaryDirectory(prefix="cidx-backlog-e2e-") as directory:
        root = Path(directory) / "cpp-indexer"
        harness = Harness(backlog, arguments.source_root.resolve(), root)
        initial_revision = prepare(harness)
        exercise(harness, initial_revision)
    print("backlog Done validation E2E passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
