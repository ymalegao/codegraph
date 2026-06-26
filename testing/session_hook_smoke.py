#!/usr/bin/env python3
"""Integration smoke test for the shared Codex/Claude lifecycle hook."""

from __future__ import annotations

import json
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / "hooks" / "codegraph_session.py"


def run(action: str, event: dict[str, Any]) -> dict[str, Any]:
    result = subprocess.run(
        [sys.executable, str(HOOK), action],
        cwd=ROOT,
        input=json.dumps(event),
        capture_output=True,
        text=True,
        check=True,
        timeout=90,
    )
    return json.loads(result.stdout or "{}")


def base_event(session_id: str, hook_event_name: str) -> dict[str, Any]:
    return {
        "session_id": session_id,
        "cwd": str(ROOT),
        "hook_event_name": hook_event_name,
    }


def main() -> int:
    session_id = f"smoke-{uuid.uuid4().hex}"

    started = run(
        "session-start",
        {
            **base_event(session_id, "SessionStart"),
            "source": "startup",
        },
    )
    context = started["hookSpecificOutput"]["additionalContext"]
    assert "CodeGraph verified resume context" in context
    assert "automatic resume failed" not in context.lower()

    run(
        "post-tool",
        {
            **base_event(session_id, "PostToolUse"),
            "tool_name": "mcp__codegraph__record_decision",
        },
    )

    compact = run(
        "pre-compact",
        {
            **base_event(session_id, "PreCompact"),
            "trigger": "manual",
        },
    )
    assert compact["continue"] is False

    stopped = run(
        "stop",
        {
            **base_event(session_id, "Stop"),
            "stop_hook_active": False,
        },
    )
    assert stopped["decision"] == "block"
    assert "write_handoff" in stopped["reason"]

    run(
        "post-tool",
        {
            **base_event(session_id, "PostToolUse"),
            "tool_name": "mcp__codegraph__write_handoff",
        },
    )
    assert run(
        "stop",
        {
            **base_event(session_id, "Stop"),
            "stop_hook_active": False,
        },
    ) == {}

    print("session hook resume: ok")
    print("session hook dirty/stop: ok")
    print("session hook handoff clears state: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
