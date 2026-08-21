# claude-desktop-buddy

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet that lives off permission approvals and
interaction with Claude. It sleeps when nothing's happening, wakes when
sessions start, gets visibly impatient when an approval prompt is waiting,
and lets you approve or deny right from the device.

## Hardware

The firmware targets the **[Xteink X4](https://github.com/Free-Ink/freeink-sdk)**
(ESP32-C3, SSD1677 800×480 e-paper, no PSRAM) via the FreeInk SDK, vendored
as the `freeink-sdk` git submodule (`git submodule update --init` before
building). A few things about this board shape the firmware:

- **Seven-button ADC ladder, not a handful of discrete GPIOs.**
  `BoardConfig::XTEINK_X4` declares `InputStyle::XteinkAdcLadder`: two ADC
  pins, each a resistor ladder multiplexing several buttons, decoding to
  seven semantic buttons — `BACK`, `CONFIRM`, `LEFT`, `RIGHT`, `UP`, `DOWN`,
  `POWER`. `InputManager::getState()` handles the decode; the firmware
  always routes through this semantic API, never raw `analogRead()`
  thresholds.
- **No IMU, no RTC.** `BoardConfig::XTEINK_X4`'s `sensors` field is
  `NO_SENSORS`, and it's absent from `FREEINK_CAP_RTC`'s device list. There's
  no shake gesture, no face-down detection, and no wall-clock time that
  survives a reboot.
- **Battery is ADC-only.** `batteryChargeStatus` is `PIN_UNASSIGNED` — no
  charge-current sensing, so charge current always reads `0`. A real
  `usbDetect` pin (GPIO20) reports external power, read directly since
  `BatteryMonitor`'s ADC backend doesn't surface it.
- **The SD card slot shares the display's SPI bus** (`BoardConfig::XTEINK_X4.sd`:
  `sclk`/`mosi` unassigned, `separateSpi=false` — only `miso` (GPIO7) and
  `cs` (GPIO12) are unique to the card). `FreeInkDisplay::begin()` only wires
  MISO into the bus when the panel driver needs it, which SSD1677 doesn't —
  so `setup()` claims the SPI bus once, MISO included, before
  `display.begin()` runs its own `SPI.begin()`. This is the same sequence
  [Free-Ink's own X4 app, inkdeck](https://github.com/Free-Ink/inkdeck/blob/main/src/main.cpp),
  uses for the same board.

## Building and flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -e xteink_x4
pio run -e xteink_x4 -t upload   # over USB, once built
```

### Flashing from the browser

`.github/workflows/firmware.yml` builds this target on every push to `main`
and publishes it to GitHub Pages as a one-click web installer
([ESP Web Tools](https://esphome.github.io/esp-web-tools/), Web Serial —
desktop Chrome or Edge only). The workflow merges the bootloader, partition
table, `boot_app0`, and app into one image at the offsets `pio run -t
upload` itself would use (`0x0`/`0x8000`/`0xe000`/`0x10000` for this board's
`default_16MB.csv` scheme — confirmed by capturing pio's own planned
`esptool` invocation, not assumed), so the browser only has to write one
file. The same job also uploads the merged image (and the unmerged parts)
as a downloadable build artifact on every push and PR, not just `main`.

**One-time setup this workflow can't do for you:** in the repo's **Settings
→ Pages**, set **Source** to **GitHub Actions**. Until that's set, the
`deploy-pages` job fails with a clear error rather than silently no-op'ing.

If you're starting from a previously-flashed device, wipe it first:

```bash
pio run -t erase && pio run -t upload
```

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the device:

- Make sure it's awake (any button press)
- Check that it hasn't been power-cycled off since its last pairing

## Controls

This firmware aims to match the earlier M5StickCPlus-based desktop-buddy's
button semantics as closely as this board's very different button set
allows: `CONFIRM` and `BACK` play the same roles that generation's two
physical buttons (`A`/`B`) did — tap `CONFIRM` to advance/approve, hold it
to open the menu; tap `BACK` to act/deny. The X4's extra buttons
(`LEFT`/`RIGHT`/`UP`/`DOWN`/`POWER`) cover things that generation had no
spare button for.

| Button                    | Normal                                                                    | On an approval prompt |
| -------------------------- | -------------------------------------------------------------------------- | ---------------------- |
| `CONFIRM` tap               | advance selection in a menu, else cycle screen (home → pet → info)         | **approve**             |
| `CONFIRM` hold (~600ms)     | open the menu; closes a nested settings/reset panel first if one is open   | —                        |
| `BACK` tap                  | act on the highlighted menu item, else next page/scroll transcript         | **deny**                |
| `LEFT` / `RIGHT`            | scroll the transcript panel                                                | —                        |
| `UP` tap                    | force a full e-paper refresh now (clears ghosting instead of waiting for the 5-minute timer) | —          |
| `UP` hold (~800ms)          | toggle nap — pauses the pet and accumulates nap time, same mechanic as the earlier generation's face-down nap | —          |
| `DOWN` hold (~800ms)        | trigger `dizzy` for 2s — the closest substitute for a shake gesture, since the X4 has no IMU | —          |
| `POWER` tap                 | toggle screen sleep — blanks the panel and halts rendering; any button press wakes it | —          |

A sleeping screen always just wakes on the first press — that press never
also fires the button's normal action, so waking the device can't
accidentally approve or deny a prompt you haven't seen yet. An incoming
approval prompt wakes a sleeping screen automatically so it's never missed.

**Input polling is async, not synchronous with the render loop.**
`display.displayWindow()`/`displayBuffer()` block the main loop for
anywhere from ~50ms (a small partial refresh) to ~2s (a full refresh) — a
CONFIRM/BACK press-and-release landing entirely inside one of those calls
would be silently dropped by a synchronous `input.update()`/`wasPressed()`
loop. `setup()` calls `input.beginAsync()` (spawns `InputManager`'s own
FreeRTOS polling task) and `handleInput()` drains edges via `popPress()`
instead — the same pattern Free-Ink's inkdeck uses for this exact reason.
`isPressed()` (a plain level read, used for the `DOWN` long-press timer
above) stays safe to call from the main loop even with async polling
active, since the background task's own `update()` call keeps it current.
Only `wasPressed()`/`update()` are unsafe to call yourself once the async
task owns the edge state.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, bit-inverted blink   |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | `DOWN` held                 | spiral eyes, wobbling        |
| `heart`     | approved in under 5s        | floating hearts             |

## Menu system

The status panel has three screens (`CONFIRM` tap cycles through them),
plus a menu/settings/reset overlay stack on top — the same structure the
earlier M5-based generation used, adapted to this board's buttons and
single fixed panel (there's no "peek" scaling: the pet sprite keeps
animating in its own region regardless of which screen is showing beside
it).

- **home** — session counts, the latest message, and a scrollable
  transcript (`LEFT`/`RIGHT`), or the approval prompt when one's pending.
- **pet** — two pages (`BACK` cycles them): mood/fed/energy pips, level,
  approvals/denials/nap time/tokens; then a how-to page.
- **info** — six pages (`BACK` cycles them): about, button reference,
  Claude session/link status, device/battery status, Bluetooth pairing
  info, and credits.

Holding `CONFIRM` opens the **menu** (`settings`, `turn off`, `help`,
`about`, `demo`, `close`) — `CONFIRM` taps advance the selection, `BACK`
acts on it, matching the earlier generation's `A`-advances/`B`-selects
pattern exactly. `settings` opens a submenu (`hud`, `flash`, `character`,
`reset`, `back`): `hud` toggles the home screen's session/transcript
content, `flash` toggles the bit-inverted blink during `attention` (the
substitute for that generation's LED), `character` cycles through the
compiled-in `bufo` plus every `.charpack` found on the SD card at boot
(persisted to NVS). `reset` opens a tap-twice-to-confirm submenu:
`delete char` reverts to the compiled-in `bufo` (without touching the SD
card's files — unlike the earlier generation, this firmware doesn't own
that storage), and `factory reset` clears NVS (stats, owner, pet name,
settings, character choice) and BLE bonds, then restarts.

**Turn off** puts the device into real ESP32 deep sleep
(`PowerManager::deepSleepUntilPowerButton()`), woken by the `POWER`
button — this board has no PMIC hard-off like the earlier generation's
AXP192, so deep sleep is the equivalent off state.

**Clock face.** With no RTC, there's nowhere to persist wall-clock time
across a reboot — but `platformTimeSync()` (`main.cpp`) keeps a software
clock in RAM: one synced moment (from the bridge's `time` heartbeat) plus
the `millis()` it arrived at, with "now" derived by adding elapsed time on
every read. When nothing's happening (home screen, no sessions, no
overlay open, on USB power, and the clock has synced at least once since
boot), the home screen shows this clock instead of session info, and the
pet's mood follows the same hour-of-day/weekday table the earlier
generation used (a pure function of the time, so it needed no changes).

**Nap** (`UP` held) is this board's substitute for that generation's
face-down-to-nap gesture, which needed an IMU this board doesn't have —
see "Known limitations". It pauses the pet's animation and accumulates nap
time toward the same stat the gesture did; holding `UP` again ends it.

### Sprite region and refresh strategy

The generated bufo assets top out at 192×200px (`bufo::MAX_ICON_W/H`). The
fixed animation region is `SPRITE_X=40, SPRITE_Y=40, SPRITE_W=240,
SPRITE_H=260` (`main.cpp`) — byte-aligned on both axes (240/8=30) so the
`attention`-state bit-inversion trick below never touches a partial byte at
the region's edge — with the rest of the 800×480 panel to the right as a
text/status panel (session counts, transcript, approval prompt, battery,
BLE link state), drawn with `FreeInkUI`'s dependency-free `DisplayTarget`
(bundled Noto Sans bitmap font, no external font library).

| State | What runs | Why |
|---|---|---|
| `sleep` | Draw once on entry, then nothing until the state changes | static frame, no refresh loop |
| `idle` | `displayWindow()` on the sprite region, ~1.5s cadence | slow partial refresh reads as a charming blink, not lag |
| `busy` | `displayWindow()`, ~350ms cadence | fast partial, but still windowed — never touches the panel outside the sprite region |
| `attention` | `displayWindow()` at ~400ms, alternating the icon with a bit-inverted copy of the same region (manual XOR over the framebuffer bytes) | no LED on this board — this reads as urgent, distinct from idle's slower blink |
| `celebrate` | Cycles the clip's frames, one `FULL_REFRESH` (whole panel) each | infrequent (every 50K tokens) — can afford the cost; also resets the DC-balance timer below |
| `dizzy` | `displayWindow()`, ~200ms cadence, one-shot ~2s | short-lived, triggered by a `DOWN` long-press |
| `heart` | `displayWindow()`, ~250ms cadence, one-shot ~2s | similar budget to celebrate but partial, not full — it fires on every fast approval, far more often |

**Mandatory full-refresh timer**, independent of the table above:
`FULL_REFRESH_INTERVAL_MS` (`main.cpp`, currently 5 minutes) forces a
whole-panel `FULL_REFRESH` on a fixed cadence regardless of what state the
pet is in, so a long `busy`/`idle` stretch running only partial refreshes
over the sprite region never lets the panel's DC balance degrade. It's one
named constant, checked once per `loop()` iteration, deliberately not tied
to any other timer in the file.

## Characters

The firmware ships one compiled-in character (`bufo`) as `freeink::Icon`
assets, and can optionally load a different character from the SD card at
boot.

### Asset pipeline

`tools/gif_to_icons.py` is the offline (host-side, not on-device) step: it
takes a character pack already normalized by `tools/prep_character.py`
(96px-wide, cropped to one consistent scale across every state) and emits
`freeink::Icon` C structs (`libs/assets/Icons/include/Icon.h`: 1-bpp,
MSB-first rows, `opticalCenterY` from the real ink pixels) directly into a
header, one clip/frame array per state, mirroring a manifest's own "single
filename or list of filenames" shape (a multi-clip state is an idle-carousel
rotation). Classification per pixel: within tolerance of the manifest's
flattened background color → transparent; else dark → ink; else (a light
non-background pixel, e.g. a highlight) → also transparent, since the
sprite region is cleared to white before every blit and a transparent pixel
just lets that white show through.

```bash
python3 tools/gif_to_icons.py characters/bufo \
  --scale 2 --out src/assets/icons_bufo.h --namespace bufo
```

`src/assets/icons_bufo.h` (checked in, ~650KB of packed icon data across
139 frames) is the generated output for the bundled `bufo` character —
proof the pipeline runs end-to-end, not just a described-but-unexercised
script. `--scale 2` (192px-wide icons) reads better than the source's
native 96px on the X4's 800×480 canvas, which is much larger by area than
a typical small color LCD.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide; height up to ~140px keeps the character at a consistent
scale across states. Crop tight to the character — transparent margins
waste screen and shrink the sprite. `tools/prep_character.py` handles the
resize: feed it source GIFs at any sizes and it produces a 96px-wide set
where the character is the same scale in every state.

See `characters/bufo/` for a working example.

### SD-backed character packs

The compiled-in `bufo` above is the guaranteed fallback; the X4 also has a
real SD card slot (shared with the display's SPI bus — see "Hardware"
above), and `--sd-out` on the same conversion pass writes a second output,
a binary `.charpack`, so a character doesn't have to live in flash to be
usable.

```bash
python3 tools/gif_to_icons.py characters/bufo \
  --scale 2 --out src/assets/icons_bufo.h --namespace bufo \
  --sd-out characters/bufo/bufo.charpack
```

**Install:** copy the `.charpack` file(s) to `/characters/` on the SD card
and insert it — no rebuild needed. This is deliberately the same shape as
[CrossPoint's SD-card-font install](https://github.com/crosspoint-reader/crosspoint-reader/blob/main/docs/sd-card-fonts.md#option-3-manual-sd-card-copy):
discovered and loaded at boot, not pushed over BLE (see below). `main.cpp`'s
`scanCharacters()` enumerates every `*.charpack` file under `/characters`
alongside the compiled-in `bufo`; Settings > `character` cycles between
them (see "Menu system"), and the choice persists to NVS across reboots. No
SD card, or none found — it falls back to the compiled-in `bufo`
automatically.

**Format** (`src/sd_character_pack.h`/`.cpp`, generated by the same
`write_charpack()` that produces the `.h` from identical per-frame data —
verified byte-for-byte identical during development, not just "should
match"): a fixed-size state→clip→frame index (12B header + 4B/state + 4B/
clip + 16B/frame — ~2.3KB total for bufo's 139 frames) followed by the raw
packed bits, referenced by absolute file offset. The device reads only that
small index into RAM at `open()` (bounded static arrays, not a heap
allocation) and seeks+reads one frame's bytes on demand per `getFrame()`
call, so an SD-backed character costs a few KB of RAM regardless of how
large the pack is on disk.

**Heap discipline.** This follows crosspoint-reader's own
[`.skills/heap-discipline`](https://github.com/crosspoint-reader/crosspoint-reader/blob/main/.skills/heap-discipline/SKILL.md)
guidance, written for the same hardware class (ESP32-C3, no PSRAM, one
full-frame-sized buffer as the tightest resource): the index arrays in
`SdCharacterPack` are fixed-size statics (`MAX_CLIPS=48`, `MAX_FRAMES=400`,
generous over bufo's 15/139), not `std::vector`, so there's no growth-
triggered fragmentation; the frame buffer (`SD_FRAME_BUF_CAP=16000` bytes,
sized for up to ~320×400px) is one static array allocated once, never
per-frame; and the SD file handle is never held open across calls (SdFat's
`FsFile` has a deleted copy/move assignment operator in this SDK's bundled
version, so `getFrame()` reopens the short on-SD path per call instead of
fighting that — at this render cadence, hundreds of ms to seconds between
frames, that reopen is well inside the frame budget).

**What this is not — yet.** BLE folder-push (`char_begin`/`file`/`chunk`/
`file_end`/`char_end` in `xfer.h`) isn't wired to write `.charpack` files to
SD; the manual-copy path above is the only install method right now — see
"Known limitations" below. There's also no runtime check that an SD pack's
frame dimensions fit the sprite region (`SPRITE_W`×`SPRITE_H`, 240×260) —
pick `--scale` to match, same constraint as the compiled-in asset.

## Resource budget

```
RAM:   [==        ]  15.5% (used 50748 bytes from 327680 bytes)
Flash: [==        ]  22.0% (used 1443631 bytes from 6553600 bytes)
```

From `pio run -e xteink_x4`'s size report (ESP32-C3, `default_16MB.csv`
partition scheme — the generic `esp32-c3-devkitm-1` board definition's
stock partition table targets a 4MB part and is too small for this image;
the X4 is a real 16MB part, so this env sets `board_build.partitions =
default_16MB.csv` explicitly). The RAM figure is static `.data`/`.bss`
only, from the linker's own accounting. It does **not** include the ~48KB
single-buffer e-paper framebuffer (`displayWidth / 8 * displayHeight` =
100 × 480), which `FreeInkDisplay::begin()` heap-allocates at runtime, nor
NimBLE's own heap usage — both come out of the ~278KB of DRAM left after
the static image, alongside whatever `ArduinoJson`'s `JsonDocument` needs
per incoming heartbeat. SD card support adds SdFat's own static footprint
plus `SdCharacterPack`'s fixed index arrays and its 16KB frame buffer (see
"SD-backed character packs") — all static, none of it heap, so it shows up
here rather than eating into that ~278KB at runtime.

## Known limitations

- No shake gesture — the X4 has no IMU. `DOWN` held substitutes for
  triggering `dizzy`; face-down nap detection is similarly replaced with
  `UP` held (see "Menu system" > "Nap") rather than dropped outright.
- No wall-clock time across reboots — the X4 has no RTC, so the clock face
  (see "Menu system") runs on a software clock that resets to unsynced on
  every boot until the bridge sends its next `time` heartbeat, and drifts
  with `millis()` between syncs rather than ticking off real hardware time.
- No buzzer, no LED, no brightness/frontlight control — the X4's
  `BoardConfig` profile declares `NO_AUDIO`, `NO_LEDS`, and `NO_FRONTLIGHT`.
  There's no substitute for these; the earlier generation's sound feedback
  and brightness setting are dropped rather than faked.
- The 18 hand-tuned ASCII-species characters from the earlier generation
  aren't ported — they were pixel-position-tuned text art for a 135×240
  color LCD at 5fps, and re-tuning all of them for 800×480 monochrome
  e-paper's much slower partial-refresh cadence would be a from-scratch
  effort per species. This board's answer to "multiple characters" is the
  compiled-in `bufo` plus SD `.charpack` cycling instead (see "SD-backed
  character packs" and "Menu system").
- Battery status's charge current (`mA`) always reads `0` — the X4 has no
  charge-status pin to read it from.
- Battery status's `usb` flag reads `BoardConfig::ACTIVE.usbDetect`
  (GPIO20) as active-high — this polarity is **an assumption**, not
  hardware-validated.
- BLE folder-push writes only the compiled-in flash path today, not SD —
  see "What this is not — yet" under "SD-backed character packs" above.
- The browser flasher (`site/index.html`) assumes Web Serial can reset the
  X4 into its ROM bootloader automatically. The X4 uses its native USB
  port for serial (`ARDUINO_USB_MODE=1`), not a separate USB-UART bridge
  chip — whether Arduino-ESP32's app-side USB CDC driver answers the reset
  request the same way a bridge chip would is **not hardware-verified**.
  The page documents the manual fallback (hold BOOT while plugging in) so
  flashing still works either way.

## Project layout

```
src/
  main.cpp                 — loop, state machine, rendering, input, setup
  ble_bridge.cpp/h          — Nordic UART service, line-buffered TX/RX
  data.h                    — wire protocol, JSON parse
  xfer.h                    — command handling (name/owner/status/unpair)
  stats.h                   — NVS-backed stats, pet name, owner name
  assets/icons_bufo.h        — generated Icon assets (see tools/gif_to_icons.py)
  sd_character_pack.h/.cpp   — reads an SD-card .charpack at runtime
characters/                — example GIF character packs (bufo/bufo.charpack is the
                             SD-card form of the same character — see "SD-backed
                             character packs")
tools/                     — generators and converters
freeink-sdk/                — git submodule (Free-Ink/freeink-sdk)
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.
