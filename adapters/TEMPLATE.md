# Writing your own adapter

The device doesn't know or care which app is on the other end of the wire —
`src/data.h`'s parser just reads newline-delimited JSON matching
[REFERENCE.md](../REFERENCE.md), whether it arrives over BLE or USB serial.
Claude's desktop apps are one sender of that JSON. An adapter is anything
else that sends it: a script that watches another AI coding tool and
translates its session activity into the same shape.

Three concrete adapters live next to this file (`copilot-cli/`, `codex-cli/`,
`aider/`) — read one of those for a worked example. This doc is the
short version: what every adapter actually needs to do, independent of
which tool it's bridging.

## 0. Pick a shape: daemon+hooks, or one long-running process?

`lib/link.js` (`HardwareBuddyLink`) is the only thing that actually opens
the device's serial port, and it should only be opened **once** and held
open — see its header comment: many Arduino-core boards (including this
repo's ESP32-C3/S3 targets) reset on DTR when a serial connection opens,
the same signal esptool/Arduino IDE use to enter the bootloader. Repeatedly
opening and closing the port risks resetting the device instead of just
talking to it.

That means the shape of your adapter depends on how the tool you're
bridging exposes events:

- **The tool has an external hook system that fires a new short-lived
  process per event** (Copilot CLI, Codex CLI — a `preToolUse` hook is
  its own process that runs once and exits). Don't open the serial port
  from inside the hook. Instead run `lib/daemon.js` once as a background
  process — it opens the port a single time and holds it — and have your
  hook scripts talk to it over a local socket via `lib/client.js`. See
  `copilot-cli/` or `codex-cli/` for the full wiring.
- **The tool has no hook system, so you're writing your own long-running
  watcher process** (Aider — no hook API, so `aider/tail-history.js` tails
  its chat history file itself). That process already lives for the whole
  session, so it can use `HardwareBuddyLink` directly — it *is* the
  daemon, there's no separate short-lived process to bridge from.

The rest of this doc covers both: swap `link.js` for `client.js` (talking
to an already-running daemon) if you're in the first case.

## 1. Open a link

Long-running watcher process (own the port directly):

```js
const { HardwareBuddyLink } = require('../lib/link');

const link = new HardwareBuddyLink({ path: '/dev/ttyACM0' }); // or COM3, etc.
```

Short-lived hook script (talk to an already-running `daemon.js`):

```js
const { sendHeartbeat, requestApproval } = require('../lib/client');
```

Don't know the port? `node lib/list-ports.js` wraps `SerialPort.list()`.
Every adapter here takes `--port` or reads it from an env var rather than
guessing, since more than one serial device is common on a dev machine.

This uses USB serial, not BLE — see `lib/link.js`'s header comment for
why. If you specifically need BLE (e.g. the tool you're bridging runs on a
different machine than the device), REFERENCE.md's "Transport" section has
the Nordic UART Service UUIDs; swap the transport inside a copy of
`link.js`, the rest of this doc still applies unchanged (`daemon.js` and
`client.js` don't care what `link.js` talks to underneath).

## 2. Push heartbeats on activity

Map your tool's own state onto `total`/`running`/`waiting`/`msg`/`entries`
and send it whenever something changes (a turn starts, a turn ends, a tool
call happens) — REFERENCE.md's "Heartbeat snapshot":

```js
const { heartbeat } = require('../lib/heartbeat');

const snapshot = heartbeat({
  total: 1,
  running: 1,
  waiting: 0,
  msg: 'running tests',
  entries: ['10:42 pytest -k foo', '10:41 edited foo.py'],
});

link.sendHeartbeat(snapshot);      // long-running process, owns the link
await sendHeartbeat(snapshot);     // hook script, via the daemon
```

If your tool has no natural "session count," `total`/`running` can just be
`0`/`1` while it's active — the device's state machine
(`main.cpp`'s `derive()`) only cares about `sessionsWaiting > 0` →
attention, `sessionsRunning >= 3` → busy, else idle. A single-session tool
never reaches `busy`, which is fine.

Send an idle snapshot (`heartbeat({ total: 0, running: 0, waiting: 0 })`,
or `idleHeartbeat()`) when your tool goes quiet — the device treats no
heartbeat for 30s as disconnected (REFERENCE.md), so for a long-running
adapter process, send *something* at least that often even when nothing
changed, the same way Claude's desktop apps send a 10s keepalive.

## 3. Forward approval prompts (optional — only if your tool has one)

If your tool has a hook that can pause a tool call for approval, forward it
to the device and await the decision:

```js
try {
  // Long-running process: link.requestApproval({...}, snapshot, timeoutMs)
  // Hook script:           await requestApproval({...}, opts) from lib/client
  const decision = await requestApproval({
    id: someUniqueId,
    tool: 'Bash',
    hint: command.slice(0, 44),
    snapshot: { total: 1, running: 0, waiting: 1, msg: `approve: ${toolName}` },
  });
  // decision is 'approve' or 'deny'
} catch {
  // Timed out, or the daemon isn't reachable at all (no device connected,
  // daemon not started, wrong socket path). Fall back to your tool's own
  // normal prompt/behavior; never let a disconnected device silently
  // block or silently approve a tool call.
}
```

`hint` is shown on a small e-paper panel — REFERENCE.md's example uses 44
characters; longer is truncated on-device, not by the library.

## 4. What NOT to build

- **Don't reimplement the JSON framing.** `lib/link.js` already handles
  line-buffering, acks, and the permission-decision correlation by `id`.
- **Don't block your tool's main loop indefinitely on the device.** Every
  adapter here treats a missing/slow device as "fall through to normal
  behavior," never as a hard dependency — a maker's device being asleep or
  unplugged shouldn't break the underlying tool.
- **Don't invent new wire fields.** If REFERENCE.md and `src/data.h` don't
  parse a field, the firmware ignores it silently — there's no error
  feedback, so a typo'd field just does nothing and is hard to debug.
