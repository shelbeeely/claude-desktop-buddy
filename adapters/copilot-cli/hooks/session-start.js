#!/usr/bin/env node
'use strict';

// Copilot CLI sessionStart hook. Input (stdin, camelCase):
//   { sessionId, timestamp, cwd, source: "startup"|"resume"|"new", initialPrompt? }
// No output is expected — this hook doesn't gate anything.
const { readStdinJson } = require('../../lib/stdin-json');
const { sendHeartbeat } = require('../../lib/client');
const { heartbeat } = require('../../lib/heartbeat');

async function main() {
  const input = await readStdinJson();
  const msg = input.initialPrompt ? input.initialPrompt.replace(/\s+/g, ' ').trim() : 'session started';
  try {
    await sendHeartbeat(heartbeat({ total: 1, running: 1, waiting: 0, msg }));
  } catch (err) {
    // Never fail the tool over a missing/unreachable device or daemon —
    // see ../../TEMPLATE.md "What NOT to build".
    console.error(`[hardware-buddy] ${err.message}`);
  }
  process.exit(0);
}

main();
