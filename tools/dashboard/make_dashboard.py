#!/usr/bin/env python3
"""Builds a dashboard document: a clock in the top half, weather and a stock in the bottom.

  python3 tools/dashboard/make_dashboard.py      # -> dashboard.json here, documents/dashboard.rc

The clock is local hours and minutes, plus an extra hour hand for another city (Beijing by default),
orange while it is day there and blue at night. Below it: the temperature, the conditions,
the high and low, the place, sunrise and sunset, then a stock (NASDAQ), its value and its
change, green up or red down.

Everything the dashboard shows comes from the variables at the top of the JSON (VALUES
below), so a new one is a copy of dashboard.json with the values edited, compiled with
tools/splash/compile.mjs (json2rc, from hoford/RemoteCompose-Examples). They are exported
(named), so a host can also change them while the document shows; on the Muse gadget,
rc.set name=temp value=61.

Every text drawing sets its own textSize: paint state carries over between drawings, and a
document that never sets it draws all its text at the player's default size.
"""
import json
import pathlib
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT_JSON = HERE / "dashboard.json"
OUT_RC = ROOT / "documents/dashboard.rc"
W, H = 360, 640

# ── What it shows. Strings are drawn as they are; numbers are used in expressions. ──
VALUES = [
    ("temp", "string", "59°", "Current temperature, drawn large; include the unit sign."),
    ("weather", "string", "Mostly Cloudy", "Conditions, under the temperature."),
    ("high", "string", "66°", "Today's high (HIGH column)."),
    ("low", "string", "51°", "Today's low (LOW column)."),
    ("location", "string", "Mountain View, CA", "Place name, under high and low."),
    ("sunrise", "string", "7:20 AM", "Sunrise time, as it should read."),
    ("sunset", "string", "6:25 PM", "Sunset time, as it should read."),
    ("stock_name", "string", "NASDAQ", "Index or ticker name."),
    ("stock_value", "string", "26,430.89", "Its value, formatted as it should read."),
    ("stock_change", "float", 1.11,
     "Change in percent, a number: >= 0 shows a green up arrow, < 0 a red down arrow."),
    ("stock_date", "string", "Mon Oct 19", "Date of the quote."),
    ("city2_name", "string", "Beijing", "Name under the clock centre for the extra hour hand."),
    ("city2_utc_offset", "float", 8, "That city's offset from UTC, in hours (8 = Beijing)."),
    ("city2_day_start", "float", 6, "Hour (0-23) its hand turns orange (day)."),
    ("city2_day_end", "float", 18, "Hour (0-23) its hand turns blue (night)."),
]

# The "comment" block at the top of dashboard.json. JSON has no comments; json2rc ignores
# keys it doesn't know, so this costs nothing in the compiled document.
USAGE = [
    "Dashboard: a clock (local time, plus an hour hand for a second city, orange by day and",
    "blue at night) above weather and a stock quote. Laid out at 360 x 640 dp and scaled to",
    "fit any window.",
    "",
    "To make a new one: copy this file and edit the values under root > global > resources >",
    "variables. Each has a 'comment' saying what it is. Strings are drawn exactly as written;",
    "the numbers (stock_change, city2_*) feed expressions.",
    "Colours are under 'colors' (#AARRGGBB): bg, face, the hands, ink/dim/faint for text,",
    "up/down for the stock change.",
    "",
    "Live updates: the variables are exported, so a host can change them while the document",
    "shows, without sending it again.",
    "",
    "Rules: set textSize before every text drawing (paint state carries over; without it all",
    "text is the player's default size). Use glyphs the players' font (Roboto) has: ↑ ↓ ° ·",
    "work, ▲ ▼ draw nothing. For time use timeInMin (minutes since local midnight) and",
    "offsetToUtc (seconds). Don't use timeInSec: the screen would redraw every second.",
]

# The palette, also exported: a host can restyle it with rc.set too.
COLORS = [
    ("bg", "#FF0B0E14"),
    ("face", "#FF161A22"),
    ("face_edge", "#FF2A3040"),
    ("tick", "#FF5A6376"),
    ("tick_hour", "#FFC7CDD8"),
    ("hand", "#FFFFFFFF"),
    ("city2_day", "#FFFF9F0A"),
    ("city2_night", "#FF2F7FD0"),
    ("ink", "#FFFFFFFF"),
    ("dim", "#FF8A94A6"),
    ("faint", "#FF5A6376"),
    ("rule", "#FF1A2230"),
    ("up", "#FF22C55E"),
    ("down", "#FFEF4444"),
]

# ── Derived values (not exported): time in the other city, and whether it is day there. ──
# timeInMin is local minutes since midnight and offsetToUtc the local offset in seconds, so
# city2_min is the other city's minutes since midnight (+2 days keeps % non-negative).
# (timeInSec is only the seconds into the hour on these players, and reading it at all makes
# the document change every second: don't.)
DERIVED = [
    ("city2_min", "(timeInMin - offsetToUtc / 60 + city2_utc_offset * 60 + 2880) % 1440"),
    ("city2_hour", "floor(city2_min / 60)"),
    # 1 from day_start up to (not including) day_end, else 0.
    ("city2_is_day", "min(max(city2_hour - city2_day_start + 1, 0), 1)"
                     " * min(max(city2_day_end - city2_hour, 0), 1)"),
]


def paint(*ops):
    # Paint state carries over between draws: reset the shader and alpha unless given.
    ops = list(ops)
    keys = {k for op in ops for k in op}
    if "alpha" not in keys:
        ops.append({"alpha": 1.0})
    return {"paint": {"ops": [{"shader": 0}] + ops}}


def color(name):
    return {"color": f"$colors.{name}"}


def text(s, x, y, size, col, pan=0, bold=False):
    """s at baseline y; pan -1: x is the left edge, 0: the centre, 1: the right edge."""
    ops = [color(col), {"style": "fill"}, {"textSize": size}]
    return [paint(*ops),
            {"drawTextAnchored": {"text": s, "x": x, "y": y, "panX": pan, "panY": 0}}]


def when(cond, v1, v2, *cmds):
    return {"conditionalOperations": {"condition": cond, "v1": v1, "v2": v2,
                                      "commands": [c for group in cmds for c in group]}}


def number(expr, whole, decimal):
    return {"type": "textFromFloat", "value": expr, "whole": whole, "decimal": decimal}


def stroke(col, width):
    return paint(color(col), {"style": "stroke"}, {"strokeWidth": width}, {"strokeCap": "round"})


def hand(angle, length, tail, width, col):
    """A hand from the centre, rotated `angle` degrees clockwise from 12."""
    return [stroke(col, width),
            {"save": {"commands": [
                {"rotate": {"angle": angle, "pivotX": CX, "pivotY": CY}},
                {"drawLine": {"x1": CX, "y1": CY + tail, "x2": CX, "y2": CY - length}}]}}]


# ── Top half: the clock ──
CX, CY, R = 180, 150, 128


def clock():
    cmds = [paint(color("face"), {"style": "fill"}),
            {"drawCircle": {"cx": CX, "cy": CY, "radius": R}},
            stroke("face_edge", 3),
            {"drawCircle": {"cx": CX, "cy": CY, "radius": R}}]
    for i in range(60):
        hour = i % 5 == 0
        cmds += [stroke("tick_hour" if hour else "tick", 4 if hour else 1.5),
                 {"save": {"commands": [
                     {"rotate": {"angle": i * 6, "pivotX": CX, "pivotY": CY}},
                     {"drawLine": {"x1": CX, "y1": CY - R + 8, "x2": CX,
                                   "y2": CY - R + (24 if hour else 15)}}]}}]
    # The other city's name, coloured like its hand, under the centre.
    cmds += [when("eq", "city2_is_day", 1, text("@city2_name", CX, CY + 52, 14, "city2_day")),
             when("ne", "city2_is_day", 1, text("@city2_name", CX, CY + 52, 14, "city2_night"))]
    # Hands: the other city's hour, then local hour and minute. No second hand, and nothing
    # reads timeInSec: the document then changes once a minute, not every second (each
    # change repaints the whole screen; on e-ink it's a visible refresh).
    bj = "(city2_min % 720) / 2"
    cmds += [when("eq", "city2_is_day", 1, hand(bj, 62, 0, 5, "city2_day")),
             when("ne", "city2_is_day", 1, hand(bj, 62, 0, 5, "city2_night"))]
    cmds += hand("(timeInMin % 720) / 2", 70, 14, 9, "hand")
    cmds += hand("(timeInMin % 60) * 6", 108, 16, 6, "hand")
    cmds += [paint(color("hand"), {"style": "fill"}),
             {"drawCircle": {"cx": CX, "cy": CY, "radius": 6}},
             paint(color("face"), {"style": "fill"}),
             {"drawCircle": {"cx": CX, "cy": CY, "radius": 2.5}}]
    return cmds


# ── Bottom half: weather, then the stock ──
L, RX = 28, 332     # left and right margins


def weather():
    cmds = [stroke("rule", 1), {"drawLine": {"x1": L, "y1": 300, "x2": RX, "y2": 300}}]
    cmds += text("@temp", CX, 376, 72, "ink")
    cmds += text("@weather", CX, 424, 22, "ink")
    # High and low in two columns either side of the centre.
    for x, label, var in ((120, "HIGH", "@high"), (240, "LOW", "@low")):
        cmds += text(label, x, 452, 11, "dim")
        cmds += text(var, x, 478, 24, "ink")
    cmds += text("@location", CX, 508, 15, "dim")
    cmds += text("SUNRISE", L, 540, 11, "dim", -1)
    cmds += text("@sunrise", L, 562, 18, "ink", -1)
    cmds += text("SUNSET", RX, 540, 11, "dim", 1)
    cmds += text("@sunset", RX, 562, 18, "ink", 1)
    return cmds


def stock():
    cmds = [stroke("rule", 1), {"drawLine": {"x1": L, "y1": 580, "x2": RX, "y2": 580}}]
    cmds += text("@stock_name", L, 606, 16, "dim", -1)
    cmds += text("@stock_value", RX, 606, 22, "ink", 1)
    cmds += text("@stock_date", L, 628, 12, "faint", -1)
    change = number("abs(stock_change)", 3, 2)
    # ↑ ↓ rather than ▲ ▼: the players' font (Roboto) has no triangles.
    for cond, arrow, col in (("ge", "↑", "up"), ("lt", "↓", "down")):
        cmds.append(when(cond, "stock_change", 0,
                         text("%", RX, 628, 14, col, 1),
                         text(change, RX - 14, 628, 14, col, 1),
                         text(arrow, RX - 62, 628, 14, col, 1)))
    return cmds


def build():
    variables = [({"name": n, "type": "string", "value": v, "export": True, "comment": c}
                  if t == "string" else {"name": n, "value": v, "export": True, "comment": c})
                 for n, t, v, c in VALUES]
    variables += [{"name": n, "value": expr, "comment": "Derived from the time; don't edit."}
                  for n, expr in DERIVED]
    # Laid out at 360 x 640 dp, scaled to fit the window and centred (Tab5: 1x, PaperS3: 1x
    # at density 1.5; a landscape window gets the column in the middle).
    k = f"min(windowWidth / {W}, windowHeight / {H})"
    body = [paint(color("bg"), {"style": "fill"}),
            {"drawRect": {"left": 0, "top": 0, "right": "windowWidth", "bottom": "windowHeight"}},
            {"save": {"commands": [
                {"translate": {"dx": f"(windowWidth - {W} * {k}) / 2",
                               "dy": f"(windowHeight - {H} * {k}) / 2"}},
                {"scale": {"sx": k, "sy": k}}] + clock() + weather() + stock()}}]
    return {
        "comment": USAGE,
        "header": {"apiLevel": 7, "width": W, "height": H, "profiles": 513,
                   "contentDescription": "Clock, weather and a stock"},
        "root": [
            {"global": [{"resources": {"variables": variables,
                                       "colors": [{n: c} for n, c in COLORS]}}]},
            {"box": {"modifiers": ["fillMaxSize"], "children": [
                {"canvas": {"modifiers": ["fillMaxSize"], "commands": body}}]}},
        ],
    }


if __name__ == "__main__":
    OUT_JSON.write_text(json.dumps(build(), indent=1, ensure_ascii=False) + "\n")
    subprocess.run(["node", str(ROOT / "tools/splash/compile.mjs"), str(OUT_JSON), str(OUT_RC)],
                   check=True)
