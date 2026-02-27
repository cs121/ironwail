#!/usr/bin/env python3
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
A2_FILES = [
    Path("Quake/backend_gl_exec.c"),
    Path("Quake/gl_screen.c"),
    Path("Quake/gl_sky.c"),
    Path("Quake/gl_rmain.c"),
    Path("Quake/r_fogvol.c"),
    Path("Quake/r_postfx.c"),
]
GL_CALL_RE = re.compile(r"\bgl[A-Z][A-Za-z0-9_]*\s*\(")
MARKER = "GL-EXCEPTION"


def main() -> int:
    violations = []
    for rel_path in A2_FILES:
        path = REPO_ROOT / rel_path
        if not path.exists():
            violations.append(f"{rel_path}: Datei nicht gefunden")
            continue

        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if GL_CALL_RE.search(line) and MARKER not in line:
                violations.append(f"{rel_path}:{lineno}: {line.strip()}")

    if violations:
        print("ERROR: direkte gl*-Aufrufe ohne GL-EXCEPTION-Marker gefunden:")
        print("\n".join(violations))
        return 1

    print("OK: Keine unmarkierten direkten gl*-Aufrufe in A2-relevanten Dateien gefunden.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
