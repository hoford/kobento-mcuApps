# kobento-mcuApps

**Full applications and integrations built on the Kobento MCU library.**

[`kobento-mcu`](../kobento-mcu) is a minimal library that *displays* RemoteCompose documents.
This repo is where the interesting, dependency-heavy things live: Wi-Fi, document delivery
([`kobento-net`](../kobento-net)), and product integrations like **Home Assistant** and the
**Muse** gadget. Keeping them here lets kobento-mcu stay tiny and build-anywhere, while these
apps show what a real device does.

Each app consumes kobento-mcu's components (`rccore`, `kobentovg`, `thorvg`, `rcstage`) and
board definitions, and adds its own code and documents.

## Apps

| App | What it shows | Builds standalone? |
|---|---|---|
| `esp32/firmware/rcplayer` | Wi-Fi + a **kobento-net** receiver (push documents / named variables over the network) + a **Home Assistant** client + the Wi-Fi setup screen | **Yes** (ESP-IDF + the sibling repos) |
| `esp32/firmware/muse-gadget` | the **Muse Gadget SDK** app with a Kobento screen | No — needs the external Muse SDK + a token |

## Expected beside this repository

```
kobento-mcuApps/   (this)
kobento-mcu/       the display library (components + boards)   REQUIRED
kobento-net/       the delivery protocol (cpp/ header is used) REQUIRED for rcplayer
remotecompose-experiments/   the engine (rccore)              REQUIRED
muse-gadget-sdk/   the Muse SDK, branch tab5-p4-hosted         only for muse-gadget
```

Override the kobento-mcu location with `-DKOBENTO_MCU=/path/to/kobento-mcu`.

## Build, flash — `rcplayer` (the networked player)

Needs ESP-IDF v6.0.1 (installed anywhere — `~/esp/esp-idf-v6` below is just the author's path)
and the `kobento-mcu` repo checked out alongside this one (or `-DKOBENTO_MCU=/path`).

```sh
. ~/esp/esp-idf-v6/export.sh                                  # or wherever you installed ESP-IDF
cd esp32/firmware/rcplayer
idf.py -B build/<board> -DBOARD=<board> build                 # <board> = a kobento-mcu board
idf.py -B build/<board> -DBOARD=<board> -p <PORT> app-flash
```

It embeds kobento-mcu's demo documents **and** this repo's integration documents (the Wi-Fi
screen, Home Assistant pages). Console adds `wifi`, `net`, `scan`, `ha`, `ble` to the base
player's commands.

- **Push a document** from a host with kobento-net:
  `python3 ../../../../kobento-net/python/kobento_net.py send <device-ip> doc.rc`
- **Home Assistant:** set the hub URL/token over the console (`ha url ...`, `ha token ...`);
  HA pushes values to the display via `POST /named` (kobento-net), and the thermostat pages
  send set-points back.

The **receiver** side of kobento-net is implemented in `net.cpp` using the header-only
`kobento_net.hpp` from kobento-net (`infoBody` / `parseNamed` / `sendOkBody` / …) — the device
and the host clients share one protocol implementation.

## Build — `muse-gadget`

Needs `../muse-gadget-sdk` (branch `tab5-p4-hosted`) and a gadget token in
`esp32/firmware/muse-gadget/sdkconfig.secrets` (`CONFIG_GADGET_SDK_TOKEN="mgst_..."`, gitignored).
Built with `../../../../tools/update-muse.sh` (writes the app partition only; keeps the Muse
pairing). Because of the external SDK + token, it does not build from a bare clone.

## Status

`rcplayer` wiring to kobento-mcu + kobento-net is in place and the receiver uses kobento-net's
protocol helpers. See the per-repo notes and the `questions` section of the hand-off for what
is verified. Integration documents and their generators are under `documents/` and `tools/`.

## License

Apache License 2.0: see [LICENSE](LICENSE). Third-party software and credits are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
