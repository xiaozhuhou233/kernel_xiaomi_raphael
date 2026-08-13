#!/usr/bin/env python3
"""Add Soviet's hardware-tested 90 Hz mode to Raphael SS EA8076 panels."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DISPLAY = ROOT / "arch/arm64/boot/dts/qcom/xiaomi/overlay/common/display"


PANELS = {
    "dsi-panel-ss-fhd-ea8076-cmd.dtsi": {
        "v_pulse_60": 20,
        "clock_60": 1_100_000_000,
        "d1_60": "0F",
        "d1_90": "0D",
        "on_tail_90": "13 98",
        "switch_tail_90": "65 45",
        "phy_60": "00 24 0A 0A 26 25 09 0A 06 03 04 00 1E 1A",
        "phy_90": "00 31 0D 0D 2B 28 0D 0E 09 02 04 00 27 1C",
    },
    "dsi-panel-ss-fhd-ea8076-global-cmd.dtsi": {
        "v_pulse_60": 27,
        "clock_60": 1_103_000_000,
        "d1_60": "11",
        "d1_90": "0F",
        "on_tail_90": "13 A6",
        "switch_tail_90": "65 8B",
        "phy_60": "00 24 0A 0A 26 24 0A 0A 06 03 04 00 1E 1A",
        "phy_90": "00 30 0D 0D 2A 28 0C 0D 09 02 04 00 27 1C",
    },
}


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one occurrence, found {count}: {old!r}")
    return text.replace(old, new, 1)


def timing_block(text: str) -> tuple[int, int, str]:
    start = text.find("\t\t\ttiming@0{")
    if start < 0:
        raise RuntimeError("timing@0 block not found")

    brace = text.find("{", start)
    depth = 0
    end = None
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                semicolon = text.find(";", pos)
                if semicolon < 0:
                    raise RuntimeError("timing@0 block has no closing semicolon")
                end = semicolon + 1
                break

    if end is None:
        raise RuntimeError("unterminated timing@0 block")
    return start, end, text[start:end]


def add_switch_command(block: str, d1: str) -> str:
    anchor = '\t\t\t\tqcom,mdss-dsi-on-command-state = "dsi_lp_mode";'
    switch = f'''\n
\t\t\t\tqcom,mdss-dsi-timing-switch-command = [
\t\t\t\t\t39 00 00 00 00 00 03 F0 5A 5A
\t\t\t\t\t39 00 00 00 00 00 03 FC 5A 5A
\t\t\t\t\t39 00 00 00 00 00 02 B0 23
\t\t\t\t\t39 00 00 00 00 00 02 D1 {d1}
\t\t\t\t\t39 00 00 00 00 00 0C E9 11 55 A6 75 A3 B9 A1 4A 00 8A 18
\t\t\t\t\t39 00 00 00 00 00 03 F0 A5 A5
\t\t\t\t\t39 01 00 00 00 00 03 FC A5 A5];
\t\t\t\tqcom,mdss-dsi-timing-switch-command-state = "dsi_lp_mode";'''
    return replace_once(block, anchor, anchor + switch, "timing switch insertion")


def make_soviet_60hz(block: str, cfg: dict[str, object]) -> str:
    replacements = (
        ("qcom,mdss-dsi-h-front-porch = <64>;", "qcom,mdss-dsi-h-front-porch = <42>;"),
        ("qcom,mdss-dsi-h-back-porch = <64>;", "qcom,mdss-dsi-h-back-porch = <42>;"),
        ("qcom,mdss-dsi-h-pulse-width = <20>;", "qcom,mdss-dsi-h-pulse-width = <12>;"),
        ("qcom,mdss-dsi-v-back-porch = <64>;", "qcom,mdss-dsi-v-back-porch = <42>;"),
        ("qcom,mdss-dsi-v-front-porch = <64>;", "qcom,mdss-dsi-v-front-porch = <42>;"),
        (f"qcom,mdss-dsi-v-pulse-width = <{cfg['v_pulse_60']}>;", "qcom,mdss-dsi-v-pulse-width = <12>;"),
    )
    for old, new in replacements:
        block = replace_once(block, old, new, "Soviet 60 Hz timing")

    jitter = "\t\t\t\tqcom,mdss-dsi-panel-jitter = <0x5 0x1>;"
    extra = f'''{jitter}
\t\t\t\tqcom,mdss-dsi-panel-phy-timings = [{cfg['phy_60']}];
\t\t\t\tqcom,display-topology = <1 0 1>;
\t\t\t\tqcom,default-topology-index = <0>;'''
    block = replace_once(block, jitter, extra, "Soviet 60 Hz PHY timing")
    return add_switch_command(block, str(cfg["d1_60"]))


def make_90hz(block: str, cfg: dict[str, object]) -> str:
    high = replace_once(block, "timing@0{", "timing@1{", "timing node")

    replacements = (
        ("qcom,mdss-dsi-panel-framerate = <60>;", "qcom,mdss-dsi-panel-framerate = <90>;"),
        (f"qcom,mdss-dsi-panel-clockrate = <{cfg['clock_60']}>;", "qcom,mdss-dsi-panel-clockrate = <1500000000>;"),
    )
    for old, new in replacements:
        high = replace_once(high, old, new, "90 Hz timing")

    high = high.replace(f"02 D1 {cfg['d1_60']}", f"02 D1 {cfg['d1_90']}")
    if high.count(f"02 D1 {cfg['d1_90']}") < 2:
        raise RuntimeError("90 Hz D1 commands were not updated")

    high = replace_once(
        high,
        "A3 B9 A1 4A 00 1A B8",
        f"A3 B9 A1 4A 00 {cfg['on_tail_90']}",
        "90 Hz panel-on oscillator",
    )
    high = replace_once(
        high,
        "A3 B9 A1 4A 00 8A 18",
        f"A3 A9 A1 4A 00 {cfg['switch_tail_90']}",
        "90 Hz timing switch oscillator",
    )

    high = replace_once(
        high,
        f"qcom,mdss-dsi-panel-phy-timings = [{cfg['phy_60']}];",
        f"qcom,mdss-dsi-panel-phy-timings = [{cfg['phy_90']}];",
        "90 Hz PHY timing",
    )
    return high


def patch_panel(path: Path, cfg: dict[str, object]) -> None:
    text = path.read_text()
    if "timing@1{" in text:
        raise RuntimeError(f"{path}: timing@1 already exists; refusing to duplicate it")

    start, end, base = timing_block(text)
    base = make_soviet_60hz(base, cfg)
    high = make_90hz(base, cfg)
    text = text[:start] + base + "\n\n" + high + text[end:]
    path.write_text(text)
    print(f"Enabled 60/90 Hz modes in {path}")


def main() -> None:
    for filename, cfg in PANELS.items():
        patch_panel(DISPLAY / filename, cfg)


if __name__ == "__main__":
    main()
