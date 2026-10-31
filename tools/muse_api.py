#!/usr/bin/env python3
"""Run a Muse command (docs/MUSE_COMMANDS.md) on a muse-gadget board over USB serial, as if
Muse had sent it, and print the result. For testing the commands without an agent.

  tools/muse_api.py rc.info
  tools/muse_api.py rc.get '{"name":"temp"}'
  tools/muse_api.py rc.set '{"values":{"temp":21,"weather":"Sunny"},"persist":true}'
  tools/muse_api.py rc.screenshot --out shot.png
  tools/muse_api.py register            # the size of this board's command registration

The board runs it from its console ('>api <command> <json>'): the same code path as
link.invoke, but the result is printed instead of sent. The port is opened as tools/snap.py
does (raw termios, modem lines untouched), so the board is not reset.
"""
import argparse, base64, glob, json, os, sys, termios, time

ap = argparse.ArgumentParser()
ap.add_argument("command")
ap.add_argument("params", nargs="?", default="")
ap.add_argument("--port", default=None)
ap.add_argument("--out", default=None, help="save an image result (rc.screenshot) here")
ap.add_argument("--timeout", type=float, default=20.0)
a = ap.parse_args()
port = a.port or (sorted(glob.glob("/dev/cu.usbmodem*")) or [None])[0]
if not port: sys.exit("no /dev/cu.usbmodem* port")
if a.params: json.loads(a.params)   # fail here, not on the board

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] = 0
t[1] = 0
t[2] = (t[2] & ~(termios.HUPCL | termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)) \
    | termios.CS8 | termios.CREAD | termios.CLOCAL
t[3] &= ~(termios.ICANON | termios.ECHO | termios.ISIG | termios.IEXTEN)
termios.tcsetattr(fd, termios.TCSANOW, t)

def read():
    try: return os.read(fd, 65536)
    except BlockingIOError: return b""

while read(): pass
line = ">api %s%s\n" % (a.command, (" " + a.params) if a.params else "")
if len(line.encode()) > 383:   # the SDK's console line (COMMAND_LINE_MAX), '\n' included
    sys.exit("command line too long for the console (%d bytes, 383 max)" % len(line.encode()))
os.write(fd, line.encode())

buf, png, in_png = b"", [], False
end = time.time() + a.timeout
while time.time() < end:
    chunk = read()
    if not chunk:
        time.sleep(0.01)
        continue
    buf += chunk
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        l = raw.decode(errors="replace").strip()
        if l.startswith("PNG BEGIN"): in_png = True; continue
        if l == "PNG END": in_png = False; continue
        if in_png: png.append(l); continue
        if l.startswith("API REGISTER"): print(l[4:]); sys.exit(0)
        if l.startswith("API RESULT "):
            result = json.loads(l[len("API RESULT "):])
            print(json.dumps(result, indent=2))
            if png:
                data = base64.b64decode("".join(png))
                out = a.out or "screenshot.png"
                open(out, "wb").write(data)
                print("saved %s (%d bytes)" % (out, len(data)))
            sys.exit(0 if result.get("ok") else 1)
sys.exit("no result within %.0f s" % a.timeout)
