#!/usr/bin/env python3
"""Builds the launcher: every document on the device, Wi-Fi first; tap one to open it.

  python3 tools/splash/make_launcher.py   # -> launcher*.json here, launcher*.rc in the docs

Two documents: launcher (dark, rectangular screens) and launcher_light (white and 1.5x
larger, for e-ink; an e-ink board shows it wherever launcher is asked for). The geometry is
the Wi-Fi screen's (make_wifi.py: a 44 dp header, then 37 dp rows, and on the light one a
status line under the header), so the player's rowsThatFit() serves both.

The document only draws; the Muse gadget (muse_rc.cpp) fills the rows in.

Named variables, set by the player:
  system.launch_status     one line under the title ("Wi-Fi NIXIOT · Muse connected")
  system.launch_wifi       the Wi-Fi row's second line ("NIXIOT", "Not connected", ...)
  system.launch_page       "1/2" when the list has more rows than the screen, else ""
  system.launch_pages      the number of pages
  system.launch_<i>_kind   row i (0..15): 0 empty, 1 Wi-Fi, 2 a document, 3 the pinned one
  system.launch_<i>_name   its title
  system.launch_<i>_detail its second line ("Pinned", "Built in", ...)
Host actions, hostAction(0, ...):
  launch.open=<i>   open row i     launch.page   next page
  launch.back       the pinned document (the start screen when nothing is pinned)
"""
import json
import pathlib
import subprocess

from make_splash import paint, text, when
from make_wifi import DARK, LIGHT, HEADER, ROW

ROWS = 16   # slots; the player fills those that fit (10 on the Wi-Fi screen)

HERE = pathlib.Path(__file__).resolve().parent
OUT_DIR = HERE.parent.parent / "documents/system"
K = 1.0
P = DARK
STATUS_LINE = 24   # dp, under the header when scaled up (as make_wifi.py)


def S(v):
    r = round(v * K, 2)
    return int(r) if r == int(r) else r


def host(md):
    return {"type": "hostAction", "actionId": 0, "metadata": md}


def canvas(cmds):
    return {"canvas": {"modifiers": ["fillMaxSize"], "commands": cmds}}


def button_box():
    return {"left": S(4), "top": S(9), "right": f"componentWidth - {S(4)}", "bottom": S(35),
            "rx": S(8), "ry": S(8)}


def outlined(box):
    return [paint({"color": P["BUTTON"]}, {"style": "fill"}), {"drawRoundRect": box},
            paint({"color": P["EDGE"]}, {"style": "stroke"}, {"width": 1}), {"drawRoundRect": box}]


def header():
    title = text("Documents", S(12), S(21) if K == 1 else S(28), S(17), P["TITLE"], -1)
    if K == 1:
        title += text("@system.launch_status", S(12), S(37), S(11), P["DIM"], -1)
    page = {"box": {"modifiers": [{"width": S(52)}, "fillMaxHeight", {"onClick": host("launch.page")}],
                    "children": [canvas([when("ne", "@system.launch_pages", 1, [
                        *outlined(button_box()),
                        *text("@system.launch_page", "componentWidth / 2", S(27), S(12), P["INK"])])])]}}
    back = {"box": {"modifiers": [{"width": S(60)}, "fillMaxHeight", {"onClick": host("launch.back")}],
                    "children": [canvas(outlined(button_box()) +
                                        text("Back", "componentWidth / 2", S(27), S(13), P["INK"]))]}}
    return {"row": {"modifiers": ["fillMaxWidth", {"height": S(HEADER)}], "children": [
        {"box": {"modifiers": [{"horizontalWeight": 1}, "fillMaxHeight"], "children": [canvas(title)]}},
        page, back]}}


def status_line():
    return {"box": {"modifiers": ["fillMaxWidth", {"height": STATUS_LINE}], "children": [
        canvas(text("@system.launch_status", S(12), 16, S(11), P["DIM"], -1))]}}


def wifi_icon(cx, bottom):
    """Three arcs over a dot, in INK."""
    cmds = [paint({"color": P["INK"]}, {"style": "stroke"}, {"width": S(1.6)})]
    for r in (4, 8, 12):
        cmds.append({"drawArc": {"left": S(cx - r), "top": S(bottom - r), "right": S(cx + r),
                                 "bottom": S(bottom + r), "startAngle": 225, "sweepAngle": 90}})
    cmds += [paint({"color": P["INK"]}, {"style": "fill"}),
             {"drawCircle": {"cx": S(cx), "cy": S(bottom - 0.5), "radius": S(1.6)}}]
    return cmds


def doc_icon(x, top):
    """A page with a folded corner."""
    w, h, f = 12, 15, 4
    L, T = S(x), S(top)
    return [paint({"color": P["INK"]}, {"style": "stroke"}, {"width": S(1.4)}),
            {"drawLine": {"x1": L, "y1": T, "x2": S(x + w - f), "y2": T}},
            {"drawLine": {"x1": S(x + w - f), "y1": T, "x2": S(x + w), "y2": S(top + f)}},
            {"drawLine": {"x1": S(x + w), "y1": S(top + f), "x2": S(x + w), "y2": S(top + h)}},
            {"drawLine": {"x1": S(x + w), "y1": S(top + h), "x2": L, "y2": S(top + h)}},
            {"drawLine": {"x1": L, "y1": S(top + h), "x2": L, "y2": T}},
            {"drawLine": {"x1": S(x + 3), "y1": S(top + 7), "x2": S(x + w - 3), "y2": S(top + 7)}},
            {"drawLine": {"x1": S(x + 3), "y1": S(top + 10.5), "x2": S(x + w - 3), "y2": S(top + 10.5)}}]


def chevron(cy):
    X = lambda d: f"componentWidth - {S(18 - d)}"
    return [paint({"color": P["DIM"]}, {"style": "stroke"}, {"width": S(1.6)}),
            {"drawLine": {"x1": X(-3), "y1": S(cy - 5), "x2": X(2), "y2": S(cy)}},
            {"drawLine": {"x1": X(2), "y1": S(cy), "x2": X(-3), "y2": S(cy + 5)}}]


def row(i, inset=8):
    kind = f"@system.launch_{i}_kind"
    card = {"left": S(inset), "top": S(2), "right": f"componentWidth - {S(inset)}",
            "bottom": S(ROW - 2), "rx": S(8), "ry": S(8)}
    body = [paint({"color": P["CARD"]}, {"style": "fill"}), {"drawRoundRect": card},
            # The pinned document: the outline the Wi-Fi screen gives the network joined.
            when("eq", kind, 3, [paint({"color": P["GREEN"]}, {"style": "stroke"},
                                       {"width": P["CONNECTED_WIDTH"]}), {"drawRoundRect": card}]),
            when("ne", kind, 3, [paint({"color": P["EDGE"]}, {"style": "stroke"}, {"width": 1}),
                                 {"drawRoundRect": card}]),
            when("eq", kind, 1, wifi_icon(inset + 17, 24)),
            when("eq", kind, 1, text("Wi-Fi", S(inset + 38), S(17), S(14), P["INK"], -1)),
            when("eq", kind, 1, text("@system.launch_wifi", S(inset + 38), S(30), S(10), P["DIM"], -1)),
            when("gt", kind, 1, doc_icon(inset + 11, 11)),
            when("gt", kind, 1, text(f"@system.launch_{i}_name", S(inset + 38), S(17), S(14), P["INK"], -1)),
            when("gt", kind, 1, text(f"@system.launch_{i}_detail", S(inset + 38), S(30), S(10), P["DIM"], -1)),
            *chevron(ROW / 2)]
    return {"box": {"modifiers": ["fillMaxWidth", {"height": S(ROW)}, {"onClick": host(f"launch.open={i}")}],
                    "children": [canvas([when("gt", kind, 0, body)])]}}


def named_variables():
    named = [
        {"name": "system.launch_status", "type": "string", "value": "", "export": True},
        {"name": "system.launch_wifi", "type": "string", "value": "", "export": True},
        {"name": "system.launch_page", "type": "string", "value": "", "export": True},
        {"name": "system.launch_pages", "value": 1, "export": True},
    ]
    for i in range(ROWS):
        named += [
            {"name": f"system.launch_{i}_kind", "value": 0, "export": True},
            {"name": f"system.launch_{i}_name", "type": "string", "value": "", "export": True},
            {"name": f"system.launch_{i}_detail", "type": "string", "value": "", "export": True},
        ]
    return named


def build(pal=DARK, k=1.0):
    global P, K
    P, K = pal, k
    light = " (light)" if pal is LIGHT else ""
    return {
        "header": {"apiLevel": 7, "width": 320, "height": 240, "profiles": 513,
                   "contentDescription": "Documents on this device" + light},
        "root": [
            {"global": [{"resources": {"variables": named_variables()}}]},
            {"column": {"modifiers": ["fillMaxSize", {"background": P["BG"]}],
                        "children": [header()] + ([status_line()] if K != 1 else [])
                                    + [row(i) for i in range(ROWS)]}},
        ],
    }


if __name__ == "__main__":
    for name, make in (("launcher", lambda: build()),
                       ("launcher_light", lambda: build(LIGHT, 1.5))):
        doc = make()
        src = HERE / f"{name}.json"
        src.write_text(json.dumps(doc, indent=1) + "\n")
        subprocess.run(["node", str(HERE / "compile.mjs"), str(src), str(OUT_DIR / f"{name}.rc")],
                       check=True)
