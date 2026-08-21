// claude-desktop-buddy — Xteink X4 firmware.
//
// Nordic UART Service BLE bridge (ble_bridge.cpp/h) + JSON wire protocol
// (data.h, xfer.h) + NVS-backed stats/owner (stats.h) driving a 7-state
// desk pet rendered on FreeInk (e-paper display, InputManager for the
// 7-button ADC ladder, BatteryMonitor). sd_character_pack.h/.cpp optionally
// replaces the compiled-in bufo icons below with one discovered on the SD
// card at boot — see README.md "SD-backed character packs".
//
// See README.md for the sprite-region size, the full-refresh timer
// interval, and the input mapping.

#include <Arduino.h>
#include <new>
#include <esp_mac.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <InputManager.h>
#include <BatteryMonitor.h>
#include <FreeInkUIDisplayTarget.h>
#include <SDCardManager.h>
#include <SPI.h>

#include "ble_bridge.h"
#include "data.h"
#include "assets/icons_bufo.h"
#include "sd_character_pack.h"

using freeink::ui::Color;
using freeink::ui::DisplayTarget;
using freeink::ui::Orientation;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

// ---------------------------------------------------------------------------
// Platform hooks required by data.h / xfer.h (see their declarations).
// ---------------------------------------------------------------------------

// X4 has no RTC (BoardConfig::XTEINK_X4 -> FREEINK_CAP_RTC is off — see
// BoardConfig.h's FREEINK_CAP_RTC device list, which does not include X4),
// and this port has no clock-face screen to feed (that M5 feature depended
// on IMU-based orientation detection the X4 also doesn't have — see README
// "Divergences"). Nothing on this board currently consumes wall-clock time,
// so the hook is a deliberate no-op rather than storing a value nothing
// reads. data.h still sets dataRtcValid() true after calling this, per its
// documented contract — a future consumer (e.g. a status timestamp) can
// start from here without touching data.h again.
void platformTimeSync(const struct tm&) {}

BatteryMonitor batteryMonitor;
PlatformBatteryStatus platformBatteryStatus() {
  BatteryMonitor::Status s = batteryMonitor.readStatus();
  PlatformBatteryStatus out{0, 0, 0, false};
  if (s.percentageKnown) out.pct = s.percentage;
  if (s.millivoltsKnown) out.mV = s.millivolts;
  // X4 has no charge-status pin (BoardConfig::XTEINK_X4.batteryChargeStatus
  // == PIN_UNASSIGNED), so BatteryMonitor can't report charging/mA. usbDetect
  // (GPIO20) IS wired on the profile; we read it directly here since nothing
  // in the SDK's ADC battery backend consumes it. Polarity (active-high) is
  // an assumption, NOT hardware-validated — flagged in README "Divergences".
  // The X4 Pro reverse-engineering doc explicitly notes its own VBUS/USB
  // pin was "not conclusively identified" even after a hardware RE session,
  // so treat this the same way until someone confirms it on a scope.
  if (BoardConfig::ACTIVE.usbDetect != BoardConfig::PIN_UNASSIGNED) {
    out.usb = digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  out.mA = 0;  // unknown — no current sense on this board
  return out;
}

// ---------------------------------------------------------------------------
// State machine — identical derivation to the M5 build's derive() in
// main.cpp, just renamed/re-typed here.
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
// Panel: 800x480. bufo's Icon assets top out at bufo::MAX_ICON_W x
// MAX_ICON_H = 192x200 (96px source x 2 scale — see tools/gif_to_icons.py
// and README "Asset pipeline"). SPRITE region is sized with margin around
// that and kept byte-aligned (x and w multiples of 8) so the manual
// framebuffer inversion used for the `attention` cue below never touches a
// partial byte at the edges.
// ---------------------------------------------------------------------------
static constexpr int16_t SPRITE_X = 40;
static constexpr int16_t SPRITE_Y = 40;
static constexpr int16_t SPRITE_W = 240;
static constexpr int16_t SPRITE_H = 260;
static_assert(SPRITE_W >= bufo::MAX_ICON_W && SPRITE_H >= bufo::MAX_ICON_H,
              "sprite region must fit the largest generated icon");

// Text/status panel occupies the rest of the 800x480 panel to the right of
// (and below) the sprite region.
static constexpr int16_t PANEL_X = SPRITE_X + SPRITE_W + 24;
static constexpr int16_t PANEL_W = 800 - PANEL_X - 16;

EInkDisplay display(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
                    BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
                    BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
InputManager input;

// DisplayTarget wraps the display's framebuffer pointer, which display.begin()
// only allocates at runtime (it's null before that) — constructing this as an
// eagerly-initialized global would capture a null pointer, since global
// constructors run before setup(). Built lazily in setup() after begin()
// instead; ui() below is the only accessor.
alignas(DisplayTarget) static unsigned char uiStorage[sizeof(DisplayTarget)];
static DisplayTarget* uiPtr = nullptr;
static DisplayTarget& ui() { return *uiPtr; }

// --- Mandatory DC-balance timer ---------------------------------------------
// Independent of activity: a long busy/idle stretch runs only partial/fast
// refreshes over the sprite region, which never DC-balances the panel.
// Force a FULL_REFRESH of the whole screen on this fixed cadence regardless
// of what state the pet is in. Tune this one constant, nothing else.
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
  // Idle carousel: advance to the next clip each time the current one loops.
  // Non-idle states have exactly one clip, so this is a no-op there.
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

// --- SD-backed character (optional) ------------------------------------------
// See README.md "SD-backed character packs": if a .charpack file is found on
// the SD card at boot, it replaces the compiled-in bufo icons for every
// state. Falls back to bufo automatically if there's no SD card, no pack on
// it, or a read hiccup mid-animation (getCurrentIcon() below) — the device
// is never left with nothing to draw.
static SdCharacterPack sdPack;
static bool sdCharacterActive = false;
// Covers up to ~320x400px @ 1bpp (⌈320/8⌉ * 400 = 16000) with margin over
// bufo's 192x200. A pack whose frames exceed this is the operator's to
// avoid by choosing --scale appropriately when converting it — there is no
// runtime dimension check here, same as the compiled-in asset (see README).
static constexpr size_t SD_FRAME_BUF_CAP = 16000;
static uint8_t sdFrameBuf[SD_FRAME_BUF_CAP];

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
static uint32_t stateEnteredMs = 0;
static uint32_t lastSpriteDrawMs = 0;
static bool     sleepFrameDrawn = false;
static bool     attentionFlashOn = false;

static void renderSprite(uint32_t now) {
  switch (activeState) {
    case P_SLEEP:
      // Static frame, no refresh loop: draw once on entry, then do nothing
      // until the state changes (checked by the caller via sleepFrameDrawn).
      if (sleepFrameDrawn) return;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_SLEEP, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      sleepFrameDrawn = true;
      return;

    case P_IDLE:
      // Slow-cadence partial refresh — e-ink reads a slow blink as
      // charming, not laggy (per the original README's framing).
      if (now - lastSpriteDrawMs < 1500) return;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_IDLE, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    case P_BUSY:
      // Fast partial refresh, fixed region only — this is the "working"
      // animation and wants to read as active.
      if (now - lastSpriteDrawMs < 350) return;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_BUSY, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    case P_ATTENTION: {
      // No LED on this board. Substitute cue: alternate the sprite region
      // between the normal icon and a bit-inverted version of it, on the
      // same ~400ms cadence the M5 build blinked its LED at. This reads as
      // "urgent" the way the idle blink can't, without needing a second
      // color plane.
      if (now - lastSpriteDrawMs < 400) return;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_ATTENTION, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      attentionFlashOn = !attentionFlashOn;
      if (attentionFlashOn) invertSpriteRegionBytes();
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;
    }

    case P_CELEBRATE: {
      // Infrequent (level-up, every 50K tokens) — can afford full refreshes.
      // Cycle the clip's frames, one FULL_REFRESH each, capped so a long
      // clip can't turn a celebration into a multi-second stall.
      if (now - lastSpriteDrawMs < 220) return;
      clearSpriteRegion();
      freeink::Icon icon;
      if (!getCurrentIcon(P_CELEBRATE, now, stateEnteredMs, icon)) break;
      drawIconCentered(icon);
      display.displayBuffer(EInkDisplay::FULL_REFRESH);
      lastFullRefreshMs = now;  // this refresh already DC-balanced the panel
      break;
    }

    case P_DIZZY:
      // Short-lived, fast partial — triggered by a button hold (no IMU on
      // this board; see the input-mapping comment above handleInput()).
      if (now - lastSpriteDrawMs < 200) return;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_DIZZY, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    case P_HEART:
      // Similar budget to celebrate but smaller/cheaper: partial refresh,
      // not full — it fires far more often (every fast approval).
      if (now - lastSpriteDrawMs < 250) return;
      clearSpriteRegion();
      {
        freeink::Icon icon;
        if (getCurrentIcon(P_HEART, now, stateEnteredMs, icon)) drawIconCentered(icon);
      }
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    default:
      return;
  }
  lastSpriteDrawMs = now;
}

// --- Status/text panel -------------------------------------------------------
static bool responseSent = false;
static char lastPromptId[40] = "";
static uint32_t promptArrivedMs = 0;

static void drawStatusPanel(uint32_t now) {
  ui().fill(Rect{PANEL_X, 0, PANEL_W, 480}, Paint::solid(Color::White));

  int16_t y = 12;
  char line[96];
  snprintf(line, sizeof(line), "%s%s%s", ownerName()[0] ? ownerName() : "", ownerName()[0] ? "'s " : "", petName());
  ui().text(Rect{PANEL_X, y, PANEL_W, 28}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
  y += 32;

  snprintf(line, sizeof(line), "%s", kStateNames[activeState]);
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 28;

  bool inPrompt = tama.promptId[0] && !responseSent;
  if (inPrompt) {
    uint32_t waited = (now - promptArrivedMs) / 1000;
    snprintf(line, sizeof(line), "approve? %lus", (unsigned long)waited);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 24;
    ui().text(Rect{PANEL_X, y, PANEL_W, 24}, tama.promptTool, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
    y += 28;
    ui().text(Rect{PANEL_X, y, PANEL_W, 40}, tama.promptHint, TextStyle{0, TextAlign::Left, Color::DarkGray, 2, false, false});
    y += 44;
    ui().text(Rect{PANEL_X, y, PANEL_W / 2, 20}, "CONFIRM: approve",
            TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 22;
    ui().text(Rect{PANEL_X, y, PANEL_W / 2, 20}, "BACK: deny", TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 28;
  } else {
    snprintf(line, sizeof(line), "sessions %u  running %u  waiting %u", tama.sessionsTotal, tama.sessionsRunning,
             tama.sessionsWaiting);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 24;
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, tama.msg, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    y += 28;
  }

  y = 340;
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

// --- Input mapping ------------------------------------------------------------
// The X4's InputStyle::XteinkAdcLadder is NOT a 3-button layout like the
// M5Stick — it's 7 distinct semantic buttons (BACK/CONFIRM/LEFT/RIGHT/UP/
// DOWN/POWER) off two resistor-ladder ADC pins, decoded by InputManager (see
// README "Input mapping" for the full verification trail against the SDK
// headers). That's MORE inputs than the M5 build's 3 physical buttons, so
// nothing needs to be combined — every required action gets its own button:
//   CONFIRM        approve (in a prompt) / wake+advance otherwise
//   BACK           deny (in a prompt) / dismiss otherwise
//   LEFT / RIGHT   scroll transcript
//   UP / DOWN      reserved (menu nav if a settings screen is added later)
//   DOWN long-press  manual "dizzy" trigger — substitute for the M5 build's
//                    shake gesture, which needed an IMU the X4 does not have
//                    (BoardConfig::XTEINK_X4 has ImuType::None; confirmed via
//                    InputManager/BoardConfig, not assumed)
//   POWER          reserved, not yet wired to any action
// Face-down nap (also IMU-driven) is dropped outright rather than remapped —
// there's no button gesture that means the same thing. See README.
static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static uint32_t downPressStart = 0;
static bool     downLongFired = false;

// InputManager runs on a background FreeRTOS task (input.beginAsync() in
// setup()), not on our own polling here — required because
// display.displayWindow()/displayBuffer() block the main loop for
// anywhere from ~50ms (a small partial) to ~2s (a full refresh, per
// FreeInkDisplay's own docs), and a CONFIRM/BACK press-and-release that
// happens entirely inside one of those blocking calls would otherwise
// never be seen. The async task calls update() itself, so isPressed()
// (a plain level read of the task's currentState) is still safe to read
// from here for the DOWN long-press check below — see InputManager.cpp's
// asyncPoll(). Only wasPressed()/update() are unsafe to call ourselves
// once async polling owns the edge state; edges come from popPress().
static void handleInput(uint32_t now) {
  uint8_t btn;
  while (input.popPress(btn)) {
    bool inPrompt = tama.promptId[0] && !responseSent;  // re-checked per event: the
                                                         // first popped press in a
                                                         // burst can flip responseSent
    if (btn == InputManager::BTN_CONFIRM && inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (now - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (btn == InputManager::BTN_BACK && inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
    }
  }

  // DOWN long-press: dizzy substitute for shake (see comment above).
  if (input.isPressed(InputManager::BTN_DOWN)) {
    if (downPressStart == 0) downPressStart = now;
    if (!downLongFired && now - downPressStart > 800 && (int32_t)(now - oneShotUntil) >= 0) {
      downLongFired = true;
      triggerOneShot(P_DIZZY, 2000);
    }
  } else {
    downPressStart = 0;
    downLongFired = false;
  }
}

// ---------------------------------------------------------------------------
// Scans /characters for the first *.charpack file and opens it, replacing
// the compiled-in bufo icons for every state. No selection UI yet — see
// README.md "SD-backed character packs" for why (v1 scope: this is the
// CrossPoint SdCardFontSystem "manual SD copy" install path, not the BLE
// push path — the SD card just needs one .charpack file on it).
static void loadSdCharacterIfPresent() {
  if (!SdMan.ready()) return;
  for (const String& name : SdMan.listFiles("/characters")) {
    if (!name.endsWith(".charpack")) continue;
    String path = "/characters/" + name;
    if (sdPack.open(path.c_str())) {
      sdCharacterActive = true;
      Serial.printf("[sd] loaded character pack %s\n", path.c_str());
    }
    return;  // first *.charpack wins, found or not — don't keep scanning
  }
}

void setup() {
  Serial.begin(115200);
  statsLoad();
  petNameLoad();

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
  SPI.begin(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.display.mosi,
            BoardConfig::ACTIVE.display.cs);

  display.begin();
  uiPtr = new (uiStorage) DisplayTarget(display.getFrameBuffer(), display.getDisplayWidth(),
                                        display.getDisplayHeight(), display.getDisplayWidthBytes(),
                                        Orientation::LandscapeCounterClockwise);
  display.clearScreen(0xFF);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);
  lastFullRefreshMs = millis();

  SdMan.begin();  // false if no card present — loadSdCharacterIfPresent() below no-ops either way
  loadSdCharacterIfPresent();

  input.begin();
  input.beginAsync();  // see handleInput()'s comment for why this is required

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  static char btName[16];
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);

  stateEnteredMs = millis();
}

void loop() {
  uint32_t now = millis();

  dataPoll(&tama);
  handleInput(now);

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0]) promptArrivedMs = now;
  }

  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  static PersonaState lastRenderedState = P_COUNT;  // force first-tick transition
  if (activeState != lastRenderedState) {
    stateEnteredMs = now;
    sleepFrameDrawn = false;
    lastRenderedState = activeState;
  }

  // Panel drawn first: renderSprite() can trigger a FULL_REFRESH (celebrate),
  // which pushes the whole framebuffer — the panel text must already be
  // current in it when that happens, not from a stale previous tick.
  drawStatusPanel(now);
  renderSprite(now);

  // Mandatory DC-balance full refresh, independent of activity/state.
  if (now - lastFullRefreshMs > FULL_REFRESH_INTERVAL_MS) {
    display.displayBuffer(EInkDisplay::FULL_REFRESH);
    lastFullRefreshMs = now;
  } else if (activeState != P_CELEBRATE) {
    // The status panel is drawn every tick above but only pushed to the
    // panel when something in it can plausibly have changed, to avoid
    // hammering the (still-visible) partial-refresh region with redundant
    // waveforms. displayWindow() over the panel's own rect keeps this
    // independent of the sprite region's refresh cadence.
    static uint32_t lastPanelDrawMs = 0;
    if (now - lastPanelDrawMs > 1000) {
      display.displayWindow(PANEL_X, 0, 800 - PANEL_X, 480);
      lastPanelDrawMs = now;
    }
  }

  delay(16);
}
