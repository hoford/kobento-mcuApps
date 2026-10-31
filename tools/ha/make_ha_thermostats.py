#!/usr/bin/env python3
"""Home Assistant thermostats, in two layouts:

  python3 tools/ha/make_ha_thermostats.py
    -> documents/wide/ha_thermostats.rc   a wide e-paper strip (the CrowPanel 5.79", 792 x 272)
    -> documents/ha/ha_climate.rc         a 320 x 240 colour screen, all three, dark
    -> documents/ha/ha_thermostat_pages.rc  one thermostat a page on the same screen (the Core2):
                                          swipe, or the player's buttons (Control.next / .back)

Three cards, one per climate entity, each with a dial from min_temp to max_temp: the thick
arc is the target (a range for heat_cool), the ring is the current temperature. Everything
comes from named variables a player fills from Home Assistant (rcplayer's ha.cpp):

  ha.<entity>                    the HVAC mode ("heat", "cool", "heat_cool", "off", ...)
  ha.<entity>.<attribute>        friendly_name, current_temperature, temperature,
                                 target_temp_low/high, min_temp, max_temp, hvac_action, fan_mode
  ha.online                      1 once the player has reached Home Assistant

Black and white, no animation and no time variables: the panel redraws only when a value
changes.
"""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "splash"))
sys.path.insert(0, str(HERE.parent / "wide"))
from make_splash import paint, text, when, number, fit  # noqa: E402
from make_wide import rect, circle, round_rect, stroke, INK, DIM  # noqa: E402

DOCS = HERE.parent.parent / "documents"
# The Home Assistant demo integration's three thermostats.
ENTITIES = ["climate.heatpump", "climate.hvac", "climate.ecobee"]
STRINGS = ["", ".friendly_name", ".hvac_action", ".fan_mode"]
FLOATS = {".current_temperature": 0, ".temperature": 0, ".target_temp_low": 0,
          ".target_temp_high": 0, ".min_temp": 7, ".max_temp": 35}
START, SWEEP = 135, 270             # the dial, degrees clockwise from 3 o'clock: open at the bottom

# Two layouts: the wide e-paper strip (black and white), and a 320 x 240 colour screen (the
# Core2), dark. Sizes in dp; each card is a third of the width.
WIDE = dict(out="wide/ha_thermostats.rc", w=792, h=272, inset=6, radius=14, border=2, pad=22,
            dial_y=140, dial_r=70, track=2, bar=12, ring=10, ring_w=3, title=(36, 24), mode=17,
            big=58, deg=(34, -12, 34), now=(40, 15), set_y=226, set=(18, 24), foot_y=252, foot=16,
            bg="#FFFFFF", card="#FFFFFF", ink=INK, dim=DIM, line=INK, accent=INK, ring_fill="#FFFFFF")
SMALL = dict(out="ha/ha_climate.rc", w=320, h=240, inset=3, radius=10, border=0, pad=9,
             dial_y=106, dial_r=40, track=3, bar=8, ring=6, ring_w=2, title=(22, 15), mode=10,
             big=34, deg=(20, -8, 20), now=(25, 10), set_y=172, set=(11, 16), foot_y=198, foot=10,
             bg="#0B0E12", card="#1A1F26", ink="#FFFFFF", dim="#8A94A3", line="#3A4250",
             accent="#FF9F0A", ring_fill="#1A1F26")


def var(e, attr=""):
    return f"@ha.{e}{attr}"


def angle(e, value):
    lo, hi = var(e, ".min_temp"), var(e, ".max_temp")
    v = f"max(min({value}, {hi}), {lo})"
    return f"({START} + {SWEEP} * ({v} - {lo}) / max({hi} - {lo}, 1))"


def card(e, L):
    cw, h, pad = L["w"] / 3, L["h"], L["pad"]
    cx, cy, r = cw / 2, L["dial_y"], L["dial_r"]
    ink, dim = L["ink"], L["dim"]

    def arc(a0, sweep, width, color):
        return [stroke(width, color), {"drawArc": {"left": cx - r, "top": cy - r, "right": cx + r,
                                                   "bottom": cy + r, "startAngle": a0, "sweepAngle": sweep}}]

    def target_arc(lo, hi):
        a0, a1 = angle(e, lo), angle(e, hi)
        # A single set point still gets a short bar, centred on it.
        return arc(f"min({a0}, {a1} - 3)", f"max({a1} - {a0}, 6)", L["bar"], L["accent"])

    i = L["inset"]
    c = round_rect(i, i, cw - i, h - i, L["radius"], color=L["card"])
    if L["border"]:
        c += round_rect(i, i, cw - i, h - i, L["radius"], fill=False, width=L["border"], color=ink)
    ty, ts = L["title"]
    c += text(var(e, ".friendly_name"), pad, ty, ts, ink, pan=-1)
    if L["w"] > 400:
        c += text(var(e), cw - pad, ty, L["mode"], dim, pan=1)
    else:   # narrow cards: the mode goes under the name
        c += text(var(e), pad, ty + ts, L["mode"], dim, pan=-1)
    # The dial: a thin track, the target, and a ring at the current temperature.
    c += arc(START, SWEEP, L["track"], L["line"])
    low, high, temp = var(e, ".target_temp_low"), var(e, ".target_temp_high"), var(e, ".temperature")
    c.append(when("gt", low, 0, target_arc(low, high)))
    c.append(when("eq", low, 0, target_arc(temp, temp)))
    rad = f"{angle(e, var(e, '.current_temperature'))} * 0.017453292"
    mx, my = f"({cx} + {r} * cos({rad}))", f"({cy} + {r} * sin({rad}))"
    c += [paint({"color": L["ring_fill"]}, {"style": "fill"}), {"drawCircle": {"cx": mx, "cy": my, "radius": L["ring"]}}]
    c += [stroke(L["ring_w"], ink), {"drawCircle": {"cx": mx, "cy": my, "radius": L["ring"]}}]
    # The current temperature in the middle.
    dx, dy, ds = L["deg"]
    c += text(number(var(e, ".current_temperature"), 2, 0), cx, cy + 2, L["big"], ink)
    c += text("°", cx + dx, cy + dy, ds, ink, pan=-1)
    ny, ns = L["now"]
    c += text("now", cx, cy + ny, ns, dim)
    # The set point(s) and what the system is doing.
    sy, (ls, vs) = L["set_y"], L["set"]
    right = cw - pad - vs / 2            # the value's right edge, before the degree sign
    digits = vs * 1.12                   # two digits' width, about
    c += text("set", pad, sy, ls, dim, pan=-1)
    c.append(when("eq", low, 0, text(number(temp, 2, 0), right, sy, vs, ink, pan=1)))
    c.append(when("gt", low, 0,
                  text(number(low, 2, 0), right - digits - vs * 0.5, sy, vs, ink, pan=1)
                  + text("–", right - digits - vs * 0.17, sy, vs, ink)
                  + text(number(high, 2, 0), right, sy, vs, ink, pan=1)))
    c += text("°", right + 2, sy - 2, vs, ink, pan=-1)
    fy = L["foot_y"]
    c += text(var(e, ".hvac_action"), pad, fy, L["foot"], ink, pan=-1)
    c += text(var(e, ".fan_mode"), cw - pad, fy + (0 if L["w"] > 400 else L["foot"] + 3), L["foot"], dim,
              pan=1 if L["w"] > 400 else 1)
    return c


# The three thermostats as pages of one document on a 320 x 240 colour screen (the Core2):
# documents/ha/ha_thermostat_pages.rc. The player's buttons step through the pages: the
# document declares the counters Control.next and Control.back, which the player raises once
# per press, and slides round to the page their difference names. Touch is kept for the set
# point, not for paging.
#
# And the set point can be changed: − and + either side of the dial move it half a degree.
# A tap changes the page's Control.climate.<id>.temperature (target_temp_low and _high
# together for a range), which the player listens to and sends to Home Assistant, then puts
# back to 0; meanwhile the page shows the value asked for.
#
# Or drag round the dial (a single set point; not a range): a touch expression in a canvas over
# the dial turns the finger's angle into a temperature in half degrees, shown while the finger
# is down (system.touch_down). The page's Control.dial.<id> follows it, and the player sends
# what it ends up at once the finger lifts.
STEP = 0.5
SETS = ["temperature", "target_temp_low", "target_temp_high"]
BTN_X, BTN_Y = 44, 180          # the − button's centre; + mirrors it
DIAL_R = 95                     # the dial takes touches within this of its centre (a square)
PAGE_W, PAGE_H = 320, 240


def ctl(e, attr):
    return f"@Control.{e}.{attr}"


def shown(e, attr):
    # What the page shows as a set point: the dial while it's dragged, the value asked for
    # while there is one, else HA's.
    return f"@shown.{e}.{attr}"


def sel(i):
    return f"(1 - min(abs(@page - {i}), 1))"   # 1 on page i


def ranged(e):
    return f"sign({var(e, '.target_temp_low')})"   # 1: a range (low .. high), no single set point


def shown_value(e, a):
    v = f"{ctl(e, a)} + (1 - sign(abs({ctl(e, a)}))) * {var(e, '.' + a)}"
    if a != "temperature":
        return v
    i = ENTITIES.index(e)
    drag = f"(@system.touch_down * @in_dial * {sel(i)} * (1 - {ranged(e)}))"
    return f"{drag} * @dial + (1 - {drag}) * ({v})"


def thermostat(e):
    W, H, ink, dim, accent = PAGE_W, PAGE_H, SMALL["ink"], SMALL["dim"], SMALL["accent"]
    cx, cy, r = 160, 128, 74

    def arc(a0, sweep, width, color):
        return [stroke(width, color), {"drawArc": {"left": cx - r, "top": cy - r, "right": cx + r,
                                                   "bottom": cy + r, "startAngle": a0, "sweepAngle": sweep}}]

    def target_arc(lo, hi):
        a0, a1 = angle(e, lo), angle(e, hi)
        return arc(f"min({a0}, {a1} - 2)", f"max({a1} - {a0}, 4)", 12, accent)

    low, high, temp = shown(e, "target_temp_low"), shown(e, "target_temp_high"), shown(e, "temperature")
    c = [{"variable": {"name": f"shown.{e}.{a}", "commit": True, "value": shown_value(e, a)}} for a in SETS]
    c += text(var(e, ".friendly_name"), 12, 20, 20, ink, pan=-1)
    c += text(var(e), W - 12, 20, 14, accent, pan=1)
    c += arc(START, SWEEP, 4, SMALL["line"])
    c.append(when("gt", low, 0, target_arc(low, high)))
    c.append(when("eq", low, 0, target_arc(temp, temp)))
    rad = f"{angle(e, var(e, '.current_temperature'))} * 0.017453292"
    mx, my = f"({cx} + {r} * cos({rad}))", f"({cy} + {r} * sin({rad}))"
    c += [paint({"color": SMALL["bg"]}, {"style": "fill"}), {"drawCircle": {"cx": mx, "cy": my, "radius": 9}}]
    c += [stroke(3, ink), {"drawCircle": {"cx": mx, "cy": my, "radius": 9}}]
    # The handle to drag, at the set point (a single one).
    trad = f"{angle(e, temp)} * 0.017453292"
    hx, hy = f"({cx} + {r} * cos({trad}))", f"({cy} + {r} * sin({trad}))"
    c.append(when("eq", low, 0, [paint({"color": accent}, {"style": "fill"}),
                                 {"drawCircle": {"cx": hx, "cy": hy, "radius": 11}}]))
    c += text(number(var(e, ".current_temperature"), 2, 0), cx, cy - 4, 64, ink)
    c += text("°", cx + 38, cy - 22, 48, ink, pan=-1)
    c += text("now", cx, cy + 36, 13, dim)
    # Under the dial's opening: the set point(s).
    c.append(when("eq", low, 0, text(number(temp, 2, 1), cx, cy + 62, 26, ink)))
    c.append(when("gt", low, 0,
                  text(number(low, 2, 1), cx - 12, cy + 62, 22, ink, pan=1)
                  + text("–", cx, cy + 62, 22, ink)
                  + text(number(high, 2, 1), cx + 12, cy + 62, 22, ink, pan=-1)))
    c += text("set", cx, cy + 83, 11, dim)
    # Corners: what it's doing, the fan.
    c += text(var(e, ".hvac_action"), 12, 46, 14, ink, pan=-1)
    c += text(var(e, ".fan_mode"), W - 12, 46, 13, dim, pan=1)
    return c


def pages():
    W, H, ink, dim, accent = PAGE_W, PAGE_H, SMALL["ink"], SMALL["dim"], SMALL["accent"]
    n = len(ENTITIES)
    # The dial's canvas, under the page's and drawing nothing: its bounds are where a touch
    # moves the dial. First, since the page's canvas uses what it works out: the position, in
    # pages (fractional while sliding), wrapped into 0 .. n; the page nearest it; the page's
    # limits; and the temperature at the finger's angle.
    cx, cy = 160, 128
    lo = " + ".join(f"{sel(i)} * {var(e, '.min_temp')}" for i, e in enumerate(ENTITIES))
    hi = " + ".join(f"{sel(i)} * {var(e, '.max_temp')}" for i, e in enumerate(ENTITIES))
    # Degrees clockwise from the dial's start (135°, bottom left), with the jump round the
    # circle in the gap at the bottom; 0 .. 270 is the arc.
    u = f"((atan2(touchY() - {cy}, touchX() - {cx}) * 57.29578 - 90) % 360 + 360) % 360 - 45"
    dial = [{"variable": {"name": "pos", "commit": True, "value": f"(@steps % {n} + {n}) % {n}"}},
            {"variable": {"name": "page", "commit": True, "value": f"floor(@pos + 0.5) % {n}"}},
            {"variable": {"name": "dial_lo", "commit": True, "value": lo}},
            {"variable": {"name": "dial_hi", "commit": True, "value": hi}},
            {"touchExpression": {"name": "dial", "defaultValue": 0, "min": 0, "max": 100, "stopMode": "absolutePos",
                                 "expression": f"@dial_lo + round(max(min(({u}) / {SWEEP}, 1), 0)"
                                               f" * (@dial_hi - @dial_lo) * 2) / 2"}},
            # Whether the last touch was on the dial (this canvas's bounds).
            {"variable": {"name": "in_dial", "commit": True,
                          "value": f"1 - sign(max(max(abs(touchX() - {cx}), abs(touchY() - {cy})) - {DIAL_R}, 0))"}}]
    for i, e in enumerate(ENTITIES):
        dial.append({"variable": {"name": f"Control.dial.{e}", "export": True,
                                  "value": f"{sel(i)} * (1 - {ranged(e)}) * @dial"}})
    c = []
    pos = "@pos"
    for i, e in enumerate(ENTITIES):
        # This page's offset from the one showing, in screens: -1.5 .. 1.5, round the ring.
        d = f"((({i} - {pos}) % {n} + {n} + {n / 2}) % {n} - {n / 2})"
        c.append(when("lt", f"abs({d})", 1,
                      [{"save": {"commands": [{"translate": {"dx": f"{d} * {W}", "dy": 0}}] + thermostat(e)}}]))
    # Chevrons over the A and C marks (drawn: the font has no arrows), and a dot per page.
    y = H - 9
    c += [stroke(2.5, dim), {"drawLine": {"x1": 58, "y1": y - 6, "x2": 52, "y2": y}},
          {"drawLine": {"x1": 52, "y1": y, "x2": 58, "y2": y + 6}},
          {"drawLine": {"x1": 262, "y1": y - 6, "x2": 268, "y2": y}},
          {"drawLine": {"x1": 268, "y1": y, "x2": 262, "y2": y + 6}}]
    for i in range(n):
        x = W / 2 + (i - (n - 1) / 2) * 14
        c += circle(x, y, 3, fill=False, width=1.5, color=dim)
        c.append(when("eq", "@page", i, circle(x, y, 4, color=ink)))
    # − and +, either side of the set point; the taps land on the boxes over them (below).
    for bx, plus in ((BTN_X, False), (W - BTN_X, True)):
        c += circle(bx, BTN_Y, 21, color=SMALL["card"]) + circle(bx, BTN_Y, 21, fill=False, width=2, color=accent)
        c += [stroke(3, ink), {"drawLine": {"x1": bx - 8, "y1": BTN_Y, "x2": bx + 8, "y2": BTN_Y}}]
        if plus:
            c += [{"drawLine": {"x1": bx, "y1": BTN_Y - 8, "x2": bx, "y2": BTN_Y + 8}}]
    c.append(when("eq", "@ha.online", 0,
                  round_rect(60, H / 2 - 22, W - 60, H / 2 + 22, 10, color=SMALL["card"])
                  + round_rect(60, H / 2 - 22, W - 60, H / 2 + 22, 10, fill=False, width=2, color=accent)
                  + text("Waiting for Home Assistant…", W / 2, H / 2, 13, ink)))
    page = rect(0, 0, "windowWidth", "windowHeight", SMALL["bg"]) + [fit(W, H, c)]
    named = [{"name": "ha.online", "value": 0, "export": True}]
    for e in ENTITIES:
        named += [{"name": f"ha.{e}{a}", "type": "string", "value": "—" if a == ".friendly_name" else "",
                   "export": True} for a in STRINGS]
        named += [{"name": f"ha.{e}{a}", "value": v, "export": True} for a, v in FLOATS.items()]
        named += [{"name": f"Control.{e}.{a}", "value": 0, "export": True} for a in SETS]
    named.append({"name": "system.touch_down", "value": 0, "export": True})
    # Button presses, counted by the player; their difference slides (0.35 s) a page a press.
    named += [{"name": "Control.next", "value": 0, "export": True},
              {"name": "Control.back", "value": 0, "export": True},
              {"name": "steps", "value": {"value": "@Control.next - @Control.back", "anim": 0.35}}]
    def tap(delta):
        # Every page's set points, changed only on the page showing (sel 1): a single set point,
        # or both ends of a range (ranged 1), by delta, kept within the entity's limits.
        actions = []
        for i, e in enumerate(ENTITIES):
            for a in SETS:
                which = ranged(e) if a != "temperature" else f"(1 - {ranged(e)})"
                to = f"max(min({shown(e, a)} + {delta}, {var(e, '.max_temp')}), {var(e, '.min_temp')})"
                actions.append({"type": "valueFloatExpressionChange", "target": f"Control.{e}.{a}",
                                "value": f"{ctl(e, a)} + {sel(i)} * {which} * ({to} - {ctl(e, a)})"})
        return actions

    def zone(delta):
        return {"box": {"modifiers": [{"width": 2 * BTN_X + 12}, "fillMaxHeight", {"onClick": tap(delta)}],
                        "children": []}}

    return {
        "header": {"apiLevel": 7, "width": W, "height": H, "profiles": 513,
                   "contentDescription": "Home Assistant thermostats, a page each"},
        "root": [
            {"global": [{"resources": {"variables": named}}]},
            {"box": {"modifiers": ["fillMaxSize"], "children": [
                {"box": {"modifiers": ["fillMaxSize", {"padding": [cx - DIAL_R, cy - DIAL_R, W - cx - DIAL_R,
                                                                    H - cy - DIAL_R]}], "children": [
                    {"canvas": {"modifiers": ["fillMaxSize"], "commands": dial}}]}},
                {"canvas": {"modifiers": ["fillMaxSize"], "commands": page}},
                {"column": {"modifiers": ["fillMaxSize"], "children": [
                    {"box": {"modifiers": [{"verticalWeight": 1}, "fillMaxWidth"], "children": []}},
                    {"row": {"modifiers": ["fillMaxWidth", {"height": 2 * (H - BTN_Y - 24)}], "children": [
                        zone(-STEP),
                        {"box": {"modifiers": [{"horizontalWeight": 1}, "fillMaxHeight"], "children": []}},
                        zone(STEP)]}},
                    {"box": {"modifiers": ["fillMaxWidth", {"height": 24}], "children": []}}]}}]}},
        ],
    }


def build(L):
    W, H = L["w"], L["h"]
    cmds = []
    for i, e in enumerate(ENTITIES):
        cmds.append({"save": {"commands": [{"translate": {"dx": i * W / 3, "dy": 0}}] + card(e, L)}})
    # Until the player has reached Home Assistant, say so over the middle.
    bw, bh, k = W * (0.43 if W > 400 else 0.66), H * 0.19, W / 792 if W > 400 else 0.55
    box = (W / 2 - bw / 2, H / 2 - bh / 2, W / 2 + bw / 2, H / 2 + bh / 2)
    cmds.append(when("eq", "@ha.online", 0,
                     round_rect(*box, 12 * k + 4, color=L["card"] if W < 400 else "#FFFFFF")
                     + round_rect(*box, 12 * k + 4, fill=False, width=3 if W > 400 else 2, color=L["accent"] if W < 400 else INK)
                     + text("Waiting for Home Assistant…", W / 2, H / 2, 22 if W > 400 else 12, L["ink"])))
    page = rect(0, 0, "windowWidth", "windowHeight", L["bg"]) + [fit(W, H, cmds)]
    named = [{"name": "ha.online", "value": 0, "export": True}]
    for e in ENTITIES:
        named += [{"name": f"ha.{e}{a}", "type": "string", "value": "—" if a == ".friendly_name" else "",
                   "export": True} for a in STRINGS]
        named += [{"name": f"ha.{e}{a}", "value": v, "export": True} for a, v in FLOATS.items()]
    return {
        "header": {"apiLevel": 7, "width": W, "height": H, "profiles": 513,
                   "contentDescription": "Home Assistant thermostats"},
        "root": [
            {"global": [{"resources": {"variables": named}}]},
            {"box": {"modifiers": ["fillMaxSize"], "children": [
                {"canvas": {"modifiers": ["fillMaxSize"], "commands": page}}]}},
        ],
    }


if __name__ == "__main__":
    for L in (WIDE, SMALL):
        out = DOCS / L["out"]
        src = HERE / (out.stem + ".json")
        src.write_text(json.dumps(build(L), indent=1, ensure_ascii=False) + "\n")
        subprocess.run(["node", str(HERE.parent / "splash/compile.mjs"), str(src), str(out)], check=True,
                       stdout=subprocess.DEVNULL)
        print(f"{out}: {out.stat().st_size} bytes")
    for out, d in ((DOCS / "ha/ha_thermostat_pages.rc", pages()),):
        src = HERE / (out.stem + ".json")
        src.write_text(json.dumps(d, indent=1, ensure_ascii=False) + "\n")
        subprocess.run(["node", str(HERE.parent / "splash/compile.mjs"), str(src), str(out)], check=True,
                       stdout=subprocess.DEVNULL)
        print(f"{out}: {out.stat().st_size} bytes")
