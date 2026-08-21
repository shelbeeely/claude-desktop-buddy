// claude-desktop-buddy — Xteink firmware (X4, X3, X4 Pro).
//
// Nordic UART Service BLE bridge (ble_bridge.cpp/h) + JSON wire protocol
// (data.h, xfer.h) + NVS-backed stats/owner/settings (stats.h) driving a
// 7-state desk pet rendered on FreeInk (e-paper display, InputManager for
// the button layout, BatteryMonitor). sd_character_pack.h/.cpp implements
// one installed character; main.cpp's characterList[] enumerates the
// compiled-in bufo plus every SD .charpack found at boot and lets the
// settings menu cycle between them — see README.md "SD-backed character
// packs" and "Menu system".
//
// This one file drives three boards, two of which (X4, X3) share one
// ESP32-C3 binary (env:xteink) picked at runtime by freeink::
// selectXteinkDevice() in setup(); the third (X4 Pro) is ESP32-S3 and
// builds separately (env:xteink_x4pro) but runs the same source. Where a
// board has real hardware the M5-era original also had (X3's IMU/RTC/
// battery gauge, X4 Pro's touch/frontlight/RTC/gauge), this file uses it via
// BoardConfig::hasImu()/hasRtc()/isX4Pro() and the Imu/Rtc/FrontlightManager
// libraries; where a board has none of it (X4: BoardConfig::XTEINK_X4 is
// NO_SENSORS/NO_AUDIO/NO_LEDS/NO_FRONTLIGHT, no RTC), the same button/
// software substitutes from the X4-only version remain. See README.md
// "Multi-board support" for the full capability matrix, and "Menu system"
// for what's substituted versus real per board.
//
// Screens and input dispatch run on the SDK's FreeInkApp framework
// (freeink-sdk/libs/ui/FreeInkUI/include/FreeInkApp.h) instead of a hand-
// rolled state machine — see docs/freeinkapp-migration.md for the screen
// model, the overlay-screen decision, and the input-routing split between
// real FreeInkApp action dispatch (BACK) and hand-rolled polling (CONFIRM/
// UP/DOWN, which need tap-vs-hold disambiguation InputSnapshot has no slot
// for on physical buttons).
//
// See README.md for the sprite-region size, the full-refresh timer
// interval, and the full input mapping.

#include <Arduino.h>
#include <new>
#include <time.h>
#include <esp_mac.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <InputManager.h>
#include <BatteryMonitor.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkApp.h>
#include <SDCardManager.h>
#include <PowerManager.h>
#include <XteinkDetect.h>
#include <Imu.h>
#include <Rtc.h>
#include <FrontlightManager.h>
#include <SPI.h>

#include "ble_bridge.h"
#include "data.h"
#include "assets/icons_bufo.h"
#include "sd_character_pack.h"

using freeink::ui::ActionEvent;
using freeink::ui::Color;
using freeink::ui::DisplayTarget;
using freeink::ui::InputBack;
using freeink::ui::InputSnapshot;
using freeink::ui::ListItem;
using freeink::ui::ListProps;
using freeink::ui::NO_ACTION;
using freeink::ui::Orientation;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::RefreshHint;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

// ---------------------------------------------------------------------------
// Platform hooks required by data.h / xfer.h (see their declarations).
// ---------------------------------------------------------------------------

// Wall-clock time. X3 and X4 Pro have a real RTC (BoardConfig::hasRtc());
// X4 doesn't (FREEINK_CAP_RTC's device list excludes it). Where there's no
// RTC, we fall back to a software clock: one synced moment (from the
// bridge's "time" heartbeat) plus the millis() it arrived at, deriving "now"
// by adding elapsed time on every read — resets on reboot, accurate only
// while powered. mktime()/gmtime_r() are used purely as a matched pair to
// add seconds to a struct tm — since both go through the same C library TZ
// state, the actual TZ setting (unconfigured -> UTC on this core) never
// matters, only that encode/decode are inverses.
static freeink::Rtc rtc;
static bool     rtcAvailable = false;
static time_t   swClockBase = 0;
static uint32_t swClockSyncMs = 0;
static bool     swClockValid = false;

static void rtcBegin() {
  if (BoardConfig::hasRtc()) rtcAvailable = rtc.begin();
}

void platformTimeSync(const struct tm& localTime) {
  struct tm lt = localTime;
  swClockBase = mktime(&lt);
  swClockSyncMs = millis();
  swClockValid = true;
  if (rtcAvailable) {
    // Write through to the real RTC so time survives a reboot/deep sleep —
    // the whole point of having one. A write failure just means the next
    // read falls back to the software clock below (rtc.now() returns false
    // on I2C error or an unset/stopped oscillator).
    freeink::Rtc::DateTime dt;
    dt.year = (uint16_t)(lt.tm_year + 1900);
    dt.month = (uint8_t)(lt.tm_mon + 1);
    dt.day = (uint8_t)lt.tm_mday;
    dt.hour = (uint8_t)lt.tm_hour;
    dt.minute = (uint8_t)lt.tm_min;
    dt.second = (uint8_t)lt.tm_sec;
    dt.weekday = (uint8_t)lt.tm_wday;
    rtc.set(dt);
  }
}

static bool getSoftClock(struct tm& out, uint32_t now) {
  if (rtcAvailable) {
    freeink::Rtc::DateTime dt;
    if (rtc.now(dt)) {
      out.tm_year = dt.year - 1900;
      out.tm_mon = dt.month - 1;
      out.tm_mday = dt.day;
      out.tm_hour = dt.hour;
      out.tm_min = dt.minute;
      out.tm_sec = dt.second;
      out.tm_wday = dt.weekday;
      return true;
    }
    // Oscillator never set / stopped, or an I2C error — fall through to the
    // software clock rather than showing nothing.
  }
  if (!swClockValid) return false;
  time_t t = swClockBase + (time_t)((now - swClockSyncMs) / 1000);
  gmtime_r(&t, &out);
  return true;
}

// Shake detection. X3 has a real QMI8658 IMU (BoardConfig::hasImu()); X4 and
// X4 Pro don't (X4: NO_SENSORS; X4 Pro: also no IMU despite its touch/
// frontlight/RTC/gauge — confirmed via BoardConfig, not assumed), so those
// boards keep the DOWN-hold substitute in handleInput() regardless. Where a
// real IMU is present this runs alongside DOWN-hold, not instead of it —
// every button keeps doing the same thing on every board; the sensor is
// just an additional, more literal way to trigger the same dizzy state.
static freeink::Imu imu;
static bool  imuAvailable = false;
static float shakeBaseline = 1.0f;

static void imuBegin() {
  if (BoardConfig::hasImu()) imuAvailable = imu.begin();
}

static bool checkShake() {
  if (!imuAvailable) return false;
  freeink::Imu::Sample s;
  if (!imu.read(s)) return false;
  float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
  float delta = fabsf(mag - shakeBaseline);
  shakeBaseline = shakeBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

// Frontlight. Only X4 Pro has one (BoardConfig::hasFrontlight() equivalent
// is FrontlightManager::present(), checked at runtime since it also covers
// the warm/cool-pair vs single-channel split) — inert everywhere else, so
// it's always safe to construct and call begin() on unconditionally.
static FrontlightManager frontlight;
// 0..4 -> 20..100%, same 5-step scale as the earlier M5-era generation's
// brightness setting (there: M5.Axp.ScreenBreath). Session-only, matching
// that generation's choice not to persist it either.
static uint8_t brightLevel = 4;
static void cycleBrightness() {
  brightLevel = (brightLevel + 1) % 5;
  frontlight.setBrightness((uint8_t)(20 + brightLevel * 20));
}

BatteryMonitor batteryMonitor;
PlatformBatteryStatus platformBatteryStatus() {
  BatteryMonitor::Status s = batteryMonitor.readStatus();
  PlatformBatteryStatus out{0, 0, 0, false};
  if (s.percentageKnown) out.pct = s.percentage;
  if (s.millivoltsKnown) out.mV = s.millivolts;
  // X3's BQ27220 and X4 Pro's CW2017 gauges report real percentage/voltage
  // over I2C (BatteryMonitor picks gauge vs. ADC at runtime from
  // BoardConfig::ACTIVE.batteryGauge — no board-specific code needed here).
  // Charging isn't observable from either gauge though (no charger IC on
  // their bus — confirmed for X4 Pro's CW2017 in freeink-sdk/docs/
  // xteink-x4pro-support.md "RTC / USB / battery"; X3 untested, assumed the
  // same), and Status doesn't expose a magnitude even when chargingKnown is
  // true, so -1 here just means "charging, current unknown" per
  // REFERENCE.md's "negative = charging" contract.
  if (s.chargingKnown) out.mA = s.charging ? -1 : 0;
  if (s.externalPowerKnown) {
    out.usb = s.externalPower;
  } else if (BoardConfig::ACTIVE.usbDetect != BoardConfig::PIN_UNASSIGNED) {
    // X4 has no charge-status pin (BoardConfig::XTEINK_X4.batteryChargeStatus
    // == PIN_UNASSIGNED) and its ADC battery backend never sets
    // externalPowerKnown, so this reads usbDetect (GPIO20) directly.
    // Polarity (active-high) is an assumption, NOT hardware-validated —
    // flagged in README "Known limitations".
    out.usb = digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  return out;
}

// ---------------------------------------------------------------------------
// State machine — identical derivation to the earlier desktop-buddy
// generation's derive(), just renamed/re-typed here.
// ---------------------------------------------------------------------------
enum PersonaState : uint8_t { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART, P_COUNT };
static const char* const kStateNames[P_COUNT] = {
    "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"};

static PersonaState derive(const TamaState& s) {
  if (!s.connected) return P_SLEEP;
  if (s.sessionsWaiting > 0) return P_ATTENTION;
  if (s.recentlyCompleted) return P_CELEBRATE;
  if (s.sessionsRunning >= 3) return P_BUSY;
  return P_IDLE;
}

TamaState     tama;
PersonaState  baseState = P_SLEEP;
PersonaState  activeState = P_SLEEP;
uint32_t      oneShotUntil = 0;
static void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// ---------------------------------------------------------------------------
// Display + sprite region.
//
// Panel: 800x480 on X4/X4 Pro, 792x528 on X3 (BoardConfig::ACTIVE picks the
// real dimensions at runtime — see "Multi-board support" below). bufo's Icon
// assets top out at bufo::MAX_ICON_W x MAX_ICON_H = 192x200 (96px source x 2
// scale — see tools/gif_to_icons.py and README "Asset pipeline"). SPRITE
// region is sized with margin around that and kept byte-aligned (x and w
// multiples of 8) so the manual framebuffer inversion used for the
// `attention` cue below never touches a partial byte at the edges — that
// margin comfortably fits either panel, so it stays a fixed size across
// boards; only the panel-relative layout below (PANEL_W, full-screen
// overlays) reads the runtime panel size.
// ---------------------------------------------------------------------------
static constexpr int16_t SPRITE_X = 40;
static constexpr int16_t SPRITE_Y = 40;
static constexpr int16_t SPRITE_W = 240;
static constexpr int16_t SPRITE_H = 260;
static_assert(SPRITE_W >= bufo::MAX_ICON_W && SPRITE_H >= bufo::MAX_ICON_H,
              "sprite region must fit the largest generated icon");

// Text/status panel occupies the rest of the panel to the right of (and
// below) the sprite region. PANEL_TOTAL_W/H and PANEL_W are set once in
// setup() from display.getDisplayWidth()/Height() once the real board is
// known (see freeink::selectXteinkDevice() there) — X3's panel is a
// different size than X4/X4 Pro's, so these can't be compile-time constants.
static constexpr int16_t PANEL_X = SPRITE_X + SPRITE_W + 24;
static int16_t PANEL_TOTAL_W = 800;
static int16_t PANEL_TOTAL_H = 480;
static int16_t PANEL_W = PANEL_TOTAL_W - PANEL_X - 16;

EInkDisplay display(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
                    BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
                    BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
InputManager input;

// DisplayTarget wraps the display's framebuffer pointer, which display.begin()
// only allocates at runtime (it's null before that) — constructing this as an
// eagerly-initialized global would capture a null pointer, since global
// constructors run before setup(). Built lazily in setup() after begin()
// instead; ui() below is the only accessor. This is also the exact DrawTarget
// FreeInkApp renders into (gApp below is constructed with `ui()`) — screen
// functions and these free-standing draw helpers share one target, so a
// helper written against `ui()` needs no change to be called from a
// FreeInkApp screen function.
alignas(DisplayTarget) static unsigned char uiStorage[sizeof(DisplayTarget)];
static DisplayTarget* uiPtr = nullptr;
static DisplayTarget& ui() { return *uiPtr; }

// --- Mandatory DC-balance timer ---------------------------------------------
// Independent of activity: a long busy/idle stretch runs only fast refreshes
// over the sprite region, which never DC-balances the panel. Force a full
// refresh of the whole screen on this fixed cadence regardless of what state
// the pet is in. Tune this one constant, nothing else.
static constexpr uint32_t FULL_REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes
static uint32_t lastFullRefreshMs = 0;

static void invertSpriteRegionBytes() {
  uint8_t* fb = display.getFrameBuffer();
  uint16_t wb = display.getDisplayWidthBytes();
  for (int16_t y = SPRITE_Y; y < SPRITE_Y + SPRITE_H; y++) {
    for (int16_t xb = SPRITE_X / 8; xb < (SPRITE_X + SPRITE_W) / 8; xb++) {
      fb[(uint32_t)y * wb + xb] ^= 0xFF;
    }
  }
}

static void clearSpriteRegion() {
  ui().fill(Rect{SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H}, Paint::solid(Color::White));
}

static void drawIconCentered(const freeink::Icon& icon) {
  int16_t x = SPRITE_X + (SPRITE_W - (int16_t)icon.w) / 2;
  int16_t y = SPRITE_Y + (SPRITE_H - (int16_t)icon.h) / 2;
  display.drawImageTransparent(icon.bits, x, y, icon.w, icon.h);
}

// Pick the current animation frame for `state`, advancing the idle carousel
// (rotating array of clips — same "activity carousel" idea as the original
// README) and per-clip frame index by wall-clock time using each frame's own
// duration from the GIF. Returns nullptr if the state has no clips (never
// happens for the 7 required states, guarded by the manifest check in
// tools/gif_to_icons.py).
static const freeink::Icon* currentFrame(PersonaState state, uint32_t now, uint32_t stateEnteredMs) {
  const bufo::IconState& is = bufo::bufo_states[state];
  if (is.clipCount == 0) return nullptr;
  uint32_t elapsed = now - stateEnteredMs;
  uint32_t clipMs = 0;
  for (uint8_t f = 0; f < is.clips[0].frameCount; f++) clipMs += is.clips[0].frames[f].durationMs;
  if (clipMs == 0) clipMs = 1;
  uint8_t clipIdx = (uint8_t)((elapsed / clipMs) % is.clipCount);
  uint32_t intoClip = elapsed % clipMs;
  const bufo::IconClip& clip = is.clips[clipIdx];
  uint32_t acc = 0;
  for (uint8_t f = 0; f < clip.frameCount; f++) {
    acc += clip.frames[f].durationMs;
    if (intoClip < acc) return clip.frames[f].icon;
  }
  return clip.frames[clip.frameCount - 1].icon;
}

// --- Installed characters: bufo (compiled-in) + every SD .charpack --------
// Settings > "character" cycles this list (matches the earlier generation's
// "ascii pet" cycling setting, adapted to this board's character system —
// see the AskUserQuestion decision recorded in README "Menu system").
struct CharacterEntry {
  bool isSd;
  char path[80];
};
static constexpr uint8_t MAX_CHARACTERS = 9;  // bufo + up to 8 SD packs
static CharacterEntry characterList[MAX_CHARACTERS];
static uint8_t characterCount = 0;
static uint8_t characterIdx = 0;

static SdCharacterPack sdPack;
static bool sdCharacterActive = false;
// Covers up to ~320x400px @ 1bpp (⌈320/8⌉ * 400 = 16000) with margin over
// bufo's 192x200. A pack whose frames exceed this is the operator's to
// avoid by choosing --scale appropriately when converting it — there is no
// runtime dimension check here, same as the compiled-in asset (see README).
static constexpr size_t SD_FRAME_BUF_CAP = 16000;
static uint8_t sdFrameBuf[SD_FRAME_BUF_CAP];

static void scanCharacters() {
  characterCount = 0;
  characterList[characterCount].isSd = false;
  characterList[characterCount].path[0] = 0;
  characterCount++;  // bufo is always index 0
  if (SdMan.ready()) {
    for (const String& name : SdMan.listFiles("/characters")) {
      if (characterCount >= MAX_CHARACTERS) break;
      if (!name.endsWith(".charpack")) continue;
      CharacterEntry& e = characterList[characterCount];
      e.isSd = true;
      snprintf(e.path, sizeof(e.path), "/characters/%s", name.c_str());
      characterCount++;
    }
  }
}

// Opens characterList[idx] (falling back to bufo — index 0 — if it's out of
// range or the SD file won't open) and updates sdCharacterActive.
static void applyCharacter(uint8_t idx) {
  if (idx >= characterCount) idx = 0;
  characterIdx = idx;
  sdCharacterActive = false;
  if (characterList[idx].isSd && sdPack.open(characterList[idx].path)) {
    sdCharacterActive = true;
    Serial.printf("[char] loaded %s\n", characterList[idx].path);
  } else if (characterList[idx].isSd) {
    Serial.printf("[char] failed to open %s, falling back to bufo\n", characterList[idx].path);
    characterIdx = 0;
  } else {
    Serial.println("[char] bufo (compiled-in)");
  }
}

static void nextCharacter() {
  applyCharacter((characterIdx + 1) % characterCount);
  characterIdxSave(characterIdx);
}

// Resolves to an Icon for (state, now-stateEnteredMs), preferring the SD
// pack when one is active and falling back to the compiled-in bufo tables
// otherwise (or on an SD read failure). Returns false only if neither
// source has anything for `state`, which shouldn't happen for the 7
// required states.
static bool getCurrentIcon(PersonaState state, uint32_t now, uint32_t stateEnteredMs, freeink::Icon& out) {
  if (sdCharacterActive) {
    uint32_t elapsed = now - stateEnteredMs;
    if (sdPack.getFrame((uint8_t)state, elapsed, out, sdFrameBuf, sizeof(sdFrameBuf))) return true;
  }
  const freeink::Icon* p = currentFrame(state, now, stateEnteredMs);
  if (!p) return false;
  out = *p;
  return true;
}

// --- Refresh strategy --------------------------------------------------------
// See README "Refresh strategy" for the rationale behind each state's
// cadence/mode. Nothing here is uniform on purpose.
//
// Pre-migration this drove display.displayWindow()/displayBuffer() calls
// directly. FreeInkApp owns the actual panel push now (its RefreshHint,
// read via app.lastRenderRefreshHint() and pushed once per loop tick with
// freeink::ui::present() — see loop()), so these functions instead redraw
// into the shared framebuffer on the same cadence as before and report back
// what refresh strength that redraw deserves; the caller (screenMain, see
// below) forwards it to gApp->invalidate().
static uint32_t stateEnteredMs = 0;
static uint32_t lastSpriteDrawMs = 0;
static bool     sleepFrameDrawn = false;
static bool     attentionFlashOn = false;

// Nap (see "Nap" below) pauses sprite animation, same as it did pre-
// migration; the menu/settings/reset/passkey "pause animation underneath"
// behavior is now automatic — those are separate FreeInkApp screens, so
// screenMain (and this function) simply doesn't run while one is active.
static bool napping = false;
static uint32_t napStartMs = 0;
static bool napFrameDrawn = false;

// Redraws the sprite region if this state's cadence says it's due, and
// reports the refresh strength that redraw deserves (RefreshHint::None if
// nothing was redrawn this tick).
static RefreshHint renderSprite(uint32_t now) {
  switch (activeState) {
    case P_SLEEP:
      if (sleepFrameDrawn) return RefreshHint::None;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_SLEEP, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      sleepFrameDrawn = true;
      return RefreshHint::Fast;

    case P_IDLE:
      if (now - lastSpriteDrawMs < 1500) return RefreshHint::None;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_IDLE, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      lastSpriteDrawMs = now;
      return RefreshHint::Fast;

    case P_BUSY:
      if (now - lastSpriteDrawMs < 350) return RefreshHint::None;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_BUSY, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      lastSpriteDrawMs = now;
      return RefreshHint::Fast;

    case P_ATTENTION: {
      // No LED on this board. Substitute cue (gated by settings().flash,
      // the renamed "led" toggle): alternate the sprite region between the
      // normal icon and a bit-inverted version of it.
      if (now - lastSpriteDrawMs < 400) return RefreshHint::None;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_ATTENTION, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      if (settings().flash) {
        attentionFlashOn = !attentionFlashOn;
        if (attentionFlashOn) invertSpriteRegionBytes();
      }
      lastSpriteDrawMs = now;
      return RefreshHint::Fast;
    }

    case P_CELEBRATE: {
      if (now - lastSpriteDrawMs < 220) return RefreshHint::None;
      clearSpriteRegion();
      freeink::Icon icon;
      if (getCurrentIcon(P_CELEBRATE, now, stateEnteredMs, icon)) drawIconCentered(icon);
      lastSpriteDrawMs = now;
      return RefreshHint::Full;
    }

    case P_DIZZY:
      if (now - lastSpriteDrawMs < 200) return RefreshHint::None;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_DIZZY, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      lastSpriteDrawMs = now;
      return RefreshHint::Fast;

    case P_HEART:
      if (now - lastSpriteDrawMs < 250) return RefreshHint::None;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_HEART, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      lastSpriteDrawMs = now;
      return RefreshHint::Fast;

    default:
      return RefreshHint::None;
  }
}

// One static frame (the sleep icon + a "napping" tag) while napping, drawn
// once on entry — mirrors P_SLEEP's draw-once pattern above.
static RefreshHint renderNapFrame() {
  if (napFrameDrawn) return RefreshHint::None;
  clearSpriteRegion();
  freeink::Icon icon;
  if (getCurrentIcon(P_SLEEP, millis(), stateEnteredMs, icon)) drawIconCentered(icon);
  ui().text(Rect{SPRITE_X, SPRITE_Y + SPRITE_H - 24, SPRITE_W, 20}, "napping...",
            TextStyle{0, TextAlign::Center, Color::DarkGray, 1, false, false});
  napFrameDrawn = true;
  return RefreshHint::Fast;
}

// ---------------------------------------------------------------------------
// Display modes, menu, settings, reset — mirrors the earlier desktop-buddy
// generation's DISP_NORMAL/PET/INFO + menuOpen/settingsOpen/resetOpen
// structure, now expressed as FreeInkApp screens (see docs/
// freeinkapp-migration.md "Screen model"). Unlike that build, PET/INFO
// don't shrink the character sprite to a "peek" size — the sprite region
// keeps showing the pet regardless of mode, since the X4's panel has room
// for both at once.
// ---------------------------------------------------------------------------
enum DisplayMode : uint8_t { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
static DisplayMode displayMode = DISP_NORMAL;

static constexpr uint8_t INFO_PAGES = 6;
static constexpr uint8_t INFO_PG_BUTTONS = 1;
static constexpr uint8_t INFO_PG_CREDITS = 5;
static uint8_t infoPage = 0;

static constexpr uint8_t PET_PAGES = 2;
static uint8_t petPage = 0;

static uint8_t menuSel = 0;
static const char* const menuItems[] = {"settings", "turn off", "help", "about", "demo", "close"};
static constexpr uint8_t MENU_N = 6;

static uint8_t settingsSel = 0;
// "brightness" only makes sense on a board with a frontlight (X4 Pro) — the
// item list and count are picked once in setup() from frontlight.present(),
// per README "Menu system".
static const char* const SETTINGS_ITEMS_BASE[] = {"hud", "flash", "character", "reset", "back"};
static const char* const SETTINGS_ITEMS_LIGHT[] = {"hud", "flash", "character", "brightness", "reset", "back"};
static const char* const* settingsItems = SETTINGS_ITEMS_BASE;
static uint8_t SETTINGS_N = 5;
static bool    hasBrightnessItem = false;

static uint8_t  resetSel = 0;
static const char* const resetItems[] = {"delete char", "factory reset", "back"};
static constexpr uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

// Which FreeInkApp screen is active. FreeInkApp itself has no screen-stack
// concept (setScreen() just swaps a function pointer — see FreeInkApp.h),
// so this is purely this file's own bookkeeping for the hand-rolled parts
// of input dispatch (CONFIRM tap/hold — see dispatchConfirmTap/Hold below)
// that need to know "what's on screen right now" without re-deriving it
// from FreeInkApp state every time.
enum ScreenId : uint8_t { SCR_MAIN, SCR_MENU, SCR_SETTINGS, SCR_RESET, SCR_SLEEP };
static ScreenId currentScreen = SCR_MAIN;

// --- Status/text panel -------------------------------------------------------
static bool responseSent = false;
static char lastPromptId[40] = "";
static uint32_t promptArrivedMs = 0;
static char btName[16] = "Claude";

static bool screenAwake = true;

static constexpr uint8_t TRANSCRIPT_VISIBLE = 6;
static uint8_t transcriptOffset = 0;
static uint16_t lastLineGen = 0;

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// Only ever evaluated from within screenMain (see below), so — unlike the
// pre-migration version — there's no need to check "is an overlay open":
// while the menu/settings/reset screen is active, screenMain doesn't run at
// all.
static bool clockActive() {
  bool inPrompt = tama.promptId[0] && !responseSent;
  return displayMode == DISP_NORMAL && !inPrompt
      && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
      && (rtcAvailable || swClockValid) && platformBatteryStatus().usb;
}

// Hour-conditioned mood table shown while the clock face is active — direct
// port of the earlier generation's clocking activeState logic (pure
// day-part/weekday rules, no hardware dependency, so it needed no changes).
static void applyClockMood(const struct tm& lt, uint32_t now) {
  uint8_t dow = lt.tm_wday;
  bool weekend = (dow == 0 || dow == 6);
  bool friday = (dow == 5);
  uint8_t h = (uint8_t)lt.tm_hour;
  if (h >= 1 && h < 7)             activeState = P_SLEEP;
  else if (weekend)                activeState = (now / 8000 % 6 == 0) ? P_HEART : P_SLEEP;
  else if (h < 9)                  activeState = (now / 6000 % 4 == 0) ? P_IDLE : P_SLEEP;
  else if (h == 12)                activeState = (now / 5000 % 3 == 0) ? P_HEART : P_IDLE;
  else if (friday && h >= 15)      activeState = (now / 4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
  else if (h >= 22 || h == 0)      activeState = (now / 7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
  else                             activeState = (now / 10000 % 5 == 0) ? P_SLEEP : P_IDLE;
}

static void drawClock(uint32_t now, int16_t y) {
  struct tm lt;
  if (!getSoftClock(lt, now)) return;
  static const char* const MON[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", lt.tm_hour, lt.tm_min);
  char dl[24]; snprintf(dl, sizeof(dl), "%s  %s %d", DOW[lt.tm_wday % 7], MON[lt.tm_mon % 12], lt.tm_mday);
  // No second bitmap font is bundled to draw big digits (DisplayTarget's
  // TextStyle has no size/scale field, only a font slot — see
  // FreeInkUIDisplayTarget.h), so the clock reads at the same size as the
  // rest of the panel; bold + its own row keeps it the visual anchor.
  ui().text(Rect{PANEL_X, y, PANEL_W, 28}, hm, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
  y += 32;
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, dl, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
}

static void drawPips(int16_t x, int16_t y, uint8_t total, uint8_t filled) {
  for (uint8_t i = 0; i < total; i++) {
    Rect r{(int16_t)(x + i * 16), y, 10, 10};
    if (i < filled) ui().fill(r, Paint::solid(Color::Black), 5);
    else ui().stroke(r, Paint::solid(Color::DarkGray), 1, 5);
  }
}

static void drawPetPage(uint32_t now, int16_t y) {
  char b[48];
  if (ownerName()[0]) snprintf(b, sizeof(b), "%s's %s", ownerName(), petName());
  else snprintf(b, sizeof(b), "%s", petName());
  ui().text(Rect{PANEL_X, y, (int16_t)(PANEL_W - 60), 24}, b, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
  snprintf(b, sizeof(b), "%u/%u", petPage + 1, PET_PAGES);
  ui().text(Rect{(int16_t)(PANEL_X + PANEL_W - 60), y, 60, 24}, b,
            TextStyle{0, TextAlign::Right, Color::DarkGray, 1, false, false});
  y += 32;

  if (petPage == 0) {
    ui().text(Rect{PANEL_X, y, 80, 20}, "mood", TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    drawPips(PANEL_X + 70, y, 4, statsMoodTier());
    y += 24;
    ui().text(Rect{PANEL_X, y, 80, 20}, "fed", TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    drawPips(PANEL_X + 70, y, 10, statsFedProgress());
    y += 24;
    ui().text(Rect{PANEL_X, y, 80, 20}, "energy", TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    drawPips(PANEL_X + 70, y, 5, statsEnergyTier());
    y += 32;

    snprintf(b, sizeof(b), "Lv %u", stats().level);
    ui().text(Rect{PANEL_X, y, 100, 24}, b, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
    y += 30;

    snprintf(b, sizeof(b), "approved %u", stats().approvals);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, b, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    y += 22;
    snprintf(b, sizeof(b), "denied   %u", stats().denials);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, b, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    y += 22;
    uint32_t nap = stats().napSeconds;
    snprintf(b, sizeof(b), "napped   %luh%02lum", nap / 3600, (nap / 60) % 60);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, b, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    y += 22;
    auto tokFmt = [&](const char* label, uint32_t v) {
      char t[32];
      if (v >= 1000000) snprintf(t, sizeof(t), "%s%lu.%luM", label, v / 1000000, (v / 100000) % 10);
      else if (v >= 1000) snprintf(t, sizeof(t), "%s%lu.%luK", label, v / 1000, (v / 100) % 10);
      else snprintf(t, sizeof(t), "%s%lu", label, v);
      ui().text(Rect{PANEL_X, y, PANEL_W, 20}, t, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
      y += 22;
    };
    tokFmt("tokens   ", stats().tokens);
    tokFmt("today    ", tama.tokensToday);
  } else {
    static const char* const lines[] = {
      "MOOD", " approve fast = up", " deny lots = down", "",
      "FED", " 50K tokens = level up", "",
      "ENERGY", " hold UP to nap, refills", "",
      "CONFIRM: screens  BACK: page", "hold CONFIRM: menu",
    };
    for (const char* l : lines) {
      if (l[0]) {
        Color c = (l[0] != ' ') ? Color::Black : Color::DarkGray;
        ui().text(Rect{PANEL_X, y, PANEL_W, 18}, l, TextStyle{0, TextAlign::Left, c, 1, false, false});
      }
      y += 18;
    }
  }
}

static void drawInfoPage(uint32_t now, int16_t y) {
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "info  %u/%u", infoPage + 1, INFO_PAGES);
  ui().text(Rect{PANEL_X, y, PANEL_W, 24}, hdr, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
  y += 32;

  auto ln = [&](Color c, const char* s) {
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, s, TextStyle{0, TextAlign::Left, c, 1, false, false});
    y += 20;
  };
  char b[64];

  if (infoPage == 0) {
    ln(Color::DarkGray, "I watch your Claude desktop");
    ln(Color::DarkGray, "sessions. I sleep when");
    ln(Color::DarkGray, "nothing's happening, wake");
    ln(Color::DarkGray, "when you start working, get");
    ln(Color::DarkGray, "impatient when approvals");
    ln(Color::DarkGray, "pile up.");
    y += 8;
    ln(Color::Black, "CONFIRM on a prompt");
    ln(Color::Black, "approves it from here.");
  } else if (infoPage == 1) {
    ln(Color::Black, "CONFIRM");
    ln(Color::DarkGray, "  tap: approve / next screen");
    ln(Color::DarkGray, "  hold: menu");
    y += 4;
    ln(Color::Black, "BACK");
    ln(Color::DarkGray, "  tap: deny / next page");
    y += 4;
    ln(Color::Black, "LEFT / RIGHT");
    ln(Color::DarkGray, "  scroll transcript");
    y += 4;
    ln(Color::Black, "UP");
    ln(Color::DarkGray, "  tap: refresh screen");
    ln(Color::DarkGray, "  hold: nap");
    y += 4;
    ln(Color::Black, "DOWN (hold)");
    ln(Color::DarkGray, "  dizzy");
    y += 4;
    ln(Color::Black, "POWER");
    ln(Color::DarkGray, "  tap: screen sleep");
  } else if (infoPage == 2) {
    snprintf(b, sizeof(b), "sessions  %u", tama.sessionsTotal); ln(Color::DarkGray, b);
    snprintf(b, sizeof(b), "running   %u", tama.sessionsRunning); ln(Color::DarkGray, b);
    snprintf(b, sizeof(b), "waiting   %u", tama.sessionsWaiting); ln(Color::DarkGray, b);
    y += 8;
    ln(Color::Black, "LINK");
    snprintf(b, sizeof(b), "via       %s", dataScenarioName()); ln(Color::DarkGray, b);
    snprintf(b, sizeof(b), "ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN"); ln(Color::DarkGray, b);
    uint32_t age = (now - tama.lastUpdated) / 1000;
    snprintf(b, sizeof(b), "last msg  %lus", (unsigned long)age); ln(Color::DarkGray, b);
    snprintf(b, sizeof(b), "state     %s", kStateNames[activeState]); ln(Color::DarkGray, b);
  } else if (infoPage == 3) {
    PlatformBatteryStatus bat = platformBatteryStatus();
    snprintf(b, sizeof(b), "%d%%  %s", bat.pct, bat.usb ? "usb" : "battery"); ln(Color::Black, b);
    snprintf(b, sizeof(b), "%d.%02dV", bat.mV / 1000, (bat.mV % 1000) / 10); ln(Color::DarkGray, b);
    y += 8;
    ln(Color::Black, "SYSTEM");
    if (ownerName()[0]) { snprintf(b, sizeof(b), "owner    %s", ownerName()); ln(Color::DarkGray, b); }
    uint32_t up = millis() / 1000;
    snprintf(b, sizeof(b), "uptime   %luh %02lum", up / 3600, (up / 60) % 60); ln(Color::DarkGray, b);
    snprintf(b, sizeof(b), "heap     %uKB", ESP.getFreeHeap() / 1024); ln(Color::DarkGray, b);
    snprintf(b, sizeof(b), "character %u/%u", characterIdx + 1, characterCount); ln(Color::DarkGray, b);
  } else if (infoPage == 4) {
    bool linked = bleConnected();
    ln(linked ? Color::Black : Color::DarkGray, linked ? (bleSecure() ? "linked" : "OPEN") : "advertising");
    snprintf(b, sizeof(b), "%s", btName); ln(Color::Black, b);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ln(Color::DarkGray, b);
    y += 8;
    if (linked) {
      uint32_t age = (now - tama.lastUpdated) / 1000;
      snprintf(b, sizeof(b), "last msg  %lus", (unsigned long)age); ln(Color::DarkGray, b);
    } else {
      ln(Color::Black, "TO PAIR");
      ln(Color::DarkGray, "open Claude desktop >");
      ln(Color::DarkGray, "Developer > Hardware Buddy");
    }
  } else {
    ln(Color::DarkGray, "made by");
    ln(Color::Black, "Felix Rieseberg");
    y += 8;
    ln(Color::DarkGray, "source");
    ln(Color::Black, "github.com/anthropics");
    ln(Color::Black, "/claude-desktop-buddy");
    y += 8;
    ln(Color::DarkGray, "hardware");
    ln(Color::Black, BoardConfig::ACTIVE.name);
    ln(Color::Black, BoardConfig::isX4Pro() ? "ESP32-S3" : "ESP32-C3");
  }
}

static void drawPasskey() {
  ui().fill(Rect{0, 0, PANEL_TOTAL_W, PANEL_TOTAL_H}, Paint::solid(Color::White));
  int16_t midY = (int16_t)(PANEL_TOTAL_H / 2);
  ui().text(Rect{0, (int16_t)(midY - 80), PANEL_TOTAL_W, 24}, "BLUETOOTH PAIRING",
            TextStyle{0, TextAlign::Center, Color::DarkGray, 1, false, false});
  ui().text(Rect{0, (int16_t)(midY + 40), PANEL_TOTAL_W, 24}, "enter on desktop:",
            TextStyle{0, TextAlign::Center, Color::DarkGray, 1, false, false});
  char b[8];
  snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  ui().text(Rect{0, (int16_t)(midY - 30), PANEL_TOTAL_W, 40}, b,
            TextStyle{0, TextAlign::Center, Color::Black, 1, true, false});
}

// Everything drawStatusPanel() used to draw MINUS the passkey early-return
// and the menu/settings/reset overlay drawn on top — those are now their
// own FreeInkApp screens (screenMenu/screenSettings/screenReset below), and
// the passkey check now lives in screenMain, which is the only caller.
static void drawStatusPanelContent(uint32_t now) {
  ui().fill(Rect{PANEL_X, 0, PANEL_W, PANEL_TOTAL_H}, Paint::solid(Color::White));

  int16_t y = 12;
  char line[96];
  snprintf(line, sizeof(line), "%s%s%s", ownerName()[0] ? ownerName() : "", ownerName()[0] ? "'s " : "", petName());
  ui().text(Rect{PANEL_X, y, PANEL_W, 28}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
  y += 32;

  snprintf(line, sizeof(line), "%s", kStateNames[activeState]);
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 28;

  bool inPrompt = tama.promptId[0] && !responseSent;
  if (napping) {
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, "napping - hold UP to wake",
              TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  } else if (inPrompt) {
    uint32_t waited = (now - promptArrivedMs) / 1000;
    snprintf(line, sizeof(line), "approve? %lus", (unsigned long)waited);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 24;
    ui().text(Rect{PANEL_X, y, PANEL_W, 24}, tama.promptTool, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
    y += 28;
    ui().text(Rect{PANEL_X, y, PANEL_W, 40}, tama.promptHint, TextStyle{0, TextAlign::Left, Color::DarkGray, 2, false, false});
    y += 44;
    ui().text(Rect{PANEL_X, y, (int16_t)(PANEL_W / 2), 20}, "CONFIRM: approve",
            TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 22;
    ui().text(Rect{PANEL_X, y, (int16_t)(PANEL_W / 2), 20}, "BACK: deny", TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 28;
  } else if (clockActive()) {
    drawClock(now, y);
  } else if (displayMode == DISP_PET) {
    drawPetPage(now, y);
  } else if (displayMode == DISP_INFO) {
    drawInfoPage(now, y);
  } else if (settings().hud) {
    snprintf(line, sizeof(line), "sessions %u  running %u  waiting %u", tama.sessionsTotal, tama.sessionsRunning,
             tama.sessionsWaiting);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 24;
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, tama.msg, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    y += 28;

    // Transcript (LEFT/RIGHT scroll — see "Input mapping"). tama.lines is
    // newest-first; transcriptOffset 0 shows the newest TRANSCRIPT_VISIBLE
    // entries.
    if (tama.nLines > 0) {
      uint8_t maxOffset = tama.nLines > TRANSCRIPT_VISIBLE ? tama.nLines - TRANSCRIPT_VISIBLE : 0;
      if (transcriptOffset > maxOffset) transcriptOffset = maxOffset;
      char hdr[40];
      if (maxOffset > 0) {
        snprintf(hdr, sizeof(hdr), "transcript  <- %u/%u ->", transcriptOffset + 1, maxOffset + 1);
      } else {
        snprintf(hdr, sizeof(hdr), "transcript");
      }
      ui().text(Rect{PANEL_X, y, PANEL_W, 18}, hdr, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
      y += 22;
      for (uint8_t i = 0; i < TRANSCRIPT_VISIBLE && (transcriptOffset + i) < tama.nLines; i++) {
        ui().text(Rect{PANEL_X, y, PANEL_W, 20}, tama.lines[transcriptOffset + i],
                  TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
        y += 22;
      }
    }
  }

  y = (int16_t)(PANEL_TOTAL_H - 140);  // bottom-anchored so it stays clear of the
                                        // content above regardless of panel height
  PlatformBatteryStatus bat = platformBatteryStatus();
  snprintf(line, sizeof(line), "battery %d%%  %s", bat.pct, bat.usb ? "usb" : "");
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 22;
  snprintf(line, sizeof(line), "ble %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 22;
  snprintf(line, sizeof(line), "Lv %u  tokens %lu", stats().level, (unsigned long)stats().tokens);
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
}

// ---------------------------------------------------------------------------
// FreeInkApp integration.
//
// See docs/freeinkapp-migration.md for the full design writeup. Summary:
//  - One FreeInkApp<UI_MAX_INTERACTIONS, UI_MAX_HANDLERS> instance (gApp)
//    owns screen transitions (setScreen) and the interaction/handler
//    tables; screens are plain functions (ScreenFn — see FreeInkApp.h),
//    not objects, matching the SDK's "no screen stack" model.
//  - Each pre-migration overlay (menu/settings/reset) becomes its own
//    full-panel screen rather than a floating box drawn over screenMain —
//    the one-active-screen model has no "underneath" to composite with, so
//    threading overlay-open flags through screenMain's draw path would
//    have fought the framework instead of using it. See "Overlay screens"
//    in the migration doc for the visual tradeoff this makes.
//  - BACK is the one button routed through FreeInkApp's real action
//    dispatch: each screen registers a single full-screen hit with
//    InputBack, and on() handlers below do what dispatchBack() used to do
//    inline. CONFIRM/UP/DOWN stay hand-rolled (pollHold() below) because
//    InputSnapshot has no hold concept for physical buttons, and CONFIRM's
//    "advance the highlighted item" behavior doesn't fit the SDK's
//    focus-then-confirm model without auto-focus bookkeeping that would
//    add complexity for no behavioral gain over calling the handler
//    directly. LEFT/RIGHT stay hand-rolled too, so a burst of queued
//    presses in one tick still decrements/increments per press instead of
//    collapsing to one InputSnapshot edge.
// ---------------------------------------------------------------------------
static constexpr size_t UI_MAX_INTERACTIONS = 48;  // headroom over the ~2
                                                    // hits any single screen
                                                    // here actually registers
static constexpr size_t UI_MAX_HANDLERS = 16;      // headroom over the 4
                                                    // ActionIds below
using AppType = freeink::ui::FreeInkApp<UI_MAX_INTERACTIONS, UI_MAX_HANDLERS>;
using ScreenCtx = AppType::ScreenType;

// Constructed lazily in setup() once ui()/uiPtr exists (same reasoning as
// uiStorage above — FreeInkApp needs a live DrawTarget& at construction).
alignas(AppType) static unsigned char appStorage[sizeof(AppType)];
static AppType* gApp = nullptr;

enum AppAction : freeink::ui::ActionId {
  ACT_MAIN_BACK = 1,
  ACT_MENU_BACK,
  ACT_SETTINGS_BACK,
  ACT_RESET_BACK,
};

static void screenMain(ScreenCtx& screen, void* user);
static void screenMenu(ScreenCtx& screen, void* user);
static void screenSettings(ScreenCtx& screen, void* user);
static void screenReset(ScreenCtx& screen, void* user);
static void screenSleep(ScreenCtx& screen, void* user);
static void screenPowerOff(ScreenCtx& screen, void* user);

// Monochrome theme matching this board's existing look (Color::Black/
// White/DarkGray only, one font slot for every text role — the bundled
// DisplayTarget points every font slot at the same bitmap font unless
// setFont() is called, which this firmware never does). The SDK's own
// migration doc (docs/freeink-ui.md "Adopting FreeInkUI in an Existing
// Firmware") suggests sourcing tokens from a JSON ThemeDocument; this
// project has no JSON config infrastructure and doesn't need one just for
// this, so the tokens are a plain constexpr-ish literal instead — see
// docs/freeinkapp-migration.md "Theme tokens".
static freeink::ui::ThemeTokens buildTheme() {
  freeink::ui::ThemeTokens t = freeink::ui::defaultThemeTokens(0, 0, 0);
  t.smallText = TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false};
  t.bodyText = TextStyle{0, TextAlign::Left, Color::Black, 1, false, false};
  t.titleText = TextStyle{0, TextAlign::Left, Color::Black, 1, true, false};
  t.rowHeight = 30;  // matches the pre-migration hand-rolled menu row spacing
  return t;
}

// Overlay chrome shared by the menu/settings/reset screens: a centered
// floating box on an otherwise blank page. Pre-migration this box floated
// over whatever screenMain was showing underneath (the menu could open
// over the pet, HUD, or clock, which stayed visible around its edges);
// FreeInkApp's one-screen model has nothing "underneath" once these become
// their own screens, so the fill() below clears the whole page to white
// first — see docs/freeinkapp-migration.md "Overlay screens".
static void drawOverlayBox(uint8_t itemCount, int16_t& boxX, int16_t& boxY, int16_t& boxW, int16_t& boxH) {
  boxW = 340;
  boxH = (int16_t)(20 + itemCount * 30 + 30);
  boxX = (int16_t)((PANEL_TOTAL_W - boxW) / 2);
  boxY = (int16_t)((PANEL_TOTAL_H - boxH) / 2);
  ui().fill(Rect{0, 0, PANEL_TOTAL_W, PANEL_TOTAL_H}, Paint::solid(Color::White));
  ui().fill(Rect{boxX, boxY, boxW, boxH}, Paint::solid(Color::White), 10);
  ui().stroke(Rect{boxX, boxY, boxW, boxH}, Paint::solid(Color::Black), 2, 10);
}

static void drawOverlayFooter(int16_t x, int16_t y, int16_t w) {
  ui().text(Rect{x, y, w, 20}, "CONFIRM: next   BACK: select",
            TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
}

// The base screen: NORMAL/PET/INFO/HUD/clock/prompt/nap are all sub-modes
// of this ONE FreeInkApp screen (not separate screens) — CONFIRM cycling
// displayMode never leaves screenMain, so there's no FreeInkApp screen
// transition involved, just like there wasn't a menuOpen-style flag flip
// pre-migration either. The passkey pairing screen is handled the same
// way: a live check of blePasskey() rather than a persistent "screen"
// state, matching how drawStatusPanel() checked it pre-migration.
static void screenMain(ScreenCtx& screen, void*) {
  uint32_t now = millis();
  if (blePasskey() != 0) {
    drawPasskey();
  } else {
    drawStatusPanelContent(now);
    RefreshHint spriteHint = napping ? renderNapFrame() : renderSprite(now);
    if (spriteHint != RefreshHint::None) gApp->invalidate(spriteHint);
  }
  // Registered unconditionally (even while the passkey screen is showing),
  // matching dispatchBack()'s pre-migration behavior of never gating on
  // passkey display either.
  screen.frame().hit(screen.frame().screen(), ACT_MAIN_BACK, 0, InputBack);
}

static void screenMenu(ScreenCtx& screen, void*) {
  int16_t boxX, boxY, boxW, boxH;
  drawOverlayBox(MENU_N, boxX, boxY, boxW, boxH);

  ListItem items[MENU_N];
  for (uint8_t i = 0; i < MENU_N; i++) {
    items[i] = ListItem{};
    items[i].label = menuItems[i];
    items[i].value = (i == 4) ? (dataDemo() ? "on" : "off") : nullptr;
  }
  ListProps props;
  props.items = items;
  props.count = MENU_N;
  props.selectedIndex = menuSel;
  props.action = NO_ACTION;  // visual only — CONFIRM/BACK drive selection
                              // by hand (see dispatchConfirmTap/menuConfirm),
                              // not through list()'s own hit registration
  props.rowHeight = 30;
  props.labelText = TextStyle{0, TextAlign::Left, Color::Black, 1, false, false};
  props.valueText = TextStyle{0, TextAlign::Right, Color::DarkGray, 1, false, false};
  Rect listRect{(int16_t)(boxX + 12), (int16_t)(boxY + 10), (int16_t)(boxW - 24), (int16_t)(MENU_N * 30)};
  freeink::ui::list(screen.frame(), listRect, props);

  drawOverlayFooter((int16_t)(boxX + 12), (int16_t)(boxY + 10 + MENU_N * 30 + 4), (int16_t)(boxW - 24));

  screen.frame().hit(screen.frame().screen(), ACT_MENU_BACK, 0, InputBack);
}

static void screenSettings(ScreenCtx& screen, void*) {
  int16_t boxX, boxY, boxW, boxH;
  drawOverlayBox(SETTINGS_N, boxX, boxY, boxW, boxH);

  ListItem items[6];  // 6 = SETTINGS_ITEMS_LIGHT's count, the largest of the two lists
  char valbuf[6][24];
  Settings& s = settings();
  for (uint8_t i = 0; i < SETTINGS_N; i++) {
    items[i] = ListItem{};
    items[i].label = settingsItems[i];
    const char* value = nullptr;
    if (i == 0) value = s.hud ? "on" : "off";
    else if (i == 1) value = s.flash ? "on" : "off";
    else if (i == 2) { snprintf(valbuf[i], sizeof(valbuf[i]), "%u/%u", characterIdx + 1, characterCount); value = valbuf[i]; }
    else if (hasBrightnessItem && i == 3) { snprintf(valbuf[i], sizeof(valbuf[i]), "%u/5", brightLevel + 1); value = valbuf[i]; }
    items[i].value = value;
  }
  ListProps props;
  props.items = items;
  props.count = SETTINGS_N;
  props.selectedIndex = settingsSel;
  props.action = NO_ACTION;
  props.rowHeight = 30;
  props.labelText = TextStyle{0, TextAlign::Left, Color::Black, 1, false, false};
  props.valueText = TextStyle{0, TextAlign::Right, Color::DarkGray, 1, false, false};
  Rect listRect{(int16_t)(boxX + 12), (int16_t)(boxY + 10), (int16_t)(boxW - 24), (int16_t)(SETTINGS_N * 30)};
  freeink::ui::list(screen.frame(), listRect, props);

  drawOverlayFooter((int16_t)(boxX + 12), (int16_t)(boxY + 10 + SETTINGS_N * 30 + 4), (int16_t)(boxW - 24));

  screen.frame().hit(screen.frame().screen(), ACT_SETTINGS_BACK, 0, InputBack);
}

static void screenReset(ScreenCtx& screen, void*) {
  uint32_t now = millis();
  int16_t boxX, boxY, boxW, boxH;
  drawOverlayBox(RESET_N, boxX, boxY, boxW, boxH);

  ListItem items[RESET_N];
  for (uint8_t i = 0; i < RESET_N; i++) {
    items[i] = ListItem{};
    // Tap-twice confirm: first tap arms (label flips to "really?"), second
    // within 3s executes — same window as pre-migration applyReset().
    bool armed = (i == resetConfirmIdx) && (int32_t)(now - resetConfirmUntil) < 0;
    items[i].label = armed ? "really?" : resetItems[i];
  }
  ListProps props;
  props.items = items;
  props.count = RESET_N;
  props.selectedIndex = resetSel;
  props.action = NO_ACTION;
  props.rowHeight = 30;
  props.labelText = TextStyle{0, TextAlign::Left, Color::Black, 1, false, false};
  Rect listRect{(int16_t)(boxX + 12), (int16_t)(boxY + 10), (int16_t)(boxW - 24), (int16_t)(RESET_N * 30)};
  freeink::ui::list(screen.frame(), listRect, props);

  drawOverlayFooter((int16_t)(boxX + 12), (int16_t)(boxY + 10 + RESET_N * 30 + 4), (int16_t)(boxW - 24));

  screen.frame().hit(screen.frame().screen(), ACT_RESET_BACK, 0, InputBack);
}

static void screenSleep(ScreenCtx&, void*) {
  ui().fill(Rect{0, 0, PANEL_TOTAL_W, PANEL_TOTAL_H}, Paint::solid(Color::White));
  ui().text(Rect{0, (int16_t)(PANEL_TOTAL_H / 2 - 20), PANEL_TOTAL_W, 40}, "sleeping - press any button",
            TextStyle{0, TextAlign::Center, Color::DarkGray, 1, false, false});
}

static void screenPowerOff(ScreenCtx&, void*) {
  ui().fill(Rect{0, 0, PANEL_TOTAL_W, PANEL_TOTAL_H}, Paint::solid(Color::White));
  ui().text(Rect{0, (int16_t)(PANEL_TOTAL_H / 2 - 20), PANEL_TOTAL_W, 40}, "powered off - press POWER",
            TextStyle{0, TextAlign::Center, Color::DarkGray, 1, false, false});
}

// ---------------------------------------------------------------------------
// Menu / settings / reset actions — mirrors the earlier generation's
// menuConfirm()/applySetting()/applyReset(), now navigating via
// gApp->setScreen() instead of flipping menuOpen/settingsOpen/resetOpen
// bools. Called from the BACK action handlers below (real FreeInkApp
// dispatch) and, for CONFIRM, from dispatchConfirmTap() (hand-rolled —
// see the FreeInkApp integration comment above for why).
// ---------------------------------------------------------------------------
static void powerOffSequence() {
  gApp->setScreen(screenPowerOff, nullptr, RefreshHint::Full);
  gApp->render(InputSnapshot{});
  freeink::ui::present(display, gApp->lastRenderRefreshHint());
  // No PMIC hard-off on this board (the earlier generation's
  // M5.Axp.PowerOff()) — real ESP32 deep sleep, woken by the POWER button,
  // is the equivalent off state. Does not return.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

static void menuConfirm() {
  switch (menuSel) {
    case 0:
      settingsSel = 0;
      currentScreen = SCR_SETTINGS;
      gApp->setScreen(screenSettings, nullptr, RefreshHint::Fast);
      break;
    case 1:
      powerOffSequence();  // does not return
      break;
    case 2:
      displayMode = DISP_INFO;
      infoPage = INFO_PG_BUTTONS;
      currentScreen = SCR_MAIN;
      gApp->setScreen(screenMain, nullptr, RefreshHint::Fast);
      break;
    case 3:
      displayMode = DISP_INFO;
      infoPage = INFO_PG_CREDITS;
      currentScreen = SCR_MAIN;
      gApp->setScreen(screenMain, nullptr, RefreshHint::Fast);
      break;
    case 4:
      dataSetDemo(!dataDemo());  // stays on the menu screen
      break;
    case 5:
      currentScreen = SCR_MAIN;
      gApp->setScreen(screenMain, nullptr, RefreshHint::Fast);
      break;
  }
}

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  // Collapse the two settings lists (with/without "brightness" — see
  // hasBrightnessItem above) onto one switch: "brightness" is peeled off
  // first, then every index past it is shifted down by one so hud/flash/
  // character/reset/back line up on the same cases regardless of which list
  // is showing, instead of duplicating the whole switch per list.
  //
  // "reset" and "back" both leave the settings screen; back returns to
  // screenMain directly (not to the menu — settings replaced the menu
  // outright when it opened, matching menuConfirm() case 0 above, so
  // there's nothing to return "to" but the base screen).
  if (hasBrightnessItem && idx == 3) { cycleBrightness(); return; }
  uint8_t normIdx = (hasBrightnessItem && idx > 3) ? (uint8_t)(idx - 1) : idx;
  switch (normIdx) {
    case 0: s.hud = !s.hud; break;
    case 1: s.flash = !s.flash; break;
    case 2: nextCharacter(); return;
    case 3:
      resetSel = 0;
      resetConfirmIdx = 0xFF;
      currentScreen = SCR_RESET;
      gApp->setScreen(screenReset, nullptr, RefreshHint::Fast);
      return;
    case 4:
      currentScreen = SCR_MAIN;
      gApp->setScreen(screenMain, nullptr, RefreshHint::Fast);
      return;
  }
  settingsSave();
}

static void applyReset(uint8_t idx, uint32_t now) {
  if (idx == 2) {  // back — returns to settings, which stayed "open" underneath
    currentScreen = SCR_SETTINGS;
    gApp->setScreen(screenSettings, nullptr, RefreshHint::Fast);
    return;
  }

  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;
  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    return;
  }

  if (idx == 0) {
    // delete char: stop using the current SD character, fall back to bufo.
    // Unlike the earlier generation (which owned its LittleFS /characters/
    // storage and wiped it), this never deletes files from the SD card —
    // that's user media the firmware doesn't own.
    applyCharacter(0);
    characterIdxSave(0);
  } else {
    // factory reset: NVS namespace wipe (stats, owner, petname, settings,
    // character choice) + BLE bonds. No filesystem to format — SD content
    // is user media, untouched.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    bleClearBonds();
    delay(300);
    ESP.restart();  // does not return
  }
  currentScreen = SCR_SETTINGS;
  gApp->setScreen(screenSettings, nullptr, RefreshHint::Fast);
}

// ---------------------------------------------------------------------------
// BACK action handlers — the one button routed through FreeInkApp's real
// on()/ActionId dispatch (see the FreeInkApp integration comment above).
// render()'s dispatch already invalidates(Fast) after any of these fires,
// so unlike the hand-rolled CONFIRM/UP/DOWN paths below, these don't need
// to call gApp->invalidate() themselves.
// ---------------------------------------------------------------------------
static void onMainBack(const ActionEvent&, void*) {
  uint32_t now = millis();
  bool inPrompt = tama.promptId[0] && !responseSent;
  if (inPrompt) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
    sendCmd(cmd);
    responseSent = true;
    statsOnDenial();
  } else if (displayMode == DISP_INFO) {
    infoPage = (infoPage + 1) % INFO_PAGES;
  } else if (displayMode == DISP_PET) {
    petPage = (petPage + 1) % PET_PAGES;
  } else {
    uint8_t maxOffset = tama.nLines > TRANSCRIPT_VISIBLE ? tama.nLines - TRANSCRIPT_VISIBLE : 0;
    transcriptOffset = (transcriptOffset >= maxOffset) ? 0 : transcriptOffset + 1;
  }
  (void)now;
}

static void onMenuBack(const ActionEvent&, void*) { menuConfirm(); }
static void onSettingsBack(const ActionEvent&, void*) { applySetting(settingsSel); }
static void onResetBack(const ActionEvent&, void*) { applyReset(resetSel, millis()); }

// ---------------------------------------------------------------------------
// CONFIRM tap/hold — hand-rolled (see the FreeInkApp integration comment
// above for why), driven from handleInput()'s pollHold().
// ---------------------------------------------------------------------------
static void dispatchConfirmTap(uint32_t now) {
  bool inPrompt = tama.promptId[0] && !responseSent;
  if (inPrompt) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
    sendCmd(cmd);
    responseSent = true;
    uint32_t tookS = (now - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    if (tookS < 5) triggerOneShot(P_HEART, 2000);
  } else if (currentScreen == SCR_RESET) {
    resetSel = (resetSel + 1) % RESET_N;
    resetConfirmIdx = 0xFF;
  } else if (currentScreen == SCR_SETTINGS) {
    settingsSel = (settingsSel + 1) % SETTINGS_N;
  } else if (currentScreen == SCR_MENU) {
    menuSel = (menuSel + 1) % MENU_N;
  } else {
    displayMode = (DisplayMode)((displayMode + 1) % DISP_COUNT);
  }
  gApp->invalidate(RefreshHint::Fast);
}

static void dispatchConfirmHold() {
  if (currentScreen == SCR_RESET) {
    currentScreen = SCR_SETTINGS;
    gApp->setScreen(screenSettings, nullptr, RefreshHint::Fast);
  } else if (currentScreen == SCR_SETTINGS) {
    currentScreen = SCR_MAIN;
    gApp->setScreen(screenMain, nullptr, RefreshHint::Fast);
  } else if (currentScreen == SCR_MENU) {
    currentScreen = SCR_MAIN;
    gApp->setScreen(screenMain, nullptr, RefreshHint::Fast);
  } else {
    menuSel = 0;
    currentScreen = SCR_MENU;
    gApp->setScreen(screenMenu, nullptr, RefreshHint::Fast);
  }
}

// ---------------------------------------------------------------------------
// Screen sleep (POWER button) — hand-rolled, entirely outside FreeInkApp's
// render()/route() loop: while asleep, loop() never calls gApp->render()
// at all (see loop() below), matching the pre-migration build never
// calling drawStatusPanel()/renderSprite() while screenAwake was false.
// ---------------------------------------------------------------------------
static void enterSleepScreen(uint32_t now) {
  screenAwake = false;
  currentScreen = SCR_SLEEP;
  gApp->setScreen(screenSleep, nullptr, RefreshHint::Full);
  gApp->render(InputSnapshot{});
  freeink::ui::present(display, gApp->lastRenderRefreshHint());
  lastFullRefreshMs = now;
}

static void wakeScreen(uint32_t now) {
  screenAwake = true;
  stateEnteredMs = now;
  sleepFrameDrawn = false;
  lastSpriteDrawMs = 0;
  currentScreen = SCR_MAIN;
  gApp->setScreen(screenMain, nullptr, RefreshHint::Full);
  gApp->render(InputSnapshot{});
  freeink::ui::present(display, gApp->lastRenderRefreshHint());
  lastFullRefreshMs = now;
}

// ---------------------------------------------------------------------------
// Nap (UP long-press) — substitutes for the earlier generation's face-down
// detection, which needed an IMU this board doesn't have (per the
// AskUserQuestion decision recorded in README "Known limitations": map nap
// to a button instead of dropping it).
// ---------------------------------------------------------------------------
static void toggleNap(uint32_t now) {
  if (!napping) {
    napping = true;
    napStartMs = now;
    napFrameDrawn = false;
  } else {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    stateEnteredMs = now;
    sleepFrameDrawn = false;
    lastSpriteDrawMs = 0;
  }
}

// ---------------------------------------------------------------------------
// Input mapping ---------------------------------------------------------------
// The X4's InputStyle::XteinkAdcLadder is NOT a 3-button layout like the
// earlier M5-based generation — it's 7 distinct semantic buttons (BACK/
// CONFIRM/LEFT/RIGHT/UP/DOWN/POWER) off two resistor-ladder ADC pins,
// decoded by InputManager (see README "Input mapping" for the full
// verification trail against the SDK headers).
//
// CONFIRM and BACK mirror that generation's BtnA/BtnB roles as closely as
// this board's buttons allow: CONFIRM tap = approve / advance selection /
// cycle screen (BtnA tap); CONFIRM hold (~600ms) = open menu, closing any
// nested overlay first (hold-A); BACK tap = deny / act on the highlighted
// item / next page (BtnB tap). The extra buttons this board has beyond
// A/B get used for things that generation didn't have a control for:
//   LEFT / RIGHT   scroll the transcript panel
//   UP             tap: force a full e-paper refresh now
//                  hold (~800ms): toggle nap
//   DOWN           hold (~800ms): trigger "dizzy" — substitute for shake
//                  (BoardConfig::XTEINK_X4 has NO_SENSORS; confirmed via
//                  BoardConfig, not assumed)
//   POWER          toggle screen sleep — enterSleepScreen()/wakeScreen() above
//
// Only BACK is fed into gSnapshot and routed through FreeInkApp's real
// dispatch (gApp->render() below); CONFIRM/UP/DOWN/LEFT/RIGHT/POWER stay
// hand-rolled here — see the FreeInkApp integration comment above for why.
// ---------------------------------------------------------------------------
struct HoldButton {
  uint32_t pressStart = 0;
  bool     longFired = false;
  bool     wasDown = false;
};
// Returns 1 on a tap (released before holdMs), 2 the instant holdMs is
// crossed while still held, 0 otherwise.
static uint8_t pollHold(HoldButton& hb, bool down, uint32_t now, uint32_t holdMs) {
  uint8_t result = 0;
  if (down && !hb.wasDown) { hb.pressStart = now; hb.longFired = false; }
  if (down && !hb.longFired && now - hb.pressStart >= holdMs) { hb.longFired = true; result = 2; }
  if (!down && hb.wasDown && !hb.longFired) result = 1;
  hb.wasDown = down;
  return result;
}

static HoldButton confirmHold, upHold, downHold;

// Rebuilt every tick by handleInput(), consumed once by gApp->render() in
// loop(). Edge-only (a tap sets it true for exactly the tick it happened
// in), matching InputSnapshot's own edge-triggered field convention.
static InputSnapshot gSnapshot;

// InputManager runs on a background FreeRTOS task (input.beginAsync() in
// setup()), not on our own polling here — required because
// freeink::ui::present() blocks the main loop for anywhere from ~50ms (a
// fast refresh) to ~2s (a full refresh, per FreeInkDisplay's own docs), and
// a press-and-release that happens entirely inside one of those blocking
// calls would otherwise never be seen. The async task calls update() itself,
// so isPressed() (a plain level read of the task's currentState) is still
// safe to read from here — see InputManager.cpp's asyncPoll(). Only
// wasPressed()/update() are unsafe to call ourselves once async polling owns
// the edge state; edges come from popPress().
static void handleInput(uint32_t now) {
  gSnapshot = InputSnapshot{};

  if (!screenAwake) {
    // Any press just wakes the screen — never doubles as that button's
    // normal action, so a CONFIRM that wakes the device can't also
    // silently approve a prompt the user hasn't seen yet. wasHomeKeyPressed()
    // covers X4 Pro's capacitive Home key, which — unlike every other
    // button — never enters the popPress() queue (see the BACK-dispatch
    // comment below); inert (returns false) on boards without touch.
    uint8_t btn;
    bool anyPress = input.wasHomeKeyPressed();
    while (input.popPress(btn)) anyPress = true;
    if (anyPress) wakeScreen(now);
    confirmHold = HoldButton{};
    upHold = HoldButton{};
    downHold = HoldButton{};
    return;
  }

  // Real shake, on boards with an IMU (see checkShake() above). Gated on
  // "not on an overlay screen and not napping" to match the earlier
  // M5-era generation's own !menuOpen/!screenOff gate — a shake mid-menu-
  // navigation or mid-nap shouldn't interrupt either.
  static uint32_t lastShakeCheckMs = 0;
  bool overlayOrNap = (currentScreen != SCR_MAIN) || napping;
  if (imuAvailable && !overlayOrNap && now - lastShakeCheckMs > 50) {
    lastShakeCheckMs = now;
    if (checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      triggerOneShot(P_DIZZY, 2000);
    }
  }

  // BACK: fed into gSnapshot below, routed through FreeInkApp. LEFT/RIGHT/
  // POWER: plain press-edge actions, handled directly here (see the
  // FreeInkApp integration comment above for why LEFT/RIGHT aren't routed
  // through an ActionId too). CONFIRM/UP/DOWN edges are drained here too
  // (silently — they're handled below via level-polling so tap and hold
  // can be told apart).
  //
  // X4 Pro has no physical BACK — its profile's input.back is PIN_UNASSIGNED
  // (confirmed via BoardConfig; see README "Multi-board support"), and
  // unlike every other button, the capacitive Home key does NOT synthesize
  // into the BTN_* system InputManager::popPress() drains — it's its own
  // API (wasHomeKeyTapped()/wasHomeKeyPressed()/wasHomeKeyLongPressed()).
  // wasHomeKeyTapped() also has no LEFT/RIGHT equivalent to fall back on
  // (X4 Pro's own input.left/right are PIN_UNASSIGNED too — its two
  // physical nav buttons wire to the semantic UP/DOWN slots instead, per
  // its BoardConfig profile), so a Home-key tap is the only way BACK
  // happens on this board; it feeds the same gSnapshot.back every other
  // board's physical BACK button sets. Inert (returns false) on boards
  // without touch.
  bool backEdge = input.wasHomeKeyTapped();

  uint8_t btn;
  while (input.popPress(btn)) {
    if (btn == InputManager::BTN_BACK) {
      backEdge = true;
    } else if (btn == InputManager::BTN_LEFT) {
      if (currentScreen == SCR_MAIN) {
        if (transcriptOffset > 0) transcriptOffset--;
        gApp->invalidate(RefreshHint::Fast);
      }
    } else if (btn == InputManager::BTN_RIGHT) {
      if (currentScreen == SCR_MAIN) {
        uint8_t maxOffset = tama.nLines > TRANSCRIPT_VISIBLE ? tama.nLines - TRANSCRIPT_VISIBLE : 0;
        if (transcriptOffset < maxOffset) transcriptOffset++;
        gApp->invalidate(RefreshHint::Fast);
      }
    } else if (btn == InputManager::BTN_POWER) {
      enterSleepScreen(now);
      return;  // screen is asleep now; the rest of this batch no longer applies
    }
  }
  gSnapshot.back = backEdge;

  uint8_t r;
  r = pollHold(confirmHold, input.isPressed(InputManager::BTN_CONFIRM), now, 600);
  if (r == 1) dispatchConfirmTap(now);
  else if (r == 2) dispatchConfirmHold();

  r = pollHold(upHold, input.isPressed(InputManager::BTN_UP), now, 800);
  if (r == 1) {
    gApp->invalidate(RefreshHint::Full);
  } else if (r == 2) {
    toggleNap(now);
    gApp->invalidate(RefreshHint::Fast);
  }

  r = pollHold(downHold, input.isPressed(InputManager::BTN_DOWN), now, 800);
  if (r == 2 && !napping && (int32_t)(now - oneShotUntil) >= 0) {
    triggerOneShot(P_DIZZY, 2000);
  }
}

void setup() {
  Serial.begin(115200);
  statsLoad();
  settingsLoad();
  petNameLoad();

  // Multi-board support: env:xteink links both the X4 (SSD1677 800x480) and
  // X3 (UC8253/UC8279 792x528) profiles into one ESP32-C3 binary — this is
  // the SDK's own documented pattern (freeink-sdk README "Supported
  // devices"), not something specific to this firmware. selectXteinkDevice()
  // I2C-fingerprints the X3-only peripherals (BQ27220 gauge, DS3231 RTC,
  // QMI8658 IMU) and, on a match, both switches BoardConfig::ACTIVE to the
  // X3 profile and (via setDisplayX3() below) tells the display driver which
  // panel it's driving — everything downstream (BatteryMonitor, Imu, Rtc,
  // this file's own PANEL_TOTAL_W/H) then reads the correct board
  // automatically. On env:xteink_x4pro (a separate ESP32-S3 build — X4 Pro
  // is a different MCU family, can't share this binary) this call is a
  // documented no-op. Must run before SdMan.begin()/display.begin().
  bool isX3 = freeink::selectXteinkDevice();
  if (isX3) display.setDisplayX3();

  // X3/X4 share the display's SPI bus with the SD card slot (BoardConfig::
  // XTEINK_X4.sd: sclk/mosi unassigned, separateSpi=false — miso(7) and
  // cs(12) are the only pins unique to the card). FreeInkDisplay::begin()
  // only wires MISO into the bus when the active panel driver needs it
  // (PanelDriver::spiMiso() defaults to -1 for SSD1677/X4 — the display
  // itself is write-only); left alone, the bus would come up with no MISO
  // pin and SD reads would never work afterward, since a second SPI.begin()
  // with different pins is unreliable once the bus is already initialized.
  // Claiming it once here, with the SD MISO included, before display.begin()
  // runs its own SPI.begin(), is exactly the sequence Free-Ink's own X4
  // consumer app (inkdeck, src/main.cpp setup()) uses for this same board.
  // X4 Pro doesn't need this: its SD card is native SDMMC on entirely
  // separate pins (CLK41/CMD42/DAT40), not a shared SPI bus — see
  // freeink-sdk/docs/xteink-x4pro-support.md "Storage".
  if (!BoardConfig::isX4Pro()) {
    SPI.begin(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.display.mosi,
              BoardConfig::ACTIVE.display.cs);
  }

  display.begin();
  PANEL_TOTAL_W = display.getDisplayWidth();
  PANEL_TOTAL_H = display.getDisplayHeight();
  PANEL_W = (int16_t)(PANEL_TOTAL_W - PANEL_X - 16);
  uiPtr = new (uiStorage) DisplayTarget(display.getFrameBuffer(), display.getDisplayWidth(),
                                        display.getDisplayHeight(), display.getDisplayWidthBytes(),
                                        Orientation::LandscapeCounterClockwise);
  display.clearScreen(0xFF);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);
  lastFullRefreshMs = millis();

  SdMan.begin();  // false if no card present — scanCharacters() below no-ops either way
  scanCharacters();
  applyCharacter(characterIdxLoad());

  imuBegin();
  rtcBegin();
  frontlight.begin();
  if (frontlight.present()) {
    settingsItems = SETTINGS_ITEMS_LIGHT;
    SETTINGS_N = 6;
    hasBrightnessItem = true;
    frontlight.setBrightness((uint8_t)(20 + brightLevel * 20));  // apply the default brightLevel=4 (100%)
  }

  input.begin();
  input.beginAsync();  // see handleInput()'s comment for why this is required

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);

  // FreeInkApp: theme + action handlers + the initial screen. See the
  // "FreeInkApp integration" comment above for the overall design.
  gApp = new (appStorage) AppType(ui(), uiPtr->deviceContext(), nullptr);
  gApp->setTheme(buildTheme());
  gApp->on(ACT_MAIN_BACK, onMainBack);
  gApp->on(ACT_MENU_BACK, onMenuBack);
  gApp->on(ACT_SETTINGS_BACK, onSettingsBack);
  gApp->on(ACT_RESET_BACK, onResetBack);
  currentScreen = SCR_MAIN;
  gApp->setScreen(screenMain, nullptr, RefreshHint::Full);

  stateEnteredMs = millis();
}

void loop() {
  uint32_t now = millis();

  dataPoll(&tama);

  // Never let a sleeping screen sit through an approval prompt unseen.
  if (!screenAwake && tama.promptId[0] && strcmp(tama.promptId, lastPromptId) != 0) {
    wakeScreen(now);
  }

  handleInput(now);

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0]) promptArrivedMs = now;
  }

  // New transcript data invalidates any scrolled-away view (see data.h's
  // lineGen comment: "lets UI reset scroll").
  if (tama.lineGen != lastLineGen) {
    lastLineGen = tama.lineGen;
    transcriptOffset = 0;
  }

  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;
  if (clockActive()) {
    struct tm lt;
    if (getSoftClock(lt, now)) applyClockMood(lt, now);
  }

  static PersonaState lastRenderedState = P_COUNT;
  if (activeState != lastRenderedState) {
    stateEnteredMs = now;
    sleepFrameDrawn = false;
    lastRenderedState = activeState;
  }

  if (!screenAwake) {
    delay(16);
    return;
  }

  // Mandatory DC-balance full refresh, independent of activity/state — see
  // FULL_REFRESH_INTERVAL_MS above. gApp->invalidate() only ever upgrades
  // the pending RefreshHint (see FreeInkApp::invalidate()), so this can't
  // downgrade a Full a screen transition already queued this tick.
  if (now - lastFullRefreshMs > FULL_REFRESH_INTERVAL_MS) {
    gApp->invalidate(RefreshHint::Full);
  }

  // Panel-content throttle: screenMain redraws HUD/pet/info/clock text into
  // the framebuffer every tick regardless (cheap — plain text draws), but a
  // panel PUSH is only requested on this cadence, to avoid hammering the
  // panel with redundant waveforms — matches the pre-migration "needsRedraw
  // || interval elapsed" gate; menu/settings/reset navigation already gets
  // its own immediate invalidate from dispatchConfirmTap()/render()'s
  // post-dispatch invalidate(Fast), so this only needs to cover screenMain's
  // otherwise-idle content (a growing "approve? Ns" counter, an updated
  // clock, new transcript lines).
  static uint32_t lastPanelInvalidateMs = 0;
  if (currentScreen == SCR_MAIN && now - lastPanelInvalidateMs > 1000) {
    gApp->invalidate(RefreshHint::Fast);
    lastPanelInvalidateMs = now;
  }

  gApp->render(gSnapshot);

  static bool loggedUiOverflow = false;
  if (!loggedUiOverflow && (gApp->interactionOverflowed() || gApp->handlerOverflowed())) {
    // Should never happen at UI_MAX_INTERACTIONS=48/UI_MAX_HANDLERS=16 given
    // what's actually registered today (see the FreeInkApp integration
    // comment above) — logged once, loudly, instead of silently dropping
    // interactions/handlers, so a future screen addition that overflows the
    // table gets noticed instead of just "acting weird".
    Serial.println("[ui] FreeInkApp interaction/handler table overflowed");
    loggedUiOverflow = true;
  }

  RefreshHint hint = gApp->lastRenderRefreshHint();
  if (hint != RefreshHint::None) {
    freeink::ui::present(display, hint);
    if (hint == RefreshHint::Full) lastFullRefreshMs = now;
  }

  delay(16);
}
