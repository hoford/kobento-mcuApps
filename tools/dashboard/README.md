# Dashboard template

A clock (hours and minutes) in the top half, with an extra hour hand for another city (orange by day, blue at
night), and weather and a stock in the bottom half. 360 x 640 dp, scaled to fit the window.

- `dashboard.json` is the document in RC JSON, the format of
  [RemoteCompose-Examples](https://hoford.github.io/RemoteCompose-Examples/).
- `make_dashboard.py` generates it and compiles `documents/dashboard.rc`.

## Make a new one

1. Copy `dashboard.json` and edit the `variables` at its top: `temp`, `weather`, `high`,
   `low`, `location`, `sunrise`, `sunset`, `stock_name`, `stock_value`, `stock_change`
   (a number in percent: its sign picks green ↑ or red ↓), `stock_date`, `city2_name`,
   `city2_utc_offset` (hours), `city2_day_start` and `city2_day_end` (hours). The colours
   are under `colors`.
2. Compile it: `node tools/splash/compile.mjs my.json my.rc`.
3. Check it before sending it: `host/build/rc2png my.rc my.png --fit 720 1280 --density 2`.
   Add `--time <epoch ms>` to see another time of day.

The variables are exported, so a host can change them while the document shows. On a Muse
gadget: `rc.set` with `name` `temp` and `value` `61°`.

## Comments in the JSON

JSON has no comments, so `dashboard.json` carries them as data: a `"comment"` list of lines
at the top, and a `"comment"` on each variable. json2rc ignores keys it doesn't know, and the
compiled `.rc` is byte-identical with or without them. Don't put one inside `colors`: each
key there is read as a colour.

## Rules learned the hard way

- **Set `textSize` in the paint before every text drawing.** Paint state carries over, and a
  document that never sets it draws all its text at the player's default size
  (`tab5_dashboard_v3.rc` did that).
- **Use glyphs Roboto has.** The players' font has ↑ ↓ ° · but not ▲ ▼; a missing glyph
  draws nothing.
- **Time:** `timeInMin` is minutes since local midnight, `timeInHr` the hour and
  `offsetToUtc` the local offset in seconds. Another city's time:
  `(timeInMin - offsetToUtc / 60 + hours * 60 + 2880) % 1440`.
- **Don't read seconds** (`timeInSec`, `continuousSec`, an animation): the document then
  changes every second, and every change repaints the whole screen (about 0.1-0.4 s on the
  Tab5, a visible refresh on e-ink). Hence no second hand. (On these players `timeInSec` is
  also only the seconds into the current hour.)
- **Conditional drawing:** `conditionalOperations` with `eq`, `ne`, `lt`, `ge`, ... against
  a variable or an expression, as the hand colour and the stock arrow do here.
