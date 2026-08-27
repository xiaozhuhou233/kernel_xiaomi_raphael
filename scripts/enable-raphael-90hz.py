#!/usr/bin/env python3
"""Transplant HyperOS3-verified 60/75/90 Hz timings to Raphael SS EA8076 panels.

HyperOS3 ships a working high-refresh configuration on the same EA8076 panel
that enables direct 60 -> 90 Hz switching with no stepping.  The key
differences from the stock/KamiOC configuration:

  - 60 Hz base porches are 42/42/12 (symmetric) instead of 64/64/20
  - a panel-phy-timings property is added to the 60 Hz node
  - 75 Hz intermediate at 1.25 GHz with A9 oscillator (1A B8 / 8A 18 FFC)
  - 90 Hz at 1.5 GHz (not 1.65 GHz) with a distinct FFC:
      cmd panel:    ON B9 oscillator 13 98 / switch A9 oscillator 65 45
      global panel: ON B9 oscillator 13 A6 / switch A9 oscillator 65 8B
  - 90 Hz D1 byte is 0D (cmd) / 0F (global), PHY timings differ per rate
  - the old EvolutionX command-mode driver requires an explicit
    qcom,mdss-mdp-transfer-time-us; values are chosen so its integer
    1000000 / transfer calculation yields exactly 60/75/90 Hz

This script rewrites the timing section of each panel DTSI to match the
HyperOS3 dtbo exactly, so 60 -> 90 switching works directly and survives
display power cycles.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
DISPLAY = ROOT / "arch/arm64/boot/dts/qcom/xiaomi/overlay/common/display"


# Per-panel configuration: the 60 Hz base timing values that HyperOS3 uses,
# plus the 90 Hz D1, PHY and FFC override.  The 90 Hz FFC tail differs between
# the cmd (13 98 / 65 45) and global-cmd (13 A6 / 65 8B) panels in the
# HyperOS3 dtbo.
PANELS = {
    "dsi-panel-ss-fhd-ea8076-cmd.dtsi": {
        "d1_60": "0F",
        "d1_75": "0F",
        "d1_90": "0D",
        "clock_60": 1_100_000_000,
        "phy_60": "00 24 0A 0A 26 25 09 0A 06 03 04 00 1E 1A",
        "phy_90": "00 31 0D 0D 2B 28 0D 0E 09 02 04 00 27 1C",
        "ffc_90_on": "A3 B9 A1 4A 00 13 98",
        "ffc_90_sw": "A3 A9 A1 4A 00 65 45",
    },
    "dsi-panel-ss-fhd-ea8076-global-cmd.dtsi": {
        "d1_60": "11",
        "d1_75": "11",
        "d1_90": "0F",
        "clock_60": 1_103_000_000,
        "phy_60": "00 24 0A 0A 26 24 0A 0A 06 03 04 00 1E 1A",
        "phy_90": "00 30 0D 0D 2A 28 0C 0D 09 02 04 00 27 1C",
        "ffc_90_on": "A3 B9 A1 4A 00 13 A6",
        "ffc_90_sw": "A3 A9 A1 4A 00 65 8B",
    },
}

# HyperOS3 timing nodes.  The 60 Hz node replaces the stock base; 75 and 90
# are added as new nodes.  All share the 42/42/12 porch set.
#   ffc_on:  the E9 ... tail in the ON command (oscillator byte + FFC tail)
#   ffc_sw:  the E9 ... tail in the timing-switch command
# The 90 Hz FFC tail is per-panel (see PANELS.ffc_90_on / ffc_90_sw).
#
# The HyperOS3 dtbo omits this property because its driver uses a different
# default.  EvolutionX's old command-mode driver falls back to 14000 us, which
# makes every generated mode calculate the wrong pixel clock.  Use the largest
# integer transfer time that still gives the requested rate after the driver's
# integer division: floor(1000000 / xfer) == fps.
MODES = (
    {"node": 0, "fps": 60, "clock_from_cfg": "clock_60",
     "clock": None, "xfer": 16666,
     "d1_from_cfg": "d1_60",
     "ffc_on": "A3 B9 A1 4A 00 1A B8",
     "ffc_sw": "A3 B9 A1 4A 00 8A 18",
     "phy_from_cfg": "phy_60",
     "replace_base": True},
    {"node": 1, "fps": 75, "clock": 1_250_000_000,
     "xfer": 13333,
     "d1_from_cfg": "d1_75",
     "ffc_on": "A3 A9 A1 4A 00 1A B8",
     "ffc_sw": "A3 A9 A1 4A 00 8A 18",
     "phy_from_cfg": "phy_60",
     "replace_base": False},
    {"node": 2, "fps": 90, "clock": 1_500_000_000,
     "xfer": 11111,
     "d1_from_cfg": "d1_90",
     "ffc_on_from_cfg": "ffc_90_on",
     "ffc_sw_from_cfg": "ffc_90_sw",
     "phy_from_cfg": "phy_90",
     "replace_base": False},
)

PORCHES = {"h_front": 42, "h_back": 42, "h_pulse": 12,
           "v_back": 42, "v_front": 42, "v_pulse": 12}


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError("%s: expected one occurrence, found %d: %r" % (label, count, old))
    return text.replace(old, new, 1)


def timing_block(text):
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


def add_switch_command(block, d1):
    anchor = '\t\t\t\tqcom,mdss-dsi-on-command-state = "dsi_lp_mode";'
    switch = '\n\n\t\t\t\tqcom,mdss-dsi-timing-switch-command = [\n' \
        '\t\t\t\t\t39 00 00 00 00 00 03 F0 5A 5A\n' \
        '\t\t\t\t\t39 00 00 00 00 00 03 FC 5A 5A\n' \
        '\t\t\t\t\t39 00 00 00 00 00 02 B0 23\n' \
        '\t\t\t\t\t39 00 00 00 00 00 02 D1 ' + d1 + '\n' \
        '\t\t\t\t\t39 00 00 00 00 00 0C E9 11 55 A6 75 A3 B9 A1 4A 00 8A 18\n' \
        '\t\t\t\t\t39 00 00 00 00 00 03 F0 A5 A5\n' \
        '\t\t\t\t\t39 01 00 00 00 00 03 FC A5 A5];\n' \
        '\t\t\t\tqcom,mdss-dsi-timing-switch-command-state = "dsi_lp_mode";'
    return replace_once(block, anchor, anchor + switch, "timing switch insertion")


def apply_mode(block, cfg, mode):
    """Apply a timing mode to a cloned timing block."""
    porches = PORCHES
    phy = cfg[mode["phy_from_cfg"]]
    d1 = cfg[mode["d1_from_cfg"]]
    clock = mode.get("clock") or cfg[mode["clock_from_cfg"]]

    # Porches - replace stock 64/64/20 values
    replacements = [
        ("qcom,mdss-dsi-h-front-porch = <64>;", "qcom,mdss-dsi-h-front-porch = <%d>;" % porches["h_front"]),
        ("qcom,mdss-dsi-h-back-porch = <64>;", "qcom,mdss-dsi-h-back-porch = <%d>;" % porches["h_back"]),
        ("qcom,mdss-dsi-h-pulse-width = <20>;", "qcom,mdss-dsi-h-pulse-width = <%d>;" % porches["h_pulse"]),
        ("qcom,mdss-dsi-v-back-porch = <64>;", "qcom,mdss-dsi-v-back-porch = <%d>;" % porches["v_back"]),
        ("qcom,mdss-dsi-v-front-porch = <64>;", "qcom,mdss-dsi-v-front-porch = <%d>;" % porches["v_front"]),
    ]
    # v-pulse-width: cnb cmd=20, global=27; HyperOS3 uses 12 for all
    for vp in (20, 27):
        old = "qcom,mdss-dsi-v-pulse-width = <%d>;" % vp
        if old in block:
            replacements.append((old, "qcom,mdss-dsi-v-pulse-width = <%d>;" % porches["v_pulse"]))
            break

    # Framerate
    replacements.append(("qcom,mdss-dsi-panel-framerate = <60>;",
                         "qcom,mdss-dsi-panel-framerate = <%d>;" % mode["fps"]))

    # The old driver derives its command-mode pixel clock from this property.
    # Strip the cloned value first, then add the mode-specific value exactly
    # once next to the clockrate.
    block = re.sub(r'\n\t\t\t\tqcom,mdss-mdp-transfer-time-us = <\d+>;', '', block)
    replacements.append(("qcom,mdss-dsi-panel-clockrate = <%d>;" % cfg["clock_60"],
                         "qcom,mdss-dsi-panel-clockrate = <%d>;\n"
                         "\t\t\t\tqcom,mdss-mdp-transfer-time-us = <%d>;" %
                         (clock, mode["xfer"])))

    for old, new in replacements:
        if old in block:
            block = replace_once(block, old, new, "%d Hz timing" % mode["fps"])

    # D1 oscillator byte
    block = block.replace("02 D1 %s" % cfg["d1_60"], "02 D1 %s" % d1)

    # FFC tails: replace the stock B9/1AB8 (ON) and B9/8A18 (switch)
    if "ffc_on_from_cfg" in mode:
        ffc_on = cfg[mode["ffc_on_from_cfg"]]
        ffc_sw = cfg[mode["ffc_sw_from_cfg"]]
    else:
        ffc_on = mode["ffc_on"]
        ffc_sw = mode["ffc_sw"]
    if "A3 B9 A1 4A 00 1A B8" in block:
        block = replace_once(block, "A3 B9 A1 4A 00 1A B8", ffc_on, "%d Hz ON FFC" % mode["fps"])
    if "A3 B9 A1 4A 00 8A 18" in block:
        block = replace_once(block, "A3 B9 A1 4A 00 8A 18", ffc_sw, "%d Hz switch FFC" % mode["fps"])

    # PHY timings: the base 60 Hz block has no phy-timings property; we add
    # it (plus topology) by expanding the jitter line.  For 75/90 Hz nodes
    # cloned from the already-modified 60 Hz block, the phy-timings and
    # topology lines are already present from the clone, so we only need to
    # replace the phy-timings value; the topology stays the same.
    jitter = "\t\t\t\tqcom,mdss-dsi-panel-jitter = <0x5 0x1>;"
    if "qcom,mdss-dsi-panel-phy-timings" not in block:
        extra = jitter + "\n\t\t\t\tqcom,mdss-dsi-panel-phy-timings = [" + phy + "];\n" \
            "\t\t\t\tqcom,display-topology = <1 0 1>;\n" \
            "\t\t\t\tqcom,default-topology-index = <0>;"
        block = replace_once(block, jitter, extra, "%d Hz PHY timing" % mode["fps"])
    else:
        # Replace existing phy-timings value (cloned from 60 Hz)
        block = re.sub(r'qcom,mdss-dsi-panel-phy-timings = \[[^\]]*\];',
                       'qcom,mdss-dsi-panel-phy-timings = [%s];' % phy, block, count=1)

    return block


def patch_panel(path, cfg):
    text = path.read_text()
    if "timing@1{" in text:
        raise RuntimeError("%s: timing@1 already exists" % path)

    start, end, base = timing_block(text)

    # Add the timing-switch command to the base 60 Hz node
    base = add_switch_command(base, str(cfg["d1_60"]))

    # Apply 60 Hz base modifications (porches, phy, FFC stays B9/1AB8)
    base_60 = apply_mode(base, cfg, MODES[0])

    # Build 75 Hz and 90 Hz nodes from the modified base
    modes_extra = []
    for mode in MODES[1:]:
        cloned = base_60.replace("timing@0{", "timing@%d{" % mode["node"], 1)
        cloned = apply_mode(cloned, cfg, mode)
        modes_extra.append(cloned)

    text = text[:start] + base_60 + "\n\n" + "\n\n".join(modes_extra) + text[end:]
    validate_panel(text, path, cfg)
    path.write_text(text)
    print("Enabled 60/75/90 Hz modes in %s" % path)


def validate_panel(text, path, cfg):
    for mode in MODES:
        marker = "timing@%d{" % mode["node"]
        start = text.find(marker)
        if start < 0:
            raise RuntimeError("%s: missing %s" % (path, marker))
        end = text.find("\n\t\t\t};", start)
        if end < 0:
            raise RuntimeError("%s: unterminated %s" % (path, marker))
        block = text[start:end]
        expected = (
            "qcom,mdss-dsi-panel-framerate = <%d>;" % mode["fps"],
            "qcom,mdss-mdp-transfer-time-us = <%d>;" % mode["xfer"],
        )
        missing = [item for item in expected if item not in block]
        if missing:
            raise RuntimeError("%s: %s missing %s" % (path, marker, missing))
        if 1000000 // mode["xfer"] != mode["fps"]:
            raise RuntimeError("%s: invalid transfer-time for %d Hz" %
                               (path, mode["fps"]))


def main():
    for filename, cfg in PANELS.items():
        patch_panel(DISPLAY / filename, cfg)


if __name__ == "__main__":
    main()
