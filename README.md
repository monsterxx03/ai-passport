<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Tachi Badge

Put [tachi](https://github.com/monsterxx03/tachi)'s "waiting" onto the AI Passport's small screen.

## What it solves

An agent stops for a human twice: when it wants to run a command the policy marks as needing
confirmation, and when it has a question for you. Both waits normally live on the computer screen —
walk away from the desk and you stop seeing them.

This firmware makes the AI Passport (a 240×320 screen with three buttons) a second door onto that
wait: the screen says who is waiting and what for, and the buttons carry your decision back.

## The four screens

| Screen | When | What you can do |
| --- | --- | --- |
| Status | the usual case | see who is running, how far along, whether the computer is connected, battery — the top bar also says which link is in use (BLE or USB) |
| Confirm | a command wants approval | read the preview, pick allow once / allow for this session / deny |
| Question | the model asked something | a list of options, multi-select supported |
| Pairing | the first time over Bluetooth | the 6-digit code on the screen, to be typed on the computer |

The pairing screen overrides the other three: while that code is up, typing it into the computer is
your only job and everything else is noise.

**It also makes a sound — two different ones**: a rising chime when something starts waiting for you
(a permission or a question), and two falling notes when a turn finishes. You can tell "someone needs
you" from "that's done, nothing for you" without looking at the screen.

The rule lives on the computer — tachi's window is not in the foreground, **or** the event belongs to
a session other than the one you are looking at; if you are watching it, it stays quiet (that sound
would just be noise). The decision is made the moment the event happens, so walking away afterwards
does not re-alert. There is a switch in the settings page (`badge.alert`, on by default).

Both clips are generated artifacts (PCM from `tools/gen_badge_sound.py`, shipped with the
repository). To use your own:

```bash
uv run tools/gen_badge_sound.py your.wav                    # the "something is waiting" sound
uv run tools/gen_badge_sound.py your.wav --kind done        # the "turn finished" sound
```

(16 kHz mono WAV; run `afconvert` first for m4a/mp3.) ⚠ Mind the rights to whatever you use: the
artifact is committed, so someone else's recording would be redistributed with it — keep that kind of
clip local instead.

Buttons: a short up/down moves between options, OK submits. On a multi-select question OK toggles
and a long press submits; when one ask holds several questions, a **long up/down switches
questions** (a single-select question advances on its own once answered). **When the body does not
fit, a long up/down pages through it** (one screenful, 3 lines) and only switches questions at either
end; when it fits, nothing changes. On the status screen a long press on OK forgets the computer
(twice — the first press only asks).

The screen also goes quiet on its own: with nothing pending and nobody pressing anything, the
backlight turns off after 60 seconds. What saves power is the backlight, not sleeping the chip —
a sleeping chip cannot hear the ask the host is trying to deliver. **The screen never blanks while
an ask is pending, nor while pairing** (that code is the task at hand and you are at the computer
typing it); after it blanks, any button lights it immediately, and that press only lights
it (on a dark screen you cannot see where the cursor is, so it must not submit anything). A new ask,
or the link dropping, lights it too.

An option that does not fit is no longer just cut off: the **selected row** scrolls horizontally to
show the whole label while the others stay dotted. Only the selected row moves because five rows
scrolling at once are unreadable, and the animation stops when the screen blanks — text moving
behind a dark screen only burns power.

**What does not fit is said out loud** rather than quietly dropped. When one question carries more
options than the screen holds, the spare row says how many are waiting on the computer instead of
hiding them. When an ask carries more questions than the screen holds — or a question whose text is
too long to display, which cannot be shortened because that text is the answer key — a one-shot
notice says how many are missing: answering only what is on screen would hand the model an answer
missing questions it never heard about. And an answer too large to encode says so, instead of
leaving the button doing nothing.

## How it talks to the computer

One link: a USB serial line (the ESP32-C3's USB-Serial-JTAG), or Bluetooth with no cable at all. Both
carry the same protocol — line-delimited JSON, every line prefixed with `@@` — so the transport
changes nothing about it.

Bluetooth pairs once: the device shows a 6-digit code and you type it on the computer (reconnects
skip that step).

The prefix is not fussiness: on the serial line the device's console log and the protocol share it,
and the prefix is the only thing separating them — you can watch them interleave on boot
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

### The avatar (committed)

The middle of the status screen is tachi's face (no longer a placeholder circle). It is generated
too, but it **ships with the repository** (`main/badge_avatar.c`, 180 KB), so a clone has it;
regenerate only when the face changes:

```bash
uv run tools/mk_badge_avatar.py        # deps are declared in the script; uv handles the env
```

It reads `~/repos/tachi/desktop/build/appicon.png` by default; `BADGE_AVATAR_SRC` overrides it.
Changing the face is a one-place edit: the script trims the white margin, scales to 120x120 and
paints the corners with the screen's background colour.

### Fonts (committed; you normally never touch this)

The Chinese text on screen comes from tachi (session titles, command previews, the questions the
model asks), so the font has to cover **arbitrary** common Han characters — LVGL's built-in CJK
subset is small enough to lack everyday characters such as U+8111 (brain), U+8FD8 (still) and
U+8FDE (connect), and a missing glyph is a hollow box.

The generated fonts **ship with the repository** (`main/badge_font_16.c`, ~18 MB, and
`main/badge_font_icon_16.c`, a few KB), so a clone builds as-is. Regenerate only when the source
font, the character ranges or the size change:

```bash
./tools/gen_badge_font.sh          # defaults to the Source Han Sans SC (OFL) that ships with the deps
```

The source font must be **redistributable**, because the artifact is committed: macOS's own Chinese
fonts (Arial Unicode, Hiragino) are not. Use an OFL one — Source Han Sans / Noto Sans SC.

The same script also generates `main/badge_font_icon_16.c`: the two **transport icons** in the top
bar (USB / Bluetooth). Neither logo has a Unicode code point, so they can only come from an icon
font's private use area (U+F287 / U+F293 — FontAwesome's brand glyphs); the script therefore uses
the FontAwesome that ships inside the LVGL component (it arrives with the dependencies; its license
text is next to it under `font_license/FontAwesome5/`). Point `BADGE_ICON_FONT_SOURCE` somewhere
else to use a different icon font. It is a separate font rather than part of the Chinese one so that
a missing icon fails the LINK instead of turning into a hollow box on the screen.

## Layout

```
main/badge_proto.c    the protocol: parse and encode one line of JSON (pure logic, host-testable)
main/badge_json.c     the minimal JSON reader/writer used by it (no deps, no allocation)
main/badge_link.c     the serial line: framing, only lines carrying @@ count
main/badge_ble.c      the Bluetooth link: GATT peripheral, pairing, the four link states, dropping a stuck connection
main/badge_state.c    the state machine: what arrives, what a key press sends (pure, host-testable)
main/badge_power.c    screen on/off: how long idle blanks it, what lights it (pure, host-testable)
main/badge_sound.c    the alert sound: wake the codec, write the PCM, sleep again (its own task)
main/badge_ui.c       the four screens (its own UI, not the baseline demo menu shell)
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
- **Changing computers is not plug-and-play**: the bond stored here and the one on the computer are
  two separate copies, and **forgetting only one of them ends in connect-then-drop, which macOS will
  not recover from on its own**. To move to another computer, long-press OK twice on the status screen
  to forget it here, then Forget the device in System Settings → Bluetooth (CoreBluetooth has no
  unpair API, so that is the only way back).
- The serial port is exclusive: while this firmware runs, `idf.py monitor` cannot open the same port.
