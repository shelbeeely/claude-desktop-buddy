# GitHub Copilot CLI adapter

Bridges [Copilot CLI's hooks](https://docs.github.com/en/copilot/reference/hooks-reference)
to a Hardware Buddy device: session start and tool activity show up as
heartbeats, and `preToolUse` calls matched by your hook config get forwarded
to the device as an approval prompt — `CONFIRM`/`BACK` on the device answers
it, same as an approval from Claude's own desktop apps.

## How it fits together

```
copilot (in your project)
  └─ .github/hooks/hardware-buddy.json   (you create this, from hooks.json.example)
       └─ hooks/session-start.js  ─┐
       └─ hooks/pre-tool-use.js   ─┼─ lib/client.js ──(local socket)──> lib/daemon.js ──(USB serial)──> device
       └─ hooks/post-tool-use.js ─┘
```

Each hook script here is a short-lived process Copilot CLI spawns per
event — it can't hold the device's serial port open itself (see
`../lib/daemon.js`'s header comment for why that specifically matters on
this hardware). `daemon.js` is the one process that owns the port; hooks
just relay through it.

## Setup

1. **Install dependencies once**, from the `adapters/` directory:
   ```bash
   cd adapters && npm install
   ```

2. **Find your device's port** and start the daemon (keep it running —
   a background job, a `tmux`/`screen` pane, or a process manager):
   ```bash
   node ../lib/list-ports.js --verbose
   node ../lib/daemon.js --port /dev/ttyACM0   # substitute your port
   ```

3. **Wire up the hooks** in whichever repo you run `copilot` from. Copy
   `hooks.json.example` to `.github/hooks/hardware-buddy.json` in that
   repo, and replace `/ABSOLUTE/PATH/TO/claude-desktop-buddy` with this
   repo's actual absolute path.

4. **Set the `matcher` to the tool calls you actually want gated behind a
   physical approval.** The example ships `"shell|write"` as a starting
   point, but Copilot CLI's exact built-in tool names can differ by
   version — check the
   [hooks reference](https://docs.github.com/en/copilot/reference/hooks-reference)
   for your installed version, or temporarily drop the `matcher` field,
   watch `toolName` values logged to stderr by a quick
   `console.error(input.toolName)` in `pre-tool-use.js`, and set the regex
   from what you actually see. Matching everything means every tool call —
   including trivial reads — waits on the device, which gets old fast.

5. Run `copilot` as usual. `sessionStart` and `postToolUse` push heartbeats
   silently; a matched `preToolUse` call lights up the device's `attention`
   state and waits (up to 25s, see `pre-tool-use.js`) for `CONFIRM`
   (approve) or `BACK` (deny) before falling back to Copilot's normal
   prompt.

## Known limitations

- **No true concurrent-session count.** Each hook run is stateless (a new
  process every time), so `total`/`running` reflect only "this one Copilot
  CLI session is active," not a real count across multiple sessions. The
  device's `busy` state (`sessionsRunning >= 3`) won't trigger from this
  adapter alone.
- **Between tool calls, there's no "thinking" signal** — Copilot CLI's
  hook set (per its docs) doesn't expose a mid-turn event, so the device's
  status between a `postToolUse` and the next `preToolUse` just reflects
  whatever the last heartbeat said, until the next event or the 30s
  connection timeout.
- **The daemon must already be running** before you start `copilot` — a
  hook that can't reach it logs a warning to stderr and falls through to
  Copilot's normal behavior rather than blocking anything.
