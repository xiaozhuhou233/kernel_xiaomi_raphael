#!/usr/bin/env python3
"""Add a 90 Hz mode to Raphael SS EA8076 panels.

The EA8076 DDIC uses two oscillator/FFC configurations: B9 for 60 Hz and
A9 for the high-refresh range.  Every previous attempt to switch to 90 Hz
at runtime (DMS timing-switch, bridged DMS, force_update, pre_switch)
failed because the DDIC cannot lock a large link-clock step together with
an oscillator change in one go: a direct B9 -> A9 switch with a 1.1 ->
1.65 GHz clock jump never locks, and the panel silently stays at 60 Hz.

What does work (verified on Bool-X, which ships 90 Hz on this panel): the
panel is *initialized* at the target rate during bring-up, with the B9
oscillator command and a 1.65 GHz link clock.  The DDIC accepts B9 at
1.65 GHz on cold start.  This transformer therefore adds a 90 Hz timing
node that keeps the stock 60 Hz ON command (B9 FFC) byte-for-byte and
only raises the link clock, frame rate, porches and PHY timings to the
Bool-X 90 Hz set.

The refresh switch itself is handled by the driver: a rate change is
forced through the full panel re-init path (cold start), never through
the light DMS timing-switch, so 90 Hz engages and survives power cycles.
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
        "phy_90": "00 24 0A 0A 26 25 09 0A 06 02 04 00 1E 1A",
    },
    "dsi-panel-ss-fhd-ea8076-global-cmd.dtsi": {
        "v_pulse_60": 27,
        "clock_60": 1_103_000_000,
        "d1_60": "11",
        "d1_90": "0F",
        "phy_90": "00 24 0A 0A 26 25 09 0A 06 02 04 00 1E 1A",
    },
}

# Only 60 (base timing@0) and 90 Hz.  The intermediate 72/84 Hz modes are
# dropped: they share the A9 high-refresh FFC while running at much lower
# clocks, desynchronizing the DDIC oscillator from the link rate, and they
# only served as manual stepping stones for the broken runtime switch.
MODES = (
    {"node": 1, "fps": 90, "clock": 1_650_000_000, "xfer": 10_108,
     # Bool-X 90 Hz porch set (paired with B9 FFC @ 1.65 GHz cold start).
     "porches": {"h_front": 32, "h_back": 16, "h_pulse": 16,
                 "v_back": 16, "v_front": 8, "v_pulse": 8},
     "phy": "00 24 0A 0A 26 25 09 0A 06 02 04 00 1E 1A"},
)


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
    # The timing-switch FFC stays byte-identical to the ON-command FFC
    # (B9, 1A B8 tail).  The 8A 18 tail latches the DDIC oscillator at
    # ~52 Hz regardless of the link rate.  The driver does not use this
    # command for 60<->90 switches (it forces a full panel re-init), but
    # keeping it correct prevents any DMS path from corrupting the state.
    switch = f'''\n
\t\t\t\tqcom,mdss-dsi-timing-switch-command = [
\t\t\t\t\t39 00 00 00 00 00 03 F0 5A 5A
\t\t\t\t\t39 00 00 00 00 00 03 FC 5A 5A
\t\t\t\t\t39 00 00 00 00 00 02 B0 23
\t\t\t\t\t39 00 00 00 00 00 02 D1 {d1}
\t\t\t\t\t39 00 00 00 00 00 0C E9 11 55 A6 75 A3 B9 A1 4A 00 1A B8
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

    # The 90 Hz timing keeps the stock ON command untouched: the DDIC
    # accepts the B9 oscillator at the 1.65 GHz link rate when the panel
    # is brought up (Bool-X verified).  Do NOT switch the FFC to the A9
    # oscillator - that is what broke the runtime switch and required the
    # 60 -> 72 -> 90 stepping.

    jitter = "\t\t\t\tqcom,mdss-dsi-panel-jitter = <0x5 0x1>;"
    extra = f'''{jitter}
\t\t\t\tqcom,mdss-dsi-panel-phy-timings = [{phy}];
\t\t\t\tqcom,display-topology = <1 0 1>;
\t\t\t\tqcom,default-topology-index = <0>;'''
    high = replace_once(high, jitter, extra, f"{mode['fps']} Hz PHY timing")
    return high


def patch_panel(path: Path, cfg: dict[str, object]) -> None:
    text = path.read_text()
    if "timing@1{" in text:
        raise RuntimeError(f"{path}: timing@1 already exists; refusing to duplicate it")

    start, end, base = timing_block(text)
    base = add_switch_command(base, str(cfg["d1_60"]))
    high_modes = [make_high_refresh(base, cfg, mode) for mode in MODES]
    text = text[:start] + base + "\n\n" + "\n\n".join(high_modes) + text[end:]
    path.write_text(text)
    print(f"Enabled 60/90 Hz modes in {path}")


def main() -> None:
    for filename, cfg in PANELS.items():
        patch_panel(DISPLAY / filename, cfg)


if __name__ == "__main__":
    main()
