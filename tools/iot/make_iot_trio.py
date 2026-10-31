#!/usr/bin/env python3
"""Three IoT panels side by side in one document, with their animation frozen.

  python3 tools/iot/make_iot_trio.py [NAME NAME NAME] [--time T] [--out NAME]
      -> tools/iot/<out>.json, documents/wide/<out>.rc   (default out: iot_trio)
  python3 tools/iot/make_iot_trio.py --cycle
      -> tools/iot/iot_cycle.json, documents/wide/iot_cycle.rc
  python3 tools/iot/make_iot_trio.py --freeze-all DIR
      -> DIR/<name>.rc for every panel, frozen (for previews)

The panels are the 80 light ones from RemoteCompose-Examples (`iot-panels-light/*`, white
backgrounds, 360 x 360 dp), downloaded with their RC JSON into documents/iot_light_src/
(index.json lists them). Every one animates through continuousSec() alone; replacing that
with a constant (--time, seconds) leaves no time dependency, so a player draws the
document once and never again: right for e-paper.

The trio is a row of three equal boxes (its header says 1080 x 360 dp), each holding one
panel's canvas, unchanged but scaled to fit the box and centred: 264 x 264 each on a
792 x 272 strip.

The cycle (--cycle) holds all 80 panels, frozen, in three slots. Every second one slot
takes the next panel: slot 0, then 1, then 2, then 0 again. Second n of the day (n =
timeInSec, whole seconds since midnight) shows panels n, n+1 and n+2 (mod 80); panel m sits
in slot m mod 3, so each second exactly one slot changes. 86400 is a multiple of both 3 and
80, so midnight is just another step. Each panel's commands are in the document once,
drawn only while it is in a slot (about 175 KB in all).
"""
import argparse
import copy
import json
import pathlib
import re
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SRC = ROOT / "documents/iot_light_src"
OUT_DIR = ROOT / "documents/wide"
COMPILE = HERE.parent / "splash/compile.mjs"
DEFAULT = ["06-thermostat", "13-smart-lock", "39-home-battery"]
SIZE = 360

CLOCK = re.compile(r"continuousSec\(\)")


def freeze(node, t):
    """A copy of the JSON with every continuousSec() replaced by the constant t."""
    if isinstance(node, str):
        return CLOCK.sub(f"{t:g}", node)
    if isinstance(node, list):
        return [freeze(n, t) for n in node]
    if isinstance(node, dict):
        return {k: freeze(v, t) for k, v in node.items()}
    return node


def load(name, t):
    doc = json.loads((SRC / f"{name}.json").read_text())
    frozen = freeze(doc, t)
    left = json.dumps(frozen).count("continuousSec")
    if left:
        raise SystemExit(f"{name}: {left} continuousSec references left after freezing")
    return frozen


def canvas_of(doc):
    # Each panel is root[0] = box(fillMaxSize) holding one canvas drawn in 0..360.
    box = doc["root"][0]["box"]
    (canvas,) = box["children"]
    return canvas


def trio(names, t):
    docs = [load(n, t) for n in names]
    # Each cell takes a third of the width; its panel, drawn in 0..360, is scaled to fit the
    # cell and centred, so the trio fits any window (792 x 264 on a 792 x 272 strip).
    k = f"min(componentWidth / {SIZE}, componentHeight / {SIZE})"
    cells = []
    for d in docs:
        c = copy.deepcopy(canvas_of(d))
        c["modifiers"] = ["fillMaxSize"]
        c["commands"] = [{"save": {"commands": [
            {"translate": {"dx": f"(componentWidth - {SIZE} * {k}) / 2",
                           "dy": f"(componentHeight - {SIZE} * {k}) / 2"}},
            {"scale": {"sx": k, "sy": k}}] + c["commands"]}}]
        cells.append({"box": {"modifiers": [{"horizontalWeight": 1}, "fillMaxHeight"], "children": [c]}})
    titles = " · ".join(d["header"]["contentDescription"] for d in docs)
    return {
        "header": {"apiLevel": 7, "width": SIZE * 3, "height": SIZE, "profiles": 513,
                   "contentDescription": f"Three IoT panels, not animated: {titles}"},
        "root": [{"row": {"modifiers": ["fillMaxSize"], "children": cells}}],
    }


def cycle(t):
    names = sorted(f.stem for f in SRC.glob("[0-9]*.json"))
    count = len(names)
    assert 86400 % count == 0 and 86400 % 3 == 0, "midnight's wrap must be a whole step"
    # Each slot is a third of the canvas; a panel is scaled to fit it and centred.
    cw = "componentWidth / 3"
    k = f"min({cw} / {SIZE}, componentHeight / {SIZE})"
    cmds = [
        {"variable": {"name": "cycle_n", "value": "timeInSec"}},
        {"variable": {"name": "cycle_k", "value": k}},
        # The panels' own backdrop colour behind the slots, edge to edge.
        {"paint": {"ops": [{"color": "#EDF0F4"}, {"style": "fill"}, {"alpha": 1.0}]}},
        {"drawRect": {"left": 0, "top": 0, "right": "componentWidth", "bottom": "componentHeight"}},
    ]
    for p, name in enumerate(names):
        panel = canvas_of(load(name, t))["commands"]
        d = f"(({p} - @cycle_n + 86400) % {count})"   # 0, 1, 2: on screen
        slot = f"((@cycle_n + {d}) % 3)"
        cmds.append({"conditionalOperations": {"condition": "lt", "v1": d, "v2": 3, "commands": [
            {"save": {"commands": [
                {"translate": {"dx": f"{slot} * {cw} + ({cw} - {SIZE} * @cycle_k) / 2",
                               "dy": f"(componentHeight - {SIZE} * @cycle_k) / 2"}},
                {"scale": {"sx": "@cycle_k", "sy": "@cycle_k"}}] + panel}}]}})
    return {
        "header": {"apiLevel": 7, "width": SIZE * 3, "height": SIZE, "profiles": 513,
                   "contentDescription": f"All {count} light IoT panels, three at a time, "
                                         "one slot changing each second, not animated"},
        "root": [{"box": {"modifiers": ["fillMaxSize"], "children": [
            {"canvas": {"modifiers": ["fillMaxSize"], "commands": cmds}}]}}],
    }


def compile_json(doc, json_path, rc_path):
    json_path.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n")
    subprocess.run(["node", str(COMPILE), str(json_path), str(rc_path)], check=True,
                   stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*", default=DEFAULT)
    ap.add_argument("--time", type=float, default=1.0, help="the frozen continuousSec(), seconds")
    ap.add_argument("--out", default="iot_trio")
    ap.add_argument("--freeze-all", metavar="DIR")
    ap.add_argument("--cycle", action="store_true", help="all panels, one slot changing each second")
    a = ap.parse_args()
    if a.cycle:
        OUT_DIR.mkdir(exist_ok=True)
        compile_json(cycle(a.time), HERE / "iot_cycle.json", OUT_DIR / "iot_cycle.rc")
        print(f"{OUT_DIR / 'iot_cycle.rc'}: {(OUT_DIR / 'iot_cycle.rc').stat().st_size} bytes")
        return
    if a.freeze_all:
        out = pathlib.Path(a.freeze_all)
        out.mkdir(parents=True, exist_ok=True)
        for f in sorted(SRC.glob("[0-9]*.json")):
            compile_json(load(f.stem, a.time), out / f"{f.stem}.json", out / f"{f.stem}.rc")
        return
    if len(a.names) != 3:
        raise SystemExit("give three panel names (see documents/iot_light_src/index.json)")
    OUT_DIR.mkdir(exist_ok=True)
    compile_json(trio(a.names, a.time), HERE / f"{a.out}.json", OUT_DIR / f"{a.out}.rc")
    print(f"{OUT_DIR / (a.out + '.rc')}: {', '.join(a.names)} at t = {a.time:g} s")


if __name__ == "__main__":
    main()
