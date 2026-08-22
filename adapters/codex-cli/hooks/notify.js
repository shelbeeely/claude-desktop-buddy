#!/usr/bin/env node
'use strict';

// Codex CLI `notify` command. Codex runs the configured argv and appends
// one JSON-string argument (process.argv[2] here, since argv[0] is node
// and argv[1] is this script's path) describing what just happened —
// commonly `{"type": "agent-turn-complete", "input-messages": [...],
// "last-assistant-message": "...", ...}`. Unlike the PreToolUse/PostToolUse
// hooks, this fires on turn completion, not on individual tool calls, and
// nothing reads its exit code or output — it's a fire-and-forget notifier.
const { sendHeartbeat } = require('../../lib/client');
const { heartbeat } = require('../../lib/heartbeat');

async function main() {
  const raw = process.argv[2];
  let payload = {};
  try {
    payload = raw ? JSON.parse(raw) : {};
  } catch {
    // Not JSON, or a shape we don't recognize — still send a generic
    // "turn complete" heartbeat rather than silently doing nothing.
  }

  const last = typeof payload['last-assistant-message'] === 'string'
    ? payload['last-assistant-message'].replace(/\s+/g, ' ').trim()
    : 'turn complete';

  try {
    await sendHeartbeat(heartbeat({ total: 1, running: 0, waiting: 0, msg: last, completed: true }));
  } catch (err) {
    console.error(`[hardware-buddy] ${err.message}`);
  }
}

main();
