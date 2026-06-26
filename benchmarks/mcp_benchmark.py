#!/usr/bin/env python3
"""Reproducible protocol-level benchmark for the CodeGraph MCP server."""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import statistics
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


EXPECTED_TOOLS = {
    "find_symbol",
    "read_symbol",
    "read_enclosing_symbol",
    "list_symbols_in_file",
    "read_file_range",
    "get_memory_for_file",
    "get_memory_for_symbol",
    "search_symbol",
    "find_prior_incidents",
    "record_correction",
    "record_decision",
    "write_handoff",
    "resume_from_handoff",
}


class McpClient:
    def __init__(self, binary: Path, repo: Path) -> None:
        self._next_id = 1
        self._process = subprocess.Popen(
            [str(binary), "mcp", str(repo)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )

    def close(self) -> None:
        if self._process.stdin:
            self._process.stdin.close()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.terminate()
            self._process.wait(timeout=5)

    def request(self, method: str, params: dict[str, Any] | None = None) -> tuple[dict[str, Any], str, float]:
        if not self._process.stdin or not self._process.stdout:
            raise RuntimeError("MCP process streams are unavailable")
        request_id = self._next_id
        self._next_id += 1
        message: dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
        }
        if params is not None:
            message["params"] = params

        started = time.perf_counter_ns()
        self._process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self._process.stdin.flush()
        raw = self._process.stdout.readline()
        elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0

        if not raw:
            return_code = self._process.poll()
            raise RuntimeError(f"MCP server closed stdout (exit={return_code})")
        response = json.loads(raw)
        if response.get("id") != request_id:
            raise RuntimeError(
                f"MCP response id mismatch: expected {request_id}, got {response.get('id')}"
            )
        return response, raw, elapsed_ms

    def call_tool(self, name: str, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, float, str]:
        response, raw, elapsed_ms = self.request(
            "tools/call",
            {"name": name, "arguments": arguments},
        )
        if "error" in response:
            raise RuntimeError(f"{name} protocol error: {response['error']}")
        result = response.get("result", {})
        if result.get("isError"):
            raise RuntimeError(f"{name} tool error: {result}")
        content = result.get("content", [])
        if not content or content[0].get("type") != "text":
            raise RuntimeError(f"{name} returned no text content")
        text = content[0].get("text", "")
        json.loads(text)
        return response, raw, elapsed_ms, text


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def summarize(samples: list[dict[str, float]], chars_per_token: float) -> dict[str, Any]:
    latencies = [sample["latency_ms"] for sample in samples]
    response_bytes = [sample["response_bytes"] for sample in samples]
    content_bytes = [sample["content_bytes"] for sample in samples]
    content_chars = [sample["content_chars"] for sample in samples]
    estimated_tokens = [
        math.ceil(sample["content_chars"] / chars_per_token) for sample in samples
    ]
    return {
        "calls": len(samples),
        "failures": 0,
        "latency_ms": {
            "mean": round(statistics.fmean(latencies), 3),
            "p50": round(percentile(latencies, 0.50), 3),
            "p95": round(percentile(latencies, 0.95), 3),
            "max": round(max(latencies), 3),
        },
        "response_bytes": {
            "mean": round(statistics.fmean(response_bytes), 1),
            "max": int(max(response_bytes)),
        },
        "content_bytes": {
            "mean": round(statistics.fmean(content_bytes), 1),
            "max": int(max(content_bytes)),
        },
        "estimated_content_tokens": {
            "method": f"ceil(content_characters/{chars_per_token:g})",
            "mean": round(statistics.fmean(estimated_tokens), 1),
            "max": int(max(estimated_tokens)),
        },
    }


def create_fixture(repo: Path) -> None:
    (repo / "src").mkdir(parents=True)
    (repo / "docs").mkdir(parents=True)
    (repo / "src" / "target.cpp").write_text(
        "#include <string>\n"
        "namespace benchmark {\n"
        "int target(int value) {\n"
        "    return value + 1;\n"
        "}\n"
        "namespace {\n"
        "std::string helper() {\n"
        '    return "benchmark helper";\n'
        "}\n"
        "}\n"
        "}\n",
        encoding="utf-8",
    )
    (repo / "docs" / "NOTES.md").write_text(
        "# Benchmark fixture\n\nThis file tests lightweight text-file nodes.\n",
        encoding="utf-8",
    )


def record_sample(
    client: McpClient,
    tool: str,
    arguments: dict[str, Any],
) -> dict[str, float]:
    _, raw, latency_ms, content = client.call_tool(tool, arguments)
    return {
        "latency_ms": latency_ms,
        "response_bytes": float(len(raw.encode("utf-8"))),
        "content_bytes": float(len(content.encode("utf-8"))),
        "content_chars": float(len(content)),
    }


def run_benchmark(args: argparse.Namespace) -> dict[str, Any]:
    binary = args.binary.resolve()
    if not binary.is_file():
        raise RuntimeError(f"CodeGraph binary not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="codegraph-mcp-bench-") as temp_dir:
        repo = Path(temp_dir)
        create_fixture(repo)
        subprocess.run(
            [str(binary), "init", str(repo)],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        client = McpClient(binary, repo)
        try:
            initialized, _, _ = client.request(
                "initialize",
                {"protocolVersion": "2025-06-18"},
            )
            if "error" in initialized:
                raise RuntimeError(f"MCP initialize failed: {initialized['error']}")
            listed, _, _ = client.request("tools/list")
            advertised = {
                tool["name"] for tool in listed.get("result", {}).get("tools", [])
            }
            missing = EXPECTED_TOOLS - advertised
            if missing:
                raise RuntimeError(f"MCP server is missing tools: {sorted(missing)}")

            setup_calls = [
                (
                    "record_correction",
                    {
                        "reason": "benchmark correction incident",
                        "affects": ["src/target.cpp"],
                        "prefer_paths": ["src/**"],
                    },
                ),
                (
                    "record_decision",
                    {
                        "title": "Benchmark decision",
                        "body": "benchmark architecture decision",
                        "affects": ["docs/NOTES.md"],
                    },
                ),
                (
                    "write_handoff",
                    {
                        "title": "Benchmark handoff",
                        "body": "benchmark handoff context",
                        "affects": ["src/target.cpp"],
                    },
                ),
            ]
            setup_metrics: dict[str, Any] = {}
            for tool, arguments in setup_calls:
                sample = record_sample(client, tool, arguments)
                setup_metrics[tool] = summarize([sample], args.chars_per_token)

            scenarios = [
                ("find_symbol", "find_symbol", {"name": "benchmark::target", "limit": 20}),
                (
                    "read_symbol",
                    "read_symbol",
                    {"query": "benchmark::target", "include_memory": False},
                ),
                (
                    "read_symbol_with_memory",
                    "read_symbol",
                    {"query": "benchmark::target", "include_memory": True},
                ),
                (
                    "read_enclosing_symbol",
                    "read_enclosing_symbol",
                    {"path": "src/target.cpp", "line": 4},
                ),
                (
                    "list_symbols_in_file",
                    "list_symbols_in_file",
                    {"path": "src/target.cpp"},
                ),
                (
                    "read_file_range",
                    "read_file_range",
                    {"path": "src/target.cpp", "start_line": 1, "end_line": 8},
                ),
                (
                    "get_memory_for_file",
                    "get_memory_for_file",
                    {"path": "src/target.cpp"},
                ),
                (
                    "get_memory_for_symbol",
                    "get_memory_for_symbol",
                    {"query": "benchmark::target"},
                ),
                (
                    "search_symbol",
                    "search_symbol",
                    {"query": "benchmark target", "limit": 20},
                ),
                (
                    "find_prior_incidents",
                    "find_prior_incidents",
                    {"query": "benchmark correction", "limit": 10},
                ),
                ("resume_from_handoff", "resume_from_handoff", {}),
            ]

            scenario_metrics: dict[str, Any] = {}
            total_calls = len(setup_calls)
            for scenario_name, tool, arguments in scenarios:
                for _ in range(args.warmup):
                    record_sample(client, tool, arguments)
                samples = [
                    record_sample(client, tool, arguments)
                    for _ in range(args.repetitions)
                ]
                scenario_metrics[scenario_name] = {
                    "tool": tool,
                    **summarize(samples, args.chars_per_token),
                }
                total_calls += len(samples)

            return {
                "benchmark": "codegraph_mcp",
                "generated_at": datetime.now(timezone.utc).isoformat(),
                "binary": str(binary),
                "codegraph_version": subprocess.check_output(
                    [str(binary), "--version"], text=True
                ).strip(),
                "platform": platform.platform(),
                "python": platform.python_version(),
                "configuration": {
                    "fixture": "disposable",
                    "repetitions": args.repetitions,
                    "warmup_calls_per_read_scenario": args.warmup,
                    "chars_per_token_estimate": args.chars_per_token,
                },
                "reliability": {
                    "advertised_tools": sorted(advertised),
                    "expected_tools_present": True,
                    "measured_calls": total_calls,
                    "failed_calls": 0,
                    "success_rate": 1.0,
                },
                "setup_write_tools": setup_metrics,
                "read_scenarios": scenario_metrics,
            }
        finally:
            client.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=Path("build/codegraph"),
        help="path to the codegraph executable (default: build/codegraph)",
    )
    parser.add_argument(
        "--repetitions",
        type=int,
        default=20,
        help="measured calls per read scenario (default: 20)",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=2,
        help="unmeasured warmup calls per read scenario (default: 2)",
    )
    parser.add_argument(
        "--chars-per-token",
        type=float,
        default=4.0,
        help="token estimate divisor; raw byte counts are always retained (default: 4)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/mcp-benchmark.json"),
        help="JSON report path (default: build/mcp-benchmark.json)",
    )
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be at least 1")
    if args.warmup < 0:
        parser.error("--warmup cannot be negative")
    if args.chars_per_token <= 0:
        parser.error("--chars-per-token must be positive")
    return args


def main() -> int:
    args = parse_args()
    report = run_benchmark(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(f"report: {args.output.resolve()}")
    print(
        "reliability: "
        f"{report['reliability']['measured_calls']} calls, "
        f"{report['reliability']['failed_calls']} failures"
    )
    for name, metrics in report["read_scenarios"].items():
        print(
            f"{name}: p50={metrics['latency_ms']['p50']:.3f}ms "
            f"p95={metrics['latency_ms']['p95']:.3f}ms "
            f"mean_tokens~={metrics['estimated_content_tokens']['mean']:.1f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
