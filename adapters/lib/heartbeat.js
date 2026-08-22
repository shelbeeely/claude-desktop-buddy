'use strict';

// Firmware-side field caps — see src/data.h's TamaState: msg[24], 8 lines
// of lines[8][92]. Truncating here is cosmetic (the firmware truncates too)
// but avoids silently dropping the tail of a line on the device.
const MSG_MAX = 23;
const LINE_MAX = 91;
const ENTRIES_MAX = 8;

/**
 * Builds a REFERENCE.md-shaped heartbeat snapshot from generic session
 * counters, so every adapter maps its own tool's state onto the same five
 * concepts (total/running/waiting/msg/entries) instead of hand-rolling the
 * JSON shape itself.
 *
 * `completed` isn't in REFERENCE.md's documented schema but is real:
 * src/data.h reads `doc["completed"]` into TamaState.recentlyCompleted,
 * which drives the `celebrate` state (main.cpp's derive()).
 */
function heartbeat({
  total = 0,
  running = 0,
  waiting = 0,
  msg = '',
  entries = [],
  tokens,
  tokensToday,
  completed,
} = {}) {
  const snap = {
    total,
    running,
    waiting,
    msg: String(msg).slice(0, MSG_MAX),
    entries: entries.slice(0, ENTRIES_MAX).map((e) => String(e).slice(0, LINE_MAX)),
  };
  if (tokens !== undefined) snap.tokens = tokens;
  if (tokensToday !== undefined) snap.tokens_today = tokensToday;
  if (completed) snap.completed = true;
  return snap;
}

/** The idle/no-activity snapshot most adapters send when nothing is running. */
function idleHeartbeat(msg = 'idle') {
  return heartbeat({ total: 0, running: 0, waiting: 0, msg });
}

module.exports = { heartbeat, idleHeartbeat };
