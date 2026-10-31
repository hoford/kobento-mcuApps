#!/usr/bin/env bash
# Update the Muse firmware on a board without losing its setup.
#
#   tools/update-muse.sh [--board BOARD] [--port PORT] [--build]   # default board: m5stack_tab5
#
# With more than one board plugged in, give --port (/dev/cu.usbmodem...): idf.py would
# otherwise pick one itself.
#
# Builds firmware/muse-gadget into build/<BOARD> and writes the app partition only
# (ota_0 at 0x20000). NVS, which holds the Muse pairing, device credentials and BLE bonds,
# is left alone, as are the bootloader and partition table. Wi-Fi and the SDK token come
# from firmware/muse-gadget/sdkconfig.secrets.
#
# Never `idf.py erase-flash` a paired board, and do not flash firmware/rcplayer onto it:
# that image's 6 MB app starts at 0x10000 and overwrites the Muse NVS at 0x11000, so the
# device would have to be paired again.
set -euo pipefail

board=m5stack_tab5
build_only=0
port=()
while [ $# -gt 0 ]; do
    case "$1" in
        --board) board="$2"; shift 2 ;;
        --port) port=(-p "$2"); shift 2 ;;
        --build) build_only=1; shift ;;
        *) echo "usage: $0 [--board BOARD] [--port PORT] [--build]" >&2; exit 2 ;;
    esac
done

here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here/firmware/muse-gadget"

[ -f sdkconfig.secrets ] || { echo "missing firmware/muse-gadget/sdkconfig.secrets" >&2; exit 1; }
# shellcheck disable=SC1091
source "${IDF_PATH:-$HOME/esp/esp-idf-v6}/export.sh" >/dev/null

idf.py -B "build/$board" -DBOARD="$board" build
[ "$build_only" = 1 ] && exit 0
idf.py -B "build/$board" -DBOARD="$board" ${port[@]+"${port[@]}"} app-flash
