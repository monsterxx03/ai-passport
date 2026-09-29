<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Tachi Badge

Put [tachi](https://github.com/monsterxx03/tachi)'s "waiting" onto the AI Passport's small screen.

## What it solves

An agent stops for a human twice: when it wants to run a command the policy marks as needing
confirmation, and when it has a question for you. Both waits normally live on the computer screen —
walk away from the desk and you stop seeing them.

This firmware makes the AI Passport (a 240×320 screen with three buttons) a second door onto that
wait: the screen says who is waiting and what for, and the buttons carry your decision back.

## The three screens

| Screen | When | What you can do |
| --- | --- | --- |
| Status | the usual case | see who is running, how far along, whether the computer is connected, battery |
| Confirm | a command wants approval | read the preview, pick allow once / allow for this session / deny |
| Question | the model asked something | a list of options, multi-select supported |

Buttons: a short up/down moves between options, OK submits. On a multi-select question OK toggles
and a long press submits; when one ask holds several questions, a **long up/down switches
questions** (a single-select question advances on its own once answered).

The screen also goes quiet on its own: with nothing pending and nobody pressing anything, the
backlight turns off after 60 seconds. What saves power is the backlight, not sleeping the chip —
a sleeping chip cannot hear the ask the host is trying to deliver. **The screen never blanks while
an ask is pending**; after it blanks, any button lights it immediately, and that press only lights
it (on a dark screen you cannot see where the cursor is, so it must not submit anything). A new ask,
or the link dropping, lights it too.

## How it talks to the computer

One USB serial line (the ESP32-C3's USB-Serial-JTAG), line-delimited JSON, every line prefixed
with `@@`.

The prefix is not fussiness: the device's console log and the protocol share that one line, and the
prefix is the only thing separating them — you can watch them interleave on boot
(`I (156) esp_image: ...` right next to `@@{"t":"hello"...}`).

The other half of the protocol lives in the tachi repository: `desktop/link/link.go`, with the design
notes in `docs/2026-09-28-agent-badge-link-design.md` (that document records why this looks the way
it does, and the constraints that were measured locally).

## Build

Requires ESP-IDF 5.5.3.

```bash
. ~/esp/esp-idf-v5.5.3/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash
```

### Generate the avatar first

The middle of the status screen is tachi's face (no longer a placeholder circle), also a generated
artifact cut from tachi's app icon and not committed:

```bash
uv run tools/mk_badge_avatar.py        # deps are declared in the script; uv handles the env
```

It reads `~/repos/tachi/desktop/build/appicon.png` by default; `BADGE_AVATAR_SRC` overrides it.
Changing the face is a one-place edit: the script trims the white margin, scales to 120x120 and
paints the corners with the screen's background colour.

### Generate the Chinese font first

The Chinese text on screen comes from tachi (session titles, command previews, the questions the
model asks), so the font has to cover **arbitrary** common Han characters — LVGL's built-in CJK
subset is small enough to lack everyday characters such as U+8111 (brain), U+8FD8 (still) and
U+8FDE (connect), and a missing glyph is a hollow box.

The font is generated, not committed: it is cut from a font on the local machine, macOS's own
Chinese fonts may not be redistributed, and the source text is 15MB. Run this once before the first
build:

```bash
./tools/gen_badge_font.sh          # defaults to /Library/Fonts/Arial Unicode.ttf
```

To publish, point it at an OFL font (Source Han Sans / Noto Sans SC); nothing else in the script
changes. At that point the artifact, its source and its license can be committed together.

## Layout

```
main/badge_proto.c    the protocol: parse and encode one line of JSON (pure logic, host-testable)
main/badge_json.c     the minimal JSON reader/writer used by it (no deps, no allocation)
main/badge_link.c     the serial line: framing, only lines carrying @@ count
main/badge_state.c    the state machine: what arrives, what a key press sends (pure, host-testable)
main/badge_power.c    screen on/off: how long idle blanks it, what lights it (pure, host-testable)
main/badge_ui.c       the three screens (its own UI, not the baseline demo menu shell)
main/main.c           entry: init, event loop, heartbeat and timeout
tests/test_badge_*.c  unit tests for the pure parts (`./tools/validate.sh --static` runs them)
```

## Known constraints

- **LVGL's font bitmap index is 20 bits wide by default (a 1MB ceiling).** This font has over
  twenty thousand glyphs and exceeds it, so `sdkconfig.defaults` enables
  `CONFIG_LV_FONT_FMT_TXT_LARGE`. Without it the build fails with `-Woverflow` and the index is
  truncated to its low 20 bits — the font table comes out scrambled, which looks like a broken
  font rather than a configuration limit.
- **After editing `sdkconfig.defaults` you must delete `sdkconfig`** before building: the defaults
  only apply when the configuration is generated, an existing `sdkconfig` does not follow them.
- **Font generation must pass `--no-compress`.** `lv_font_conv` compresses bitmaps with RLE by
  default and writes `bitmap_format = 1`; that format is from the LVGL 8 era and LVGL 9.5 cannot
  decode it. The failure is deceptive: glyphs resolve, bounding boxes are correct, yet nothing is
  drawn at all — while a built-in font renders fine, so it reads like "the font is too big",
  "out of memory" or "the renderer is broken". (`tools/gen_badge_font.sh` already passes it.)
- The serial port is exclusive: while this firmware runs, `idf.py monitor` cannot open the same port.
