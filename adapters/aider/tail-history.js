#!/usr/bin/env node
'use strict';

// Aider has no hook or event API to bridge from (unlike Copilot CLI/Codex
// CLI's adapters next door) — the only externally observable state is
// `.aider.chat.history.md`, the markdown transcript it appends to on
// every turn. So this adapter watches that file grow instead of reacting
// to structured events:
//
//   - Growth within the last IDLE_AFTER_MS -> heartbeat "running"
//   - No growth for IDLE_AFTER_MS          -> heartbeat "idle"
//   - New `#### ` lines (aider's own prefix for a user prompt, per its
//     example transcripts) are used as a best-effort one-line summary;
//     anything else just proves *some* activity happened, without
//     claiming to know what it was.
//
// This is intentionally heartbeat-only. There is no approval-forwarding
// here: Aider prompts for file edits/shell commands directly in its own
// terminal session with no hook this script can intercept, so there is
// nothing to forward. See README.md "Known limitations".
const fs = require('fs');
const path = require('path');
const { HardwareBuddyLink } = require('../lib/link');
const { heartbeat, idleHeartbeat } = require('../lib/heartbeat');

const POLL_MS = 1000;
const IDLE_AFTER_MS = 8000;
const HEARTBEAT_MS = 5000; // send *something* well under the device's 30s timeout

function parseArgs(argv) {
  const opts = { historyPath: '.aider.chat.history.md' };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--port') opts.port = argv[++i];
    else if (argv[i] === '--baud') opts.baud = Number(argv[++i]);
    else if (argv[i] === '--history') opts.historyPath = argv[++i];
  }
  return opts;
}

function lastPromptLine(text) {
  const lines = text.split('\n');
  for (let i = lines.length - 1; i >= 0; i--) {
    if (lines[i].startsWith('#### ')) return lines[i].slice(5).trim();
  }
  return null;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (!opts.port) {
    console.error('usage: node tail-history.js --port <serial-port> [--history <path>] [--baud 115200]');
    console.error('       node ../lib/list-ports.js   # to find <serial-port>');
    process.exit(1);
  }

  const historyPath = path.resolve(opts.historyPath);
  const link = new HardwareBuddyLink({ path: opts.port, baudRate: opts.baud });

  let lastSize = fs.existsSync(historyPath) ? fs.statSync(historyPath).size : 0;
  let lastActivityMs = 0;
  let lastPrompt = null;

  function checkGrowth() {
    let stat;
    try {
      stat = fs.statSync(historyPath);
    } catch {
      return; // file doesn't exist yet — aider creates it on first turn
    }
    if (stat.size <= lastSize) return;

    const fd = fs.openSync(historyPath, 'r');
    const len = stat.size - lastSize;
    const buf = Buffer.alloc(len);
    fs.readSync(fd, buf, 0, len, lastSize);
    fs.closeSync(fd);
    lastSize = stat.size;
    lastActivityMs = Date.now();

    const prompt = lastPromptLine(buf.toString('utf8'));
    if (prompt) lastPrompt = prompt;
    console.error(`[hardware-buddy] activity: ${prompt ? prompt.slice(0, 60) : '(non-prompt output)'}`);
  }

  function tick() {
    checkGrowth();
    const active = lastActivityMs !== 0 && Date.now() - lastActivityMs < IDLE_AFTER_MS;
    const snapshot = active
      ? heartbeat({ total: 1, running: 1, waiting: 0, msg: lastPrompt ? lastPrompt.slice(0, 23) : 'aider: working' })
      : idleHeartbeat(lastActivityMs === 0 ? 'aider: waiting' : 'aider: idle');
    link.sendHeartbeat(snapshot);
  }

  const pollTimer = setInterval(checkGrowth, POLL_MS);
  const heartbeatTimer = setInterval(tick, HEARTBEAT_MS);
  tick();

  console.error(`[hardware-buddy] watching ${historyPath}`);

  const shutdown = () => {
    clearInterval(pollTimer);
    clearInterval(heartbeatTimer);
    link.close().finally(() => process.exit(0));
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

main();
