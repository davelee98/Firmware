#!/usr/bin/env python3
"""Shared helpers for toolbox-style config packet hex."""

from __future__ import annotations

import re

MAX_PACKET = 4096


def crc16ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def outer_packet_crc(body: bytes) -> int:
    """CRC16-CCITT over outer packet body (length field zeroed)."""
    if len(body) < 2:
        return crc16ccitt(body)
    tmp = bytearray(body)
    tmp[0] = 0
    tmp[1] = 0
    return crc16ccitt(bytes(tmp))


def parse_hex_bytes(text: str) -> bytes:
    text = text.strip()
    if not text:
        raise ValueError("empty config hex string")
    if re.search(r"[\s,]", text):
        parts = re.split(r"[\s,]+", text)
        return bytes(int(p, 16) for p in parts if p)
    if len(text) % 2:
        raise ValueError("hex string has odd length")
    return bytes.fromhex(text)


def read_hex_arg(value: str | None, file_path: str | None) -> bytes:
    if bool(value) == bool(file_path):
        raise ValueError("specify exactly one of --config-hex or --config-file")
    packet = parse_hex_bytes(value) if value else open(file_path, "rb").read()
    validate_packet(packet)
    return packet


def validate_packet(packet: bytes) -> None:
    if len(packet) < 4:
        raise ValueError("config packet too short")
    if len(packet) > MAX_PACKET:
        raise ValueError(f"config packet too large ({len(packet)} bytes, max {MAX_PACKET})")
    declared = packet[0] | (packet[1] << 8)
    if declared != len(packet):
        raise ValueError(f"length field {declared} does not match packet size {len(packet)}")
    given = packet[-2] | (packet[-1] << 8)
    calc = outer_packet_crc(packet[:-2])
    if given != calc:
        raise ValueError(f"CRC mismatch (given 0x{given:04X}, expected 0x{calc:04X})")
