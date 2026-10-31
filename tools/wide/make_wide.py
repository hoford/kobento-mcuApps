#!/usr/bin/env python3
"""Builds three static documents for wide strip screens (the CrowPanel 5.79", 792 x 272):

  python3 tools/wide/make_wide.py     # -> wide_*.json here, documents/wide/wide_*.rc

  wide_welcome    Remote Compose on this board, and how to step through the documents
  wide_forecast   a five-day weather forecast (fixed sample data)
  wide_agenda     a day's agenda (fixed sample data)

Each is laid out at 792 x 272 dp and scaled to fit the window, in black and white for
e-paper: no animation, no time variables, so the panel is drawn once. documents/wide/ is
built in only by boards that name these in CONFIG_RCPLAYER_DOCUMENTS.
"""
import json
import math
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "splash"))
from make_splash import paint, text, fit  # noqa: E402

OUT_DIR = HERE.parent.parent / "documents/wide"
W, H = 792, 272
INK, DIM, RULE = "#000000", "#555555", "#999999"


def rect(l, t, r, b, color=INK):
    return [paint({"color": color}, {"style": "fill"}),
            {"drawRect": {"left": l, "top": t, "right": r, "bottom": b}}]


def stroke(width, color=INK):
    return paint({"color": color}, {"style": "stroke"}, {"strokeWidth": width}, {"strokeCap": "round"})


def line(x1, y1, x2, y2, width=2, color=INK):
    return [stroke(width, color), {"drawLine": {"x1": x1, "y1": y1, "x2": x2, "y2": y2}}]


def circle(cx, cy, r, fill=True, width=3, color=INK):
    p = paint({"color": color}, {"style": "fill"}) if fill else stroke(width, color)
    return [p, {"drawCircle": {"cx": cx, "cy": cy, "radius": r}}]


def round_rect(l, t, r, b, rad, fill=True, width=2, color=INK):
    p = paint({"color": color}, {"style": "fill"}) if fill else stroke(width, color)
    return [p, {"drawRoundRect": {"left": l, "top": t, "right": r, "bottom": b, "rx": rad, "ry": rad}}]


def doc(name, description, cmds):
    page = rect(0, 0, "windowWidth", "windowHeight", "#FFFFFF") + [fit(W, H, cmds)]
    return {
        "header": {"apiLevel": 7, "width": W, "height": H, "profiles": 513,
                   "contentDescription": description},
        "root": [{"box": {"modifiers": ["fillMaxSize"], "children": [
            {"canvas": {"modifiers": ["fillMaxSize"], "commands": page}}]}}],
    }


# ── wide_welcome ──

def welcome():
    c = []
    # The mark: three linked rings, the "compose" of parts.
    for cx, cy in ((150, 112), (196, 112), (173, 152)):
        c += circle(cx, cy, 40, fill=False, width=7)
    c += circle(173, 125, 9)
    c += text("Remote Compose", 290, 104, 58, INK, pan=-1)
    c += text("documents drawn on the device, a few KB each", 292, 146, 21, DIM, pan=-1)
    c += line(292, 176, 760, 176, 2, RULE)
    c += text("CrowPanel 5.79″ e-paper  ·  792 × 272  ·  ESP32-S3", 292, 206, 21, INK, pan=-1)
    c += text("Turn the dial for the next document, press it to redraw", 292, 238, 18, DIM, pan=-1)
    c += round_rect(8, 8, W - 8, H - 8, 18, fill=False, width=3)
    return doc("wide_welcome", "Remote Compose welcome card for a wide e-paper strip", c)


# ── wide_forecast ──

def sun(cx, cy, r=22):
    c = circle(cx, cy, r)
    for k in range(8):

        a = k * math.pi / 4
        c += line(cx + math.cos(a) * (r + 7), cy + math.sin(a) * (r + 7),
                  cx + math.cos(a) * (r + 16), cy + math.sin(a) * (r + 16), 4)
    return c


def cloud(cx, cy, k=1.0, fill=INK):
    c = []
    for dx, dy, r in ((-16, 6, 15), (2, -6, 20), (20, 6, 14)):
        c += circle(cx + dx * k, cy + dy * k, r * k, color=fill)
    c += rect(cx - 16 * k, cy + 6 * k, cx + 20 * k, cy + 20 * k, fill)
    return c


def rain(cx, cy):
    c = cloud(cx, cy - 8)
    for dx in (-14, 0, 14):
        c += line(cx + dx, cy + 22, cx + dx - 6, cy + 36, 3)
    return c


def part_sun(cx, cy):
    c = sun(cx + 12, cy - 10, 15)
    # A white rim so the cloud stands out of the sun.
    c += cloud(cx - 4, cy + 6, 1.15, "#FFFFFF")
    c += cloud(cx - 4, cy + 6, 1.0)
    return c


def forecast():
    days = [("Thu", sun, 68, 51, "Sunny"), ("Fri", part_sun, 64, 50, "Some clouds"),
            ("Sat", cloud, 59, 49, "Cloudy"), ("Sun", rain, 56, 47, "Showers"),
            ("Mon", sun, 66, 50, "Clear")]
    c = text("Mountain View", 24, 34, 26, INK, pan=-1)
    c += text("5-day forecast  ·  °F", W - 24, 34, 20, DIM, pan=1)
    c += line(24, 54, W - 24, 54, 2)
    col = (W - 48) / len(days)
    for i, (day, icon, hi, lo, words) in enumerate(days):
        x0 = 24 + i * col
        cx = x0 + col / 2
        if i:
            c += line(x0, 70, x0, 252, 1, RULE)
        c += text(day, cx, 84, 26, INK)
        c += icon(cx, 140)
        c += text(f"{hi}°", cx - 6, 206, 34, INK, pan=1)
        c += text(f"{lo}°", cx + 6, 206, 26, DIM, pan=-1)
        c += text(words, cx, 242, 17, DIM)
    return doc("wide_forecast", "Five-day weather forecast (sample data)", c)


# ── wide_agenda ──

def agenda():
    items = [("9:00", "Stand-up", "Video call", True), ("11:30", "Design review", "Room 4B · RC players", True),
             ("14:00", "Flash the CrowPanel", "Three static documents", False),
             ("16:30", "Bike home", "Before the rain", False)]
    c = rect(0, 0, 220, H)
    c += text("THURSDAY", 110, 58, 24, "#FFFFFF")
    c += text("23", 110, 140, 104, "#FFFFFF")
    c += text("OCTOBER 2026", 110, 222, 20, "#FFFFFF")
    c += text("Today", 248, 34, 26, INK, pan=-1)
    c += text(f"{sum(1 for i in items if i[3])} of {len(items)} done", W - 24, 34, 20, DIM, pan=1)
    c += line(248, 54, W - 24, 54, 2)
    row = 50
    for i, (at, title, detail, done) in enumerate(items):
        y = 64 + i * row
        if i:
            c += line(248, y, W - 24, y, 1, RULE)
        c += text(at, 300, y + 26, 22, INK, pan=1)
        c += round_rect(320, y + 13, 344, y + 37, 4, fill=done, width=2)
        if done:
            c += [stroke(3, "#FFFFFF")] + [{"drawLine": {"x1": 325, "y1": y + 25, "x2": 330, "y2": y + 31}},
                                           {"drawLine": {"x1": 330, "y1": y + 31, "x2": 339, "y2": y + 19}}]
        c += text(title, 360, y + 25, 22, DIM if done else INK, pan=-1)
        c += text(detail, W - 24, y + 25, 17, DIM, pan=1)
    return doc("wide_agenda", "A day's agenda (sample data)", c)


if __name__ == "__main__":
    OUT_DIR.mkdir(exist_ok=True)
    for name, build in (("wide_welcome", welcome), ("wide_forecast", forecast), ("wide_agenda", agenda)):
        src = HERE / f"{name}.json"
        src.write_text(json.dumps(build(), indent=1) + "\n")
        subprocess.run(["node", str(HERE.parent / "splash/compile.mjs"), str(src), str(OUT_DIR / f"{name}.rc")],
                       check=True)
