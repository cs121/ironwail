#!/usr/bin/env python3
"""
Renderer backend status audit.

This is a lightweight, repo-local inventory tool for the renderer migration.
It does not try to "fix" anything. It reports:

- build/topology clues for plugin boundaries
- broad GL symbol hits outside the intended ownership zones
- direct GL callsites and API-shaped assumptions
- hotspots that are worth auditing first
"""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass
import pathlib
import re
import sys
from typing import Iterable


ROOT = pathlib.Path(__file__).resolve().parents[1]

SCAN_ROOTS = (
    ROOT / "Quake" / "src",
    ROOT / "Quake" / "include",
    ROOT / "Windows" / "VisualStudio",
)

TARGET_FILES = (
    ROOT / "CMakeLists.txt",
    ROOT / "Windows" / "VisualStudio" / "ironwail.vcxproj",
    ROOT / "Windows" / "VisualStudio" / "ref_gl.vcxproj",
    ROOT / "Quake" / "include" / "renderer_plugin.h",
    ROOT / "Quake" / "include" / "render_api.h",
    ROOT / "Quake" / "include" / "renderer_host_bridge.h",
)

ALLOWED_GL_PATHS = (
    "Quake/src/render/",
    "Quake/src/platform/gl_vidsdl.c",
    "Quake/include/gl",
    "Quake/include/r_resources_gl.h",
)

IGNORED_OUTSIDE_BOUNDARY = {
    "CMakeLists.txt",
    "Windows/VisualStudio/ironwail.vcxproj",
    "Windows/VisualStudio/ref_gl.vcxproj",
}

TOPLOGY_CHECKS = (
    ("CMake host excludes ref_gl plugin units", ROOT / "CMakeLists.txt", "ref_gl_plugin.c"),
    ("CMake host excludes ref_vk plugin units", ROOT / "CMakeLists.txt", "ref_vk_plugin.c"),
    ("CMake host excludes ref_dx12 plugin units", ROOT / "CMakeLists.txt", "ref_dx12_plugin.c"),
    ("ref_gl project exists", ROOT / "Windows" / "VisualStudio" / "ref_gl.vcxproj", "ref_gl_plugin.c"),
    ("ironwail project still links OpenGL", ROOT / "Windows" / "VisualStudio" / "ironwail.vcxproj", "opengl32.lib"),
)

PATTERNS = (
    ("gl call", re.compile(r"\bgl[A-Z][A-Za-z0-9_]*\s*\(")),
    ("GL symbol", re.compile(r"\bGL_[A-Za-z0-9_]+\b")),
    ("GL type", re.compile(r"\bGL(?:enum|sizei|char|byte|short|int|float|double|boolean|bitfield|sync|intptr|sizeiptr|u?int(?:64)?|int64|uint64)\b")),
    ("GL include", re.compile(r'#include\s+"(?:glquake\.h|gl_[^"]+\.h)"')),
    ("GL FBO", re.compile(r"\bFBO\b|\bframebuffer\b", re.IGNORECASE)),
    ("GL bind", re.compile(r"\bgl(?:Bind|UseProgram|ActiveTexture|Framebuffer|Tex|Buffer|VertexArray)\b")),
    ("OpenGL mention", re.compile(r"\bopengl\b", re.IGNORECASE)),
    ("SDL GL", re.compile(r"\bSDL_GL\b")),
    ("WGL", re.compile(r"\bwgl[A-Za-z0-9_]*\b")),
    ("backend ref", re.compile(r"\br_backend\b|\bref_gl\b")),
)

STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')


@dataclass
class Hit:
    path: str
    line_no: int
    label: str
    line: str


def is_allowed(rel_path: str) -> bool:
    return any(rel_path.startswith(prefix) for prefix in ALLOWED_GL_PATHS)


def sanitize_line(line: str) -> str:
    line = line.split("//", 1)[0]
    return STRING_LITERAL_RE.sub("", line)


def scan_file(path: pathlib.Path) -> list[Hit]:
    rel_path = path.relative_to(ROOT).as_posix()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        return []

    hits: list[Hit] = []
    for line_no, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith("#ifndef GL_") or stripped.startswith("#define GL_") or stripped.startswith("#endif") and "GL_" in stripped:
            continue
        code = sanitize_line(line)
        for label, pattern in PATTERNS:
            if pattern.search(code):
                hits.append(Hit(rel_path, line_no, label, line.strip()))
                break
    return hits


def print_topology() -> None:
    print("== Topology ==")
    for label, path, needle in TOPLOGY_CHECKS:
        text = path.read_text(encoding="utf-8", errors="replace")
        ok = needle in text
        status = "OK" if ok else "MISSING"
        print(f"[{status}] {label}: {needle}")
    print()


def print_abi_summary() -> None:
    header = (ROOT / "Quake" / "include" / "renderer_plugin.h").read_text(encoding="utf-8", errors="replace")
    major = re.search(r"IW_RENDERER_PLUGIN_ABI_MAJOR\s+([0-9]+)u?", header)
    minor = re.search(r"IW_RENDERER_PLUGIN_ABI_MINOR\s+([0-9]+)u?", header)
    print("== Plugin ABI ==")
    print(f"ABI major: {major.group(1) if major else 'unknown'}")
    print(f"ABI minor: {minor.group(1) if minor else 'unknown'}")
    print()


def print_hotspots(all_hits: Iterable[Hit]) -> None:
    hits = list(all_hits)
    by_path = Counter(hit.path for hit in hits)
    print("== Hotspots ==")
    for path, count in by_path.most_common(20):
        print(f"{count:4d}  {path}")
    print()


def print_outside_boundary_hits(all_hits: Iterable[Hit]) -> None:
    print("== Outside intended GL ownership ==")
    outside = [hit for hit in all_hits if not is_allowed(hit.path) and hit.path not in IGNORED_OUTSIDE_BOUNDARY]
    if not outside:
        print("No hits outside the allowed renderer ownership zones.")
        print()
        return

    grouped: dict[str, list[Hit]] = defaultdict(list)
    for hit in outside:
        grouped[hit.path].append(hit)

    for path in sorted(grouped):
        print(path)
        for hit in grouped[path][:12]:
            print(f"  {hit.line_no:4d} [{hit.label}] {hit.line}")
        if len(grouped[path]) > 12:
            print(f"  ... {len(grouped[path]) - 12} more")
        print()


def print_pattern_summary(all_hits: Iterable[Hit]) -> None:
    counts = Counter(hit.label for hit in all_hits)
    print("== Pattern Summary ==")
    for label, count in counts.most_common():
        print(f"{count:4d}  {label}")
    print()


def main() -> int:
    files: list[pathlib.Path] = []
    for root in SCAN_ROOTS:
        if root.is_dir():
            files.extend(path for path in root.rglob("*") if path.is_file() and path.suffix.lower() in {".c", ".h", ".vcxproj"})

    # Targeted build files are included explicitly even if they are outside the scan roots.
    for path in TARGET_FILES:
        if path.is_file() and path not in files:
            files.append(path)

    all_hits: list[Hit] = []
    for path in files:
        all_hits.extend(scan_file(path))

    print_topology()
    print_abi_summary()
    print_pattern_summary(all_hits)
    print_hotspots(all_hits)
    print_outside_boundary_hits(all_hits)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
