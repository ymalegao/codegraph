#!/usr/bin/env python3
"""
Block grep/cat/head/tail on source files. Use CodeGraph MCP tools instead.
Permitted fallback: rg (as a last resort per CLAUDE.md).
"""
import sys
import json
import re

SRC = re.compile(r'\.(cpp|h|c|py|rs|go|ts|tsx|js|jsx|rb|java|cs)\b')
GREP = re.compile(r'\bgrep\b')
RAW_READ = re.compile(r'\b(cat|head|tail)\b')

try:
    data = json.load(sys.stdin)
    cmd = data.get("tool_input", {}).get("command", "")

    if GREP.search(cmd) and SRC.search(cmd):
        print(
            "CodeGraph guard: use MCP tools instead of grep on source files.\n"
            "  Known name      → find_symbol → read_symbol\n"
            "  Keywords/intent → search_symbol → read_symbol\n"
            "  Path + line     → read_enclosing_symbol\n"
            "  Last resort     → rg (not grep), then read_symbol on the hit"
        )
        sys.exit(2)

    if RAW_READ.search(cmd) and SRC.search(cmd):
        print(
            "CodeGraph guard: use MCP tools instead of raw file reads.\n"
            "  Symbol body     → read_symbol\n"
            "  Path + line     → read_enclosing_symbol\n"
            "  Exact range     → read_file_range"
        )
        sys.exit(2)

except Exception:
    pass
