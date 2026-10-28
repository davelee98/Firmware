#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyyaml"]
# ///
"""Encode a toolbox config export (oep_config.json) to raw config-packet bytes.

Faithful port of the web toolbox's collectPacketBytes() encoder, driven by the toolbox's
config.yaml schema (in-repo copy: configs/config.yaml), so the output matches what the toolbox writes.

  json2config.py configs/config_X_Y.json -o config.bin   # raw bytes
  json2config.py configs/config_X_Y.json --hex           # "1D 00 01 ..." for --config-hex
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import yaml

TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOL_DIR))
from config_packet import MAX_PACKET, validate_packet  # noqa: E402

# In-repo copy of the toolbox schema (configs/README.md says how to refresh it).
DEFAULT_SCHEMA = TOOL_DIR.parent / "configs" / "config.yaml"
SCHEMA_URL = "https://opendisplay.org/firmware/toolbox/config.yaml"


def crc16ccitt(data: bytes | list[int]) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def num_to_bytes_le(num: int, size: int) -> list[int]:
    out = []
    for _ in range(size):
        out.append(num & 0xFF)
        num //= 256
    return out


def parse_size(tok) -> int | None:
    if tok is None:
        return None
    tok = str(tok).strip()
    if re.fullmatch(r"\d+", tok):
        return int(tok)
    m = re.fullmatch(r"(\d+) bytes?", tok, re.IGNORECASE)
    return int(m.group(1)) if m else None


def parse_number(raw) -> int | None:
    if raw is None:
        return None
    if isinstance(raw, (int, float)) and not isinstance(raw, bool):
        return int(raw)
    raw = str(raw).strip()
    if not raw:
        return None
    if raw[:2].lower() == "0x":
        try:
            return int(raw, 16)
        except ValueError:
            return None
    return int(raw) if re.fullmatch(r"[-+]?\d+", raw) else None


def parse_bytes(raw) -> list[int]:
    raw = str(raw).strip()
    if raw[:2].lower() == "0x" or re.search(r"[A-Fa-f0-9]{2}", re.sub(r"\s+", "", raw)):
        s = re.sub(r"[^A-Fa-f0-9]", "", raw)
        return [int(s[i : i + 2], 16) for i in range(0, len(s), 2)]
    if re.fullmatch(r"\d+(\s+\d+)*", raw):
        return [int(x) & 0xFF for x in raw.split()]
    return list(raw.encode("utf-8"))


def truthy(raw) -> bool:
    return raw not in (None, "", 0, False)


def encode_field(f: dict, raw) -> list[int]:
    size = parse_size(f.get("size"))
    name = f.get("name")
    if size is None:
        return parse_bytes(raw) if truthy(raw) else []
    if f.get("type") == "text" or name in ("ssid", "password"):
        enc = str(raw if raw is not None else "").encode("utf-8")[: size - 1]
        return list(enc) + [0] * (size - len(enc))
    if f.get("type") == "bytes" or name == "encryption_key" or size > 8:
        b = parse_bytes(raw) if truthy(raw) else []
        return [b[i] if i < len(b) else 0 for i in range(size)]
    val = parse_number(raw)
    return [0] * size if val is None else num_to_bytes_le(val, size)


def encode(config: dict, schema: dict) -> tuple[bytes, list[str]]:
    types = {str(k): v for k, v in schema["packet_types"].items()}
    notes: list[str] = []
    insts = []
    for p in config.get("packets", []):
        pid = str(p.get("id", ""))
        if pid not in types:
            notes.append(f"skipped unknown packet id {pid!r}")
            continue
        insts.append((pid, p.get("fields") or {}))

    def key(inst):
        pid, fields = inst
        inum = parse_number(fields.get("instance_number"))
        return (int(pid) if pid.isdigit() else 0, inum or 0)

    insts.sort(key=key)  # stable, like the toolbox's Array.sort
    out = [0, 0, schema.get("version") or 1]
    for seq, (pid, fields) in enumerate(insts):
        pdef = types[pid]
        out += [seq & 0xFF, int(pid) & 0xFF]
        known = {f["name"] for f in pdef["fields"]}
        for f in pdef["fields"]:
            out += encode_field(f, fields.get(f["name"]))
        extra = sorted(set(fields) - known)
        if extra:
            notes.append(f"{pdef.get('name', pid)}: ignored field(s) not in schema: {', '.join(extra)}")
    crc = crc16ccitt(out)
    total = len(out) + 2
    out[0], out[1] = total & 0xFF, total >> 8
    out += num_to_bytes_le(crc, 2)
    return bytes(out), notes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", type=Path, help="toolbox export, e.g. oep_config.json")
    ap.add_argument(
        "--schema",
        type=Path,
        default=DEFAULT_SCHEMA,
        help="toolbox schema (default: configs/config.yaml)",
    )
    ap.add_argument("-o", "--output", type=Path, help="write raw bytes here")
    ap.add_argument("--hex", action="store_true", help="print space-separated hex to stdout")
    args = ap.parse_args()

    if not args.schema.is_file():
        print(f"error: schema not found: {args.schema}\n  pass --schema, or download {SCHEMA_URL}", file=sys.stderr)
        return 1
    schema = yaml.safe_load(args.schema.read_text())["ble_proto"]
    config = json.loads(args.config.read_text())
    packet, notes = encode(config, schema)
    if len(packet) > MAX_PACKET:
        print(f"error: packet is {len(packet)} bytes, firmware max is {MAX_PACKET}", file=sys.stderr)
        return 1
    validate_packet(packet)
    for n in notes:
        print(f"note: {n}", file=sys.stderr)
    cv, sv = config.get("minor_version"), schema.get("minor_version")
    if cv is not None and sv is not None and cv != sv:
        print(f"note: JSON minor_version {cv} vs schema {sv}; missing fields encode as 0", file=sys.stderr)
    if args.output:
        args.output.write_bytes(packet)
        print(f"wrote {len(packet)} bytes to {args.output}", file=sys.stderr)
    if args.hex or not args.output:
        print(" ".join(f"{b:02X}" for b in packet))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
