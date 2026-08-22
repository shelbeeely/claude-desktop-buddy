# OpenAI Codex CLI adapter

Bridges [Codex CLI's hooks and `notify`](https://learn.chatgpt.com/docs/hooks)
to a Hardware Buddy device: `PreToolUse`/`PostToolUse` hooks push heartbeats
and forward matched tool calls to the device as an approval prompt, and
`notify` pushes a "turn complete" heartbeat — the closest equivalent to
Claude's own `celebrate` state.

## How it fits together

Same shape as the `../copilot-cli/` adapter — read that one first if this
is your first time here. Each hook/notify script is a short-lived process
Codex spawns per event, so it talks to a long-running `../lib/daemon.js`
over a local socket rather than opening the device's serial port itself
(daemon.js's header comment explains why that specifically matters on
this hardware).

```
codex (in your project)
  └─ ~/.codex/config.toml (or <repo>/.codex/config.toml), from config.toml.example
       └─ hooks/pre-tool-use.js  ─┐
       └─ hooks/post-tool-use.js ─┼─ lib/client.js ──(local socket)──> lib/daemon.js ──(USB serial)──> device
       └─ hooks/notify.js       ─┘
```

## Setup

1. **Install dependencies once**, from the `adapters/` directory:
   ```bash
   cd adapters && npm install
   ```

2. **Find your device's port** and start the daemon (keep it running):
   ```bash
   node ../lib/list-ports.js --verbose
   node ../lib/daemon.js --port /dev/ttyACM0   # substitute your port
   ```

3. **Merge `config.toml.example` into `~/.codex/config.toml`** (or a
   project-local `.codex/config.toml`), replacing
   `/ABSOLUTE/PATH/TO/claude-desktop-buddy` with this repo's real path.
   Codex also supports a `hooks.json` file instead of inline
   `[hooks]` tables in `config.toml` — the TOML form here is what's
   directly confirmed from Codex's docs; if you'd rather use `hooks.json`,
   translate the same `matcher`/`command` shape per those docs.

4. **Check the `matcher` regex** against your Codex CLI version's actual
   tool names before trusting it — `^(Bash|apply_patch)$` is a reasonable
   starting guess, not a guarantee. Widening it to match everything means
   every tool call, including trivial reads, waits on the device.

5. Run `codex` as usual. A matched `PreToolUse` call lights up the
   device's `attention` state and waits (up to 25s, see
   `hooks/pre-tool-use.js`) for `CONFIRM` (approve) or `BACK` (deny)
   before falling back to Codex's normal approval flow.

## Known limitations

- **No true concurrent-session count** — same caveat as the Copilot CLI
  adapter: each hook run is a stateless, separate process, so
  `total`/`running` reflect only "this one Codex session is active."
- **The `notify` payload's exact fields aren't fully pinned down** —
  `last-assistant-message` is confirmed; other fields some Codex versions
  send (`cwd`, `thread-id`, etc.) aren't used here but are available in
  `hooks/notify.js` via `payload` if you want to extend it.
- **The daemon must already be running** before you start `codex` — a
  hook/notify script that can't reach it logs a warning to stderr and
  falls through to Codex's normal behavior rather than blocking anything.
