#!/usr/bin/env python3
"""Builds the Wi-Fi screen: the networks the player knows and sees, tap one to join it.

  python3 tools/splash/make_wifi.py      # -> wifi*.json here, wifi_settings*.rc in the docs

Three documents: wifi_settings (rectangular screens: a header row, then rows filling the
height), wifi_settings_light (the same in a white palette and 1.5x larger, for e-ink) and wifi_settings_round
(240 x 240 round faces: the title centred at the top, three rows across the middle of the
circle, the buttons at the bottom). A round board shows the round one, and an e-ink board the
light one, wherever wifi_settings is asked for.

The document only draws; the player owns Wi-Fi (firmware/rcplayer/main/net.cpp) and fills
the rows in. It lays out with RC's own layout (a header row, then fixed-height rows), so the
tap targets follow the screen at any size; the player fills only the rows that fit.

Named variables, set by the player (docs/RC_SYSTEM_VARIABLES.md):
  system.wifi_status       one line under the title ("Connected to NIXIOT", ...)
  system.wifi_page         "1/2" when the list has more rows than the screen, else ""
  system.wifi_scanning     1 while a scan runs
  system.wifi_<i>_ssid     row i (0..9)
  system.wifi_<i>_state    0 empty, 1 connected, 2 connecting, 3 saved, 4 open (not saved),
                           5 saved but not in range, 6 couldn't connect,
                           7 needs a password (not saved: add it with "wifi add" on the console)
  system.wifi_<i>_signal   0..100
  system.wifi_<i>_lock     1 when the network needs a password
Host actions, hostAction(0, ...):
  wifi.join=<i>   join row i     wifi.scan   scan again     wifi.page   next page
  doc.back        back to the document shown before this one
"""
import json
import pathlib
import subprocess

from make_splash import paint, text, when

HERE = pathlib.Path(__file__).resolve().parent
OUT_DIR = HERE.parent.parent / "documents/system"
ROWS = 10           # slots in the document; the player fills those that fit
HEADER, ROW = 44, 37  # dp at scale 1; the player's rowsThatFit() uses the same numbers
K = 1.0              # the scale of the rectangular layout (S()): 1.5 for the light list
# Palettes. Light is for e-ink (16 greys): white ground, dark ink, outlines rather than
# tinted fills (a light grey fill comes out as a dither pattern), and status colours dark
# enough to read as grey.
DARK = {"BG": "#0B1220", "CARD": "#141C2A", "EDGE": "#26324A", "BUTTON": "#1C2840",
        "TITLE": "#FFFFFF", "INK": "#E6EDF7", "DIM": "#8FA0BE",
        "GREEN": "#4CD964", "AMBER": "#FFC107", "RED": "#FF453A", "CONNECTED_WIDTH": 1.5}
LIGHT = {"BG": "#FFFFFF", "CARD": "#FFFFFF", "EDGE": "#555555", "BUTTON": "#FFFFFF",
         "TITLE": "#111111", "INK": "#111111", "DIM": "#555555",
         "GREEN": "#1B7A34", "AMBER": "#8A5A00", "RED": "#B3261E", "CONNECTED_WIDTH": 2.5}
P = DARK


def use_palette(pal):
    global P, BG, CARD, EDGE, INK, DIM, GREEN, AMBER, RED
    P = pal
    BG, CARD, EDGE, INK, DIM = pal["BG"], pal["CARD"], pal["EDGE"], pal["INK"], pal["DIM"]
    GREEN, AMBER, RED = pal["GREEN"], pal["AMBER"], pal["RED"]


use_palette(DARK)


def host(md):
    return {"type": "hostAction", "actionId": 0, "metadata": md}


def canvas(cmds):
    return {"canvas": {"modifiers": ["fillMaxSize"], "commands": cmds}}


def S(v):
    """A length in the rectangular layout's units, scaled by K (the light list is 1.5x)."""
    r = round(v * K, 2)
    return int(r) if r == int(r) else r


def button_box():
    return {"left": S(4), "top": S(9), "right": f"componentWidth - {S(4)}", "bottom": S(35),
            "rx": S(8), "ry": S(8)}


def button(label, md, width, extra=()):
    """A rounded button filling a fixed-width cell of the header."""
    box = button_box()
    cmds = [paint({"color": P["BUTTON"]}, {"style": "fill"}), {"drawRoundRect": box},
            paint({"color": EDGE}, {"style": "stroke"}, {"width": 1}), {"drawRoundRect": box}]
    cmds += text(label, "componentWidth / 2", S(27), S(13), INK)
    return {"box": {"modifiers": [{"width": S(width)}, "fillMaxHeight", {"onClick": host(md)}],
                    "children": [canvas(list(extra) + cmds)]}}


def header():
    # Scaled up, the buttons leave no room for the status line beside the title: it gets a
    # line of its own (status_line()).
    title = text("Wi-Fi", S(12), S(21) if K == 1 else S(28), S(17), P["TITLE"], -1)
    if K == 1:
        title += text("@system.wifi_status", S(12), S(37), S(11), DIM, -1)
    # The page button only draws when there is more than one page.
    page_box = button_box()
    page = {"box": {"modifiers": [{"width": S(52)}, "fillMaxHeight", {"onClick": host("wifi.page")}],
                    "children": [canvas([when("ne", "@system.wifi_pages", 1, [
                        paint({"color": P["BUTTON"]}, {"style": "fill"}), {"drawRoundRect": page_box},
                        # On white the button needs its outline to show at all.
                        *([paint({"color": EDGE}, {"style": "stroke"}, {"width": 1}),
                           {"drawRoundRect": page_box}] if P is LIGHT else []),
                        *text("@system.wifi_page", "componentWidth / 2", S(27), S(12), INK)])])]}}
    # "Scan" turns into "..." while scanning.
    scan_box = button_box()
    scan = {"box": {"modifiers": [{"width": S(56)}, "fillMaxHeight", {"onClick": host("wifi.scan")}],
                    "children": [canvas([
                        paint({"color": P["BUTTON"]}, {"style": "fill"}), {"drawRoundRect": scan_box},
                        paint({"color": EDGE}, {"style": "stroke"}, {"width": 1}),
                        {"drawRoundRect": scan_box},
                        when("eq", "@system.wifi_scanning", 0,
                             text("Scan", "componentWidth / 2", S(27), S(13), INK)),
                        when("ne", "@system.wifi_scanning", 0,
                             text("···", "componentWidth / 2", S(27), S(13), AMBER))])]}}
    return {"row": {"modifiers": ["fillMaxWidth", {"height": S(HEADER)}], "children": [
        {"box": {"modifiers": [{"horizontalWeight": 1}, "fillMaxHeight"],
                 "children": [canvas(title)]}},
        page, scan, button("Done", "doc.back", 60)]}}


STATUS_LINE = 24   # dp, under the header when scaled up


def status_line():
    return {"box": {"modifiers": ["fillMaxWidth", {"height": STATUS_LINE}], "children": [
        canvas(text("@system.wifi_status", S(12), 16, S(11), DIM, -1))]}}


def bars(x, bottom, i):
    """Four signal bars, lit by system.wifi_<i>_signal; all dim when not in range."""
    sig = f"@system.wifi_{i}_signal"
    cmds = []
    for j, (h, t) in enumerate(((4, 0), (7, 30), (10, 55), (13, 80))):
        cmds += [paint({"color": INK}, {"style": "fill"},
                       {"alpha": f"0.2 + 0.8 * clamp(0, 1, ({sig} - {t}) * 1000)"}),
                 {"drawRoundRect": {"left": S(x + j * 5), "top": S(bottom - h),
                                    "right": S(x + j * 5 + 3), "bottom": S(bottom),
                                    "rx": S(1), "ry": S(1)}}]
    return cmds


def lock(inset, cy):
    """A small padlock, centred `inset` dp in from the right edge: a shackle over a body."""
    X = lambda d: f"componentWidth - {S(inset - d)}"
    return [paint({"color": DIM}, {"style": "stroke"}, {"width": S(1.5)}),
            {"drawArc": {"left": X(-3.5), "top": S(cy - 8), "right": X(3.5), "bottom": S(cy - 1),
                         "startAngle": 180, "sweepAngle": 180}},
            {"drawLine": {"x1": X(-3.5), "y1": S(cy - 4.5), "x2": X(-3.5), "y2": S(cy - 1)}},
            {"drawLine": {"x1": X(3.5), "y1": S(cy - 4.5), "x2": X(3.5), "y2": S(cy - 1)}},
            paint({"color": DIM}, {"style": "fill"}),
            {"drawRoundRect": {"left": X(-5), "top": S(cy - 1), "right": X(5), "bottom": S(cy + 6),
                               "rx": S(1.5), "ry": S(1.5)}}]


# What each state says, and its colour (a palette key).
DETAIL = {1: ("Connected", "GREEN"), 2: ("Connecting…", "AMBER"), 3: ("Saved", "DIM"),
          4: ("Open · no password", "DIM"), 5: ("Saved · not in range", "DIM"),
          6: ("Couldn't connect", "RED"), 7: ("Needs a password · add from computer", "DIM")}
# Shorter labels for the round face's narrower rows.
DETAIL_SHORT = {**DETAIL, 4: ("Open", "DIM"), 7: ("Needs a password", "DIM")}


def row(i, width="fillMaxWidth", inset=8, ssid_size=14, detail_size=10, details=None):
    state = f"@system.wifi_{i}_state"
    card = {"left": S(inset), "top": S(2), "right": f"componentWidth - {S(inset)}",
            "bottom": S(ROW - 2), "rx": S(8), "ry": S(8)}
    body = [paint({"color": CARD}, {"style": "fill"}), {"drawRoundRect": card},
            when("eq", state, 1, [paint({"color": GREEN}, {"style": "stroke"},
                                        {"width": P["CONNECTED_WIDTH"]}),
                                  {"drawRoundRect": card}]),
            when("ne", state, 1, [paint({"color": EDGE}, {"style": "stroke"}, {"width": 1}),
                                  {"drawRoundRect": card}])]
    body += bars(inset + 10, 25, i)
    body += text(f"@system.wifi_{i}_ssid", S(inset + 38), S(17), S(ssid_size), INK, -1)
    for s, (label, color) in (details or DETAIL).items():
        body.append(when("eq", state, s, text(label, S(inset + 38), S(30), S(detail_size), P[color], -1)))
    body.append(when("eq", f"@system.wifi_{i}_lock", 1, lock(inset + 16, 18)))
    size = [width] if isinstance(width, str) else [{"width": S(width)}]
    return {"box": {"modifiers": size + [{"height": S(ROW)}, {"onClick": host(f"wifi.join={i}")}],
                    "children": [canvas([when("gt", state, 0, body)])]}}


def named_variables():
    named = [
        {"name": "system.wifi_status", "type": "string", "value": "Searching…", "export": True},
        {"name": "system.wifi_page", "type": "string", "value": "", "export": True},
        {"name": "system.wifi_pages", "value": 1, "export": True},
        {"name": "system.wifi_scanning", "value": 0, "export": True},
    ]
    for i in range(ROWS):
        named += [
            {"name": f"system.wifi_{i}_ssid", "type": "string", "value": "", "export": True},
            {"name": f"system.wifi_{i}_state", "value": 0, "export": True},
            {"name": f"system.wifi_{i}_signal", "value": 0, "export": True},
            {"name": f"system.wifi_{i}_lock", "value": 0, "export": True},
        ]
    return named


ROUND_ROWS = 3   # rows that fit across the middle of a 240 dp circle


def round_button(label_cmds, md, width):
    box = {"left": 2, "top": 0, "right": "componentWidth - 2", "bottom": "componentHeight",
           "rx": 13, "ry": 13}
    return {"box": {"modifiers": [{"width": width}, "fillMaxHeight", {"onClick": host(md)}],
                    "children": [canvas([paint({"color": P["BUTTON"]}, {"style": "fill"}),
                                         {"drawRoundRect": box},
                                         paint({"color": EDGE}, {"style": "stroke"}, {"width": 1}),
                                         {"drawRoundRect": box}] + label_cmds)]}}


def build_round():
    """240 x 240 dp, round. Everything stays inside the circle: the rows are 192 dp wide
    between y 64 and 172, where the circle is at least 214 dp across, and the buttons sit
    above y 204 (167 dp across)."""
    title = text("Wi-Fi", "componentWidth / 2", 20, 16, P["TITLE"]) + \
        text("@system.wifi_status", "componentWidth / 2", 36, 10, DIM)
    mid = "componentWidth / 2"
    label = lambda t, color=None: text(t, mid, 18, 12, color or INK)
    scan = round_button([when("eq", "@system.wifi_scanning", 0, label("Scan")),
                         when("ne", "@system.wifi_scanning", 0, label("···", AMBER))],
                        "wifi.scan", 60)
    # The page button is always there to keep the row centred; it draws only with pages.
    page = {"box": {"modifiers": [{"width": 44}, "fillMaxHeight", {"onClick": host("wifi.page")}],
                    "children": [canvas([when("ne", "@system.wifi_pages", 1, [
                        paint({"color": P["BUTTON"]}, {"style": "fill"}),
                        {"drawRoundRect": {"left": 2, "top": 0, "right": "componentWidth - 2",
                                           "bottom": "componentHeight", "rx": 13, "ry": 13}},
                        *text("@system.wifi_page", mid, 18, 11, INK)])])]}}
    done = round_button(label("Done"), "doc.back", 60)
    buttons = {"row": {"modifiers": [{"height": 26}], "verticalAlignment": "center",
                       "children": [scan, {"box": {"modifiers": [{"width": 4}]}}, page,
                                    {"box": {"modifiers": [{"width": 4}]}}, done]}}
    children = [{"box": {"modifiers": [{"height": 20}]}},
                {"box": {"modifiers": [{"width": 170}, {"height": 44}],
                         "children": [canvas(title)]}}]
    children += [row(i, width=192, inset=4, ssid_size=13, detail_size=9, details=DETAIL_SHORT) for i in range(ROUND_ROWS)]
    children += [{"box": {"modifiers": [{"height": 3}]}}, buttons]
    return {
        "header": {"apiLevel": 7, "width": 240, "height": 240, "profiles": 513,
                   "contentDescription": "Wi-Fi networks (round screen)"},
        "root": [
            {"global": [{"resources": {"variables": named_variables()}}]},
            {"column": {"modifiers": ["fillMaxSize", {"background": BG}],
                        "horizontalAlignment": "center", "children": children}},
        ],
    }


def build(pal=DARK, k=1.0):
    global K
    use_palette(pal)
    K = k
    light = " (light)" if pal is LIGHT else ""
    return {
        "header": {"apiLevel": 7, "width": 320, "height": 240, "profiles": 513,
                   "contentDescription": "Wi-Fi networks" + light},
        "root": [
            {"global": [{"resources": {"variables": named_variables()}}]},
            {"column": {"modifiers": ["fillMaxSize", {"background": BG}],
                        "children": [header()] + ([status_line()] if K != 1 else [])
                                    + [row(i) for i in range(ROWS)]}},
        ],
    }


if __name__ == "__main__":
    for name, make in (("wifi_settings", lambda: build()),
                       # E-ink: read at arm's length on a big, low-contrast panel.
                       ("wifi_settings_light", lambda: build(LIGHT, 1.5)),
                       ("wifi_settings_round", lambda: (build(), build_round())[1])):
        doc = make()
        src = HERE / (name.replace("_settings", "") + ".json")
        src.write_text(json.dumps(doc, indent=1) + "\n")
        subprocess.run(["node", str(HERE / "compile.mjs"), str(src), str(OUT_DIR / f"{name}.rc")],
                       check=True)
