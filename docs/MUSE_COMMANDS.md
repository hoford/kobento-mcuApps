# Muse commands

What a Muse agent can ask a board running `firmware/muse-gadget` to do (`link.invoke`), beyond
what the Muse Gadget SDK answers itself. They are registered with `link.register` at connect
time, and the code is `firmware/muse-gadget/components/muse_rc/commands.cpp`. Keep the code and
this page in step.

Every reply is `{"ok": true, "payload": {...}}`, or `{"ok": false, "error": {"code", "message"}}`.
Parameters are JSON values. A boolean may be sent as `true`/`false`, `1`/`0` or `"true"`/`"false"`,
and a number may be sent as a string.

Try any command over USB without Muse: `tools/muse_api.py <command> '<json>'` (see
[Testing](#testing)).

| Command | What it does |
|---|---|
| [`rc.list`](#rclist) | built-in documents, which is showing, which is pinned |
| [`rc.show`](#rcshow) | show a built-in document (and pin it) |
| [`rc.show_url`](#rcshow_url) | download a `.rc` and show it (fitted, pinned, named) |
| [`rc.set`](#rcset) | set named variables; says which the document declares |
| [`rc.get`](#rcget) | read named variables back, as drawn |
| [`rc.pin`](#rcpin--rcunpin) / [`rc.unpin`](#rcpin--rcunpin) | keep the showing document across reboots / stop |
| [`rc.screenshot`](#rcscreenshot) | PNG of the screen as composed |
| [`rc.info`](#rcinfo) | screen geometry and the showing document's size |
| [`rc.last_touch`](#rclast_touch) | the last touch |
| [`rc.events`](#rcevents) | taps, long presses, buttons, document actions, loads |
| [`rc.banner`](#rcbanner) | a banner over the screen |
| [`device.time`](#devicetime) | system time, where it came from, RTC and NTP state |
| [`device.health`](#devicehealth) | the SDK's report, plus battery, Wi-Fi, IP, document |
| [`device.wifi`](#devicewifi) | the network joined and the saved ones; add, forget or join one |
| [`device.beep`](#devicebeep) | a tone (boards with a speaker or buzzer) |
| [`device.motion`](#devicemotion) | accelerometer and gyroscope, and which way is down |
| [`device.sleep`](#devicesleep) | sleep N seconds, or between the document's frames (PaperS3) |
| [`device.battery_log`](#devicebattery_log) | a battery reading per auto-sleep check-in, and the drain (PaperS3) |
| [`display.refresh`](#displayrefresh) | full e-ink refresh (e-ink boards) |

## Documents

### rc.list

`{}` → `{"documents": ["dashboard", ...], "showing": "muse_splash", "pinned": null}`

`documents` are the built-in ones. Variants for other screens are listed too (`_round`,
`_light`), but `rc.show` picks the right one by itself.

### rc.show

`{"name": "dashboard", "persist": false}` → `{"showing": "dashboard", "bytes": 8575, "fit": false}`

The reply comes once the document is showing. `persist: true` also pins it (see `rc.pin`),
and the reply adds `"pinned": true`. Errors:
- `not_found`: no such document; see `rc.list`.
- `load_failed`: the player couldn't load it, and the old document stays.

### rc.show_url

`{"url": "https://.../maze/doc.rc", "fit": true, "persist": false, "name": "maze"}` →
`{"showing": "maze", "bytes": 5179, "fit": true}`

Downloads a `.rc` file of up to 4 MB over http or https and replies once it's showing.
- `fit`: lay the document out at its own size (its header's, else 600 × 600) and scale it
  to fit the screen, centred on black. Use it for documents designed at a fixed size: the
  examples site's, watch faces, anything drawn for a phone canvas. Without `fit`, the
  document lays out at the screen's logical size (`rc.info` `logical`), as the built-in ones do.
- `persist: true`: pin it. The bytes are kept in flash, so it shows after a reboot without
  the network.
- `name`: what `rc.info`, `rc.list` and `device.health` call it. By default it's the file
  name without `.rc`, or the folder when the file is `doc.rc` / `index.rc`.

Errors:
- `download_failed`: the message has the HTTP status.
- `load_failed`: not a document this player can load; the old one stays.

### rc.pin / rc.unpin

`rc.pin {}` → `{"showing": "maze", "bytes": 5179, "fit": true, "pinned": true}`

Keeps the showing document across reboots. The board starts on the start screen as usual,
then switches to the pinned document as soon as storage is up, about a second later. A
built-in document is kept by name; any other document is kept whole in flash. Pinning again
replaces the old pin. `pinned` is `false` with a `pin_error` if it couldn't be kept.

`rc.unpin {"keep_values": false}` → `{}`

The board goes back to booting on its start screen. It also forgets the values set with
`persist`, unless `keep_values` is true. The document showing now stays until something
else is shown.

Storage:
- Name and kind: NVS, namespace `rcpin`.
- Downloaded documents: a 4 MB flash region at 12 MB, above the partition table, registered
  at run time. The partition table and the app-only update are unchanged.
- A board whose flash is smaller than 16 MB can pin only built-in documents.

## Variables

### rc.set

```json
{"name": "temp", "value": "72°"}
{"values": {"temp": "72°", "stock_change": -0.42, "weather": "Sunny"}, "persist": true}
```
→
```json
{"showing": "dashboard", "applied": ["temp", "stock_change"], "not_declared": ["weather"], "persisted": true}
```

Sets named variables, the document's exported `NAMED_VARIABLE`s:
- A number for a float or integer variable.
- `#RRGGBB` or `#AARRGGBB` for a colour.
- Anything else for text.
- Booleans become `1` or `0`.

`values` sets several at once and may also be a JSON string of that object. The reply says
which names the showing document declares (`applied`) and which it doesn't (`not_declared`),
so a push to the wrong document is visible, not silent.

Values are sticky. One the showing document doesn't declare still reaches the next document
loaded that does. `persist: true` keeps the values in NVS, and they're applied again after a
reboot. They're written only when a value changes, so an agent can send `persist` every time.

### rc.get

`{"name": "stock_change"}` → `{"showing": "dashboard", "name": "stock_change", "type": "number", "value": -0.42}`

`{}` → `{"showing": "dashboard", "variables": [{"name", "type", "value"}, ...]}`

Reads the values the player has now, read between frames, including any the document
changed itself. `type`:
- `number` and `integer`: the value is a JSON number.
- `color`: the value is `#AARRGGBB`.
- `text`: the value is the string.

Errors:
- `not_declared`: the showing document has no such variable.
- `busy`: the player didn't answer within 2 s (a very slow frame).

## The screen

### rc.screenshot

`{"width": 360, "gray": false}` →
`{"format": "png-base64", "width": 360, "height": 640, "color": "rgb", "png_bytes": 35406, "ms": 352, "showing": "dashboard", "data_base64": "iVBOR..."}`

A PNG of the last frame drawn: the document and the status banner, as composed and before
the panel's own conversion.
- `width`: defaults to the logical width (`rc.info` `logical.w`), up to the render width;
  the height keeps the aspect.
- On e-ink the PNG is grey by default, quantised to the panel's 16 levels (without its
  dither).

Typical sizes: Tab5 360 × 640 colour, about 35 KB in 350 ms; PaperS3 360 × 640 grey, about
13 KB in 250 ms.

### rc.info

`{}` →
```json
{"board": "m5stack_tab5", "board_name": "M5Stack Tab5  ·  ESP32-P4",
 "panel": {"w": 720, "h": 1280}, "density": 2, "logical": {"w": 360, "h": 640},
 "render": {"w": 720, "h": 1280}, "round": false, "eink": false, "grays": null,
 "sound": "speaker",
 "document": {"name": "dashboard", "size": {"w": 360, "h": 640}, "fit": false, "scale": 1, "bytes": 8575},
 "pinned": null, "firmware": "7a7f85c-dirty",
 "sleep": {"woke": "rtc", "auto": true, "mode": "off", "check_in_s": 900, "awake_s": 30, "next_frame_ms": 41250}}
```

- `panel`: pixels.
- `logical`: the dp a document lays out in (panel ÷ density).
- `render`: the frame the player draws.
- `grays`: the panel's grey levels on e-ink, `null` for colour.
- `sound`: `speaker`, `buzzer` or `""`.
- `document.size`: the size the showing document lays out at (its own when `fit`).
- `document.scale`: how much it's scaled to the screen.
- `sleep`: why the board is running, the auto-sleep settings, and when the document next
  needs a frame (see `device.sleep`).

| Board | panel | density | logical | grays | sound |
|---|---|---|---|---|---|
| m5stack_tab5 | 720 × 1280 | 2 | 360 × 640 | – | speaker |
| m5stack_papers3 | 540 × 960 | 1.5 | 360 × 640 | 16 | buzzer |

### rc.last_touch

`{}` → `{"down": false, "presses": 3, "x": 182.5, "y": 401, "panel_x": 365, "panel_y": 802, "age_ms": 5321}`

Where the screen was last touched:
- `x`, `y`: in the document's dp, the same coordinates its layout uses, so a fitted document
  gets its own.
- `panel_x`, `panel_y`: panel pixels.
- `age_ms`: since the last contact (`null`: never).
- `presses`: how many presses since boot.

### rc.events

`{"since": 12}` →
```json
{"events": [{"seq": 13, "type": "tap", "age_ms": 840, "x": 180, "y": 320},
            {"seq": 14, "type": "action", "age_ms": 838, "name": "0", "value": "doc.next"}],
 "latest": 14}
```

What happened on the device, oldest first. The last 32 events are kept; poll with the
previous `latest` as `since`.

| type | fields |
|---|---|
| `tap` | `x`, `y` (document dp) |
| `long_press` | `x`, `y`; it also steps to the next built-in document |
| `button` | `value` `+1` (forward) or `-1` (back): a hardware button, which also steps documents |
| `action` | a document's `hostAction(id, metadata)`: `name` = id, `value` = metadata |
| `named_action` | a document's `HostNamedAction`: `name`, `value` |
| `document` | a document was loaded: `name` |

A document can raise its own events for the agent with a `hostAction` whose metadata the
player doesn't use itself. The player's own actions are in `docs/RC_ACTION_PATTERNS.md`.

### rc.banner

`{"title": "Updated", "detail": "NASDAQ +1.2%"}` → `{}`

A banner across the bottom of the screen; an empty title hides it. The player's own status
banners (Wi-Fi, pairing) replace it when the status changes.

### display.refresh

`{"mode": "full"}` → `{"mode": "full"}`

E-ink only, and registered only there. It redraws every pixel of the picture on the panel,
which clears the ghosting left by many partial updates. It takes as long as a full update.
Other panels get `unsupported`.

## The device

### device.time

`{}` →
```json
{"epoch": 1792552488.68, "utc": "2026-10-21T03:14:48Z", "local": "2026-10-20T20:14:48-07:00",
 "utc_offset": -25200, "zone": "PDT", "tz": "PST8PDT,M3.2.0,M11.1.0",
 "source": "ntp", "synced": true,
 "ntp": {"server": "pool.ntp.org", "last_sync": 1792552463, "age_s": 25},
 "rtc": {"valid": true, "epoch": 1792552487, "drift_s": -1},
 "uptime_s": 36}
```

What time the device thinks it is, and why.
- `source`:
  - `ntp`: SNTP set it.
  - `rtc`: the clock chip set it at boot, before the network.
  - `none`: nothing has set it, so the time is not to be trusted.
- `ntp.last_sync`: the last SNTP sync, `null` if none. SNTP syncs hourly once Wi-Fi has an
  address.
- `rtc`:
  - `valid`: false with no RTC, or when it lost power.
  - `epoch`: the RTC's own time, read now.
  - `drift_s`: RTC minus system time.
- The time zone is the build's (`CONFIG_RCSTAGE_TIMEZONE`).

The RTC is set from every NTP sync, and the system time comes from the RTC at boot. So a
board that restarts without a network keeps the right time:
- Tab5: RX8130 at 0x32.
- PaperS3: BM8563 at 0x51.

### device.health

The SDK answers this one itself. This project fills in what it leaves `null` and adds a few
fields:

```json
{"overall": "ok", "metrics": {"uptime_seconds": 36, "heap_free_internal": ..., "heap_free_psram": ...,
  "battery_pct": 99, "battery_mv": 8355, "charging": false, "usb_power": true,
  "battery_ma": -1, "wifi_rssi": -42, "temperature_c": 37},
 "network_ssid": "...", "version": "...",
 "ip": "192.168.50.154", "board": "m5stack_tab5", "showing": "dashboard", "pinned": null}
```

- `battery_pct`, `battery_mv`, `charging`, `usb_power`:
  - Tab5: from the INA226 (2S pack).
  - PaperS3: from its battery ADC, charge-status and USB-detect lines.
- `battery_ma`: positive into the battery; Tab5 only.
- `temperature_c`: the chip's own sensor.
- `ip`, `board`, `showing`, `pinned`, `wifi_ssid` (null when off Wi-Fi): added at the top
  level. `rc.info` has `wifi_ssid` too.
- `woke`: why the board is running: `timer` or `rtc` (a `device.sleep` wake), `power_on`,
  `restart`, `reset`.
- `sleep`: the auto-sleep state, as in `rc.info` (see `device.sleep`). Includes `holding`
  and `next_check_in`.

### device.wifi

```json
{}
{"add": "Office", "password": "secret123"}
{"add": "HiddenNet", "password": "secret123", "hidden": true}
{"forget": "Office"}
{"join": "Office"}
```

Every form replies with the state after any change (passwords are never sent back):

```json
{"connected": true, "ssid": "NIXIOT", "rssi": -42, "channel": 11,
 "saved": [{"ssid": "NIXIOT", "password": true, "firmware": true},
           {"ssid": "Google-Guest-Legacy", "password": false}]}
```

- **`add`** saves a network and its password (8–64 characters; none for an open network). It
  doesn't join it now: the board joins it whenever it's in range, at boot or by roaming. This
  is the way to give a board a network with a password. The on-screen Wi-Fi list can only
  join open networks and saved ones. Saving a network again replaces its password.
- **`forget`** removes a saved network. The firmware's own network (`"firmware": true`,
  `CONFIG_HOMEHUB_WIFI_SSID`) can't be forgotten.
- **`join`** moves to a saved network now. The reply comes first; the agent's session then
  drops and comes back on the new network (about 15 s). If the join fails, roaming brings
  the board back to a network in range.
- When off Wi-Fi, the state has `down_s` instead of `ssid`.

**Moving between places** (home and work, say):
- **Save each place's network once.** Do it while connected, with `add`; open networks
  can also be picked on the device's Wi-Fi screen.
- **At boot** the board tries the firmware's network, then whichever saved network is in
  range, strongest first. An auto-sleep check-in is a boot, so a sleeping PaperS3 that
  moves finds the new network at its next check-in.
- **Roaming:** if Wi-Fi drops while the board is on, the SDK only retries the network it
  lost. After 30 s down, the board scans and joins the strongest saved network in range.
  With none in range it looks again after 30 s, doubling up to 5 minutes. Tested 2026-10-21:
  back on Wi-Fi 30 s after the drop, and on Muse 7 s after that.
- **At start** (a restart or the power button) the board shows its start screen until Wi-Fi
  joins, then the pinned document. With no network within 60 s, or a touch on the start
  screen, it opens the launcher, whose top row is Wi-Fi. (A finger already on the screen at
  power-up doesn't count: the touch chip calibrates it away. Lift it and tap.) A long press opens the launcher at any time; its Back button returns to
  the pinned document. Picking an open network on the Wi-Fi screen saves it.
- **Later**, if no saved network can be joined, the Wi-Fi screen opens by itself.

### device.beep

`{"hz": 880, "ms": 200, "count": 1, "gap_ms": 120}` → `{"on": "speaker", "hz": 880, "ms": 200, "count": 1}`

Plays a tone without waiting for it; a newer tone replaces one still playing. Ranges:
- `hz`: 50–10000
- `ms`: 10–5000
- `count`: 1–10
- `gap_ms`: 20–2000

It's only registered on boards with something to play it on:
- Tab5: a sine through the ES8388 codec and speaker; the first tone also starts the codec.
- PaperS3: a square wave on the passive buzzer, GPIO 21.

### device.motion

`{}` → `{"accel": {"x": 0.2, "y": 8.93, "z": 3.56}, "gyro": {"x": 0, "y": 0, "z": 0}, "orientation": "upright", "g": 0.98}`

A snapshot from the BMI270 (Tab5, PaperS3).
- Units: accelerometer in m/s² with gravity included, gyroscope in rad/s.
- Axes are the screen's, as Android reports them: x to the right, y up the screen, z out
  of it. Lying face up, z = +9.8; standing upright, y = +9.8.
- `orientation`: the axis gravity dominates: `upright`, `upside_down`, `left_edge_down`,
  `right_edge_down`, `face_up` or `face_down`.
- `g`: the total in g. Well above 1 means it's being moved or shaken.

Documents get the same readings as Android's sensor system variables (accelX..gyroZ, ids
17–22) at 50 Hz, while one uses them. Error `unsupported`: no motion sensor.

The z axis on both boards is checked (the chips face the back). The x axis on both is not
yet checked by tilting; see `imu` on the board's console.

### device.sleep

PaperS3 only for now; it is registered only there.

For document authors, `docs/RC_SLEEP.md` explains how a document sets the wakes.

```json
{"seconds": 600, "mode": "off"}
{"auto": true, "mode": "off", "check_in_s": 900, "awake_s": 60}
{"auto": true, "hold": true}
{"resume": true}
{"auto": false}
```

The e-ink keeps its picture without power, so a sleeping PaperS3 looks the same as an awake
one. Muse can't reach it while it sleeps.

| mode | What sleeps | Waking | Kept |
|---|---|---|---|
| `off` (default) | everything: the board switches itself off | the RTC switches it on: a full boot | NVS, flash: the pinned document, persisted values |
| `deep` | everything but the ESP32's timer | a full boot | the same |
| `light` | the CPU pauses; Wi-Fi drops | it carries on, then reconnects | everything, the document's state too |

`off` sleeps for at most 255 minutes (the RTC's timer). It also worked on USB power in
testing (2026-10-20), and the board falls back to `deep` if it ever stays on.

**Once:** `{"seconds": N, "mode": ...}` replies, then sleeps 1.5 s later. Without
`seconds`, it sleeps until the document's next frame (`bad_param` if it has none, or
animates).

**Auto:** `{"auto": true, ...}` keeps doing it, by the document's own schedule.
- After each frame, the board sleeps until the document next needs one. The engine's
  schedule is what the Java player uses (`RemoteComposeView` reads `needsRepaint()`):
  - reading the time in seconds means a frame each second;
  - a minute hand means a frame each minute;
  - hours mean a frame each hour;
  - a `WakeIn(s)` op means a frame in s seconds;
  - continuous time (`continuousSec`) means animating, which never sleeps.
- **Local wakes:** in `off` and `deep`, a wake for a frame doesn't start Wi-Fi or Muse.
  The board boots, draws the pinned document with the time from the RTC, and sleeps again.
  It wakes 3 s early and draws on the boundary. Measured on the PaperS3 with a minute hand:
  awake 2.5–4.7 s a minute.
- **Check-ins:** every `check_in_s` (60–15300, default 900) it boots fully, connects to Muse,
  and stays up `awake_s` (60–600, default 60) from connecting. Each command, touch or button
  press gives it `awake_s` more. This is when an agent can push values, change the document,
  or send `{"auto": false}`. A static document (nothing scheduled) sleeps from check-in to
  check-in. If it can't connect, it gives up a minute after `awake_s`.
- **Hold:** with `"hold": true`, a check-in doesn't end on a timer. Once connected, the
  board stays awake until the agent sends `{"resume": true}`. Use it when the agent must be
  sure of reaching the board (see below).
  - `hold_max_s` (default 3600; 0: no limit) is the safety net: an agent that never comes
    still lets the board sleep after that long. With no limit, a lost agent keeps it awake
    until the battery runs out (about 18 h).
  - Turning auto on with hold starts a hold straight away: send `resume` to begin the cycle.
- **`{"resume": true}`** ends the check-in now: the board sleeps about 2 s after replying,
  until the next frame or check-in. It also ends the window after a button press. The reply
  gives `next_check_in` (epoch s). `bad_param` if auto is off.
- **When it stays awake regardless:**
  - On USB power: there's no battery to save, and you're probably working on it. It
    keeps checking in, holding and logging, but doesn't sleep until it's unplugged.
  - For 5 minutes after a wake that wasn't the timer (`woke` of `power_on`, `restart` or
    `reset`): pressing the power button means someone is there. `resume` ends it early.
- Gaps shorter than 8 s aren't slept in `off` and `deep`; the board stays awake for them.
- The settings survive reboots (NVS). `rc.info` `sleep` and `device.health` `sleep` show
  them, with `holding` (connected at a check-in, waiting for `resume`) and `next_check_in`
  (epoch s, when the board next connects).

**For agents: reaching a sleeping board.** Muse can't wake it, so a command sent while it
sleeps fails. Either:
- **Hold (sure):** set `{"auto": true, "hold": true}` once. At each check-in the board
  connects and waits. When the agent sees the board connect (or finds `device.health`
  `sleep.holding` true), it does its work, then sends `{"resume": true}`. Forgetting
  `resume` costs battery (up to `hold_max_s` per check-in), not correctness.
- **Timed (cheaper):** without hold, the agent has `awake_s` after the board connects.
  Read `next_check_in` from the last `device.health` and send then; each command extends the
  window. Raise `awake_s` if the agent's turn is slow.
- Either way, to work with the board for a while, send `{"auto": false}` first and turn auto
  back on when done (that session counts as a check-in).

**What a document can count on:**
- On every wake it starts afresh in `off` and `deep`: the pinned document with its persisted
  values and fresh device variables (battery, temperature). Anything the document computed
  itself is gone.
- A document that keeps history needs `light`, such as a graph of temperature readings
  taken every 5 minutes with `WakeIn(300)`. Otherwise the agent keeps the history and
  pushes it at each check-in.
- Pin the document first (`rc.pin` or `persist`), or the board wakes to its start screen.

### device.battery_log

`{}` →
```json
{"readings": [{"t": 1792561435, "mv": 4146, "pct": 100, "usb": false, "charging": false, "connected": true}, ...],
 "on_battery": {"readings": 40, "hours": 9.75, "mv_drop": 92, "mv_per_hour": 9.4,
                "pct_per_hour": 0.51, "days_full_to_empty_rough": 8.2}}
```

`{"clear": true}` → `{}`: start a new log.

One reading at the end of each auto-sleep check-in (Wi-Fi up, so a little lower than at
rest). The last 256 are kept in NVS, which is 64 hours at the default 15-minute check-in.
- `t`: epoch seconds.
- `pct`: the board's level, a straight line from 3.30 V to 4.15 V.
- `usb`: on USB, which charges the battery, so these readings say nothing about drain.

`on_battery` summarises the latest stretch of readings off USB:
- `mv_per_hour`: the voltage drop per hour.
- `pct_per_hour` and `days_full_to_empty_rough`: from the board's level. A lithium cell
  isn't a straight line, so trust them over a day or more, not an hour.

To measure a schedule:
1. Pin the document.
2. `device.battery_log {"clear": true}`.
3. Turn auto sleep on.
4. Unplug USB.
5. Read the log after a day.

## Testing

`tools/muse_api.py` runs a command on a board over USB serial, through the same code as
`link.invoke`, and prints the result. The port is opened without resetting the board.

```sh
tools/muse_api.py --port /dev/cu.usbmodem31101 rc.info
tools/muse_api.py rc.set '{"values":{"temp":21},"persist":true}'
tools/muse_api.py rc.screenshot --out shot.png
tools/muse_api.py device.health      # the fields this project adds
tools/muse_api.py register           # this board's registration size
```

- The console line limit is 383 bytes (`COMMAND_LINE_MAX` in the SDK's
  `sdk_token_console.c`).
- `link.register` must fit in 8 KB in all. These commands take about 2.9 KB, so keep their
  descriptions short and put the detail here.
