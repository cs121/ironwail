#!/usr/bin/env python3
"""
Guardrail for renderer artifact topology.

This check enforces that plugin implementation units are not compiled into the
host target definitions we currently maintain in source control.
"""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]

CHECKS = [
    (
        ROOT / "CMakeLists.txt",
        [
            "list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_gl_plugin.c)",
            "list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_gl_bridge_stubs.c)",
            "list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_vk_plugin.c)",
            "list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/ref_dx12_plugin.c)",
            "list(REMOVE_ITEM IWAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Quake/src/render/gl_backend.c)",
        ],
    ),
    (
        ROOT / "Quake" / "Makefile",
        [
            "RENDERER_PLUGIN_TARGET = ref_gl.so",
            "src/render/ref_gl_plugin.c",
        ],
    ),
    (
        ROOT / "Quake" / "Makefile.w32",
        [
            "RENDERER_PLUGIN_TARGET = ref_gl.dll",
            "src/render/ref_gl_plugin.c",
        ],
    ),
    (
        ROOT / "Quake" / "Makefile.w64",
        [
            "RENDERER_PLUGIN_TARGET = ref_gl.dll",
            "src/render/ref_gl_plugin.c",
        ],
    ),
    (
        ROOT / "Windows" / "VisualStudio" / "ref_gl.vcxproj",
        [
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\ref_gl_plugin.c\" />",
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\ref_gl_bridge_stubs.c\" />",
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\gl_backend.c\" />",
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\gl_backend_runtime.c\" />",
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\gl_backend_resources.c\" />",
        ],
    ),
    (
        ROOT / "Windows" / "VisualStudio" / "ironwail.vcxproj",
        [
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\gl_backend_runtime.c\" />",
            "<ClCompile Include=\"..\\..\\Quake\\src\\render\\gl_backend_resources.c\" />",
        ],
    ),
]

FORBIDDEN_BY_FILE = {
    ROOT / "Quake" / "Makefile": ["renderer_builtin_gl_plugin.c"],
    ROOT / "Quake" / "Makefile.w32": ["renderer_builtin_gl_plugin.c"],
    ROOT / "Quake" / "Makefile.w64": ["renderer_builtin_gl_plugin.c"],
    ROOT / "Windows" / "VisualStudio" / "ironwail.vcxproj": [
        "..\\..\\Quake\\src\\render\\renderer_builtin_gl_plugin.c",
        "..\\..\\Quake\\src\\render\\gl_backend.c",
    ],
}


def main() -> int:
    failures = []

    files_to_scan = set(path for path, _ in CHECKS) | set(FORBIDDEN_BY_FILE.keys())
    file_text = {}
    for path in files_to_scan:
        file_text[path] = path.read_text(encoding="utf-8", errors="replace")

    for path, required_lines in CHECKS:
        text = file_text[path]
        for required in required_lines:
            if required not in text:
                failures.append(f"{path}: missing required topology rule: {required}")

    for path, forbidden_lines in FORBIDDEN_BY_FILE.items():
        text = file_text[path]
        for forbidden in forbidden_lines:
            if forbidden in text:
                failures.append(f"{path}: forbidden legacy topology snippet present: {forbidden}")

    if failures:
        print("Renderer topology check failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print("Renderer topology check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
