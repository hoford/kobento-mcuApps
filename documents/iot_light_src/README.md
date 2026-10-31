# Light IoT panels (sources)

The 80 `iot-panels-light/*` documents from
[RemoteCompose-Examples](https://hoford.github.io/RemoteCompose-Examples/) (tagged #iot,
white backgrounds, 360 x 360 dp), downloaded 2026-10-23: each one's `doc.rc` and `doc.json`
(the JSON reproduces the `.rc`). `index.json` lists their titles.

They are not built into any player as they are (nothing globs this directory): all of them
animate, through `continuousSec()`. `tools/iot/make_iot_trio.py` freezes three of them and
puts them side by side in one document, `documents/wide/iot_trio.rc`, for e-paper.
