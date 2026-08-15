#!/usr/bin/env python3
"""Extract the embedded PTMC Metal source so CI can compile the exact runtime kernel."""

from __future__ import annotations

import ast
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "PlayTools/MetalCapture/PTMetalCapture.inc").read_text()
marker = "static const char *ptmc_shader_source ="
try:
    body = source.split(marker, 1)[1].split("\n;\n", 1)[0]
except IndexError as exc:
    raise SystemExit("unable to locate ptmc_shader_source") from exc

parts: list[str] = []
for raw_line in body.splitlines():
    line = raw_line.strip()
    if not line.startswith('"'):
        continue
    try:
        parts.append(ast.literal_eval(line))
    except (SyntaxError, ValueError) as exc:
        raise SystemExit(f"invalid embedded C string: {line}") from exc

print("".join(parts), end="")
