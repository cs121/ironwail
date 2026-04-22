#!/usr/bin/env python3
import pathlib
import re
import sys


def fail(msg: str) -> None:
    print(f"[framegraph-contract] {msg}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    root = pathlib.Path(__file__).resolve().parents[1]
    text = (root / "Quake" / "src" / "render" / "r_passes.c").read_text(encoding="utf-8")

    if "s_scene_color_attachments" not in text:
        fail("missing scene color attachment table")
    if "RENDER_RES_VELOCITY" not in text:
        fail("scene path no longer declares velocity resource")

    scene_match = re.search(
        r"static const RenderPassDesc s_scene_framegraph_pass = \{(.*?)\};",
        text,
        re.DOTALL,
    )
    if not scene_match:
        fail("missing s_scene_framegraph_pass")
    scene = scene_match.group(1)
    if ".num_color_attachments = 2" not in scene:
        fail("scene pass must expose two color attachments (scene_color + velocity)")
    if ".depth_attachment = &s_scene_depth_attachment" not in scene:
        fail("scene pass missing explicit depth attachment")

    shadow_match = re.search(
        r"static const RenderPassDesc s_shadowmaps_framegraph_pass = \{(.*?)\};",
        text,
        re.DOTALL,
    )
    if not shadow_match:
        fail("missing s_shadowmaps_framegraph_pass")
    shadow = shadow_match.group(1)
    if ".depth_attachment = &s_shadow_depth_attachment" not in shadow:
        fail("shadow pass missing explicit depth attachment")

    print("[framegraph-contract] OK")


if __name__ == "__main__":
    main()
