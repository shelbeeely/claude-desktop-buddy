# Aider adapter

Aider has no hook or plugin API to bridge from — unlike the Copilot CLI and
Codex CLI adapters next door, there's no event this script can subscribe
to. The only externally observable state is `.aider.chat.history.md`, the
markdown transcript Aider appends to on every turn, so this adapter tails
that file and turns its growth into heartbeats.

Because there's no hook, this is a single long-running process that owns
the device's serial port directly (see `../lib/link.js`) — there's no
short-lived-hook-plus-daemon split to worry about here, unlike
`../copilot-cli/` or `../codex-cli/`.

## Setup

```bash
cd adapters && npm install
node lib/list-ports.js --verbose   # find your device's port
node aider/tail-history.js --port /dev/ttyACM0
```

Run this in the same directory you run `aider` from (or pass
`--history /path/to/.aider.chat.history.md` if Aider's `--chat-history-file`
points elsewhere). Then start `aider` as usual in another terminal — no
Aider-side configuration needed.

## What it shows

- New content appended to the history file → device shows `busy`/`idle`
  activity for ~8s per burst (see `IDLE_AFTER_MS` in `tail-history.js`).
- A best-effort one-line summary of the most recent `#### ` line (Aider's
  own prefix for a user prompt in its transcripts) — if the file's format
  doesn't match that assumption for some version of Aider, this just falls
  back to a generic "aider: working" message; activity detection itself
  doesn't depend on the exact format, only the summary text does.

## Known limitations

- **No approval forwarding, at all.** Aider prompts for file edits and
  shell commands directly in its own terminal session (or auto-applies
  them under `--yes-always`) with nothing this script can intercept — so
  the device's `attention` state and physical `CONFIRM`/`BACK` buttons are
  inert for Aider. If you want that, `--yes-always` combined with your own
  review process is the closest Aider gets; it's not something this
  adapter can add.
- **File growth is a proxy for activity, not a real session/tool-call
  count.** `total`/`running` are always `0`/`1` or `1`/`1` — there's no
  concept of concurrent sessions or a real "waiting" state, since neither
  is observable from the transcript file.
- **Polling, not push.** Activity is detected up to `POLL_MS` (1s) late,
  and a burst of edits within `IDLE_AFTER_MS` (8s) of each other reads as
  one continuous "busy" stretch rather than distinct turns.
