#!/usr/bin/env python3
"""Shared Codex/Claude Code lifecycle hooks for verified session continuity."""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


HANDOFF_TOOL = re.compile(r"(?:^|[._])write_handoff$")
MEMORY_WRITE_TOOL = re.compile(
    r"(?:^|[._])(record_correction|record_decision|write_handoff)$"
)


def read_event() -> dict[str, Any]:
    try:
        return json.load(sys.stdin)
    except Exception:
        return {}


def repository_root(event: dict[str, Any]) -> Path:
    cwd = Path(event.get("cwd") or os.getcwd())
    result = subprocess.run(
        ["git", "-C", str(cwd), "rev-parse", "--show-toplevel"],
        check=True,
        capture_output=True,
        text=True,
    )
    return Path(result.stdout.strip())


def codegraph_binary(root: Path) -> Path:
    candidates = []
    for relative in (
        "build/codegraph",
        "build-ninja/codegraph",
        "build-lifecycle/codegraph",
    ):
        candidate = root / relative
        if candidate.is_file() and os.access(candidate, os.X_OK):
            candidates.append(candidate)
    if candidates:
        return max(candidates, key=lambda path: path.stat().st_mtime_ns)
    raise RuntimeError("CodeGraph binary not found in build/ or build-ninja/")


def state_path(root: Path, event: dict[str, Any]) -> Path:
    session_id = re.sub(r"[^A-Za-z0-9_.-]", "_", str(event.get("session_id", "unknown")))
    state_dir = root / ".codegraph" / "hooks"
    state_dir.mkdir(parents=True, exist_ok=True)
    return state_dir / f"{session_id}.json"


def repository_lock_path(root: Path) -> Path:
    state_dir = root / ".codegraph" / "hooks"
    state_dir.mkdir(parents=True, exist_ok=True)
    return state_dir / "repository.lock"


def load_state(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_state(path: Path, state: dict[str, Any]) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, sort_keys=True), encoding="utf-8")
    temporary.replace(path)


def worktree_fingerprint(root: Path) -> str:
    digest = hashlib.sha256()
    diff = subprocess.run(
        ["git", "-C", str(root), "diff", "--binary", "HEAD", "--", "."],
        check=True,
        capture_output=True,
    ).stdout
    digest.update(diff)

    untracked = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        check=True,
        capture_output=True,
    ).stdout.split(b"\0")
    for raw_path in sorted(path for path in untracked if path):
        digest.update(raw_path)
        path = root / raw_path.decode("utf-8", errors="surrogateescape")
        if path.is_file():
            digest.update(path.read_bytes())
    return digest.hexdigest()


def changed_paths(root: Path) -> list[str]:
    output = subprocess.run(
        ["git", "-C", str(root), "status", "--short"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line[3:] for line in output.splitlines() if len(line) > 3]


def hook_output(event_name: str, context: str) -> None:
    print(
        json.dumps(
            {
                "hookSpecificOutput": {
                    "hookEventName": event_name,
                    "additionalContext": context,
                }
            }
        )
    )


def run_codegraph_with_retry(
    arguments: list[str],
    *,
    attempts: int = 4,
    timeout: int = 55,
) -> subprocess.CompletedProcess[str]:
    result: subprocess.CompletedProcess[str] | None = None
    for attempt in range(attempts):
        result = subprocess.run(
            arguments,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        combined = f"{result.stdout}\n{result.stderr}".lower()
        if result.returncode == 0 or "database is locked" not in combined:
            return result
        time.sleep(0.25 * (2**attempt))
    assert result is not None
    return result


def session_start(root: Path, binary: Path, event: dict[str, Any], state: dict[str, Any]) -> None:
    state.update(
        {
            "baseline_fingerprint": worktree_fingerprint(root),
            "dirty": False,
            "stop_prompted": False,
        }
    )
    with repository_lock_path(root).open("w", encoding="utf-8") as repository_lock:
        fcntl.flock(repository_lock, fcntl.LOCK_EX)
        result = run_codegraph_with_retry(
            [str(binary), "resume-context", str(root)],
            timeout=55,
        )
    if result.returncode != 0:
        context = (
            "CodeGraph automatic resume failed. Do not treat this as an empty "
            f"handoff. Error: {result.stderr.strip() or result.stdout.strip()}"
        )
    else:
        context = result.stdout.strip()
    hook_output("SessionStart", context)


def post_tool(root: Path, binary: Path, event: dict[str, Any], state: dict[str, Any]) -> None:
    tool_name = str(event.get("tool_name", ""))
    current = worktree_fingerprint(root)

    if HANDOFF_TOOL.search(tool_name):
        state.update(
            {
                "baseline_fingerprint": current,
                "dirty": False,
                "stop_prompted": False,
            }
        )
        return

    baseline = state.get("baseline_fingerprint", current)
    memory_changed = MEMORY_WRITE_TOOL.search(tool_name) is not None
    if current != baseline or memory_changed:
        state["dirty"] = True
        state["changed_paths"] = changed_paths(root)
        state["stop_prompted"] = False
        log_path = root / ".codegraph" / "logs" / "hook.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with repository_lock_path(root).open("w", encoding="utf-8") as repository_lock:
            fcntl.flock(repository_lock, fcntl.LOCK_EX)
            result = run_codegraph_with_retry(
                [str(binary), "index", str(root)],
                timeout=55,
            )
        with log_path.open("a", encoding="utf-8") as log:
            log.write(result.stdout)
            log.write(result.stderr)


def pre_compact(event: dict[str, Any], state: dict[str, Any]) -> None:
    if not state.get("dirty"):
        return
    if event.get("trigger") == "manual":
        print(
            json.dumps(
                {
                    "continue": False,
                    "stopReason": (
                        "Write a CodeGraph handoff for the current work before "
                        "manual compaction."
                    ),
                }
            )
        )
    else:
        print(
            json.dumps(
                {
                    "systemMessage": (
                        "Automatic compaction occurred with unhanded-off changes. "
                        "Write a CodeGraph handoff at the next safe stopping point."
                    )
                }
            )
        )


def stop(event: dict[str, Any], state: dict[str, Any]) -> None:
    if not state.get("dirty"):
        print("{}")
        return
    if event.get("stop_hook_active") or state.get("stop_prompted"):
        print("{}")
        return

    state["stop_prompted"] = True
    paths = state.get("changed_paths", [])
    path_text = ", ".join(paths[:12]) if paths else "repository state"
    print(
        json.dumps(
            {
                "decision": "block",
                "reason": (
                    "Before stopping, call CodeGraph write_handoff. Summarize the "
                    "current objective and success condition, completed work and "
                    "verification, exact next action, blockers, and unresolved "
                    "questions. Anchor the relevant files and symbols in affects. "
                    f"Detected changed state: {path_text}."
                ),
            }
        )
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: codegraph_session.py session-start|post-tool|pre-compact|stop", file=sys.stderr)
        return 2

    event = read_event()
    root = repository_root(event)
    binary = codegraph_binary(root)
    path = state_path(root, event)
    lock_path = path.with_suffix(".lock")

    with lock_path.open("w", encoding="utf-8") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        state = load_state(path)
        action = sys.argv[1]
        if action == "session-start":
            session_start(root, binary, event, state)
        elif action == "post-tool":
            post_tool(root, binary, event, state)
        elif action == "pre-compact":
            pre_compact(event, state)
        elif action == "stop":
            stop(event, state)
        else:
            raise RuntimeError(f"unknown hook action: {action}")
        save_state(path, state)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"CodeGraph hook failed: {error}", file=sys.stderr)
        raise SystemExit(1)
