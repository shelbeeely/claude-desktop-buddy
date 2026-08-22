# Adapters

The device firmware in `../src/` doesn't know or care which app is on the
other end of the wire — it just parses newline-delimited JSON matching
[`../REFERENCE.md`](../REFERENCE.md), the same code path whether that JSON
arrives over BLE or USB serial (`../src/data.h`). Claude's desktop apps are
one sender of that JSON. The scripts in this directory are others: small
host-side bridges that watch a different AI coding tool and translate its
own session activity into the same protocol, so a Hardware Buddy device
built from this repo isn't limited to sessions started from Claude's own
desktop apps.

| Adapter | Tool | Approval forwarding? |
|---|---|---|
| [`copilot-cli/`](copilot-cli/) | [GitHub Copilot CLI](https://docs.github.com/en/copilot/reference/hooks-reference) | Yes, via its `preToolUse` hook |
| [`codex-cli/`](codex-cli/) | [OpenAI Codex CLI](https://learn.chatgpt.com/docs/hooks) | Yes, via its `PreToolUse` hook |
| [`aider/`](aider/) | [Aider](https://aider.chat/) | No — Aider has no hook API to forward from |

Building a bridge for something else (Cursor, a custom agent, your own
script)? [`TEMPLATE.md`](TEMPLATE.md) walks through the three building
blocks every adapter here is made of.

## Shared pieces (`lib/`)

- `link.js` — opens the device's USB serial port and speaks the wire
  protocol (heartbeats, acks, the permission-decision round trip). Used
  directly by any adapter that's already a single long-running process
  (`aider/`), since it needs to hold the port open exactly once — see its
  header comment for why repeatedly opening it is actually risky on this
  hardware, not just wasteful.
- `daemon.js` / `client.js` — for adapters built from a tool's own hook
  system, where each event is its own short-lived process (`copilot-cli/`,
  `codex-cli/`). `daemon.js` is the one long-running process that owns the
  port; hook scripts are cheap `client.js` callers that talk to it over a
  local socket instead.
- `heartbeat.js` — builds a REFERENCE.md-shaped snapshot from generic
  `total`/`running`/`waiting`/`msg`/`entries` fields.
- `list-ports.js` — `node lib/list-ports.js [--verbose]` to find your
  device's serial port.
- `stdin-json.js` — reads a hook's JSON payload from stdin (used by the
  Copilot CLI and Codex CLI hook scripts).

## Why USB serial instead of BLE

REFERENCE.md documents the BLE transport because that's what Claude's
desktop apps use, but the firmware itself parses identical JSON over plain
USB serial (`../src/data.h`'s `_usbLine` alongside `_btLine`). Serial needs
no OS Bluetooth stack, no pairing/bonding, and works the moment the device
is plugged into the same machine the adapter runs on — the common case for
a script sitting next to a CLI tool. If you need BLE instead (the tool
you're bridging runs on a different machine than the device), REFERENCE.md's
"Transport" section has the Nordic UART Service UUIDs to swap into a copy
of `lib/link.js`.

## Install

All adapters share one `package.json`:

```bash
cd adapters && npm install
```

Then follow the README in whichever adapter directory matches your tool.
