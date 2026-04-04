#!/usr/bin/env python3
"""Compute sha256 hashes for paired reference screenshots."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

EFFECTS = ("TE_EXPLOSION", "GUNSHOT", "BLOOD", "LIGHTNING", "TELEPORT")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline_dir", type=Path)
    parser.add_argument("candidate_dir", type=Path)
    parser.add_argument("--ext", default="png", help="Screenshot extension (default: png)")
    args = parser.parse_args()

    ok = True
    print("effect,baseline,candidate,match")
    for effect in EFFECTS:
        base = args.baseline_dir / f"{effect}.{args.ext}"
        cand = args.candidate_dir / f"{effect}.{args.ext}"
        if not base.exists() or not cand.exists():
            print(f"{effect},MISSING,MISSING,false")
            ok = False
            continue
        b = sha256(base)
        c = sha256(cand)
        match = b == c
        print(f"{effect},{b},{c},{str(match).lower()}")
        ok = ok and match

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
