#!/usr/bin/env python3
"""Save the document a board is showing: tools/getdoc.py OUT.rc [--port PORT] [--prefix '>']
Sends `doc` on the console (raw os.open, so the board isn't reset) and decodes the
DOC BEGIN / DOC END block. Render it with host/build/rc2png."""
import argparse, base64, glob, os, termios, time

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--port", default=(glob.glob("/dev/cu.usbmodem*") or [""])[0])
ap.add_argument("--prefix", default=">", help="'>' for firmware/muse-gadget, '' for rcplayer")
ap.add_argument("--wait", type=float, default=15)
a = ap.parse_args()

fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] = t[1] = t[3] = 0
t[2] = (t[2] & ~(termios.HUPCL | termios.CSIZE | termios.PARENB | termios.CRTSCTS)) | termios.CS8 | termios.CREAD | termios.CLOCAL
t[4] = t[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, t)
os.write(fd, (a.prefix + "doc\n").encode())
buf, end = b"", time.monotonic() + a.wait
while time.monotonic() < end and b"DOC END" not in buf:
    try:
        buf += os.read(fd, 65536)
    except BlockingIOError:
        time.sleep(0.02)
text = buf.decode(errors="replace")
if "DOC BEGIN" not in text or "DOC END" not in text:
    raise SystemExit("no document in the reply")
body = text[text.index("DOC BEGIN"):text.index("DOC END")].splitlines()
size, name = body[0].split()[2], " ".join(body[0].split()[3:])
data = b"".join(base64.b64decode(l.strip()) for l in body[1:] if l.strip() and not l.startswith(("I (", "W (", "E (")))
if len(data) != int(size):
    raise SystemExit(f"got {len(data)} of {size} bytes")
open(a.out, "wb").write(data)
print(f"{a.out}: {name}, {len(data)} bytes")
