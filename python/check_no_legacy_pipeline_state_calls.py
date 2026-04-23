#!/usr/bin/env python3
import pathlib
import re
import sys


LEGACY_CALL_RE = re.compile(r"\bR_Backend_SetPipelineState\s*\(")


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    source_root = repo_root / "Quake" / "src"
    allow_file = (source_root / "render" / "r_backend.c").resolve()
    failures = []

    for path in source_root.rglob("*.c"):
        full = path.resolve()
        text = full.read_text(encoding="utf-8")
        if full == allow_file:
            continue

        for line_no, line in enumerate(text.splitlines(), start=1):
            if LEGACY_CALL_RE.search(line):
                failures.append((str(path.relative_to(repo_root)).replace("\\", "/"), line_no, line.strip()))

    if failures:
        print("Legacy pipeline-state call check failed:")
        for rel_path, line_no, line in failures:
            print(f"  {rel_path}:{line_no}: {line}")
        print("Use R_Backend_ApplyLegacyPipelineState() (bridge) or explicit bind_pipeline + set_dynamic_state.")
        return 1

    print("Legacy pipeline-state call check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
