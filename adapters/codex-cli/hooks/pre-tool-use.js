#!/usr/bin/env node
'use strict';

// Codex CLI PreToolUse hook. Input (stdin, snake_case) includes at least:
//   session_id, turn_id, transcript_path, cwd, hook_event_name, model,
//   permission_mode, tool_name, tool_use_id, tool_input
// Output (stdout, on exit 0):
//   { "hookSpecificOutput": { "hookEventName": "PreToolUse",
//                              "permissionDecision": "allow"|"deny",
//                              "permissionDecisionReason"?: string } }
// Omitting hookSpecificOutput defers to Codex's own normal approval flow.
// See https://learn.chatgpt.com/docs/hooks
//
// Only tool calls matched by config.toml's own hook `matcher` reach this
// script — see README.md for why that matters (you don't want a physical
// approve/deny round-trip on every trivial read-only call).
const { readStdinJson } = require('../../lib/stdin-json');
const { requestApproval } = require('../../lib/client');
const { heartbeat } = require('../../lib/heartbeat');

// Kept short so this hook resolves its own "no opinion" fallback and
// exits cleanly rather than being killed by Codex's own hook timeout —
// tune the hook's timeout in config.toml to comfortably exceed this.
const APPROVAL_TIMEOUT_MS = 25_000;

function summarize(toolInput) {
  try {
    if (toolInput && typeof toolInput.command === 'string') return toolInput.command.slice(0, 44);
    return JSON.stringify(toolInput).slice(0, 44);
  } catch {
    return '';
  }
}

async function main() {
  const input = await readStdinJson();
  const toolName = input.tool_name || 'tool';
  const id = input.tool_use_id || `${input.session_id || 'session'}-${input.turn_id || Date.now()}`;

  try {
    const decision = await requestApproval({
      id,
      tool: toolName,
      hint: summarize(input.tool_input),
      snapshot: heartbeat({ total: 1, running: 0, waiting: 1, msg: `approve: ${toolName}` }),
      timeoutMs: APPROVAL_TIMEOUT_MS,
    });
    process.stdout.write(JSON.stringify({
      hookSpecificOutput: {
        hookEventName: 'PreToolUse',
        permissionDecision: decision === 'deny' ? 'deny' : 'allow',
        ...(decision === 'deny' ? { permissionDecisionReason: 'Denied on the Hardware Buddy device' } : {}),
      },
    }));
  } catch (err) {
    // No device/daemon reachable, or nobody answered in time — defer to
    // Codex's own normal approval flow (permission_mode). Never turn a
    // disconnected device into a hard block or a silent approval.
    console.error(`[hardware-buddy] ${err.message} — falling back to normal approval flow`);
  }
  process.exit(0);
}

main();
