#!/usr/bin/env python3
import fnmatch
import pathlib
import re
import sys


SCAN_ROOTS = (
    pathlib.Path("Quake/src"),
)

RENDER_TREE_PREFIX = "Quake/src/render/"

FILE_EXTENSIONS = {".c", ".h"}

# Global renderer-boundary rule:
# - GL symbols must stay in renderer/backend-owned files.
# - Transitional UI/console callsites are explicitly allowlisted until moved.
ALLOWED_GLOBS = (
    "Quake/src/render/ref_gl_*",
    "Quake/src/render/gl_backend*",
    "Quake/src/render/r_resources_gl.*",
    "Quake/src/platform/gl_vidsdl.c",
)

# Transitional non-render files that still use GL canvas helpers.
TRANSITIONAL_ALLOWED_FILES = {
    "Quake/src/core/quakedef.h",
}

CODE_PATTERNS = (
    re.compile(r"\bgl[A-Z][A-Za-z0-9_]*\b"),
    re.compile(r"\bGL_[A-Za-z0-9_]+\b"),
    re.compile(r"\bGL(?:enum|sizei|char|byte|short|int|float|double|boolean|bitfield|sync|intptr|sizeiptr)\b"),
    re.compile(r"\bGL(?:u?int(?:64)?|int64|uint64)\b"),
)

INCLUDE_PATTERNS = (
    re.compile(r'#include\s+"glquake\.h"'),
    re.compile(r'#include\s+"gl_[^"]+\.h"'),
)

STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')
HOST_PATH_RE = re.compile(r"^Quake/src/(core|client|server|network|ui|audio|physics|gamecode|bot|platform)/(?!gl_vidsdl\\.c).+")
HOST_GL_PATTERN = re.compile(r"\b(?:GL_[A-Za-z0-9_]+|SDL_GL_[A-Za-z0-9_]+)\b")


def sanitize_code_line(line: str) -> str:
    line = line.split("//", 1)[0]
    line = STRING_LITERAL_RE.sub("", line)
    return line


def is_allowed(rel_path: str) -> bool:
    if rel_path in TRANSITIONAL_ALLOWED_FILES:
        return True
    return any(fnmatch.fnmatch(rel_path, glob_pattern) for glob_pattern in ALLOWED_GLOBS)


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    failures = []

    for scan_root in SCAN_ROOTS:
        root = repo_root / scan_root
        if not root.is_dir():
            continue

        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in FILE_EXTENSIONS:
                continue

            rel_path = path.relative_to(repo_root).as_posix()
            if rel_path.startswith(RENDER_TREE_PREFIX) and not is_allowed(rel_path):
                continue
            if is_allowed(rel_path):
                continue

            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except UnicodeDecodeError:
                # Not expected for source files, but avoid crashing CI if encountered.
                continue

            for line_no, line in enumerate(lines, start=1):
                stripped = line.strip()
                if HOST_PATH_RE.match(rel_path):
                    code_line = sanitize_code_line(line)
                    if HOST_GL_PATTERN.search(code_line):
                        failures.append((rel_path, line_no, line.strip()))
                        continue
                if re.match(r"^\s*#\s*(ifdef|ifndef|if|elif|define|undef|endif)\b.*\bGL_[A-Za-z0-9_]+\b", stripped):
                    continue
                code_line = sanitize_code_line(line)
                for pattern in CODE_PATTERNS:
                    if pattern.search(code_line):
                        failures.append((rel_path, line_no, line.strip()))
                        break
                else:
                    for pattern in INCLUDE_PATTERNS:
                        if pattern.search(line):
                            failures.append((rel_path, line_no, line.strip()))
                            break

    if failures:
        print("GL symbol boundary check failed:")
        for rel_path, line_no, line in failures:
            print(f"  {rel_path}:{line_no}: {line}")
        print("Move GL usage into renderer/backend files, or explicitly document/allowlist a transitional seam.")
        return 1

    print("GL symbol boundary check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
