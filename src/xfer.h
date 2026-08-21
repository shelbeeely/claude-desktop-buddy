#pragma once
#include <Arduino.h>
#include <SDCardManager.h>
#include "ble_bridge.h"
#include <ArduinoJson.h>

// Ack goes to both streams — we don't track which one delivered the command,
// and writes to a clientless SerialBT just drop. The bridge listens on
// whichever port it opened.
static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok?"true":"false", (unsigned long)n);
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}

void petNameSet(const char* name);
const char* petName();
void ownerSet(const char* name);
const char* ownerName();
#include "stats.h"

// Platform hook: battery/power telemetry for the "status" ack below.
// The X4 has no PMIC — a plain ADC divider (see BoardConfig::XTEINK_X4)
// read via BatteryMonitor, no charge-status pin. Declared here (not
// defined) so xfer.h doesn't need to know about BatteryMonitor directly;
// main.cpp defines it.
struct PlatformBatteryStatus {
  int pct;   // 0-100
  int mV;    // battery millivolts
  int mA;    // battery current, negative = charging (always 0 — no charge-status pin)
  bool usb;  // external power present
};
PlatformBatteryStatus platformBatteryStatus();

inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // Dump everything the info screens show. Manual printf rather than
    // ArduinoJson serialize — less heap churn, and the shape is fixed.
    // fsFree/fsTotal report the SD card (this board's only storage besides
    // flash) — 0/0 when no card is mounted, matching what an empty/absent
    // filesystem would report anyway.
    PlatformBatteryStatus bat = platformBatteryStatus();
    unsigned long fsFree = 0, fsTotal = 0;
    if (SdMan.ready()) {
      fsTotal = (unsigned long)SdMan.sdTotalBytes();
      fsFree = (unsigned long)(SdMan.sdTotalBytes() - SdMan.sdUsedBytes());
    }
    char b[320];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":%d,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%u,\"fsFree\":%lu,\"fsTotal\":%lu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      bat.pct, bat.mV, bat.mA, bat.usb ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap(), fsFree, fsTotal,
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  // Runtime BLE character-pack push isn't supported — this board's
  // character art is either compiled in or loaded from an SD-card
  // .charpack at boot (see tools/gif_to_icons.py --sd-out and
  // src/sd_character_pack.h). Decline without acking char_begin, per
  // REFERENCE.md: "If your device doesn't want pushed files, don't ack
  // char_begin. The desktop times out after a few seconds and tells the
  // user it failed." file/chunk/file_end/char_end never arrive since the
  // desktop won't send them after an unacked char_begin.
  if (strcmp(cmd, "char_begin") == 0) return true;

  return false;
}
