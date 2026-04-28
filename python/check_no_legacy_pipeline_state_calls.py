#!/usr/bin/env python3
import pathlib
import re
import sys


LEGACY_CALL_RE = re.compile(r"\bR_Backend_SetPipelineState\s*\(")
SCAN_ROOTS = (
    pathlib.Path("Quake/src"),
)


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    allow_file = (repo_root / "Quake" / "src" / "render" / "r_backend.c").resolve()
    failures = []

    for scan_root in SCAN_ROOTS:
        root = repo_root / scan_root
        if not root.is_dir():
            continue

        for path in root.rglob("*.c"):
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
