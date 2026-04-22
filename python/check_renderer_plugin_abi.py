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
    header = (root / "Quake" / "include" / "renderer_plugin.h").read_text(encoding="utf-8")
    backend = (root / "Quake" / "src" / "render" / "r_backend.c").read_text(encoding="utf-8")
    builtin = (root / "Quake" / "src" / "render" / "renderer_builtin_gl_plugin.c").read_text(encoding="utf-8")

    require(r"#define\s+IW_RENDERER_PLUGIN_ABI_MAJOR\s+4u?\b", header, "ABI major version 4")
    require(r"#define\s+IW_RENDERER_PLUGIN_ABI_MINOR\s+0u?\b", header, "ABI minor version 0")
    require(r"qboolean\s*\(\*register_backend\)\s*\(\s*const IRenderBackend \*backend\s*\)\s*;", header, "host register_backend callback")
    require(r"qboolean\s*\(\*register_builtin_backend\)\s*\(\s*const char \*backend_name\s*\)\s*;", header, "deprecated compatibility callback")
    require(r"const IRenderBackend \*builtin_opengl_backend\s*;", header, "builtin_opengl_backend pointer")
    require(r"const iw_renderer_plugin_surface_services_t \*surface_services\s*;", header, "surface services pointer")
    require(r"const iw_renderer_plugin_resource_services_t \*resource_services\s*;", header, "resource services pointer")
    require(r"const iw_renderer_plugin_upload_services_t \*upload_services\s*;", header, "upload services pointer")
    require(r"const iw_renderer_plugin_pipeline_services_t \*pipeline_services\s*;", header, "pipeline services pointer")
    require(r"IW_RENDERER_PLUGIN_DESCRIPTOR_MIN_SIZE", header, "descriptor minimum-size ABI guard")
    require(r"ABI mismatch", backend, "clear ABI mismatch diagnostics")
    require(r"surface_services", backend, "host API wiring for v3 surface services")
    require(r"resource_services", backend, "host API wiring for v3 resource services")
    require(r"upload_services", backend, "host API wiring for v3 upload services")
    require(r"pipeline_services", backend, "host API wiring for v3 pipeline services")
    require(r"register_backend", builtin, "builtin plugin direct backend registration path")

    print("[renderer-plugin-abi] OK")


if __name__ == "__main__":
    main()
