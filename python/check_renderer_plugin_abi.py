#!/usr/bin/env python3
import pathlib
import re
import sys


def fail(msg: str) -> None:
    print(f"[renderer-plugin-abi] {msg}", file=sys.stderr)
    raise SystemExit(1)


def require(pattern: str, text: str, label: str) -> None:
    if not re.search(pattern, text, re.MULTILINE):
        fail(f"missing {label}")


def main() -> None:
    root = pathlib.Path(__file__).resolve().parents[1]
    header = (root / "Quake" / "renderer_plugin.h").read_text(encoding="utf-8")
    backend = (root / "Quake" / "r_backend.c").read_text(encoding="utf-8")
    builtin = (root / "Quake" / "renderer_builtin_gl_plugin.c").read_text(encoding="utf-8")

    require(r"#define\s+IW_RENDERER_PLUGIN_ABI_MAJOR\s+2u?\b", header, "ABI major version 2")
    require(r"#define\s+IW_RENDERER_PLUGIN_ABI_MINOR\s+0u?\b", header, "ABI minor version 0")
    require(r"qboolean\s*\(\*register_backend\)\s*\(\s*const IRenderBackend \*backend\s*\)\s*;", header, "host register_backend callback")
    require(r"qboolean\s*\(\*register_builtin_backend\)\s*\(\s*const char \*backend_name\s*\)\s*;", header, "deprecated compatibility callback")
    require(r"const IRenderBackend \*builtin_opengl_backend\s*;", header, "builtin_opengl_backend pointer")
    require(r"ABI mismatch", backend, "clear ABI mismatch diagnostics")
    require(r"register_backend", builtin, "builtin plugin direct backend registration path")

    print("[renderer-plugin-abi] OK")


if __name__ == "__main__":
    main()
