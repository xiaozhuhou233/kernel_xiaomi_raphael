#!/usr/bin/env python3
"""Add KamiOC-derived 72/84/90 Hz modes to Raphael SS EA8076 panels.

KamiOC's old-tree DTBO cannot target the EvolutionX overlay nodes directly.
This transformer keeps the current stable 60 Hz node and transplants KamiOC's
known-working high-refresh porches, clocks and oscillator commands.  Explicit
transfer times emulate the newer DSI clock calculation in the older driver.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DISPLAY = ROOT / "arch/arm64/boot/dts/qcom/xiaomi/overlay/common/display"


PANELS = {
    "dsi-panel-ss-fhd-ea8076-cmd.dtsi": {
        "v_pulse_60": 20,
        "clock_60": 1_100_000_000,
        "d1_60": "0F",
        "d1_90": "0F",
        "phy_90": "00 24 0A 0A 26 25 09 0A 06 03 04 00 1E 1A",
    },
    "dsi-panel-ss-fhd-ea8076-global-cmd.dtsi": {
        "v_pulse_60": 27,
        "clock_60": 1_103_000_000,
        "d1_60": "11",
        "d1_90": "0F",
        "phy_90": "00 24 0A 0A 26 25 09 0A 06 03 04 00 1E 1A",
    },
}

MODES = (
    {"node": 1, "fps": 72, "clock": 1_200_000_000, "xfer": 12_635},
    {"node": 2, "fps": 84, "clock": 1_400_000_000, "xfer": 10_829},
    {"node": 3, "fps": 90, "clock": 1_500_000_000, "xfer": 10_108},
)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one occurrence, found {count}: {old!r}")
    return text.replace(old, new, 1)


def timing_block(text: str, node: int = 0) -> tuple[int, int, str]:
    marker = f"\t\t\ttiming@{node}{{"
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"timing@{node} block not found")

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
                    raise RuntimeError(
                        f"timing@{node} block has no closing semicolon"
                    )
                end = semicolon + 1
                break

    if end is None:
        raise RuntimeError(f"unterminated timing@{node} block")
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


def make_high_refresh(
    block: str, cfg: dict[str, object], mode: dict[str, int]
) -> str:
    high = replace_once(
        block, "timing@0{", f"timing@{mode['node']}{{", "timing node"
    )

    porches = mode.get(
        "porches",
        {"h_front": 22, "h_back": 16, "h_pulse": 16,
         "v_back": 22, "v_front": 16, "v_pulse": 16},
    )
    phy = mode.get("phy", cfg["phy_90"])

    replacements = (
        ("qcom,mdss-dsi-h-front-porch = <64>;",
         f"qcom,mdss-dsi-h-front-porch = <{porches['h_front']}>;"),
        ("qcom,mdss-dsi-h-back-porch = <64>;",
         f"qcom,mdss-dsi-h-back-porch = <{porches['h_back']}>;"),
        ("qcom,mdss-dsi-h-pulse-width = <20>;",
         f"qcom,mdss-dsi-h-pulse-width = <{porches['h_pulse']}>;"),
        ("qcom,mdss-dsi-v-back-porch = <64>;",
         f"qcom,mdss-dsi-v-back-porch = <{porches['v_back']}>;"),
        ("qcom,mdss-dsi-v-front-porch = <64>;",
         f"qcom,mdss-dsi-v-front-porch = <{porches['v_front']}>;"),
        (f"qcom,mdss-dsi-v-pulse-width = <{cfg['v_pulse_60']}>;",
         f"qcom,mdss-dsi-v-pulse-width = <{porches['v_pulse']}>;"),
        ("qcom,mdss-dsi-panel-framerate = <60>;", f"qcom,mdss-dsi-panel-framerate = <{mode['fps']}>;"),
        (
            f"qcom,mdss-dsi-panel-clockrate = <{cfg['clock_60']}>;",
            f"qcom,mdss-dsi-panel-clockrate = <{mode['clock']}>;\n"
            f"\t\t\t\tqcom,mdss-mdp-transfer-time-us = <{mode['xfer']}>;",
        ),
    )
    for old, new in replacements:
        high = replace_once(high, old, new, f"{mode['fps']} Hz timing")

    high = high.replace(f"02 D1 {cfg['d1_60']}", f"02 D1 {cfg['d1_90']}")
    if high.count(f"02 D1 {cfg['d1_90']}") < 2:
        raise RuntimeError(f"{mode['fps']} Hz D1 commands were not updated")

    high = replace_once(
        high,
        "A3 B9 A1 4A 00 1A B8",
        "A3 A9 A1 4A 00 1A B8",
        "KamiOC high-refresh panel-on oscillator",
    )
    high = replace_once(
        high,
        "A3 B9 A1 4A 00 8A 18",
        "A3 A9 A1 4A 00 8A 18",
        "KamiOC high-refresh timing switch oscillator",
    )

    jitter = "\t\t\t\tqcom,mdss-dsi-panel-jitter = <0x5 0x1>;"
    extra = f'''{jitter}
\t\t\t\tqcom,mdss-dsi-panel-phy-timings = [{phy}];
\t\t\t\tqcom,display-topology = <1 0 1>;
\t\t\t\tqcom,default-topology-index = <0>;'''
    high = replace_once(high, jitter, extra, f"{mode['fps']} Hz PHY timing")
    return high


def validate_panel(text: str, path: Path, cfg: dict[str, object]) -> None:
    _, _, high90 = timing_block(text, 3)
    expected_90 = (
        "qcom,mdss-dsi-panel-framerate = <90>;",
        "qcom,mdss-dsi-panel-clockrate = <1500000000>;",
        "qcom,mdss-mdp-transfer-time-us = <10108>;",
        "qcom,mdss-dsi-h-front-porch = <22>;",
        "qcom,mdss-dsi-h-back-porch = <16>;",
        "qcom,mdss-dsi-h-pulse-width = <16>;",
        "qcom,mdss-dsi-v-back-porch = <22>;",
        "qcom,mdss-dsi-v-front-porch = <16>;",
        "qcom,mdss-dsi-v-pulse-width = <16>;",
        f"qcom,mdss-dsi-panel-phy-timings = [{cfg['phy_90']}];",
        "A3 A9 A1 4A 00 1A B8",
        "A3 A9 A1 4A 00 8A 18",
    )
    expected_step = (
        "xiaomi,mdss-dsi-step-refresh-switch;",
        "xiaomi,mdss-dsi-step-refresh-base-rate = <60>;",
        "xiaomi,mdss-dsi-step-refresh-bridge-rate = <72>;",
        "xiaomi,mdss-dsi-step-refresh-target-rate = <90>;",
    )

    missing = [item for item in expected_90 if item not in high90]
    missing.extend(item for item in expected_step if item not in text)
    if missing:
        raise RuntimeError(f"{path}: generated true-90 validation failed: {missing}")


def patch_panel(path: Path, cfg: dict[str, object]) -> None:
    text = path.read_text()
    if "timing@1{" in text:
        raise RuntimeError(f"{path}: timing@1 already exists; refusing to duplicate it")

    model = '\t\tqcom,mdss-dsi-panel-model = "SAMSUNG FHD EA8076 CMD PANEL";'
    step_refresh = '''
\t\txiaomi,mdss-dsi-step-refresh-switch;
\t\txiaomi,mdss-dsi-step-refresh-base-rate = <60>;
\t\txiaomi,mdss-dsi-step-refresh-bridge-rate = <72>;
\t\txiaomi,mdss-dsi-step-refresh-target-rate = <90>;'''
    text = replace_once(
        text, model, model + step_refresh, "step refresh configuration"
    )

    start, end, base = timing_block(text)
    base = add_switch_command(base, str(cfg["d1_60"]))
    high_modes = [make_high_refresh(base, cfg, mode) for mode in MODES]
    text = text[:start] + base + "\n\n" + "\n\n".join(high_modes) + text[end:]
    validate_panel(text, path, cfg)
    path.write_text(text)
    print(f"Enabled 60/72/84/90 Hz modes in {path}")


def main() -> None:
    for filename, cfg in PANELS.items():
        patch_panel(DISPLAY / filename, cfg)


if __name__ == "__main__":
    main()
