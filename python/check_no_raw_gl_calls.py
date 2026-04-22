#!/usr/bin/env python3
import pathlib
import re
import sys


RAW_GL_CALL_RE = re.compile(r"\bgl[A-Z][A-Za-z0-9_]*\b")

# Phase 2 guardrails: keep raw GL calls out of non-backend renderer files.
ENFORCED_FILES = (
    pathlib.Path("Quake/src/render/r_world.c"),
    pathlib.Path("Quake/src/render/r_postfx.c"),
)


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    failures = []

    for rel_path in ENFORCED_FILES:
        file_path = repo_root / rel_path
        if not file_path.is_file():
            failures.append((str(rel_path), 0, "missing file"))
            continue

        for line_no, line in enumerate(file_path.read_text(encoding="utf-8").splitlines(), start=1):
            if RAW_GL_CALL_RE.search(line):
                failures.append((str(rel_path), line_no, line.strip()))

    if failures:
        print("Raw GL symbol check failed:")
        for rel_path, line_no, line in failures:
            print(f"  {rel_path}:{line_no}: {line}")
        print("Use backend wrappers/API seams instead of direct gl* entrypoints in enforced files.")
        return 1

    print("Raw GL symbol check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
