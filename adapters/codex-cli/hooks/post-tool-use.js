#!/usr/bin/env node
'use strict';

// Codex CLI PostToolUse hook. Input (stdin, snake_case) adds `tool_response`
// on top of PreToolUse's fields. No output needed — this hook only reports
// status, it doesn't gate.
const { readStdinJson } = require('../../lib/stdin-json');
const { sendHeartbeat } = require('../../lib/client');
const { heartbeat } = require('../../lib/heartbeat');

async function main() {
  const input = await readStdinJson();
  const toolName = input.tool_name || 'tool';
  try {
    await sendHeartbeat(heartbeat({
      total: 1,
      running: 1,
      waiting: 0,
      msg: `ok: ${toolName}`,
    }));
  } catch (err) {
    console.error(`[hardware-buddy] ${err.message}`);
  }
  process.exit(0);
}

main();
