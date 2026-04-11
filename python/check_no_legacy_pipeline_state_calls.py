#!/usr/bin/env python3
import pathlib
import re
import sys


LEGACY_CALL_RE = re.compile(r"\bR_Backend_SetPipelineState\s*\(")


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    quake_dir = repo_root / "Quake"
    allow_file = (quake_dir / "r_backend.c").resolve()
    failures = []

    for path in quake_dir.glob("*.c"):
        full = path.resolve()
        text = full.read_text(encoding="utf-8")
        if full == allow_file:
            continue

        for line_no, line in enumerate(text.splitlines(), start=1):
            if LEGACY_CALL_RE.search(line):
                failures.append((str(path.relative_to(repo_root)), line_no, line.strip()))

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
