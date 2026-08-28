#!/usr/bin/env python3
"""Verify the compiled Raphael EA8076 60/75/90 Hz timing contract.

The verifier accepts either a raw FDT/DTBO overlay or a packed dtbo.img. It
checks the values consumed by the legacy EvolutionX command-mode DSI driver,
as well as the panel commands that select the physical FFC oscillator.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


DT_TABLE_MAGIC = 0xD7B7AB1E
FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9

PANELS = {
    "qcom,mdss_dsi_ss_fhd_ea8076_cmd": {
        "clock60": 1_100_000_000,
        "phy60": bytes.fromhex("00 24 0A 0A 26 25 09 0A 06 03 04 00 1E 1A"),
        "phy90": bytes.fromhex("00 31 0D 0D 2B 28 0D 0E 09 02 04 00 27 1C"),
        "on90": bytes.fromhex("A3 B9 A1 4A 00 13 98"),
        "switch90": bytes.fromhex("A3 A9 A1 4A 00 65 45"),
        "d1_60": bytes.fromhex("02 D1 0F"),
        "d1_90": bytes.fromhex("02 D1 0D"),
    },
    "qcom,mdss_dsi_ss_fhd_ea8076_global_cmd": {
        "clock60": 1_103_000_000,
        "phy60": bytes.fromhex("00 24 0A 0A 26 24 0A 0A 06 03 04 00 1E 1A"),
        "phy90": bytes.fromhex("00 30 0D 0D 2A 28 0C 0D 09 02 04 00 27 1C"),
        "on90": bytes.fromhex("A3 B9 A1 4A 00 13 A6"),
        "switch90": bytes.fromhex("A3 A9 A1 4A 00 65 8B"),
        "d1_60": bytes.fromhex("02 D1 11"),
        "d1_90": bytes.fromhex("02 D1 0F"),
    },
}

MODES = {
    0: (60, 16666),
    1: (75, 13333),
    2: (90, 11111),
}
PORCHES = {
    "qcom,mdss-dsi-h-front-porch": 42,
    "qcom,mdss-dsi-h-back-porch": 42,
    "qcom,mdss-dsi-h-pulse-width": 12,
    "qcom,mdss-dsi-v-front-porch": 42,
    "qcom,mdss-dsi-v-back-porch": 42,
    "qcom,mdss-dsi-v-pulse-width": 12,
}


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def align4(value: int) -> int:
    return (value + 3) & ~3


def words(data: bytes, offset: int, count: int) -> tuple[int, ...]:
    end = offset + count * 4
    if end > len(data):
        fail(f"truncated integer block at offset {offset}")
    return struct.unpack_from(f">{count}I", data, offset)


def fdt_blobs(image: bytes) -> list[bytes]:
    if len(image) < 4:
        fail("input is too small")
    magic = struct.unpack_from(">I", image, 0)[0]
    if magic == FDT_MAGIC:
        return [image]
    if magic != DT_TABLE_MAGIC:
        fail(f"unsupported image magic 0x{magic:08x}")

    (
        _magic,
        total_size,
        header_size,
        entry_size,
        entry_count,
        entries_offset,
        _page_size,
        version,
    ) = words(image, 0, 8)
    if total_size > len(image) or header_size < 32 or entry_size < 32:
        fail("invalid DT table header")

    result = []
    for index in range(entry_count):
        entry = entries_offset + index * entry_size
        entry_words = words(image, entry, 8)
        dt_size, dt_offset = entry_words[:2]
        if version >= 1 and entry_words[4] & 0xF:
            fail(f"DT entry {index} is compressed")
        end = dt_offset + dt_size
        if end > len(image):
            fail(f"DT entry {index} exceeds image bounds")
        blob = image[dt_offset:end]
        if len(blob) < 4 or struct.unpack_from(">I", blob, 0)[0] != FDT_MAGIC:
            fail(f"DT entry {index} is not an FDT blob")
        result.append(blob)
    if not result:
        fail("DT table contains no entries")
    return result


def parse_fdt(blob: bytes) -> dict[str, dict[str, bytes]]:
    if len(blob) < 40:
        fail("FDT header is truncated")
    (
        magic,
        total_size,
        struct_offset,
        strings_offset,
        _reserve_offset,
        _version,
        _last_compatible,
        _boot_cpu,
        strings_size,
        struct_size,
    ) = words(blob, 0, 10)
    if magic != FDT_MAGIC or total_size > len(blob):
        fail("invalid FDT header")
    struct_end = struct_offset + struct_size
    strings_end = strings_offset + strings_size
    if struct_end > total_size or strings_end > total_size:
        fail("FDT structure or strings exceed total size")

    strings = blob[strings_offset:strings_end]
    nodes: dict[str, dict[str, bytes]] = {}
    stack: list[str] = []

    def path() -> str:
        names = [name for name in stack if name]
        return "/" + "/".join(names) if names else "/"

    offset = struct_offset
    while offset < struct_end:
        token = words(blob, offset, 1)[0]
        offset += 4
        if token == FDT_BEGIN_NODE:
            end = blob.find(b"\0", offset, struct_end)
            if end < 0:
                fail("unterminated FDT node name")
            stack.append(blob[offset:end].decode("ascii"))
            offset = align4(end + 1)
            nodes.setdefault(path(), {})
        elif token == FDT_END_NODE:
            if not stack:
                fail("unbalanced FDT_END_NODE")
            stack.pop()
        elif token == FDT_PROP:
            length, name_offset = words(blob, offset, 2)
            offset += 8
            end = offset + length
            if end > struct_end:
                fail("FDT property exceeds structure block")
            name_end = strings.find(b"\0", name_offset)
            if name_offset >= len(strings) or name_end < 0:
                fail("invalid FDT property name offset")
            name = strings[name_offset:name_end].decode("ascii")
            nodes.setdefault(path(), {})[name] = blob[offset:end]
            offset = align4(end)
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            break
        else:
            fail(f"unknown FDT token {token}")
    return nodes


def one_node(nodes: dict[str, dict[str, bytes]], basename: str) -> str:
    matches = [
        path
        for path in nodes
        if path.rsplit("/", 1)[-1] == basename
        and not path.startswith("/__local_fixups__/")
    ]
    if len(matches) != 1:
        fail(f"{basename}: expected one node, found {len(matches)}")
    return matches[0]


def cell(props: dict[str, bytes], name: str, label: str) -> int:
    value = props.get(name)
    if value is None:
        fail(f"{label}: missing {name}")
    if len(value) != 4:
        fail(f"{label}: {name} is not one cell")
    return struct.unpack(">I", value)[0]


def prop(props: dict[str, bytes], name: str, label: str) -> bytes:
    value = props.get(name)
    if value is None:
        fail(f"{label}: missing {name}")
    return value


def check_contains(data: bytes, wanted: bytes, label: str) -> None:
    if wanted not in data:
        fail(f"{label}: missing byte sequence {wanted.hex(' ')}")


def verify_panel(nodes: dict[str, dict[str, bytes]], panel_name: str) -> None:
    panel_path = one_node(nodes, panel_name)
    timing_root = panel_path + "/qcom,mdss-dsi-display-timings"
    cfg = PANELS[panel_name]

    for index, (fps, transfer) in MODES.items():
        timing_path = f"{timing_root}/timing@{index}"
        if timing_path not in nodes:
            fail(f"{panel_name}: missing timing@{index}")
        timing = nodes[timing_path]
        label = f"{panel_name}:timing@{index}"
        if cell(timing, "qcom,mdss-dsi-panel-framerate", label) != fps:
            fail(f"{label}: wrong frame rate")
        expected_clock = cfg["clock60"] if index == 0 else (
            1_250_000_000 if index == 1 else 1_500_000_000
        )
        if cell(timing, "qcom,mdss-dsi-panel-clockrate", label) != expected_clock:
            fail(f"{label}: wrong panel clockrate")
        if cell(timing, "qcom,mdss-mdp-transfer-time-us", label) != transfer:
            fail(f"{label}: wrong mdp transfer time")
        if 1_000_000 // transfer != fps:
            fail(f"internal transfer-time tuple is invalid for {fps}Hz")
        for name, expected in PORCHES.items():
            if cell(timing, name, label) != expected:
                fail(f"{label}: wrong {name}")

        phy = prop(timing, "qcom,mdss-dsi-panel-phy-timings", label)
        expected_phy = cfg["phy90"] if index == 2 else cfg["phy60"]
        if phy != expected_phy:
            fail(f"{label}: wrong PHY {phy.hex(' ')}")

        on = prop(timing, "qcom,mdss-dsi-on-command", label)
        switch = prop(timing, "qcom,mdss-dsi-timing-switch-command", label)
        d1 = cfg["d1_90"] if index == 2 else cfg["d1_60"]
        check_contains(on, d1, f"{label}: on-command D1")
        check_contains(switch, d1, f"{label}: timing-switch D1")
        if index == 0:
            check_contains(on, bytes.fromhex("A3 B9 A1 4A 00 1A B8"), label)
            check_contains(switch, bytes.fromhex("A3 B9 A1 4A 00 8A 18"), label)
        elif index == 1:
            check_contains(on, bytes.fromhex("A3 A9 A1 4A 00 1A B8"), label)
            check_contains(switch, bytes.fromhex("A3 A9 A1 4A 00 8A 18"), label)
        else:
            check_contains(on, cfg["on90"], label)
            check_contains(switch, cfg["switch90"], label)

    print(f"OK {panel_name}: 60/75/90Hz clock/transfer/FFC contract")


def verify_image(image_path: Path) -> None:
    image = image_path.read_bytes()
    print(f"SHA256 {hashlib.sha256(image).hexdigest()}  {image_path}")
    verified = False
    for index, blob in enumerate(fdt_blobs(image)):
        nodes = parse_fdt(blob)
        present = {
            path.rsplit("/", 1)[-1]
            for path in nodes
            if not path.startswith("/__local_fixups__/")
        }
        if not set(PANELS).issubset(present):
            continue
        print(f"Verifying DT entry {index}, FDT bytes={len(blob)}")
        for panel_name in PANELS:
            verify_panel(nodes, panel_name)
        verified = True
    if not verified:
        fail("no DT entry contains both Raphael EA8076 panel nodes")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    verify_image(parser.parse_args().image)


if __name__ == "__main__":
    main()
