#!/usr/bin/env node
'use strict';

// Copilot CLI preToolUse hook. Input (stdin, camelCase):
//   { sessionId, timestamp, cwd, toolName, toolArgs }
// Output (stdout, on exit 0): { permissionDecision: "allow"|"deny"|"ask", permissionDecisionReason? }
// Omitting permissionDecision defers to Copilot's own normal flow — see
// https://docs.github.com/en/copilot/reference/hooks-reference
//
// Only tool calls matched by hooks.json's own "matcher" regex reach this
// script at all — see README.md for why that matters (you don't want a
// physical approve/deny round-trip on every trivial read-only call).
const { readStdinJson } = require('../../lib/stdin-json');
const { requestApproval } = require('../../lib/client');
const { heartbeat } = require('../../lib/heartbeat');

// Kept below hooks.json's own timeoutSec (see README.md's sample config)
// so this hook reaches its own "no opinion" fallback and exits cleanly,
// rather than being killed by Copilot's harsher timeout.
const APPROVAL_TIMEOUT_MS = 25_000;

function summarize(toolArgs) {
  try {
    const s = typeof toolArgs === 'string' ? toolArgs : JSON.stringify(toolArgs);
    return s.slice(0, 44);
  } catch {
    return '';
  }
}

async function main() {
  const input = await readStdinJson();
  const toolName = input.toolName || 'tool';
  const id = `${input.sessionId || 'session'}-${input.timestamp || Date.now()}`;

  try {
    const decision = await requestApproval({
      id,
      tool: toolName,
      hint: summarize(input.toolArgs),
      snapshot: heartbeat({ total: 1, running: 0, waiting: 1, msg: `approve: ${toolName}` }),
      timeoutMs: APPROVAL_TIMEOUT_MS,
    });
    if (decision === 'deny') {
      process.stdout.write(JSON.stringify({
        permissionDecision: 'deny',
        permissionDecisionReason: 'Denied on the Hardware Buddy device',
      }));
    } else {
      process.stdout.write(JSON.stringify({ permissionDecision: 'allow' }));
    }
  } catch (err) {
    // No device/daemon reachable, or nobody answered in time — defer to
    // Copilot's own normal approval flow. Never turn a disconnected
    // device into a hard block (or a silent approval).
    console.error(`[hardware-buddy] ${err.message} — falling back to normal approval flow`);
  }
  process.exit(0);
}

main();
