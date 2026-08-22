#!/usr/bin/env node
'use strict';

// Copilot CLI postToolUse hook. Input (stdin, camelCase):
//   { sessionId, timestamp, cwd, toolName, toolArgs, toolResult: { resultType, textResultForLlm } }
// No output needed — this hook only reports status, it doesn't gate.
const { readStdinJson } = require('../../lib/stdin-json');
const { sendHeartbeat } = require('../../lib/client');
const { heartbeat } = require('../../lib/heartbeat');

async function main() {
  const input = await readStdinJson();
  const toolName = input.toolName || 'tool';
  const ok = !input.toolResult || input.toolResult.resultType !== 'error';
  try {
    await sendHeartbeat(heartbeat({
      total: 1,
      running: 1,
      waiting: 0,
      msg: `${ok ? 'ok' : 'error'}: ${toolName}`,
    }));
  } catch (err) {
    console.error(`[hardware-buddy] ${err.message}`);
  }
  process.exit(0);
}

main();
