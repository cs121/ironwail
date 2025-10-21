#!/usr/bin/env python3
"""Scan Ironwail shader sources for accidental use of reserved GLSL identifiers."""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterable, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = REPO_ROOT / "Quake" / "shaders"

# Identifiers that generated GLSL compile errors on some drivers when used as
# variable names. We reserve the ability to extend this list in the future.
BANNED_IDENTIFIERS = {"smooth"}
SUPPORTED_EXTENSIONS = {".frag", ".vert", ".comp", ".glsl"}


def _tokenize_identifiers(source: str) -> Iterable[Tuple[str, int]]:
    length = len(source)
    i = 0
    line = 1

    while i < length:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < length else ""

        if ch == "\n":
            line += 1
            i += 1
            continue
        if ch == "/" and nxt == "/":  # line comment
            i += 2
            while i < length and source[i] not in "\r\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":  # block comment
            i += 2
            while i + 1 < length and not (source[i] == "*" and source[i + 1] == "/"):
                if source[i] == "\n":
                    line += 1
                i += 1
            if i + 1 < length:
                i += 2
            continue
        if ch in {'"', "'"}:  # string or character literal
            quote = ch
            i += 1
            while i < length:
                current = source[i]
                if current == "\n":
                    line += 1
                if current == "\\":
                    i += 2
                    continue
                if current == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch.isalpha() or ch == "_":
            start = i
            i += 1
            while i < length and (source[i].isalnum() or source[i] == "_"):
                i += 1
            yield source[start:i], line
            continue

        i += 1


def main() -> int:
    shader_files = [p for p in SHADER_DIR.rglob("*") if p.suffix in SUPPORTED_EXTENSIONS]
    shader_files.sort()

    violations: list[str] = []
    for path in shader_files:
        text = path.read_text(encoding="utf-8")
        for identifier, line in _tokenize_identifiers(text):
            if identifier in BANNED_IDENTIFIERS:
                rel = path.relative_to(REPO_ROOT)
                violations.append(f"{rel}:{line}: banned identifier '{identifier}'")
                break  # report only the first offending token per file

    if violations:
        print("Detected reserved identifiers in shader sources:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("No banned identifiers found in shader sources.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
