#!/usr/bin/env python3
import pathlib
import re
import sys


FORBIDDEN_PATTERNS = (
    re.compile(r"\bglDrawArrays\s*\("),
    re.compile(r"\bglDrawElements\s*\("),
    re.compile(r"\bGL_DrawArraysInstancedFunc\b"),
    re.compile(r"\bGL_DrawElementsInstancedFunc\b"),
    re.compile(r"\bGL_DrawElementsIndirectFunc\b"),
    re.compile(r"\bGL_MultiDrawElementsIndirectFunc\b"),
)

SCAN_ROOTS = (
    pathlib.Path("Quake/src"),
)


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    allow_files = {
        (repo_root / "Quake" / "src" / "render" / "gl_backend.c").resolve(),
    }
    failures = []

    for scan_root in SCAN_ROOTS:
        root = repo_root / scan_root
        if not root.is_dir():
            continue

        for path in root.rglob("*.c"):
            full = path.resolve()
            if full in allow_files:
                continue

            text = full.read_text(encoding="utf-8")
            lines = text.splitlines()
            for line_no, line in enumerate(lines, start=1):
                for pattern in FORBIDDEN_PATTERNS:
                    if pattern.search(line):
                        failures.append((str(path.relative_to(repo_root)).replace("\\", "/"), line_no, line.strip()))
                        break

    if failures:
        print("Direct GL draw call check failed:")
        for rel_path, line_no, line in failures:
            print(f"  {rel_path}:{line_no}: {line}")
        print("Route draw-path calls through R_Backend_Draw* wrappers.")
        return 1

    print("Direct GL draw call check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
