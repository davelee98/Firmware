#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyyaml"]
# ///
"""
Build a flashable factory image from a toolbox config JSON, in one step.

  JSON --(json2config)--> config packet --(pio run, factory embed)--> firmware
       --> verify the packet is inside --> copy the flashable image to factory_images/

Flashable image per target family:
  ESP32  firmware.factory.bin  full flash image (bootloader + partitions + app),
                               merged by the pioarduino platform; flash at 0x0
  nRF    firmware.uf2          drag-and-drop onto the bootloader drive
                               (firmware.hex is copied alongside)

The PlatformIO env comes from configs/firmware_targets.json, which maps each config to the
one firmware target it is built for. The build is refused if the env is not a released target
(.github/firmware-targets.json) or does not match the config's system_config.ic_type.

Examples:
  ./tools/build_factory_image.py configs/config_Seeed-Studio_reTerminal-E1004.json
  ./tools/build_factory_image.py configs/config_X_Y.json --version 2.26.7
  ./tools/build_factory_image.py --check          # validate every config <-> target mapping
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
FW_DIR = TOOL_DIR.parent
sys.path.insert(0, str(TOOL_DIR))

import yaml  # noqa: E402
from json2config import DEFAULT_SCHEMA, encode, parse_number  # noqa: E402
from provision_firmware import append_build_flags, find_pio, git_sha  # noqa: E402

FACTORY_CFG_MAGIC = 0xFAC70A5A  # src/factory_config.h
DEFAULT_OUT = FW_DIR / "factory_images"
CONFIGS_DIR = FW_DIR / "configs"
TARGET_MAP = CONFIGS_DIR / "firmware_targets.json"
RELEASED_TARGETS = FW_DIR / ".github" / "firmware-targets.json"

# system_config.ic_type (configs/config.yaml) -> chip/family in .github/firmware-targets.json.
# 5-8 (nRF52811, EFR32BG22, nRF54L15, nRF54LM20) run other firmware repos, so no entry here.
IC_TYPE_CHIP = {1: "nrf52", 2: "esp32-s3", 3: "esp32-c3", 4: "esp32-c6", 9: "esp32"}

# Inherited values for these would silently change what gets built (a stale exported
# config embedded instead of ours, or a config-clearing image), so never pass them through.
SCRUB_ENV = (
    "OPENDISPLAY_FACTORY_CONFIG_HEX",
    "PLATFORMIO_FACTORY_CONFIG_HEX",
    "OPENDISPLAY_FACTORY_CLEAR_CONFIG",
    "PLATFORMIO_FACTORY_CLEAR_CONFIG",
)

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157


def fail(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


def secrets_in(config: dict) -> list[str]:
    """Name the packets that would put credentials into the image."""
    found = []
    for p in config.get("packets", []):
        fields = p.get("fields") or {}
        name = p.get("name") or str(p.get("id"))
        if name == "wifi_config" and str(fields.get("password", "")).strip():
            found.append("wifi_config (Wi-Fi password)")
        if name == "security_config":
            key = "".join(c for c in str(fields.get("encryption_key", "")) if c not in " x")
            enabled = str(fields.get("encryption_enabled", "0")).strip() not in ("", "0", "0x0", "0x00")
            if enabled or key.strip("0"):
                found.append("security_config (encryption key)")
    return found


def load_target_map() -> dict[str, str]:
    if not TARGET_MAP.is_file():
        return {}
    return json.loads(TARGET_MAP.read_text()).get("targets", {})


def load_released_targets() -> dict[str, str]:
    """env -> chip (or family for nRF) for every target CI releases."""
    targets = json.loads(RELEASED_TARGETS.read_text())["targets"]
    return {t["env"]: t.get("chip") or t["family"] for t in targets}


def target_problems(config: dict, env: str, released: dict[str, str]) -> list[str]:
    if env not in released:
        return [f"env {env!r} is not a released target in {RELEASED_TARGETS.relative_to(FW_DIR)}"]
    ic_type = None
    for p in config.get("packets", []):
        if p.get("name") == "system_config" or str(p.get("id")) == "1":
            ic_type = parse_number((p.get("fields") or {}).get("ic_type"))
    if ic_type is None:
        return ["config has no system_config.ic_type"]
    want = IC_TYPE_CHIP.get(ic_type)
    if want is None:
        return [f"ic_type {ic_type} is not built by this firmware repo"]
    if released[env] != want:
        return [f"ic_type {ic_type} is {want}, but env {env!r} builds {released[env]}"]
    return []


def check_all() -> int:
    """Validate configs/firmware_targets.json against the configs on disk."""
    mapping = load_target_map()
    released = load_released_targets()
    configs = sorted(p.name for p in CONFIGS_DIR.glob("config_*.json"))
    problems = [f"{name}: no entry in {TARGET_MAP.name}" for name in configs if name not in mapping]
    problems += [f"{name}: mapped in {TARGET_MAP.name} but file is missing" for name in mapping if name not in configs]
    for name in configs:
        if name in mapping:
            config = json.loads((CONFIGS_DIR / name).read_text())
            problems += [f"{name}: {msg}" for msg in target_problems(config, mapping[name], released)]
    for msg in problems:
        print(f"error: {msg}", file=sys.stderr)
    if not problems:
        print(f"ok: {len(configs)} config(s) mapped to valid targets", file=sys.stderr)
    return 1 if problems else 0


def default_version() -> str | None:
    r = subprocess.run(
        ["git", "describe", "--tags", "--abbrev=0"], cwd=FW_DIR, capture_output=True, text=True, check=False
    )
    return r.stdout.strip() or None if r.returncode == 0 else None


def uf2_flash_bytes(path: Path) -> bytes:
    """Reassemble the flash contents a UF2 file writes, so the packet can be searched for."""
    data = path.read_bytes()
    blocks = {}
    for off in range(0, len(data) - 511, 512):
        m0, m1, _flags, addr, size = struct.unpack_from("<5I", data, off)
        if m0 == UF2_MAGIC_START0 and m1 == UF2_MAGIC_START1:
            blocks[addr] = data[off + 32 : off + 32 + size]
    if not blocks:
        return b""
    base = min(blocks)
    out = bytearray(max(a + len(b) for a, b in blocks.items()) - base)
    for addr, chunk in blocks.items():
        out[addr - base : addr - base + len(chunk)] = chunk
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", type=Path, nargs="?", help="toolbox config JSON, e.g. configs/config_X_Y.json")
    ap.add_argument(
        "-e",
        "--env",
        help="PlatformIO env; required only for a config not in configs/firmware_targets.json, "
        "and must match the mapping otherwise",
    )
    ap.add_argument("--check", action="store_true", help="validate every config <-> target mapping and exit")
    ap.add_argument("--version", help="BUILD_VERSION (default: latest git tag)")
    ap.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA, help="toolbox schema (default: configs/config.yaml)")
    ap.add_argument("-o", "--out-dir", type=Path, default=DEFAULT_OUT, help="output directory (default: factory_images)")
    ap.add_argument(
        "--allow-secrets",
        action="store_true",
        help="build even if the JSON carries a Wi-Fi password or encryption key "
        "(they end up in plain text in the image, identical on every unit)",
    )
    args = ap.parse_args()

    if args.check:
        return check_all()
    if args.config is None:
        return fail("give a config JSON, or --check")

    # 1. JSON -> config packet
    if not args.config.is_file():
        return fail(f"config not found: {args.config}")
    if not args.schema.is_file():
        return fail(f"schema not found: {args.schema}")
    config = json.loads(args.config.read_text())

    # Firmware target: the mapping is authoritative; -e only fills in for unmapped configs.
    mapped = load_target_map().get(args.config.name)
    if mapped and args.env and args.env != mapped:
        return fail(f"{args.config.name} is mapped to {mapped!r} in {TARGET_MAP.name}, not {args.env!r}")
    env = mapped or args.env
    if env is None:
        return fail(f"{args.config.name} has no entry in {TARGET_MAP.relative_to(FW_DIR)}; add one (or pass -e)")
    if not mapped:
        print(f"warning: {args.config.name} is not in {TARGET_MAP.name}; using -e {env}", file=sys.stderr)
    problems = target_problems(config, env, load_released_targets())
    if problems:
        return fail(f"{args.config.name} -> {env}: {problems[0]}")
    args.env = env
    secrets = secrets_in(config)
    if secrets and not args.allow_secrets:
        return fail(
            f"{args.config.name} contains {', '.join(secrets)}.\n"
            "  Remove it from the JSON, or pass --allow-secrets to embed it anyway."
        )
    schema = yaml.safe_load(args.schema.read_text())["ble_proto"]
    packet, notes = encode(config, schema)
    for n in notes:
        print(f"note: {n}", file=sys.stderr)
    print(f"config: {args.config.name} -> {len(packet)} byte packet", file=sys.stderr)

    # 2. Build with the packet embedded
    version = args.version or default_version()
    extra: dict[str, str] = {}
    append_build_flags(extra, version, git_sha() if version else None)
    env_vars = {k: v for k, v in os.environ.items() if k not in SCRUB_ENV}
    env_vars.update(extra)
    env_vars["OPENDISPLAY_FACTORY_CONFIG_HEX"] = " ".join(f"{b:02X}" for b in packet)
    cmd = [find_pio(), "run", "-e", args.env]
    print(" ".join(cmd) + (f"  (BUILD_VERSION={version})" if version else ""), file=sys.stderr)
    rc = subprocess.call(cmd, cwd=FW_DIR, env=env_vars)
    if rc != 0:
        return fail(
            f"pio run failed (exit {rc}). If the link error is 'undefined reference to g_factory_embed', "
            "something regenerated src/generated/ mid-build (e.g. the IDE re-indexing); run again."
        )

    # 3. Pick up the flashable image. No freshness check: an up-to-date build does not relink,
    #    and step 4 proves the image carries exactly this packet either way.
    build = FW_DIR / ".pio" / "build" / args.env
    candidates = [build / "firmware.factory.bin", build / "firmware.uf2"]
    image = next((p for p in candidates if p.is_file()), None)
    if image is None:
        return fail(f"no firmware.factory.bin or firmware.uf2 in {build}")
    extras = [build / "firmware.hex"] if image.suffix == ".uf2" else []

    # 4. Verify the packet is inside, in the exact form the firmware looks for
    needle = struct.pack("<II", FACTORY_CFG_MAGIC, len(packet)) + packet
    flash = uf2_flash_bytes(image) if image.suffix == ".uf2" else image.read_bytes()
    if needle not in flash:
        return fail(f"built image {image} does not contain the config packet")

    # 5. Copy out under a name that says what it is. One image per config: drop any earlier
    #    build of this config (other version), so the directory only holds current images.
    args.out_dir.mkdir(parents=True, exist_ok=True)
    # config_[Manufacturer]_[Model].json -> factory_[Manufacturer]_[Model]_[version]. The env is
    # left out of the name: firmware_targets.json maps each config to exactly one.
    base = "factory_" + args.config.stem.removeprefix("config_")
    stem = base + (f"_{version}" if version else "")
    for old in sorted(args.out_dir.glob(f"{base}_*")):
        if old.is_file() and not old.name.startswith(stem + "_") and not old.name.startswith(stem + "."):
            old.unlink()
            print(f"removed previous image: {old.name}", file=sys.stderr)
    out = args.out_dir / f"{stem}{'.uf2' if image.suffix == '.uf2' else '_full.bin'}"
    shutil.copyfile(image, out)
    for extra_file in extras:
        if extra_file.is_file():
            shutil.copyfile(extra_file, args.out_dir / f"{stem}{extra_file.suffix}")

    print(f"\nfactory image: {out}  (config verified inside)", file=sys.stderr)
    if out.suffix == ".uf2":
        print("flash: double-tap reset, then copy the .uf2 onto the bootloader drive", file=sys.stderr)
    else:
        print(f"flash: esptool.py write-flash 0x0 {out}", file=sys.stderr)
    print(
        "note: the config applies only when no valid config is stored; erase flash first on a used device.",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
