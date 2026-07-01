#!/usr/bin/env python3
"""PlatformIO post-build: convert firmware.hex to drag-and-drop firmware.uf2 (nRF52840)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

Import("env")

PROJECT_DIR = Path(env["PROJECT_DIR"])
UF2CONV = PROJECT_DIR / "tools" / "uf2conv.py"
UF2_FAMILY = "0xADA52840"


def make_uf2(source, target, env) -> None:
    del source, target
    hex_path = Path(env.subst("$BUILD_DIR/${PROGNAME}.hex"))
    uf2_path = Path(env.subst("$BUILD_DIR/${PROGNAME}.uf2"))
    if not hex_path.is_file():
        print(f"nrf_uf2_post: no {hex_path}, skipping UF2", file=sys.stderr)
        return
    if not UF2CONV.is_file():
        print(f"nrf_uf2_post: missing {UF2CONV}", file=sys.stderr)
        env.Exit(1)
    cmd = [
        sys.executable,
        str(UF2CONV),
        str(hex_path),
        "--family",
        UF2_FAMILY,
        "--output",
        str(uf2_path),
    ]
    print(" ".join(cmd), file=sys.stderr)
    subprocess.check_call(cmd, cwd=UF2CONV.parent)
    print(f"nrf_uf2_post: wrote {uf2_path}", file=sys.stderr)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", make_uf2)
