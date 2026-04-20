#!/usr/bin/env bash
set -euo pipefail

CC_BIN="${CC:-cc}"
CFLAGS_COMMON="-std=gnu11 -fsyntax-only -IQuake/include"
SDL_CFLAGS="$(pkg-config --cflags sdl2 2>/dev/null || true)"

units=(
  Quake/src/ui/menu_common.c
  Quake/src/ui/menu_main.c
  Quake/src/ui/menu_options.c
  Quake/src/ui/menu_multiplayer.c
  Quake/src/ui/menu_mods.c
  Quake/src/ui/menu_video.c
)

for unit in "${units[@]}"; do
  echo "[menu-compile] $unit"
  $CC_BIN $CFLAGS_COMMON $SDL_CFLAGS "$unit"
done
